#ifndef LOVAX_MODULE_STRINGS_HPP
#define LOVAX_MODULE_STRINGS_HPP

#include "common.hpp"

namespace Lovax {
namespace StdLib {

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

// ===== strings module =====

inline ObjPtr makeTextModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    // split(text, separator): splits into parts; "" splits into characters
    def("split", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("split", "2", args.size(), line);
        if (args[0]->type() != ObjectType::STRING || args[1]->type() != ObjectType::STRING) {
            return makeError("split(text, separator) expects two strings", line);
        }
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        const std::string& sep = static_cast<StringObject*>(args[1].get())->value;
        auto list = makeObj<ListObject>();
        if (sep.empty()) {
            size_t i = 0;
            while (i < s.size()) {
                int len = utf8CharLen(static_cast<unsigned char>(s[i]));
                list->elements.push_back(makeObj<StringObject>(s.substr(i, len)));
                i += len;
            }
            return list;
        }
        size_t pos = 0;
        while (true) {
            size_t found = s.find(sep, pos);
            if (found == std::string::npos) {
                list->elements.push_back(makeObj<StringObject>(s.substr(pos)));
                break;
            }
            list->elements.push_back(makeObj<StringObject>(s.substr(pos, found - pos)));
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
        return makeObj<StringObject>(out);
    });

    // upper/lower: Turkish-aware (i→İ, ı→I, ş→Ş ...)
    def("upper", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("upper() expects a string", line);
        }
        return makeObj<StringObject>(
            caseConvert(static_cast<StringObject*>(args[0].get())->value, true));
    });
    def("lower", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("lower() expects a string", line);
        }
        return makeObj<StringObject>(
            caseConvert(static_cast<StringObject*>(args[0].get())->value, false));
    });

    // trim(text): strips leading/trailing whitespace
    def("trim", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("trim() expects a string", line);
        }
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return makeObj<StringObject>("");
        size_t b = s.find_last_not_of(" \t\r\n");
        return makeObj<StringObject>(s.substr(a, b - a + 1));
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
        return makeObj<StringObject>(out);
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
        return makeObj<StringObject>(out);
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
        return makeObj<IntegerObject>((long long)cp);
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
        return makeObj<IntegerObject>(n);
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
            return makeObj<StringObject>(atStart ? pad + sv : sv + pad);
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
        return makeObj<StringObject>(buf);
    });

    // lines(text): splits into a list of lines (handles \r\n and \n)
    def("lines", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING)
            return makeError("lines(text) expects a string", line);
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        auto out = makeObj<ListObject>();
        std::string cur;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n') { out->elements.push_back(makeObj<StringObject>(cur)); cur.clear(); }
            else if (s[i] == '\r') { /* skip */ }
            else cur += s[i];
        }
        out->elements.push_back(makeObj<StringObject>(cur));
        return out;
    });
    // capitalize(text): first letter upper, rest lower (Turkish-aware)
    def("capitalize", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING)
            return makeError("capitalize(text) expects a string", line);
        const std::string& s = static_cast<StringObject*>(args[0].get())->value;
        if (s.empty()) return makeObj<StringObject>("");
        int len = utf8CharLen((unsigned char)s[0]);
        std::string first = caseConvert(s.substr(0, len), true);
        std::string rest = caseConvert(s.substr(len), false);
        return makeObj<StringObject>(first + rest);
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
        return makeObj<StringObject>(out);
    });

    // partition(text, sep) -> (before, sep, after); sep absent -> (text, "", "")
    def("partition", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::STRING ||
            args[1]->type() != ObjectType::STRING) {
            return makeError("partition(text, sep) expects two strings", line);
        }
        const std::string& t = static_cast<StringObject*>(args[0].get())->value;
        const std::string& sep = static_cast<StringObject*>(args[1].get())->value;
        auto out = makeObj<TupleObject>();
        GcRoot _gr(out.get());
        size_t pos = sep.empty() ? std::string::npos : t.find(sep);
        if (pos == std::string::npos) {
            out->elements.push_back(makeObj<StringObject>(t));
            out->elements.push_back(makeObj<StringObject>(""));
            out->elements.push_back(makeObj<StringObject>(""));
        } else {
            out->elements.push_back(makeObj<StringObject>(t.substr(0, pos)));
            out->elements.push_back(makeObj<StringObject>(sep));
            out->elements.push_back(makeObj<StringObject>(t.substr(pos + sep.size())));
        }
        return out;
    });

    // fmt(x, spec): ".2f" fixed decimals | "05d" zero-padded int | "x" hex
    def("fmt", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !isNumeric(args[0]) || args[1]->type() != ObjectType::STRING) {
            return makeError("fmt(x, spec) expects a number and a spec string (\".2f\", \"05d\", \"x\")", line);
        }
        const std::string& spec = static_cast<StringObject*>(args[1].get())->value;
        char buf[64];
        if (spec.size() >= 2 && spec[0] == '.' && spec.back() == 'f') {
            int prec = 0;
            for (size_t k = 1; k + 1 < spec.size(); ++k) {
                if (spec[k] < '0' || spec[k] > '9') return makeError("fmt() bad spec: " + spec, line);
                prec = prec * 10 + (spec[k] - '0');
            }
            if (prec > 30) return makeError("fmt() precision too large", line);
            std::snprintf(buf, sizeof(buf), "%.*f", prec, asDouble(args[0]));
            return makeObj<StringObject>(buf);
        }
        if (spec.size() >= 2 && spec[0] == '0' && spec.back() == 'd' &&
            args[0]->type() == ObjectType::INTEGER) {
            int width = 0;
            for (size_t k = 1; k + 1 < spec.size(); ++k) {
                if (spec[k] < '0' || spec[k] > '9') return makeError("fmt() bad spec: " + spec, line);
                width = width * 10 + (spec[k] - '0');
            }
            if (width > 60) return makeError("fmt() width too large", line);
            std::snprintf(buf, sizeof(buf), "%0*lld", width,
                          static_cast<IntegerObject*>(args[0].get())->value);
            return makeObj<StringObject>(buf);
        }
        if (spec == "x" && args[0]->type() == ObjectType::INTEGER) {
            std::snprintf(buf, sizeof(buf), "%llx",
                          (unsigned long long)static_cast<IntegerObject*>(args[0].get())->value);
            return makeObj<StringObject>(buf);
        }
        return makeError("fmt() bad spec: " + spec + " (\".Nf\", \"0Nd\", \"x\")", line);
    });

    mod->frozen = true;
    mod->moduleName = "strings";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}


} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_STRINGS_HPP
