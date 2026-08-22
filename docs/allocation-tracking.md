# Typed allocation analysis

CVite analyzes allocation sites through Clang's semantic AST. It does not scan source text or implement a second C parser.

The initial index recognizes ordinary calls to:

```c
malloc(...)
calloc(...)
realloc(...)
aligned_alloc(...)
```

and records the source location, allocation kind, inferred record identity, and confidence.

## Conservative inference

Two inference paths are supported:

1. **Typed result** — the allocator call is immediately wrapped by a typed expression such as a cast to `Player *`.
2. **`sizeof` type** — the call contains exactly one record type reference, as in `malloc(sizeof(Player))` or `calloc(n, sizeof(Player))`.

Examples:

```c
Player *a = malloc(sizeof(Player));       /* typed: sizeof-type */
Player *b = (Player *)malloc(bytes);       /* typed: typed-result */
void *c = malloc(bytes);                   /* unknown */
```

An unknown site stays unknown. CVite does not guess a type from nearby names or coincidental byte sizes.

## CLI

```console
cvite-allocation-index index \
  --source src/world.c \
  --output world.cvaidx \
  -- -std=c11 -Iinclude

cvite-allocation-index show --index world.cvaidx
```

Example output:

```text
src/world.c:42:17 malloc type=Player confidence=sizeof-type
src/world.c:57:12 malloc type=? confidence=unknown
```

## Relationship to managed migration

The allocation index is semantic evidence, not proof that an object can move. A future LLVM development-build pass can use high-confidence sites to insert hidden managed-allocation registration. It must still track pointer stores and mark the object escaped whenever a pointer enters code or storage whose behavior cannot be summarized safely.

The following remain restart-only unless additional proof exists:

- custom allocators;
- pointers returned through unknown wrappers;
- pointer-to-integer conversions;
- inline assembly;
- foreign-library callbacks and retained pointers;
- untracked interior pointers;
- stack, mapped, shared-memory, device, or protocol-visible objects.

This deliberately preserves the rule that native state is migrated only when both **type compatibility** and **pointer provenance** are established.
