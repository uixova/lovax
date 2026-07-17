#ifndef LOVAX_MODULE_TIME_HPP
#define LOVAX_MODULE_TIME_HPP

#include "common.hpp"

namespace Lovax {
namespace StdLib {

// ===== time module =====
inline ObjPtr makeTimeModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;
    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    // now(): seconds since the Unix epoch (float)
    def("now", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("now", "0", args.size(), line);
        auto d = std::chrono::system_clock::now().time_since_epoch();
        return makeObj<FloatObject>(std::chrono::duration<double>(d).count());
    });
    def("now_ms", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("now_ms", "0", args.size(), line);
        auto d = std::chrono::system_clock::now().time_since_epoch();
        return makeObj<IntegerObject>(
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    });
    auto toTm = [](const Args& args, std::tm& out) -> bool {
        std::time_t t;
        if (args.empty()) t = std::time(nullptr);
        else if (isNumeric(args[0])) t = (std::time_t)asDouble(args[0]);
        else return false;
#if defined(_WIN32)
        localtime_s(&out, &t);
#else
        localtime_r(&t, &out);
#endif
        return true;
    };
    // date([unix]): a map {year, month, day, hour, minute, second, weekday}
    def("date", [toTm](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() > 1) return argCountError("date", "0-1", args.size(), line);
        std::tm tm{};
        if (!toTm(args, tm)) return makeError("date([unix]) expects a number", line);
        auto m = makeObj<MapObject>();
        m->set(strKey("year"),    makeObj<IntegerObject>(tm.tm_year + 1900));
        m->set(strKey("month"),   makeObj<IntegerObject>(tm.tm_mon + 1));
        m->set(strKey("day"),     makeObj<IntegerObject>(tm.tm_mday));
        m->set(strKey("hour"),    makeObj<IntegerObject>(tm.tm_hour));
        m->set(strKey("minute"),  makeObj<IntegerObject>(tm.tm_min));
        m->set(strKey("second"),  makeObj<IntegerObject>(tm.tm_sec));
        m->set(strKey("weekday"), makeObj<IntegerObject>(tm.tm_wday));
        return m;
    });
    // date_text([unix]): "YYYY-MM-DD HH:MM:SS"
    def("date_text", [toTm](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() > 1) return argCountError("date_text", "0-1", args.size(), line);
        std::tm tm{};
        if (!toTm(args, tm)) return makeError("date_text([unix]) expects a number", line);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return makeObj<StringObject>(buf);
    });

    mod->frozen = true;
    mod->moduleName = "time";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}


} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_TIME_HPP
