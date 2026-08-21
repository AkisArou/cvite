# Development server

The first executable vertical slice of CVite is available through:

```console
cvite run app.c
```

For a project directory, CVite discovers exactly one of `main.c` or
`src/main.c` as the entry translation unit:

```console
cvite run .
```

Application arguments follow `--`:

```console
cvite run . -- --level arena.json
```

Application source remains ordinary C. The development server does not require
CVite headers, macros, annotations, state APIs, allocators, or lifecycle
callbacks.

## Current loop

On startup, CVite:

1. locates `compile_commands.json` or `build/compile_commands.json` near the
   project;
2. asks libclang for every C compile command in the entry program and
   normalizes each translation unit's project flags;
3. invokes Clang 18 separately for each translation unit, producing LLVM IR and
   a Make-style dependency file;
4. applies `cvite-origin` to each module before linking, canonicalizing
   translation-unit-local function and storage identities from the original
   source path;
5. links the canonical modules with `llvm-link` into one program IR module;
6. applies `cvite-lowering`, `cvite-baseline`, and
   `cvite-baseline-manifest` to the linked program;
7. emits one position-independent object and links it into the CVite process
   with LLVM ORC/JITLink;
8. registers compiler-generated stable functions and persistent storage;
9. calls the transformed program's real C `main` on the process main thread;
10. watches every translation unit, the compilation database, and every
    non-system header reported by Clang.

CVite watches dependency **directories** and filters events by file name. This
continues to work when an editor saves through an atomic rename instead of
writing the original inode in place.

## Multi-translation-unit refresh

CVite keeps an origin-tagged LLVM IR cache for every selected translation
unit. A file-system event starts a candidate transaction, but only units whose
source or Clang-reported dependencies are newer than their active cache are
recompiled:

```text
worker.c changes
      │
      ├─ compile worker.c → staged worker IR
      ├─ reuse active main.c IR
      ├─ reuse active renderer.c IR
      └─ llvm-link one complete candidate program
                         │
                         ▼
            validate function/storage manifests
                         │
                         ▼
             publish one dispatch snapshot
                         │
                         ▼
          commit staged TU caches atomically
```

A shared-header edit invalidates every unit whose depfile names that header. A
source-only edit normally recompiles one unit. The complete set of active and
staged modules is still linked and validated as one program, so runtime
publication remains all-or-nothing across translation-unit boundaries.

Failed compilation, IR linking, ABI checks, or storage-layout checks discard
all staged cache entries. The old IR cache, dependency graph, JIT code, and
runtime dispatch snapshot remain active.

The `cvite-origin` pass runs before `llvm-link`. It records each function and
global object's original source file and declaration name. This prevents
linker-generated renames from changing the identities of same-named private
functions, file statics, and static locals in different `.c` files.

## Candidate behavior

On save, the watcher verifies that the active project model is unchanged,
recompiles only invalidated translation units, links staged and cached IR into a
complete candidate program, and runs `cvite-candidate`. The result is staged in
an isolated ORC generation, validated against the active function and storage
registries, and published atomically.

After publication, all candidate dependency files replace the active watch
graph as one set. Headers introduced by the edit therefore become refreshable
immediately, and headers no longer used by any translation unit are removed.
The compilation database itself is watched. Changing its translation-unit set
or normalized compile flags currently requests a safe process restart rather
than mutating the sealed host registry or silently compiling with mixed project
semantics.

A successful edit looks like:

```text
[cvite] recompiled 1/4 translation units
[cvite] refreshed 17 functions from 1/4 TUs → generation 4 (31.2 ms)
```

A syntax error in any source or watched header is reported by Clang and followed
by last-known-good behavior:

```text
[cvite] candidate compilation failed for /project/src/physics.c; previous code remains active
```

Cross-TU link failures, ABI changes, and persistent-layout incompatibilities are
rejected in the same way.

## Compilation database

CVite uses Clang's public compilation-database API rather than implementing a
second JSON parser or a second interpretation of C compiler commands. Both the
`arguments` and `command` forms accepted by libclang are supported.

Every selected translation unit retains its own project semantics, including:

- `-D` and `-U` definitions;
- language-dialect flags;
- include, quote, and system include paths;
- target and machine-feature flags;
- warning, extension, sanitizer, and project-specific frontend options.

Relative include, sysroot, response-file, and resource paths are resolved
against the command's recorded working directory. CVite removes the old source,
output, dependency, optimization, debug, PIC, and compile-mode arguments, then
appends its controlled development pipeline. This prevents an old `-o`, `-c`,
`-O3`, or depfile option from escaping the Fast Refresh build boundary.

The initial target-selection rule accepts C commands under the project root and
requires the discovered entry source to be present. Projects whose compilation
database contains several unrelated executables should provide a target-scoped
database for now. Target-aware graph selection is planned before general M2
project support.

When no usable compilation database exists, CVite discovers sibling `.c`
files beside the entry translation unit and compiles them with C11 development
defaults. A compilation database remains the authoritative path for real project
flags and source selection. If an already-active database disappears or no
longer describes the entry program, the candidate is rejected and the previous
code remains active.

## Toolchain selection

Build-tree defaults are embedded by CMake. They can be overridden without
changing application source:

```console
CVITE_CLANG=/usr/bin/clang-18 \
CVITE_OPT=/usr/bin/opt-18 \
CVITE_LLVM_LINK=/usr/bin/llvm-link-18 \
CVITE_PASS_PLUGIN=/path/to/CViteLoweringPass.so \
cvite run .
```

Set `CVITE_VERBOSE=1` to print every compiler and LLVM command.

`cvite doctor` reports the compiler, optimizer, IR linker, pass plugin,
compilation-database capability, and project mode.

## Current constraints

The current command remains an experimental Linux/Clang vertical slice:

- Linux with `inotify`;
- Clang/LLVM 18;
- C11-compatible application code and debug/`-O0` development generation;
- one supported C `main` across the linked translation units;
- incremental per-TU compilation with a whole-program LLVM relink;
- compilation-database C commands under one project root;
- CMake File API, CMake `link.txt`, and Ninja command discovery for native
  support inputs;
- no automatic layout-changing state migration;
- JIT generations retained until process exit.

The current TU cache narrows compilation while retaining atomic project-wide
publication. Changed-function filtering, smallest-boundary restart, and safe
old-generation reclamation remain later orchestration layers.

## Lifetime policy

The active ORC loader is intentionally retained after the transformed `main`
returns. This keeps JIT code available while normal process-exit and `atexit`
handling runs, and avoids unloading code that a surviving application thread
could still be executing. Epoch/quiescence-based reclamation will eventually
replace this conservative process-lifetime retention.

## Automatic native link discovery

CVite reconstructs the native support environment of the selected executable
without requiring application source changes. Discovery is backend-based and
uses the first unambiguous source of target metadata:

1. **CMake File API.** CVite reads the newest codemodel-v2 reply, matches an
   executable target against the selected translation units, and consumes its
   `link.commandFragments`.
2. **CMake `link.txt`.** Older or non-queried CMake builds fall back to the
   matching `CMakeFiles/<target>.dir/link.txt` command.
3. **Ninja command tool.** Standalone Ninja projects are inspected through
   `ninja -C <build> -t commands`; CVite scores executable link commands against
   the compilation-database sources.

The discovery layer recognizes `-L`, `-l`, `-l:filename`, direct shared-library
paths, static archives, standalone support objects, and rpath search hints.
Dynamic libraries are loaded into an LLVM ORC platform JITDylib. Static archives
and support objects are linked into dedicated ORC namespaces. Every baseline
and candidate generation receives the same namespaces in its link order, so
external symbol addresses remain stable across compatible refreshes.

CMake File API index, codemodel, and target reply files are watched. Ninja's
`build.ninja`, recursively referenced `include`/`subninja` files, compilation
database, and every resolved native input are also part of the active native
plan. A successful rediscovery replaces these watches as one set.

CVite reads explicit target options from the Clang compilation database and,
when none are present, asks the selected Clang driver for its default target.
A target whose architecture or operating-system family differs from the host JIT
is rejected before native code is published. One in-process CVite session cannot
execute a foreign architecture.

The native plan fingerprints the normalized target, selected backend, target
name, link tokens, resolved input kinds and paths, and the **contents** of native
inputs. This catches replacement of a shared library, archive, or support object
at the same pathname—even when an editor or build tool swaps the inode
atomically. Metadata-only timestamp changes that resolve to an identical plan do
not restart the application.

A native input or link-plan change is a red refresh. CVite re-executes itself so
the edited program starts from a fresh baseline and coherent native symbol
graph:

```text
[cvite] a native link input changed; restarting the process
```

Set `CVITE_DISABLE_AUTO_RESTART=1` to inspect the rejection without restarting.
`CVITE_PRELOAD` remains available as an explicit override for plugin systems,
unusual build systems, and libraries whose ownership cannot be inferred.
`CVITE_NINJA` can override the Ninja executable used by the discovery backend.
