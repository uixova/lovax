# RFC-022 — structured errors: the kind convention

## Design (Lua-spirit: no exception hierarchy)

`throw` now carries STRUCTURED values intact. When the thrown value is a map,
struct, list or tuple, `catch e` binds the ORIGINAL value — so `e.kind`,
`e.msg`, `t[0]` work naturally in the handler. Strings (and builtin runtime
errors) still bind the message string: zero breakage, all 79 pre-existing
golden outputs byte-identical.

The convention: `throw error("io", "disk full")` — the new `error(kind, msg)`
builtin builds `{kind, msg}`; handlers branch with
`if kind(e) == "map" and e.kind == "io"`.

Mechanics: `ErrorObject` gained a GC-marked `payload`; `THROW_` attaches it
for structured values; the unwind path pushes the payload when present.

## v2 (post-engine)

Kind tags on BUILTIN runtime errors ("type", "index", "perm"…) — needs a
golden sweep since every caught-error message is golden-locked.
