#ifndef STDLIB_HPP
#define STDLIB_HPP

#include <memory>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <limits>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <unordered_map>
#include <cstring>
#include "../object/object.hpp"
#include "../object/environment.hpp"
#include "builtins.hpp"
#include "../utils/colors.hpp"

// Sockets for the `net` module. OS syscalls only — still zero third-party deps.
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <sys/time.h>
#endif

// Lume built-in standard library (RFC-006).
// Modules NEVER enter the global scope until invited with 'use':
//   use math   -> math.lerp(0, 10, 0.5)
//   use game: pick_weighted, signal
//   use strings: upper, split
//   use file as f -> f.save_data(...)
// Every module is a FROZEN map: contents are immutable, the language cannot be broken.

namespace Lume {
namespace StdLib {

using Args = Builtins::Args;
using ObjPtr = Builtins::ObjPtr;
using CallFn = BuiltinObject::CallFn;
using Builtins::argCountError;
using Builtins::isNumeric;
using Builtins::asDouble;
using Builtins::rng;

// ===== Shared helpers =====

inline long long floorMod(long long a, long long b) {
    long long m = a % b;
    if (m != 0 && ((m < 0) != (b < 0))) m += b;
    return m;
}

inline double floorModF(double a, double b) {
    double m = std::fmod(a, b);
    if (m != 0.0 && ((m < 0.0) != (b < 0.0))) m += b;
    return m;
}

// Turkish-aware case mapping (including i→İ, ı→I)
inline const std::unordered_map<std::string, std::string>& trToUpper() {
    static const std::unordered_map<std::string, std::string> m = {
        {"ç","Ç"}, {"ğ","Ğ"}, {"ı","I"}, {"i","İ"}, {"ö","Ö"}, {"ş","Ş"}, {"ü","Ü"}
    };
    return m;
}
inline const std::unordered_map<std::string, std::string>& trToLower() {
    static const std::unordered_map<std::string, std::string> m = {
        {"Ç","ç"}, {"Ğ","ğ"}, {"I","ı"}, {"İ","i"}, {"Ö","ö"}, {"Ş","ş"}, {"Ü","ü"}
    };
    return m;
}

inline std::string caseConvert(const std::string& s, bool toUpper) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        int len = utf8CharLen(static_cast<unsigned char>(s[i]));
        std::string ch = s.substr(i, len);
        if (len == 1) {
            char c = ch[0];
            if (toUpper) {
                if (c == 'i') ch = "İ";                       // Turkish rule
                else if (c >= 'a' && c <= 'z') ch = std::string(1, (char)(c - 32));
            } else {
                if (c == 'I') ch = "ı";                       // Turkish rule
                else if (c >= 'A' && c <= 'Z') ch = std::string(1, (char)(c + 32));
            }
        } else {
            const auto& table = toUpper ? trToUpper() : trToLower();
            auto it = table.find(ch);
            if (it != table.end()) ch = it->second;
        }
        out += ch;
        i += len;
    }
    return out;
}

// ===== JSON writer (save_data) =====

inline bool jsonWrite(const ObjPtr& v, std::string& out, std::string& err, int depth) {
    if (depth > 100) { err = "data too deep (nesting limit 100)"; return false; }
    switch (v->type()) {
        case ObjectType::NULL_OBJ: out += "null"; return true;
        case ObjectType::BOOLEAN:
            out += static_cast<BooleanObject*>(v.get())->value ? "true" : "false";
            return true;
        case ObjectType::INTEGER: out += v->inspect(); return true;
        case ObjectType::FLOAT: {
            double d = static_cast<FloatObject*>(v.get())->value;
            if (!std::isfinite(d)) { err = "infinite/NaN float cannot be written to JSON"; return false; }
            out += formatFloat(d);
            return true;
        }
        case ObjectType::STRING: {
            out += '"';
            for (char c : static_cast<StringObject*>(v.get())->value) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else {
                            out += c; // UTF-8 bytes pass through untouched
                        }
                }
            }
            out += '"';
            return true;
        }
        case ObjectType::LIST: {
            out += '[';
            auto* l = static_cast<ListObject*>(v.get());
            for (size_t i = 0; i < l->elements.size(); ++i) {
                if (i > 0) out += ", ";
                if (!jsonWrite(l->elements[i], out, err, depth + 1)) return false;
            }
            out += ']';
            return true;
        }
        case ObjectType::MAP: {
            out += '{';
            auto* m = static_cast<MapObject*>(v.get());
            for (size_t i = 0; i < m->entries.size(); ++i) {
                if (i > 0) out += ", ";
                if (m->entries[i].first->type() != ObjectType::STRING) {
                    err = "JSON map keys must be strings (found " +
                          typeName(m->entries[i].first->type()) + ")";
                    return false;
                }
                if (!jsonWrite(m->entries[i].first, out, err, depth + 1)) return false;
                out += ": ";
                if (!jsonWrite(m->entries[i].second, out, err, depth + 1)) return false;
            }
            out += '}';
            return true;
        }
        default:
            err = typeName(v->type()) + " cannot be saved (data only: null/bool/number/string/list/map)";
            return false;
    }
}

// ===== JSON okuyucu (load_data) =====

struct JsonParser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    JsonParser(const std::string& src) : s(src) {}

    void skipWs() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++; }
    bool fail(const std::string& msg) { if (err.empty()) err = msg + " (position " + std::to_string(i) + ")"; return false; }

    bool parseValue(ObjPtr& out, int depth) {
        if (depth > 100) return fail("JSON too deep");
        skipWs();
        if (i >= s.size()) return fail("unexpected end of data");
        char c = s[i];
        if (c == 'n') return parseLit("null", NULL_OBJ_, out);
        if (c == 't') return parseLit("true", TRUE_OBJ, out);
        if (c == 'f') return parseLit("false", FALSE_OBJ, out);
        if (c == '"') return parseString(out);
        if (c == '[') return parseArray(out, depth);
        if (c == '{') return parseObject(out, depth);
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
        return fail(std::string("unexpected character '") + c + "'");
    }

    bool parseLit(const char* lit, const ObjPtr& val, ObjPtr& out) {
        size_t n = std::string(lit).size();
        if (s.compare(i, n, lit) != 0) return fail("malformed value");
        i += n;
        out = val;
        return true;
    }

    bool parseNumber(ObjPtr& out) {
        size_t start = i;
        if (i < s.size() && s[i] == '-') i++;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        bool isFloat = false;
        if (i < s.size() && s[i] == '.') {
            isFloat = true;
            i++;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            isFloat = true;
            i++;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        }
        std::string numStr = s.substr(start, i - start);
        try {
            if (isFloat) out = std::make_shared<FloatObject>(std::stod(numStr));
            else out = std::make_shared<IntegerObject>(std::stoll(numStr));
        } catch (...) {
            return fail("could not parse number: " + numStr);
        }
        return true;
    }

    void appendUtf8(std::string& str, unsigned int cp) {
        if (cp < 0x80) str += (char)cp;
        else if (cp < 0x800) {
            str += (char)(0xC0 | (cp >> 6));
            str += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            str += (char)(0xE0 | (cp >> 12));
            str += (char)(0x80 | ((cp >> 6) & 0x3F));
            str += (char)(0x80 | (cp & 0x3F));
        } else {
            str += (char)(0xF0 | (cp >> 18));
            str += (char)(0x80 | ((cp >> 12) & 0x3F));
            str += (char)(0x80 | ((cp >> 6) & 0x3F));
            str += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool readHex4(unsigned int& out) {
        if (i + 4 > s.size()) return fail("incomplete \\u sequence");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (unsigned)(c - 'A' + 10);
            else return fail("malformed \\u sequence");
        }
        return true;
    }

    bool parseString(ObjPtr& out) {
        i++; // opening quote
        std::string str;
        while (i < s.size() && s[i] != '"') {
            char c = s[i];
            if (c == '\\') {
                i++;
                if (i >= s.size()) return fail("unterminated string");
                char e = s[i++];
                switch (e) {
                    case '"':  str += '"';  break;
                    case '\\': str += '\\'; break;
                    case '/':  str += '/';  break;
                    case 'n':  str += '\n'; break;
                    case 'r':  str += '\r'; break;
                    case 't':  str += '\t'; break;
                    case 'b':  str += '\b'; break;
                    case 'f':  str += '\f'; break;
                    case 'u': {
                        unsigned int cp;
                        if (!readHex4(cp)) return false;
                        // Surrogate pair: high + low combination
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (i + 1 < s.size() && s[i] == '\\' && s[i+1] == 'u') {
                                i += 2;
                                unsigned int lo;
                                if (!readHex4(lo)) return false;
                                if (lo < 0xDC00 || lo > 0xDFFF) return fail("malformed surrogate pair");
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else return fail("missing surrogate pair");
                        }
                        appendUtf8(str, cp);
                        break;
                    }
                    default: return fail(std::string("unknown escape: \\") + e);
                }
            } else {
                str += c;
                i++;
            }
        }
        if (i >= s.size()) return fail("unterminated string");
        i++; // closing quote
        out = std::make_shared<StringObject>(str);
        return true;
    }

    bool parseArray(ObjPtr& out, int depth) {
        i++; // '['
        auto list = std::make_shared<ListObject>();
        skipWs();
        if (i < s.size() && s[i] == ']') { i++; out = list; return true; }
        while (true) {
            ObjPtr v;
            if (!parseValue(v, depth + 1)) return false;
            list->elements.push_back(v);
            skipWs();
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == ']') { i++; break; }
            return fail("expected ',' or ']'");
        }
        out = list;
        return true;
    }

    bool parseObject(ObjPtr& out, int depth) {
        i++; // '{'
        auto map = std::make_shared<MapObject>();
        skipWs();
        if (i < s.size() && s[i] == '}') { i++; out = map; return true; }
        while (true) {
            skipWs();
            if (i >= s.size() || s[i] != '"') return fail("expected a string key");
            ObjPtr key;
            if (!parseString(key)) return false;
            skipWs();
            if (i >= s.size() || s[i] != ':') return fail("expected ':'");
            i++;
            ObjPtr val;
            if (!parseValue(val, depth + 1)) return false;
            map->set(key, val);
            skipWs();
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == '}') { i++; break; }
            return fail("expected ',' or '}'");
        }
        out = map;
        return true;
    }
};

// ===== Easing (Penner functions) =====

inline bool easeByName(const std::string& name, double t, double& out) {
    const double PI_ = 3.14159265358979323846;
    if (name == "linear")       { out = t; return true; }
    if (name == "in_quad")      { out = t * t; return true; }
    if (name == "out_quad")     { out = 1 - (1 - t) * (1 - t); return true; }
    if (name == "in_out_quad")  { out = t < 0.5 ? 2*t*t : 1 - std::pow(-2*t + 2, 2) / 2; return true; }
    if (name == "in_cubic")     { out = t * t * t; return true; }
    if (name == "out_cubic")    { out = 1 - std::pow(1 - t, 3); return true; }
    if (name == "in_out_cubic") { out = t < 0.5 ? 4*t*t*t : 1 - std::pow(-2*t + 2, 3) / 2; return true; }
    if (name == "in_sine")      { out = 1 - std::cos(t * PI_ / 2); return true; }
    if (name == "out_sine")     { out = std::sin(t * PI_ / 2); return true; }
    if (name == "in_out_sine")  { out = -(std::cos(PI_ * t) - 1) / 2; return true; }
    if (name == "out_back") {
        const double c1 = 1.70158, c3 = c1 + 1;
        out = 1 + c3 * std::pow(t - 1, 3) + c1 * std::pow(t - 1, 2);
        return true;
    }
    if (name == "out_elastic") {
        if (t == 0) { out = 0; return true; }
        if (t == 1) { out = 1; return true; }
        const double c4 = (2 * PI_) / 3;
        out = std::pow(2, -10 * t) * std::sin((t * 10 - 0.75) * c4) + 1;
        return true;
    }
    if (name == "out_bounce") {
        const double n1 = 7.5625, d1 = 2.75;
        if (t < 1 / d1)        out = n1 * t * t;
        else if (t < 2 / d1)   { t -= 1.5 / d1;   out = n1 * t * t + 0.75; }
        else if (t < 2.5 / d1) { t -= 2.25 / d1;  out = n1 * t * t + 0.9375; }
        else                   { t -= 2.625 / d1; out = n1 * t * t + 0.984375; }
        return true;
    }
    return false;
}

// ===== Module construction helpers =====

inline std::shared_ptr<StringObject> strKey(const std::string& k) {
    return std::make_shared<StringObject>(k);
}

// ===== Capability permissions (RFC-015, sandbox / --allow-* flags) =====
// Default: everything allowed (a script you wrote, you trust). `lume --sandbox`
// flips these to deny, and `--allow-net/read/write/env/run` grant back exactly
// what a program needs. This is the runtime half of the malicious-package
// defense (the other half is version pinning, RFC-007 phase 2): even an
// installed package physically cannot open a socket or read your files unless
// you allowed it.
struct Perms {
    bool net = true, read = true, write = true, env = true, run = true;
};
inline Perms& perms() { static Perms p; return p; }

// Returns a catchable permission error when the capability is denied, else null.
inline ObjPtr permGate(bool allowed, const char* what, const char* flag, int line) {
    if (allowed) return nullptr;
    return makeError(std::string("permission denied: ") + what + " requires " + flag +
                     " (or --allow-all)", line);
}
#define LUME_GATE(cond, what, flag) \
    do { if (auto _pe = permGate((cond), (what), (flag), line)) return _pe; } while (0)

// ===== math module =====

inline ObjPtr makeMathModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = std::make_shared<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), std::make_shared<BuiltinObject>(name, std::move(fn)));
    };

    mod->set(strKey("PI"),  std::make_shared<FloatObject>(3.14159265358979323846));
    mod->set(strKey("TAU"), std::make_shared<FloatObject>(6.28318530717958647692));

    // lerp(a, b, t): linear interpolation — t=0 -> a, t=1 -> b
    def("lerp", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3) return argCountError("lerp", "3", args.size(), line);
        if (!isNumeric(args[0]) || !isNumeric(args[1]) || !isNumeric(args[2])) {
            return makeError("lerp(a, b, t) expects three numbers", line);
        }
        double a = asDouble(args[0]), b = asDouble(args[1]), t = asDouble(args[2]);
        return std::make_shared<FloatObject>(a + (b - a) * t);
    });

    // clamp(x, lo, hi): clamps the value into the range
    def("clamp", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3) return argCountError("clamp", "3", args.size(), line);
        bool allInt = true;
        for (const auto& a : args) {
            if (!isNumeric(a)) return makeError("clamp(x, lo, hi) expects three numbers", line);
            if (a->type() == ObjectType::FLOAT) allInt = false;
        }
        double x = asDouble(args[0]), lo = asDouble(args[1]), hi = asDouble(args[2]);
        if (lo > hi) return makeError("clamp() lower bound cannot exceed the upper bound", line);
        double r = x < lo ? lo : (x > hi ? hi : x);
        if (allInt) return std::make_shared<IntegerObject>((long long)r);
        return std::make_shared<FloatObject>(r);
    });

    // remap(x, a1, b1, a2, b2): maps x from range [a1,b1] to [a2,b2]
    def("remap", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 5) return argCountError("remap", "5", args.size(), line);
        for (const auto& a : args) {
            if (!isNumeric(a)) return makeError("remap() expects five numbers", line);
        }
        double x = asDouble(args[0]);
        double a1 = asDouble(args[1]), b1 = asDouble(args[2]);
        double a2 = asDouble(args[3]), b2 = asDouble(args[4]);
        if (b1 == a1) return makeError("remap() source range has zero width (a1 == b1)", line);
        return std::make_shared<FloatObject>(a2 + (x - a1) * (b2 - a2) / (b1 - a1));
    });

    // sign(x): -1, 0, or 1
    def("sign", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("sign", "1", args.size(), line);
        if (!isNumeric(args[0])) return makeError("sign() expects a number", line);
        double v = asDouble(args[0]);
        return std::make_shared<IntegerObject>(v > 0 ? 1 : (v < 0 ? -1 : 0));
    });

    // wrap(x, min, max): wraps the value into the range (angles, grids) — max EXCLUSIVE
    def("wrap", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3) return argCountError("wrap", "3", args.size(), line);
        bool allInt = true;
        for (const auto& a : args) {
            if (!isNumeric(a)) return makeError("wrap(x, min, max) expects three numbers", line);
            if (a->type() == ObjectType::FLOAT) allInt = false;
        }
        if (allInt) {
            long long x = static_cast<IntegerObject*>(args[0].get())->value;
            long long lo = static_cast<IntegerObject*>(args[1].get())->value;
            long long hi = static_cast<IntegerObject*>(args[2].get())->value;
            if (hi <= lo) return makeError("wrap() max must be greater than min", line);
            return std::make_shared<IntegerObject>(lo + floorMod(x - lo, hi - lo));
        }
        double x = asDouble(args[0]), lo = asDouble(args[1]), hi = asDouble(args[2]);
        if (hi <= lo) return makeError("wrap() max must be greater than min", line);
        return std::make_shared<FloatObject>(lo + floorModF(x - lo, hi - lo));
    });

    // move_toward(current, target, delta): moves toward target by at most delta (Godot)
    def("move_toward", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3) return argCountError("move_toward", "3", args.size(), line);
        for (const auto& a : args) {
            if (!isNumeric(a)) return makeError("move_toward() expects three numbers", line);
        }
        double cur = asDouble(args[0]), target = asDouble(args[1]), delta = asDouble(args[2]);
        if (std::fabs(target - cur) <= delta) return std::make_shared<FloatObject>(target);
        return std::make_shared<FloatObject>(cur + (target > cur ? delta : -delta));
    });

    // dist(x1, y1, x2, y2): distance between two points
    def("dist", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 4) return argCountError("dist", "4", args.size(), line);
        for (const auto& a : args) {
            if (!isNumeric(a)) return makeError("dist(x1, y1, x2, y2) expects four numbers", line);
        }
        double dx = asDouble(args[2]) - asDouble(args[0]);
        double dy = asDouble(args[3]) - asDouble(args[1]);
        return std::make_shared<FloatObject>(std::hypot(dx, dy));
    });

    // Trigonometry + angle conversion
    auto floatFn1 = [](const std::string& name, double(*f)(double)) {
        return [name, f](const Args& args, int line, const CallFn&) -> ObjPtr {
            if (args.size() != 1) return argCountError(name, "1", args.size(), line);
            if (!isNumeric(args[0])) return makeError(name + "() expects a number", line);
            return std::make_shared<FloatObject>(f(asDouble(args[0])));
        };
    };
    def("sin", floatFn1("sin", std::sin));
    def("cos", floatFn1("cos", std::cos));
    def("tan", floatFn1("tan", std::tan));
    def("atan2", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("atan2", "2", args.size(), line);
        if (!isNumeric(args[0]) || !isNumeric(args[1])) return makeError("atan2(y, x) expects two numbers", line);
        return std::make_shared<FloatObject>(std::atan2(asDouble(args[0]), asDouble(args[1])));
    });
    def("deg", floatFn1("deg", [](double r) { return r * 180.0 / 3.14159265358979323846; }));
    def("rad", floatFn1("rad", [](double d) { return d * 3.14159265358979323846 / 180.0; }));
    def("asin", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) return makeError("asin() expects a number", line);
        double v = asDouble(args[0]);
        if (v < -1 || v > 1) return makeError("asin() expects a value in -1..1", line);
        return std::make_shared<FloatObject>(std::asin(v));
    });
    def("acos", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) return makeError("acos() expects a number", line);
        double v = asDouble(args[0]);
        if (v < -1 || v > 1) return makeError("acos() expects a value in -1..1", line);
        return std::make_shared<FloatObject>(std::acos(v));
    });
    def("exp", floatFn1("exp", std::exp));
    // log(x): natural log | log(x, base)
    def("log", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.empty() || args.size() > 2 || !isNumeric(args[0])) {
            return makeError("log(x[, base]) expects one or two numbers", line);
        }
        double v = asDouble(args[0]);
        if (v <= 0) return makeError("log() expects a positive number", line);
        if (args.size() == 1) return std::make_shared<FloatObject>(std::log(v));
        if (!isNumeric(args[1])) return makeError("log() base must be a number", line);
        double base = asDouble(args[1]);
        if (base <= 0 || base == 1) return makeError("log() base must be positive and not 1", line);
        return std::make_shared<FloatObject>(std::log(v) / std::log(base));
    });
    // snap(x, step): rounds to the nearest multiple (grid alignment) — snap(13, 5) -> 15.0
    def("snap", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !isNumeric(args[0]) || !isNumeric(args[1])) {
            return makeError("snap(x, step) expects two numbers", line);
        }
        double step = asDouble(args[1]);
        if (step == 0) return makeError("snap() step cannot be 0", line);
        return std::make_shared<FloatObject>(std::round(asDouble(args[0]) / step) * step);
    });

    mod->set(strKey("INF"), std::make_shared<FloatObject>(std::numeric_limits<double>::infinity()));

    // inverse_lerp(a, b, v): where v falls between a and b as 0..1
    def("inverse_lerp", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3 || !isNumeric(args[0]) || !isNumeric(args[1]) || !isNumeric(args[2]))
            return makeError("inverse_lerp(a, b, v) expects three numbers", line);
        double a = asDouble(args[0]), b = asDouble(args[1]), v = asDouble(args[2]);
        if (a == b) return std::make_shared<FloatObject>(0.0);
        return std::make_shared<FloatObject>((v - a) / (b - a));
    });
    // smoothstep(a, b, t): smooth Hermite interpolation, clamped
    def("smoothstep", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3 || !isNumeric(args[0]) || !isNumeric(args[1]) || !isNumeric(args[2]))
            return makeError("smoothstep(a, b, t) expects three numbers", line);
        double a = asDouble(args[0]), b = asDouble(args[1]), t = asDouble(args[2]);
        if (a == b) return std::make_shared<FloatObject>(0.0);
        t = (t - a) / (b - a);
        if (t < 0) t = 0; if (t > 1) t = 1;
        return std::make_shared<FloatObject>(t * t * (3 - 2 * t));
    });
    // ping_pong(t, length): bounces t back and forth in [0, length]
    def("ping_pong", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !isNumeric(args[0]) || !isNumeric(args[1]))
            return makeError("ping_pong(t, length) expects two numbers", line);
        double t = asDouble(args[0]), len = asDouble(args[1]);
        if (len <= 0) return makeError("ping_pong() length must be positive", line);
        double m = floorModF(t, len * 2);
        return std::make_shared<FloatObject>(m <= len ? m : len * 2 - m);
    });
    // approx(a, b[, eps]): float equality within a tolerance
    def("approx", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 2 || args.size() > 3 || !isNumeric(args[0]) || !isNumeric(args[1]))
            return makeError("approx(a, b[, eps]) expects two numbers", line);
        double eps = args.size() == 3 && isNumeric(args[2]) ? asDouble(args[2]) : 1e-9;
        return boolObj(std::fabs(asDouble(args[0]) - asDouble(args[1])) <= eps);
    });
    def("is_nan", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) return makeError("is_nan() expects a number", line);
        return boolObj(std::isnan(asDouble(args[0])));
    });
    def("is_inf", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) return makeError("is_inf() expects a number", line);
        return boolObj(std::isinf(asDouble(args[0])));
    });
    def("gcd", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::INTEGER || args[1]->type() != ObjectType::INTEGER)
            return makeError("gcd(a, b) expects two integers", line);
        long long a = std::llabs(static_cast<IntegerObject*>(args[0].get())->value);
        long long b = std::llabs(static_cast<IntegerObject*>(args[1].get())->value);
        while (b) { long long t = b; b = a % b; a = t; }
        return std::make_shared<IntegerObject>(a);
    });
    def("lcm", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::INTEGER || args[1]->type() != ObjectType::INTEGER)
            return makeError("lcm(a, b) expects two integers", line);
        long long a = std::llabs(static_cast<IntegerObject*>(args[0].get())->value);
        long long b = std::llabs(static_cast<IntegerObject*>(args[1].get())->value);
        if (a == 0 || b == 0) return std::make_shared<IntegerObject>(0);
        long long g = a; long long bb = b; while (bb) { long long t = bb; bb = g % bb; g = t; }
        return std::make_shared<IntegerObject>(a / g * b);
    });
    def("factorial", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::INTEGER)
            return makeError("factorial(n) expects an integer", line);
        long long n = static_cast<IntegerObject*>(args[0].get())->value;
        if (n < 0) return makeError("factorial() expects n >= 0", line);
        if (n > 20) return makeError("factorial() overflows for n > 20", line);
        long long r = 1; for (long long i = 2; i <= n; ++i) r *= i;
        return std::make_shared<IntegerObject>(r);
    });

    mod->frozen = true;
    mod->moduleName = "math";
    cached = mod;
    return mod;
}

// ===== game module =====

inline ObjPtr makeGameModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = std::make_shared<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), std::make_shared<BuiltinObject>(name, std::move(fn)));
    };

    // ease(t, "out_bounce"): Penner easing — t is clamped to [0,1]
    def("ease", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("ease", "2", args.size(), line);
        if (!isNumeric(args[0]) || args[1]->type() != ObjectType::STRING) {
            return makeError("ease(t, name) expects a number and a string", line);
        }
        double t = asDouble(args[0]);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        const std::string& name = static_cast<StringObject*>(args[1].get())->value;
        double out;
        if (!easeByName(name, t, out)) {
            return makeError("unknown easing: \"" + name + "\" (valid: linear, in_quad, out_quad, "
                             "in_out_quad, in_cubic, out_cubic, in_out_cubic, in_sine, out_sine, "
                             "in_out_sine, out_back, out_elastic, out_bounce)", line);
        }
        return std::make_shared<FloatObject>(out);
    });

    // pick(list): random element
    def("pick", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("pick", "1", args.size(), line);
        if (args[0]->type() != ObjectType::LIST) {
            return makeError("pick() expects a list, got " + typeName(args[0]->type()) + "", line);
        }
        auto* l = static_cast<ListObject*>(args[0].get());
        if (l->elements.empty()) return makeError("pick() cannot pick from an empty list", line);
        std::uniform_int_distribution<size_t> dist(0, l->elements.size() - 1);
        return l->elements[dist(rng())];
    });

    // pick_weighted({"sword": 5, "gem": 1}): picks a key by weight (loot tables!)
    def("pick_weighted", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("pick_weighted", "1", args.size(), line);
        if (args[0]->type() != ObjectType::MAP) {
            return makeError("pick_weighted() expects a map: {\"reward\": weight, ...}", line);
        }
        auto* m = static_cast<MapObject*>(args[0].get());
        if (m->entries.empty()) return makeError("pick_weighted() cannot pick from an empty map", line);
        double total = 0;
        for (const auto& e : m->entries) {
            if (!isNumeric(e.second)) {
                return makeError("pick_weighted() weights must be numbers", line);
            }
            double w = asDouble(e.second);
            if (w < 0) return makeError("pick_weighted() weights cannot be negative", line);
            total += w;
        }
        if (total <= 0) return makeError("pick_weighted() total weight must be greater than 0", line);
        std::uniform_real_distribution<double> dist(0.0, total);
        double r = dist(rng());
        double acc = 0;
        for (const auto& e : m->entries) {
            acc += asDouble(e.second);
            if (r < acc) return e.first;
        }
        return m->entries.back().first;
    });

    // shuffle(list): shuffles in place (Fisher-Yates), returns the list
    def("shuffle", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("shuffle", "1", args.size(), line);
        if (args[0]->type() != ObjectType::LIST) {
            return makeError("shuffle() expects a list, got " + typeName(args[0]->type()) + "", line);
        }
        auto& els = static_cast<ListObject*>(args[0].get())->elements;
        for (size_t i = els.size(); i > 1; --i) {
            std::uniform_int_distribution<size_t> dist(0, i - 1);
            std::swap(els[i - 1], els[dist(rng())]);
        }
        return args[0];
    });

    // signal(): new signal object
    def("signal", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("signal", "0", args.size(), line);
        return std::make_shared<SignalObject>();
    });

    // connect(signal, fn): adds a listener (the same function is not added twice)
    def("connect", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("connect", "2", args.size(), line);
        if (args[0]->type() != ObjectType::SIGNAL ||
            (args[1]->type() != ObjectType::FUNCTION && args[1]->type() != ObjectType::BUILTIN)) {
            return makeError("connect(signal, fn) expects a signal and a function", line);
        }
        auto* sig = static_cast<SignalObject*>(args[0].get());
        for (const auto& l : sig->listeners) {
            if (l.get() == args[1].get()) return args[0]; // already connected
        }
        sig->listeners.push_back(args[1]);
        return args[0];
    });

    // disconnect(signal, fn): removes a listener
    def("disconnect", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("disconnect", "2", args.size(), line);
        if (args[0]->type() != ObjectType::SIGNAL) {
            return makeError("disconnect(signal, fn) expects a signal as its first argument", line);
        }
        auto* sig = static_cast<SignalObject*>(args[0].get());
        for (size_t i = 0; i < sig->listeners.size(); ++i) {
            if (sig->listeners[i].get() == args[1].get()) {
                sig->listeners.erase(sig->listeners.begin() + i);
                return TRUE_OBJ;
            }
        }
        return FALSE_OBJ;
    });

    // emit(signal, ...args): calls every listener in order
    def("emit", [](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.empty()) return argCountError("emit", "at least 1", args.size(), line);
        if (args[0]->type() != ObjectType::SIGNAL) {
            return makeError("emit(signal, ...) expects a signal as its first argument", line);
        }
        auto* sig = static_cast<SignalObject*>(args[0].get());
        Args fwd(args.begin() + 1, args.end());
        // Snapshot copy: stays safe if a listener disconnects during emit
        auto listeners = sig->listeners;
        for (const auto& l : listeners) {
            auto r = call(l, fwd, line);
            if (isError(r)) return r;
        }
        return NULL_OBJ_;
    });

    // ===== Timers (poll-based: checked each frame with timer_done) =====

    auto nowSeconds = []() {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration<double>(now).count();
    };

    // timer(seconds): creates a countdown object (map)
    def("timer", [nowSeconds](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) {
            return makeError("timer(seconds) expects a number", line);
        }
        auto t = std::make_shared<MapObject>();
        t->set(std::make_shared<StringObject>("duration"),
               std::make_shared<FloatObject>(asDouble(args[0])));
        t->set(std::make_shared<StringObject>("start"),
               std::make_shared<FloatObject>(nowSeconds()));
        return t;
    });

    auto timerFields = [](const ObjPtr& t, double& süre, double& start) -> bool {
        if (t->type() != ObjectType::MAP) return false;
        auto* m = static_cast<MapObject*>(t.get());
        auto s = m->get(std::make_shared<StringObject>("duration"));
        auto b = m->get(std::make_shared<StringObject>("start"));
        if (s == nullptr || b == nullptr || !isNumeric(s) || !isNumeric(b)) return false;
        süre = asDouble(s);
        start = asDouble(b);
        return true;
    };

    // timer_done(t): has the duration elapsed?
    def("timer_done", [nowSeconds, timerFields](const Args& args, int line, const CallFn&) -> ObjPtr {
        double süre, start;
        if (args.size() != 1 || !timerFields(args[0], süre, start)) {
            return makeError("timer_done() expects an object created by timer()", line);
        }
        return boolObj(nowSeconds() - start >= süre);
    });

    // timer_left(t): remaining seconds (never below 0)
    def("timer_left", [nowSeconds, timerFields](const Args& args, int line, const CallFn&) -> ObjPtr {
        double süre, start;
        if (args.size() != 1 || !timerFields(args[0], süre, start)) {
            return makeError("timer_left() expects an object created by timer()", line);
        }
        double left = süre - (nowSeconds() - start);
        return std::make_shared<FloatObject>(left > 0 ? left : 0.0);
    });

    // timer_reset(t): restarts the countdown (cooldown pattern)
    def("timer_reset", [nowSeconds, timerFields](const Args& args, int line, const CallFn&) -> ObjPtr {
        double süre, start;
        if (args.size() != 1 || !timerFields(args[0], süre, start)) {
            return makeError("timer_reset() expects an object created by timer()", line);
        }
        static_cast<MapObject*>(args[0].get())->set(
            std::make_shared<StringObject>("start"),
            std::make_shared<FloatObject>(nowSeconds()));
        return args[0];
    });

    // chance(p): true with probability p (0..1) — game sugar for random() < p
    def("chance", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) return makeError("chance(p) expects a number in 0..1", line);
        double p = asDouble(args[0]);
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return boolObj(dist(rng()) < p);
    });
    // noise(x[, y]): deterministic value noise in 0..1 (seeded; terrain/jitter)
    def("noise", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.empty() || args.size() > 2 || !isNumeric(args[0]) ||
            (args.size() == 2 && !isNumeric(args[1])))
            return makeError("noise(x[, y]) expects one or two numbers", line);
        auto hash = [](long long n) -> double {
            unsigned long long h = (unsigned long long)n * 2654435761ULL;
            h ^= h >> 15; h *= 0x2545F4914F6CDD1DULL; h ^= h >> 13;
            return (double)(h & 0xFFFFFF) / (double)0xFFFFFF;
        };
        auto smooth = [](double t) { return t * t * (3 - 2 * t); };
        double x = asDouble(args[0]);
        double y = args.size() == 2 ? asDouble(args[1]) : 0.0;
        long long xi = (long long)std::floor(x), yi = (long long)std::floor(y);
        double xf = x - xi, yf = y - yi;
        auto corner = [&](long long cx, long long cy) { return hash(cx * 73856093LL ^ cy * 19349663LL); };
        double n00 = corner(xi, yi), n10 = corner(xi + 1, yi);
        double n01 = corner(xi, yi + 1), n11 = corner(xi + 1, yi + 1);
        double sx = smooth(xf), sy = smooth(yf);
        double a = n00 + (n10 - n00) * sx;
        double b = n01 + (n11 - n01) * sx;
        return std::make_shared<FloatObject>(a + (b - a) * sy);
    });

    mod->frozen = true;
    mod->moduleName = "game";
    cached = mod;
    return mod;
}

// ===== strings module =====

inline ObjPtr makeTextModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = std::make_shared<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), std::make_shared<BuiltinObject>(name, std::move(fn)));
    };

    // split(text, separator): splits into parts; "" splits into characters
    def("split", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("split", "2", args.size(), line);
        if (args[0]->type() != ObjectType::STRING || args[1]->type() != ObjectType::STRING) {
            return makeError("split(text, separator) expects two strings", line);
        }
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        const std::string& sep = static_cast<StringObject*>(args[1].get())->value;
        auto list = std::make_shared<ListObject>();
        if (sep.empty()) {
            size_t i = 0;
            while (i < s.size()) {
                int len = utf8CharLen(static_cast<unsigned char>(s[i]));
                list->elements.push_back(std::make_shared<StringObject>(s.substr(i, len)));
                i += len;
            }
            return list;
        }
        size_t pos = 0;
        while (true) {
            size_t found = s.find(sep, pos);
            if (found == std::string::npos) {
                list->elements.push_back(std::make_shared<StringObject>(s.substr(pos)));
                break;
            }
            list->elements.push_back(std::make_shared<StringObject>(s.substr(pos, found - pos)));
            pos = found + sep.size();
        }
        return list;
    });

    // join(list, separator): joins elements as text
    def("join", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("join", "2", args.size(), line);
        if (args[0]->type() != ObjectType::LIST || args[1]->type() != ObjectType::STRING) {
            return makeError("join(list, separator) expects a list and a string", line);
        }
        const auto& els = static_cast<ListObject*>(args[0].get())->elements;
        const std::string& sep = static_cast<StringObject*>(args[1].get())->value;
        std::string out;
        for (size_t i = 0; i < els.size(); ++i) {
            if (i > 0) out += sep;
            out += els[i]->inspect();
        }
        return std::make_shared<StringObject>(out);
    });

    // upper/lower: Turkish-aware (i→İ, ı→I, ş→Ş ...)
    def("upper", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("upper() expects a string", line);
        }
        return std::make_shared<StringObject>(
            caseConvert(static_cast<StringObject*>(args[0].get())->value, true));
    });
    def("lower", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("lower() expects a string", line);
        }
        return std::make_shared<StringObject>(
            caseConvert(static_cast<StringObject*>(args[0].get())->value, false));
    });

    // trim(text): strips leading/trailing whitespace
    def("trim", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("trim() expects a string", line);
        }
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::make_shared<StringObject>("");
        size_t b = s.find_last_not_of(" \t\r\n");
        return std::make_shared<StringObject>(s.substr(a, b - a + 1));
    });

    // replace(text, old, new): replaces all occurrences
    def("replace", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3) return argCountError("replace", "3", args.size(), line);
        for (const auto& a : args) {
            if (a->type() != ObjectType::STRING) return makeError("replace(text, old, new) expects three strings", line);
        }
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        const std::string& from = static_cast<StringObject*>(args[1].get())->value;
        const std::string& to = static_cast<StringObject*>(args[2].get())->value;
        if (from.empty()) return makeError("replace() cannot work with an empty 'old' text", line);
        std::string out;
        size_t pos = 0;
        while (true) {
            size_t found = s.find(from, pos);
            if (found == std::string::npos) { out += s.substr(pos); break; }
            out += s.substr(pos, found - pos);
            out += to;
            pos = found + from.size();
        }
        return std::make_shared<StringObject>(out);
    });

    def("starts_with", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::STRING || args[1]->type() != ObjectType::STRING) {
            return makeError("starts_with(text, prefix) expects two strings", line);
        }
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        const std::string& p = static_cast<StringObject*>(args[1].get())->value;
        return boolObj(s.size() >= p.size() && s.compare(0, p.size(), p) == 0);
    });

    def("ends_with", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::STRING || args[1]->type() != ObjectType::STRING) {
            return makeError("ends_with(text, suffix) expects two strings", line);
        }
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        const std::string& p = static_cast<StringObject*>(args[1].get())->value;
        return boolObj(s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0);
    });

    // chr(code): converts a Unicode code point to a one-character string; ord() is the inverse
    def("chr", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::INTEGER) {
            return makeError("chr() expects an integer (Unicode code point)", line);
        }
        long long cp = static_cast<IntegerObject*>(args[0].get())->value;
        if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return makeError("chr() invalid code point: " + std::to_string(cp), line);
        }
        std::string out;
        unsigned int u = (unsigned int)cp;
        if (u < 0x80) out += (char)u;
        else if (u < 0x800) {
            out += (char)(0xC0 | (u >> 6));
            out += (char)(0x80 | (u & 0x3F));
        } else if (u < 0x10000) {
            out += (char)(0xE0 | (u >> 12));
            out += (char)(0x80 | ((u >> 6) & 0x3F));
            out += (char)(0x80 | (u & 0x3F));
        } else {
            out += (char)(0xF0 | (u >> 18));
            out += (char)(0x80 | ((u >> 12) & 0x3F));
            out += (char)(0x80 | ((u >> 6) & 0x3F));
            out += (char)(0x80 | (u & 0x3F));
        }
        return std::make_shared<StringObject>(out);
    });

    // ord(char): Unicode code point of a single character
    def("ord", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("ord() expects a single-character string", line);
        }
        const std::string& sv = static_cast<StringObject*>(args[0].get())->value;
        if (sv.empty() || utf8Length(sv) != 1) {
            return makeError("ord() expects exactly ONE character, got " +
                             std::to_string(utf8Length(sv)) + "", line);
        }
        unsigned int cp = 0;
        unsigned char c0 = (unsigned char)sv[0];
        int len = utf8CharLen(c0);
        if (len == 1) cp = c0;
        else if (len == 2) cp = ((c0 & 0x1F) << 6) | ((unsigned char)sv[1] & 0x3F);
        else if (len == 3) cp = ((c0 & 0x0F) << 12) | (((unsigned char)sv[1] & 0x3F) << 6) |
                                ((unsigned char)sv[2] & 0x3F);
        else cp = ((c0 & 0x07) << 18) | (((unsigned char)sv[1] & 0x3F) << 12) |
                  (((unsigned char)sv[2] & 0x3F) << 6) | ((unsigned char)sv[3] & 0x3F);
        return std::make_shared<IntegerObject>((long long)cp);
    });

    // count(text, needle): number of (non-overlapping) occurrences
    def("count", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::STRING ||
            args[1]->type() != ObjectType::STRING) {
            return makeError("count(text, needle) expects two strings", line);
        }
        const std::string& hay = static_cast<StringObject*>(args[0].get())->value;
        const std::string& needle = static_cast<StringObject*>(args[1].get())->value;
        if (needle.empty()) return makeError("count() cannot search for an empty text", line);
        long long n = 0;
        size_t pos = 0;
        while ((pos = hay.find(needle, pos)) != std::string::npos) {
            n++;
            pos += needle.size();
        }
        return std::make_shared<IntegerObject>(n);
    });

    // pad_start / pad_end: pads text to a target length with a fill character (UTF-8 aware)
    auto padFn = [](bool atStart) {
        return [atStart](const Args& args, int line, const CallFn&) -> ObjPtr {
            std::string fname = atStart ? "pad_start" : "pad_end";
            if (args.size() < 2 || args.size() > 3 ||
                args[0]->type() != ObjectType::STRING ||
                args[1]->type() != ObjectType::INTEGER) {
                return makeError(fname + "(text, length[, fill]) expected", line);
            }
            std::string dolgu = " ";
            if (args.size() == 3) {
                if (args[2]->type() != ObjectType::STRING ||
                    utf8Length(static_cast<StringObject*>(args[2].get())->value) != 1) {
                    return makeError(fname + "() fill must be a single-character string", line);
                }
                dolgu = static_cast<StringObject*>(args[2].get())->value;
            }
            const std::string& sv = static_cast<StringObject*>(args[0].get())->value;
            long long hedef = static_cast<IntegerObject*>(args[1].get())->value;
            long long mevcut = utf8Length(sv);
            if (hedef <= mevcut) return args[0];
            std::string pad;
            for (long long i = mevcut; i < hedef; ++i) pad += dolgu;
            return std::make_shared<StringObject>(atStart ? pad + sv : sv + pad);
        };
    };
    def("pad_start", padFn(true));
    def("pad_end", padFn(false));

    // fixed(number, digits): fixed-decimal string — for UI: fixed(3.14159, 2) -> "3.14"
    def("fixed", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !isNumeric(args[0]) ||
            args[1]->type() != ObjectType::INTEGER) {
            return makeError("fixed(number, digits) expects a number and an integer", line);
        }
        long long digits = static_cast<IntegerObject*>(args[1].get())->value;
        if (digits < 0 || digits > 17) {
            return makeError("fixed() digit count must be within 0-17", line);
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", (int)digits, asDouble(args[0]));
        return std::make_shared<StringObject>(buf);
    });

    // lines(text): splits into a list of lines (handles \r\n and \n)
    def("lines", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING)
            return makeError("lines(text) expects a string", line);
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        auto out = std::make_shared<ListObject>();
        std::string cur;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n') { out->elements.push_back(std::make_shared<StringObject>(cur)); cur.clear(); }
            else if (s[i] == '\r') { /* skip */ }
            else cur += s[i];
        }
        out->elements.push_back(std::make_shared<StringObject>(cur));
        return out;
    });
    // capitalize(text): first letter upper, rest lower (Turkish-aware)
    def("capitalize", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING)
            return makeError("capitalize(text) expects a string", line);
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        if (s.empty()) return std::make_shared<StringObject>("");
        int len = utf8CharLen((unsigned char)s[0]);
        std::string first = caseConvert(s.substr(0, len), true);
        std::string rest = caseConvert(s.substr(len), false);
        return std::make_shared<StringObject>(first + rest);
    });
    auto charClass = [](const std::string& name, int (*pred)(int)) {
        return [name, pred](const Args& args, int line, const CallFn&) -> ObjPtr {
            if (args.size() != 1 || args[0]->type() != ObjectType::STRING)
                return makeError(name + "() expects a string", line);
            const std::string& s = static_cast<StringObject*>(args[0].get())->value;
            if (s.empty()) return FALSE_OBJ;
            for (unsigned char c : s) { if (c >= 0x80) return FALSE_OBJ; if (!pred(c)) return FALSE_OBJ; }
            return TRUE_OBJ;
        };
    };
    def("is_digit", charClass("is_digit", [](int c){ return std::isdigit(c); }));
    def("is_alpha", charClass("is_alpha", [](int c){ return std::isalpha(c); }));
    def("is_space", charClass("is_space", [](int c){ return std::isspace(c); }));
    // repeat_text(text, n): the string repeated n times
    def("repeat_text", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::STRING || args[1]->type() != ObjectType::INTEGER)
            return makeError("repeat_text(text, n) expects a string and an integer", line);
        long long n = static_cast<IntegerObject*>(args[1].get())->value;
        if (n < 0) n = 0;
        if (n > 1000000) return makeError("repeat_text() count too large", line);
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        std::string out; out.reserve(s.size() * (size_t)n);
        for (long long i = 0; i < n; ++i) out += s;
        return std::make_shared<StringObject>(out);
    });

    mod->frozen = true;
    mod->moduleName = "strings";
    cached = mod;
    return mod;
}

// ===== file module =====

// Quotes a CSV field when needed (contains separator, quote, or newline)
inline std::string csvQuote(const std::string& field, const std::string& sep) {
    bool needs = field.find(sep) != std::string::npos ||
                 field.find('"') != std::string::npos ||
                 field.find('\n') != std::string::npos ||
                 field.find('\r') != std::string::npos;
    if (!needs) return field;
    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

inline ObjPtr makeFileModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = std::make_shared<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), std::make_shared<BuiltinObject>(name, std::move(fn)));
    };

    auto pathArg = [](const Args& args, const std::string& fname, int line,
                      std::string& out) -> ObjPtr {
        if (args.empty() || args[0]->type() != ObjectType::STRING) {
            return makeError(fname + "() expects a string path as its first argument", line);
        }
        out = static_cast<StringObject*>(args[0].get())->value;
        return nullptr;
    };

    // exists(path): does the file or directory exist?
    def("exists", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("exists", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "exists", line, path)) return err;
        std::error_code ec;
        return boolObj(std::filesystem::exists(path, ec));
    });

    // read_text(path): reads the file as a string
    def("read_text", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().read, "file read", "--allow-read");
        if (args.size() != 1) return argCountError("read_text", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "read_text", line, path)) return err;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot open file: " + path, line);
        std::stringstream buf;
        buf << f.rdbuf();
        return std::make_shared<StringObject>(buf.str());
    });

    // write_text(path, text): writes the string to a file (overwrites)
    def("write_text", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().write, "file write", "--allow-write");
        if (args.size() != 2 || args[1]->type() != ObjectType::STRING) {
            return makeError("write_text(path, text) expects two strings", line);
        }
        std::string path;
        if (auto err = pathArg(args, "write_text", line, path)) return err;
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot write file: " + path, line);
        f << static_cast<StringObject*>(args[1].get())->value;
        return NULL_OBJ_;
    });

    // append_text(path, text): appends to the file (for log files)
    def("append_text", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().write, "file write", "--allow-write");
        if (args.size() != 2 || args[1]->type() != ObjectType::STRING) {
            return makeError("append_text(path, text) expects two strings", line);
        }
        std::string path;
        if (auto err = pathArg(args, "append_text", line, path)) return err;
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f.is_open()) return makeError("cannot write file: " + path, line);
        f << static_cast<StringObject*>(args[1].get())->value;
        return NULL_OBJ_;
    });

    // read_lines(path): list of lines (line endings stripped)
    def("read_lines", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().read, "file read", "--allow-read");
        if (args.size() != 1) return argCountError("read_lines", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "read_lines", line, path)) return err;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot open file: " + path, line);
        auto list = std::make_shared<ListObject>();
        std::string ln;
        while (std::getline(f, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            list->elements.push_back(std::make_shared<StringObject>(ln));
        }
        return list;
    });

    // delete_file(path): deletes the file; returns success
    def("delete_file", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().write, "file delete", "--allow-write");
        if (args.size() != 1) return argCountError("delete_file", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "delete_file", line, path)) return err;
        std::error_code ec;
        return boolObj(std::filesystem::remove(path, ec));
    });

    // make_dir(path): creates directories (nested included)
    def("make_dir", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().write, "directory create", "--allow-write");
        if (args.size() != 1) return argCountError("make_dir", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "make_dir", line, path)) return err;
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return boolObj(!ec);
    });

    // list_dir(path): ALPHABETICAL list of names in a directory
    def("list_dir", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().read, "directory listing", "--allow-read");
        if (args.size() != 1) return argCountError("list_dir", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "list_dir", line, path)) return err;
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec)) {
            return makeError("not a directory or does not exist: " + path, line);
        }
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            names.push_back(entry.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        auto list = std::make_shared<ListObject>();
        for (const auto& n : names) {
            list->elements.push_back(std::make_shared<StringObject>(n));
        }
        return list;
    });

    // save_data(path, value): saves the value as JSON (game save system)
    def("save_data", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().write, "file write", "--allow-write");
        if (args.size() != 2) return argCountError("save_data", "2", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "save_data", line, path)) return err;
        std::string json, err;
        if (!jsonWrite(args[1], json, err, 0)) {
            return makeError("save_data() failed: " + err, line);
        }
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot write file: " + path, line);
        f << json << "\n";
        return NULL_OBJ_;
    });

    // load_data(path): parses a JSON file into a Lume value
    def("load_data", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().read, "file read", "--allow-read");
        if (args.size() != 1) return argCountError("load_data", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "load_data", line, path)) return err;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot open file: " + path, line);
        std::stringstream buf;
        buf << f.rdbuf();
        std::string content = buf.str();

        JsonParser parser(content);
        ObjPtr result;
        if (!parser.parseValue(result, 0)) {
            return makeError("load_data() could not parse JSON: " + parser.err + " [" + path + "]", line);
        }
        parser.skipWs();
        if (parser.i != content.size()) {
            return makeError("load_data() trailing data after JSON [" + path + "]", line);
        }
        return result;
    });

    // read_bytes(path): reads a binary file as a list of ints (0-255).
    // For game assets, .bin saves, etc. A dedicated bytes type arrives with the VM;
    // until then files larger than 10 MB are rejected to avoid allocation storms.
    def("read_bytes", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().read, "file read", "--allow-read");
        if (args.size() != 1) return argCountError("read_bytes", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "read_bytes", line, path)) return err;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return makeError("cannot open file: " + path, line);
        auto size = f.tellg();
        if (size > 10 * 1024 * 1024) {
            return makeError("read_bytes() file too large (10 MB limit): " + path, line);
        }
        f.seekg(0);
        std::vector<char> buf((size_t)size);
        f.read(buf.data(), size);
        auto list = std::make_shared<ListObject>();
        list->elements.reserve((size_t)size);
        for (char c : buf) {
            list->elements.push_back(
                std::make_shared<IntegerObject>((long long)(unsigned char)c));
        }
        return list;
    });

    // write_bytes(path, list): writes a list of ints (0-255) as a binary file
    def("write_bytes", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().write, "file write", "--allow-write");
        if (args.size() != 2) return argCountError("write_bytes", "2", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "write_bytes", line, path)) return err;
        if (args[1]->type() != ObjectType::LIST) {
            return makeError("write_bytes(path, list) expects a list of bytes", line);
        }
        std::string buf;
        const auto& els = static_cast<ListObject*>(args[1].get())->elements;
        buf.reserve(els.size());
        for (const auto& e : els) {
            if (e->type() != ObjectType::INTEGER) {
                return makeError("write_bytes() elements must be integers (0-255)", line);
            }
            long long v = static_cast<IntegerObject*>(e.get())->value;
            if (v < 0 || v > 255) {
                return makeError("write_bytes() byte out of range: " + std::to_string(v), line);
            }
            buf += (char)(unsigned char)v;
        }
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot write file: " + path, line);
        f << buf;
        return NULL_OBJ_;
    });

    // read_csv(path[, separator]): list of rows; each row is a list of string cells.
    // Supports quoted fields: "a,b", "" escaping, newlines inside quotes.
    def("read_csv", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().read, "file read", "--allow-read");
        if (args.empty() || args.size() > 2) return argCountError("read_csv", "1-2", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "read_csv", line, path)) return err;
        std::string sep = ",";
        if (args.size() == 2) {
            if (args[1]->type() != ObjectType::STRING ||
                static_cast<StringObject*>(args[1].get())->value.size() != 1) {
                return makeError("read_csv() separator must be a single-character string", line);
            }
            sep = static_cast<StringObject*>(args[1].get())->value;
        }
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot open file: " + path, line);
        std::stringstream buf;
        buf << f.rdbuf();
        std::string content = buf.str();

        auto rows = std::make_shared<ListObject>();
        auto row = std::make_shared<ListObject>();
        std::string cell;
        bool inQuotes = false;
        size_t i = 0;
        char sc = sep[0];

        auto pushCell = [&]() {
            row->elements.push_back(std::make_shared<StringObject>(cell));
            cell.clear();
        };
        auto pushRow = [&]() {
            pushCell();
            rows->elements.push_back(row);
            row = std::make_shared<ListObject>();
        };

        while (i < content.size()) {
            char c = content[i];
            if (inQuotes) {
                if (c == '"') {
                    if (i + 1 < content.size() && content[i + 1] == '"') {
                        cell += '"'; // "" kaçışı
                        i += 2;
                        continue;
                    }
                    inQuotes = false;
                    i++;
                    continue;
                }
                cell += c;
                i++;
                continue;
            }
            if (c == '"' && cell.empty()) { inQuotes = true; i++; continue; }
            if (c == sc) { pushCell(); i++; continue; }
            if (c == '\r') { i++; continue; }
            if (c == '\n') { pushRow(); i++; continue; }
            cell += c;
            i++;
        }
        // Last row (if the file does not end with \n)
        if (!cell.empty() || !row->elements.empty()) pushRow();
        return rows;
    });

    // write_csv(path, rows[, separator]): writes a list of lists as CSV
    def("write_csv", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().write, "file write", "--allow-write");
        if (args.size() < 2 || args.size() > 3) return argCountError("write_csv", "2-3", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "write_csv", line, path)) return err;
        if (args[1]->type() != ObjectType::LIST) {
            return makeError("write_csv() expects a list of rows (list of lists)", line);
        }
        std::string sep = ",";
        if (args.size() == 3) {
            if (args[2]->type() != ObjectType::STRING ||
                static_cast<StringObject*>(args[2].get())->value.size() != 1) {
                return makeError("write_csv() separator must be a single-character string", line);
            }
            sep = static_cast<StringObject*>(args[2].get())->value;
        }
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return makeError("cannot write file: " + path, line);
        for (const auto& rowObj : static_cast<ListObject*>(args[1].get())->elements) {
            if (rowObj->type() != ObjectType::LIST) {
                return makeError("write_csv() every row must be a list", line);
            }
            const auto& cells = static_cast<ListObject*>(rowObj.get())->elements;
            for (size_t i = 0; i < cells.size(); ++i) {
                if (i > 0) f << sep;
                f << csvQuote(cells[i]->inspect(), sep);
            }
            f << "\n";
        }
        return NULL_OBJ_;
    });

    // size(path): byte size of a file
    def("size", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("size", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "size", line, path)) return err;
        std::error_code ec;
        auto sz = std::filesystem::file_size(path, ec);
        if (ec) return makeError("cannot stat file: " + path, line);
        return std::make_shared<IntegerObject>((long long)sz);
    });
    def("is_dir", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("is_dir", "1", args.size(), line);
        std::string path;
        if (auto err = pathArg(args, "is_dir", line, path)) return err;
        std::error_code ec;
        return boolObj(std::filesystem::is_directory(path, ec));
    });
    def("copy_file", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().write, "file write", "--allow-write");
        if (args.size() != 2 || args[1]->type() != ObjectType::STRING)
            return makeError("copy_file(from, to) expects two strings", line);
        std::string from;
        if (auto err = pathArg(args, "copy_file", line, from)) return err;
        std::string to = static_cast<StringObject*>(args[1].get())->value;
        std::error_code ec;
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
        return boolObj(!ec);
    });
    def("rename", [pathArg](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().write, "file rename", "--allow-write");
        if (args.size() != 2 || args[1]->type() != ObjectType::STRING)
            return makeError("rename(from, to) expects two strings", line);
        std::string from;
        if (auto err = pathArg(args, "rename", line, from)) return err;
        std::string to = static_cast<StringObject*>(args[1].get())->value;
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        return boolObj(!ec);
    });

    mod->frozen = true;
    mod->moduleName = "file";
    cached = mod;
    return mod;
}

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

    auto mod = std::make_shared<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), std::make_shared<BuiltinObject>(name, std::move(fn)));
    };

    // env(name): reads an environment variable; null if unset
    def("env", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().env, "environment read", "--allow-env");
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("env(name) expects a string", line);
        }
        const char* v = std::getenv(static_cast<StringObject*>(args[0].get())->value.c_str());
        if (v == nullptr) return NULL_OBJ_;
        return std::make_shared<StringObject>(v);
    });

    // set_env(name, value): sets an environment variable for this process
    def("set_env", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        LUME_GATE(perms().env, "environment write", "--allow-env");
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
        return std::make_shared<StringObject>("windows");
#elif defined(__APPLE__)
        return std::make_shared<StringObject>("macos");
#else
        return std::make_shared<StringObject>("linux");
#endif
    });

    // cwd(): current working directory
    def("cwd", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("cwd", "0", args.size(), line);
        std::error_code ec;
        auto p = std::filesystem::current_path(ec);
        if (ec) return makeError("cannot read the working directory", line);
        return std::make_shared<StringObject>(p.string());
    });

    // args(): extra command-line arguments passed after the script path
    def("args", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("args", "0", args.size(), line);
        auto list = std::make_shared<ListObject>();
        for (const auto& a : scriptArgs()) {
            list->elements.push_back(std::make_shared<StringObject>(a));
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
        return std::make_shared<StringObject>(p.string());
    });

    def("basename", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) return makeError("basename(path) expects a string", line);
        return std::make_shared<StringObject>(std::filesystem::path(static_cast<StringObject*>(args[0].get())->value).filename().string());
    });
    def("dirname", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) return makeError("dirname(path) expects a string", line);
        return std::make_shared<StringObject>(std::filesystem::path(static_cast<StringObject*>(args[0].get())->value).parent_path().string());
    });
    def("extension", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) return makeError("extension(path) expects a string", line);
        return std::make_shared<StringObject>(std::filesystem::path(static_cast<StringObject*>(args[0].get())->value).extension().string());
    });
    def("temp_dir", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("temp_dir", "0", args.size(), line);
        std::error_code ec;
        return std::make_shared<StringObject>(std::filesystem::temp_directory_path(ec).string());
    });
    def("home_dir", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("home_dir", "0", args.size(), line);
        const char* h = std::getenv("HOME");
        if (!h) h = std::getenv("USERPROFILE");
        return h ? (ObjPtr)std::make_shared<StringObject>(h) : NULL_OBJ_;
    });

    mod->frozen = true;
    mod->moduleName = "os";
    cached = mod;
    return mod;
}

// ===== time module =====
inline ObjPtr makeTimeModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;
    auto mod = std::make_shared<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), std::make_shared<BuiltinObject>(name, std::move(fn)));
    };

    // now(): seconds since the Unix epoch (float)
    def("now", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("now", "0", args.size(), line);
        auto d = std::chrono::system_clock::now().time_since_epoch();
        return std::make_shared<FloatObject>(std::chrono::duration<double>(d).count());
    });
    def("now_ms", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("now_ms", "0", args.size(), line);
        auto d = std::chrono::system_clock::now().time_since_epoch();
        return std::make_shared<IntegerObject>(
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
        auto m = std::make_shared<MapObject>();
        m->set(strKey("year"),    std::make_shared<IntegerObject>(tm.tm_year + 1900));
        m->set(strKey("month"),   std::make_shared<IntegerObject>(tm.tm_mon + 1));
        m->set(strKey("day"),     std::make_shared<IntegerObject>(tm.tm_mday));
        m->set(strKey("hour"),    std::make_shared<IntegerObject>(tm.tm_hour));
        m->set(strKey("minute"),  std::make_shared<IntegerObject>(tm.tm_min));
        m->set(strKey("second"),  std::make_shared<IntegerObject>(tm.tm_sec));
        m->set(strKey("weekday"), std::make_shared<IntegerObject>(tm.tm_wday));
        return m;
    });
    // date_text([unix]): "YYYY-MM-DD HH:MM:SS"
    def("date_text", [toTm](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() > 1) return argCountError("date_text", "0-1", args.size(), line);
        std::tm tm{};
        if (!toTm(args, tm)) return makeError("date_text([unix]) expects a number", line);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return std::make_shared<StringObject>(buf);
    });

    mod->frozen = true;
    mod->moduleName = "time";
    cached = mod;
    return mod;
}

// ===== canvas module (terminal renderer) =====
// A canvas is a map holding width/height plus a code-point buffer and a color
// buffer. Playable ASCII/roguelike graphics before the real engine exists.
namespace CanvasImpl {
    inline const std::unordered_map<std::string, std::string>& colorCodes() {
        static const std::unordered_map<std::string, std::string> m = {
            {"black","30"},{"red","31"},{"green","32"},{"yellow","33"},
            {"blue","34"},{"magenta","35"},{"cyan","36"},{"white","37"},{"gray","90"}
        };
        return m;
    }
    inline MapObject* asCanvas(const ObjPtr& v) {
        if (v->type() != ObjectType::MAP) return nullptr;
        auto* m = static_cast<MapObject*>(v.get());
        if (m->get(std::make_shared<StringObject>("__canvas__")) == nullptr) return nullptr;
        return m;
    }
    inline long long dim(MapObject* c, const char* k) {
        auto v = c->get(std::make_shared<StringObject>(k));
        return v && v->type() == ObjectType::INTEGER ? static_cast<IntegerObject*>(v.get())->value : 0;
    }
    inline ListObject* cbuf(MapObject* c) {
        return static_cast<ListObject*>(c->get(std::make_shared<StringObject>("buf")).get());
    }
    inline ListObject* ccol(MapObject* c) {
        return static_cast<ListObject*>(c->get(std::make_shared<StringObject>("col")).get());
    }
}

inline ObjPtr makeCanvasModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;
    auto mod = std::make_shared<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), std::make_shared<BuiltinObject>(name, std::move(fn)));
    };
    using CanvasImpl::asCanvas;

    // create(w, h): a blank canvas filled with spaces
    def("create", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::INTEGER || args[1]->type() != ObjectType::INTEGER)
            return makeError("create(w, h) expects two integers", line);
        long long w = static_cast<IntegerObject*>(args[0].get())->value;
        long long h = static_cast<IntegerObject*>(args[1].get())->value;
        if (w <= 0 || h <= 0 || w > 1000 || h > 1000) return makeError("create() size out of range (1..1000)", line);
        auto c = std::make_shared<MapObject>();
        c->set(strKey("__canvas__"), TRUE_OBJ);
        c->set(strKey("w"), std::make_shared<IntegerObject>(w));
        c->set(strKey("h"), std::make_shared<IntegerObject>(h));
        auto b = std::make_shared<ListObject>();
        auto col = std::make_shared<ListObject>();
        for (long long i = 0; i < w * h; ++i) {
            b->elements.push_back(std::make_shared<StringObject>(" "));
            col->elements.push_back(std::make_shared<StringObject>("white"));
        }
        c->set(strKey("buf"), b);
        c->set(strKey("col"), col);
        return c;
    });
    auto putCell = [](MapObject* c, long long x, long long y, const std::string& ch, const std::string& color) {
        long long w = CanvasImpl::dim(c, "w"), h = CanvasImpl::dim(c, "h");
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        long long idx = y * w + x;
        static_cast<StringObject*>(CanvasImpl::cbuf(c)->elements[idx].get())->value = ch;
        static_cast<StringObject*>(CanvasImpl::ccol(c)->elements[idx].get())->value = color;
    };
    // put(canvas, x, y, char[, color])
    def("put", [putCell](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 4 || args.size() > 5) return argCountError("put", "4-5", args.size(), line);
        auto* c = asCanvas(args[0]);
        if (!c || args[1]->type() != ObjectType::INTEGER || args[2]->type() != ObjectType::INTEGER ||
            args[3]->type() != ObjectType::STRING)
            return makeError("put(canvas, x, y, char[, color]) - bad arguments", line);
        std::string color = args.size() == 5 && args[4]->type() == ObjectType::STRING
            ? static_cast<StringObject*>(args[4].get())->value : "white";
        std::string ch = static_cast<StringObject*>(args[3].get())->value;
        if (utf8Length(ch) != 1) return makeError("put() char must be exactly one character", line);
        putCell(c, static_cast<IntegerObject*>(args[1].get())->value,
                   static_cast<IntegerObject*>(args[2].get())->value, ch, color);
        return args[0];
    });
    // write(canvas, x, y, text[, color]): draws a string left to right
    def("write", [putCell](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 4 || args.size() > 5) return argCountError("write", "4-5", args.size(), line);
        auto* c = asCanvas(args[0]);
        if (!c || args[1]->type() != ObjectType::INTEGER || args[2]->type() != ObjectType::INTEGER ||
            args[3]->type() != ObjectType::STRING)
            return makeError("write(canvas, x, y, text[, color]) - bad arguments", line);
        std::string color = args.size() == 5 && args[4]->type() == ObjectType::STRING
            ? static_cast<StringObject*>(args[4].get())->value : "white";
        const std::string& text = static_cast<StringObject*>(args[3].get())->value;
        long long x = static_cast<IntegerObject*>(args[1].get())->value;
        long long y = static_cast<IntegerObject*>(args[2].get())->value;
        size_t i = 0; long long dx = 0;
        while (i < text.size()) {
            int len = utf8CharLen((unsigned char)text[i]);
            putCell(c, x + dx, y, text.substr(i, len), color);
            i += len; dx++;
        }
        return args[0];
    });
    // fill(canvas, char[, color]): fills the whole canvas
    def("fill", [putCell](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 2 || args.size() > 3) return argCountError("fill", "2-3", args.size(), line);
        auto* c = asCanvas(args[0]);
        if (!c || args[1]->type() != ObjectType::STRING) return makeError("fill(canvas, char[, color]) - bad arguments", line);
        std::string color = args.size() == 3 && args[2]->type() == ObjectType::STRING
            ? static_cast<StringObject*>(args[2].get())->value : "white";
        std::string ch = static_cast<StringObject*>(args[1].get())->value;
        long long w = CanvasImpl::dim(c, "w"), h = CanvasImpl::dim(c, "h");
        for (long long y = 0; y < h; ++y) for (long long x = 0; x < w; ++x) putCell(c, x, y, ch, color);
        return args[0];
    });
    // rect / fill_rect(canvas, x, y, w, h, char[, color])
    auto rectFn = [putCell](bool filled) {
        return [putCell, filled](const Args& args, int line, const CallFn&) -> ObjPtr {
            std::string fname = filled ? "fill_rect" : "rect";
            if (args.size() < 6 || args.size() > 7) return argCountError(fname, "6-7", args.size(), line);
            auto* c = asCanvas(args[0]);
            for (int i = 1; i <= 4; ++i) if (args[i]->type() != ObjectType::INTEGER) return makeError(fname + "() coords must be integers", line);
            if (!c || args[5]->type() != ObjectType::STRING) return makeError(fname + "() - bad arguments", line);
            std::string color = args.size() == 7 && args[6]->type() == ObjectType::STRING
                ? static_cast<StringObject*>(args[6].get())->value : "white";
            std::string ch = static_cast<StringObject*>(args[5].get())->value;
            long long x = static_cast<IntegerObject*>(args[1].get())->value;
            long long y = static_cast<IntegerObject*>(args[2].get())->value;
            long long rw = static_cast<IntegerObject*>(args[3].get())->value;
            long long rh = static_cast<IntegerObject*>(args[4].get())->value;
            for (long long j = 0; j < rh; ++j) for (long long i = 0; i < rw; ++i) {
                bool edge = (i == 0 || j == 0 || i == rw - 1 || j == rh - 1);
                if (filled || edge) putCell(c, x + i, y + j, ch, color);
            }
            return args[0];
        };
    };
    def("rect", rectFn(false));
    def("fill_rect", rectFn(true));
    // line(canvas, x1, y1, x2, y2, char[, color]): Bresenham
    def("line", [putCell](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 6 || args.size() > 7) return argCountError("line", "6-7", args.size(), line);
        auto* c = asCanvas(args[0]);
        for (int i = 1; i <= 4; ++i) if (args[i]->type() != ObjectType::INTEGER) return makeError("line() coords must be integers", line);
        if (!c || args[5]->type() != ObjectType::STRING) return makeError("line() - bad arguments", line);
        std::string color = args.size() == 7 && args[6]->type() == ObjectType::STRING
            ? static_cast<StringObject*>(args[6].get())->value : "white";
        std::string ch = static_cast<StringObject*>(args[5].get())->value;
        long long x1 = static_cast<IntegerObject*>(args[1].get())->value, y1 = static_cast<IntegerObject*>(args[2].get())->value;
        long long x2 = static_cast<IntegerObject*>(args[3].get())->value, y2 = static_cast<IntegerObject*>(args[4].get())->value;
        long long dx = std::llabs(x2 - x1), dy = -std::llabs(y2 - y1);
        long long sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1, err = dx + dy;
        while (true) {
            putCell(c, x1, y1, ch, color);
            if (x1 == x2 && y1 == y2) break;
            long long e2 = 2 * err;
            if (e2 >= dy) { err += dy; x1 += sx; }
            if (e2 <= dx) { err += dx; y1 += sy; }
        }
        return args[0];
    });
    // circle(canvas, cx, cy, r, char[, color]): midpoint outline
    def("circle", [putCell](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 5 || args.size() > 6) return argCountError("circle", "5-6", args.size(), line);
        auto* c = asCanvas(args[0]);
        for (int i = 1; i <= 3; ++i) if (args[i]->type() != ObjectType::INTEGER) return makeError("circle() coords must be integers", line);
        if (!c || args[4]->type() != ObjectType::STRING) return makeError("circle() - bad arguments", line);
        std::string color = args.size() == 6 && args[5]->type() == ObjectType::STRING
            ? static_cast<StringObject*>(args[5].get())->value : "white";
        std::string ch = static_cast<StringObject*>(args[4].get())->value;
        long long cx = static_cast<IntegerObject*>(args[1].get())->value, cy = static_cast<IntegerObject*>(args[2].get())->value;
        long long r = static_cast<IntegerObject*>(args[3].get())->value;
        long long x = r, y = 0, err = 1 - r;
        while (x >= y) {
            putCell(c, cx + x, cy + y, ch, color); putCell(c, cx - x, cy + y, ch, color);
            putCell(c, cx + x, cy - y, ch, color); putCell(c, cx - x, cy - y, ch, color);
            putCell(c, cx + y, cy + x, ch, color); putCell(c, cx - y, cy + x, ch, color);
            putCell(c, cx + y, cy - x, ch, color); putCell(c, cx - y, cy - x, ch, color);
            y++;
            if (err < 0) err += 2 * y + 1;
            else { x--; err += 2 * (y - x) + 1; }
        }
        return args[0];
    });
    // render(canvas): prints the canvas with ANSI colors (plain when not a TTY)
    def("render", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("render", "1", args.size(), line);
        auto* c = asCanvas(args[0]);
        if (!c) return makeError("render() expects a canvas", line);
        long long w = CanvasImpl::dim(c, "w"), h = CanvasImpl::dim(c, "h");
        auto* b = CanvasImpl::cbuf(c); auto* col = CanvasImpl::ccol(c);
        bool tty = Color::stdoutIsTTY();
        std::string out;
        for (long long y = 0; y < h; ++y) {
            for (long long x = 0; x < w; ++x) {
                long long i = y * w + x;
                const std::string& ch = static_cast<StringObject*>(b->elements[i].get())->value;
                if (tty) {
                    const std::string& cn = static_cast<StringObject*>(col->elements[i].get())->value;
                    auto it = CanvasImpl::colorCodes().find(cn);
                    if (it != CanvasImpl::colorCodes().end()) out += "\033[1;" + it->second + "m" + ch + "\033[0m";
                    else out += ch;
                } else out += ch;
            }
            out += "\n";
        }
        std::cout << out;
        return NULL_OBJ_;
    });
    // clear_screen(): moves the cursor home + clears (ANSI)
    def("clear_screen", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("clear_screen", "0", args.size(), line);
        if (Color::stdoutIsTTY()) std::cout << "\033[2J\033[H";
        return NULL_OBJ_;
    });

    mod->frozen = true;
    mod->moduleName = "canvas";
    cached = mod;
    return mod;
}


// Built-in module registry: use <name> looks up here
// ===== net module — blocking TCP/UDP sockets (OS syscalls, zero deps) =====
// A socket is an int handle. Every function validates its arguments and returns
// a catchable error object on any failure, so a misused socket never crashes the
// VM. Blocking calls honor net.set_timeout so a server loop can't hang forever.
#ifdef _WIN32
  inline void netCloseFd(long long fd) { closesocket((SOCKET)fd); }
  inline void netInit() {
      static bool done = false;
      if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = true; }
  }
#else
  inline void netCloseFd(long long fd) { ::close((int)fd); }
  inline void netInit() {}
#endif

inline ObjPtr netErr(const std::string& what, int line) {
    return makeError("net." + what + " failed: " + std::string(std::strerror(errno)), line);
}
inline bool netIsInt(const ObjPtr& o) { return o->type() == ObjectType::INTEGER; }
inline long long netInt(const ObjPtr& o) { return static_cast<IntegerObject*>(o.get())->value; }
inline bool netIsStr(const ObjPtr& o) { return o->type() == ObjectType::STRING; }
inline const std::string& netStr(const ObjPtr& o) { return static_cast<StringObject*>(o.get())->value; }

inline ObjPtr makeNetModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;
    netInit();
    auto mod = std::make_shared<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), std::make_shared<BuiltinObject>(name, std::move(fn)));
    };

    // tcp_listen(port) -> server socket handle
    def("tcp_listen", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 1 || !netIsInt(a[0]))
            return makeError("net.tcp_listen(port) expects an integer port", line);
        long long port = netInt(a[0]);
        if (port < 1 || port > 65535) return makeError("net.tcp_listen: port out of range (1-65535)", line);
        int fd = (int)::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return netErr("tcp_listen", line);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t)port);
        if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { auto e = netErr("tcp_listen (bind)", line); netCloseFd(fd); return e; }
        if (::listen(fd, 16) < 0) { auto e = netErr("tcp_listen (listen)", line); netCloseFd(fd); return e; }
        return std::make_shared<IntegerObject>(fd);
    });

    // tcp_accept(server) -> client socket handle (blocks until a client connects)
    def("tcp_accept", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 1 || !netIsInt(a[0]))
            return makeError("net.tcp_accept(server) expects a socket handle", line);
        int c = (int)::accept((int)netInt(a[0]), nullptr, nullptr);
        if (c < 0) return netErr("tcp_accept", line);
        return std::make_shared<IntegerObject>(c);
    });

    // tcp_connect(host, port) -> client socket handle
    def("tcp_connect", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 2 || !netIsStr(a[0]) || !netIsInt(a[1]))
            return makeError("net.tcp_connect(host, port) expects (string, int)", line);
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        std::string portStr = std::to_string(netInt(a[1]));
        if (::getaddrinfo(netStr(a[0]).c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
            return makeError("net.tcp_connect: cannot resolve host '" + netStr(a[0]) + "'", line);
        int fd = (int)::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) { freeaddrinfo(res); return netErr("tcp_connect", line); }
        if (::connect(fd, res->ai_addr, (int)res->ai_addrlen) < 0) {
            auto e = netErr("tcp_connect", line); freeaddrinfo(res); netCloseFd(fd); return e;
        }
        freeaddrinfo(res);
        return std::make_shared<IntegerObject>(fd);
    });

    // send(sock, text) -> bytes sent
    def("send", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 2 || !netIsInt(a[0]) || !netIsStr(a[1]))
            return makeError("net.send(sock, text) expects (socket, string)", line);
        const std::string& s = netStr(a[1]);
        long long n = ::send((int)netInt(a[0]), s.data(), (int)s.size(), 0);
        if (n < 0) return netErr("send", line);
        return std::make_shared<IntegerObject>(n);
    });

    // recv(sock, [maxbytes=4096]) -> string ("" when the peer closed)
    def("recv", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.empty() || a.size() > 2 || !netIsInt(a[0]))
            return makeError("net.recv(sock, [maxbytes]) expects a socket handle", line);
        long long maxb = (a.size() == 2 && netIsInt(a[1])) ? netInt(a[1]) : 4096;
        if (maxb < 1 || maxb > 16 * 1024 * 1024) maxb = 4096;
        std::string buf((size_t)maxb, '\0');
        long long n = ::recv((int)netInt(a[0]), &buf[0], (int)maxb, 0);
        if (n < 0) return netErr("recv", line);
        buf.resize((size_t)n);
        return std::make_shared<StringObject>(buf);
    });

    // set_timeout(sock, seconds) -> null. 0 = blocking forever. Prevents hangs.
    def("set_timeout", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 2 || !netIsInt(a[0]) || (a[1]->type() != ObjectType::FLOAT && a[1]->type() != ObjectType::INTEGER))
            return makeError("net.set_timeout(sock, seconds) expects (socket, number)", line);
        double sec = a[1]->type() == ObjectType::FLOAT ? static_cast<FloatObject*>(a[1].get())->value
                                                       : (double)netInt(a[1]);
        int fd = (int)netInt(a[0]);
#ifdef _WIN32
        DWORD ms = (DWORD)(sec * 1000.0);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
#else
        timeval tv{}; tv.tv_sec = (long)sec; tv.tv_usec = (long)((sec - (long)sec) * 1e6);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        return NULL_OBJ_;
    });

    // close(sock) -> null
    def("close", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 1 || !netIsInt(a[0]))
            return makeError("net.close(sock) expects a socket handle", line);
        netCloseFd(netInt(a[0]));
        return NULL_OBJ_;
    });

    // udp_socket() -> handle
    def("udp_socket", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (!a.empty()) return argCountError("udp_socket", "0", a.size(), line);
        int fd = (int)::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return netErr("udp_socket", line);
        return std::make_shared<IntegerObject>(fd);
    });
    // udp_bind(sock, port) -> null
    def("udp_bind", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 2 || !netIsInt(a[0]) || !netIsInt(a[1]))
            return makeError("net.udp_bind(sock, port) expects (socket, int)", line);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t)netInt(a[1]));
        if (::bind((int)netInt(a[0]), (sockaddr*)&addr, sizeof(addr)) < 0) return netErr("udp_bind", line);
        return NULL_OBJ_;
    });
    // udp_send(sock, host, port, text) -> bytes sent
    def("udp_send", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 4 || !netIsInt(a[0]) || !netIsStr(a[1]) || !netIsInt(a[2]) || !netIsStr(a[3]))
            return makeError("net.udp_send(sock, host, port, text) expects (socket, string, int, string)", line);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)netInt(a[2]));
        if (::inet_pton(AF_INET, netStr(a[1]).c_str(), &addr.sin_addr) != 1)
            return makeError("net.udp_send: invalid host '" + netStr(a[1]) + "'", line);
        const std::string& s = netStr(a[3]);
        long long n = ::sendto((int)netInt(a[0]), s.data(), (int)s.size(), 0, (sockaddr*)&addr, sizeof(addr));
        if (n < 0) return netErr("udp_send", line);
        return std::make_shared<IntegerObject>(n);
    });
    // udp_recv(sock, [maxbytes]) -> string
    def("udp_recv", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.empty() || a.size() > 2 || !netIsInt(a[0]))
            return makeError("net.udp_recv(sock, [maxbytes]) expects a socket handle", line);
        long long maxb = (a.size() == 2 && netIsInt(a[1])) ? netInt(a[1]) : 4096;
        if (maxb < 1 || maxb > 16 * 1024 * 1024) maxb = 4096;
        std::string buf((size_t)maxb, '\0');
        long long n = ::recvfrom((int)netInt(a[0]), &buf[0], (int)maxb, 0, nullptr, nullptr);
        if (n < 0) return netErr("udp_recv", line);
        buf.resize((size_t)n);
        return std::make_shared<StringObject>(buf);
    });

    mod->frozen = true;
    mod->moduleName = "net";
    cached = mod;
    return mod;
}

inline ObjPtr getBuiltinModule(const std::string& name) {
    if (name == "net") return makeNetModule();
    if (name == "math") return makeMathModule();
    if (name == "game") return makeGameModule();
    if (name == "strings") return makeTextModule();
    if (name == "file") return makeFileModule();
    if (name == "os")   return makeOsModule();
    if (name == "time") return makeTimeModule();
    if (name == "canvas") return makeCanvasModule();
    return nullptr;
}

inline std::string builtinModuleList() {
    return "math, game, strings, file, os, time, canvas, net";
}

} // namespace StdLib
} // namespace Lume

#endif // STDLIB_HPP
