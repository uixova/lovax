#ifndef OBJECT_HPP
#define OBJECT_HPP

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <sstream>

namespace Lume {

// Forward declarations to avoid circular dependencies
class Environment;
class BlockStatement;

enum class ObjectType {
    INTEGER,
    FLOAT,
    BOOLEAN,
    NULL_OBJ,
    STRING,
    LIST,
    MAP,
    RANGE,
    FUNCTION,
    BUILTIN,
    SIGNAL,
    RETURN_VALUE,
    BREAK_SIGNAL,
    CONTINUE_SIGNAL,
    ERROR
};

class Object {
public:
    virtual ~Object() = default;
    virtual ObjectType type() const = 0;
    virtual std::string inspect() const = 0;
};

// Human-friendly float printing: 6.28 (not 6.280000), 2.0 (not 2 — keep the type visible)
inline std::string formatFloat(double v) {
    std::ostringstream ss;
    ss.precision(14);
    ss << v;
    std::string s = ss.str();
    // "inf", "nan" gibi özel durumlar olduğu gibi kalır
    if (s.find('.') == std::string::npos &&
        s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos &&
        s.find("nan") == std::string::npos) {
        s += ".0";
    }
    return s;
}

class IntegerObject : public Object {
public:
    long long value;
    IntegerObject(long long val) : value(val) {}
    ObjectType type() const override { return ObjectType::INTEGER; }
    std::string inspect() const override { return std::to_string(value); }
};

class FloatObject : public Object {
public:
    double value;
    FloatObject(double val) : value(val) {}
    ObjectType type() const override { return ObjectType::FLOAT; }
    std::string inspect() const override { return formatFloat(value); }
};

class StringObject : public Object {
public:
    std::string value;
    StringObject(const std::string& val) : value(val) {}
    ObjectType type() const override { return ObjectType::STRING; }
    std::string inspect() const override { return value; } // Prints the raw text on the console
};

class BooleanObject : public Object {
public:
    bool value;
    BooleanObject(bool val) : value(val) {}
    ObjectType type() const override { return ObjectType::BOOLEAN; }
    std::string inspect() const override { return value ? "true" : "false"; }
};

class NullObject : public Object {
public:
    ObjectType type() const override { return ObjectType::NULL_OBJ; }
    std::string inspect() const override { return "null"; }
};

// ===== Shared constants (singletons) =====
// Single instances so true/false/null never allocate new objects.
inline const std::shared_ptr<BooleanObject> TRUE_OBJ  = std::make_shared<BooleanObject>(true);
inline const std::shared_ptr<BooleanObject> FALSE_OBJ = std::make_shared<BooleanObject>(false);
inline const std::shared_ptr<NullObject>    NULL_OBJ_ = std::make_shared<NullObject>();

inline std::shared_ptr<BooleanObject> boolObj(bool b) { return b ? TRUE_OBJ : FALSE_OBJ; }

// Shows strings quoted inside nested structures: say ["a"] -> ["a"]
inline std::string inspectQuoted(const std::shared_ptr<Object>& obj) {
    if (obj->type() == ObjectType::STRING) {
        return "\"" + obj->inspect() + "\"";
    }
    return obj->inspect();
}

// List object -> [1, 2, "three"]
class ListObject : public Object {
public:
    std::vector<std::shared_ptr<Object>> elements;
    ObjectType type() const override { return ObjectType::LIST; }
    std::string inspect() const override {
        std::string out = "[";
        for (size_t i = 0; i < elements.size(); ++i) {
            if (i > 0) out += ", ";
            out += inspectQuoted(elements[i]);
        }
        return out + "]";
    }
};

// Map object -> {"name": "Kai"} — insertion order preserved (deterministic output)
// Modules are frozen maps too: use math -> math.lerp (RFC-006)
class MapObject : public Object {
public:
    std::vector<std::pair<std::shared_ptr<Object>, std::shared_ptr<Object>>> entries;
    std::unordered_map<std::string, size_t> index; // tagged key -> index into entries
    bool frozen = false;          // true: immutable (modules)
    std::string moduleName;       // module name if this is a module (for errors + inspect)

    // Converts keys to type-tagged strings ("s:name", "i:42", "b:1")
    static std::string keyTag(const std::shared_ptr<Object>& key) {
        switch (key->type()) {
            case ObjectType::STRING:  return "s:" + key->inspect();
            case ObjectType::INTEGER: return "i:" + key->inspect();
            case ObjectType::BOOLEAN: return "b:" + key->inspect();
            default: return ""; // unsupported key type (caller raises the error)
        }
    }

    void set(const std::shared_ptr<Object>& key, const std::shared_ptr<Object>& val) {
        std::string tag = keyTag(key);
        auto it = index.find(tag);
        if (it != index.end()) {
            entries[it->second].second = val;
        } else {
            index[tag] = entries.size();
            entries.push_back({key, val});
        }
    }

    std::shared_ptr<Object> get(const std::shared_ptr<Object>& key) const {
        auto it = index.find(keyTag(key));
        if (it == index.end()) return nullptr;
        return entries[it->second].second;
    }

    bool remove(const std::shared_ptr<Object>& key) {
        std::string tag = keyTag(key);
        auto it = index.find(tag);
        if (it == index.end()) return false;
        size_t pos = it->second;
        entries.erase(entries.begin() + pos);
        index.erase(it);
        // Shift every index after the removed slot
        for (auto& kv : index) {
            if (kv.second > pos) kv.second--;
        }
        return true;
    }

    ObjectType type() const override { return ObjectType::MAP; }
    std::string inspect() const override {
        if (!moduleName.empty()) {
            return "<modül " + moduleName + ": " + std::to_string(entries.size()) + " öğe>";
        }
        std::string out = "{";
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i > 0) out += ", ";
            out += inspectQuoted(entries[i].first) + ": " + inspectQuoted(entries[i].second);
        }
        return out + "}";
    }
};

// Lazy range object -> range(0, 10): never allocates a million-element list
class RangeObject : public Object {
public:
    long long start, end, step;
    RangeObject(long long s, long long e, long long st) : start(s), end(e), step(st) {}

    long long length() const {
        if (step > 0) {
            if (end <= start) return 0;
            return (end - start + step - 1) / step;
        }
        if (end >= start) return 0;
        return (start - end + (-step) - 1) / (-step);
    }

    ObjectType type() const override { return ObjectType::RANGE; }
    std::string inspect() const override {
        std::string out = "range(" + std::to_string(start) + ", " + std::to_string(end);
        if (step != 1) out += ", " + std::to_string(step);
        return out + ")";
    }
};

// Forward declaration: default parameter expressions point into the AST
class Expression;

// Runtime object for functions
class FunctionObject : public Object {
public:
    std::vector<std::string> parameters;      // Parameter name list
    // Default value expressions (aligned with parameters; nullptr when absent).
    // Evaluated at call time in the closure environment.
    std::vector<const Expression*> defaults;
    const BlockStatement* body;               // Pointer to the function body in the AST
    std::shared_ptr<Environment> env;         // Environment where the function was born (closure)
    std::string name;                         // Function name for error messages

    FunctionObject(const std::vector<std::string>& params,
                   const std::vector<const Expression*>& defs,
                   const BlockStatement* b,
                   std::shared_ptr<Environment> e, const std::string& n = "")
        : parameters(params), defaults(defs), body(b), env(e), name(n) {}

    // Number of required (defaultless) parameters
    size_t requiredCount() const {
        size_t n = 0;
        for (const auto* d : defaults) {
            if (d == nullptr) n++;
        }
        return n;
    }

    ObjectType type() const override { return ObjectType::FUNCTION; }
    std::string inspect() const override { return "fn " + (name.empty() ? "?" : name) + "(...)"; }
};

// Builtin function object implemented in C++.
// CallFn: lets a builtin invoke Lume functions (for each/filter/emit) — RFC-005
class BuiltinObject : public Object {
public:
    using CallFn = std::function<std::shared_ptr<Object>(
        const std::shared_ptr<Object>& fn,
        const std::vector<std::shared_ptr<Object>>& args,
        int line)>;

    using BuiltinFn = std::function<std::shared_ptr<Object>(
        const std::vector<std::shared_ptr<Object>>&, int line, const CallFn&)>;

    std::string name;
    BuiltinFn fn;

    BuiltinObject(const std::string& n, BuiltinFn f) : name(n), fn(std::move(f)) {}
    ObjectType type() const override { return ObjectType::BUILTIN; }
    std::string inspect() const override { return "builtin " + name + "(...)"; }
};

// Signal object: holds a list of listeners — signal()/connect()/emit() (RFC-005)
class SignalObject : public Object {
public:
    std::vector<std::shared_ptr<Object>> listeners;

    ObjectType type() const override { return ObjectType::SIGNAL; }
    std::string inspect() const override {
        return "signal(" + std::to_string(listeners.size()) + " dinleyici)";
    }
};

class ReturnValueObject : public Object {
public:
    std::shared_ptr<Object> value;
    ReturnValueObject(std::shared_ptr<Object> val) : value(val) {}
    ObjectType type() const override { return ObjectType::RETURN_VALUE; }
    std::string inspect() const override { return value->inspect(); }
};

// break/continue signals: bubble up from the loop body; the loop catches them
class BreakSignalObject : public Object {
public:
    int srcLine;
    BreakSignalObject(int l) : srcLine(l) {}
    ObjectType type() const override { return ObjectType::BREAK_SIGNAL; }
    std::string inspect() const override { return "break"; }
};

class ContinueSignalObject : public Object {
public:
    int srcLine;
    ContinueSignalObject(int l) : srcLine(l) {}
    ObjectType type() const override { return ObjectType::CONTINUE_SIGNAL; }
    std::string inspect() const override { return "continue"; }
};

// Runtime error: propagates as a value; the first error stops execution
class ErrorObject : public Object {
public:
    std::string message;
    int srcLine;
    ErrorObject(const std::string& msg, int l = 0) : message(msg), srcLine(l) {}
    ObjectType type() const override { return ObjectType::ERROR; }
    std::string inspect() const override {
        if (srcLine > 0) {
            return "[Çalışma Hatası] satır " + std::to_string(srcLine) + ": " + message;
        }
        return "[Çalışma Hatası] " + message;
    }
};

inline bool isError(const std::shared_ptr<Object>& obj) {
    return obj != nullptr && obj->type() == ObjectType::ERROR;
}

inline std::shared_ptr<ErrorObject> makeError(const std::string& msg, int line = 0) {
    return std::make_shared<ErrorObject>(msg, line);
}

// Returns the type name (for the kind() builtin and error messages)
inline std::string typeName(ObjectType t) {
    switch (t) {
        case ObjectType::INTEGER:      return "int";
        case ObjectType::FLOAT:        return "float";
        case ObjectType::BOOLEAN:      return "bool";
        case ObjectType::NULL_OBJ:     return "null";
        case ObjectType::STRING:       return "string";
        case ObjectType::LIST:         return "list";
        case ObjectType::MAP:          return "map";
        case ObjectType::RANGE:        return "range";
        case ObjectType::FUNCTION:     return "fn";
        case ObjectType::BUILTIN:      return "builtin";
        case ObjectType::SIGNAL:       return "signal";
        case ObjectType::RETURN_VALUE: return "return";
        case ObjectType::BREAK_SIGNAL: return "break";
        case ObjectType::CONTINUE_SIGNAL: return "continue";
        case ObjectType::ERROR:        return "error";
    }
    return "?";
}

// ===== Shared semantic helpers =====
// The evaluator AND builtins (contains, find, sort_by, filter) use the same rules.

// Deep equality: numbers compare across types (5 == 5.0), lists/maps by content, functions by identity
inline bool objectEquals(const std::shared_ptr<Object>& a, const std::shared_ptr<Object>& b) {
    bool aNum = (a->type() == ObjectType::INTEGER || a->type() == ObjectType::FLOAT);
    bool bNum = (b->type() == ObjectType::INTEGER || b->type() == ObjectType::FLOAT);
    if (aNum && bNum) {
        double av = (a->type() == ObjectType::INTEGER)
                    ? (double)static_cast<IntegerObject*>(a.get())->value
                    : static_cast<FloatObject*>(a.get())->value;
        double bv = (b->type() == ObjectType::INTEGER)
                    ? (double)static_cast<IntegerObject*>(b.get())->value
                    : static_cast<FloatObject*>(b.get())->value;
        return av == bv;
    }

    if (a->type() != b->type()) return false;

    switch (a->type()) {
        case ObjectType::NULL_OBJ: return true;
        case ObjectType::BOOLEAN:
            return static_cast<BooleanObject*>(a.get())->value ==
                   static_cast<BooleanObject*>(b.get())->value;
        case ObjectType::STRING:
            return static_cast<StringObject*>(a.get())->value ==
                   static_cast<StringObject*>(b.get())->value;
        case ObjectType::LIST: {
            auto* la = static_cast<ListObject*>(a.get());
            auto* lb = static_cast<ListObject*>(b.get());
            if (la->elements.size() != lb->elements.size()) return false;
            for (size_t i = 0; i < la->elements.size(); ++i) {
                if (!objectEquals(la->elements[i], lb->elements[i])) return false;
            }
            return true;
        }
        case ObjectType::MAP: {
            auto* ma = static_cast<MapObject*>(a.get());
            auto* mb = static_cast<MapObject*>(b.get());
            if (ma->entries.size() != mb->entries.size()) return false;
            for (const auto& e : ma->entries) {
                auto bv = mb->get(e.first);
                if (bv == nullptr || !objectEquals(e.second, bv)) return false;
            }
            return true;
        }
        case ObjectType::RANGE: {
            auto* ra = static_cast<RangeObject*>(a.get());
            auto* rb = static_cast<RangeObject*>(b.get());
            return ra->start == rb->start && ra->end == rb->end && ra->step == rb->step;
        }
        default:
            return a.get() == b.get(); // functions/signals: identity comparison
    }
}

// Truthiness rules (Python model): null, false, 0, 0.0, "", [], {} -> false
inline bool objectTruthy(const std::shared_ptr<Object>& obj) {
    switch (obj->type()) {
        case ObjectType::NULL_OBJ: return false;
        case ObjectType::BOOLEAN:  return static_cast<BooleanObject*>(obj.get())->value;
        case ObjectType::INTEGER:  return static_cast<IntegerObject*>(obj.get())->value != 0;
        case ObjectType::FLOAT:    return static_cast<FloatObject*>(obj.get())->value != 0.0;
        case ObjectType::STRING:   return !static_cast<StringObject*>(obj.get())->value.empty();
        case ObjectType::LIST:     return !static_cast<ListObject*>(obj.get())->elements.empty();
        case ObjectType::MAP:      return !static_cast<MapObject*>(obj.get())->entries.empty();
        case ObjectType::RANGE:    return static_cast<RangeObject*>(obj.get())->length() > 0;
        default: return true;
    }
}

// ===== UTF-8 helpers =====
// Turkish characters are multi-byte, so len/indexing counts code points.

inline int utf8CharLen(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1; // malformed byte: count as one byte to guarantee progress
}

inline long long utf8Length(const std::string& s) {
    long long count = 0;
    size_t i = 0;
    while (i < s.size()) {
        i += utf8CharLen(static_cast<unsigned char>(s[i]));
        count++;
    }
    return count;
}

// Lists the byte offsets of code points (for slice/reverse/find)
inline std::vector<size_t> utf8Offsets(const std::string& s) {
    std::vector<size_t> offs;
    size_t i = 0;
    while (i < s.size()) {
        offs.push_back(i);
        i += utf8CharLen(static_cast<unsigned char>(s[i]));
    }
    offs.push_back(s.size()); // end boundary
    return offs;
}

// Returns the i-th code point as a one-character string ("" = out of range)
inline std::string utf8At(const std::string& s, long long idx) {
    size_t i = 0;
    long long count = 0;
    while (i < s.size()) {
        int len = utf8CharLen(static_cast<unsigned char>(s[i]));
        if (count == idx) return s.substr(i, len);
        i += len;
        count++;
    }
    return "";
}

} // namespace Lume

#endif // OBJECT_HPP
