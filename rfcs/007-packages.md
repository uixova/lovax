# RFC-007: Packages — `lovax install`, `lovax_libs/`, and the Road to a Registry

**Status:** ACCEPTED (phase 1 implemented)

## Motivation
Users of other ecosystems don't clone repositories to use a library — they run one
command (`npm install`, `pip install`) and import by name. Lovax needs the same flow,
and open-source users must be able to publish their own libraries.

## Phase 1 (implemented)

**Convention — `lovax_libs/`** (Lovax's `node_modules`):
```
my_game/
├── main.lov
└── lovax_libs/
    └── inventory/
        └── inventory.lov    # or main.lov
```

**Resolution.** `use <bare-name>` looks up, in order:
1. built-in modules (`math`, `game`, `text`, `file`, `os`);
2. `lovax_libs/<name>/<name>.lov`, then `lovax_libs/<name>/main.lov`
   (relative to the project root = the entry script's directory).

So an installed package imports exactly like a built-in: `use inventory`.
All module guarantees apply (cached once, cycle detection, frozen module map).

**Install command.**
```
lovax install user/repo          # GitHub shorthand
lovax install https://any.git/url.git
```
Shallow-clones into `lovax_libs/<repo-name>` via the system `git`, then strips `.git`
(a package is a plain folder, not a repository). Already-installed → clear error.
`lovax_libs/` belongs in the consuming project's own `.gitignore` or gets vendored —
the project's choice, like node_modules.

**Publishing a package** = pushing a repo whose root contains `<name>.lov` or `main.lov`.
Nothing else required.

## Phase 2 (with the installer, planned)
- `lovax.json` project manifest: name, version, `deps: {"user/repo": "v1.2.0"}`.
- `lovax install` with no arguments reads the manifest and installs everything (npm model);
  versions pin to git tags.
- Lockfile for reproducible builds.

## Phase 3 (registry, later)
- A central index (static JSON over HTTPS is enough at first): `lovax install inventory`
  without the user/ prefix, search, download counts.
- Ships together with the self-updating installer (stable/latest channels).
