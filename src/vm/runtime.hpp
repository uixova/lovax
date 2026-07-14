#ifndef VM_RUNTIME_HPP
#define VM_RUNTIME_HPP

#include <memory>
#include <string>
#include <cmath>
#include "../object/object.hpp"

// Object-path semantics shared by the VM's slow paths: binary operators on
// heap objects, index access, and member access. Extracted verbatim from the
// retired tree-walking evaluator so every error message stays byte-identical.

namespace Lume {
namespace Runtime {

    inline bool isValidMapKey(const Ref<Object>& key) {
        return key->type() == ObjectType::STRING ||
               key->type() == ObjectType::INTEGER ||
               key->type() == ObjectType::BOOLEAN;
    }

    inline Ref<Object> evalMemberAccess(const Ref<Object>& obj,
                                                    const std::string& prop, int line) {
        if (obj->type() != ObjectType::MAP) {
            return makeError("'.' access expects a map or module, got " +
                             typeName(obj->type()) + "", line);
        }
        auto* map = static_cast<MapObject*>(obj.get());
        auto val = map->getStr(prop);   // zero-allocation string-key lookup
        if (val != nullptr) return val;

        if (!map->moduleName.empty()) {
            // Not in the module: list available names to aid discovery
            std::string avail;
            size_t shown = 0;
            for (const auto& e : map->entries) {
                if (shown >= 10) { avail += ", ..."; break; }
                if (shown > 0) avail += ", ";
                avail += e.first->inspect();
                shown++;
            }
            return makeError("'" + map->moduleName + "' module has no member '" + prop +
                             "' (available: " + avail + ")", line);
        }
        return makeError("key not in map: \"" + prop +
                         "\" (check with has())", line);
    }

    inline Ref<Object> evalIndexAccess(const Ref<Object>& obj,
                                                   const Ref<Object>& idx,
                                                   int line) {
        if (obj->type() == ObjectType::LIST) {
            if (idx->type() != ObjectType::INTEGER) {
                return makeError("list index must be an integer, got " + typeName(idx->type()) + "", line);
            }
            auto* list = static_cast<ListObject*>(obj.get());
            long long i = static_cast<IntegerObject*>(idx.get())->value;
            long long n = (long long)list->elements.size();
            if (i < 0) i += n; // negatif indeks: liste[-1] son eleman
            if (i < 0 || i >= n) {
                return makeError("list index out of range: " + idx->inspect() +
                                 " (length " + std::to_string(n) + ")", line);
            }
            return list->elements[i];
        }

        if (obj->type() == ObjectType::MAP) {
            if (!isValidMapKey(idx)) {
                return makeError("map keys must be string, int or bool; got " +
                                 typeName(idx->type()) + "", line);
            }
            auto val = static_cast<MapObject*>(obj.get())->get(idx);
            if (val == nullptr) {
                return makeError("key not in map: " + inspectQuoted(idx) +
                                 " (check with has())", line);
            }
            return val;
        }

        if (obj->type() == ObjectType::STRING) {
            if (idx->type() != ObjectType::INTEGER) {
                return makeError("string index must be an integer, got " + typeName(idx->type()) + "", line);
            }
            const std::string& s = static_cast<StringObject*>(obj.get())->value;
            long long i = static_cast<IntegerObject*>(idx.get())->value;
            long long n = utf8Length(s);
            if (i < 0) i += n;
            if (i < 0 || i >= n) {
                return makeError("string index out of range: " + idx->inspect() +
                                 " (length " + std::to_string(n) + ")", line);
            }
            return makeObj<StringObject>(utf8At(s, i));
        }

        return makeError("indexing only works on list, map and string; got " +
                         typeName(obj->type()) + "", line);
    }

    inline Ref<Object> evalInfixExpression(const std::string& op,
                                                       const Ref<Object>& left,
                                                       const Ref<Object>& right,
                                                       int line) {
        // The caller reached here after popping the operands into C++ locals, so
        // they are no longer on the value stack. Root them: the concat/repeat
        // branches below allocate (which can trigger a collection).
        GcRoot _lr(left.get());
        GcRoot _rr(right.get());
        // Equality is defined for every type pair
        if (op == "==") return boolObj(objectEquals(left, right));
        if (op == "!=") return boolObj(!objectEquals(left, right));

        // Membership: element in list / substring in string / key in map / number in range
        if (op == "in") {
            switch (right->type()) {
                case ObjectType::LIST: {
                    for (const auto& e : static_cast<ListObject*>(right.get())->elements) {
                        if (objectEquals(e, left)) return TRUE_OBJ;
                    }
                    return FALSE_OBJ;
                }
                case ObjectType::STRING: {
                    if (left->type() != ObjectType::STRING) {
                        return makeError("only a string can be searched inside a string: " +
                                         typeName(left->type()) + " in string", line);
                    }
                    const std::string& hay = static_cast<StringObject*>(right.get())->value;
                    const std::string& needle = static_cast<StringObject*>(left.get())->value;
                    return boolObj(hay.find(needle) != std::string::npos);
                }
                case ObjectType::MAP:
                    return boolObj(static_cast<MapObject*>(right.get())->get(left) != nullptr);
                case ObjectType::RANGE: {
                    if (left->type() != ObjectType::INTEGER) return FALSE_OBJ;
                    auto* r = static_cast<RangeObject*>(right.get());
                    long long v = static_cast<IntegerObject*>(left.get())->value;
                    if (r->step > 0) {
                        return boolObj(v >= r->start && v < r->end && (v - r->start) % r->step == 0);
                    }
                    return boolObj(v <= r->start && v > r->end && (r->start - v) % (-r->step) == 0);
                }
                default:
                    return makeError("'in' expects list/map/string/range on the right, got " +
                                     typeName(right->type()) + "", line);
            }
        }

        // String operations
        if (left->type() == ObjectType::STRING && right->type() == ObjectType::STRING) {
            const std::string& l = static_cast<StringObject*>(left.get())->value;
            const std::string& r = static_cast<StringObject*>(right.get())->value;
            if (op == "+")  return makeObj<StringObject>(l + r);
            if (op == "<")  return boolObj(l < r);
            if (op == ">")  return boolObj(l > r);
            if (op == "<=") return boolObj(l <= r);
            if (op == ">=") return boolObj(l >= r);
            return makeError("unsupported operator on strings: " + op, line);
        }

        // "abc" * 3 -> string repetition (Python model)
        if ((left->type() == ObjectType::STRING && right->type() == ObjectType::INTEGER) ||
            (left->type() == ObjectType::INTEGER && right->type() == ObjectType::STRING)) {
            if (op == "*") {
                const auto& strObj = (left->type() == ObjectType::STRING) ? left : right;
                const auto& intObj = (left->type() == ObjectType::INTEGER) ? left : right;
                long long count = static_cast<IntegerObject*>(intObj.get())->value;
                if (count < 0) count = 0;
                if (count > 1000000) {
                    return makeError("string repetition limit exceeded (1,000,000)", line);
                }
                const std::string& s = static_cast<StringObject*>(strObj.get())->value;
                std::string out;
                out.reserve(s.size() * (size_t)count);
                for (long long i = 0; i < count; ++i) out += s;
                return makeObj<StringObject>(out);
            }
        }

        // List concatenation: [1] + [2] -> [1, 2]
        if (left->type() == ObjectType::LIST && right->type() == ObjectType::LIST) {
            if (op == "+") {
                auto out = makeObj<ListObject>();
                auto* la = static_cast<ListObject*>(left.get());
                auto* lb = static_cast<ListObject*>(right.get());
                out->elements.reserve(la->elements.size() + lb->elements.size());
                out->elements.insert(out->elements.end(), la->elements.begin(), la->elements.end());
                out->elements.insert(out->elements.end(), lb->elements.begin(), lb->elements.end());
                return out;
            }
            return makeError("unsupported operator on lists: " + op, line);
        }

        // Integer operations
        if (left->type() == ObjectType::INTEGER && right->type() == ObjectType::INTEGER) {
            long long l = static_cast<IntegerObject*>(left.get())->value;
            long long r = static_cast<IntegerObject*>(right.get())->value;

            if (op == "+") return makeObj<IntegerObject>(l + r);
            if (op == "-") return makeObj<IntegerObject>(l - r);
            if (op == "*") return makeObj<IntegerObject>(l * r);
            if (op == "/") {
                if (r == 0) return makeError("division by zero", line);
                // Floor division: keeps the identity with floor-mod -> (a / b) * b + a % b == a
                long long q = l / r;
                if ((l % r != 0) && ((l < 0) != (r < 0))) q--;
                return makeObj<IntegerObject>(q);
            }
            if (op == "&") return makeObj<IntegerObject>(l & r);
            if (op == "|") return makeObj<IntegerObject>(l | r);
            if (op == "^") return makeObj<IntegerObject>(l ^ r);
            if (op == "<<" || op == ">>") {
                if (r < 0 || r > 63) {
                    return makeError("shift amount must be within 0-63: " + std::to_string(r), line);
                }
                return makeObj<IntegerObject>(op == "<<" ? (l << r) : (l >> r));
            }
            if (op == "%") {
                if (r == 0) return makeError("modulo by zero", line);
                // Floor mod (Python/Lua rule): the result carries the divisor's sign.
                // The right behavior for grid/angle wrapping in games: -5 % 3 -> 1
                long long m = l % r;
                if (m != 0 && ((m < 0) != (r < 0))) m += r;
                return makeObj<IntegerObject>(m);
            }
            if (op == "**") {
                double result = std::pow((double)l, (double)r);
                // Non-negative exponent with an integral result stays int (2 ** 10 -> 1024)
                if (r >= 0 && result == std::floor(result) && std::fabs(result) < 9.2e18) {
                    return makeObj<IntegerObject>((long long)result);
                }
                return makeObj<FloatObject>(result);
            }
            if (op == "<")  return boolObj(l < r);
            if (op == ">")  return boolObj(l > r);
            if (op == "<=") return boolObj(l <= r);
            if (op == ">=") return boolObj(l >= r);
        }

        // Float or mixed-number operations
        else if ((left->type() == ObjectType::FLOAT || left->type() == ObjectType::INTEGER) &&
                 (right->type() == ObjectType::FLOAT || right->type() == ObjectType::INTEGER)) {

            double l = (left->type() == ObjectType::FLOAT)
                       ? static_cast<FloatObject*>(left.get())->value
                       : (double)static_cast<IntegerObject*>(left.get())->value;
            double r = (right->type() == ObjectType::FLOAT)
                       ? static_cast<FloatObject*>(right.get())->value
                       : (double)static_cast<IntegerObject*>(right.get())->value;

            if (op == "+") return makeObj<FloatObject>(l + r);
            if (op == "-") return makeObj<FloatObject>(l - r);
            if (op == "*") return makeObj<FloatObject>(l * r);
            if (op == "/") {
                if (r == 0.0) return makeError("division by zero", line);
                return makeObj<FloatObject>(l / r);
            }
            if (op == "%") {
                if (r == 0.0) return makeError("modulo by zero", line);
                // Floor mod (Python/Lua rule), float version
                double m = std::fmod(l, r);
                if (m != 0.0 && ((m < 0.0) != (r < 0.0))) m += r;
                return makeObj<FloatObject>(m);
            }
            if (op == "**") {
                return makeObj<FloatObject>(std::pow(l, r));
            }
            if (op == "<")  return boolObj(l < r);
            if (op == ">")  return boolObj(l > r);
            if (op == "<=") return boolObj(l <= r);
            if (op == ">=") return boolObj(l >= r);
        }

        return makeError("unsupported operation: " + typeName(left->type()) + " " + op + " " +
                         typeName(right->type()) +
                         (op == "+" && (left->type() == ObjectType::STRING ||
                                        right->type() == ObjectType::STRING)
                          ? " (use text() or \"{...}\" interpolation to convert)"
                          : ""),
                         line);
    }

} // namespace Runtime
} // namespace Lume

#endif // VM_RUNTIME_HPP
