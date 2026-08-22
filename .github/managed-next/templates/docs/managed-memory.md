# Managed native memory experiment

CVite's long-term state-refresh goal requires a distinction between memory it can prove safe to migrate and arbitrary native memory it cannot control.

`cvite_managed_domain` is an internal development-runtime experiment for the first category. Ordinary application source does not call this API. A future LLVM instrumentation pass can use it underneath normal `malloc`, pointer stores, and compiler-generated metadata when the allocation pattern is provably typed and controlled.

## Transaction model

A type migration proceeds as one transaction:

1. reject the operation if any matching object escaped into untracked state;
2. validate every tracked interior-pointer offset against the proposed size;
3. allocate every replacement object;
4. run every migration callback into staged storage;
5. if any allocation or callback fails, free all staged storage and publish nothing;
6. switch all object records to their replacement addresses;
7. rewrite all registered direct and interior pointer slots;
8. free the old allocations.

The caller must hold CVite's process-wide quiescence gate. The domain mutex protects registry integrity, but it cannot make concurrent uninstrumented dereferences safe by itself.

## Byte-plan bridge

`cvite_managed_byte_plan` is the C runtime representation of the conservative plan emitted by the Clang semantic layout planner:

```text
zero complete replacement object
copy field A old+offset -> new+offset
copy field B old+offset -> new+offset
```

Copying fields individually avoids propagating uninitialized padding. New fields remain zero-initialized.

## Hard rejection boundary

Migration is rejected when an object has escaped through any path CVite cannot track. Examples include:

- a pointer stored by a foreign library;
- a pointer converted to an integer;
- inline assembly;
- an unmanaged interior pointer;
- memory shared through a protocol, mapping, device, or kernel interface;
- a stack object;
- a custom allocator that has not supplied provenance metadata.

The existence of this managed runtime does not change the safety contract for arbitrary C. Unproven state still triggers a controlled restart.

## Next compiler step

The next integration layer is an LLVM development-build pass that recognizes conservative typed allocations such as `malloc(sizeof(T))`, registers them invisibly, tracks selected pointer stores, and marks calls into unknown code as escapes unless a trusted summary proves otherwise.
