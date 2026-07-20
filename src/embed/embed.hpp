#ifndef LOVAX_EMBED_HPP
#define LOVAX_EMBED_HPP

// C++ embedding facade (RFC-025). A host application (a future game engine)
// drives a script with this: load code, register native functions/modules,
// call script functions each frame, read/write globals. A thin layer over the
// existing VM public API — no core behavior changes.
//
//   Lovax::Embed::Host rt;
//   rt.native("log", ...);
//   rt.loadFile("game.lov");
//   rt.call("on_frame", { Embed::box(dt) });   // host -> script
//
// Error model is return-value based (no C++ exception crosses the host
// boundary; the only one, CompileError, is caught inside VM::interpret).

#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <utility>
#include <algorithm>
#include "../lexer/lexer.hpp"
#include "../parser/parser.hpp"
#include "../vm/vm.hpp"
#include "marshal.hpp"

namespace Lovax {
namespace Embed {

// A script value the host keeps between frames. The tracing GC scans VM
// stacks/globals + permanent/temp roots; a value living only in the engine's
// own C++ state is invisible to it without this. RAII: rooted on construction,
// unrooted on destruction.
class Persistent {
public:
    Persistent() = default;
    explicit Persistent(const Ref<Object>& o) : obj_(o) {
        if (obj_) Heap::get().hostRoots.push_back(obj_.get());
    }
    ~Persistent() { release(); }
    Persistent(const Persistent&) = delete;
    Persistent& operator=(const Persistent&) = delete;
    Persistent(Persistent&& o) noexcept : obj_(o.obj_) { o.obj_.reset(); }
    Persistent& operator=(Persistent&& o) noexcept {
        if (this != &o) { release(); obj_ = o.obj_; o.obj_.reset(); }
        return *this;
    }
    const Ref<Object>& get() const { return obj_; }
    explicit operator bool() const { return (bool)obj_; }
    void release() {
        if (obj_) {
            auto& hr = Heap::get().hostRoots;
            hr.erase(std::remove(hr.begin(), hr.end(), obj_.get()), hr.end());
            obj_.reset();
        }
    }
private:
    Ref<Object> obj_;
};

class Host {
public:
    Host() { VM::setBaseDir("."); }

    VM& vm() { return vm_; }

    // ---- registration (before load) ----
    void native(const std::string& name, BuiltinObject::BuiltinFn fn) {
        vm_.native(name, std::move(fn));
    }
    void nativeModule(const std::string& name,
                      std::initializer_list<
                          std::pair<std::string, BuiltinObject::BuiltinFn>> fns) {
        vm_.nativeModule(name, fns);
    }

    // ---- load + run script source (top level) ----
    bool loadSource(const std::string& src, const std::string& /*name*/ = "<embed>") {
        Lexer lexer(src);
        Parser parser(lexer);
        auto program = parser.parseProgram();
        if (!parser.errors().empty()) {
            std::string msg = "syntax error";
            if (!parser.errors().empty()) msg = parser.errors()[0].toString();
            setError(makeError(msg));
            return false;
        }
        auto r = vm_.interpret(program.get());
        if (isError(r)) { setError(r); return false; }
        clearError();
        return true;
    }
    bool loadFile(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) { setError(makeError("cannot open file: " + path)); return false; }
        std::stringstream ss; ss << f.rdbuf();
        VM::setBaseDir(std::string(path).substr(0, path.find_last_of("/\\") + 1).empty()
                       ? "." : path.substr(0, path.find_last_of("/\\")));
        return loadSource(ss.str(), path);
    }

    // ---- host -> script call ----
    // Invoke a script global function by name. Returns its result, or nullptr
    // with lastError() set (unknown name / not a function / runtime error).
    Ref<Object> call(const std::string& fnName,
                     const std::vector<Ref<Object>>& args = {}) {
        auto fn = vm_.getGlobal(fnName);
        if (!fn) { setError(makeError("no such global function: " + fnName)); return nullptr; }
        if (fn->type() != ObjectType::FUNCTION && fn->type() != ObjectType::BUILTIN) {
            setError(makeError("global '" + fnName + "' is not callable")); return nullptr;
        }
        auto r = vm_.callFromNative(fn, args, 0);
        if (isError(r)) { setError(r); return nullptr; }
        clearError();
        return r;
    }

    // ---- globals ----
    Ref<Object> getGlobal(const std::string& name) { return vm_.getGlobal(name); }
    void setGlobal(const std::string& name, const Ref<Object>& v) { vm_.setGlobal(name, v); }

    // Last error object (nullptr if the last op succeeded). inspect() for text.
    // NOTE: values returned by call()/getGlobal() are NOT rooted — use them
    // immediately or wrap in a Persistent before code that can allocate/GC.
    Ref<Object> lastError() const { return lastErr_.get(); }

private:
    void setError(const Ref<Object>& e) { lastErr_ = Persistent(e); }  // rooted while held
    void clearError() { lastErr_.release(); }

    VM vm_;
    Persistent lastErr_;
};

} // namespace Embed
} // namespace Lovax

#endif // LOVAX_EMBED_HPP
