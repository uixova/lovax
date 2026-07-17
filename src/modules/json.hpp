#ifndef LOVAX_MODULE_JSON_HPP
#define LOVAX_MODULE_JSON_HPP

#include "common.hpp"

// json — in-memory JSON (the file module's load_data/save_data share the same
// core in common.hpp; this exposes it for strings and network payloads).

namespace Lovax {
namespace StdLib {

inline ObjPtr makeJsonModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    // parse(text): JSON string -> value
    def("parse", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1 || args[0]->type() != ObjectType::STRING) {
            return makeError("json.parse(text) expects a string", line);
        }
        const std::string& src = static_cast<StringObject*>(args[0].get())->value;
        JsonParser parser(src);
        ObjPtr out;
        if (!parser.parseValue(out, 0)) {
            return makeError("json.parse: " + parser.err, line);
        }
        parser.skipWs();
        if (parser.i != src.size()) {
            return makeError("json.parse: trailing characters after value (position " +
                             std::to_string(parser.i) + ")", line);
        }
        return out;
    });

    // text(value): value -> compact JSON string
    def("text", [](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 1) return argCountError("text", "1", args.size(), line);
        std::string out, err;
        if (!jsonWrite(args[0], out, err, 0)) {
            return makeError("json.text: " + err, line);
        }
        return makeObj<StringObject>(out);
    });

    mod->frozen = true;
    mod->moduleName = "json";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_JSON_HPP
