#ifndef VM_HPP
#define VM_HPP

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <cmath>
#include "value.hpp"
#include "chunk.hpp"
#include "compiler.hpp"
#include "runtime.hpp"
#include "../lexer/lexer.hpp"
#include "../parser/parser.hpp"
#include "../object/environment.hpp"
#include "../evaluator/builtins.hpp"
#include "../evaluator/stdlib.hpp"
#include "../utils/colors.hpp"

// The Lovax virtual machine: a stack-based bytecode interpreter with
// computed-goto dispatch, closed upvalues, and immediate numeric values.
// Object-typed operations reuse the exact runtime semantics (and error
// messages) of the retired tree-walker via runtime.hpp.

// Direct-threaded dispatch (computed goto) on GCC/Clang: each opcode handler
// jumps straight to the next handler, giving the branch predictor one indirect
// jump per handler instead of a single shared one. ~15-25% on tight loops.
// Define LOVAX_NO_COMPUTED_GOTO to force the portable switch dispatch.
#if (defined(__GNUC__) || defined(__clang__)) && !defined(LOVAX_NO_COMPUTED_GOTO)
#define LOVAX_CG 1
#endif

namespace Lovax {

// A single call frame. Namespace-scoped (not VM-private) so coroutine state can
// hold its own frame stack.
struct Frame {
    // Raw pointer: the callee Value at stack_[base] keeps the closure alive
    // until RETURN truncates the stack, so no refcount traffic per call.
    ClosureObject* closure;
    const uint8_t* ip;
    size_t base;    // stack index of the callee value; locals start at base+1
    int nargs;
    // Cached from closure->proto->chunk so frame switches (call/return) do a
    // flat load instead of chasing two shared_ptrs.
    const Value* consts;
    const Chunk* chunk;
};

// Active try handler (RFC-008): where to jump when a value is thrown.
struct Handler {
    size_t frameDepth;   // frames_.size() at the try
    size_t stackTop;     // sp_ offset at the try
    const uint8_t* catchIp;
};

// A suspended execution context: everything a running program owns. The main
// program and each coroutine each have one; resume/yield swap them (RFC-014).
struct ExecState {
    std::unique_ptr<Value[]> stack;
    size_t spOffset = 0;
    std::vector<Frame> frames;
    std::vector<std::shared_ptr<UpvalueCell>> openUpvalues;
    std::vector<Handler> handlers;
};

// A coroutine value: a paused function that yields values and resumes where it
// left off. Holds its own stack/frames while suspended.
class CoroObject : public Object {
public:
    enum class Status { CREATED, SUSPENDED, RUNNING, DEAD };
    Ref<Object> fn;   // the closure to run on first resume
    Status status = Status::CREATED;
    ExecState state;              // saved stack/frames while suspended
    CoroObject(Ref<Object> f)
        : Object(ObjectType::COROUTINE), fn(std::move(f)) {}
    void gcMark() override {
        gcMarkObject(fn.get());
        // Suspended context (empty while running — the VM marks the live copy).
        if (state.stack) {
            Value* base = state.stack.get();
            for (size_t i = 0; i < state.spOffset; ++i) gcMarkValue(base[i]);
        }
        for (auto& f : state.frames) gcMarkObject(f.closure);
        for (auto& u : state.openUpvalues) if (u) gcMarkValue(u->value);
    }
    std::string inspect() const override {
        const char* s = status == Status::CREATED   ? "created"
                      : status == Status::SUSPENDED ? "suspended"
                      : status == Status::RUNNING   ? "running" : "dead";
        return std::string("<coroutine ") + s + ">";
    }
    size_t gcBytes() const override {
        // A suspended coroutine owns a full value stack (STACK_LIMIT slots).
        size_t n = sizeof(*this);
        if (state.stack) n += (size_t(1) << 16) * sizeof(Value);
        n += state.frames.capacity() * 64;
        return n;
    }
};

class VM {
public:
    static constexpr int MAX_FRAMES = 500;
    static constexpr size_t STACK_LIMIT = 1 << 16;

    VM() {
        stackMem_ = std::make_unique<Value[]>(STACK_LIMIT);
        sp_ = stackMem_.get();
        // Register for GC root scanning + install the collector's root callback once.
        liveVMs().push_back(this);
        Heap::get().markRoots = &VM::markAllRoots;
        installBuiltinGlobals();
    }
    ~VM() {
        auto& v = liveVMs();
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }

    // Every VM whose stack/globals/frames are live roots for the GC.
    static std::vector<VM*>& liveVMs() { static std::vector<VM*> v; return v; }

    // Execution contexts swapped out of a VM into a C++ local during a coroutine
    // resume — still live roots while the coroutine runs.
    static std::vector<ExecState*>& savedStates() { static std::vector<ExecState*> v; return v; }

    static void markExecState(const ExecState& s) {
        if (s.stack) {
            Value* base = s.stack.get();
            for (size_t i = 0; i < s.spOffset; ++i) gcMarkValue(base[i]);
        }
        for (auto& f : s.frames) gcMarkObject(f.closure);
        for (auto& u : s.openUpvalues) if (u) gcMarkValue(u->value);
    }

    // Marks this VM's own live references (called by markAllRoots).
    void markOwnRoots() {
        for (Value* p = stackMem_.get(); p < sp_; ++p) gcMarkValue(*p);
        for (auto& g : globals_) gcMarkValue(g);
        for (auto& f : frames_) gcMarkObject(f.closure);
        for (auto& u : openUpvalues_) if (u) gcMarkValue(u->value);
        if (yieldValue_) gcMarkObject(yieldValue_.get());
        if (activeCoro_) gcMarkObject(activeCoro_);
    }

    // The GC's root callback: singletons, every live VM, and the file-module cache.
    static void markAllRoots() {
        gcMarkObject(TRUE_OBJ.get());
        gcMarkObject(FALSE_OBJ.get());
        gcMarkObject(NULL_OBJ_.get());
        for (VM* vm : liveVMs()) vm->markOwnRoots();
        for (ExecState* s : savedStates()) markExecState(*s);
        for (auto& kv : moduleCache()) gcMarkObject(kv.second.get());
    }

    // Relative module paths resolve from the entry script's directory.
    static void setBaseDir(const std::string& dir) {
        baseDirs().clear();
        baseDirs().push_back(dir.empty() ? "." : dir);
    }

    // ===== Coroutines (RFC-014) =====

    // Moves the live execution context (stack, frames, upvalues, handlers) out
    // into an ExecState. The VM's members are left empty; the caller loads a
    // replacement immediately.
    ExecState saveExec() {
        ExecState e;
        e.spOffset = (size_t)(sp_ - stackMem_.get());
        e.stack = std::move(stackMem_);
        e.frames = std::move(frames_);
        e.openUpvalues = std::move(openUpvalues_);
        e.handlers = std::move(handlers_);
        return e;
    }
    void loadExec(ExecState&& e) {
        stackMem_ = std::move(e.stack);
        sp_ = stackMem_.get() + e.spOffset;
        frames_ = std::move(e.frames);
        openUpvalues_ = std::move(e.openUpvalues);
        handlers_ = std::move(e.handlers);
    }

    // resume(co, value): runs the coroutine until it yields or returns. Returns
    // the yielded/returned value, or an error object.
    Ref<Object> resumeCoroutine(const Ref<Object>& coObj,
                                             const Ref<Object>& arg, int line) {
        auto* co = static_cast<CoroObject*>(coObj.get());
        if (co->status == CoroObject::Status::DEAD)
            return makeError("cannot resume a finished coroutine", line);
        if (co->status == CoroObject::Status::RUNNING)
            return makeError("cannot resume a coroutine that is already running", line);

        ExecState caller = saveExec();
        // The caller's whole context now lives in a C++ local; register it as a GC
        // root for the duration, or a collection while the coroutine runs would
        // free the resumer's objects. RAII pop covers both the normal and error exit.
        savedStates().push_back(&caller);
        struct PopSaved { ~PopSaved() { savedStates().pop_back(); } } _popSaved;
        CoroObject* prevActive = activeCoro_;
        bool prevYielded = yielded_;
        activeCoro_ = co;

        Ref<Object> result;
        if (co->status == CoroObject::Status::CREATED) {
            // Fresh: give the coroutine its own stack and set up the fn call frame.
            stackMem_ = std::make_unique<Value[]>(STACK_LIMIT);
            sp_ = stackMem_.get();
            frames_.clear();
            openUpvalues_.clear();
            handlers_.clear();
            push(Value::object(co->fn));
            // The first resume value is passed as the fn's argument only if it
            // declares a parameter; a zero-parameter coroutine ignores it.
            int coargc = 0;
            auto* clo = static_cast<ClosureObject*>(co->fn.get());
            if (clo->proto->paramCount >= 1) { push(Value::object(arg)); coargc = 1; }
            co->status = CoroObject::Status::RUNNING;
            auto err = callValue(coargc, line);
            if (err != nullptr && isError(err)) {
                co->status = CoroObject::Status::DEAD;
                loadExec(std::move(caller));
                activeCoro_ = prevActive; yielded_ = prevYielded;
                return err;
            }
            yielded_ = false;
            result = run(0);
        } else {
            // Suspended: restore its stack; the resume value becomes the value of
            // the yield expression it paused on.
            loadExec(std::move(co->state));
            push(Value::object(arg));
            co->status = CoroObject::Status::RUNNING;
            yielded_ = false;
            result = run(0);
        }

        Ref<Object> out;
        if (isError(result)) {
            co->status = CoroObject::Status::DEAD;
            out = result;
        } else if (yielded_) {
            // Paused at a yield: keep its context for the next resume.
            co->status = CoroObject::Status::SUSPENDED;
            co->state = saveExec();
            // Barrier: a black coroutine just absorbed live stack contents.
            if (co->gcColor == GC_BLACK) { co->gcColor = GC_GRAY; Heap::get().worklist.push_back(co); }
            out = yieldValue_ ? yieldValue_ : NULL_OBJ_;
        } else {
            // Ran to completion: the return value is on top of the coroutine stack.
            co->status = CoroObject::Status::DEAD;
            out = (sp_ > stackMem_.get()) ? toObject(pop()) : NULL_OBJ_;
        }

        loadExec(std::move(caller));
        activeCoro_ = prevActive;
        yielded_ = prevYielded;
        return out;
    }

    // Compiles and runs a program. Returns NULL_OBJ_ or an ErrorObject.
    Ref<Object> interpret(const Program* program) {
        // GC stays off through compilation and frame setup: the compile-time
        // constants live in the proto (a shared_ptr, invisible to the GC) and are
        // only reachable once the top-level closure is built and pushed as a root.
        // Enabling GC afterwards makes them reachable via closure -> proto -> consts.
        bool wasEnabled = Heap::get().enabled;
        Heap::get().enabled = false;

        Compiler compiler(globalsTable_);
        std::shared_ptr<Proto> proto;
        try {
            proto = compiler.compileProgram(program);
        } catch (const CompileError& ce) {
            // A bytecode operand limit was exceeded (e.g. >65535 constants). Surface
            // it as a located compile-time error instead of running miscompiled code.
            auto err = makeError(ce.message, ce.line);   // allocate while GC is still off
            err->compileTime = true;
            Heap::get().enabled = wasEnabled;
            return err;
        }
        syncGlobalSlots();

        auto closure = makeObj<ClosureObject>(proto);
        push(Value::object(closure));
        // Reserve the script frame's local slots (for-loop variables) just like a
        // function call would, so the value stack runs above them.
        for (int i = 0; i < proto->localCount; ++i) push(Value::nil());
        frames_.reserve(MAX_FRAMES);
        frames_.push_back({closure.get(), proto->chunk.code.data(), 0, 0,
                           proto->chunk.consts.data(), &proto->chunk});

        Heap::get().enabled = true;   // closure is rooted on the stack now
        auto result = run(0);
        Heap::get().enabled = wasEnabled;
        return isError(result) ? result : NULL_OBJ_;
    }

    // REPL: wipe the value stack / frames so a fresh top-level program runs cleanly
    // while globals (names, values, defined-flags) persist across entered lines.
    void resetReplState() {
        sp_ = stackMem_.get();
        frames_.clear();
        handlers_.clear();
        openUpvalues_.clear();
    }

    // Native -> Lovax call bridge (builtins calling closures: filter/emit/sort_by).
    Ref<Object> callFromNative(const Ref<Object>& fn,
                                           const std::vector<Ref<Object>>& args,
                                           int line) {
        if (fn->type() == ObjectType::BUILTIN) {
            auto* b = static_cast<BuiltinObject*>(fn.get());
            return b->fn(args, line, callFn());
        }
        if (fn->type() != ObjectType::FUNCTION) {
            return makeError("not a function, cannot be called: " + typeName(fn->type()), line);
        }
        size_t entryFrames = frames_.size();
        push(Value::object(fn));
        for (const auto& a : args) push(fromObject(a));
        auto err = callValue((int)args.size(), line);
        if (err != nullptr) return err;
        auto result = run(entryFrames);
        if (isError(result)) return result;
        Value v = pop();
        return toObject(v);
    }

    BuiltinObject::CallFn callFn() {
        VM* self = this;
        return [self](const Ref<Object>& f,
                      const std::vector<Ref<Object>>& a,
                      int l) { return self->callFromNative(f, a, l); };
    }

    // ===== Module loading (mirrors the retired tree-walker loader) =====

    static std::vector<std::string>& baseDirs() {
        static std::vector<std::string> dirs = {"."};
        return dirs;
    }

    Ref<Object> loadModule(const UseSpec& spec, int line) {
        if (!spec.isFile) {
            // Network access is gated at import: 'use net' needs --allow-net.
            if (spec.target == "net" && !StdLib::perms().net) {
                return makeError("permission denied: 'use net' requires --allow-net "
                                 "(or --allow-all)", line);
            }
            auto builtin = StdLib::getBuiltinModule(spec.target);
            if (builtin != nullptr) return builtin;
            // Installed package? lovax_libs/<name>/<name>.lov or main.lov
            namespace fs = std::filesystem;
            fs::path root(baseDirs().front());
            fs::path candidates[2] = {
                root / "lovax_libs" / spec.target / (spec.target + ".lov"),
                root / "lovax_libs" / spec.target / "main.lov"
            };
            for (const auto& cand : candidates) {
                std::error_code ec;
                if (fs::exists(cand, ec)) {
                    return loadFileModule(fs::absolute(cand).string(), line);
                }
            }
            return makeError("unknown module '" + spec.target +
                             "' (built-ins: " + StdLib::builtinModuleList() +
                             "; no package at lovax_libs/" + spec.target + "/" +
                             "; for a file module use quotes: use \"" + spec.target + ".lov\")",
                             line);
        }
        return loadFileModule(spec.target, line);
    }

    Ref<Object> loadFileModule(const std::string& rawPath, int line) {
        namespace fs = std::filesystem;

        fs::path p(rawPath);
        if (p.is_relative()) p = fs::path(baseDirs().back()) / p;
        std::error_code ec;
        fs::path resolved = fs::weakly_canonical(p, ec);
        if (ec) resolved = p.lexically_normal();
        std::string key = resolved.string();

        if (!fs::exists(resolved)) {
            return makeError("module file not found: " + rawPath +
                             " (resolved path: " + key + ")", line);
        }

        auto& cache = moduleCache();
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;

        auto& loading = loadingStack();
        for (const auto& l : loading) {
            if (l == key) {
                std::string chain;
                for (const auto& x : loading) chain += fs::path(x).filename().string() + " -> ";
                chain += fs::path(key).filename().string();
                return makeError("circular use detected: " + chain, line);
            }
        }

        std::ifstream f(resolved, std::ios::binary);
        if (!f.is_open()) return makeError("cannot open module file: " + key, line);
        std::stringstream buf;
        buf << f.rdbuf();
        std::string source = buf.str();

        Lexer lexer(source);
        Parser parser(lexer);
        auto program = parser.parseProgram();
        if (!parser.errors().empty()) {
            std::string msg = "syntax error in module [" + rawPath + "]: " +
                              parser.errors()[0].toString();
            if (parser.errors().size() > 1) {
                msg += " (+" + std::to_string(parser.errors().size() - 1) + " more)";
            }
            return makeError(msg, line);
        }

        // The module runs on its own VM with its own globals; caches are shared.
        // The VM is kept alive for the whole program so exported functions can
        // resolve their module globals through a borrowed pointer (no leak: it
        // lives in a static, like the module cache itself).
        loading.push_back(key);
        baseDirs().push_back(resolved.parent_path().string());
        moduleVMs().push_back(std::make_unique<VM>());
        VM& moduleVM = *moduleVMs().back();
        auto result = moduleVM.interpret(program.get());
        baseDirs().pop_back();
        loading.pop_back();

        if (isError(result)) {
            auto* err = static_cast<ErrorObject*>(result.get());
            return makeError("error while loading module [" + rawPath + "]: " + err->message +
                             " (module line " + std::to_string(err->srcLine) + ")", line);
        }

        auto mod = moduleVM.exportGlobals();
        mod->moduleName = resolved.stem().string();
        cache[key] = mod;
        return mod;
    }

    // Exports this VM's defined, non-builtin globals as a frozen module map.
    Ref<MapObject> exportGlobals() {
        std::vector<std::pair<std::string, Value>> items;
        for (size_t i = 0; i < globalsTable_.names.size(); ++i) {
            if (!globalDefined_[i]) continue;
            const std::string& name = globalsTable_.names[i];
            if (builtinNames().count(name)) {
                // Skip untouched core builtins; keep user redefinitions.
                if (globals_[i].isObj() &&
                    globals_[i].obj->type() == ObjectType::BUILTIN &&
                    static_cast<BuiltinObject*>(globals_[i].obj)->name == name) {
                    continue;
                }
            }
            items.push_back({name, globals_[i]});
        }
        std::sort(items.begin(), items.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        auto mod = makeObj<MapObject>();
        GcRoot mr(mod.get());
        for (auto& [name, val] : items) {
            auto obj = toObject(val);
            GcRoot og(obj.get());   // protect while the key string is allocated
            // Rebind every exported function to this module's globals so it reads
            // its module-level state even when called from another VM.
            if (obj->type() == ObjectType::FUNCTION) {
                static_cast<ClosureObject*>(obj.get())->moduleGlobals = &globals_;
            }
            mod->set(makeObj<StringObject>(name), obj);
        }
        mod->frozen = true;
        return mod;
    }

    static std::vector<std::unique_ptr<VM>>& moduleVMs() {
        static std::vector<std::unique_ptr<VM>> vms;
        return vms;
    }

private:
    GlobalTable globalsTable_;
    std::vector<Value> globals_;
    std::vector<uint8_t> globalDefined_;
    // Raw-pointer stack: no vector bookkeeping, no destructor churn in the hot
    // loop. Popped slots keep stale values until overwritten — harmless, and the
    // whole buffer dies with the VM.
    std::unique_ptr<Value[]> stackMem_;
    Value* sp_;
    std::vector<Frame> frames_;
    std::vector<std::shared_ptr<UpvalueCell>> openUpvalues_; // sorted by slot
    std::vector<Handler> handlers_;

    // Coroutine boundary state (RFC-014). yielded_ is set by the YIELD op to make
    // run() unwind back to the resume() that invoked it; activeCoro_ is the
    // coroutine currently executing (null = main program).
    bool yielded_ = false;
    CoroObject* activeCoro_ = nullptr;
    Ref<Object> yieldValue_;   // value handed out by the last YIELD

    static std::unordered_map<std::string, Ref<MapObject>>& moduleCache() {
        static std::unordered_map<std::string, Ref<MapObject>> c;
        return c;
    }
    static std::vector<std::string>& loadingStack() {
        static std::vector<std::string> s;
        return s;
    }
    static const std::unordered_map<std::string, int>& builtinNames() {
        static std::unordered_map<std::string, int> names = [] {
            std::unordered_map<std::string, int> m;
            auto env = std::make_shared<Environment>();
            Builtins::installBuiltins(env);
            for (const auto& [k, v] : env->entries()) m[k] = 1;
            // VM-bound coroutine builtins (installed separately) count as core too,
            // so a file module doesn't re-export them.
            for (const char* n : {"spawn", "resume", "co_status", "co_done"}) m[n] = 1;
            return m;
        }();
        return names;
    }

    void installBuiltinGlobals() {
        auto env = std::make_shared<Environment>();
        Builtins::installBuiltins(env);
        for (const auto& [name, obj] : env->entries()) {
            bindGlobal(name, obj);
        }
        installCoroutineBuiltins();
    }

    void bindGlobal(const std::string& name, const Ref<Object>& obj) {
        uint16_t slot = globalsTable_.slot(name);
        if (slot >= globals_.size()) {
            globals_.resize(slot + 1);
            globalDefined_.resize(slot + 1, 0);
        }
        globals_[slot] = Value::object(obj);
        globalDefined_[slot] = 1;
    }

    // Coroutine builtins are VM-bound (they drive resume/state swaps), so they are
    // installed here rather than in the VM-agnostic Builtins table.
    void installCoroutineBuiltins() {
        VM* self = this;
        using Args = std::vector<Ref<Object>>;
        using CallFn = BuiltinObject::CallFn;

        bindGlobal("spawn", makeObj<BuiltinObject>("spawn",
            [](const Args& a, int line, const CallFn&) -> Ref<Object> {
                if (a.size() != 1)
                    return makeError("spawn() expects 1 argument (a function), got " +
                                     std::to_string(a.size()), line);
                if (a[0]->type() != ObjectType::FUNCTION)
                    return makeError("spawn() expects a function, got " + typeName(a[0]->type()), line);
                return makeObj<CoroObject>(a[0]);
            }));

        bindGlobal("resume", makeObj<BuiltinObject>("resume",
            [self](const Args& a, int line, const CallFn&) -> Ref<Object> {
                if (a.empty() || a.size() > 2)
                    return makeError("resume() expects (coroutine, [value]), got " +
                                     std::to_string(a.size()) + " arguments", line);
                if (a[0]->type() != ObjectType::COROUTINE)
                    return makeError("resume() expects a coroutine, got " + typeName(a[0]->type()), line);
                auto arg = a.size() == 2 ? a[1] : NULL_OBJ_;
                return self->resumeCoroutine(a[0], arg, line);
            }));

        bindGlobal("co_status", makeObj<BuiltinObject>("co_status",
            [](const Args& a, int line, const CallFn&) -> Ref<Object> {
                if (a.size() != 1 || a[0]->type() != ObjectType::COROUTINE)
                    return makeError("co_status() expects a coroutine", line);
                auto st = static_cast<CoroObject*>(a[0].get())->status;
                const char* s = st == CoroObject::Status::CREATED   ? "created"
                              : st == CoroObject::Status::SUSPENDED ? "suspended"
                              : st == CoroObject::Status::RUNNING   ? "running" : "dead";
                return makeObj<StringObject>(s);
            }));

        bindGlobal("co_done", makeObj<BuiltinObject>("co_done",
            [](const Args& a, int line, const CallFn&) -> Ref<Object> {
                if (a.size() != 1 || a[0]->type() != ObjectType::COROUTINE)
                    return makeError("co_done() expects a coroutine", line);
                return boolObj(static_cast<CoroObject*>(a[0].get())->status ==
                               CoroObject::Status::DEAD);
            }));
    }

    // The compiler may have registered new global names; size the tables to match.
    void syncGlobalSlots() {
        if (globals_.size() < globalsTable_.names.size()) {
            globals_.resize(globalsTable_.names.size());
            globalDefined_.resize(globalsTable_.names.size(), 0);
        }
    }

    void push(const Value& v) { *sp_++ = v; }
    void push(Value&& v) { *sp_++ = std::move(v); }
    Value pop() { return std::move(*--sp_); }
    Value& peek(int distance = 0) { return sp_[-1 - distance]; }
    size_t stackSize() const { return (size_t)(sp_ - stackMem_.get()); }
    Value* stackAt(size_t idx) { return stackMem_.get() + idx; }

    // ===== Upvalues =====

    std::shared_ptr<UpvalueCell> captureUpvalue(int slot) {
        for (auto& cell : openUpvalues_) {
            if (cell->stackSlot == slot) return cell;
        }
        auto cell = std::make_shared<UpvalueCell>(slot);
        openUpvalues_.push_back(cell);
        return cell;
    }

    void closeUpvalues(int fromSlot) {
        for (size_t i = 0; i < openUpvalues_.size();) {
            if (openUpvalues_[i]->stackSlot >= fromSlot) {
                openUpvalues_[i]->value = *stackAt(openUpvalues_[i]->stackSlot);
                // Barrier: the value leaves the root stack for a heap cell.
                if (openUpvalues_[i]->value.kind == VKind::OBJ)
                    gcShade(openUpvalues_[i]->value.obj);
                openUpvalues_[i]->closed = true;
                openUpvalues_.erase(openUpvalues_.begin() + i);
            } else {
                ++i;
            }
        }
    }

    // ===== Calls =====

    // Returns nullptr on success, or an ErrorObject.
    Ref<Object> callValue(int argc, int line) {
        Value& callee = peek(argc);

        if (callee.isObjType(ObjectType::FUNCTION)) {
            auto* closure = static_cast<ClosureObject*>(callee.obj);
            const auto& proto = *closure->proto;

            if (proto.variadic) {
                // Last parameter collects the extra args into a list.
                int fixed = proto.paramCount - 1;
                if (argc < fixed) {
                    std::string fname = proto.name.empty() ? "function" : "'" + proto.name + "'";
                    return makeError(fname + " expects at least " + std::to_string(fixed) +
                                     " parameter(s), got " + std::to_string(argc), line);
                }
                int extra = argc - fixed;
                auto rest = makeObj<ListObject>();
                GcRoot rr(rest.get());
                for (int i = extra - 1; i >= 0; --i) rest->elements.push_back(toObject(peek(i)));
                sp_ -= extra;
                push(Value::object(rest));
                argc = proto.paramCount;
            } else if (argc < proto.requiredCount || argc > proto.paramCount) {
                std::string fname = proto.name.empty() ? "function" : "'" + proto.name + "'";
                std::string expected = (proto.requiredCount == proto.paramCount)
                    ? std::to_string(proto.paramCount)
                    : std::to_string(proto.requiredCount) + "-" + std::to_string(proto.paramCount);
                return makeError(fname + " expects " + expected + " parameter(s), got " +
                                 std::to_string(argc), line);
            }
            if (frames_.size() >= MAX_FRAMES) {
                return makeError("maximum call depth exceeded (" + std::to_string(MAX_FRAMES) +
                                 ") — possible infinite recursion", line);
            }
            if (stackSize() + proto.localCount + 16 > STACK_LIMIT) {
                return makeError("value stack overflow", line);
            }

            // Missing optional args + non-parameter locals start as null.
            for (int i = argc; i < proto.localCount; ++i) push(Value::nil());

            frames_.push_back({closure, proto.chunk.code.data(),
                               stackSize() - proto.localCount - 1, argc,
                               proto.chunk.consts.data(), &proto.chunk});
            return nullptr;
        }

        if (callee.isObjType(ObjectType::BUILTIN)) {
            auto builtin = refCast<BuiltinObject>(callee.obj);
            std::vector<Ref<Object>> args;
            args.reserve(argc);
            for (int i = argc - 1; i >= 0; --i) args.push_back(toObject(peek(i)));
            // Root the args during the call: toObject boxes immediate args into
            // fresh objects that are NOT on the value stack, so a builtin that
            // allocates (or calls back into Lovax) would otherwise free them.
            Heap& h = Heap::get();
            size_t rootBase = h.tempRoots.size();
            for (auto& a : args) h.tempRoots.push_back(a.get());
            auto result = builtin->fn(args, line, callFn());
            h.tempRoots.resize(rootBase);
            if (isError(result)) return result;
            sp_ -= argc + 1;
            push(fromObject(result));
            return Ref<Object>(nullptr); // handled fully
        }

        return makeError("not a function, cannot be called: " + valueTypeName(callee), line);
    }

    // ===== The run loop =====

    Ref<Object> run(size_t exitFrameDepth) {
        Frame* frame = &frames_.back();
        const uint8_t* ip = frame->ip;
        const Value* consts = frame->consts;
        Value* slots = stackAt(frame->base + 1);

        // Keep the hot state in locals; resync whenever the frame changes or the
        // stack may reallocate (calls can grow it and invalidate 'slots').
        auto syncOut = [&]() { frame->ip = ip; };
        auto syncIn = [&]() {
            frame = &frames_.back();
            ip = frame->ip;
            consts = frame->consts;
            slots = stackAt(frame->base + 1);
        };

        auto readByte = [&]() -> uint8_t { return *ip++; };
        auto readU16 = [&]() -> uint16_t {
            uint16_t v = (uint16_t)((ip[0] << 8) | ip[1]);
            ip += 2;
            return v;
        };
        auto currentLine = [&]() -> int {
            const Chunk& c = *frame->chunk;
            size_t offset = (size_t)(ip - c.code.data()) - 1;
            if (offset >= c.lines.size()) offset = c.lines.size() - 1;
            return c.lines[offset];
        };
        auto constant = [&](uint16_t idx) -> const Value& { return consts[idx]; };

        // Unwinds everything and reports; the first error is terminal.
        auto runtimeError = [&](const Ref<Object>& err) -> Ref<Object> {
            frames_.resize(exitFrameDepth);
            return err;
        };

        // Error dispatch (RFC-008): if a try handler is active within this run's
        // frame window, unwind to it, bind the error message, and resume in catch.
        auto tryHandle = [&](const Ref<Object>& err) -> bool {
            if (handlers_.empty() || handlers_.back().frameDepth < exitFrameDepth) return false;
            Handler h = handlers_.back();
            handlers_.pop_back();
            while (frames_.size() > h.frameDepth) {
                if (!openUpvalues_.empty()) closeUpvalues((int)frames_.back().base);
                frames_.pop_back();
            }
            sp_ = stackMem_.get() + h.stackTop;
            if (isError(err) && static_cast<ErrorObject*>(err.get())->payload != nullptr) {
                // Structured throw: the handler receives the original value.
                push(fromObject(static_cast<ErrorObject*>(err.get())->payload));
            } else {
                std::string msg = isError(err) ? static_cast<ErrorObject*>(err.get())->message
                                               : valueInspect(fromObject(err));
                push(Value::object(makeObj<StringObject>(msg)));
            }
            frame = &frames_.back();
            ip = h.catchIp;
            consts = frame->consts;
            slots = stackAt(frame->base + 1);
            return true;
        };
        // Raises err: jump to the nearest handler, or unwind and return it.
        #define VM_THROW(E) { Ref<Object> _e = (E); if (tryHandle(_e)) continue; frames_.resize(exitFrameDepth); return _e; }

        for (;;) {
#ifdef LOVAX_CG
            static const void* lovax_dispatch[] = {
            &&L_CONST,
            &&L_NIL,
            &&L_TRUE_,
            &&L_FALSE_,
            &&L_POP,
            &&L_DUP,
            &&L_GET_LOCAL,
            &&L_SET_LOCAL,
            &&L_GET_GLOBAL,
            &&L_DEFINE_GLOBAL,
            &&L_SET_GLOBAL,
            &&L_GET_UPVALUE,
            &&L_SET_UPVALUE,
            &&L_EQUAL,
            &&L_NOT_EQUAL,
            &&L_ADD,
            &&L_SUB,
            &&L_MUL,
            &&L_DIV,
            &&L_MOD,
            &&L_POW,
            &&L_LESS,
            &&L_GREATER,
            &&L_LESS_EQ,
            &&L_GREATER_EQ,
            &&L_BIT_AND,
            &&L_BIT_OR,
            &&L_BIT_XOR,
            &&L_SHL,
            &&L_SHR,
            &&L_IN,
            &&L_NEGATE,
            &&L_NOT_,
            &&L_BIT_NOT,
            &&L_JUMP,
            &&L_JUMP_IF_FALSE,
            &&L_LOOP,
            &&L_AND_KEEP,
            &&L_OR_KEEP,
            &&L_CALL,
            &&L_CALL_METHOD,
            &&L_CLOSURE,
            &&L_ARG_DEFAULT,
            &&L_RETURN,
            &&L_LIST,
            &&L_MAP,
            &&L_INDEX_GET,
            &&L_INDEX_GET_KEEP,
            &&L_INDEX_SET,
            &&L_MEMBER_GET,
            &&L_MEMBER_GET_SAFE,
            &&L_MEMBER_GET_KEEP,
            &&L_MEMBER_SET,
            &&L_INTERP,
            &&L_SAY,
            &&L_FOR_SETUP,
            &&L_FOR_NEXT,
            &&L_CLOSE_UPVALUE,
            &&L_USE,
            &&L_RUNTIME_ERROR,
            &&L_TRY_PUSH,
            &&L_TRY_POP,
            &&L_THROW_,
            &&L_COALESCE,
            &&L_RANGE_NEW,
            &&L_IS_TYPE,
            &&L_UNPACK,
            &&L_SLICE,
            &&L_ADD_I,
            &&L_SUB_I,
            &&L_MUL_I,
            &&L_MOD_I,
            &&L_BAND_I,
            &&L_BOR_I,
            &&L_BXOR_I,
            &&L_LESS_JF,
            &&L_LESS_EQ_JF,
            &&L_GREATER_JF,
            &&L_GREATER_EQ_JF,
            &&L_EQUAL_JF,
            &&L_NOT_EQUAL_JF,
            &&L_LGET2,
            &&L_LGET_ADD_I,
            &&L_LGET_SUB_I,
            &&L_ADD_INPLACE,
            &&L_LT_I_JF,
            &&L_LE_I_JF,
            &&L_GT_I_JF,
            &&L_GE_I_JF,
            &&L_EQ_I_JF,
            &&L_NE_I_JF,
            &&L_YIELD_,
            &&L_STRUCT_SHAPE,
            &&L_STRUCT_BIND,
            &&L_STRUCT_MAKE,
            &&L_TUPLE,
            &&L_HALT
            };
        #define VM_CASE(name) L_##name:
        // Safe everywhere: a plain goto runs destructors of handler locals
        // before the (computed) dispatch at the loop head.
        #define VM_NEXT       goto lovax_dispatch_next
        // Only in handlers with NO named non-trivial locals in scope: a computed
        // goto skips destructors (GCC cannot see the target), so 'Value'/string
        // locals alive here would leak.
        #define VM_NEXT_FAST  { op = (Op)*ip++; goto *lovax_dispatch[(uint8_t)op]; }
            Op op;
        lovax_dispatch_next:
            VM_NEXT_FAST;
#else
        #define VM_CASE(name) case Op::name:
        #define VM_NEXT       break
        #define VM_NEXT_FAST  break
            Op op = (Op)readByte();
            switch (op) {
#endif
                VM_CASE(CONST) push(constant(readU16())); VM_NEXT_FAST;
                VM_CASE(NIL)    push(Value::nil()); VM_NEXT_FAST;
                VM_CASE(TRUE_)  push(Value::boolean(true)); VM_NEXT_FAST;
                VM_CASE(FALSE_) push(Value::boolean(false)); VM_NEXT_FAST;
                VM_CASE(POP)    pop(); VM_NEXT_FAST;
                VM_CASE(DUP)    push(peek()); VM_NEXT_FAST;

                VM_CASE(GET_LOCAL) push(slots[readU16()]); VM_NEXT_FAST;
                VM_CASE(SET_LOCAL) slots[readU16()] = pop(); VM_NEXT_FAST;

                VM_CASE(GET_GLOBAL) {
                    uint16_t slot = readU16();
                    // Module functions resolve globals against their own module.
                    std::vector<Value>* mg = frame->closure->moduleGlobals;
                    if (mg) {
                        if (slot < mg->size()) push((*mg)[slot]);
                        else push(Value::nil());
                        VM_NEXT_FAST;
                    }
                    if (!globalDefined_[slot]) {
                        const std::string& name = globalsTable_.names[slot];
                        VM_THROW(makeError(
                            "undefined variable '" + name +
                            "' (define it with: set " + name + " = ...)", currentLine()));
                    }
                    push(globals_[slot]);
                    VM_NEXT_FAST;
                }
                VM_CASE(DEFINE_GLOBAL) {
                    uint16_t slot = readU16();
                    globals_[slot] = pop();
                    globalDefined_[slot] = 1;
                    VM_NEXT_FAST;
                }
                VM_CASE(SET_GLOBAL) {
                    uint16_t slot = readU16();
                    std::vector<Value>* mg = frame->closure->moduleGlobals;
                    if (mg) {
                        if (slot < mg->size()) (*mg)[slot] = pop();
                        else pop();
                        VM_NEXT_FAST;
                    }
                    if (!globalDefined_[slot]) {
                        const std::string& name = globalsTable_.names[slot];
                        VM_THROW(makeError(
                            "undefined variable '" + name +
                            "' (define it with: set " + name + " = ...)", currentLine()));
                    }
                    globals_[slot] = pop();
                    VM_NEXT_FAST;
                }
                VM_CASE(GET_UPVALUE) {
                    auto& cell = frame->closure->upvalues[readU16()];
                    push(cell->closed ? cell->value : *stackAt(cell->stackSlot));
                    VM_NEXT_FAST;
                }
                VM_CASE(SET_UPVALUE) {
                    auto& cell = frame->closure->upvalues[readU16()];
                    if (cell->closed) {
                        cell->value = pop();
                        // Barrier: the owning closure may already be black.
                        if (cell->value.kind == VKind::OBJ) gcShade(cell->value.obj);
                    }
                    else *stackAt(cell->stackSlot) = pop();
                    VM_NEXT_FAST;
                }

                VM_CASE(EQUAL) {
                    bool eq = valueEquals(sp_[-2], sp_[-1]);
                    sp_[-1].obj = nullptr; sp_[-2].obj = nullptr;
                    sp_[-2].kind = VKind::BOOL; sp_[-2].b = eq; --sp_;
                    VM_NEXT_FAST;
                }
                VM_CASE(NOT_EQUAL) {
                    bool eq = valueEquals(sp_[-2], sp_[-1]);
                    sp_[-1].obj = nullptr; sp_[-2].obj = nullptr;
                    sp_[-2].kind = VKind::BOOL; sp_[-2].b = !eq; --sp_;
                    VM_NEXT_FAST;
                }

                // In-place numeric fast path: both operands stay in their stack slots
                // (numbers carry no shared_ptr, so overwriting them is free); only the
                // object slow path materializes Values, inside its own scope so the
                // destructor-safe VM_NEXT applies.
                #define NUMERIC_FAST(opChar, intExpr, dblExpr)                              \
                    Value* pa = sp_ - 2; Value* pb = sp_ - 1;                               \
                    if (pa->kind == VKind::INT && pb->kind == VKind::INT) {                 \
                        long long l = pa->i, r = pb->i; (void)l; (void)r;                   \
                        *pa = intExpr; --sp_; VM_NEXT_FAST;                                 \
                    }                                                                       \
                    if (pa->isNumber() && pb->isNumber()) {                                 \
                        double l = pa->asDouble(), r = pb->asDouble(); (void)l; (void)r;    \
                        *pa = dblExpr; --sp_; VM_NEXT_FAST;                                 \
                    }                                                                       \
                    {                                                                       \
                        Value b = pop(), a = pop();                                         \
                        auto res = Runtime::evalInfixExpression(opChar, toObject(a),        \
                                                                toObject(b), currentLine());\
                        if (isError(res)) VM_THROW(res);                                    \
                        push(fromObject(res));                                              \
                    }

                VM_CASE(ADD) { NUMERIC_FAST("+", Value::integer(l + r), Value::real(l + r)); VM_NEXT; }
                VM_CASE(SUB) { NUMERIC_FAST("-", Value::integer(l - r), Value::real(l - r)); VM_NEXT; }
                VM_CASE(MUL) { NUMERIC_FAST("*", Value::integer(l * r), Value::real(l * r)); VM_NEXT; }
                VM_CASE(DIV) {
                    Value* pa = sp_ - 2; Value* pb = sp_ - 1;
                    if (pa->kind == VKind::INT && pb->kind == VKind::INT) {
                        if (pb->i == 0) VM_THROW(makeError("division by zero", currentLine()));
                        long long q = pa->i / pb->i;
                        if ((pa->i % pb->i != 0) && ((pa->i < 0) != (pb->i < 0))) q--;
                        pa->i = q; --sp_; VM_NEXT_FAST;
                    }
                    if (pa->isNumber() && pb->isNumber()) {
                        double r = pb->asDouble();
                        if (r == 0.0) VM_THROW(makeError("division by zero", currentLine()));
                        *pa = Value::real(pa->asDouble() / r); --sp_; VM_NEXT_FAST;
                    }
                    {
                        Value b = pop(), a = pop();
                        auto res = Runtime::evalInfixExpression("/", toObject(a), toObject(b), currentLine());
                        if (isError(res)) VM_THROW(res);
                        push(fromObject(res));
                    }
                    VM_NEXT;
                }
                VM_CASE(MOD) {
                    Value* pa = sp_ - 2; Value* pb = sp_ - 1;
                    if (pa->kind == VKind::INT && pb->kind == VKind::INT) {
                        if (pb->i == 0) VM_THROW(makeError("modulo by zero", currentLine()));
                        long long m = pa->i % pb->i;
                        if (m != 0 && ((m < 0) != (pb->i < 0))) m += pb->i;
                        pa->i = m; --sp_; VM_NEXT_FAST;
                    }
                    if (pa->isNumber() && pb->isNumber()) {
                        double r = pb->asDouble();
                        if (r == 0.0) VM_THROW(makeError("modulo by zero", currentLine()));
                        double m = std::fmod(pa->asDouble(), r);
                        if (m != 0.0 && ((m < 0.0) != (r < 0.0))) m += r;
                        *pa = Value::real(m); --sp_; VM_NEXT_FAST;
                    }
                    {
                        Value b = pop(), a = pop();
                        auto res = Runtime::evalInfixExpression("%", toObject(a), toObject(b), currentLine());
                        if (isError(res)) VM_THROW(res);
                        push(fromObject(res));
                    }
                    VM_NEXT;
                }
                VM_CASE(POW) {
                    Value b = pop(), a = pop();
                    if (a.isNumber() && b.isNumber()) {
                        double result = std::pow(a.asDouble(), b.asDouble());
                        if (a.kind == VKind::INT && b.kind == VKind::INT &&
                            b.i >= 0 && result == std::floor(result) && std::fabs(result) < 9.2e18) {
                            push(Value::integer((long long)result));
                        } else {
                            push(Value::real(result));
                        }
                    } else {
                        auto res = Runtime::evalInfixExpression("**", toObject(a), toObject(b), currentLine());
                        if (isError(res)) VM_THROW(res);
                        push(fromObject(res));
                    }
                    VM_NEXT;
                }
                VM_CASE(LESS)       { NUMERIC_FAST("<",  Value::boolean(l < r),  Value::boolean(l < r));  VM_NEXT; }
                VM_CASE(GREATER)    { NUMERIC_FAST(">",  Value::boolean(l > r),  Value::boolean(l > r));  VM_NEXT; }
                VM_CASE(LESS_EQ)    { NUMERIC_FAST("<=", Value::boolean(l <= r), Value::boolean(l <= r)); VM_NEXT; }
                VM_CASE(GREATER_EQ) { NUMERIC_FAST(">=", Value::boolean(l >= r), Value::boolean(l >= r)); VM_NEXT; }

                #define INT_ONLY_OP(opStr, expr)                                            \
                    Value* pa = sp_ - 2; Value* pb = sp_ - 1;                               \
                    if (pa->kind == VKind::INT && pb->kind == VKind::INT) {                 \
                        long long l = pa->i, r = pb->i; (void)l; (void)r;                   \
                        *pa = expr; --sp_; VM_NEXT_FAST;                                    \
                    }                                                                       \
                    {                                                                       \
                        Value b = pop(), a = pop();                                         \
                        auto res = Runtime::evalInfixExpression(opStr, toObject(a),         \
                                                                toObject(b), currentLine());\
                        if (isError(res)) VM_THROW(res);                                    \
                        push(fromObject(res));                                              \
                    }

                VM_CASE(BIT_AND) { INT_ONLY_OP("&", Value::integer(l & r)); VM_NEXT; }
                VM_CASE(BIT_OR)  { INT_ONLY_OP("|", Value::integer(l | r)); VM_NEXT; }
                VM_CASE(BIT_XOR) { INT_ONLY_OP("^", Value::integer(l ^ r)); VM_NEXT; }
                VM_CASE(SHL) VM_CASE(SHR) {
                    Value b = pop(), a = pop();
                    const char* opStr = (op == Op::SHL) ? "<<" : ">>";
                    if (a.kind == VKind::INT && b.kind == VKind::INT) {
                        if (b.i < 0 || b.i > 63) {
                            VM_THROW(makeError(
                                "shift amount must be within 0-63: " + std::to_string(b.i),
                                currentLine()));
                        }
                        push(Value::integer(op == Op::SHL ? (a.i << b.i) : (a.i >> b.i)));
                    } else {
                        auto res = Runtime::evalInfixExpression(opStr, toObject(a), toObject(b), currentLine());
                        if (isError(res)) VM_THROW(res);
                        push(fromObject(res));
                    }
                    VM_NEXT;
                }
                VM_CASE(IN) {
                    Value b = pop(), a = pop();
                    auto res = Runtime::evalInfixExpression("in", toObject(a), toObject(b), currentLine());
                    if (isError(res)) VM_THROW(res);
                    push(fromObject(res));
                    VM_NEXT;
                }

                VM_CASE(NEGATE) {
                    Value* pa = sp_ - 1;
                    if (pa->kind == VKind::INT)   { pa->i = -pa->i; VM_NEXT_FAST; }
                    if (pa->kind == VKind::FLOAT) { pa->d = -pa->d; VM_NEXT_FAST; }
                    if (pa->isObjType(ObjectType::COMPLEX)) {
                        auto* c = static_cast<ComplexObject*>(pa->obj);
                        *pa = Value::object(makeObj<ComplexObject>(-c->re, -c->im));
                        VM_NEXT;
                    }
                    VM_THROW(makeError(
                        "unary '-' only works on numbers, got " + valueTypeName(*pa),
                        currentLine()));
                }
                VM_CASE(NOT_) {
                    bool t = valueTruthy(sp_[-1]);
                    sp_[-1].obj = nullptr;
                    sp_[-1].kind = VKind::BOOL; sp_[-1].b = !t;
                    VM_NEXT_FAST;
                }
                VM_CASE(BIT_NOT) {
                    Value* pa = sp_ - 1;
                    if (pa->kind == VKind::INT) { pa->i = ~pa->i; VM_NEXT_FAST; }
                    VM_THROW(makeError(
                        "'~' only works on integers, got " + valueTypeName(*pa),
                        currentLine()));
                }

                VM_CASE(JUMP) { uint16_t d = readU16(); ip += d; VM_NEXT_FAST; }
                VM_CASE(JUMP_IF_FALSE) {
                    uint16_t d = readU16();
                    if (!valueTruthy(pop())) ip += d;
                    VM_NEXT_FAST;
                }
                VM_CASE(LOOP) { uint16_t d = readU16(); ip -= d; gcSafepoint(); VM_NEXT; }
                VM_CASE(AND_KEEP) {
                    uint16_t d = readU16();
                    if (!valueTruthy(peek())) ip += d;
                    else pop();
                    VM_NEXT_FAST;
                }
                VM_CASE(OR_KEEP) {
                    uint16_t d = readU16();
                    if (valueTruthy(peek())) ip += d;
                    else pop();
                    VM_NEXT_FAST;
                }

                VM_CASE(CALL) {
                    int argc = readByte();
                    gcSafepoint();   // stack = [callee, args..] are all roots here
                    // Exact-arity closure call, inlined: no defaults in play, no
                    // variadic collection — the dominant call shape in real code.
                    {
                        Value& callee = peek(argc);
                        if (callee.isObjType(ObjectType::FUNCTION)) {
                            ClosureObject* cl = static_cast<ClosureObject*>(callee.obj);
                            const Proto& p = *cl->proto;
                            if (!p.variadic && argc == p.paramCount &&
                                frames_.size() < MAX_FRAMES &&
                                stackSize() + p.localCount + 16 <= STACK_LIMIT) {
                                syncOut();
                                for (int i = argc; i < p.localCount; ++i) push(Value::nil());
                                frames_.push_back({cl, p.chunk.code.data(),
                                                   stackSize() - p.localCount - 1, argc,
                                                   p.chunk.consts.data(), &p.chunk});
                                syncIn();
                                VM_NEXT_FAST;
                            }
                        }
                    }
                    int line = currentLine();
                    syncOut();
                    {
                        auto err = callValue(argc, line);
                        if (err != nullptr && isError(err)) VM_THROW(err);
                    }
                    syncIn();
                    VM_NEXT_FAST;
                }
                VM_CASE(CALL_METHOD) {
                    int argc = readByte();
                    gcSafepoint();   // stack = [recv, method, args..] are all roots
                    int line = currentLine();
                    // Stack: [recv, method, a1..aN]. Pass recv as 'this' only when it
                    // is a struct instance (a map tagged with __type__); otherwise call
                    // plainly (module/plain-map field call keeps its original arity).
                    static const std::string typeKey = "__type__";
                    Value& recv = peek(argc + 1);
                    bool isStruct = recv.isObjType(ObjectType::STRUCT) ||
                        (recv.isObjType(ObjectType::MAP) &&
                         static_cast<MapObject*>(recv.obj)->getStr(typeKey) != nullptr);
                    syncOut();
                    {
                        Ref<Object> err;
                        if (isStruct) {
                            Value* basep = sp_ - argc - 2;   // [0]=recv, [1]=method
                            std::swap(basep[0], basep[1]);   // -> [method, recv, args..]
                            err = callValue(argc + 1, line);
                        } else {
                            Value* basep = sp_ - argc - 2;
                            for (int i = 0; i <= argc; ++i) basep[i] = basep[i + 1]; // drop recv
                            --sp_;
                            err = callValue(argc, line);
                        }
                        if (err != nullptr && isError(err)) VM_THROW(err);
                    }
                    syncIn();
                    VM_NEXT_FAST;
                }
                VM_CASE(CLOSURE) {
                    uint16_t protoIdx = readU16();
                    auto holder = refCast<ProtoObject>(constant(protoIdx).obj);
                    auto closure = makeObj<ClosureObject>(holder->proto);
                    // Functions created inside a module keep resolving globals
                    // against that module.
                    closure->moduleGlobals = frame->closure->moduleGlobals;
                    uint16_t upvalCount = readU16();
                    for (uint16_t i = 0; i < upvalCount; ++i) {
                        uint8_t isLocal = readByte();
                        uint16_t index = readU16();
                        if (isLocal) {
                            closure->upvalues.push_back(
                                captureUpvalue((int)(frame->base + 1 + index)));
                        } else {
                            closure->upvalues.push_back(frame->closure->upvalues[index]);
                        }
                    }
                    push(Value::object(closure));
                    VM_NEXT;
                }
                VM_CASE(ARG_DEFAULT) {
                    uint16_t paramSlot = readU16();
                    uint16_t skip = readU16();
                    if (frame->nargs > (int)paramSlot) ip += skip;
                    VM_NEXT;
                }
                VM_CASE(STRUCT_SHAPE) {
                    // Builds the per-type shape at declaration time. Method
                    // closures sit on the stack (rooted); the shape is GcRoot'd
                    // while its tables fill up.
                    uint16_t nameC = readU16();
                    uint16_t nf = readU16();
                    auto shape = makeObj<StructShapeObject>();
                    GcRoot sr(shape.get());
                    shape->name = static_cast<StringObject*>(constant(nameC).obj)->value;
                    shape->fieldNames.reserve(nf);
                    for (uint16_t i = 0; i < nf; ++i) {
                        uint16_t fc = readU16();
                        auto key = refCast<StringObject>(constant(fc).obj);
                        shape->fieldIndex[key->value] = (int)i;
                        shape->fieldNames.push_back(key);
                    }
                    uint16_t nm = readU16();
                    shape->methods.reserve(nm);
                    for (uint16_t i = 0; i < nm; ++i) {
                        uint16_t mc = readU16();
                        auto key = refCast<StringObject>(constant(mc).obj);
                        // Methods were pushed in declaration order: oldest deepest.
                        shape->methodIndex[key->value] = shape->methods.size();
                        shape->methods.push_back({key, toObject(peek(nm - 1 - i))});
                    }
                    sp_ -= nm;
                    push(Value::object(shape));
                    VM_NEXT;
                }
                VM_CASE(STRUCT_BIND) {
                    // Stack: [shape, factory] -> attach and keep the factory.
                    Value fac = pop();
                    Value shp = pop();
                    static_cast<ClosureObject*>(fac.obj)->structShape = toObject(shp);
                    push(fac);
                    VM_NEXT;
                }
                VM_CASE(STRUCT_MAKE) {
                    uint16_t nf = readU16();
                    auto* factory = frame->closure;
                    auto inst = makeObj<StructInstanceObject>();
                    GcRoot ir(inst.get());
                    inst->shape = static_cast<StructShapeObject*>(factory->structShape.get());
                    inst->slots.reserve(nf);
                    // Field locals live at base+1..base+nf; boxing may allocate,
                    // but the locals are stack roots and inst is temp-rooted.
                    for (uint16_t i = 0; i < nf; ++i) {
                        inst->slots.push_back(toObject(*stackAt(frame->base + 1 + i)));
                    }
                    push(Value::object(inst));
                    VM_NEXT;
                }
                VM_CASE(RETURN) {
                    if (!openUpvalues_.empty()) closeUpvalues((int)frame->base);
                    // Move the result from the stack top into the callee slot; the
                    // move-assign releases the callee closure reference in passing.
                    *stackAt(frame->base) = std::move(sp_[-1]);
                    sp_ = stackAt(frame->base) + 1;
                    frames_.pop_back();
                    if (frames_.size() == exitFrameDepth) return NULL_OBJ_;
                    syncIn();
                    VM_NEXT_FAST;
                }

                VM_CASE(LIST) {
                    uint16_t n = readU16();
                    auto list = makeObj<ListObject>();
                    GcRoot lr(list.get());   // boxing int elements allocates -> GC
                    list->elements.reserve(n);
                    for (int i = n - 1; i >= 0; --i) list->elements.push_back(toObject(peek(i)));
                    sp_ -= n;
                    push(Value::object(list));
                    VM_NEXT;
                }
                VM_CASE(TUPLE) {
                    uint16_t n = readU16();
                    auto tup = makeObj<TupleObject>();
                    GcRoot tr(tup.get());    // boxing int elements allocates -> GC
                    tup->elements.reserve(n);
                    for (int i = n - 1; i >= 0; --i) tup->elements.push_back(toObject(peek(i)));
                    sp_ -= n;
                    push(Value::object(tup));
                    VM_NEXT;
                }
                VM_CASE(MAP) {
                    uint16_t n = readU16();
                    auto map = makeObj<MapObject>();
                    GcRoot mr(map.get());    // boxing keys/values allocates -> GC
                    int line = currentLine();
                    for (int i = 0; i < n; ++i) {
                        auto key = toObject(peek((n - i) * 2 - 1));
                        GcRoot kr(key.get()); // protect key while the value is boxed
                        auto val = toObject(peek((n - i) * 2 - 2));
                        if (!Runtime::isValidMapKey(key)) {
                            VM_THROW(makeError(
                                "map keys must be string, int or bool; got " +
                                typeName(key->type()), line));
                        }
                        map->set(key, val);
                    }
                    sp_ -= n * 2;
                    push(Value::object(map));
                    VM_NEXT;
                }

                VM_CASE(INDEX_GET) {
                    Value idx = pop(), obj = pop();
                    if (obj.isObjType(ObjectType::LIST) && idx.kind == VKind::INT) {
                        auto* list = static_cast<ListObject*>(obj.obj);
                        long long i = idx.i;
                        long long n = (long long)list->elements.size();
                        if (i < 0) i += n;
                        if (i >= 0 && i < n) {
                            push(fromObject(list->elements[i]));
                            VM_NEXT;
                        }
                    }
                    auto res = Runtime::evalIndexAccess(toObject(obj), toObject(idx), currentLine());
                    if (isError(res)) VM_THROW(res);
                    push(fromObject(res));
                    VM_NEXT;
                }
                VM_CASE(INDEX_GET_KEEP) {
                    Value& obj = peek(1);
                    Value& idx = peek(0);
                    if (obj.isObjType(ObjectType::LIST) && idx.kind == VKind::INT) {
                        auto* list = static_cast<ListObject*>(obj.obj);
                        long long i = idx.i;
                        long long n = (long long)list->elements.size();
                        if (i < 0) i += n;
                        if (i >= 0 && i < n) {
                            push(fromObject(list->elements[i]));
                            VM_NEXT;
                        }
                    }
                    auto res = Runtime::evalIndexAccess(toObject(peek(1)), toObject(peek(0)),
                                                        currentLine());
                    if (isError(res)) VM_THROW(res);
                    push(fromObject(res));
                    VM_NEXT;
                }
                VM_CASE(INDEX_SET) {
                    Value val = pop(), idx = pop(), obj = pop();
                    if (obj.isObjType(ObjectType::LIST) && idx.kind == VKind::INT) {
                        auto* list = static_cast<ListObject*>(obj.obj);
                        long long i = idx.i;
                        long long n = (long long)list->elements.size();
                        if (i < 0) i += n;
                        if (i >= 0 && i < n) {
                            auto nv = toObject(val);
                            gcShade(nv.get());          // write barrier (RFC-023)
                            list->elements[i] = nv;
                            VM_NEXT;
                        }
                    }
                    auto err = indexSet(toObject(obj), toObject(idx), toObject(val), currentLine());
                    if (err != nullptr) VM_THROW(err);
                    VM_NEXT;
                }
                // Member-access inline cache: each site remembers the entry index
                // that answered last time. A hit is verified against the LIVE entry
                // (string equality, SSO memcmp) — a stale index can only miss, never
                // return the wrong field. Struct factories insert fields in a fixed
                // order, so every instance of a struct hits the same index.
                #define IC_LOOKUP(mapPtr, propRef, icsIdx, entOut)                      \
                    const MapObject* icm = (mapPtr);                                    \
                    uint32_t& ic = frame->chunk->icache[icsIdx];                        \
                    const std::pair<Ref<Object>, Ref<Object>>*  \
                        entOut = nullptr;                                               \
                    if (ic < icm->entries.size()) {                                     \
                        const auto& cand = icm->entries[ic];                            \
                        if (cand.first->type() == ObjectType::STRING &&                 \
                            static_cast<StringObject*>(cand.first.get())->value ==      \
                                (propRef)) {                                            \
                            entOut = &cand;                                             \
                        }                                                               \
                    }                                                                   \
                    if (entOut == nullptr) {                                            \
                        size_t pos = icm->findStr(propRef);                             \
                        if (pos != MapObject::NPOS) {                                   \
                            ic = (uint32_t)pos;                                         \
                            entOut = &icm->entries[pos];                                \
                        }                                                               \
                    }

                // Struct fast path: the inline-cache slot stores the field's slot
                // index, verified against the shape's field name (like the map IC,
                // a stale index can only miss). Methods/__type__/missing-field all
                // resolve in the shared fallback (evalMemberAccess).
                #define STRUCT_IC_GET(top, icsIdx, ACT)                                  \
                    if ((top)->isObjType(ObjectType::STRUCT)) {                          \
                        auto* si = static_cast<StructInstanceObject*>((top)->obj);       \
                        uint32_t& ic = frame->chunk->icache[icsIdx];                     \
                        if (ic < si->slots.size() &&                                     \
                            si->shape->fieldNames[ic]->value == prop) {                  \
                            ACT(si->slots[ic]);                                          \
                            VM_NEXT_FAST;                                                \
                        }                                                                \
                        auto it = si->shape->fieldIndex.find(prop);                      \
                        if (it != si->shape->fieldIndex.end()) {                         \
                            ic = (uint32_t)it->second;                                   \
                            ACT(si->slots[it->second]);                                  \
                            VM_NEXT_FAST;                                                \
                        }                                                                \
                    }

                VM_CASE(MEMBER_GET) {
                    uint16_t nameC = readU16(), ics = readU16();
                    const std::string& prop =
                        static_cast<StringObject*>(constant(nameC).obj)->value;
                    Value* top = sp_ - 1;
                    #define STRUCT_ACT_REPLACE(slotRef) *top = fromObject(slotRef)
                    STRUCT_IC_GET(top, ics, STRUCT_ACT_REPLACE)
                    #undef STRUCT_ACT_REPLACE
                    if (top->isObjType(ObjectType::MAP)) {
                        IC_LOOKUP(static_cast<MapObject*>(top->obj), prop, ics, ent)
                        if (ent != nullptr) {
                            *top = fromObject(ent->second);
                            VM_NEXT_FAST;
                        }
                    }
                    {
                        Value obj = pop();
                        auto res = Runtime::evalMemberAccess(toObject(obj), prop, currentLine());
                        if (isError(res)) VM_THROW(res);
                        push(fromObject(res));
                    }
                    VM_NEXT;
                }
                VM_CASE(MEMBER_GET_SAFE) {
                    uint16_t nameC = readU16(), ics = readU16();
                    Value* top = sp_ - 1;
                    if (top->isNil()) VM_NEXT_FAST;            // a?.b -> null (in place)
                    const std::string& prop =
                        static_cast<StringObject*>(constant(nameC).obj)->value;
                    #define STRUCT_ACT_REPLACE(slotRef) *top = fromObject(slotRef)
                    STRUCT_IC_GET(top, ics, STRUCT_ACT_REPLACE)
                    #undef STRUCT_ACT_REPLACE
                    if (top->isObjType(ObjectType::MAP)) {
                        IC_LOOKUP(static_cast<MapObject*>(top->obj), prop, ics, ent)
                        if (ent != nullptr) {
                            *top = fromObject(ent->second);
                            VM_NEXT_FAST;
                        }
                    }
                    {
                        Value obj = pop();
                        auto res = Runtime::evalMemberAccess(toObject(obj), prop, currentLine());
                        if (isError(res)) VM_THROW(res);
                        push(fromObject(res));
                    }
                    VM_NEXT;
                }
                VM_CASE(MEMBER_GET_KEEP) {
                    uint16_t nameC = readU16(), ics = readU16();
                    const std::string& prop =
                        static_cast<StringObject*>(constant(nameC).obj)->value;
                    Value* top = sp_ - 1;
                    #define STRUCT_ACT_PUSH(slotRef) push(fromObject(slotRef))
                    STRUCT_IC_GET(top, ics, STRUCT_ACT_PUSH)
                    #undef STRUCT_ACT_PUSH
                    if (top->isObjType(ObjectType::MAP)) {
                        IC_LOOKUP(static_cast<MapObject*>(top->obj), prop, ics, ent)
                        if (ent != nullptr) {
                            push(fromObject(ent->second));
                            VM_NEXT_FAST;
                        }
                    }
                    {
                        auto res = Runtime::evalMemberAccess(toObject(peek(0)), prop, currentLine());
                        if (isError(res)) VM_THROW(res);
                        push(fromObject(res));
                    }
                    VM_NEXT;
                }
                VM_CASE(MEMBER_SET) {
                    uint16_t nameC = readU16(), ics = readU16();
                    const std::string& prop =
                        static_cast<StringObject*>(constant(nameC).obj)->value;
                    Value* pobj = sp_ - 2; Value* pval = sp_ - 1;
                    if (pobj->isObjType(ObjectType::STRUCT)) {
                        auto* si = static_cast<StructInstanceObject*>(pobj->obj);
                        uint32_t& ic = frame->chunk->icache[ics];
                        size_t slot = MapObject::NPOS;
                        if (ic < si->slots.size() &&
                            si->shape->fieldNames[ic]->value == prop) {
                            slot = ic;
                        } else {
                            auto it = si->shape->fieldIndex.find(prop);
                            if (it != si->shape->fieldIndex.end()) {
                                ic = (uint32_t)it->second;
                                slot = (size_t)it->second;
                            }
                        }
                        if (slot != MapObject::NPOS) {
                            auto nv = toObject(*pval);
                            gcShade(nv.get());          // write barrier (RFC-023)
                            si->slots[slot] = nv;
                            pval->obj = nullptr; pobj->obj = nullptr; sp_ -= 2;
                            VM_NEXT_FAST;
                        }
                        // Struct fields are fixed at declaration: no new fields.
                        sp_ -= 2;
                        VM_THROW(makeError("struct '" + si->shape->name +
                                           "' has no field '" + prop +
                                           "' (fields are fixed at declaration)",
                                           currentLine()));
                    }
                    if (pobj->isObjType(ObjectType::MAP) &&
                        !static_cast<MapObject*>(pobj->obj)->frozen) {
                        auto* m = static_cast<MapObject*>(pobj->obj);
                        IC_LOOKUP(m, prop, ics, ent)
                        if (ent != nullptr) {
                            auto nv = toObject(*pval);
                            gcShade(nv.get());          // write barrier (RFC-023)
                            const_cast<std::pair<Ref<Object>,
                                Ref<Object>>*>(ent)->second = nv;
                            pval->obj = nullptr; pobj->obj = nullptr; sp_ -= 2;
                            VM_NEXT_FAST;
                        }
                    }
                    {
                        Value val = pop(), obj = pop();
                        auto err = memberSet(toObject(obj), prop, toObject(val), currentLine());
                        if (err != nullptr) VM_THROW(err);
                    }
                    VM_NEXT;
                }

                VM_CASE(INTERP) {
                    uint16_t n = readU16();
                    std::string out;
                    for (int i = n - 1; i >= 0; --i) out += valueInspect(peek(i));
                    sp_ -= n;
                    push(Value::object(makeObj<StringObject>(out)));
                    VM_NEXT;
                }
                VM_CASE(SAY) {
                    uint8_t n = readByte();
                    std::string out;
                    VKind firstKind = VKind::NIL;
                    ObjectType firstObjType = ObjectType::NULL_OBJ;
                    for (int i = n - 1; i >= 0; --i) {
                        if (i == n - 1) {
                            firstKind = peek(i).kind;
                            if (peek(i).isObj()) firstObjType = peek(i).obj->type();
                        } else {
                            out += " ";
                        }
                        out += valueInspect(peek(i));
                    }
                    sp_ -= n;

                    std::string color;
                    switch (firstKind) {
                        case VKind::INT: case VKind::FLOAT: color = Color::yellow(); break;
                        case VKind::BOOL:                   color = Color::blue();   break;
                        case VKind::NIL:                    color = Color::red();    break;
                        case VKind::OBJ:
                            color = (firstObjType == ObjectType::STRING) ? Color::green()
                                                                         : Color::cyan();
                            break;
                    }
                    std::cout << color << out << Color::reset() << "\n";
                    VM_NEXT;
                }

                VM_CASE(FOR_SETUP) {
                    Value iterable = peek();   // stays on the stack (a root) while we allocate
                    auto iter = makeObj<IterObject>();
                    GcRoot ir(iter.get());
                    auto obj = toObject(iterable);
                    switch (obj->type()) {
                        case ObjectType::LIST:
                        case ObjectType::TUPLE:   // same layout; iteration is read-only
                            iter->kind = IterObject::Kind::LIST;
                            iter->source = obj;
                            break;
                        case ObjectType::RANGE: {
                            iter->kind = IterObject::Kind::RANGE;
                            iter->source = obj;
                            iter->index = static_cast<RangeObject*>(obj.get())->start;
                            break;
                        }
                        case ObjectType::STRING:
                            iter->kind = IterObject::Kind::STRING;
                            iter->source = obj;
                            break;
                        case ObjectType::MAP:
                        case ObjectType::SET: {   // a set is a value-less map
                            iter->kind = IterObject::Kind::MAP_KEYS;
                            iter->source = obj; // for the pair form's value lookup
                            for (const auto& e : static_cast<MapObject*>(obj.get())->entries) {
                                iter->snapshot.push_back(e.first);
                            }
                            break;
                        }
                        default:
                            VM_THROW(makeError(
                                "for loops iterate over list, range, string or map; got " +
                                typeName(obj->type()), currentLine()));
                    }
                    sp_[-1] = Value::object(iter);   // replace the iterable with its iterator
                    VM_NEXT;
                }
                VM_CASE(FOR_NEXT) {
                    uint8_t flags = readByte();
                    uint16_t v1 = readU16();
                    uint16_t v2 = readU16();
                    uint16_t exitJump = readU16();
                    bool pair = flags & 2;
                    // Range iteration is the hot loop shape (for i in 0..n): all-scalar,
                    // in-place slot writes, direct dispatch.
                    {
                        IterObject* itp = static_cast<IterObject*>(peek().obj);
                        if (itp->kind == IterObject::Kind::RANGE) {
                            auto* r = static_cast<RangeObject*>(itp->source.get());
                            if (r->step > 0 ? itp->index >= r->end : itp->index <= r->end) {
                                ip += exitJump;
                                VM_NEXT_FAST;
                            }
                            long long cur = itp->index;
                            itp->index += r->step;
                            slots[v1] = Value::integer(cur);
                            if (pair) slots[v2] = Value::integer(cur);
                            VM_NEXT_FAST;
                        }
                    }
                    auto iter = refCast<IterObject>(peek().obj);

                    Value first, second;
                    bool done = false;
                    long long curIndex = iter->index;
                    switch (iter->kind) {
                        case IterObject::Kind::LIST: {
                            auto* list = static_cast<ListObject*>(iter->source.get());
                            if (iter->index >= (long long)list->elements.size()) done = true;
                            else { second = fromObject(list->elements[iter->index]);
                                   first = Value::integer(iter->index); iter->index++; }
                            break;
                        }
                        case IterObject::Kind::RANGE: {
                            auto* r = static_cast<RangeObject*>(iter->source.get());
                            if (r->step > 0 ? iter->index >= r->end : iter->index <= r->end) done = true;
                            else { second = Value::integer(iter->index);
                                   first = Value::integer(curIndex); iter->index += r->step; }
                            break;
                        }
                        case IterObject::Kind::STRING: {
                            const std::string& sv = static_cast<StringObject*>(iter->source.get())->value;
                            if (iter->bytePos >= sv.size()) done = true;
                            else {
                                int len = utf8CharLen((unsigned char)sv[iter->bytePos]);
                                second = Value::object(makeObj<StringObject>(sv.substr(iter->bytePos, len)));
                                first = Value::integer(iter->index);
                                iter->bytePos += len; iter->index++;
                            }
                            break;
                        }
                        case IterObject::Kind::MAP_KEYS: {
                            if (iter->index >= (long long)iter->snapshot.size()) done = true;
                            else {
                                auto key = iter->snapshot[iter->index];
                                first = fromObject(key);
                                if (pair) {
                                    // pair over a map yields key, value
                                    auto* mp = static_cast<MapObject*>(iter->source.get());
                                    auto val = mp ? mp->get(key) : nullptr;
                                    second = val ? fromObject(val) : Value::nil();
                                }
                                iter->index++;
                            }
                            break;
                        }
                    }

                    if (done) {
                        ip += exitJump;
                        VM_NEXT;
                    }
                    // Single form: the variable receives the element (map -> key).
                    Value primary = pair ? first
                        : (iter->kind == IterObject::Kind::MAP_KEYS ? first : second);
                    slots[v1] = primary;
                    if (pair) slots[v2] = second;
                    VM_NEXT;
                }
                VM_CASE(CLOSE_UPVALUE) {
                    uint16_t slot = readU16();
                    if (!openUpvalues_.empty())
                        closeUpvalues((int)(frame->base + 1 + slot));
                    VM_NEXT_FAST;
                }

                VM_CASE(USE) {
                    uint16_t specIdx = readU16();
                    syncOut();
                    const UseSpec& spec = frame->closure->proto->chunk.useSpecs[specIdx];
                    int line = currentLine();

                    auto module = loadModule(spec, line);
                    if (isError(module)) VM_THROW(module);
                    auto* map = static_cast<MapObject*>(module.get());

                    if (!spec.names.empty()) {
                        for (size_t i = 0; i < spec.names.size(); ++i) {
                            auto val = map->get(makeObj<StringObject>(spec.names[i]));
                            if (val == nullptr) {
                                VM_THROW(makeError(
                                    "'" + map->moduleName + "' module has no member '" +
                                    spec.names[i] + "'", line));
                            }
                            globals_[spec.nameSlots[i]] = fromObject(val);
                            globalDefined_[spec.nameSlots[i]] = 1;
                        }
                    } else {
                        if (spec.bindName.empty()) {
                            VM_THROW(makeError(
                                "could not derive a module name; give one with 'as': "
                                "use \"...\" as name", line));
                        }
                        globals_[spec.bindSlot] = Value::object(module);
                        globalDefined_[spec.bindSlot] = 1;
                    }
                    VM_NEXT;
                }

                VM_CASE(TRY_PUSH) {
                    uint16_t off = readU16();
                    handlers_.push_back({frames_.size(), stackSize(), ip + off});
                    VM_NEXT;
                }
                VM_CASE(TRY_POP)
                    if (!handlers_.empty()) handlers_.pop_back();
                    VM_NEXT;
                VM_CASE(THROW_) {
                    Value v = pop();
                    std::string msg = valueInspect(v);
                    auto err = makeError(msg, currentLine());
                    // Structured throw (RFC-022): maps/structs/lists/tuples are
                    // carried intact so catch can inspect e.kind etc.
                    if (v.kind == VKind::OBJ &&
                        (v.isObjType(ObjectType::MAP) || v.isObjType(ObjectType::STRUCT) ||
                         v.isObjType(ObjectType::LIST) || v.isObjType(ObjectType::TUPLE))) {
                        err->payload = toObject(v);
                    }
                    VM_THROW(err);
                }
                VM_CASE(COALESCE) {
                    uint16_t off = readU16();
                    if (!peek().isNil()) ip += off;
                    else pop();
                    VM_NEXT;
                }
                VM_CASE(RANGE_NEW) {
                    Value b = pop(), a = pop();
                    if (a.kind != VKind::INT || b.kind != VKind::INT) {
                        VM_THROW(makeError("range operator '..' expects two integers", currentLine()));
                    }
                    push(Value::object(makeObj<RangeObject>(a.i, b.i, 1)));
                    VM_NEXT;
                }
                VM_CASE(IS_TYPE) {
                    Value name = pop(), val = pop();
                    if (!name.isObjType(ObjectType::STRING)) {
                        VM_THROW(makeError("'is' expects a type name string on the right "
                                           "(e.g. x is \"int\")", currentLine()));
                    }
                    const std::string& want = static_cast<StringObject*>(name.obj)->value;
                    push(Value::boolean(valueTypeName(val) == want));
                    VM_NEXT;
                }
                VM_CASE(UNPACK) {
                    uint16_t n = readU16();
                    Value v = pop();
                    if (!v.isObjType(ObjectType::LIST) && !v.isObjType(ObjectType::TUPLE)) {
                        VM_THROW(makeError("unpacking assignment expects a list or tuple, got " +
                                           valueTypeName(v), currentLine()));
                    }
                    auto* list = static_cast<ListObject*>(v.obj);
                    if (list->elements.size() != n) {
                        VM_THROW(makeError("unpacking mismatch: " + std::to_string(n) +
                                           " target(s) but value has " +
                                           std::to_string(list->elements.size()), currentLine()));
                    }
                    for (uint16_t i = 0; i < n; ++i) push(fromObject(list->elements[i]));
                    VM_NEXT;
                }
                VM_CASE(SLICE) {
                    Value endV = pop(), startV = pop(), obj = pop();
                    auto res = sliceOp(toObject(obj), startV, endV, currentLine());
                    if (isError(res)) VM_THROW(res);
                    push(fromObject(res));
                    VM_NEXT;
                }
                VM_CASE(RUNTIME_ERROR) {
                    uint16_t msgC = readU16();
                    const std::string& msg =
                        static_cast<StringObject*>(constant(msgC).obj)->value;
                    VM_THROW(makeError(msg, currentLine()));
                }

                // ===== Fused superinstructions (v0.8 peephole) =====
                // IMM_ARITH: top op= i16 immediate, in place; generic fallback keeps
                // the exact error messages of the unfused op.
                #define IMM_ARITH(opStr, intStmt, dblStmt)                              \
                    int16_t k = (int16_t)readU16();                                     \
                    Value* pa = sp_ - 1;                                                \
                    if (pa->kind == VKind::INT)   { intStmt; VM_NEXT_FAST; }            \
                    if (pa->kind == VKind::FLOAT) { dblStmt; VM_NEXT_FAST; }            \
                    {                                                                   \
                        Value a = pop();                                                \
                        auto res = Runtime::evalInfixExpression(opStr, toObject(a),     \
                            makeObj<IntegerObject>(k), currentLine());         \
                        if (isError(res)) VM_THROW(res);                                \
                        push(fromObject(res));                                          \
                    }                                                                   \
                    VM_NEXT;

                VM_CASE(ADD_I) { IMM_ARITH("+", pa->i += k, pa->d += k) }
                VM_CASE(SUB_I) { IMM_ARITH("-", pa->i -= k, pa->d -= k) }
                VM_CASE(MUL_I) { IMM_ARITH("*", pa->i *= k, pa->d *= k) }
                VM_CASE(MOD_I) {
                    int16_t k = (int16_t)readU16();
                    Value* pa = sp_ - 1;
                    if (pa->kind == VKind::INT && k != 0) {
                        long long m = pa->i % k;
                        if (m != 0 && ((m < 0) != (k < 0))) m += k;
                        pa->i = m;
                        VM_NEXT_FAST;
                    }
                    {
                        Value a = pop();
                        auto res = Runtime::evalInfixExpression("%", toObject(a),
                            makeObj<IntegerObject>(k), currentLine());
                        if (isError(res)) VM_THROW(res);
                        push(fromObject(res));
                    }
                    VM_NEXT;
                }
                #define IMM_BITWISE(opStr, intStmt)                                     \
                    int16_t k = (int16_t)readU16();                                     \
                    Value* pa = sp_ - 1;                                                \
                    if (pa->kind == VKind::INT) { intStmt; VM_NEXT_FAST; }              \
                    {                                                                   \
                        Value a = pop();                                                \
                        auto res = Runtime::evalInfixExpression(opStr, toObject(a),     \
                            makeObj<IntegerObject>(k), currentLine());         \
                        if (isError(res)) VM_THROW(res);                                \
                        push(fromObject(res));                                          \
                    }                                                                   \
                    VM_NEXT;

                VM_CASE(BAND_I) { IMM_BITWISE("&", pa->i &= k) }
                VM_CASE(BOR_I)  { IMM_BITWISE("|", pa->i |= k) }
                VM_CASE(BXOR_I) { IMM_BITWISE("^", pa->i ^= k) }

                // CMP_JF: compare the top two values, pop both, jump when NOT true.
                // One dispatch instead of compare + JUMP_IF_FALSE + a bool roundtrip.
                #define CMP_JF(opStr, cmpInt, cmpDbl)                                   \
                    uint16_t d = readU16();                                             \
                    Value* pa = sp_ - 2; Value* pb = sp_ - 1;                           \
                    if (pa->kind == VKind::INT && pb->kind == VKind::INT) {             \
                        long long l = pa->i, r = pb->i; (void)l; (void)r;               \
                        sp_ -= 2; if (!(cmpInt)) ip += d; VM_NEXT_FAST;                 \
                    }                                                                   \
                    if (pa->isNumber() && pb->isNumber()) {                             \
                        double l = pa->asDouble(), r = pb->asDouble(); (void)l; (void)r;\
                        sp_ -= 2; if (!(cmpDbl)) ip += d; VM_NEXT_FAST;                 \
                    }                                                                   \
                    {                                                                   \
                        Value b = pop(), a = pop();                                     \
                        auto res = Runtime::evalInfixExpression(opStr, toObject(a),     \
                                                                toObject(b), currentLine());\
                        if (isError(res)) VM_THROW(res);                                \
                        if (!objectTruthy(res)) ip += d;                                \
                    }                                                                   \
                    VM_NEXT;

                VM_CASE(LESS_JF)       { CMP_JF("<",  l < r,  l < r)  }
                VM_CASE(LESS_EQ_JF)    { CMP_JF("<=", l <= r, l <= r) }
                VM_CASE(GREATER_JF)    { CMP_JF(">",  l > r,  l > r)  }
                VM_CASE(GREATER_EQ_JF) { CMP_JF(">=", l >= r, l >= r) }
                VM_CASE(EQUAL_JF) {
                    uint16_t d = readU16();
                    bool t = valueEquals(sp_[-2], sp_[-1]);
                    sp_[-1].obj = nullptr; sp_[-2].obj = nullptr; sp_ -= 2;
                    if (!t) ip += d;
                    VM_NEXT_FAST;
                }
                VM_CASE(NOT_EQUAL_JF) {
                    uint16_t d = readU16();
                    bool t = valueEquals(sp_[-2], sp_[-1]);
                    sp_[-1].obj = nullptr; sp_[-2].obj = nullptr; sp_ -= 2;
                    if (t) ip += d;
                    VM_NEXT_FAST;
                }

                VM_CASE(LGET2) {
                    uint16_t a = readU16(), b = readU16();
                    push(slots[a]); push(slots[b]);
                    VM_NEXT_FAST;
                }
                #define LGET_ARITH_I(opStr, intExpr, dblExpr)                           \
                    uint16_t sl = readU16(); int16_t k = (int16_t)readU16();            \
                    Value* v = &slots[sl];                                              \
                    if (v->kind == VKind::INT)   { push(intExpr); VM_NEXT_FAST; }       \
                    if (v->kind == VKind::FLOAT) { push(dblExpr); VM_NEXT_FAST; }       \
                    {                                                                   \
                        auto res = Runtime::evalInfixExpression(opStr, toObject(*v),    \
                            makeObj<IntegerObject>(k), currentLine());         \
                        if (isError(res)) VM_THROW(res);                                \
                        push(fromObject(res));                                          \
                    }                                                                   \
                    VM_NEXT;

                VM_CASE(LGET_ADD_I) { LGET_ARITH_I("+", Value::integer(v->i + k), Value::real(v->d + k)) }
                VM_CASE(LGET_SUB_I) { LGET_ARITH_I("-", Value::integer(v->i - k), Value::real(v->d - k)) }

                VM_CASE(ADD_INPLACE) {
                    Value* pa = sp_ - 2; Value* pb = sp_ - 1;
                    if (pa->kind == VKind::INT && pb->kind == VKind::INT) {
                        pa->i += pb->i; --sp_; VM_NEXT_FAST;
                    }
                    if (pa->isNumber() && pb->isNumber()) {
                        *pa = Value::real(pa->asDouble() + pb->asDouble()); --sp_; VM_NEXT_FAST;
                    }
                    if (pa->isObjType(ObjectType::STRING) && pb->isObjType(ObjectType::STRING)) {
                        // Under a tracing GC there is no cheap uniqueness check, so
                        // always build a fresh string (correct — never mutates a
                        // possibly-shared one). makeObj can't collect mid-instruction
                        // (deferred to the next safepoint), so ls/rs stay valid.
                        // TODO(v0.11-perf): restore in-place append with a builder flag.
                        auto* ls = static_cast<StringObject*>(pa->obj);
                        const std::string& rs = static_cast<StringObject*>(pb->obj)->value;
                        auto out = makeObj<StringObject>(std::string());
                        out->value.reserve(ls->value.size() + rs.size());
                        out->value.append(ls->value).append(rs);
                        pa->obj = out.get();
                        pb->obj = nullptr; --sp_;
                        VM_NEXT_FAST;
                    }
                    {
                        Value b = pop(), a = pop();
                        auto res = Runtime::evalInfixExpression("+", toObject(a),
                                                                toObject(b), currentLine());
                        if (isError(res)) VM_THROW(res);
                        push(fromObject(res));
                    }
                    VM_NEXT;
                }
                // Pop the top, compare against an i16 immediate, jump when NOT true.
                #define CMP_I_JF(opStr, cmpInt, cmpDbl)                                 \
                    int16_t k = (int16_t)readU16(); uint16_t d = readU16();             \
                    Value* pa = sp_ - 1;                                                \
                    if (pa->kind == VKind::INT) {                                       \
                        long long l = pa->i; (void)l;                                   \
                        --sp_; if (!(cmpInt)) ip += d; VM_NEXT_FAST;                    \
                    }                                                                   \
                    if (pa->kind == VKind::FLOAT) {                                     \
                        double l = pa->d; (void)l;                                      \
                        --sp_; if (!(cmpDbl)) ip += d; VM_NEXT_FAST;                    \
                    }                                                                   \
                    {                                                                   \
                        Value a = pop();                                                \
                        auto res = Runtime::evalInfixExpression(opStr, toObject(a),     \
                            makeObj<IntegerObject>(k), currentLine());         \
                        if (isError(res)) VM_THROW(res);                                \
                        if (!objectTruthy(res)) ip += d;                                \
                    }                                                                   \
                    VM_NEXT;

                VM_CASE(LT_I_JF) { CMP_I_JF("<",  l < k,  l < k)  }
                VM_CASE(LE_I_JF) { CMP_I_JF("<=", l <= k, l <= k) }
                VM_CASE(GT_I_JF) { CMP_I_JF(">",  l > k,  l > k)  }
                VM_CASE(GE_I_JF) { CMP_I_JF(">=", l >= k, l >= k) }
                VM_CASE(EQ_I_JF) { CMP_I_JF("==", l == k, l == k) }
                VM_CASE(NE_I_JF) { CMP_I_JF("!=", l != k, l != k) }

                VM_CASE(YIELD_) {
                    if (activeCoro_ == nullptr) {
                        VM_THROW(makeError("yield used outside a coroutine", currentLine()));
                    }
                    // Hand the value out and unwind run() back to resumeCoroutine;
                    // ip already points past this op, so resume continues here with
                    // the next resume() value pushed as this expression's result.
                    yieldValue_ = toObject(pop());
                    yielded_ = true;
                    syncOut();
                    return NULL_OBJ_;
                }
                VM_CASE(HALT)
                    frames_.pop_back();
                    sp_ = stackMem_.get();
                    return NULL_OBJ_;
            #ifndef LOVAX_CG
            }
#endif
        }
        #undef VM_CASE
        #undef VM_NEXT
        #undef VM_NEXT_FAST
        #undef VM_THROW
    }

    // ===== Assignment targets (same messages as the tree-walker) =====

    Ref<Object> indexSet(const Ref<Object>& obj,
                                     const Ref<Object>& idx,
                                     const Ref<Object>& val, int line) {
        if (obj->type() == ObjectType::LIST) {
            auto* list = static_cast<ListObject*>(obj.get());
            if (idx->type() != ObjectType::INTEGER) {
                return makeError("list index must be an integer, got " + typeName(idx->type()), line);
            }
            long long i = static_cast<IntegerObject*>(idx.get())->value;
            long long n = (long long)list->elements.size();
            if (i < 0) i += n;
            if (i < 0 || i >= n) {
                return makeError("list index out of range: " + idx->inspect() +
                                 " (length " + std::to_string(n) + ")", line);
            }
            gcShade(val.get());                          // write barrier (RFC-023)
            list->elements[i] = val;
            return nullptr;
        }
        if (obj->type() == ObjectType::MAP) {
            auto* map = static_cast<MapObject*>(obj.get());
            if (map->frozen) {
                return makeError("module '" + map->moduleName + "' cannot be modified (frozen)", line);
            }
            if (!Runtime::isValidMapKey(idx)) {
                return makeError("map keys must be string, int or bool; got " +
                                 typeName(idx->type()), line);
            }
            map->set(idx, val);
            return nullptr;
        }
        if (obj->type() == ObjectType::TUPLE) {
            return makeError("tuples are immutable — convert to a list to modify", line);
        }
        if (obj->type() == ObjectType::BYTES) {
            return makeError("bytes are immutable — build a new bytes value instead", line);
        }
        if (obj->type() == ObjectType::STRUCT) {
            auto* si = static_cast<StructInstanceObject*>(obj.get());
            if (idx->type() != ObjectType::STRING) {
                return makeError("struct fields are accessed by name (a string), got " +
                                 typeName(idx->type()), line);
            }
            const std::string& prop = static_cast<StringObject*>(idx.get())->value;
            auto it = si->shape->fieldIndex.find(prop);
            if (it == si->shape->fieldIndex.end()) {
                return makeError("struct '" + si->shape->name + "' has no field '" +
                                 prop + "' (fields are fixed at declaration)", line);
            }
            gcShade(val.get());                          // write barrier (RFC-023)
            si->slots[it->second] = val;
            return nullptr;
        }
        return makeError("indexed assignment only works on list and map, got " +
                         typeName(obj->type()), line);
    }

    // list[a:b] / string[a:b] with Python rules (negative indices, clamped, end-exclusive)
    Ref<Object> sliceOp(const Ref<Object>& obj,
                                    const Value& startV, const Value& endV, int line) {
        if (startV.kind != VKind::NIL && startV.kind != VKind::INT)
            return makeError("slice bounds must be integers", line);
        if (endV.kind != VKind::NIL && endV.kind != VKind::INT)
            return makeError("slice bounds must be integers", line);

        auto norm = [](long long idx, long long n) {
            if (idx < 0) idx += n;
            if (idx < 0) idx = 0;
            if (idx > n) idx = n;
            return idx;
        };

        if (obj->type() == ObjectType::LIST) {
            auto* list = static_cast<ListObject*>(obj.get());
            long long n = (long long)list->elements.size();
            long long a = startV.kind == VKind::INT ? norm(startV.i, n) : 0;
            long long b = endV.kind == VKind::INT ? norm(endV.i, n) : n;
            auto out = makeObj<ListObject>();
            for (long long i = a; i < b; ++i) out->elements.push_back(list->elements[i]);
            return out;
        }
        if (obj->type() == ObjectType::STRING) {
            const std::string& s = static_cast<StringObject*>(obj.get())->value;
            auto offs = utf8Offsets(s);
            long long n = (long long)offs.size() - 1;
            long long a = startV.kind == VKind::INT ? norm(startV.i, n) : 0;
            long long b = endV.kind == VKind::INT ? norm(endV.i, n) : n;
            if (a >= b) return makeObj<StringObject>("");
            return makeObj<StringObject>(s.substr(offs[a], offs[b] - offs[a]));
        }
        if (obj->type() == ObjectType::BYTES) {
            const std::string& d = static_cast<BytesObject*>(obj.get())->data;
            long long n = (long long)d.size();
            long long a = startV.kind == VKind::INT ? norm(startV.i, n) : 0;
            long long b = endV.kind == VKind::INT ? norm(endV.i, n) : n;
            if (a >= b) return makeObj<BytesObject>(std::string());
            return makeObj<BytesObject>(d.substr((size_t)a, (size_t)(b - a)));
        }
        return makeError("slicing only works on list and string, got " + typeName(obj->type()), line);
    }

    Ref<Object> memberSet(const Ref<Object>& obj, const std::string& prop,
                          const Ref<Object>& val, int line) {
        if (obj->type() == ObjectType::STRUCT) {
            auto* si = static_cast<StructInstanceObject*>(obj.get());
            auto it = si->shape->fieldIndex.find(prop);
            if (it == si->shape->fieldIndex.end()) {
                return makeError("struct '" + si->shape->name + "' has no field '" +
                                 prop + "' (fields are fixed at declaration)", line);
            }
            gcShade(val.get());                          // write barrier (RFC-023)
            si->slots[it->second] = val;
            return nullptr;
        }
        if (obj->type() != ObjectType::MAP) {
            return makeError("member assignment (object.field = ...) only works on maps, got " +
                             typeName(obj->type()), line);
        }
        auto* map = static_cast<MapObject*>(obj.get());
        if (map->frozen) {
            return makeError("module '" + map->moduleName + "' cannot be modified (frozen)", line);
        }
        // Allocate the key object only when the field is genuinely new.
        size_t pos = map->findStr(prop);
        gcShade(val.get());                              // write barrier (RFC-023)
        if (pos != MapObject::NPOS) map->entries[pos].second = val;
        else map->setStr(makeObj<StringObject>(prop), prop, val);
        return nullptr;
    }
};

} // namespace Lovax

#endif // VM_HPP
