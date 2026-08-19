# Architecture

## Decision: CVite owns a version-matched Clang/LLVM toolchain boundary

CVite will use Clang's frontend libraries and LLVM's transformation/JIT layers as
one tested compiler service. It will not implement a C parser, and it will not
make its primary distribution depend on loading an ABI-fragile plugin into an
arbitrary system Clang.

A standalone `cvite` process reads the project's compilation database, launches
isolated compiler workers, and produces a semantic manifest plus relocatable code
for each candidate generation. The application host receives only validated,
versioned patch packages.

Dynamic Clang and LLVM pass plugins remain useful during development and for
integration experiments, but the product boundary is a pinned compiler service.

## Execution model

The long-term host executes application code through LLVM ORC/JITLink. User
`main` is a JIT symbol invoked by a small native host. External libraries and
process symbols are exposed to the JIT through an explicit symbol policy.

Each refreshable function has two identities:

- a stable callable entry whose address never changes;
- a versioned implementation that may be added for each successful build.

The LLVM transform rewrites function definitions into versioned implementation
symbols. Calls and function-address expressions resolve to stable entries. A
runtime-owned immutable dispatch table maps stable function slots to the active
implementations.

A patch is published by creating a complete new dispatch table and replacing one
atomic root pointer. Readers therefore observe either the old table or the new
table, never a partially updated set. Old code and old dispatch snapshots remain
alive until a later epoch/quiescence mechanism proves they can be reclaimed.

The M0 runtime in this repository implements that transaction model without the
ORC object layer. It is the host-side contract the compiler pipeline will target.

## Semantic manifests

Clang owns all source-language interpretation. The semantic manifest contains
stable 128-bit identities and fingerprints for:

- functions and their target ABI;
- globals, internal-linkage globals, and static locals;
- record, union, and enum layouts;
- fields and layout-relevant attributes;
- translation-unit and header dependencies;
- implementation symbols and required external symbols.

Stable IDs must derive from canonical declaration identity, project identity,
target triple, and CVite schema version. Source line numbers alone are not stable
enough. The exact hash algorithm and canonical serialization are protocol
choices, not C parsing.

## Persistent storage

The LLVM transform externalizes refresh-persistent globals and statics. The host
allocates their storage once and defines stable symbols at those addresses. A
compatible refresh reuses storage and does not rerun initializers.

M0 accepts only exact layout fingerprints. Future append-only or explicit
migrations must pass a compatibility proof before publication. Heap objects are
naturally preserved while their layouts stay unchanged. CVite rejects uncertain
layout-changing refreshes rather than attempting universal pointer relocation.

## Compiler failures

Compilation occurs out of process. A compiler diagnostic or worker crash cannot
invalidate the running generation. Only a complete patch that passes protocol,
symbol-resolution, and compatibility checks becomes publishable.

## Native runtime failures

React can recover many render errors because it controls component execution.
Unrestricted C may corrupt process memory or receive a fatal signal. CVite's
first releases do not claim transparent recovery from arbitrary native faults.
The development server can supervise and restart the host, and later versions
may add optional checkpoints, but compile-time last-known-good behavior is the
initial guarantee.
