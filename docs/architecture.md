# Architecture

## Product boundary

CVite is a development tool around ordinary C. Application source does not use a
CVite framework API. A development build is produced through a compiler-driver
wrapper; a production build remains a normal native build.

Clang is the only C parser and semantic authority. CVite never tokenizes C into a
parallel AST and never guesses layouts from source text.

## Decision: hybrid native baseline plus JIT-linked patches

The baseline application remains a normal instrumented native executable. CVite
does **not** require the whole program, including `main`, to run under a JIT.
Only candidate patch objects are loaded into the existing process through LLVM
ORC/JITLink.

```text
ordinary source + existing build flags
                 │
                 ▼
        cvite compiler wrapper
                 │
       ┌─────────┴──────────┐
       │                    │
       ▼                    ▼
 normal native link    semantic manifest
       │                    │
       ▼                    │
instrumented baseline       │
       │                    │
       └─────────┬──────────┘
                 │
          running process
                 │
source edit ─► candidate object ─► ORC/JITLink ─► staged generation
                                                   │
                                      compatibility + resolution
                                                   │
                                                   ▼
                                      atomic dispatch publication
```

This preserves native startup, existing static/dynamic-library integration,
debugger behavior, operating-system resources, and the project's linker model.
ORC supplies the patch-object linker, symbol namespaces, resource tracking, and
process/library symbol reflection; it is not the application runtime model.

## Compiler-service boundary

CVite ships and tests a version-matched Clang/LLVM toolchain boundary. It must not
silently load a binary plugin into an arbitrary incompatible system compiler.
There are two compiler-side stages:

1. **Semantic indexing.** The bootstrap implementation uses libclang's stable C
   API to obtain Clang USRs, canonical types, target layouts, source locations,
   dependencies, and diagnostics. If the stable API proves insufficient for a
   required feature, a version-matched LibTooling frontend action can enrich the
   same manifest without changing the product boundary.
2. **IR transformation and ABI extraction.** A version-matched LLVM pass creates
   stable call entries, versioned implementations, persistent-storage symbols,
   and machine-accurate ABI fingerprints after Clang code generation.

The semantic manifest's `semantic_fingerprint` is useful for dependency and
schema comparison but is deliberately not called an ABI fingerprint. The final
ABI gate must include the lowered LLVM function type, calling convention,
parameter/return attributes, data layout, target triple, and CVite ABI-schema
version.

Compiler work runs outside the application process. A syntax error, compiler
crash, or malformed candidate cannot destroy the active generation.

## Stable function identity

Each refreshable function has two identities:

- a stable callable entry whose address never changes during the process;
- a versioned implementation supplied by the baseline or a later patch.

Conceptually:

```text
foo                    stable public address
  └─ dispatch slot 17
       ├─ generation 5 ─► foo.__cvite_impl.5
       └─ generation 6 ─► foo.__cvite_impl.6
```

The LLVM transform splits original definitions into stable entries and versioned
implementations. Direct calls and function-address expressions resolve to the
stable entry. Function pointers passed to uninstrumented libraries therefore
retain identity across compatible refreshes.

Inlining across a refresh boundary is disabled initially. Later optimized
development builds may record inlining dependencies and rebuild every affected
caller before publication.

## Transactional dispatch

A patch never mutates function slots one by one. It builds an immutable dispatch
snapshot, validates the complete candidate, and publishes one atomic root
pointer. The M0 runtime implements this contract now.

A thread can pin one snapshot for an outermost refreshable call chain. Nested
calls then remain on one generation even if publication occurs concurrently.
Generation pinning and epoch-based reclamation are later runtime work; M0
conservatively retains old snapshots and code.

## Patch staging

A candidate generation is publishable only after all of these steps succeed:

1. compile every invalidated translation unit;
2. build semantic and lowered-ABI manifests;
3. compare declarations, storage layouts, and dependencies;
4. add candidate objects to an isolated ORC resource tracker;
5. resolve all stable application and external-library symbols;
6. run link-time validation and finalize executable memory;
7. construct the complete dispatch snapshot;
8. publish the snapshot with one release operation.

Failure before step 8 removes or abandons the staged resources and leaves the
last known-good generation active.

## Symbol resolution

The baseline transform records addresses for stable entries and persistent data.
The runtime defines these for ORC as explicit absolute symbols. Exported process
and dynamic-library symbols may be exposed through a filtered dynamic-library
search generator. Internal application symbols are never assumed to be visible
through the platform dynamic symbol table; CVite registers them from its own
manifest.

This also gives the runtime a place to enforce symbol policy rather than exposing
every process symbol to candidate code by default.

## Persistent storage

Global variables, internal-linkage globals, and static locals have stable storage
identities. The baseline owns or registers their initial addresses. Candidate
code resolves those same identities and does not rerun initializers during a
compatible body refresh.

M0 accepts only exact storage-layout fingerprints. A changed initializer does
not overwrite live state. A changed size, alignment, field layout, TLS model, or
other storage contract rejects the refresh until a migration strategy can be
proved safe.

Ordinary heap allocations survive because the process does not restart. Layout
changes that may describe live heap objects are rejected in the first releases.
Transparent allocation/pointer analysis may later permit narrower decisions, but
universal relocation of arbitrary C pointer graphs is not a prerequisite and is
not promised.

## Stable identities and collision handling

Semantic entities use versioned 128-bit IDs derived from Clang USRs plus project
and source identity where linkage requires it. The bootstrap hash is explicitly
non-cryptographic. Manifests retain the complete canonical identity input, and a
single generation must reject two different identities that map to one ID.

A future hash change increments the manifest/identity schema. Stable source DX
does not depend on a particular hashing algorithm.

## Error behavior

### Candidate compile or link errors

The running process keeps executing the previous generation. Diagnostics are
reported by the development server. Fixing the source produces a new candidate;
there is no need to restart merely because an intermediate edit did not compile.

### Native runtime faults

React can recover many render exceptions because React controls component
execution. Unrestricted C can corrupt memory or receive a fatal signal. CVite's
initial contract does not claim transparent recovery from arbitrary native
faults. The development server may supervise and restart the host, and later
versions may add optional checkpoints, but this is distinct from compile-time
last-known-good behavior.

## Current vertical slice

The repository currently contains:

- a runtime registry for stable function and storage IDs;
- exact ABI/layout compatibility gates;
- immutable generation snapshots and atomic publication;
- a versioned binary patch-envelope parser;
- concurrency, transaction, protocol, and state-preservation tests;
- an opt-in `cvite-manifest` semantic indexer backed by libclang;
- an LLVM 18 lowering pass that emits machine-facing ABI fingerprints and protects refresh boundaries from inlining;
- an ORC/JITLink candidate loader with isolated generation namespaces, explicit host-symbol reflection, resource tracking, and a live object-to-runtime integration test.

The next execution milestone is extending the LLVM pass to emit baseline stable
entries and versioned implementations, then connecting those descriptors to the
existing loader and runtime automatically.


## Dispatch snapshot reclamation

Every compatible refresh publishes a new immutable runtime dispatch snapshot.
Readers acquire a lightweight snapshot scope before dereferencing the active
snapshot; explicit `cvite_dispatch_view` users hold that scope until
`cvite_runtime_release_view`.

Retired snapshots are reclaimed through a non-blocking writer gate. Collection
raises the gate, verifies that no snapshot reader is active, detaches the full
retired list while holding the runtime writer lock, and frees it. A reader that
races with the gate either completes before collection or backs out and retries
against the active snapshot. If a long-lived view is still active, collection
returns successfully with zero reclaimed snapshots and the watcher retries on a
later idle poll.

This lifecycle is independent from ORC machine-code reclamation. ORC generation
ownership protects executable targets; the runtime reader gate protects the
immutable table that stores those targets. Both must be safe before their
respective memory can be released.
