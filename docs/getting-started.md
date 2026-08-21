# Getting started

CVite is currently a Linux/Clang 18 research prototype. It is suitable for
experimentation, examples, and continued development; it is not yet a stable
replacement for a normal production build.

## Bootstrap on Ubuntu 24.04

From the repository root:

```console
./scripts/bootstrap-ubuntu-24.04.sh
export PATH="$PWD/.local/bin:$PATH"
cvite doctor
```

The script installs the pinned development dependencies, configures the
`llvm18` CMake preset, builds the compiler/runtime components, runs the test
suite, and installs CVite beneath `.local` by default. Set
`CVITE_INSTALL_PREFIX` to use another installation prefix.

The equivalent manual commands are:

```console
cmake --preset llvm18
cmake --build --preset llvm18
ctest --preset llvm18
```

## Run the ordinary-C example

The example does not include CVite headers and does not use CVite macros,
allocators, state wrappers, or callbacks.

```console
cmake -S examples/live-counter \
      -B examples/live-counter/build \
      -G Ninja \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

ln -sfn build/compile_commands.json \
        examples/live-counter/compile_commands.json

cvite run examples/live-counter
```

While it is running, edit `examples/live-counter/counter.c` and change the
increment in `counter_step`. Saving a compatible function-body edit should
change subsequent behavior without resetting the live counter.

A syntax error leaves the last known-good implementation running. A function
ABI, persistent-storage layout, TLS, constructor/destructor, translation-unit
set, target, or native-link-plan change takes the controlled restart path.

## Semantic layout inspection

The compiler build also installs `cvite-semantic-index`. It uses libclang's
actual AST and target layout; it is not a second C parser.

```console
cvite-semantic-index index \
  --source old_player.c \
  --output old_player.cvsidx \
  -- -std=c11 -Iinclude

cvite-semantic-index index \
  --source new_player.c \
  --output new_player.cvsidx \
  -- -std=c11 -Iinclude

cvite-semantic-index diff \
  --old old_player.cvsidx \
  --new new_player.cvsidx
```

The diff identifies record size/alignment changes and field additions,
removals, type changes, offset changes, and bit-field changes. An append-only
classification means the source layout is structurally append-only; it does
**not** mean arbitrary live C storage can already be migrated safely.

## Current boundaries

- Linux with `inotify`.
- Clang/LLVM 18.
- C11-compatible application source and development-oriented compilation.
- One supported C `main` entry.
- Adding or removing a translation unit restarts the process.
- Incremental translation-unit compilation still performs a complete candidate
  LLVM relink.
- Arbitrary changed struct layouts and unmanaged pointer graphs are not
  migrated transparently.
- Address-escaped or inline-assembly generations may be pinned conservatively.
- No packaged macOS or Windows backend yet.

Release builds remain ordinary native builds; applications do not ship the
CVite development runtime unless their build explicitly links it.
