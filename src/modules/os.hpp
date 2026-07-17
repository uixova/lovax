#ifndef LOVAX_MODULE_OS_HPP
#define LOVAX_MODULE_OS_HPP

#include "common.hpp"
#ifndef _WIN32
  #include <sys/wait.h>
#endif

namespace Lovax {
namespace StdLib {

// ===== os module =====
// System access: environment variables, platform, working directory, script args.
// Follows Python's os/sys spirit, trimmed to what game tooling actually needs.
// Deliberately NO process execution (exec/system) in the language core.

// Script arguments (set by main from argv); os.args() exposes them
inline std::vector<std::string>& scriptArgs() {
    static std::vector<std::string> args;
    return args;
}

inline ObjPtr makeOsModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    // env(name): reads an environment variable; null if unset
    def("env", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().env, "environment read", "--allow-env");
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("env(name) expects a string", line);
        }
        const char* v = std::getenv(static_cast<StringObject*>(args[0].get())->value.c_str());
        if (v == nullptr) return NULL_OBJ_;
        return makeObj<StringObject>(v);
    });

    // set_env(name, value): sets an environment variable for this process
    def("set_env", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().env, "environment write", "--allow-env");
        if (args.size() != 2 || args[0]->type() != ObjectType::STRING ||
            args[1]->type() != ObjectType::STRING) {
            return makeError("set_env(name, value) expects two strings", line);
        }
#if defined(_WIN32)
        _putenv_s(static_cast<StringObject*>(args[0].get())->value.c_str(),
                  static_cast<StringObject*>(args[1].get())->value.c_str());
#else
        setenv(static_cast<StringObject*>(args[0].get())->value.c_str(),
               static_cast<StringObject*>(args[1].get())->value.c_str(), 1);
#endif
        return NULL_OBJ_;
    });

    // platform(): "linux", "windows" or "macos"
    def("platform", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("platform", "0", args.size(), line);
#if defined(_WIN32)
        return makeObj<StringObject>("windows");
#elif defined(__APPLE__)
        return makeObj<StringObject>("macos");
#else
        return makeObj<StringObject>("linux");
#endif
    });

    // cwd(): current working directory
    def("cwd", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("cwd", "0", args.size(), line);
        std::error_code ec;
        auto p = std::filesystem::current_path(ec);
        if (ec) return makeError("cannot read the working directory", line);
        return makeObj<StringObject>(p.string());
    });

    // args(): extra command-line arguments passed after the script path
    def("args", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("args", "0", args.size(), line);
        auto list = makeObj<ListObject>();
        for (const auto& a : scriptArgs()) {
            list->elements.push_back(makeObj<StringObject>(a));
        }
        return list;
    });

    // path_join(a, b, ...): joins path segments with the platform separator
    def("path_join", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 2) return argCountError("path_join", "at least 2", args.size(), line);
        std::filesystem::path p;
        for (const auto& a : args) {
            if (a->type() != ObjectType::STRING) {
                return makeError("path_join() expects only string segments", line);
            }
            p /= static_cast<StringObject*>(a.get())->value;
        }
        return makeObj<StringObject>(p.string());
    });

    def("basename", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) return makeError("basename(path) expects a string", line);
        return makeObj<StringObject>(std::filesystem::path(static_cast<StringObject*>(args[0].get())->value).filename().string());
    });
    def("dirname", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) return makeError("dirname(path) expects a string", line);
        return makeObj<StringObject>(std::filesystem::path(static_cast<StringObject*>(args[0].get())->value).parent_path().string());
    });
    def("extension", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) return makeError("extension(path) expects a string", line);
        return makeObj<StringObject>(std::filesystem::path(static_cast<StringObject*>(args[0].get())->value).extension().string());
    });
    def("temp_dir", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("temp_dir", "0", args.size(), line);
        std::error_code ec;
        return makeObj<StringObject>(std::filesystem::temp_directory_path(ec).string());
    });
    def("home_dir", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("home_dir", "0", args.size(), line);
        const char* h = std::getenv("HOME");
        if (!h) h = std::getenv("USERPROFILE");
        return h ? (ObjPtr)makeObj<StringObject>(h) : NULL_OBJ_;
    });

    // run(cmd): executes a shell command, captures stdout -> {code, out}.
    // Gated by --allow-run (the perms.run capability, until now unused).
    def("run", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        LOVAX_GATE(perms().run, "process execution", "--allow-run");
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("os.run(cmd) expects a string", line);
        }
        const std::string& cmd = static_cast<StringObject*>(args[0].get())->value;
#ifdef _WIN32
        FILE* pipe = _popen(cmd.c_str(), "r");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        if (!pipe) return makeError("os.run() could not start: " + cmd, line);
        std::string out;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) {
            out.append(buf, n);
            if (out.size() > 16 * 1024 * 1024) break;   // 16 MB output cap
        }
#ifdef _WIN32
        int rc = _pclose(pipe);
#else
        int rc = pclose(pipe);
        if (rc != -1) rc = WEXITSTATUS(rc);
#endif
        auto res = makeObj<MapObject>();
        GcRoot _gr(res.get());
        res->set(makeObj<StringObject>("code"), makeObj<IntegerObject>(rc));
        res->set(makeObj<StringObject>("out"), makeObj<StringObject>(out));
        return res;
    });

    mod->frozen = true;
    mod->moduleName = "os";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}


} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_OS_HPP
