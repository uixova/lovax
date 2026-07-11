# Changelog

## v0.5.0 — first public release

### Language
- Indentation-based, Python-simple syntax; full UTF-8 identifiers (Turkish-friendly).
- Number literals: hex (`0xFF`), binary (`0b1010`), `_` separators, scientific (`1e6`).
- Operators: full arithmetic with floor `/` and `%` (identity-consistent), `**` (right-assoc),
  comparisons with deep equality, `and/or/not` (short-circuit), `in` membership,
  bitwise `& | ^ ~ << >>` (Python precedence), compound assignment `+= -= *= /= %=`.
- Control flow: `if / else if / else`, `match` (multi-pattern, `_` wildcard, no fallthrough),
  ternary (`a if cond else b`), `while`, `for/in` over list/range/string/map, `break/continue`.
- Functions: closures, recursion, default parameters (call-time evaluation),
  anonymous functions, strict arity, call-depth protection.
- Strings: interpolation `"hp: {hp * 2}"` (full expressions), escapes, multiline `"""..."""`.
- Data: list, insertion-ordered map with dot access (`player.hp`), lazy range.
- `set` defines / bare assignment updates (typo protection, RFC-001).

### Module System (RFC-006)
- `use math` / `use x as y` / `use x: a, b` / `use "file.lm"` — built-ins load only when invited.
- Modules are frozen maps; user `.lm` files are modules too (cached, cycle-detected,
  importer-relative paths).

### Built-in Modules
- `math`: lerp, clamp, remap, wrap, move_toward, dist, snap, full trig, exp/log, PI/TAU.
- `game`: Penner easing, pick/pick_weighted/shuffle (loot), signals, poll-based timers.
- `strings`: split, join, Turkish-aware upper/lower, trim, replace, count, padding, chr/ord, fixed.
- `file`: text/lines/binary/JSON/CSV read-write, exists, list_dir, make_dir, delete_file.
- `os`: env, set_env, platform, cwd, args, path_join.

### Packages (RFC-007)
- `lume install user/repo` fetches libraries into `lume_libs/`; `use <name>` imports
  installed packages exactly like built-ins.

### Quality
- Located, helpful English error messages; syntax errors prevent execution entirely.
- 53 golden-file tests, benchmark suite, ASan/UBSan clean.
