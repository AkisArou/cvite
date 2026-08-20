# Development server

The first executable vertical slice of CVite is available through:

```console
cvite run app.c
```

For a simple directory, CVite discovers exactly one of `main.c` or
`src/main.c`, so this also works:

```console
cvite run .
```

Application arguments follow `--`:

```console
cvite run . -- --level arena.json
```

The source remains ordinary C. The M1 runner does not require CVite headers,
macros, annotations, state APIs, allocators, or lifecycle callbacks.

## Current loop

On startup, CVite:

1. invokes the configured Clang 18 compiler to emit LLVM IR and a Make-style
   dependency file;
2. applies `cvite-lowering`, `cvite-baseline`, and
   `cvite-baseline-manifest`;
3. emits a position-independent object;
4. links that object into the CVite process with LLVM ORC/JITLink;
5. registers compiler-generated stable functions and persistent storage;
6. calls the transformed program's real C `main` on the process main thread;
7. watches the source file and every non-system header reported by Clang.

CVite watches dependency **directories** and filters events by file name. This
continues to work when an editor saves through an atomic rename instead of
writing the original inode in place.

On save, the watcher compiles a candidate through `cvite-candidate`, stages it
in an isolated JIT generation, validates its function and storage manifest, and
publishes one immutable dispatch snapshot. The previous generation remains
active unless every stage succeeds.

After a candidate is published, its Clang dependency file atomically replaces
the active watch graph. A header introduced by the edit therefore becomes
refreshable immediately; a header no longer used by the translation unit is
removed from the graph.

A successful edit looks like:

```text
[cvite] refreshed 3 functions → generation 4 (31.7 ms)
```

A syntax error in either the source or a watched header is reported by Clang and
followed by:

```text
[cvite] candidate compilation failed; previous code remains active
```

ABI and persistent-layout incompatibilities are rejected in the same
last-known-good manner.

## Toolchain selection

Build-tree defaults are embedded by CMake. They can be overridden without
changing application source:

```console
CVITE_CLANG=/usr/bin/clang-18 \
CVITE_OPT=/usr/bin/opt-18 \
CVITE_PASS_PLUGIN=/path/to/CViteLoweringPass.so \
cvite run app.c
```

Set `CVITE_VERBOSE=1` to print every compiler command.

`cvite doctor` reports the selected compiler, optimizer, pass plugin, and
project mode.

## M1 constraints

The current command is intentionally narrow:

- Linux with `inotify`;
- Clang/LLVM 18;
- one C11 translation unit;
- the translation unit's Clang-reported non-system header graph;
- debug/`-O0` development compilation;
- exactly one supported C `main` entry;
- no build-system compile flags, link libraries, or multi-translation-unit
  invalidation yet;
- JIT generations are retained until process exit.

`compile_commands.json`, multiple translation units, custom link inputs,
smallest-boundary process restart, and generation reclamation are the next
orchestration milestones.

## Lifetime policy

The active ORC loader is intentionally retained after the transformed `main`
returns. This keeps JIT code available while normal process-exit and `atexit`
handling runs, and avoids unloading code that a surviving application thread
could still be executing. Epoch/quiescence-based reclamation will eventually
replace this conservative process-lifetime retention.
