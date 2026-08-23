# Lowered ABI pass

`CViteLoweringPass` is the first version-matched LLVM extension in the compiler
pipeline. It runs after Clang has lowered C declarations to LLVM IR and records a
machine-facing compatibility fingerprint for each defined function.

The pass is deliberately separate from `cvite-manifest`:

- the Clang semantic manifest knows source identities, fields, declarations, and
  project dependencies;
- the LLVM pass knows the target triple, data layout, lowered function type,
  calling convention, and return/parameter attributes.

A refresh is considered ABI-compatible only when both layers agree.

## Current transformation

The bootstrap pass:

1. computes a deterministic MD5-encoded fingerprint from a versioned lowered-ABI
   seed;
2. attaches `!cvite.abi` metadata to each defined non-intrinsic function;
3. emits a deterministic `!cvite.functions` module index;
4. adds the `cvite.lowered-abi.schema` module flag;
5. removes `alwaysinline` and applies `noinline` to keep refresh boundaries from
   disappearing before the stable-entry transform exists.

MD5 is used only as a compact deterministic identifier. It is not an
integrity/security mechanism, and the complete compatibility inputs remain
versioned by schema.

## Test contract

The LLVM 18 CI job compiles ordinary C to LLVM IR, runs the pass twice to check
idempotence, and verifies that:

- body-only edits preserve lowered ABI fingerprints;
- signature edits change the affected fingerprint;
- unrelated function fingerprints remain stable;
- per-function metadata, the module index, and no-inline boundaries are present.

## Next transform

The next pass stage will split each refreshable definition into a stable callable
entry and a versioned implementation, then emit baseline symbol/slot descriptors
for the runtime and ORC/JITLink patch loader.
