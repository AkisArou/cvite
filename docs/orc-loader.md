# ORC/JITLink candidate loader

`cvite_orc_loader` is the first in-process code-loading component. It exposes a
small C ABI to the host runtime while using LLVM ORC and `ObjectLinkingLayer`
(JITLink) internally.

The loader is not a public application framework. `cvite run` will own it.

## Generation lifecycle

A candidate object follows this lifecycle:

```text
relocatable object
       │
       ▼
isolated JITDylib + ResourceTracker
       │
       ├─ define explicit host symbols
       ├─ add object
       ├─ materialize requested functions
       └─ resolve every required relocation
       │
       ▼
staged generation
       │
       ├─ compatibility rejected ─► discard ResourceTracker
       │
       └─ compatibility accepted ─► publish one runtime dispatch snapshot
```

Loading code and publishing code are separate operations. A generation can link
successfully and still be rejected by the semantic/ABI gate. The ORC loader
never changes the runtime's active function table by itself.

## Namespace isolation

Each staged object receives its own `JITDylib`. This permits multiple candidate
generations to define the same logical source symbols without colliding. The
compiler transform will eventually version implementation symbols as well, but
isolated namespaces remain valuable for transactional staging and diagnostics.

Each generation also owns an ORC `ResourceTracker`. Rejected, unmaterialized, or
otherwise unpublished code can therefore be removed as one unit. Published code
must remain alive until the runtime proves no thread can execute it; the current
runtime conservatively retains active code until loader shutdown.

## Host symbol policy

The loader supports explicit callable host symbols. CVite can expose stable
entries, runtime helpers, and selected library functions to candidate code even
when they are not exported by the native executable's dynamic symbol table.

The bootstrap API requires host symbols to be declared before staging the first
generation. Later protocol work will transmit an allow-listed symbol manifest
rather than exposing the complete process by default.

Data-symbol support is intentionally deferred until persistent-storage
registration is integrated with the LLVM transform.

## Bootstrap integration test

The `orc-loader` integration test compiles two ordinary C object files:

- a valid candidate that calls an explicitly registered host helper;
- a broken candidate with an unresolved external symbol.

It verifies that the broken generation fails materialization and leaves runtime
generation zero active. It then loads the valid object, resolves its update
function, atomically publishes it through `cvite_runtime_apply_patch`, and proves
that the original C state object survives while behavior changes.

This is not yet the final invisible source-edit loop. It proves the complete
host-side transaction from relocatable candidate code to a live function target.
