#ifndef LOVAX_MODULE_RANDOM_HPP
#define LOVAX_MODULE_RANDOM_HPP

#include "common.hpp"

// random — distributions and sequence sampling (RFC-020).
//
// Every algorithm here is OUR OWN implementation over the two deterministic
// primitives (rngU53 / rngBounded). std::random's distributions give different
// results on different standard libraries; a game replay seeded with seed(42)
// must produce the same run on every platform, so we cannot use them.
// Methods studied from the literature (Box-Muller, Marsaglia-Tsang, Knuth) and
// Python's Lib/random.py — studied, never copied.

namespace Lovax {
namespace StdLib {

inline ObjPtr makeRandomModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    auto needNum = [](const Args& args, size_t i) { return isNumeric(args[i]); };

    // seed(n): same generator as the global seed() — one stream everywhere
    def("seed", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::INTEGER) {
            return makeError("random.seed(n) expects an integer", line);
        }
        rng().seed((unsigned long long)static_cast<IntegerObject*>(args[0].get())->value);
        return NULL_OBJ_;
    });

    // uniform(a, b): float in [a, b)
    def("uniform", [needNum](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !needNum(args, 0) || !needNum(args, 1)) {
            return makeError("random.uniform(a, b) expects two numbers", line);
        }
        double a = asDouble(args[0]), b = asDouble(args[1]);
        return makeObj<FloatObject>(a + rngU53() * (b - a));
    });

    // normal(mean, stddev): Gaussian via Box-Muller (no cached spare — the
    // hidden pair state would complicate getstate/setstate)
    def("normal", [needNum](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !needNum(args, 0) || !needNum(args, 1)) {
            return makeError("random.normal(mean, stddev) expects two numbers", line);
        }
        double u1 = rngU53(), u2 = rngU53();
        if (u1 <= 0.0) u1 = 5e-324;   // guard log(0)
        double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.28318530717958647692 * u2);
        return makeObj<FloatObject>(asDouble(args[0]) + z * asDouble(args[1]));
    });

    // exponential(lambda): inverse CDF
    def("exponential", [needNum](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !needNum(args, 0)) {
            return makeError("random.exponential(lambda) expects a number", line);
        }
        double lam = asDouble(args[0]);
        if (lam <= 0) return makeError("random.exponential() lambda must be > 0", line);
        double u = rngU53();
        if (u >= 1.0) u = 1.0 - 5e-324;
        return makeObj<FloatObject>(-std::log(1.0 - u) / lam);
    });

    // poisson(lambda): Knuth's product method (exact, deterministic)
    def("poisson", [needNum](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !needNum(args, 0)) {
            return makeError("random.poisson(lambda) expects a number", line);
        }
        double lam = asDouble(args[0]);
        if (lam <= 0) return makeError("random.poisson() lambda must be > 0", line);
        if (lam > 700) return makeError("random.poisson() lambda too large (limit 700)", line);
        double L = std::exp(-lam), p = 1.0;
        long long k = 0;
        do { k++; p *= rngU53(); } while (p > L);
        return makeObj<IntegerObject>(k - 1);
    });

    // gamma(alpha, beta): Marsaglia-Tsang squeeze; alpha<1 boosted by u^(1/alpha)
    def("gamma", [needNum](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !needNum(args, 0) || !needNum(args, 1)) {
            return makeError("random.gamma(alpha, beta) expects two numbers", line);
        }
        double alpha = asDouble(args[0]), beta = asDouble(args[1]);
        if (alpha <= 0 || beta <= 0) return makeError("random.gamma() parameters must be > 0", line);
        double boost = 1.0;
        double a = alpha;
        if (a < 1.0) {
            double u = rngU53();
            if (u <= 0.0) u = 5e-324;
            boost = std::pow(u, 1.0 / a);
            a += 1.0;
        }
        double d = a - 1.0 / 3.0, c = 1.0 / std::sqrt(9.0 * d);
        double v;
        while (true) {
            double x, u1 = rngU53(), u2 = rngU53();
            if (u1 <= 0.0) u1 = 5e-324;
            // standard normal via Box-Muller for the squeeze step
            x = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.28318530717958647692 * u2);
            v = 1.0 + c * x;
            if (v <= 0) continue;
            v = v * v * v;
            double u = rngU53();
            if (u < 1.0 - 0.0331 * x * x * x * x) break;
            if (u > 0.0 && std::log(u) < 0.5 * x * x + d * (1.0 - v + std::log(v))) break;
        }
        return makeObj<FloatObject>(boost * d * v / beta);
    });

    // beta(a, b): ratio of gammas
    def("beta", [needNum](const Args& args, int line, const CallFn& call) -> ObjPtr {
        if (args.size() != 2 || !needNum(args, 0) || !needNum(args, 1)) {
            return makeError("random.beta(a, b) expects two numbers", line);
        }
        double a = asDouble(args[0]), b = asDouble(args[1]);
        if (a <= 0 || b <= 0) return makeError("random.beta() parameters must be > 0", line);
        // inline gamma(k, 1) sampler (same algorithm as gamma above)
        auto gamma1 = [](double k) -> double {
            double boost = 1.0;
            if (k < 1.0) {
                double u = rngU53();
                if (u <= 0.0) u = 5e-324;
                boost = std::pow(u, 1.0 / k);
                k += 1.0;
            }
            double d = k - 1.0 / 3.0, c = 1.0 / std::sqrt(9.0 * d);
            while (true) {
                double u1 = rngU53(), u2 = rngU53();
                if (u1 <= 0.0) u1 = 5e-324;
                double x = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.28318530717958647692 * u2);
                double v = 1.0 + c * x;
                if (v <= 0) continue;
                v = v * v * v;
                double u = rngU53();
                if (u < 1.0 - 0.0331 * x * x * x * x) return boost * d * v;
                if (u > 0.0 && std::log(u) < 0.5 * x * x + d * (1.0 - v + std::log(v))) return boost * d * v;
            }
        };
        double ga = gamma1(a), gb = gamma1(b);
        return makeObj<FloatObject>(ga / (ga + gb));
    });

    // triangular(low, high, mode): inverse CDF
    def("triangular", [needNum](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3 || !needNum(args, 0) || !needNum(args, 1) || !needNum(args, 2)) {
            return makeError("random.triangular(low, high, mode) expects three numbers", line);
        }
        double lo = asDouble(args[0]), hi = asDouble(args[1]), mode = asDouble(args[2]);
        if (!(lo <= mode && mode <= hi) || lo == hi) {
            return makeError("random.triangular() needs low <= mode <= high", line);
        }
        double u = rngU53(), f = (mode - lo) / (hi - lo);
        double r = (u < f) ? lo + std::sqrt(u * (hi - lo) * (mode - lo))
                           : hi - std::sqrt((1 - u) * (hi - lo) * (hi - mode));
        return makeObj<FloatObject>(r);
    });

    // choice(list|tuple): one uniformly random element
    def("choice", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 ||
            (args[0]->type() != ObjectType::LIST && args[0]->type() != ObjectType::TUPLE)) {
            return makeError("random.choice(list) expects a list or tuple", line);
        }
        const auto& els = static_cast<ListObject*>(args[0].get())->elements;
        if (els.empty()) return makeError("random.choice() of an empty list", line);
        return els[(size_t)rngBounded(els.size())];
    });

    // shuffle(list): in-place Fisher-Yates, returns the list
    def("shuffle", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::LIST) {
            return makeError("random.shuffle(list) expects a list", line);
        }
        auto& els = static_cast<ListObject*>(args[0].get())->elements;
        for (size_t i = els.size(); i > 1; --i) {
            std::swap(els[i - 1], els[(size_t)rngBounded(i)]);
        }
        return args[0];
    });

    // sample(list, k): k distinct elements, without replacement
    def("sample", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 ||
            (args[0]->type() != ObjectType::LIST && args[0]->type() != ObjectType::TUPLE) ||
            args[1]->type() != ObjectType::INTEGER) {
            return makeError("random.sample(list, k) expects a list and an integer", line);
        }
        const auto& els = static_cast<ListObject*>(args[0].get())->elements;
        long long k = static_cast<IntegerObject*>(args[1].get())->value;
        if (k < 0 || (size_t)k > els.size()) {
            return makeError("random.sample() k out of range (0-" +
                             std::to_string(els.size()) + ")", line);
        }
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        std::vector<Ref<Object>> pool = els;   // partial Fisher-Yates on a copy
        for (long long i = 0; i < k; ++i) {
            size_t j = (size_t)i + (size_t)rngBounded(pool.size() - (size_t)i);
            std::swap(pool[(size_t)i], pool[j]);
            out->elements.push_back(pool[(size_t)i]);
        }
        return out;
    });

    // choices(list, weights, k): k weighted draws WITH replacement (loot tables)
    def("choices", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3 || args[0]->type() != ObjectType::LIST ||
            args[1]->type() != ObjectType::LIST || args[2]->type() != ObjectType::INTEGER) {
            return makeError("random.choices(list, weights, k) expects (list, list, int)", line);
        }
        const auto& els = static_cast<ListObject*>(args[0].get())->elements;
        const auto& ws = static_cast<ListObject*>(args[1].get())->elements;
        long long k = static_cast<IntegerObject*>(args[2].get())->value;
        if (els.empty() || els.size() != ws.size()) {
            return makeError("random.choices() needs equal-length non-empty lists", line);
        }
        if (k < 0 || k > 1000000) return makeError("random.choices() k out of range", line);
        std::vector<double> cum;
        cum.reserve(ws.size());
        double total = 0;
        for (const auto& w : ws) {
            if (!isNumeric(w)) return makeError("random.choices() weights must be numbers", line);
            double d = asDouble(w);
            if (d < 0) return makeError("random.choices() weights must be >= 0", line);
            total += d;
            cum.push_back(total);
        }
        if (total <= 0) return makeError("random.choices() total weight must be > 0", line);
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        for (long long i = 0; i < k; ++i) {
            double r = rngU53() * total;
            size_t idx = (size_t)(std::lower_bound(cum.begin(), cum.end(), r) - cum.begin());
            if (idx >= els.size()) idx = els.size() - 1;
            out->elements.push_back(els[idx]);
        }
        return out;
    });

    // getstate() / setstate(s): capture and restore the generator (replays)
    def("getstate", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (!args.empty()) return argCountError("getstate", "0", args.size(), line);
        std::ostringstream ss;
        ss << rng();
        return makeObj<StringObject>(ss.str());
    });
    def("setstate", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("random.setstate(state) expects the string from getstate()", line);
        }
        std::istringstream ss(static_cast<StringObject*>(args[0].get())->value);
        std::mt19937_64 test;
        ss >> test;
        if (ss.fail()) return makeError("random.setstate() invalid state string", line);
        rng() = test;
        return NULL_OBJ_;
    });

    mod->frozen = true;
    mod->moduleName = "random";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_RANDOM_HPP
