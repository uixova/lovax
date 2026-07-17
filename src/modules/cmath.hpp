#ifndef LOVAX_MODULE_CMATH_HPP
#define LOVAX_MODULE_CMATH_HPP

#include "common.hpp"
#include <complex>

// cmath — complex-number math (RFC-018). The literal `3 + 4j` and the
// arithmetic operators live in the core; this module holds the functions,
// mirroring Python's cmath (studied, not copied).

namespace Lovax {
namespace StdLib {

inline ObjPtr makeCmathModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    // Reads a complex out of an argument (numbers promote to re+0j).
    auto asComplex = [](const ObjPtr& o, std::complex<double>& out) -> bool {
        switch (o->type()) {
            case ObjectType::COMPLEX: {
                auto* c = static_cast<ComplexObject*>(o.get());
                out = {c->re, c->im};
                return true;
            }
            case ObjectType::INTEGER:
                out = {(double)static_cast<IntegerObject*>(o.get())->value, 0.0};
                return true;
            case ObjectType::FLOAT:
                out = {static_cast<FloatObject*>(o.get())->value, 0.0};
                return true;
            default:
                return false;
        }
    };

    // make(re, im): builds a complex from two numbers
    def("make", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("make", "2", args.size(), line);
        if (!isNumeric(args[0]) || !isNumeric(args[1])) {
            return makeError("cmath.make(re, im) expects two numbers", line);
        }
        return makeObj<ComplexObject>(asDouble(args[0]), asDouble(args[1]));
    });

    // One-argument complex -> complex functions
    auto unary = [asComplex, &def](const std::string& name,
                                   std::complex<double>(*f)(const std::complex<double>&)) {
        def(name, [asComplex, name, f](const Args& args, int line, const CallFn&) -> ObjPtr {
            if (args.size() != 1) return argCountError(name, "1", args.size(), line);
            std::complex<double> z;
            if (!asComplex(args[0], z)) {
                return makeError("cmath." + name + "() expects a complex or number", line);
            }
            auto r = f(z);
            return makeObj<ComplexObject>(r.real(), r.imag());
        });
    };
    unary("sqrt", +[](const std::complex<double>& z) { return std::sqrt(z); });
    unary("exp",  +[](const std::complex<double>& z) { return std::exp(z); });
    unary("log",  +[](const std::complex<double>& z) { return std::log(z); });
    unary("sin",  +[](const std::complex<double>& z) { return std::sin(z); });
    unary("cos",  +[](const std::complex<double>& z) { return std::cos(z); });
    unary("conj", +[](const std::complex<double>& z) { return std::conj(z); });

    // abs(z) -> magnitude (float), arg(z) -> phase angle (float)
    def("abs", [asComplex](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("abs", "1", args.size(), line);
        std::complex<double> z;
        if (!asComplex(args[0], z)) return makeError("cmath.abs() expects a complex or number", line);
        return makeObj<FloatObject>(std::abs(z));
    });
    def("arg", [asComplex](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("arg", "1", args.size(), line);
        std::complex<double> z;
        if (!asComplex(args[0], z)) return makeError("cmath.arg() expects a complex or number", line);
        return makeObj<FloatObject>(std::arg(z));
    });

    // polar(r, theta): complex from magnitude + angle
    def("polar", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 2) return argCountError("polar", "2", args.size(), line);
        if (!isNumeric(args[0]) || !isNumeric(args[1])) {
            return makeError("cmath.polar(r, theta) expects two numbers", line);
        }
        auto z = std::polar(asDouble(args[0]), asDouble(args[1]));
        return makeObj<ComplexObject>(z.real(), z.imag());
    });

    mod->frozen = true;
    mod->moduleName = "cmath";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_CMATH_HPP
