# Stable function entries

The `cvite-baseline` LLVM pass turns supported ordinary C function definitions
into stable callable entries backed by versioned implementations.

Given:

```c
int update(State *state, int delta)
{
    state->value += delta;
    return state->value;
}
```

the development build behaves conceptually like:

```text
update                         stable address kept for the process lifetime
  └─ load CVite slot
     └─ load target from active immutable dispatch snapshot
        └─ call __cvite_impl.<stable-id>
```

Application source, direct calls, and function-address expressions continue to
refer to `update`. The original body is moved into an internal implementation.
Existing uses of the original LLVM function value are redirected to the stable
entry, so calls between transformed functions cross the same refresh boundary.

## Registration

Each transformed translation unit receives a low-priority-number (early)
constructor. Before `main`, it registers every stable function ID, lowered ABI
fingerprint, initial implementation, and debug name with the hidden CVite host.
The host assigns immutable dispatch slots and stores them in compiler-generated
private globals.

The runtime is sealed lazily at the first stable call or patch request. This lets
all baseline module constructors register before normal application execution.
A module that attempts late registration after sealing fails loudly rather than
silently producing a partial dispatch table.

## ABI preservation

The wrapper has exactly the lowered LLVM function type and calling convention of
the original definition. Return and parameter attributes are preserved at the
stable entry and indirect call site. Optimization-only function attributes are
not copied to the wrapper because runtime target resolution reads CVite state and
must not be misclassified as `readnone`, `nosync`, or a similar property.

The first pass supports strong external, internal, and private non-variadic
functions in address space zero. It deliberately skips:

- `main`;
- variadic functions, whose unknown argument tail cannot be forwarded by a
  generic LLVM wrapper;
- weak/link-once/available-externally definitions;
- functions used by `blockaddress`/computed-goto constructs;
- non-default program address spaces.

Skipped functions remain ordinary native functions and will become explicit
refresh-boundary diagnostics in the project orchestration layer.

## Atomic publication

Stable entries do not own mutable per-function target pointers. They ask the
host runtime for a target by immutable slot. The runtime resolves that slot from
one immutable dispatch snapshot. A multi-function patch constructs a complete
new snapshot and publishes one atomic root pointer, so readers cannot observe a
half-installed generation.

The bootstrap wrapper performs one host call per stable function invocation.
Later optimization can inline the read-only fast path or pin one snapshot per
outermost refresh call chain without changing the source contract.

## End-to-end test

The compiler-pipeline CI test now:

1. compiles an ordinary C baseline to LLVM IR;
2. applies `cvite-lowering` and `cvite-baseline`;
3. emits and links the transformed native object with the hidden host;
4. calls the original stable function address;
5. JIT-links a new Clang-produced C implementation through ORC/JITLink;
6. publishes it through the transactional runtime;
7. calls the same stable address again and verifies new behavior with the same
   live C state object.

This is the first end-to-end proof of the intended invisible source model. File
watching, project build orchestration, candidate transforms, and automatic
manifest comparison still sit above this mechanism.
