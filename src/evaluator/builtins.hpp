#ifndef BUILTINS_HPP
#define BUILTINS_HPP

#include <memory>
#include <vector>
#include <string>
#include <cmath>
#include <random>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <thread>
#include "../object/object.hpp"
#include "../object/environment.hpp"

// Lume core builtin functions.
// Naming philosophy: short, friendly, Lume-flavored but never confusing.
//   say  -> print (statement)      ask  -> read a line from the user
//   text -> convert to string      num  -> convert to number
//   kind -> type name              len  -> length (UTF-8 aware)
//   push/pop/remove -> list/map editing
// Built-in modules (math/game/text/file) live in stdlib.hpp — loaded with 'use'

namespace Lume {
namespace Builtins {

using Args = std::vector<std::shared_ptr<Object>>;
using ObjPtr = std::shared_ptr<Object>;
using CallFn = BuiltinObject::CallFn;

// ===== Helpers =====

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

// Deep copy for clone(): nested lists/maps become fully independent
inline ObjPtr deepClone(const ObjPtr& v, int line, int depth) {
    if (depth > 100) return makeError("clone() structure too deep (nesting limit 100)", line);
    if (v->type() == ObjectType::LIST) {
        auto out = std::make_shared<ListObject>();
        for (const auto& e : static_cast<ListObject*>(v.get())->elements) {
            auto c = deepClone(e, line, depth + 1);
            if (isError(c)) return c;
            out->elements.push_back(c);
        }
        return out;
    }
    if (v->type() == ObjectType::MAP) {
        auto out = std::make_shared<MapObject>();
        for (const auto& e : static_cast<MapObject*>(v.get())->entries) {
            auto c = deepClone(e.second, line, depth + 1);
            if (isError(c)) return c;
            out->set(e.first, c);
        }
        return out;
    }
    return v;
}

// ===== Builtin registration =====

inline void installBuiltins(const std::shared_ptr<Environment>& env) {

    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        env->define(name, std::make_shared<BuiltinObject>(name, std::move(fn)));
    };

    // --- len(x): length. Counts UTF-8 code points for strings ("şey" -> 3) ---
    def("len", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("len", "1", args.size(), line);
        const auto& a = args[0];
        switch (a->type()) {
            case ObjectType::STRING:
                return std::make_shared<IntegerObject>(utf8Length(static_cast<StringObject*>(a.get())->value));
            case ObjectType::LIST:
                return std::make_shared<IntegerObject>((long long)static_cast<ListObject*>(a.get())->elements.size());
            case ObjectType::MAP:
                return std::make_shared<IntegerObject>((long long)static_cast<MapObject*>(a.get())->entries.size());
            case ObjectType::RANGE:
                return std::make_shared<IntegerObject>(static_cast<RangeObject*>(a.get())->length());
            default:
                return makeError("len() expects string/list/map/range, got " + typeName(a->type()) + "", line);
        }
    });

    // --- text(x): converts any value to a string (RFC-002: no implicit conversion) ---
    def("text", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("text", "1", args.size(), line);
        return std::make_shared<StringObject>(args[0]->inspect());
    });

    // --- num(x): converts text to a number; "42" -> 42, "3.14" -> 3.14 ---
    def("num", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("num", "1", args.size(), line);
        const auto& a = args[0];
        if (isNumeric(a)) return a;
        if (a->type() != ObjectType::STRING) {
            return makeError("num() expects a string or number, got " + typeName(a->type()) + "", line);
        }
        const std::string& s = static_cast<StringObject*>(a.get())->value;
        try {
            size_t pos = 0;
            if (s.find('.') != std::string::npos || s.find('e') != std::string::npos) {
                double d = std::stod(s, &pos);
                if (pos != s.size()) throw std::invalid_argument("");
                return std::make_shared<FloatObject>(d);
            }
            long long v = std::stoll(s, &pos);
            if (pos != s.size()) throw std::invalid_argument("");
            return std::make_shared<IntegerObject>(v);
        } catch (...) {
            return makeError("num() could not convert: \"" + s + "\" is not a valid number", line);
        }
    });

    // --- kind(x): returns the type name: "int", "float", "string", "list"... ---
    def("kind", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("kind", "1", args.size(), line);
        return std::make_shared<StringObject>(typeName(args[0]->type()));
    });

    // --- push(list, element): appends, returns the list (chainable) ---
    def("push", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("push", "2", args.size(), line);
        if (args[0]->type() != ObjectType::LIST) {
            return makeError("push() expects a list as its first argument, got " + typeName(args[0]->type()) + "", line);
        }
        static_cast<ListObject*>(args[0].get())->elements.push_back(args[1]);
        return args[0];
    });

    // --- pop(list): removes and returns the last element ---
    def("pop", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("pop", "1", args.size(), line);
        if (args[0]->type() != ObjectType::LIST) {
            return makeError("pop() expects a list, got " + typeName(args[0]->type()) + "", line);
        }
        auto* list = static_cast<ListObject*>(args[0].get());
        if (list->elements.empty()) return makeError("pop() cannot be called on an empty list", line);
        auto last = list->elements.back();
        list->elements.pop_back();
        return last;
    });

    // --- remove(list, index) or remove(map, key): deletes the element ---
    def("remove", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("remove", "2", args.size(), line);
        if (args[0]->type() == ObjectType::LIST) {
            auto* list = static_cast<ListObject*>(args[0].get());
            if (args[1]->type() != ObjectType::INTEGER) {
                return makeError("remove() expects an integer index for lists", line);
            }
            long long idx = static_cast<IntegerObject*>(args[1].get())->value;
            long long n = (long long)list->elements.size();
            if (idx < 0) idx += n;
            if (idx < 0 || idx >= n) {
                return makeError("remove() index out of range: " + std::to_string(idx) + " (length " + std::to_string(n) + ")", line);
            }
            auto removed = list->elements[idx];
            list->elements.erase(list->elements.begin() + idx);
            return removed;
        }
        if (args[0]->type() == ObjectType::MAP) {
            auto* map = static_cast<MapObject*>(args[0].get());
            if (map->frozen) {
                return makeError("module '" + map->moduleName + "' cannot be modified (frozen)", line);
            }
            auto val = map->get(args[1]);
            if (val == nullptr) return NULL_OBJ_;
            map->remove(args[1]);
            return val;
        }
        return makeError("remove() expects a list or map, got " + typeName(args[0]->type()) + "", line);
    });

    // --- keys(map): returns keys as a list in insertion order ---
    def("keys", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("keys", "1", args.size(), line);
        if (args[0]->type() != ObjectType::MAP) {
            return makeError("keys() expects a map, got " + typeName(args[0]->type()) + "", line);
        }
        auto list = std::make_shared<ListObject>();
        for (const auto& e : static_cast<MapObject*>(args[0].get())->entries) {
            list->elements.push_back(e.first);
        }
        return list;
    });

    // --- values(map): returns values as a list in insertion order ---
    def("values", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("values", "1", args.size(), line);
        if (args[0]->type() != ObjectType::MAP) {
            return makeError("values() expects a map, got " + typeName(args[0]->type()) + "", line);
        }
        auto list = std::make_shared<ListObject>();
        for (const auto& e : static_cast<MapObject*>(args[0].get())->entries) {
            list->elements.push_back(e.second);
        }
        return list;
    });

    // --- has(map, key): does the key exist? ---
    def("has", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("has", "2", args.size(), line);
        if (args[0]->type() != ObjectType::MAP) {
            return makeError("has() expects a map, got " + typeName(args[0]->type()) + "", line);
        }
        return boolObj(static_cast<MapObject*>(args[0].get())->get(args[1]) != nullptr);
    });

    // --- range(end) / range(start, end) / range(start, end, step) ---
    // Python rule: end EXCLUSIVE. range(3) -> 0,1,2. Lazy object: allocates no memory.
    def("range", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.empty() || args.size() > 3) return argCountError("range", "1-3", args.size(), line);
        long long vals[3] = {0, 0, 1};
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i]->type() != ObjectType::INTEGER) {
                return makeError("range() expects integer arguments", line);
            }
            vals[i] = static_cast<IntegerObject*>(args[i].get())->value;
        }
        long long start = 0, end = 0, step = 1;
        if (args.size() == 1) { end = vals[0]; }
        else { start = vals[0]; end = vals[1]; if (args.size() == 3) step = vals[2]; }
        if (step == 0) return makeError("range() step cannot be 0", line);
        return std::make_shared<RangeObject>(start, end, step);
    });

    // ===== Math (core) =====

    def("abs", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("abs", "1", args.size(), line);
        if (args[0]->type() == ObjectType::INTEGER) {
            long long v = static_cast<IntegerObject*>(args[0].get())->value;
            return std::make_shared<IntegerObject>(v < 0 ? -v : v);
        }
        if (args[0]->type() == ObjectType::FLOAT) {
            return std::make_shared<FloatObject>(std::fabs(asDouble(args[0])));
        }
        return makeError("abs() expects a number, got " + typeName(args[0]->type()) + "", line);
    });

    auto minMax = [](const Args& rawArgs, int line, bool isMin) -> ObjPtr {
        // min([3, 1, 2]) also works with a single list argument
        const Args* argsPtr = &rawArgs;
        Args expanded;
        if (rawArgs.size() == 1 && rawArgs[0]->type() == ObjectType::LIST) {
            expanded = static_cast<ListObject*>(rawArgs[0].get())->elements;
            if (expanded.empty()) {
                return makeError(std::string(isMin ? "min" : "max") + "() cannot be called with an empty list", line);
            }
            argsPtr = &expanded;
        }
        const Args& args = *argsPtr;
        if (args.size() < 2 && argsPtr == &rawArgs) {
            return argCountError(isMin ? "min" : "max", "at least 2 (or one list)", args.size(), line);
        }
        bool allInt = true;
        for (const auto& a : args) {
            if (!isNumeric(a)) {
                return makeError(std::string(isMin ? "min" : "max") + "() only works with numbers", line);
            }
            if (a->type() == ObjectType::FLOAT) allInt = false;
        }
        double best = asDouble(args[0]);
        for (size_t i = 1; i < args.size(); ++i) {
            double v = asDouble(args[i]);
            if (isMin ? (v < best) : (v > best)) best = v;
        }
        if (allInt) return std::make_shared<IntegerObject>((long long)best);
        return std::make_shared<FloatObject>(best);
    };
    def("min", [minMax](const Args& a, int l, const CallFn&) { return minMax(a, l, true); });
    def("max", [minMax](const Args& a, int l, const CallFn&) { return minMax(a, l, false); });

    def("sqrt", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("sqrt", "1", args.size(), line);
        if (!isNumeric(args[0])) {
            return makeError("sqrt() expects a number, got " + typeName(args[0]->type()) + "", line);
        }
        double v = asDouble(args[0]);
        if (v < 0) return makeError("sqrt() cannot take a negative number", line);
        return std::make_shared<FloatObject>(std::sqrt(v));
    });

    // floor/ceil/round return ints (handy for coord -> pixel in games)
    auto intRound = [](const std::string& name, double(*f)(double)) {
        return [name, f](const Args& args, int line, const CallFn&) -> ObjPtr {
            if (args.size() != 1) return argCountError(name, "1", args.size(), line);
            if (!isNumeric(args[0])) {
                return makeError(name + "() expects a number, got " + typeName(args[0]->type()) + "", line);
            }
            return std::make_shared<IntegerObject>((long long)f(asDouble(args[0])));
        };
    };
    def("floor", intRound("floor", std::floor));
    def("ceil",  intRound("ceil",  std::ceil));
    def("round", intRound("round", std::round));

    def("pow", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("pow", "2", args.size(), line);
        if (!isNumeric(args[0]) || !isNumeric(args[1])) {
            return makeError("pow() expects two numbers", line);
        }
        double result = std::pow(asDouble(args[0]), asDouble(args[1]));
        // Two ints with an integral result stay int (pow(2,10) -> 1024)
        if (args[0]->type() == ObjectType::INTEGER && args[1]->type() == ObjectType::INTEGER &&
            asDouble(args[1]) >= 0 && result == std::floor(result) &&
            std::fabs(result) < 9.2e18) {
            return std::make_shared<IntegerObject>((long long)result);
        }
        return std::make_shared<FloatObject>(result);
    });

    // --- random(): float in 0..1 | random(a, b): int in a..b (both INCLUSIVE) ---
    def("random", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.empty()) {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            return std::make_shared<FloatObject>(dist(rng()));
        }
        if (args.size() == 2 &&
            args[0]->type() == ObjectType::INTEGER && args[1]->type() == ObjectType::INTEGER) {
            long long a = static_cast<IntegerObject*>(args[0].get())->value;
            long long b = static_cast<IntegerObject*>(args[1].get())->value;
            if (a > b) std::swap(a, b);
            std::uniform_int_distribution<long long> dist(a, b);
            return std::make_shared<IntegerObject>(dist(rng()));
        }
        return makeError("random() takes no arguments or two integers: random(1, 6)", line);
    });

    // --- seed(n): makes randomness deterministic (tests + game replays) ---
    def("seed", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::INTEGER) {
            return makeError("seed() expects an integer", line);
        }
        rng().seed((unsigned long long)static_cast<IntegerObject*>(args[0].get())->value);
        return NULL_OBJ_;
    });

    // --- clock(): high-resolution time in seconds (benchmarks/delta-time) ---
    def("clock", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("clock", "0", args.size(), line);
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        double secs = std::chrono::duration<double>(now).count();
        return std::make_shared<FloatObject>(secs);
    });

    // --- ask("question"): prompts the user, returns the line as a string (sibling of say) ---
    def("ask", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() > 1) return argCountError("ask", "0-1", args.size(), line);
        if (args.size() == 1) {
            std::cout << args[0]->inspect() << std::flush;
        }
        std::string input;
        if (!std::getline(std::cin, input)) return NULL_OBJ_;
        return std::make_shared<StringObject>(input);
    });

    // ===== General collection utilities (core — like Python's sorted/sum/filter) =====

    // contains(x, needle): substring in string / element in list / key in map
    def("contains", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("contains", "2", args.size(), line);
        if (args[0]->type() == ObjectType::STRING) {
            if (args[1]->type() != ObjectType::STRING) {
                return makeError("contains(): only a string can be searched inside a string", line);
            }
            const std::string& s = static_cast<StringObject*>(args[0].get())->value;
            const std::string& sub = static_cast<StringObject*>(args[1].get())->value;
            return boolObj(s.find(sub) != std::string::npos);
        }
        if (args[0]->type() == ObjectType::LIST) {
            for (const auto& e : static_cast<ListObject*>(args[0].get())->elements) {
                if (objectEquals(e, args[1])) return TRUE_OBJ;
            }
            return FALSE_OBJ;
        }
        if (args[0]->type() == ObjectType::MAP) {
            return boolObj(static_cast<MapObject*>(args[0].get())->get(args[1]) != nullptr);
        }
        return makeError("contains() expects string/list/map, got " + typeName(args[0]->type()) + "", line);
    });

    // find(x, needle): first position (string: code-point index, list: index); -1 if absent
    def("find", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("find", "2", args.size(), line);
        if (args[0]->type() == ObjectType::STRING) {
            if (args[1]->type() != ObjectType::STRING) {
                return makeError("find(): only a string can be searched inside a string", line);
            }
            const std::string& s = static_cast<StringObject*>(args[0].get())->value;
            const std::string& sub = static_cast<StringObject*>(args[1].get())->value;
            size_t byteIdx = s.find(sub);
            if (byteIdx == std::string::npos) return std::make_shared<IntegerObject>(-1);
            return std::make_shared<IntegerObject>(utf8Length(s.substr(0, byteIdx)));
        }
        if (args[0]->type() == ObjectType::LIST) {
            const auto& els = static_cast<ListObject*>(args[0].get())->elements;
            for (size_t i = 0; i < els.size(); ++i) {
                if (objectEquals(els[i], args[1])) return std::make_shared<IntegerObject>((long long)i);
            }
            return std::make_shared<IntegerObject>(-1);
        }
        return makeError("find() expects a string or list, got " + typeName(args[0]->type()) + "", line);
    });

    // slice(x, start[, end]): slicing — Python rules (negative index, end exclusive, clamped)
    def("slice", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() < 2 || args.size() > 3) return argCountError("slice", "2-3", args.size(), line);
        if (args[1]->type() != ObjectType::INTEGER ||
            (args.size() == 3 && args[2]->type() != ObjectType::INTEGER)) {
            return makeError("slice() indices must be integers", line);
        }
        auto normalize = [](long long idx, long long n) -> long long {
            if (idx < 0) idx += n;
            if (idx < 0) idx = 0;
            if (idx > n) idx = n;
            return idx;
        };
        if (args[0]->type() == ObjectType::STRING) {
            const std::string& s = static_cast<StringObject*>(args[0].get())->value;
            auto offs = utf8Offsets(s);
            long long n = (long long)offs.size() - 1;
            long long a = normalize(static_cast<IntegerObject*>(args[1].get())->value, n);
            long long b = (args.size() == 3)
                          ? normalize(static_cast<IntegerObject*>(args[2].get())->value, n)
                          : n;
            if (a >= b) return std::make_shared<StringObject>("");
            return std::make_shared<StringObject>(s.substr(offs[a], offs[b] - offs[a]));
        }
        if (args[0]->type() == ObjectType::LIST) {
            const auto& els = static_cast<ListObject*>(args[0].get())->elements;
            long long n = (long long)els.size();
            long long a = normalize(static_cast<IntegerObject*>(args[1].get())->value, n);
            long long b = (args.size() == 3)
                          ? normalize(static_cast<IntegerObject*>(args[2].get())->value, n)
                          : n;
            auto out = std::make_shared<ListObject>();
            for (long long i = a; i < b; ++i) out->elements.push_back(els[i]);
            return out;
        }
        return makeError("slice() expects a string or list, got " + typeName(args[0]->type()) + "", line);
    });

    // reverse(x): reverses a list in place / returns a reversed string (UTF-8 aware)
    def("reverse", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("reverse", "1", args.size(), line);
        if (args[0]->type() == ObjectType::LIST) {
            auto& els = static_cast<ListObject*>(args[0].get())->elements;
            std::reverse(els.begin(), els.end());
            return args[0];
        }
        if (args[0]->type() == ObjectType::STRING) {
            const std::string& s = static_cast<StringObject*>(args[0].get())->value;
            auto offs = utf8Offsets(s);
            std::string out;
            out.reserve(s.size());
            for (size_t k = offs.size() - 1; k > 0; --k) {
                out += s.substr(offs[k - 1], offs[k] - offs[k - 1]);
            }
            return std::make_shared<StringObject>(out);
        }
        return makeError("reverse() expects a list or string, got " + typeName(args[0]->type()) + "", line);
    });

    // sort(list): sorts in place (all numbers OR all strings), returns the list
    def("sort", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("sort", "1", args.size(), line);
        if (args[0]->type() != ObjectType::LIST) {
            return makeError("sort() expects a list, got " + typeName(args[0]->type()) + "", line);
        }
        auto& els = static_cast<ListObject*>(args[0].get())->elements;
        if (els.empty()) return args[0];
        bool allNum = true, allStr = true;
        for (const auto& e : els) {
            if (!isNumeric(e)) allNum = false;
            if (e->type() != ObjectType::STRING) allStr = false;
        }
        if (allNum) {
            std::stable_sort(els.begin(), els.end(), [](const ObjPtr& a, const ObjPtr& b) {
                return asDouble(a) < asDouble(b);
            });
        } else if (allStr) {
            std::stable_sort(els.begin(), els.end(), [](const ObjPtr& a, const ObjPtr& b) {
                return static_cast<StringObject*>(a.get())->value <
                       static_cast<StringObject*>(b.get())->value;
            });
        } else {
            return makeError("sort() cannot sort mixed types (all numbers or all strings; "
                             "use sort_by for custom ordering)", line);
        }
        return args[0];
    });

    // sort_by(list, fn): sorts by the fn(element) key (Schwartzian transform)
    def("sort_by", [](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.size() != 2) return argCountError("sort_by", "2", args.size(), line);
        if (args[0]->type() != ObjectType::LIST ||
            (args[1]->type() != ObjectType::FUNCTION && args[1]->type() != ObjectType::BUILTIN)) {
            return makeError("sort_by(list, fn) expects a list and a function", line);
        }
        auto& els = static_cast<ListObject*>(args[0].get())->elements;
        if (els.empty()) return args[0];

        std::vector<std::pair<ObjPtr, ObjPtr>> keyed; // (key, element)
        keyed.reserve(els.size());
        bool allNum = true, allStr = true;
        for (const auto& e : els) {
            auto key = call(args[1], {e}, line);
            if (isError(key)) return key;
            if (!isNumeric(key)) allNum = false;
            if (key->type() != ObjectType::STRING) allStr = false;
            keyed.push_back({key, e});
        }
        if (!allNum && !allStr) {
            return makeError("sort_by() keys must be all numbers or all strings", line);
        }
        std::stable_sort(keyed.begin(), keyed.end(),
            [allNum](const std::pair<ObjPtr, ObjPtr>& a, const std::pair<ObjPtr, ObjPtr>& b) {
                if (allNum) return asDouble(a.first) < asDouble(b.first);
                return static_cast<StringObject*>(a.first.get())->value <
                       static_cast<StringObject*>(b.first.get())->value;
            });
        for (size_t i = 0; i < keyed.size(); ++i) els[i] = keyed[i].second;
        return args[0];
    });

    // sum(list): sum of numbers (int if all ints)
    def("sum", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("sum", "1", args.size(), line);
        if (args[0]->type() != ObjectType::LIST) {
            return makeError("sum() expects a list, got " + typeName(args[0]->type()) + "", line);
        }
        const auto& els = static_cast<ListObject*>(args[0].get())->elements;
        bool allInt = true;
        double total = 0;
        long long totalInt = 0;
        for (const auto& e : els) {
            if (!isNumeric(e)) return makeError("sum() only works with a list of numbers", line);
            if (e->type() == ObjectType::FLOAT) allInt = false;
            total += asDouble(e);
            if (e->type() == ObjectType::INTEGER) totalInt += static_cast<IntegerObject*>(e.get())->value;
        }
        if (allInt) return std::make_shared<IntegerObject>(totalInt);
        return std::make_shared<FloatObject>(total);
    });

    // each(list, fn): calls fn(element) for every element
    def("each", [](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.size() != 2) return argCountError("each", "2", args.size(), line);
        if (args[0]->type() != ObjectType::LIST ||
            (args[1]->type() != ObjectType::FUNCTION && args[1]->type() != ObjectType::BUILTIN)) {
            return makeError("each(list, fn) expects a list and a function", line);
        }
        const auto& els = static_cast<ListObject*>(args[0].get())->elements;
        for (size_t i = 0; i < els.size(); ++i) {
            auto r = call(args[1], {els[i]}, line);
            if (isError(r)) return r;
        }
        return NULL_OBJ_;
    });

    // filter(list, fn): NEW list of elements where fn(element) is truthy
    def("filter", [](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.size() != 2) return argCountError("filter", "2", args.size(), line);
        if (args[0]->type() != ObjectType::LIST ||
            (args[1]->type() != ObjectType::FUNCTION && args[1]->type() != ObjectType::BUILTIN)) {
            return makeError("filter(list, fn) expects a list and a function", line);
        }
        const auto& els = static_cast<ListObject*>(args[0].get())->elements;
        auto out = std::make_shared<ListObject>();
        for (const auto& e : els) {
            auto r = call(args[1], {e}, line);
            if (isError(r)) return r;
            if (objectTruthy(r)) out->elements.push_back(e);
        }
        return out;
    });

    // transform(list, fn): NEW list mapping each element to fn(element)
    def("transform", [](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.size() != 2) return argCountError("transform", "2", args.size(), line);
        if (args[0]->type() != ObjectType::LIST ||
            (args[1]->type() != ObjectType::FUNCTION && args[1]->type() != ObjectType::BUILTIN)) {
            return makeError("transform(list, fn) expects a list and a function", line);
        }
        const auto& els = static_cast<ListObject*>(args[0].get())->elements;
        auto out = std::make_shared<ListObject>();
        out->elements.reserve(els.size());
        for (const auto& e : els) {
            auto r = call(args[1], {e}, line);
            if (isError(r)) return r;
            out->elements.push_back(r);
        }
        return out;
    });

    // ===== Assertions, timing, and copying =====

    // check(condition[, message]): stops execution with an error if falsy (assert)
    def("check", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.empty() || args.size() > 2) return argCountError("check", "1-2", args.size(), line);
        if (objectTruthy(args[0])) return NULL_OBJ_;
        std::string msg = "check failed";
        if (args.size() == 2) msg += ": " + args[1]->inspect();
        return makeError(msg, line);
    });

    // sleep(seconds): pauses execution (demos/tool scripts)
    def("sleep", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) {
            return makeError("sleep(seconds) expects a number", line);
        }
        double secs = asDouble(args[0]);
        if (secs > 0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(secs));
        }
        return NULL_OBJ_;
    });

    // copy(x): SHALLOW copy — new container for list/map, elements shared.
    // (Assignment shares references; use this when an independent container is needed.)
    def("copy", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("copy", "1", args.size(), line);
        if (args[0]->type() == ObjectType::LIST) {
            auto out = std::make_shared<ListObject>();
            out->elements = static_cast<ListObject*>(args[0].get())->elements;
            return out;
        }
        if (args[0]->type() == ObjectType::MAP) {
            auto* src = static_cast<MapObject*>(args[0].get());
            auto out = std::make_shared<MapObject>();
            out->entries = src->entries;
            out->index = src->index;
            return out;
        }
        return args[0]; // immutable types: no copy needed
    });

    // clone(x): DEEP copy — nested lists/maps become fully independent
    def("clone", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("clone", "1", args.size(), line);
        return deepClone(args[0], line, 0);
    });

    // insert(list, index, element): inserts (index == len appends)
    def("insert", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3) return argCountError("insert", "3", args.size(), line);
        if (args[0]->type() != ObjectType::LIST || args[1]->type() != ObjectType::INTEGER) {
            return makeError("insert(list, index, element) expects a list and an integer index", line);
        }
        auto& els = static_cast<ListObject*>(args[0].get())->elements;
        long long idx = static_cast<IntegerObject*>(args[1].get())->value;
        long long n = (long long)els.size();
        if (idx < 0) idx += n;
        if (idx < 0 || idx > n) {
            return makeError("insert() index out of range: " + args[1]->inspect() +
                             " (length " + std::to_string(n) + ")", line);
        }
        els.insert(els.begin() + idx, args[2]);
        return args[0];
    });

    // clear(x): empties a list or map
    def("clear", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("clear", "1", args.size(), line);
        if (args[0]->type() == ObjectType::LIST) {
            static_cast<ListObject*>(args[0].get())->elements.clear();
            return args[0];
        }
        if (args[0]->type() == ObjectType::MAP) {
            auto* m = static_cast<MapObject*>(args[0].get());
            if (m->frozen) {
                return makeError("module '" + m->moduleName + "' cannot be modified (frozen)", line);
            }
            m->entries.clear();
            m->index.clear();
            return args[0];
        }
        return makeError("clear() expects a list or map, got " + typeName(args[0]->type()) + "", line);
    });

    // merge(m1, m2): merges two maps into a NEW map (m2 wins on conflict)
    def("merge", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("merge", "2", args.size(), line);
        if (args[0]->type() != ObjectType::MAP || args[1]->type() != ObjectType::MAP) {
            return makeError("merge(m1, m2) expects two maps", line);
        }
        auto out = std::make_shared<MapObject>();
        for (const auto& e : static_cast<MapObject*>(args[0].get())->entries) out->set(e.first, e.second);
        for (const auto& e : static_cast<MapObject*>(args[1].get())->entries) out->set(e.first, e.second);
        return out;
    });
}

} // namespace Builtins
} // namespace Lume

#endif // BUILTINS_HPP
