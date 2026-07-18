#ifndef LOVAX_MODULE_COLLECTIONS_HPP
#define LOVAX_MODULE_COLLECTIONS_HPP

#include "common.hpp"

// collections — Counter, deque, setdefault. (OrderedDict is unnecessary —
// Lovax maps preserve insertion order; namedtuple is unnecessary — structs.)

namespace Lovax {
namespace StdLib {

inline ObjPtr makeCollectionsModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    // counter(list): frequency map (element -> count); elements string/int/bool
    def("counter", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 ||
            (args[0]->type() != ObjectType::LIST && args[0]->type() != ObjectType::TUPLE)) {
            return makeError("collections.counter(list) expects a list or tuple", line);
        }
        auto out = makeObj<MapObject>();
        GcRoot _gr(out.get());
        for (const auto& e : static_cast<ListObject*>(args[0].get())->elements) {
            if (e->type() != ObjectType::STRING && e->type() != ObjectType::INTEGER &&
                e->type() != ObjectType::BOOLEAN) {
                return makeError("collections.counter() elements must be string, int or bool", line);
            }
            auto cur = out->get(e);
            long long n = cur ? static_cast<IntegerObject*>(cur.get())->value : 0;
            out->set(e, makeObj<IntegerObject>(n + 1));
        }
        return out;
    });

    // setdefault(map, key, default): returns map[key], inserting default if absent
    def("setdefault", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3 || args[0]->type() != ObjectType::MAP) {
            return makeError("collections.setdefault(map, key, default) expects a map", line);
        }
        auto* m = static_cast<MapObject*>(args[0].get());
        if (m->frozen) {
            return makeError("module '" + m->moduleName + "' cannot be modified (frozen)", line);
        }
        if (args[1]->type() != ObjectType::STRING && args[1]->type() != ObjectType::INTEGER &&
            args[1]->type() != ObjectType::BOOLEAN) {
            return makeError("map keys must be string, int or bool; got " +
                             typeName(args[1]->type()), line);
        }
        auto v = m->get(args[1]);
        if (v != nullptr) return v;
        m->set(args[1], args[2]);
        return args[2];
    });

    // deque([list]): O(1) push/pop at both ends
    def("deque", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() > 1) return argCountError("deque", "0-1", args.size(), line);
        auto out = makeObj<DequeObject>();
        GcRoot _gr(out.get());
        if (args.size() == 1) {
            if (args[0]->type() != ObjectType::LIST && args[0]->type() != ObjectType::TUPLE) {
                return makeError("collections.deque(list) expects a list or tuple", line);
            }
            for (const auto& e : static_cast<ListObject*>(args[0].get())->elements) {
                out->items.push_back(e);
            }
        }
        return out;
    });

    auto needDeque = [](const Args& args, const char* fname, int line) -> ObjPtr {
        if (args.empty() || args[0]->type() != ObjectType::DEQUE) {
            return makeError(std::string("collections.") + fname + "() expects a deque", line);
        }
        return nullptr;
    };

    def("push_back", [needDeque](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (auto e = needDeque(args, "push_back", line)) return e;
        if (args.size() != 2) return argCountError("push_back", "2", args.size(), line);
        gcShade(args[1].get());   // write barrier (RFC-023)
        static_cast<DequeObject*>(args[0].get())->items.push_back(args[1]);
        return args[0];
    });
    def("push_front", [needDeque](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (auto e = needDeque(args, "push_front", line)) return e;
        if (args.size() != 2) return argCountError("push_front", "2", args.size(), line);
        gcShade(args[1].get());   // write barrier (RFC-023)
        static_cast<DequeObject*>(args[0].get())->items.push_front(args[1]);
        return args[0];
    });
    def("pop_back", [needDeque](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (auto e = needDeque(args, "pop_back", line)) return e;
        auto* d = static_cast<DequeObject*>(args[0].get());
        if (d->items.empty()) return makeError("pop_back() on an empty deque", line);
        auto v = d->items.back();
        d->items.pop_back();
        return v;
    });
    def("pop_front", [needDeque](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (auto e = needDeque(args, "pop_front", line)) return e;
        auto* d = static_cast<DequeObject*>(args[0].get());
        if (d->items.empty()) return makeError("pop_front() on an empty deque", line);
        auto v = d->items.front();
        d->items.pop_front();
        return v;
    });
    def("to_list", [needDeque](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (auto e = needDeque(args, "to_list", line)) return e;
        auto* d = static_cast<DequeObject*>(args[0].get());
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        for (const auto& e2 : d->items) out->elements.push_back(e2);
        return out;
    });

    // ---- Heap (priority queue) + bisect: the last P1 gap-analysis items. ----
    // A heap is a plain list kept in min-heap order by these functions.
    // Elements: numbers, or [priority, payload] lists compared by priority.
    auto heapKey = [](const ObjPtr& e, double& out) -> bool {
        if (isNumeric(e)) { out = asDouble(e); return true; }
        if (e->type() == ObjectType::LIST || e->type() == ObjectType::TUPLE) {
            const auto& els = static_cast<ListObject*>(e.get())->elements;
            if (!els.empty() && isNumeric(els[0])) { out = asDouble(els[0]); return true; }
        }
        return false;
    };

    // heap_push(heap, x): inserts keeping min-heap order
    def("heap_push", [heapKey](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::LIST) {
            return makeError("collections.heap_push(heap, x) expects a list and a value", line);
        }
        double kx;
        if (!heapKey(args[1], kx)) {
            return makeError("heap elements must be numbers or [priority, ...] lists", line);
        }
        gcShade(args[1].get());   // write barrier (RFC-023)
        auto& els = static_cast<ListObject*>(args[0].get())->elements;
        els.push_back(args[1]);
        size_t i = els.size() - 1;
        while (i > 0) {           // sift up
            size_t parent = (i - 1) / 2;
            double kp;
            if (!heapKey(els[parent], kp)) {
                return makeError("heap contains a non-heap element", line);
            }
            if (kp <= kx) break;
            std::swap(els[i], els[parent]);
            i = parent;
        }
        return args[0];
    });

    // heap_pop(heap): removes and returns the SMALLEST element
    def("heap_pop", [heapKey](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::LIST) {
            return makeError("collections.heap_pop(heap) expects a list", line);
        }
        auto& els = static_cast<ListObject*>(args[0].get())->elements;
        if (els.empty()) return makeError("heap_pop() on an empty heap", line);
        auto top = els[0];
        els[0] = els.back();
        els.pop_back();
        size_t i = 0, n = els.size();
        while (true) {            // sift down
            size_t l = 2 * i + 1, r = 2 * i + 2, smallest = i;
            double ks, kc;
            if (n > 0 && !heapKey(els[smallest], ks)) break;
            if (l < n && heapKey(els[l], kc) && kc < ks) { smallest = l; ks = kc; }
            if (r < n && heapKey(els[r], kc) && kc < ks) smallest = r;
            if (smallest == i) break;
            std::swap(els[i], els[smallest]);
            i = smallest;
        }
        return top;
    });

    // heapify(list): reorders a list into min-heap order, in place
    def("heapify", [heapKey](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::LIST) {
            return makeError("collections.heapify(list) expects a list", line);
        }
        auto& els = static_cast<ListObject*>(args[0].get())->elements;
        for (const auto& e : els) {
            double k;
            if (!heapKey(e, k)) {
                return makeError("heap elements must be numbers or [priority, ...] lists", line);
            }
        }
        auto lt = [heapKey](const ObjPtr& a, const ObjPtr& b) {
            double ka = 0, kb = 0;
            heapKey(a, ka); heapKey(b, kb);
            return ka < kb;
        };
        std::make_heap(els.begin(), els.end(),
                       [lt](const ObjPtr& a, const ObjPtr& b) { return lt(b, a); });
        return args[0];
    });

    // bisect(sorted_list, x): leftmost insertion index that keeps the order
    // (Python's bisect_left; binary_search — exact find — is a core builtin)
    def("bisect", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 ||
            (args[0]->type() != ObjectType::LIST && args[0]->type() != ObjectType::TUPLE)) {
            return makeError("collections.bisect(sorted_list, x) expects a list", line);
        }
        const auto& els = static_cast<ListObject*>(args[0].get())->elements;
        bool numeric = isNumeric(args[1]);
        bool stringy = args[1]->type() == ObjectType::STRING;
        if (!numeric && !stringy) {
            return makeError("bisect() works with numbers or strings", line);
        }
        size_t lo = 0, hi = els.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            bool less;
            if (numeric) {
                if (!isNumeric(els[mid])) return makeError("bisect() needs a uniformly typed sorted list", line);
                less = asDouble(els[mid]) < asDouble(args[1]);
            } else {
                if (els[mid]->type() != ObjectType::STRING) return makeError("bisect() needs a uniformly typed sorted list", line);
                less = static_cast<StringObject*>(els[mid].get())->value <
                       static_cast<StringObject*>(args[1].get())->value;
            }
            if (less) lo = mid + 1; else hi = mid;
        }
        return makeObj<IntegerObject>((long long)lo);
    });

    mod->frozen = true;
    mod->moduleName = "collections";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_COLLECTIONS_HPP
