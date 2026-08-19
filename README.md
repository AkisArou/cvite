# CVite

**Fast Refresh for ordinary C.**

CVite is an experimental development runtime that aims to bring the React + Vite edit/save/refresh experience to native C programs without requiring application code to use CVite-specific state APIs, macros, annotations, allocators, or lifecycle hooks.

```console
$ cvite run .

  CVite

  ✓ project discovered
  ✓ application running
  ➜ watching source files

  14:32:08  physics.c changed
            ⚡ refreshed 3 functions

  14:32:14  game.c changed
            ✗ game.c:91:18: expected expression
            previous code is still running

  14:32:19  game.c changed
            ✓ error resolved
            ⚡ refreshed game_update
```

> [!IMPORTANT]
> CVite is currently at the architecture/bootstrap stage. The product contract below is the target; unsupported edits must fall back safely rather than pretending that arbitrary C memory can always be migrated.

## Product contract

A normal application should remain normal C:

```c
#include <stdlib.h>

typedef struct {
    float x;
    float y;
    int health;
} Player;

static Player player = {
    .x = 10.0f,
    .y = 20.0f,
    .health = 100,
};

void game_update(float dt)
{
    player.x += 100.0f * dt;
}

int main(void)
{
    Player *other = malloc(sizeof(*other));
    /* application code */
    free(other);
    return 0;
}
```

The default development workflow should require only the tool:

```console
cvite run .
```

No public `cv_state`, `CV_HOT`, `CV_COMPONENT`, custom `malloc`, or source rewrite is part of the normal programming model. Optional escape hatches may eventually exist for genuinely ambiguous native-memory cases, but they must not be required for the common path.

Production builds remain ordinary native builds and do not depend on the CVite runtime.

## What “Fast Refresh” means for C

CVite separates **hot module replacement** from **state-aware refresh**.

### Compatible edit

When a function body changes while its ABI and referenced data layouts remain compatible:

1. compile the changed translation unit;
2. keep the last known-good program running if compilation fails;
3. load the new implementation into the existing process;
4. atomically redirect future calls to it;
5. let already-active calls finish in their old implementation;
6. preserve globals, statics, heap allocations, OS resources, and process identity.

### Incompatible edit

C exposes raw pointers, interior pointers, unions, custom allocators, inline assembly, foreign libraries, and active native stack frames. Arbitrary state migration cannot always be proven safe.

CVite must therefore follow the same principle as framework Fast Refresh:

> Preserve compatible state, invalidate the smallest safe boundary when compatibility cannot be established, and use a full process restart as the final fallback.

A rejected refresh is a successful safety decision. CVite must never silently corrupt memory to avoid a restart.

## Refined architecture

CVite does **not** implement a custom C parser. Clang is the single semantic source of truth.

The architecture is deliberately split between an out-of-process development server and an in-process native runtime:

```text
                               ordinary C project
                                       │
                                       ▼
┌──────────────────────────────────────────────────────────────────────┐
│                         cvite development server                     │
│                                                                      │
│  project discovery  file watching  build graph  diagnostics         │
│          │               │             │             │               │
│          └───────────────┴─────────────┴─────────────┘               │
│                                  │                                   │
│                                  ▼                                   │
│                    Clang compilation service                         │
│              (Clang frontend APIs; no custom parser)                 │
│                                  │                                   │
│                       semantic refresh manifest                       │
│                                  │                                   │
│                                  ▼                                   │
│                         LLVM IR transforms                           │
│              stable calls  stable storage  metadata                  │
│                                  │                                   │
│                                  ▼                                   │
│                         versioned patch package                      │
└──────────────────────────────────┬───────────────────────────────────┘
                                   │ IPC
                                   ▼
┌──────────────────────────────────────────────────────────────────────┐
│                         cvite application host                       │
│                                                                      │
│  LLVM ORC JIT / object layer                                         │
│  stable function stubs                                               │
│  persistent global/static storage                                    │
│  compatibility gate                                                  │
│  atomic patch transaction                                            │
│  old-code lifetime management                                        │
│                                                                      │
│                  user application executes here                      │
└──────────────────────────────────────────────────────────────────────┘
```

### Why a JIT/object layer instead of rewriting live prologues

The first implementation will use stable function entry stubs and versioned implementations rather than overwriting arbitrary machine instructions in place.

Conceptually:

```text
stable address: game_update
        │
        └────► game_update.__cvite_v17

refresh
        │
        └────► game_update.__cvite_v18
```

All refreshable direct calls and function-address uses are made to resolve through the stable entry. Installing a compatible update becomes an atomic target swap. Active v17 frames may finish normally while new calls enter v18. Old code is reclaimed only after it is no longer executable by any thread; the bootstrap implementation may conservatively retain old versions.

This avoids instruction-tearing hazards and makes function-pointer identity stable.

### Clang semantic layer

A Clang-based frontend action produces a refresh manifest from the compiler’s real AST and target layout information. It records, among other things:

- stable declaration identities;
- function ABI fingerprints;
- global, static, and static-local identities;
- record/union/enum layouts;
- field identities and offsets;
- translation-unit and header dependencies;
- source locations and diagnostics;
- address-taken and escape-relevant facts where Clang can establish them.

The semantic layer is implemented with Clang libraries or a version-matched Clang frontend extension. It is not a parallel parser and must never disagree with the compiler about preprocessing, macros, typedefs, attributes, packing, target ABI, or conditional compilation.

### LLVM transformation layer

LLVM IR transformations provide the machine-level mechanics:

- rename each implementation to a versioned private symbol;
- route calls through stable dispatch stubs;
- prevent development-build optimizations that erase refresh boundaries;
- externalize refresh-persistent global/static storage;
- emit compact runtime metadata;
- add safe instrumentation only where required;
- preserve a clean, non-CVite production build path.

The initial implementation prioritizes correctness and observability over optimization.

### Persistent globals and statics

Global variables, internal-linkage globals, and static locals receive stable identities and storage addresses in the application host.

A compatible code refresh reuses the existing storage and does not rerun its initializer. This mirrors stateful refresh semantics: an edited initializer does not unexpectedly overwrite live application state.

Future versions may reserve virtual address space around selected persistent objects so certain append-only size changes can remain at the same address. Layout-changing migrations are accepted only when their safety can be proven.

### Heap state

Ordinary heap allocations naturally survive function replacement because the process does not restart.

The first safe contract is:

- heap state is preserved when relevant layouts are unchanged;
- a changed layout that may describe live heap objects invalidates the refresh;
- CVite falls back rather than guessing.

Later work may infer and instrument common typed allocation forms such as `malloc(sizeof(T))` and `calloc(n, sizeof(T))`. Transparent tracking can improve diagnostics and permit more local refresh decisions, but universal pointer relocation is not a prerequisite for a useful first release.

### Compile and runtime errors

A failed candidate build never replaces the active implementation. The application continues with the last known-good patch set while CVite reports diagnostics.

Patch installation is transactional:

1. validate the patch package;
2. compare old and new refresh manifests;
3. resolve every required symbol;
4. stage new code and metadata;
5. atomically publish compatible targets;
6. roll back the staged update if any pre-publication step fails.

Runtime-error recovery and an optional in-application diagnostic overlay are later layers. Terminal diagnostics are the initial interface.

## Refresh compatibility levels

Every edit is classified before publication:

| Level | Meaning | Action |
|---|---|---|
| Green | Function implementation changed; ABI and layouts are compatible | Atomically patch affected functions |
| Yellow | Compatible storage/schema evolution is provably safe | Migrate or extend state, then patch |
| Orange | Dependencies require a wider rebuild | Recompile and atomically publish the affected set |
| Red | State or ABI compatibility cannot be established | Reset the smallest safe boundary or restart |

The bootstrap milestone implements Green refresh and safe Red fallback first. Yellow refresh is deliberately not faked.

## Components

The repository will evolve toward this layout:

```text
cvite/
├── CMakeLists.txt
├── cmake/
├── include/cvite/
│   ├── protocol.h
│   └── runtime.h
├── src/
│   ├── cli/                 # cvite command and orchestration
│   ├── compiler/            # Clang frontend integration
│   ├── transform/           # LLVM IR transformation pipeline
│   ├── host/                # application host and patch transport
│   ├── runtime/             # stable stubs, state, transactions
│   └── protocol/            # versioned patch/manifest format
├── examples/
├── tests/
└── docs/
```

Implementation code may begin with a smaller vertical slice, but component boundaries must follow the architecture rather than a throwaway shared-library API exposed to users.

## Milestones

### M0 — repository and contracts

- architecture and safety contract;
- reproducible CMake build;
- CI and formatting baseline;
- versioned protocol skeleton;
- executable test harness.

### M1 — function-body Fast Refresh

- Linux x86-64;
- a pinned/supported Clang + LLVM toolchain range;
- ordinary C source;
- stable function stubs;
- versioned implementations;
- edit/save compilation;
- last-known-good behavior on compiler errors;
- global, static, heap, and process state preserved when layouts are unchanged;
- single-threaded example and integration test.

### M2 — project integration

- CMake `compile_commands.json` ingestion;
- multiple translation units and headers;
- dependency invalidation;
- added and removed functions;
- terminal diagnostics and timing;
- deterministic patch transactions.

### M3 — concurrency and lifecycle

- multithreaded patch publication;
- old-code quiescence/epoch management;
- thread-local storage policy;
- constructors/destructors policy;
- debugger/profiler integration tests.

### M4 — richer state compatibility

- semantic type-diff engine;
- persistent-global evolution;
- inferred typed-allocation registry;
- safe append-only migrations;
- precise explanations for rejected refreshes;
- smallest-safe-boundary reset where provable.

### M5 — portability

- macOS arm64/x86-64;
- Windows x64;
- additional build systems;
- packaged toolchains or strict compatibility checks.

## Initial constraints

The first working target is intentionally narrow:

- Linux x86-64;
- C11 application code;
- Clang/LLVM;
- debug/development optimization settings;
- no C++ application ABI;
- no promise of live struct migration;
- no unload of old code until lifetime safety exists.

These constraints protect the final DX. They are implementation limits, not source-level framework requirements.

## Design principles

1. **Ordinary C in, ordinary C out.** No required CVite source API.
2. **Clang is the parser.** CVite never implements a competing C frontend.
3. **Keep the last good program alive.** Candidate failures are development diagnostics, not process failures.
4. **Stable identity before migration.** Functions and storage need durable identities across compilations.
5. **Patch atomically.** A refresh is published completely or not at all.
6. **Prefer a safe restart to memory corruption.** Compatibility is proven, not assumed.
7. **Production remains native.** Instrumentation and runtime overhead are development-only.
8. **Build a vertical slice first.** The first demo must compile, run, refresh, and be tested end to end.

## Non-goals for the first release

- interpreting C;
- replacing Clang with a custom parser;
- transparently migrating every arbitrary pointer graph;
- hot-swapping active stack-frame layouts;
- promising zero restarts for ABI-breaking edits;
- supporting optimized production builds as refresh targets;
- requiring developers to rewrite applications around a CVite framework.

## Status

The repository is being bootstrapped. The next code change will create the M0 build/protocol/runtime skeleton and a testable vertical slice toward M1.
