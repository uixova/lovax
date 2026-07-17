# RFC-018 — Core types: tuple, Set, bytes, complex

## Motivation

The Python-3.16/Lua-5.5 gap analysis (ANALYSIS_*.md, feature-matrix Tier 1)
flagged four missing fundamentals. All four land together in v0.13 — each is a
new `ObjectType` on the existing GC heap; no value-representation change.

## tuple — `(a, b)`

- Immutable sequence. `TupleObject` **shares `ListObject`'s layout**, so every
  element-walking path (iteration, unpack, len) reuses the list code; the TUPLE
  tag alone blocks mutation (index-set, push, insert reject it).
- Literals: `(a, b)`, one-tuple `(5,)`, empty `()` — the comma makes the tuple,
  `(x)` stays grouping (Python's rule).
- **Multi-return**: `return a, b` builds a tuple; `set q, r = f()` unpacks it
  (`UNPACK` accepts list or tuple).
- Indexing (negative ok), iteration, `in`, deep equality (tuple ≠ list),
  `len/contains/find/first/last/sum`.

## Set — `Set([1, 2, 3])`

- Unique elements (string/int/bool — the map-key set), insertion-ordered.
  Named `Set` (capitalized) because lowercase `set` is the declaration keyword.
- `SetObject` **inherits `MapObject`** — a set is a value-less map, so the
  typed indexes, lookup, removal and key-snapshot iteration are all reused.
- Operators: `a | b` union, `a & b` intersection, `a - b` difference;
  `x in s`; `==` is order-insensitive.
- Builtins: `Set()/Set(list)`, `add`, `remove`, `has`, `contains`, `len`,
  `values` (elements as list), `clone`. Prints as `Set{1, 2}`.

## bytes — `bytes([72, 101])` / `bytes("text")`

- Immutable binary data (`std::string` storage). Index → int 0-255, slice →
  bytes, `len` = byte count, `text(b)` decodes (raw passthrough), `sum`.
  Prints as `b"He\x01"`.
- `file.read_bytes` now **returns bytes** (was list-of-ints — breaking, golden
  updated); `file.write_bytes` accepts bytes or the old list form;
  `net.send` accepts bytes.
- This is the Tier-3 foundation: future hash/compress/binary-protocol modules
  are one-file additions over bytes.

## complex — `3 + 4j`

- `ComplexObject` (heap: two doubles cannot fit the 16-byte tagged Value —
  CPython also heap-allocates).
- Lexer: number + `j` suffix → imaginary literal. Arithmetic `+ - * / **`
  against complex/numbers in the runtime slow path (`std::complex`, C++
  stdlib — still zero third-party deps). Unary `-`, `.real`/`.imag`, `abs(z)`,
  `3+0j == 3` (Python rule).
- `use cmath`: `make, sqrt, exp, log, sin, cos, conj, abs, arg, polar`.

## Gates (all green)

74/74 golden bit-for-bit in both dispatch modes (4 new cases: 45-tuple,
46-set, 47-bytes, 48-complex; 32-os-bytes + e14 regenerated),
GC_STRESS + ASan/UBSan/Leak clean, fuzz + sandbox green, bench flat,
**3150-call new-type fuzz (every builtin × every new type + operator matrix)
under ASan: 0 crashes**.
