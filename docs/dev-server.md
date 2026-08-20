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

1. invokes the configured Clang 18 compiler to emit LLVM IR;
2. applies `cvite-lowering`, `cvite-baseline`, and
   `cvite-baseline-manifest`;
3. emits a position-independent object;
4. links that object into the CVite process with LLVM ORC/JITLink;
5. registers compiler-generated stable functions and persistent storage;
6. calls the transformed program's real C `main` on the process main thread;
7. watches the source directory with Linux `inotify`.

On save, the watcher compiles a candidate through `cvite-candidate`, stages it
in an isolated JIT generation, validates its function and storage manifest, and
publishes one immutable dispatch snapshot. The previous generation remains
active unless every stage succeeds.

A successful edit looks like:

```text
[cvite] refreshed 3 functions → generation 4 (31.7 ms)
```

A syntax error is reported by Clang and followed by:

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
- one watched `.c` file;
- debug/`-O0` development compilation;
- exactly one supported C `main` entry;
- no build-system flags, link libraries, or header dependency watcher yet;
- JIT generations are retained until process exit.

Included headers are compiled normally, but edits to them do not yet trigger a
refresh. Multi-translation-unit projects, `compile_commands.json`, dependency
invalidation, custom link inputs, and process-restart fallback are the next
orchestration milestones.

## Lifetime policy

The active ORC loader is intentionally retained after the transformed `main`
returns. This keeps JIT code available while normal process-exit and `atexit`
handling runs, and avoids unloading code that a surviving application thread
could still be executing. Epoch/quiescence-based reclamation will eventually
replace this conservative process-lifetime retention.
