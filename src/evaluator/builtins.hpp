#ifndef BUILTINS_HPP
#define BUILTINS_HPP

// Core builtins live in src/modules/ (one file per concern, Lua-style):
//   modules/common.hpp — shared helpers (args, RNG, clone, permission gates, JSON core)
//   modules/base.hpp   — the global builtin functions (say/len/push/sort/...)
// This header keeps the historical include point: everything that included
// evaluator/builtins.hpp keeps compiling unchanged.

#include "../modules/common.hpp"
#include "../modules/base.hpp"

#endif // BUILTINS_HPP
