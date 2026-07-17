#ifndef LOVAX_MODULE_DATETIME_HPP
#define LOVAX_MODULE_DATETIME_HPP

#include "common.hpp"

// datetime — calendar time (RFC-019).
//
// A datetime value is a plain map: {year, month, day, hour, minute, second,
// weekday, timestamp} — readable with dot access (dt.year) and printable.
// All math is our own civil-calendar arithmetic over the UTC timestamp
// (the standard days-from-civil algorithm), so results are identical on
// every platform — no libc timezone tables involved. Local time appears
// only in now_local() (display convenience).

namespace Lovax {
namespace StdLib {

// Days since 1970-01-01 from a civil date (proleptic Gregorian).
inline long long dtDaysFromCivil(long long y, long long m, long long d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;                                  // [0, 399]
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    return era * 146097 + doe - 719468;
}

// Civil date from days since 1970-01-01 (inverse of the above).
inline void dtCivilFromDays(long long z, long long& y, long long& m, long long& d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;                               // [0, 146096]
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);        // [0, 365]
    long long mp = (5 * doy + 2) / 153;                             // [0, 11]
    d = doy - (153 * mp + 2) / 5 + 1;                               // [1, 31]
    m = mp + (mp < 10 ? 3 : -9);                                    // [1, 12]
    y += (m <= 2);
}

// Builds the datetime map for a UTC timestamp.
inline Ref<Object> dtFromTimestamp(long long ts) {
    long long days = ts / 86400, rem = ts % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    long long y, m, d;
    dtCivilFromDays(days, y, m, d);
    // 1970-01-01 was a Thursday; Monday = 0 (Python's weekday()).
    long long wd = (days % 7 + 7 + 3) % 7;
    auto mod = makeObj<MapObject>();
    GcRoot _gr(mod.get());
    auto put = [&](const char* k, long long v) {
        mod->set(makeObj<StringObject>(k), makeObj<IntegerObject>(v));
    };
    put("year", y); put("month", m); put("day", d);
    put("hour", rem / 3600); put("minute", (rem % 3600) / 60); put("second", rem % 60);
    put("weekday", wd);
    put("timestamp", ts);
    return mod;
}

// Reads a datetime map (or a raw integer timestamp) back into seconds.
inline bool dtToTimestamp(const Ref<Object>& o, long long& out) {
    if (o->type() == ObjectType::INTEGER) {
        out = static_cast<IntegerObject*>(o.get())->value;
        return true;
    }
    if (o->type() != ObjectType::MAP) return false;
    auto* m = static_cast<MapObject*>(o.get());
    auto ts = m->getStr("timestamp");
    if (ts != nullptr && ts->type() == ObjectType::INTEGER) {
        out = static_cast<IntegerObject*>(ts.get())->value;
        return true;
    }
    return false;
}

inline ObjPtr makeDatetimeModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    // now(): current moment, UTC fields
    def("now", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("now", "0", args.size(), line);
        auto t = std::chrono::system_clock::now().time_since_epoch();
        return dtFromTimestamp(std::chrono::duration_cast<std::chrono::seconds>(t).count());
    });

    // now_local(): current moment in the machine's local time (display only)
    def("now_local", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("now_local", "0", args.size(), line);
        std::time_t t = std::time(nullptr);
        std::tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        // Rebuild through our own math using the local offset for consistency.
        long long ts = dtDaysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400
                     + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
        return dtFromTimestamp(ts);
    });

    // from_timestamp(secs): UTC datetime map
    def("from_timestamp", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::INTEGER) {
            return makeError("datetime.from_timestamp(secs) expects an integer", line);
        }
        return dtFromTimestamp(static_cast<IntegerObject*>(args[0].get())->value);
    });

    // make(year, month, day[, hour, minute, second]): validated datetime
    def("make", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3 && args.size() != 6) {
            return argCountError("make", "3 or 6", args.size(), line);
        }
        long long v[6] = {0, 0, 0, 0, 0, 0};
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i]->type() != ObjectType::INTEGER) {
                return makeError("datetime.make() expects integers", line);
            }
            v[i] = static_cast<IntegerObject*>(args[i].get())->value;
        }
        static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        bool leap = (v[0] % 4 == 0 && v[0] % 100 != 0) || v[0] % 400 == 0;
        long long dim = mdays[(v[1] >= 1 && v[1] <= 12) ? v[1] - 1 : 0] + (v[1] == 2 && leap ? 1 : 0);
        if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > dim ||
            v[3] < 0 || v[3] > 23 || v[4] < 0 || v[4] > 59 || v[5] < 0 || v[5] > 59) {
            return makeError("datetime.make() invalid date/time", line);
        }
        long long ts = dtDaysFromCivil(v[0], v[1], v[2]) * 86400 +
                       v[3] * 3600 + v[4] * 60 + v[5];
        return dtFromTimestamp(ts);
    });

    // add(dt, days, hours, minutes, seconds): a new shifted datetime
    def("add", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 5) {
            return makeError("datetime.add(dt, days, hours, minutes, seconds) expects 5 arguments", line);
        }
        long long ts;
        if (!dtToTimestamp(args[0], ts)) {
            return makeError("datetime.add() first argument must be a datetime (or timestamp)", line);
        }
        for (int i = 1; i <= 4; ++i) {
            if (args[i]->type() != ObjectType::INTEGER) {
                return makeError("datetime.add() offsets must be integers", line);
            }
        }
        ts += static_cast<IntegerObject*>(args[1].get())->value * 86400
            + static_cast<IntegerObject*>(args[2].get())->value * 3600
            + static_cast<IntegerObject*>(args[3].get())->value * 60
            + static_cast<IntegerObject*>(args[4].get())->value;
        return dtFromTimestamp(ts);
    });

    // diff(a, b): a - b in seconds
    def("diff", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("diff", "2", args.size(), line);
        long long a, b;
        if (!dtToTimestamp(args[0], a) || !dtToTimestamp(args[1], b)) {
            return makeError("datetime.diff(a, b) expects datetimes (or timestamps)", line);
        }
        return makeObj<IntegerObject>(a - b);
    });

    // format(dt, fmt): %Y %m %d %H %M %S %w supported
    def("format", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[1]->type() != ObjectType::STRING) {
            return makeError("datetime.format(dt, fmt) expects a datetime and a string", line);
        }
        long long ts;
        if (!dtToTimestamp(args[0], ts)) {
            return makeError("datetime.format() first argument must be a datetime", line);
        }
        long long days = ts / 86400, rem = ts % 86400;
        if (rem < 0) { rem += 86400; days -= 1; }
        long long y, m, d;
        dtCivilFromDays(days, y, m, d);
        long long wd = (days % 7 + 7 + 3) % 7;
        const std::string& fmt = static_cast<StringObject*>(args[1].get())->value;
        std::string out;
        char buf[16];
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%' || i + 1 >= fmt.size()) { out += fmt[i]; continue; }
            char c = fmt[++i];
            switch (c) {
                case 'Y': std::snprintf(buf, sizeof(buf), "%04lld", y); out += buf; break;
                case 'm': std::snprintf(buf, sizeof(buf), "%02lld", m); out += buf; break;
                case 'd': std::snprintf(buf, sizeof(buf), "%02lld", d); out += buf; break;
                case 'H': std::snprintf(buf, sizeof(buf), "%02lld", rem / 3600); out += buf; break;
                case 'M': std::snprintf(buf, sizeof(buf), "%02lld", (rem % 3600) / 60); out += buf; break;
                case 'S': std::snprintf(buf, sizeof(buf), "%02lld", rem % 60); out += buf; break;
                case 'w': std::snprintf(buf, sizeof(buf), "%lld", wd); out += buf; break;
                case '%': out += '%'; break;
                default:
                    return makeError(std::string("datetime.format() unknown directive %") + c +
                                     " (supported: %Y %m %d %H %M %S %w %%)", line);
            }
        }
        return makeObj<StringObject>(out);
    });

    // parse(text, fmt): inverse of format for the same directives
    def("parse", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::STRING ||
            args[1]->type() != ObjectType::STRING) {
            return makeError("datetime.parse(text, fmt) expects two strings", line);
        }
        const std::string& txt = static_cast<StringObject*>(args[0].get())->value;
        const std::string& fmt = static_cast<StringObject*>(args[1].get())->value;
        long long y = 1970, mo = 1, d = 1, h = 0, mi = 0, se = 0;
        size_t ti = 0;
        auto readInt = [&](int width, long long& out) -> bool {
            long long v = 0; int n = 0;
            while (n < width && ti < txt.size() && txt[ti] >= '0' && txt[ti] <= '9') {
                v = v * 10 + (txt[ti] - '0'); ti++; n++;
            }
            if (n == 0) return false;
            out = v;
            return true;
        };
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%' || i + 1 >= fmt.size()) {
                if (ti >= txt.size() || txt[ti] != fmt[i]) {
                    return makeError("datetime.parse() text does not match format", line);
                }
                ti++;
                continue;
            }
            char c = fmt[++i];
            bool ok = true;
            switch (c) {
                case 'Y': ok = readInt(4, y); break;
                case 'm': ok = readInt(2, mo); break;
                case 'd': ok = readInt(2, d); break;
                case 'H': ok = readInt(2, h); break;
                case 'M': ok = readInt(2, mi); break;
                case 'S': ok = readInt(2, se); break;
                case '%': ok = (ti < txt.size() && txt[ti] == '%'); if (ok) ti++; break;
                default:
                    return makeError(std::string("datetime.parse() unknown directive %") + c, line);
            }
            if (!ok) return makeError("datetime.parse() text does not match format", line);
        }
        if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 59) {
            return makeError("datetime.parse() parsed values out of range", line);
        }
        return dtFromTimestamp(dtDaysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se);
    });

    // weekday(dt): Monday = 0 ... Sunday = 6
    def("weekday", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("weekday", "1", args.size(), line);
        long long ts;
        if (!dtToTimestamp(args[0], ts)) {
            return makeError("datetime.weekday(dt) expects a datetime (or timestamp)", line);
        }
        long long days = ts / 86400;
        if (ts % 86400 < 0) days -= 1;
        return makeObj<IntegerObject>((days % 7 + 7 + 3) % 7);
    });

    mod->frozen = true;
    mod->moduleName = "datetime";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_DATETIME_HPP
