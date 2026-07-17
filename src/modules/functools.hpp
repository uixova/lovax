#ifndef LOVAX_MODULE_FUNCTOOLS_HPP
#define LOVAX_MODULE_FUNCTOOLS_HPP

#include "common.hpp"

// functools — partial application and memoization. (reduce is a core builtin.)
//
// GC note: both wrappers capture Lovax objects inside C++ lambdas, which the
// tracing collector cannot see. Every captured object is permanent-rooted at
// creation — wrappers live for the program, which is how they are used.

namespace Lovax {
namespace StdLib {

inline ObjPtr makeFunctoolsModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    auto isFn = [](const ObjPtr& o) {
        return o->type() == ObjectType::FUNCTION || o->type() == ObjectType::BUILTIN;
    };

    // partial(fn, a, b, ...): a new function with the leading arguments bound
    def("partial", [isFn](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.empty() || !isFn(args[0])) {
            return makeError("functools.partial(fn, args...) expects a function first", line);
        }
        ObjPtr fn = args[0];
        std::vector<ObjPtr> bound(args.begin() + 1, args.end());
        gcPermanentRoot(fn.get());
        for (auto& b : bound) gcPermanentRoot(b.get());
        return makeObj<BuiltinObject>("partial",
            [fn, bound](const Args& callArgs, int line2, const CallFn& call) -> ObjPtr {
                std::vector<ObjPtr> full = bound;
                for (const auto& a : callArgs) full.push_back(a);
                return call(fn, full, line2);
            });
    });

    // memoize(fn): caches results by argument values (repr-keyed).
    // The wrapped function must be pure for this to be meaningful.
    def("memoize", [isFn](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || !isFn(args[0])) {
            return makeError("functools.memoize(fn) expects a function", line);
        }
        ObjPtr fn = args[0];
        gcPermanentRoot(fn.get());
        auto cache = makeObj<MapObject>();       // Lovax map: GC-visible storage
        gcPermanentRoot(cache.get());
        Ref<MapObject> cacheRef = cache;
        return makeObj<BuiltinObject>("memoized",
            [fn, cacheRef](const Args& callArgs, int line2, const CallFn& call) -> ObjPtr {
                std::string key;
                for (const auto& a : callArgs) {
                    key += inspectQuoted(a);
                    key += '\x1f';               // unit separator between args
                }
                if (auto hit = cacheRef->getStr(key)) return hit;
                auto result = call(fn, callArgs, line2);
                if (isError(result)) return result;
                cacheRef->setStr(makeObj<StringObject>(key), key, result);
                return result;
            });
    });

    mod->frozen = true;
    mod->moduleName = "functools";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_FUNCTOOLS_HPP
