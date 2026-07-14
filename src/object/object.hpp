#ifndef OBJECT_HPP
#define OBJECT_HPP

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <sstream>

namespace Lume {


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
    COROUTINE,
    RETURN_VALUE,
    BREAK_SIGNAL,
    CONTINUE_SIGNAL,
    ERROR
};

class Object {
public:
    // Non-virtual type tag: hot paths read this field instead of paying for a
    // virtual call. type() stays for readability and returns the same tag.
    const ObjectType tag;

    // GC bookkeeping (RFC-013). gcNext links every heap object; gcMarked is the
    // mark bit. Set by the tracing collector only — never on the hot path.
    Object* gcNext = nullptr;
    bool gcMarked = false;

    explicit Object(ObjectType t) : tag(t) {}
    virtual ~Object() = default;
    ObjectType type() const { return tag; }
    virtual std::string inspect() const = 0;

    // Marks this object's outgoing references (children). Leaf objects (numbers,
    // strings, bools) have none. Container/closure types override this.
    virtual void gcMark() {}
};

} // namespace Lume — reopened below; gc.hpp needs Object complete
#include "../vm/gc.hpp"
namespace Lume {

// Marks an object gray: sets the bit and queues it for child-scanning. Safe on
// null and on already-marked objects (the graph may have cycles).
inline void gcMarkObject(Object* o) {
    if (o == nullptr || o->gcMarked) return;
    o->gcMarked = true;
    Heap::get().worklist.push_back(o);
}

// One full stop-the-world mark-sweep cycle.
inline void gcCollect() {
    Heap& h = Heap::get();
    if (h.collecting) return;
    h.collecting = true;

    // 1. Mark roots: permanent (singletons/modules), the runtime's VM roots, and
    //    any C++-held temporaries.
    for (Object* r : h.permanentRoots) gcMarkObject(r);
    if (h.markRoots) h.markRoots();
    for (Object* r : h.tempRoots) gcMarkObject(r);

    // 2. Trace: drain the gray worklist, marking each object's children.
    while (!h.worklist.empty()) {
        Object* o = h.worklist.back();
        h.worklist.pop_back();
        o->gcMark();
    }

    // 3. Sweep: free the unmarked, clear the mark bit on survivors.
    Object** link = &h.first;
    while (*link) {
        Object* o = *link;
        if (o->gcMarked) {
            o->gcMarked = false;
            link = &o->gcNext;
        } else {
            *link = o->gcNext;
            delete o;
        }
    }

    h.nextGC = h.bytesAllocated * 2;
    if (h.nextGC < 1024 * 1024) h.nextGC = 1024 * 1024;
    h.collecting = false;
}

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
    IntegerObject(long long val) : Object(ObjectType::INTEGER), value(val) {}
    std::string inspect() const override { return std::to_string(value); }
};

class FloatObject : public Object {
public:
    double value;
    FloatObject(double val) : Object(ObjectType::FLOAT), value(val) {}
    std::string inspect() const override { return formatFloat(value); }
};

class StringObject : public Object {
public:
    std::string value;
    // Cached UTF-8 code-point count; -1 = not computed. Strings are immutable
    // except for the VM's in-place append (ADD_INPLACE), which resets it.
    mutable long long lenCache = -1;
    StringObject(const std::string& val) : Object(ObjectType::STRING), value(val) {}
    std::string inspect() const override { return value; } // Prints the raw text on the console
};

class BooleanObject : public Object {
public:
    bool value;
    BooleanObject(bool val) : Object(ObjectType::BOOLEAN), value(val) {}
    std::string inspect() const override { return value ? "true" : "false"; }
};

class NullObject : public Object {
public:
    NullObject() : Object(ObjectType::NULL_OBJ) {}
    std::string inspect() const override { return "null"; }
};

// ===== Shared constants (singletons) =====
// Single instances so true/false/null never allocate new objects.
inline const Ref<BooleanObject> TRUE_OBJ  = makeObj<BooleanObject>(true);
inline const Ref<BooleanObject> FALSE_OBJ = makeObj<BooleanObject>(false);
inline const Ref<NullObject>    NULL_OBJ_ = makeObj<NullObject>();

inline Ref<BooleanObject> boolObj(bool b) { return b ? TRUE_OBJ : FALSE_OBJ; }

// Shows strings quoted inside nested structures: say ["a"] -> ["a"]
inline std::string inspectQuoted(const Ref<Object>& obj) {
    if (obj->type() == ObjectType::STRING) {
        return "\"" + obj->inspect() + "\"";
    }
    return obj->inspect();
}

// List object -> [1, 2, "three"]
class ListObject : public Object {
public:
    ListObject() : Object(ObjectType::LIST) {}
    std::vector<Ref<Object>> elements;
    void gcMark() override { for (auto& e : elements) gcMarkObject(e.get()); }
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
    MapObject() : Object(ObjectType::MAP) {}
    std::vector<std::pair<Ref<Object>, Ref<Object>>> entries;
    // Typed indexes: no key-tag string is ever built for a lookup. String keys
    // hash the raw bytes, int/bool keys never touch a string at all.
    std::unordered_map<std::string, size_t> strIndex;
    std::unordered_map<long long, size_t> intIndex;
    static constexpr size_t NPOS = (size_t)-1;
    size_t boolIndex[2] = {NPOS, NPOS};
    bool frozen = false;          // true: immutable (modules)
    std::string moduleName;       // module name if this is a module (for errors + inspect)

    void gcMark() override {
        for (auto& e : entries) { gcMarkObject(e.first.get()); gcMarkObject(e.second.get()); }
    }

    void copyIndexFrom(const MapObject& src) {
        strIndex = src.strIndex;
        intIndex = src.intIndex;
        boolIndex[0] = src.boolIndex[0];
        boolIndex[1] = src.boolIndex[1];
    }
    void clearIndex() {
        strIndex.clear();
        intIndex.clear();
        boolIndex[0] = boolIndex[1] = NPOS;
    }

    // Zero-allocation lookups by native key
    Ref<Object> getStr(const std::string& k) const {
        auto it = strIndex.find(k);
        return it == strIndex.end() ? nullptr : entries[it->second].second;
    }
    size_t findStr(const std::string& k) const {
        auto it = strIndex.find(k);
        return it == strIndex.end() ? NPOS : it->second;
    }
    void setStr(const Ref<Object>& key, const std::string& k,
                const Ref<Object>& val) {
        auto it = strIndex.find(k);
        if (it != strIndex.end()) { entries[it->second].second = val; return; }
        strIndex[k] = entries.size();
        entries.push_back({key, val});
    }

    void set(const Ref<Object>& key, const Ref<Object>& val) {
        switch (key->type()) {
            case ObjectType::STRING:
                setStr(key, static_cast<StringObject*>(key.get())->value, val);
                return;
            case ObjectType::INTEGER: {
                long long k = static_cast<IntegerObject*>(key.get())->value;
                auto it = intIndex.find(k);
                if (it != intIndex.end()) { entries[it->second].second = val; return; }
                intIndex[k] = entries.size();
                entries.push_back({key, val});
                return;
            }
            case ObjectType::BOOLEAN: {
                int k = static_cast<BooleanObject*>(key.get())->value ? 1 : 0;
                if (boolIndex[k] != NPOS) { entries[boolIndex[k]].second = val; return; }
                boolIndex[k] = entries.size();
                entries.push_back({key, val});
                return;
            }
            default: return; // unsupported key type (caller raises the error)
        }
    }

    Ref<Object> get(const Ref<Object>& key) const {
        switch (key->type()) {
            case ObjectType::STRING:
                return getStr(static_cast<StringObject*>(key.get())->value);
            case ObjectType::INTEGER: {
                auto it = intIndex.find(static_cast<IntegerObject*>(key.get())->value);
                return it == intIndex.end() ? nullptr : entries[it->second].second;
            }
            case ObjectType::BOOLEAN: {
                size_t pos = boolIndex[static_cast<BooleanObject*>(key.get())->value ? 1 : 0];
                return pos == NPOS ? nullptr : entries[pos].second;
            }
            default: return nullptr;
        }
    }

    bool remove(const Ref<Object>& key) {
        size_t pos = NPOS;
        switch (key->type()) {
            case ObjectType::STRING: {
                auto it = strIndex.find(static_cast<StringObject*>(key.get())->value);
                if (it == strIndex.end()) return false;
                pos = it->second; strIndex.erase(it);
                break;
            }
            case ObjectType::INTEGER: {
                auto it = intIndex.find(static_cast<IntegerObject*>(key.get())->value);
                if (it == intIndex.end()) return false;
                pos = it->second; intIndex.erase(it);
                break;
            }
            case ObjectType::BOOLEAN: {
                int k = static_cast<BooleanObject*>(key.get())->value ? 1 : 0;
                if (boolIndex[k] == NPOS) return false;
                pos = boolIndex[k]; boolIndex[k] = NPOS;
                break;
            }
            default: return false;
        }
        entries.erase(entries.begin() + pos);
        // Shift every index after the removed slot
        for (auto& kv : strIndex) {
            if (kv.second > pos) kv.second--;
        }
        for (auto& kv : intIndex) {
            if (kv.second > pos) kv.second--;
        }
        for (int i = 0; i < 2; ++i) {
            if (boolIndex[i] != NPOS && boolIndex[i] > pos) boolIndex[i]--;
        }
        return true;
    }
    std::string inspect() const override {
        if (!moduleName.empty()) {
            return "<module " + moduleName + ": " + std::to_string(entries.size()) + " items>";
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
    RangeObject(long long s, long long e, long long st)
        : Object(ObjectType::RANGE), start(s), end(e), step(st) {}

    long long length() const {
        if (step > 0) {
            if (end <= start) return 0;
            return (end - start + step - 1) / step;
        }
        if (end >= start) return 0;
        return (start - end + (-step) - 1) / (-step);
    }
    std::string inspect() const override {
        std::string out = "range(" + std::to_string(start) + ", " + std::to_string(end);
        if (step != 1) out += ", " + std::to_string(step);
        return out + ")";
    }
};

// Builtin function object implemented in C++.
// CallFn: lets a builtin invoke Lume functions (for each/filter/emit) — RFC-005
class BuiltinObject : public Object {
public:
    using CallFn = std::function<Ref<Object>(
        const Ref<Object>& fn,
        const std::vector<Ref<Object>>& args,
        int line)>;

    using BuiltinFn = std::function<Ref<Object>(
        const std::vector<Ref<Object>>&, int line, const CallFn&)>;

    std::string name;
    BuiltinFn fn;

    BuiltinObject(const std::string& n, BuiltinFn f)
        : Object(ObjectType::BUILTIN), name(n), fn(std::move(f)) {}
    std::string inspect() const override { return "builtin " + name + "(...)"; }
};

// Signal object: holds a list of listeners — signal()/connect()/emit() (RFC-005)
class SignalObject : public Object {
public:
    SignalObject() : Object(ObjectType::SIGNAL) {}
    std::vector<Ref<Object>> listeners;
    void gcMark() override { for (auto& l : listeners) gcMarkObject(l.get()); }
    std::string inspect() const override {
        return "signal(" + std::to_string(listeners.size()) + " listeners)";
    }
};

class ReturnValueObject : public Object {
public:
    Ref<Object> value;
    ReturnValueObject(Ref<Object> val)
        : Object(ObjectType::RETURN_VALUE), value(val) {}
    void gcMark() override { gcMarkObject(value.get()); }
    std::string inspect() const override { return value->inspect(); }
};

// break/continue signals: bubble up from the loop body; the loop catches them
class BreakSignalObject : public Object {
public:
    int srcLine;
    BreakSignalObject(int l) : Object(ObjectType::BREAK_SIGNAL), srcLine(l) {}
    std::string inspect() const override { return "break"; }
};

class ContinueSignalObject : public Object {
public:
    int srcLine;
    ContinueSignalObject(int l) : Object(ObjectType::CONTINUE_SIGNAL), srcLine(l) {}
    std::string inspect() const override { return "continue"; }
};

// Runtime error: propagates as a value; the first error stops execution
class ErrorObject : public Object {
public:
    std::string message;
    int srcLine;
    ErrorObject(const std::string& msg, int l = 0)
        : Object(ObjectType::ERROR), message(msg), srcLine(l) {}
    std::string inspect() const override {
        if (srcLine > 0) {
            return "[Runtime Error] line " + std::to_string(srcLine) + ": " + message;
        }
        return "[Runtime Error] " + message;
    }
};

inline bool isError(const Ref<Object>& obj) {
    return obj != nullptr && obj->type() == ObjectType::ERROR;
}

inline Ref<ErrorObject> makeError(const std::string& msg, int line = 0) {
    return makeObj<ErrorObject>(msg, line);
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
        case ObjectType::COROUTINE:    return "coroutine";
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
inline bool objectEquals(const Ref<Object>& a, const Ref<Object>& b) {
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
inline bool objectTruthy(const Ref<Object>& obj) {
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
