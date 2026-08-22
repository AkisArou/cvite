# Semantic refresh and managed migration

CVite uses Clang's semantic model for record-layout analysis. It does not parse C declarations independently.

## Field-level diagnostics

`cvite-semantic-index` stores a compact versioned index containing record identities, target information, size, alignment, fields, canonical field types, and exact bit offsets.

```console
cvite-semantic-index index \
  --source old/player.c \
  --output old-player.cvsidx \
  -- -std=c11 -Iinclude

cvite-semantic-index index \
  --source new/player.c \
  --output new-player.cvsidx \
  -- -std=c11 -Iinclude

cvite-semantic-index diff \
  --old old-player.cvsidx \
  --new new-player.cvsidx
```

An edit such as:

```c
typedef struct Player {
    int score;
    float speed;
} Player;
```

becoming:

```c
typedef struct Player {
    int score;
    float speed;
    int health;
} Player;
```

is reported with the actual field:

```text
struct Player: append-only managed candidate
  size: 8 -> 12 bytes
  + field health: int @ bit 64
```

The phrase **managed candidate** is intentional. Semantic compatibility alone does not prove that live native pointers can be redirected safely.

## Conservative migration planner

The planner accepts only strict append-only changes where:

- the record identity is unchanged;
- the type is a struct, not a union;
- alignment is unchanged;
- every existing field keeps its order, canonical type, size, alignment, and offset;
- added fields begin after the complete old object extent;
- no field is a bit-field or flexible array member;
- every copied field is byte-addressable.

The generated operation is equivalent to:

```text
zero the complete new object
copy every compatible old field to its unchanged offset
```

This avoids copying uninitialized padding and gives new fields deterministic zero initialization.

## Critical safety boundary

A migration plan may execute only for memory that CVite owns and whose pointer provenance remains controlled. It does **not** make these cases movable:

- stack objects;
- arbitrary `malloc` allocations not registered with CVite;
- interior pointers;
- addresses stored by foreign libraries;
- pointers converted to integers;
- memory-mapped or protocol-visible objects;
- unions, bit-fields, or flexible array members;
- objects reachable through an untracked raw-pointer graph.

Those cases continue to use the controlled process-restart fallback. A rejected migration is a successful safety decision.

## Integration direction

The semantic index and migration planner are intentionally separate from storage publication. The next storage milestone is a managed-storage backend that can reserve a stable handle/address boundary, stage a replacement allocation, execute a plan, rewrite only proven references, and publish the new storage atomically with the code transaction.
