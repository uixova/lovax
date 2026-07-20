#ifndef LOVAX_MODULES_COMMON_HPP
#define LOVAX_MODULES_COMMON_HPP

#include <memory>
#include <vector>
#include <string>
#include <cmath>
#include <random>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <limits>
#include <cctype>
#include <ctime>
#include <unordered_map>
#include <cstring>
#include "../object/object.hpp"
#include "../object/environment.hpp"

// Shared plumbing for every Lovax builtin/stdlib module: argument helpers, the
// deterministic RNG, deep clone, module-map construction, capability gates and
// the JSON core (used by the file module today, the json module later).

namespace Lovax {
namespace Builtins {

using Args = std::vector<Ref<Object>>;
using ObjPtr = Ref<Object>;
using CallFn = BuiltinObject::CallFn;

inline ObjPtr argCountError(const std::string& fname, const std::string& expected, size_t got, int line) {
    return makeError(fname + "() expects " + expected + " argument(s), got " + std::to_string(got) + "", line);
}

inline bool isNumeric(const ObjPtr& o) {
    return o->type() == ObjectType::INTEGER || o->type() == ObjectType::FLOAT;
}

inline double asDouble(const ObjPtr& o) {
    if (o->type() == ObjectType::INTEGER) return (double)static_cast<IntegerObject*>(o.get())->value;
    return static_cast<FloatObject*>(o.get())->value;
}

// RNG: made deterministic with seed(n) (game replays/tests)
inline std::mt19937_64& rng() {
    static std::mt19937_64 gen(std::random_device{}());
    return gen;
}

// Deterministic uniform sampling built directly on the mt19937_64 stream.
// std::uniform_*_distribution results differ across standard-library
// implementations — that would break the cross-platform replay promise.
// These are the only two primitives every random feature builds on.
inline double rngU53() {           // float in [0, 1), 53-bit precision
    return (double)(rng()() >> 11) * (1.0 / 9007199254740992.0);
}
inline long long rngBounded(unsigned long long n) {   // int in [0, n)
    // Lemire multiply-shift: deterministic, bias < n / 2^64 (negligible)
    return (long long)(((unsigned __int128)rng()() * n) >> 64);
}

// Deep copy for clone(): nested lists/maps become fully independent
inline ObjPtr deepClone(const ObjPtr& v, int line, int depth) {
    if (depth > 100) return makeError("clone() structure too deep (nesting limit 100)", line);
    if (v->type() == ObjectType::LIST) {
        auto out = makeObj<ListObject>();
        GcRoot _gr56(out.get());
        for (const auto& e : static_cast<ListObject*>(v.get())->elements) {
            auto c = deepClone(e, line, depth + 1);
            if (isError(c)) return c;
            out->elements.push_back(c);
        }
        return out;
    }
    if (v->type() == ObjectType::MAP) {
        auto out = makeObj<MapObject>();
        GcRoot _gr65(out.get());
        for (const auto& e : static_cast<MapObject*>(v.get())->entries) {
            auto c = deepClone(e.second, line, depth + 1);
            if (isError(c)) return c;
            out->set(e.first, c);
        }
        return out;
    }
    if (v->type() == ObjectType::SET) {
        auto* src = static_cast<SetObject*>(v.get());
        auto out = makeObj<SetObject>();
        GcRoot _grcs(out.get());
        for (const auto& e : src->entries) out->set(e.first, TRUE_OBJ);
        return out;
    }
    if (v->type() == ObjectType::STRUCT) {
        auto* src = static_cast<StructInstanceObject*>(v.get());
        auto out = makeObj<StructInstanceObject>();
        GcRoot _grs(out.get());
        out->shape = src->shape;
        out->slots.reserve(src->slots.size());
        for (const auto& s : src->slots) {
            auto c = deepClone(s, line, depth + 1);
            if (isError(c)) return c;
            out->slots.push_back(c);
        }
        return out;
    }
    return v;
}

} // namespace Builtins

namespace StdLib {

using Args = Builtins::Args;
using ObjPtr = Builtins::ObjPtr;
using CallFn = BuiltinObject::CallFn;
using Builtins::argCountError;
using Builtins::isNumeric;
using Builtins::asDouble;
using Builtins::rng;
using Builtins::rngU53;
using Builtins::rngBounded;

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
            if (isFloat) out = makeObj<FloatObject>(std::stod(numStr));
            else out = makeObj<IntegerObject>(std::stoll(numStr));
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
        out = makeObj<StringObject>(str);
        return true;
    }

    bool parseArray(ObjPtr& out, int depth) {
        i++; // '['
        auto list = makeObj<ListObject>();
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
        auto map = makeObj<MapObject>();
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

// ===== Module construction helpers =====

inline Ref<StringObject> strKey(const std::string& k) {
    return makeObj<StringObject>(k);
}

// ===== Capability permissions (RFC-015, sandbox / --allow-* flags) =====
// Default: everything allowed (a script you wrote, you trust). `lovax --sandbox`
// flips these to deny, and `--allow-net/read/write/env/run` grant back exactly
// what a program needs. This is the runtime half of the malicious-package
// defense (the other half is version pinning, RFC-007 phase 2): even an
// installed package physically cannot open a socket or read your files unless
// you allowed it.
struct Perms {
    bool net = true, read = true, write = true, env = true, run = true;
    // ffi: loading/calling untrusted native code (v1.0 FFI bridge, RFC-025).
    // Host-registered engine natives are trusted and bypass this; only
    // dynamically loaded plugins / ffi paths consult it.
    bool ffi = true;
};
inline Perms& perms() { static Perms p; return p; }

// Returns a catchable permission error when the capability is denied, else null.
inline ObjPtr permGate(bool allowed, const char* what, const char* flag, int line) {
    if (allowed) return nullptr;
    return makeError(std::string("permission denied: ") + what + " requires " + flag +
                     " (or --allow-all)", line);
}
#define LOVAX_GATE(cond, what, flag) \
    do { if (auto _pe = permGate((cond), (what), (flag), line)) return _pe; } while (0)

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULES_COMMON_HPP
