#ifndef LOVAX_MODULE_MATH_HPP
#define LOVAX_MODULE_MATH_HPP

#include "common.hpp"

namespace Lovax {
namespace StdLib {


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

// ===== math module =====

inline ObjPtr makeMathModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    mod->set(strKey("PI"),  makeObj<FloatObject>(3.14159265358979323846));
    mod->set(strKey("TAU"), makeObj<FloatObject>(6.28318530717958647692));

    // lerp(a, b, t): linear interpolation — t=0 -> a, t=1 -> b
    def("lerp", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3) return argCountError("lerp", "3", args.size(), line);
        if (!isNumeric(args[0]) || !isNumeric(args[1]) || !isNumeric(args[2])) {
            return makeError("lerp(a, b, t) expects three numbers", line);
        }
        double a = asDouble(args[0]), b = asDouble(args[1]), t = asDouble(args[2]);
        return makeObj<FloatObject>(a + (b - a) * t);
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
        if (allInt) return makeObj<IntegerObject>((long long)r);
        return makeObj<FloatObject>(r);
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
        return makeObj<FloatObject>(a2 + (x - a1) * (b2 - a2) / (b1 - a1));
    });

    // sign(x): -1, 0, or 1
    def("sign", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("sign", "1", args.size(), line);
        if (!isNumeric(args[0])) return makeError("sign() expects a number", line);
        double v = asDouble(args[0]);
        return makeObj<IntegerObject>(v > 0 ? 1 : (v < 0 ? -1 : 0));
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
            return makeObj<IntegerObject>(lo + floorMod(x - lo, hi - lo));
        }
        double x = asDouble(args[0]), lo = asDouble(args[1]), hi = asDouble(args[2]);
        if (hi <= lo) return makeError("wrap() max must be greater than min", line);
        return makeObj<FloatObject>(lo + floorModF(x - lo, hi - lo));
    });

    // move_toward(current, target, delta): moves toward target by at most delta (Godot)
    def("move_toward", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3) return argCountError("move_toward", "3", args.size(), line);
        for (const auto& a : args) {
            if (!isNumeric(a)) return makeError("move_toward() expects three numbers", line);
        }
        double cur = asDouble(args[0]), target = asDouble(args[1]), delta = asDouble(args[2]);
        if (std::fabs(target - cur) <= delta) return makeObj<FloatObject>(target);
        return makeObj<FloatObject>(cur + (target > cur ? delta : -delta));
    });

    // dist(x1, y1, x2, y2): distance between two points
    def("dist", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 4) return argCountError("dist", "4", args.size(), line);
        for (const auto& a : args) {
            if (!isNumeric(a)) return makeError("dist(x1, y1, x2, y2) expects four numbers", line);
        }
        double dx = asDouble(args[2]) - asDouble(args[0]);
        double dy = asDouble(args[3]) - asDouble(args[1]);
        return makeObj<FloatObject>(std::hypot(dx, dy));
    });

    // Trigonometry + angle conversion
    auto floatFn1 = [](const std::string& name, double(*f)(double)) {
        return [name, f](const Args& args, int line, const CallFn&) -> ObjPtr {
            if (args.size() != 1) return argCountError(name, "1", args.size(), line);
            if (!isNumeric(args[0])) return makeError(name + "() expects a number", line);
            return makeObj<FloatObject>(f(asDouble(args[0])));
        };
    };
    def("sin", floatFn1("sin", std::sin));
    def("cos", floatFn1("cos", std::cos));
    def("tan", floatFn1("tan", std::tan));
    def("atan2", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("atan2", "2", args.size(), line);
        if (!isNumeric(args[0]) || !isNumeric(args[1])) return makeError("atan2(y, x) expects two numbers", line);
        return makeObj<FloatObject>(std::atan2(asDouble(args[0]), asDouble(args[1])));
    });
    def("deg", floatFn1("deg", [](double r) { return r * 180.0 / 3.14159265358979323846; }));
    def("rad", floatFn1("rad", [](double d) { return d * 3.14159265358979323846 / 180.0; }));
    def("asin", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) return makeError("asin() expects a number", line);
        double v = asDouble(args[0]);
        if (v < -1 || v > 1) return makeError("asin() expects a value in -1..1", line);
        return makeObj<FloatObject>(std::asin(v));
    });
    def("acos", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) return makeError("acos() expects a number", line);
        double v = asDouble(args[0]);
        if (v < -1 || v > 1) return makeError("acos() expects a value in -1..1", line);
        return makeObj<FloatObject>(std::acos(v));
    });
    def("exp", floatFn1("exp", std::exp));
    // log(x): natural log | log(x, base)
    def("log", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.empty() || args.size() > 2 || !isNumeric(args[0])) {
            return makeError("log(x[, base]) expects one or two numbers", line);
        }
        double v = asDouble(args[0]);
        if (v <= 0) return makeError("log() expects a positive number", line);
        if (args.size() == 1) return makeObj<FloatObject>(std::log(v));
        if (!isNumeric(args[1])) return makeError("log() base must be a number", line);
        double base = asDouble(args[1]);
        if (base <= 0 || base == 1) return makeError("log() base must be positive and not 1", line);
        return makeObj<FloatObject>(std::log(v) / std::log(base));
    });
    // snap(x, step): rounds to the nearest multiple (grid alignment) — snap(13, 5) -> 15.0
    def("snap", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !isNumeric(args[0]) || !isNumeric(args[1])) {
            return makeError("snap(x, step) expects two numbers", line);
        }
        double step = asDouble(args[1]);
        if (step == 0) return makeError("snap() step cannot be 0", line);
        return makeObj<FloatObject>(std::round(asDouble(args[0]) / step) * step);
    });

    mod->set(strKey("INF"), makeObj<FloatObject>(std::numeric_limits<double>::infinity()));

    // inverse_lerp(a, b, v): where v falls between a and b as 0..1
    def("inverse_lerp", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3 || !isNumeric(args[0]) || !isNumeric(args[1]) || !isNumeric(args[2]))
            return makeError("inverse_lerp(a, b, v) expects three numbers", line);
        double a = asDouble(args[0]), b = asDouble(args[1]), v = asDouble(args[2]);
        if (a == b) return makeObj<FloatObject>(0.0);
        return makeObj<FloatObject>((v - a) / (b - a));
    });
    // smoothstep(a, b, t): smooth Hermite interpolation, clamped
    def("smoothstep", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3 || !isNumeric(args[0]) || !isNumeric(args[1]) || !isNumeric(args[2]))
            return makeError("smoothstep(a, b, t) expects three numbers", line);
        double a = asDouble(args[0]), b = asDouble(args[1]), t = asDouble(args[2]);
        if (a == b) return makeObj<FloatObject>(0.0);
        t = (t - a) / (b - a);
        if (t < 0) t = 0; if (t > 1) t = 1;
        return makeObj<FloatObject>(t * t * (3 - 2 * t));
    });
    // ping_pong(t, length): bounces t back and forth in [0, length]
    def("ping_pong", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !isNumeric(args[0]) || !isNumeric(args[1]))
            return makeError("ping_pong(t, length) expects two numbers", line);
        double t = asDouble(args[0]), len = asDouble(args[1]);
        if (len <= 0) return makeError("ping_pong() length must be positive", line);
        double m = floorModF(t, len * 2);
        return makeObj<FloatObject>(m <= len ? m : len * 2 - m);
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
        return makeObj<IntegerObject>(a);
    });
    def("lcm", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::INTEGER || args[1]->type() != ObjectType::INTEGER)
            return makeError("lcm(a, b) expects two integers", line);
        long long a = std::llabs(static_cast<IntegerObject*>(args[0].get())->value);
        long long b = std::llabs(static_cast<IntegerObject*>(args[1].get())->value);
        if (a == 0 || b == 0) return makeObj<IntegerObject>(0);
        long long g = a; long long bb = b; while (bb) { long long t = bb; bb = g % bb; g = t; }
        return makeObj<IntegerObject>(a / g * b);
    });
    def("factorial", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::INTEGER)
            return makeError("factorial(n) expects an integer", line);
        long long n = static_cast<IntegerObject*>(args[0].get())->value;
        if (n < 0) return makeError("factorial() expects n >= 0", line);
        if (n > 20) return makeError("factorial() overflows for n > 20", line);
        long long r = 1; for (long long i = 2; i <= n; ++i) r *= i;
        return makeObj<IntegerObject>(r);
    });

    // Constants (Python-parity: math.pi, tau, e, inf, nan)
    mod->set(strKey("pi"),  makeObj<FloatObject>(3.14159265358979323846));
    mod->set(strKey("tau"), makeObj<FloatObject>(6.28318530717958647692));
    mod->set(strKey("e"),   makeObj<FloatObject>(2.71828182845904523536));
    mod->set(strKey("inf"), makeObj<FloatObject>(std::numeric_limits<double>::infinity()));
    mod->set(strKey("nan"), makeObj<FloatObject>(std::numeric_limits<double>::quiet_NaN()));

    // log10 / log2 / hypot / copysign / isfinite / prod
    def("log10", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) return makeError("log10(x) expects a number", line);
        double v = asDouble(args[0]);
        if (v <= 0) return makeError("log10() expects x > 0", line);
        return makeObj<FloatObject>(std::log10(v));
    });
    def("log2", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) return makeError("log2(x) expects a number", line);
        double v = asDouble(args[0]);
        if (v <= 0) return makeError("log2() expects x > 0", line);
        return makeObj<FloatObject>(std::log2(v));
    });
    def("hypot", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !isNumeric(args[0]) || !isNumeric(args[1]))
            return makeError("hypot(a, b) expects two numbers", line);
        return makeObj<FloatObject>(std::hypot(asDouble(args[0]), asDouble(args[1])));
    });
    def("copysign", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2 || !isNumeric(args[0]) || !isNumeric(args[1]))
            return makeError("copysign(a, b) expects two numbers", line);
        return makeObj<FloatObject>(std::copysign(asDouble(args[0]), asDouble(args[1])));
    });
    def("is_finite", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isNumeric(args[0])) return makeError("is_finite(x) expects a number", line);
        return boolObj(std::isfinite(asDouble(args[0])));
    });
    def("prod", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 ||
            (args[0]->type() != ObjectType::LIST && args[0]->type() != ObjectType::TUPLE))
            return makeError("prod(list) expects a list", line);
        bool allInt = true;
        double total = 1;
        long long totalInt = 1;
        for (const auto& e : static_cast<ListObject*>(args[0].get())->elements) {
            if (!isNumeric(e)) return makeError("prod() only works with numbers", line);
            if (e->type() == ObjectType::FLOAT) allInt = false;
            total *= asDouble(e);
            if (e->type() == ObjectType::INTEGER) totalInt *= static_cast<IntegerObject*>(e.get())->value;
        }
        if (allInt) return makeObj<IntegerObject>(totalInt);
        return makeObj<FloatObject>(total);
    });

    mod->frozen = true;
    mod->moduleName = "math";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}


} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_MATH_HPP
