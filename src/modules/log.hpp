#ifndef LOVAX_MODULE_LOG_HPP
#define LOVAX_MODULE_LOG_HPP

#include "common.hpp"
#include "datetime.hpp"

// log — leveled logging with timestamps: console by default, optional file.
//   use log
//   log.set_level("debug")      # debug | info | warn | error
//   log.info("oyun başladı")    # [2026-07-17 15:30:00] [INFO] oyun başladı
//   log.to_file("game.log")     # also append every record to a file

namespace Lovax {
namespace StdLib {

struct LogState {
    int level = 1;               // 0 debug, 1 info, 2 warn, 3 error
    std::string filePath;        // empty = console only
};
inline LogState& logState() { static LogState s; return s; }

inline ObjPtr makeLogModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    auto emitRecord = [](int lvl, const char* tag, const Args& args, int line) -> ObjPtr {
        if (lvl < logState().level) return NULL_OBJ_;
        auto t = std::chrono::system_clock::now().time_since_epoch();
        long long ts = std::chrono::duration_cast<std::chrono::seconds>(t).count();
        long long days = ts / 86400, rem = ts % 86400;
        if (rem < 0) { rem += 86400; days -= 1; }
        long long y, mo, d;
        dtCivilFromDays(days, y, mo, d);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "%04lld-%02lld-%02lld %02lld:%02lld:%02lld",
                      y, mo, d, rem / 3600, (rem % 3600) / 60, rem % 60);
        std::string msg;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) msg += " ";
            msg += args[i]->inspect();
        }
        std::string recordLine = std::string("[") + stamp + "] [" + tag + "] " + msg;
        std::cerr << recordLine << "\n";
        if (!logState().filePath.empty()) {
            // File logging respects the write capability (checked at to_file time
            // too, but the sandbox may matter for long-running programs).
            if (perms().write) {
                std::ofstream f(logState().filePath, std::ios::app | std::ios::binary);
                if (f.is_open()) f << recordLine << "\n";
            }
        }
        (void)line;
        return NULL_OBJ_;
    };

    def("debug", [emitRecord](const Args& a, int l, const CallFn&) { return emitRecord(0, "DEBUG", a, l); });
    def("info",  [emitRecord](const Args& a, int l, const CallFn&) { return emitRecord(1, "INFO",  a, l); });
    def("warn",  [emitRecord](const Args& a, int l, const CallFn&) { return emitRecord(2, "WARN",  a, l); });
    def("error", [emitRecord](const Args& a, int l, const CallFn&) { return emitRecord(3, "ERROR", a, l); });

    // set_level("debug"|"info"|"warn"|"error"): records below the level are dropped
    def("set_level", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("log.set_level(level) expects a string", line);
        }
        const std::string& lv = static_cast<StringObject*>(args[0].get())->value;
        if (lv == "debug") logState().level = 0;
        else if (lv == "info") logState().level = 1;
        else if (lv == "warn") logState().level = 2;
        else if (lv == "error") logState().level = 3;
        else return makeError("log.set_level(): debug | info | warn | error", line);
        return NULL_OBJ_;
    });

    // to_file(path): additionally append records to a file ("" turns it off)
    def("to_file", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("log.to_file(path) expects a string", line);
        }
        const std::string& p = static_cast<StringObject*>(args[0].get())->value;
        if (!p.empty()) LOVAX_GATE(perms().write, "log file write", "--allow-write");
        logState().filePath = p;
        return NULL_OBJ_;
    });

    mod->frozen = true;
    mod->moduleName = "log";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_LOG_HPP
