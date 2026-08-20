# Candidate objects

The `cvite-candidate` LLVM pass turns an edited ordinary-C translation unit
into a self-describing candidate object. The source does not use CVite names,
attributes, macros, allocators, or lifecycle functions.

Given an edited definition:

```c
int update(State *state, int delta)
{
    state->value += delta * 10;
    return state->value;
}
```

the candidate transform emits three internal pieces:

```text
__cvite_patch.<stable-id>      new implementation body
update                         candidate-local stable-reference entry
__cvite_candidate_manifest    IDs, lowered ABI fingerprints, targets
```

The implementation pointer is recorded directly in the manifest, so the host
does not discover patches by guessing linker symbol names. The ORC loader
materializes the object, validates the manifest schema and records, and builds a
generation-owned `cvite_patch` view. The existing transactional runtime remains
the final compatibility gate and publishes all candidate functions together.

## Calls made by candidate code

The transform redirects recursive calls, calls between functions in the edited
translation unit, and function-address expressions through generated entries.
Those entries resolve the target by stable function ID and lowered ABI
fingerprint from the baseline host's active immutable dispatch snapshot.

This avoids baking another candidate implementation address into generated code
and preserves atomic multi-function publication semantics. The bootstrap path
performs a host lookup on those cross-function calls; later work can cache stable
slots or emit faster machine stubs without changing the source contract.

## Loader contract

`cvite_orc_loader_prepare_patch` looks up
`__cvite_candidate_manifest`, rejects malformed or unsupported manifests, copies
function updates into storage owned by the staged generation, and returns a
`cvite_patch` whose function array remains valid until that generation is
prepared again, discarded, or the loader is destroyed.

The loader automatically exposes the hidden host resolver required by
compiler-generated candidate entries. Application/library symbols referenced by
edited code still need to be resolved from the baseline process or an explicit
symbol provider; project orchestration will automate that layer.

## Safety

A candidate object remains isolated until all of the following succeed:

1. object linking and relocation;
2. manifest lookup and schema validation;
3. duplicate/invalid record checks;
4. stable-ID and ABI checks in the runtime;
5. construction of a complete immutable dispatch snapshot.

Failure before publication leaves the last known-good generation active.
Published JIT generations are retained for now because quiescence/epoch-based
code reclamation has not yet been implemented.
