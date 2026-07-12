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

// The Lume virtual machine: a stack-based bytecode interpreter with
// computed-goto dispatch, closed upvalues, and immediate numeric values.
// Object-typed operations reuse the exact runtime semantics (and error
// messages) of the retired tree-walker via runtime.hpp.

namespace Lume {

class VM {
public:
    static constexpr int MAX_FRAMES = 500;
    static constexpr size_t STACK_LIMIT = 1 << 16;

    VM() {
        stackMem_ = std::make_unique<Value[]>(STACK_LIMIT);
        sp_ = stackMem_.get();
        installBuiltinGlobals();
    }

    // Relative module paths resolve from the entry script's directory.
    static void setBaseDir(const std::string& dir) {
        baseDirs().clear();
        baseDirs().push_back(dir.empty() ? "." : dir);
    }

    // Compiles and runs a program. Returns NULL_OBJ_ or an ErrorObject.
    std::shared_ptr<Object> interpret(const Program* program) {
        Compiler compiler(globalsTable_);
        auto proto = compiler.compileProgram(program);
        syncGlobalSlots();

        auto closure = std::make_shared<ClosureObject>(proto);
        push(Value::object(closure));
        frames_.reserve(MAX_FRAMES);
        frames_.push_back({closure.get(), proto->chunk.code.data(), 0, 0});
        auto result = run(0);
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

    // Native -> Lume call bridge (builtins calling closures: filter/emit/sort_by).
    std::shared_ptr<Object> callFromNative(const std::shared_ptr<Object>& fn,
                                           const std::vector<std::shared_ptr<Object>>& args,
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
        return [self](const std::shared_ptr<Object>& f,
                      const std::vector<std::shared_ptr<Object>>& a,
                      int l) { return self->callFromNative(f, a, l); };
    }

    // ===== Module loading (mirrors the retired tree-walker loader) =====

    static std::vector<std::string>& baseDirs() {
        static std::vector<std::string> dirs = {"."};
        return dirs;
    }

    std::shared_ptr<Object> loadModule(const UseSpec& spec, int line) {
        if (!spec.isFile) {
            auto builtin = StdLib::getBuiltinModule(spec.target);
            if (builtin != nullptr) return builtin;
            // Installed package? lume_libs/<name>/<name>.lm or main.lm
            namespace fs = std::filesystem;
            fs::path root(baseDirs().front());
            fs::path candidates[2] = {
                root / "lume_libs" / spec.target / (spec.target + ".lm"),
                root / "lume_libs" / spec.target / "main.lm"
            };
            for (const auto& cand : candidates) {
                std::error_code ec;
                if (fs::exists(cand, ec)) {
                    return loadFileModule(fs::absolute(cand).string(), line);
                }
            }
            return makeError("unknown module '" + spec.target +
                             "' (built-ins: " + StdLib::builtinModuleList() +
                             "; no package at lume_libs/" + spec.target + "/" +
                             "; for a file module use quotes: use \"" + spec.target + ".lm\")",
                             line);
        }
        return loadFileModule(spec.target, line);
    }

    std::shared_ptr<Object> loadFileModule(const std::string& rawPath, int line) {
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
        loading.push_back(key);
        baseDirs().push_back(resolved.parent_path().string());
        VM moduleVM;
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
    std::shared_ptr<MapObject> exportGlobals() {
        std::vector<std::pair<std::string, Value>> items;
        for (size_t i = 0; i < globalsTable_.names.size(); ++i) {
            if (!globalDefined_[i]) continue;
            const std::string& name = globalsTable_.names[i];
            if (builtinNames().count(name)) {
                // Skip untouched core builtins; keep user redefinitions.
                if (globals_[i].isObj() &&
                    globals_[i].obj->type() == ObjectType::BUILTIN &&
                    static_cast<BuiltinObject*>(globals_[i].obj.get())->name == name) {
                    continue;
                }
            }
            items.push_back({name, globals_[i]});
        }
        std::sort(items.begin(), items.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        auto mod = std::make_shared<MapObject>();
        for (auto& [name, val] : items) {
            mod->set(std::make_shared<StringObject>(name), toObject(val));
        }
        mod->frozen = true;
        return mod;
    }

private:
    struct Frame {
        // Raw pointer: the callee Value at stack_[base] keeps the closure alive
        // until RETURN truncates the stack, so no refcount traffic per call.
        ClosureObject* closure;
        const uint8_t* ip;
        size_t base;    // stack index of the callee value; locals start at base+1
        int nargs;
    };

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

    // Active try handlers (RFC-008): where to jump when a value is thrown.
    struct Handler {
        size_t frameDepth;   // frames_.size() at the try
        size_t stackTop;     // sp_ offset at the try
        const uint8_t* catchIp;
    };
    std::vector<Handler> handlers_;

    static std::unordered_map<std::string, std::shared_ptr<MapObject>>& moduleCache() {
        static std::unordered_map<std::string, std::shared_ptr<MapObject>> c;
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
            return m;
        }();
        return names;
    }

    void installBuiltinGlobals() {
        auto env = std::make_shared<Environment>();
        Builtins::installBuiltins(env);
        for (const auto& [name, obj] : env->entries()) {
            uint16_t slot = globalsTable_.slot(name);
            if (slot >= globals_.size()) {
                globals_.resize(slot + 1);
                globalDefined_.resize(slot + 1, 0);
            }
            globals_[slot] = Value::object(obj);
            globalDefined_[slot] = 1;
        }
    }

    // The compiler may have registered new global names; size the tables to match.
    void syncGlobalSlots() {
        if (globals_.size() < globalsTable_.names.size()) {
            globals_.resize(globalsTable_.names.size());
            globalDefined_.resize(globalsTable_.names.size(), 0);
        }
    }

    void push(Value v) { *sp_++ = std::move(v); }
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
                openUpvalues_[i]->closed = true;
                openUpvalues_.erase(openUpvalues_.begin() + i);
            } else {
                ++i;
            }
        }
    }

    // ===== Calls =====

    // Returns nullptr on success, or an ErrorObject.
    std::shared_ptr<Object> callValue(int argc, int line) {
        Value& callee = peek(argc);

        if (callee.isObjType(ObjectType::FUNCTION)) {
            auto* closure = static_cast<ClosureObject*>(callee.obj.get());
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
                auto rest = std::make_shared<ListObject>();
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
                               stackSize() - proto.localCount - 1, argc});
            return nullptr;
        }

        if (callee.isObjType(ObjectType::BUILTIN)) {
            auto builtin = std::static_pointer_cast<BuiltinObject>(callee.obj);
            std::vector<std::shared_ptr<Object>> args;
            args.reserve(argc);
            for (int i = argc - 1; i >= 0; --i) args.push_back(toObject(peek(i)));
            auto result = builtin->fn(args, line, callFn());
            if (isError(result)) return result;
            sp_ -= argc + 1;
            push(fromObject(result));
            return std::shared_ptr<Object>(nullptr); // handled fully
        }

        return makeError("not a function, cannot be called: " + valueTypeName(callee), line);
    }

    // ===== The run loop =====

    std::shared_ptr<Object> run(size_t exitFrameDepth) {
        Frame* frame = &frames_.back();
        const uint8_t* ip = frame->ip;
        const Value* consts = frame->closure->proto->chunk.consts.data();
        Value* slots = stackAt(frame->base + 1);

        // Keep the hot state in locals; resync whenever the frame changes or the
        // stack may reallocate (calls can grow it and invalidate 'slots').
        auto syncOut = [&]() { frame->ip = ip; };
        auto syncIn = [&]() {
            frame = &frames_.back();
            ip = frame->ip;
            consts = frame->closure->proto->chunk.consts.data();
            slots = stackAt(frame->base + 1);
        };

        auto readByte = [&]() -> uint8_t { return *ip++; };
        auto readU16 = [&]() -> uint16_t {
            uint16_t v = (uint16_t)((ip[0] << 8) | ip[1]);
            ip += 2;
            return v;
        };
        auto currentLine = [&]() -> int {
            const Chunk& c = frame->closure->proto->chunk;
            size_t offset = (size_t)(ip - c.code.data()) - 1;
            if (offset >= c.lines.size()) offset = c.lines.size() - 1;
            return c.lines[offset];
        };
        auto constant = [&](uint16_t idx) -> const Value& { return consts[idx]; };

        // Unwinds everything and reports; the first error is terminal.
        auto runtimeError = [&](const std::shared_ptr<Object>& err) -> std::shared_ptr<Object> {
            frames_.resize(exitFrameDepth);
            return err;
        };

        // Error dispatch (RFC-008): if a try handler is active within this run's
        // frame window, unwind to it, bind the error message, and resume in catch.
        auto tryHandle = [&](const std::shared_ptr<Object>& err) -> bool {
            if (handlers_.empty() || handlers_.back().frameDepth < exitFrameDepth) return false;
            Handler h = handlers_.back();
            handlers_.pop_back();
            while (frames_.size() > h.frameDepth) {
                if (!openUpvalues_.empty()) closeUpvalues((int)frames_.back().base);
                frames_.pop_back();
            }
            sp_ = stackMem_.get() + h.stackTop;
            std::string msg = isError(err) ? static_cast<ErrorObject*>(err.get())->message
                                           : valueInspect(fromObject(err));
            push(Value::object(std::make_shared<StringObject>(msg)));
            frame = &frames_.back();
            ip = h.catchIp;
            consts = frame->closure->proto->chunk.consts.data();
            slots = stackAt(frame->base + 1);
            return true;
        };
        // Raises err: jump to the nearest handler, or unwind and return it.
        #define VM_THROW(E) { std::shared_ptr<Object> _e = (E); if (tryHandle(_e)) continue; frames_.resize(exitFrameDepth); return _e; }

        for (;;) {
            Op op = (Op)readByte();
            switch (op) {
                case Op::CONST: push(constant(readU16())); break;
                case Op::NIL:    push(Value::nil()); break;
                case Op::TRUE_:  push(Value::boolean(true)); break;
                case Op::FALSE_: push(Value::boolean(false)); break;
                case Op::POP:    pop(); break;
                case Op::DUP:    push(peek()); break;

                case Op::GET_LOCAL: push(slots[readU16()]); break;
                case Op::SET_LOCAL: slots[readU16()] = pop(); break;

                case Op::GET_GLOBAL: {
                    uint16_t slot = readU16();
                    if (!globalDefined_[slot]) {
                        const std::string& name = globalsTable_.names[slot];
                        VM_THROW(makeError(
                            "undefined variable '" + name +
                            "' (define it with: set " + name + " = ...)", currentLine()));
                    }
                    push(globals_[slot]);
                    break;
                }
                case Op::DEFINE_GLOBAL: {
                    uint16_t slot = readU16();
                    globals_[slot] = pop();
                    globalDefined_[slot] = 1;
                    break;
                }
                case Op::SET_GLOBAL: {
                    uint16_t slot = readU16();
                    if (!globalDefined_[slot]) {
                        const std::string& name = globalsTable_.names[slot];
                        VM_THROW(makeError(
                            "undefined variable '" + name +
                            "' (define it with: set " + name + " = ...)", currentLine()));
                    }
                    globals_[slot] = pop();
                    break;
                }
                case Op::GET_UPVALUE: {
                    auto& cell = frame->closure->upvalues[readU16()];
                    push(cell->closed ? cell->value : *stackAt(cell->stackSlot));
                    break;
                }
                case Op::SET_UPVALUE: {
                    auto& cell = frame->closure->upvalues[readU16()];
                    if (cell->closed) cell->value = pop();
                    else *stackAt(cell->stackSlot) = pop();
                    break;
                }

                case Op::EQUAL: {
                    Value b = pop(), a = pop();
                    push(Value::boolean(valueEquals(a, b)));
                    break;
                }
                case Op::NOT_EQUAL: {
                    Value b = pop(), a = pop();
                    push(Value::boolean(!valueEquals(a, b)));
                    break;
                }

                #define NUMERIC_FAST(opChar, intExpr, dblExpr)                              \
                    Value b = pop(), a = pop();                                             \
                    if (a.kind == VKind::INT && b.kind == VKind::INT) {                     \
                        long long l = a.i, r = b.i; (void)l; (void)r;                       \
                        push(intExpr);                                                      \
                    } else if (a.isNumber() && b.isNumber()) {                              \
                        double l = a.asDouble(), r = b.asDouble(); (void)l; (void)r;        \
                        push(dblExpr);                                                      \
                    } else {                                                                \
                        auto res = Runtime::evalInfixExpression(opChar, toObject(a),        \
                                                                toObject(b), currentLine());\
                        if (isError(res)) VM_THROW(res);                         \
                        push(fromObject(res));                                              \
                    }

                case Op::ADD: { NUMERIC_FAST("+", Value::integer(l + r), Value::real(l + r)); break; }
                case Op::SUB: { NUMERIC_FAST("-", Value::integer(l - r), Value::real(l - r)); break; }
                case Op::MUL: { NUMERIC_FAST("*", Value::integer(l * r), Value::real(l * r)); break; }
                case Op::DIV: {
                    Value b = pop(), a = pop();
                    if (a.kind == VKind::INT && b.kind == VKind::INT) {
                        if (b.i == 0) VM_THROW(makeError("division by zero", currentLine()));
                        long long q = a.i / b.i;
                        if ((a.i % b.i != 0) && ((a.i < 0) != (b.i < 0))) q--;
                        push(Value::integer(q));
                    } else if (a.isNumber() && b.isNumber()) {
                        double r = b.asDouble();
                        if (r == 0.0) VM_THROW(makeError("division by zero", currentLine()));
                        push(Value::real(a.asDouble() / r));
                    } else {
                        auto res = Runtime::evalInfixExpression("/", toObject(a), toObject(b), currentLine());
                        if (isError(res)) VM_THROW(res);
                        push(fromObject(res));
                    }
                    break;
                }
                case Op::MOD: {
                    Value b = pop(), a = pop();
                    if (a.kind == VKind::INT && b.kind == VKind::INT) {
                        if (b.i == 0) VM_THROW(makeError("modulo by zero", currentLine()));
                        long long m = a.i % b.i;
                        if (m != 0 && ((m < 0) != (b.i < 0))) m += b.i;
                        push(Value::integer(m));
                    } else if (a.isNumber() && b.isNumber()) {
                        double r = b.asDouble();
                        if (r == 0.0) VM_THROW(makeError("modulo by zero", currentLine()));
                        double m = std::fmod(a.asDouble(), r);
                        if (m != 0.0 && ((m < 0.0) != (r < 0.0))) m += r;
                        push(Value::real(m));
                    } else {
                        auto res = Runtime::evalInfixExpression("%", toObject(a), toObject(b), currentLine());
                        if (isError(res)) VM_THROW(res);
                        push(fromObject(res));
                    }
                    break;
                }
                case Op::POW: {
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
                    break;
                }
                case Op::LESS:       { NUMERIC_FAST("<",  Value::boolean(l < r),  Value::boolean(l < r));  break; }
                case Op::GREATER:    { NUMERIC_FAST(">",  Value::boolean(l > r),  Value::boolean(l > r));  break; }
                case Op::LESS_EQ:    { NUMERIC_FAST("<=", Value::boolean(l <= r), Value::boolean(l <= r)); break; }
                case Op::GREATER_EQ: { NUMERIC_FAST(">=", Value::boolean(l >= r), Value::boolean(l >= r)); break; }

                #define INT_ONLY_OP(opStr, expr)                                            \
                    Value b = pop(), a = pop();                                             \
                    if (a.kind == VKind::INT && b.kind == VKind::INT) {                     \
                        long long l = a.i, r = b.i; (void)l; (void)r;                       \
                        push(expr);                                                         \
                    } else {                                                                \
                        auto res = Runtime::evalInfixExpression(opStr, toObject(a),         \
                                                                toObject(b), currentLine());\
                        if (isError(res)) VM_THROW(res);                         \
                        push(fromObject(res));                                              \
                    }

                case Op::BIT_AND: { INT_ONLY_OP("&", Value::integer(l & r)); break; }
                case Op::BIT_OR:  { INT_ONLY_OP("|", Value::integer(l | r)); break; }
                case Op::BIT_XOR: { INT_ONLY_OP("^", Value::integer(l ^ r)); break; }
                case Op::SHL: case Op::SHR: {
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
                    break;
                }
                case Op::IN: {
                    Value b = pop(), a = pop();
                    auto res = Runtime::evalInfixExpression("in", toObject(a), toObject(b), currentLine());
                    if (isError(res)) VM_THROW(res);
                    push(fromObject(res));
                    break;
                }

                case Op::NEGATE: {
                    Value a = pop();
                    if (a.kind == VKind::INT)        push(Value::integer(-a.i));
                    else if (a.kind == VKind::FLOAT) push(Value::real(-a.d));
                    else {
                        VM_THROW(makeError(
                            "unary '-' only works on numbers, got " + valueTypeName(a),
                            currentLine()));
                    }
                    break;
                }
                case Op::NOT_: {
                    Value a = pop();
                    push(Value::boolean(!valueTruthy(a)));
                    break;
                }
                case Op::BIT_NOT: {
                    Value a = pop();
                    if (a.kind == VKind::INT) push(Value::integer(~a.i));
                    else {
                        VM_THROW(makeError(
                            "'~' only works on integers, got " + valueTypeName(a),
                            currentLine()));
                    }
                    break;
                }

                case Op::JUMP: { uint16_t d = readU16(); ip += d; break; }
                case Op::JUMP_IF_FALSE: {
                    uint16_t d = readU16();
                    if (!valueTruthy(pop())) ip += d;
                    break;
                }
                case Op::LOOP: { uint16_t d = readU16(); ip -= d; break; }
                case Op::AND_KEEP: {
                    uint16_t d = readU16();
                    if (!valueTruthy(peek())) ip += d;
                    else pop();
                    break;
                }
                case Op::OR_KEEP: {
                    uint16_t d = readU16();
                    if (valueTruthy(peek())) ip += d;
                    else pop();
                    break;
                }

                case Op::CALL: {
                    int argc = readByte();
                    int line = currentLine();
                    syncOut();
                    auto err = callValue(argc, line);
                    if (err != nullptr && isError(err)) VM_THROW(err);
                    syncIn();
                    break;
                }
                case Op::CALL_METHOD: {
                    int argc = readByte();
                    int line = currentLine();
                    // Stack: [recv, method, a1..aN]. Pass recv as 'this' only when it
                    // is a struct instance (a map tagged with __type__); otherwise call
                    // plainly (module/plain-map field call keeps its original arity).
                    static const std::shared_ptr<StringObject> typeKey =
                        std::make_shared<StringObject>("__type__");
                    Value recv = peek(argc + 1);
                    bool isStruct = recv.isObjType(ObjectType::MAP) &&
                        static_cast<MapObject*>(recv.obj.get())->get(typeKey) != nullptr;
                    syncOut();
                    std::shared_ptr<Object> err;
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
                    syncIn();
                    break;
                }
                case Op::CLOSURE: {
                    uint16_t protoIdx = readU16();
                    auto holder = std::static_pointer_cast<ProtoObject>(constant(protoIdx).obj);
                    auto closure = std::make_shared<ClosureObject>(holder->proto);
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
                    break;
                }
                case Op::ARG_DEFAULT: {
                    uint16_t paramSlot = readU16();
                    uint16_t skip = readU16();
                    if (frame->nargs > (int)paramSlot) ip += skip;
                    break;
                }
                case Op::RETURN: {
                    Value result = pop();
                    if (!openUpvalues_.empty()) closeUpvalues((int)frame->base);
                    sp_ = stackAt(frame->base);
                    frames_.pop_back();
                    push(std::move(result));
                    if (frames_.size() == exitFrameDepth) return NULL_OBJ_;
                    syncIn();
                    break;
                }

                case Op::LIST: {
                    uint16_t n = readU16();
                    auto list = std::make_shared<ListObject>();
                    list->elements.reserve(n);
                    for (int i = n - 1; i >= 0; --i) list->elements.push_back(toObject(peek(i)));
                    sp_ -= n;
                    push(Value::object(list));
                    break;
                }
                case Op::MAP: {
                    uint16_t n = readU16();
                    auto map = std::make_shared<MapObject>();
                    int line = currentLine();
                    for (int i = 0; i < n; ++i) {
                        auto key = toObject(peek((n - i) * 2 - 1));
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
                    break;
                }

                case Op::INDEX_GET: {
                    Value idx = pop(), obj = pop();
                    if (obj.isObjType(ObjectType::LIST) && idx.kind == VKind::INT) {
                        auto* list = static_cast<ListObject*>(obj.obj.get());
                        long long i = idx.i;
                        long long n = (long long)list->elements.size();
                        if (i < 0) i += n;
                        if (i >= 0 && i < n) {
                            push(fromObject(list->elements[i]));
                            break;
                        }
                    }
                    auto res = Runtime::evalIndexAccess(toObject(obj), toObject(idx), currentLine());
                    if (isError(res)) VM_THROW(res);
                    push(fromObject(res));
                    break;
                }
                case Op::INDEX_GET_KEEP: {
                    Value& obj = peek(1);
                    Value& idx = peek(0);
                    if (obj.isObjType(ObjectType::LIST) && idx.kind == VKind::INT) {
                        auto* list = static_cast<ListObject*>(obj.obj.get());
                        long long i = idx.i;
                        long long n = (long long)list->elements.size();
                        if (i < 0) i += n;
                        if (i >= 0 && i < n) {
                            push(fromObject(list->elements[i]));
                            break;
                        }
                    }
                    auto res = Runtime::evalIndexAccess(toObject(peek(1)), toObject(peek(0)),
                                                        currentLine());
                    if (isError(res)) VM_THROW(res);
                    push(fromObject(res));
                    break;
                }
                case Op::INDEX_SET: {
                    Value val = pop(), idx = pop(), obj = pop();
                    if (obj.isObjType(ObjectType::LIST) && idx.kind == VKind::INT) {
                        auto* list = static_cast<ListObject*>(obj.obj.get());
                        long long i = idx.i;
                        long long n = (long long)list->elements.size();
                        if (i < 0) i += n;
                        if (i >= 0 && i < n) {
                            list->elements[i] = toObject(val);
                            break;
                        }
                    }
                    auto err = indexSet(toObject(obj), toObject(idx), toObject(val), currentLine());
                    if (err != nullptr) VM_THROW(err);
                    break;
                }
                case Op::MEMBER_GET: {
                    uint16_t nameC = readU16();
                    Value obj = pop();
                    const std::string& prop =
                        static_cast<StringObject*>(constant(nameC).obj.get())->value;
                    auto res = Runtime::evalMemberAccess(toObject(obj), prop, currentLine());
                    if (isError(res)) VM_THROW(res);
                    push(fromObject(res));
                    break;
                }
                case Op::MEMBER_GET_SAFE: {
                    uint16_t nameC = readU16();
                    Value obj = pop();
                    if (obj.isNil()) { push(Value::nil()); break; } // a?.b -> null
                    const std::string& prop =
                        static_cast<StringObject*>(constant(nameC).obj.get())->value;
                    auto res = Runtime::evalMemberAccess(toObject(obj), prop, currentLine());
                    if (isError(res)) VM_THROW(res);
                    push(fromObject(res));
                    break;
                }
                case Op::MEMBER_GET_KEEP: {
                    uint16_t nameC = readU16();
                    const std::string& prop =
                        static_cast<StringObject*>(constant(nameC).obj.get())->value;
                    auto res = Runtime::evalMemberAccess(toObject(peek(0)), prop, currentLine());
                    if (isError(res)) VM_THROW(res);
                    push(fromObject(res));
                    break;
                }
                case Op::MEMBER_SET: {
                    uint16_t nameC = readU16();
                    Value val = pop(), obj = pop();
                    const std::string& prop =
                        static_cast<StringObject*>(constant(nameC).obj.get())->value;
                    auto err = memberSet(toObject(obj), prop, toObject(val), currentLine());
                    if (err != nullptr) VM_THROW(err);
                    break;
                }

                case Op::INTERP: {
                    uint16_t n = readU16();
                    std::string out;
                    for (int i = n - 1; i >= 0; --i) out += valueInspect(peek(i));
                    sp_ -= n;
                    push(Value::object(std::make_shared<StringObject>(out)));
                    break;
                }
                case Op::SAY: {
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
                    break;
                }

                case Op::FOR_SETUP: {
                    Value iterable = pop();
                    auto iter = std::make_shared<IterObject>();
                    auto obj = toObject(iterable);
                    switch (obj->type()) {
                        case ObjectType::LIST:
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
                        case ObjectType::MAP: {
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
                    push(Value::object(iter));
                    break;
                }
                case Op::FOR_NEXT: {
                    uint8_t flags = readByte();
                    uint16_t v1 = readU16();
                    uint16_t v2 = readU16();
                    uint16_t exitJump = readU16();
                    bool isGlobal = flags & 1;
                    bool pair = flags & 2;
                    auto iter = std::static_pointer_cast<IterObject>(peek().obj);

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
                                second = Value::object(std::make_shared<StringObject>(sv.substr(iter->bytePos, len)));
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
                        break;
                    }
                    // Single form: the variable receives the element (map -> key).
                    Value primary = pair ? first
                        : (iter->kind == IterObject::Kind::MAP_KEYS ? first : second);
                    auto store = [&](uint16_t slot, const Value& val) {
                        if (isGlobal) { globals_[slot] = val; globalDefined_[slot] = 1; }
                        else slots[slot] = val;
                    };
                    store(v1, primary);
                    if (pair) store(v2, second);
                    break;
                }

                case Op::USE: {
                    uint16_t specIdx = readU16();
                    syncOut();
                    const UseSpec& spec = frame->closure->proto->chunk.useSpecs[specIdx];
                    int line = currentLine();

                    auto module = loadModule(spec, line);
                    if (isError(module)) VM_THROW(module);
                    auto* map = static_cast<MapObject*>(module.get());

                    if (!spec.names.empty()) {
                        for (size_t i = 0; i < spec.names.size(); ++i) {
                            auto val = map->get(std::make_shared<StringObject>(spec.names[i]));
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
                    break;
                }

                case Op::TRY_PUSH: {
                    uint16_t off = readU16();
                    handlers_.push_back({frames_.size(), stackSize(), ip + off});
                    break;
                }
                case Op::TRY_POP:
                    if (!handlers_.empty()) handlers_.pop_back();
                    break;
                case Op::THROW_: {
                    Value v = pop();
                    std::string msg = valueInspect(v);
                    VM_THROW(makeError(msg, currentLine()));
                }
                case Op::COALESCE: {
                    uint16_t off = readU16();
                    if (!peek().isNil()) ip += off;
                    else pop();
                    break;
                }
                case Op::RANGE_NEW: {
                    Value b = pop(), a = pop();
                    if (a.kind != VKind::INT || b.kind != VKind::INT) {
                        VM_THROW(makeError("range operator '..' expects two integers", currentLine()));
                    }
                    push(Value::object(std::make_shared<RangeObject>(a.i, b.i, 1)));
                    break;
                }
                case Op::IS_TYPE: {
                    Value name = pop(), val = pop();
                    if (!name.isObjType(ObjectType::STRING)) {
                        VM_THROW(makeError("'is' expects a type name string on the right "
                                           "(e.g. x is \"int\")", currentLine()));
                    }
                    const std::string& want = static_cast<StringObject*>(name.obj.get())->value;
                    push(Value::boolean(valueTypeName(val) == want));
                    break;
                }
                case Op::UNPACK: {
                    uint16_t n = readU16();
                    Value v = pop();
                    if (!v.isObjType(ObjectType::LIST)) {
                        VM_THROW(makeError("unpacking assignment expects a list, got " +
                                           valueTypeName(v), currentLine()));
                    }
                    auto* list = static_cast<ListObject*>(v.obj.get());
                    if (list->elements.size() != n) {
                        VM_THROW(makeError("unpacking mismatch: " + std::to_string(n) +
                                           " target(s) but list has " +
                                           std::to_string(list->elements.size()), currentLine()));
                    }
                    for (uint16_t i = 0; i < n; ++i) push(fromObject(list->elements[i]));
                    break;
                }
                case Op::SLICE: {
                    Value endV = pop(), startV = pop(), obj = pop();
                    auto res = sliceOp(toObject(obj), startV, endV, currentLine());
                    if (isError(res)) VM_THROW(res);
                    push(fromObject(res));
                    break;
                }
                case Op::RUNTIME_ERROR: {
                    uint16_t msgC = readU16();
                    const std::string& msg =
                        static_cast<StringObject*>(constant(msgC).obj.get())->value;
                    VM_THROW(makeError(msg, currentLine()));
                }

                case Op::HALT:
                    frames_.pop_back();
                    sp_ = stackMem_.get();
                    return NULL_OBJ_;
            }
        }
        #undef VM_THROW
    }

    // ===== Assignment targets (same messages as the tree-walker) =====

    std::shared_ptr<Object> indexSet(const std::shared_ptr<Object>& obj,
                                     const std::shared_ptr<Object>& idx,
                                     const std::shared_ptr<Object>& val, int line) {
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
        return makeError("indexed assignment only works on list and map, got " +
                         typeName(obj->type()), line);
    }

    // list[a:b] / string[a:b] with Python rules (negative indices, clamped, end-exclusive)
    std::shared_ptr<Object> sliceOp(const std::shared_ptr<Object>& obj,
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
            auto out = std::make_shared<ListObject>();
            for (long long i = a; i < b; ++i) out->elements.push_back(list->elements[i]);
            return out;
        }
        if (obj->type() == ObjectType::STRING) {
            const std::string& s = static_cast<StringObject*>(obj.get())->value;
            auto offs = utf8Offsets(s);
            long long n = (long long)offs.size() - 1;
            long long a = startV.kind == VKind::INT ? norm(startV.i, n) : 0;
            long long b = endV.kind == VKind::INT ? norm(endV.i, n) : n;
            if (a >= b) return std::make_shared<StringObject>("");
            return std::make_shared<StringObject>(s.substr(offs[a], offs[b] - offs[a]));
        }
        return makeError("slicing only works on list and string, got " + typeName(obj->type()), line);
    }

    std::shared_ptr<Object> memberSet(const std::shared_ptr<Object>& obj,
                                      const std::string& prop,
                                      const std::shared_ptr<Object>& val, int line) {
        if (obj->type() != ObjectType::MAP) {
            return makeError("member assignment (object.field = ...) only works on maps, got " +
                             typeName(obj->type()), line);
        }
        auto* map = static_cast<MapObject*>(obj.get());
        if (map->frozen) {
            return makeError("module '" + map->moduleName + "' cannot be modified (frozen)", line);
        }
        map->set(std::make_shared<StringObject>(prop), val);
        return nullptr;
    }
};

} // namespace Lume

#endif // VM_HPP
