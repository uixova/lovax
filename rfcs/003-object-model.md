# RFC-003: Object Model — struct + Composition, No Inheritance

**Status:** DRAFT (planned)

## Motivation
Deep inheritance hierarchies (Entity → Character → NPC → Vendor...) are brittle in game code;
modern engines moved to composition/ECS. Go rejected inheritance for the same reason.

## Proposal (draft)
- `struct Name:` with field declarations + methods.
- NO inheritance. Reuse = composition (holding another struct as a field).
- The engine-side component system (v1.0) will be the main vehicle for shared behavior.
- Designed together with optional type hints.

## Open questions
- Method syntax; constructor convention; struct equality (deep vs identity).
