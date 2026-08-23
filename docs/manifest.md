# Semantic manifest

`cvite-manifest` is the first compiler-service executable. It asks libclang to
parse one translation unit with the project's actual compiler arguments and
writes a deterministic JSON manifest.

```console
cvite-manifest \
  --source src/game.c \
  --project-root . \
  --project-id 1f3f... \
  --output game.cvmanifest.json \
  -- -std=c11 -Iinclude -DMY_FEATURE=1
```

Normal users will not call this command. `cvite run` will read
`compile_commands.json`, invoke compiler workers, and consume their manifests.
The standalone executable keeps the compiler boundary testable and isolates
candidate parse failures from the running application.

## Contents

The bootstrap manifest records:

- compiler version, target triple, and pointer width;
- project-local translation-unit dependencies reported by Clang;
- function identities, canonical types, calling conventions, and semantic
  fingerprints;
- record/union identities, aliases, target size/alignment, exact field offsets,
  bit-field information, and layout fingerprints;
- global, internal, and static-local storage identities and layout fingerprints;
- source locations and complete identity seeds for collision detection.

Clang performs preprocessing, macro expansion, semantic analysis, target layout,
and diagnostics. CVite does not parse C source text.

## Identities and fingerprints

Every declaration carries a stable 128-bit ID derived from a versioned canonical
identity seed. External declarations use Clang's USR where possible. Internal
linkage also incorporates the project-relative owning file. Static locals include
their semantic parent.

The manifest retains the complete identity seed. Consumers must reject an ID
collision if the same ID is ever paired with a different seed.

The bootstrap hash is a deterministic non-cryptographic 128-bit construction. It
is a replaceable schema detail, not a security primitive.

A function's `semantic_fingerprint` describes source-level compatibility. It is
**not** the final ABI fingerprint. The LLVM transformation stage will add a
lowered ABI fingerprint containing the generated LLVM function type, calling
convention, parameter/return attributes, data layout, target triple, and ABI
schema version.

## Candidate errors

If Clang reports an error or fatal diagnostic, `cvite-manifest` exits nonzero and
does not replace the requested output. The development server therefore keeps
the last known-good generation alive.
