#ifndef STDLIB_HPP
#define STDLIB_HPP

// Lovax built-in standard library (RFC-006).
// Modules NEVER enter the global scope until invited with 'use':
//   use math   -> math.lerp(0, 10, 0.5)
//   use game: pick_weighted, signal
//   use strings: upper, split
//   use file as f -> f.save_data(...)
// Every module is a FROZEN map: contents are immutable, the language cannot be broken.
//
// Each module lives in its own file under src/modules/ (one file per library,
// Lua-style). This header is the loader: it pulls them in and dispatches 'use'.

#include "builtins.hpp"
#include "../modules/math.hpp"
#include "../modules/game.hpp"
#include "../modules/strings.hpp"
#include "../modules/file.hpp"
#include "../modules/os.hpp"
#include "../modules/time.hpp"
#include "../modules/canvas.hpp"
#include "../modules/net.hpp"
#include "../modules/cmath.hpp"
#include "../modules/random.hpp"
#include "../modules/datetime.hpp"
#include "../modules/regex.hpp"

namespace Lovax {
namespace StdLib {

inline ObjPtr getBuiltinModule(const std::string& name) {
    if (name == "net") return makeNetModule();
    if (name == "math") return makeMathModule();
    if (name == "game") return makeGameModule();
    if (name == "strings") return makeTextModule();
    if (name == "file") return makeFileModule();
    if (name == "os")   return makeOsModule();
    if (name == "time") return makeTimeModule();
    if (name == "canvas") return makeCanvasModule();
    if (name == "cmath") return makeCmathModule();
    if (name == "random") return makeRandomModule();
    if (name == "datetime") return makeDatetimeModule();
    if (name == "regex") return makeRegexModule();
    return nullptr;
}

inline std::string builtinModuleList() {
    return "math, game, strings, file, os, time, canvas, net, cmath, random, datetime, regex";
}

} // namespace StdLib
} // namespace Lovax

#endif // STDLIB_HPP
