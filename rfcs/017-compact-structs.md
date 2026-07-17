# RFC-017 — Compact struct instances (shape + slot array)

## Motivation

The cross-language benchmark exposed struct memory as Lovax's worst number:
the binary-tree benchmark peaked at **270 MB versus Lua's 24 MB (11×)**.
Root cause: `struct` compiled to a factory that built a full `MapObject` per
instance — an entries vector holding `__type__`, every field, and **every
method closure**, plus three per-instance hash indexes (string/int/bool).
A three-field node cost ~500+ bytes before its values.

## Design

One `StructShapeObject` per struct **type**, built once when the declaration
executes; instances shrink to a shape pointer plus a flat slot array.

- `StructShapeObject` — type name, `fieldNames` (slot → key), `fieldIndex`
  (name → slot), and the method table. Methods now compile **once, in the
  declaration scope** (previously each instantiation re-created every method
  closure inside the factory).
- `StructInstanceObject` — `{ StructShapeObject* shape; vector<Ref<Object>> slots; }`.
- Three opcodes: `STRUCT_SHAPE` (build shape from consts + method closures on
  the stack), `STRUCT_BIND` (attach shape to the factory closure),
  `STRUCT_MAKE` (factory epilogue: locals[0..n) → slots; defaults still run as
  bytecode via `ARG_DEFAULT`, unchanged).
- Member access: the existing inline-cache slot now stores the **slot index**,
  verified against `shape->fieldNames[ic]` — a stale cache can only miss,
  never read the wrong field. Methods, `__type__`, and miss diagnostics
  resolve through `evalMemberAccess`.
- `p["field"]` read/write, `keys/values/len/has/get/copy/clone`, equality
  (same shape + equal slots), and `inspect` all support instances.

## Breaking change (approved)

Writing an **undeclared** field (`p.z = 1` where `z` was never declared) is now
a runtime error: `struct 'P' has no field 'z' (fields are fixed at declaration)`.
Previously it silently grew the instance map. This catches field-name typos at
the assignment site and is what makes the flat slot layout possible.

## Results (same machine as benchmarks/cross)

| Metric | before | after |
|---|---:|---:|
| btree peak RSS | 270 MB | **45 MB** (6.0×; GC-threshold accounting closes the rest — see v0.12.3) |
| golden | 68/68 | 70/70 (2 new struct cases), both dispatch modes, bit-for-bit |
| GC_STRESS + ASan/UBSan/Leak | clean | clean |

## Alternatives rejected

- **Overflow side-map for dynamic fields** — keeps `p.z = 1` working but costs
  a pointer per instance and keeps the typo-hiding behavior; user chose strict.
- **Inline fixed-size slot array** — saves the vector header; deferred, the
  JIT-era object layout pass owns that decision.
