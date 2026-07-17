#ifndef LOVAX_MODULE_ITERS_HPP
#define LOVAX_MODULE_ITERS_HPP

#include "common.hpp"

// iters — iteration helpers in the spirit of Python's itertools (eager, list-
// returning: lazy sequences arrive with generators/coroutines later).
// List-first argument order matches the rest of Lovax (takewhile(list, fn)).

namespace Lovax {
namespace StdLib {

inline ObjPtr makeItersModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    auto isSeq = [](const ObjPtr& o) {
        return o->type() == ObjectType::LIST || o->type() == ObjectType::TUPLE;
    };
    auto els = [](const ObjPtr& o) -> const std::vector<Ref<Object>>& {
        return static_cast<ListObject*>(o.get())->elements;
    };
    auto isFn = [](const ObjPtr& o) {
        return o->type() == ObjectType::FUNCTION || o->type() == ObjectType::BUILTIN;
    };

    // chain(a, b, ...): one list with everything, in order
    def("chain", [isSeq, els](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.empty()) return argCountError("chain", "1+", args.size(), line);
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        for (const auto& a : args) {
            if (!isSeq(a)) return makeError("iters.chain() expects lists/tuples", line);
            for (const auto& e : els(a)) out->elements.push_back(e);
        }
        return out;
    });

    // product(a, b): all [x, y] pairs
    def("product", [isSeq, els](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !isSeq(args[0]) || !isSeq(args[1])) {
            return makeError("iters.product(a, b) expects two lists", line);
        }
        const auto& la = els(args[0]);
        const auto& lb = els(args[1]);
        if (la.size() * lb.size() > 1000000) {
            return makeError("iters.product() result too large (limit 1M)", line);
        }
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        for (const auto& x : la) {
            for (const auto& y : lb) {
                auto pair = makeObj<ListObject>();
                GcRoot _pr(pair.get());
                pair->elements.push_back(x);
                pair->elements.push_back(y);
                out->elements.push_back(pair);
            }
        }
        return out;
    });

    // combinations(list, k): k-element subsequences (order preserved)
    def("combinations", [isSeq, els](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !isSeq(args[0]) || args[1]->type() != ObjectType::INTEGER) {
            return makeError("iters.combinations(list, k) expects a list and an integer", line);
        }
        const auto& src = els(args[0]);
        long long k = static_cast<IntegerObject*>(args[1].get())->value;
        long long n = (long long)src.size();
        if (k < 0 || k > n) return makeObj<ListObject>();
        // count check: C(n, k) capped
        double count = 1;
        for (long long i2 = 0; i2 < k; ++i2) count = count * (double)(n - i2) / (double)(i2 + 1);
        if (count > 1000000) return makeError("iters.combinations() result too large (limit 1M)", line);
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        std::vector<long long> idx(k);
        for (long long i2 = 0; i2 < k; ++i2) idx[i2] = i2;
        while (true) {
            auto combo = makeObj<ListObject>();
            GcRoot _cr(combo.get());
            for (long long i2 = 0; i2 < k; ++i2) combo->elements.push_back(src[idx[i2]]);
            out->elements.push_back(combo);
            long long i2 = k - 1;
            while (i2 >= 0 && idx[i2] == n - k + i2) i2--;
            if (i2 < 0) break;
            idx[i2]++;
            for (long long j = i2 + 1; j < k; ++j) idx[j] = idx[j - 1] + 1;
        }
        return out;
    });

    // permutations(list[, k]): k-element orderings (default k = len)
    def("permutations", [isSeq, els](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.empty() || args.size() > 2 || !isSeq(args[0]) ||
            (args.size() == 2 && args[1]->type() != ObjectType::INTEGER)) {
            return makeError("iters.permutations(list[, k]) expects a list and an optional integer", line);
        }
        const auto& src = els(args[0]);
        long long n = (long long)src.size();
        long long k = args.size() == 2 ? static_cast<IntegerObject*>(args[1].get())->value : n;
        if (k < 0 || k > n) return makeObj<ListObject>();
        double count = 1;
        for (long long i2 = 0; i2 < k; ++i2) count *= (double)(n - i2);
        if (count > 1000000) return makeError("iters.permutations() result too large (limit 1M)", line);
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        std::vector<long long> idx(n);
        for (long long i2 = 0; i2 < n; ++i2) idx[i2] = i2;
        // simple recursive generation over index prefixes
        std::vector<long long> cur;
        std::vector<bool> used(n, false);
        std::function<void()> gen = [&]() {
            if ((long long)cur.size() == k) {
                auto perm = makeObj<ListObject>();
                GcRoot _pr(perm.get());
                for (long long ix : cur) perm->elements.push_back(src[ix]);
                out->elements.push_back(perm);
                return;
            }
            for (long long i2 = 0; i2 < n; ++i2) {
                if (used[i2]) continue;
                used[i2] = true;
                cur.push_back(i2);
                gen();
                cur.pop_back();
                used[i2] = false;
            }
        };
        gen();
        return out;
    });

    // accumulate(list[, fn]): running totals (default +) — [1,2,3] -> [1,3,6]
    def("accumulate", [isSeq, els, isFn](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.empty() || args.size() > 2 || !isSeq(args[0]) ||
            (args.size() == 2 && !isFn(args[1]))) {
            return makeError("iters.accumulate(list[, fn]) expects a list and an optional function", line);
        }
        const auto& src = els(args[0]);
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        ObjPtr acc = nullptr;
        for (const auto& e : src) {
            if (acc == nullptr) acc = e;
            else if (args.size() == 2) {
                acc = call(args[1], {acc, e}, line);
                if (isError(acc)) return acc;
            } else {
                if (!isNumeric(acc) || !isNumeric(e)) {
                    return makeError("iters.accumulate() without fn needs numbers", line);
                }
                if (acc->type() == ObjectType::INTEGER && e->type() == ObjectType::INTEGER) {
                    acc = makeObj<IntegerObject>(static_cast<IntegerObject*>(acc.get())->value +
                                                 static_cast<IntegerObject*>(e.get())->value);
                } else {
                    acc = makeObj<FloatObject>(asDouble(acc) + asDouble(e));
                }
            }
            out->elements.push_back(acc);
        }
        return out;
    });

    // takewhile(list, fn) / dropwhile(list, fn)
    def("takewhile", [isSeq, els, isFn](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.size() != 2 || !isSeq(args[0]) || !isFn(args[1])) {
            return makeError("iters.takewhile(list, fn) expects a list and a function", line);
        }
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        for (const auto& e : els(args[0])) {
            auto r = call(args[1], {e}, line);
            if (isError(r)) return r;
            if (!objectTruthy(r)) break;
            out->elements.push_back(e);
        }
        return out;
    });
    def("dropwhile", [isSeq, els, isFn](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.size() != 2 || !isSeq(args[0]) || !isFn(args[1])) {
            return makeError("iters.dropwhile(list, fn) expects a list and a function", line);
        }
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        bool dropping = true;
        for (const auto& e : els(args[0])) {
            if (dropping) {
                auto r = call(args[1], {e}, line);
                if (isError(r)) return r;
                if (objectTruthy(r)) continue;
                dropping = false;
            }
            out->elements.push_back(e);
        }
        return out;
    });

    // groupby(list, fn): consecutive runs -> [[key, [items]], ...]
    def("groupby", [isSeq, els, isFn](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.size() != 2 || !isSeq(args[0]) || !isFn(args[1])) {
            return makeError("iters.groupby(list, fn) expects a list and a function", line);
        }
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        ObjPtr curKey = nullptr;
        Ref<ListObject> curGroup = nullptr;
        for (const auto& e : els(args[0])) {
            auto key = call(args[1], {e}, line);
            if (isError(key)) return key;
            if (curKey == nullptr || !objectEquals(key, curKey)) {
                auto pair = makeObj<ListObject>();
                GcRoot _pr(pair.get());
                curGroup = makeObj<ListObject>();
                pair->elements.push_back(key);
                pair->elements.push_back(curGroup);
                out->elements.push_back(pair);
                curKey = key;
            }
            curGroup->elements.push_back(e);
        }
        return out;
    });

    // repeat(x, n): [x, x, ... n times]
    def("repeat", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[1]->type() != ObjectType::INTEGER) {
            return makeError("iters.repeat(x, n) expects a value and an integer", line);
        }
        long long n = static_cast<IntegerObject*>(args[1].get())->value;
        if (n < 0 || n > 1000000) return makeError("iters.repeat() n out of range (0-1M)", line);
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        out->elements.reserve((size_t)n);
        for (long long i2 = 0; i2 < n; ++i2) out->elements.push_back(args[0]);
        return out;
    });

    // zip_longest(a, b, fill): pairs padded with fill to the longer length
    def("zip_longest", [isSeq, els](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3 || !isSeq(args[0]) || !isSeq(args[1])) {
            return makeError("iters.zip_longest(a, b, fill) expects two lists and a fill value", line);
        }
        const auto& la = els(args[0]);
        const auto& lb = els(args[1]);
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        size_t n = std::max(la.size(), lb.size());
        for (size_t i2 = 0; i2 < n; ++i2) {
            auto pair = makeObj<ListObject>();
            GcRoot _pr(pair.get());
            pair->elements.push_back(i2 < la.size() ? la[i2] : args[2]);
            pair->elements.push_back(i2 < lb.size() ? lb[i2] : args[2]);
            out->elements.push_back(pair);
        }
        return out;
    });

    mod->frozen = true;
    mod->moduleName = "iters";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_ITERS_HPP
