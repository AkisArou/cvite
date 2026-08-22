# Managed semantic refresh coordinator

The managed refresh coordinator is the bridge between Clang's record-layout analysis and the transactional managed-memory runtime.

Given:

- the active semantic index;
- a candidate semantic index;
- a stable managed type identity;
- a CVite-managed allocation domain;

it performs the following operation:

```text
old semantic record
        │
        ├─ exact field/type/offset comparison
        ├─ conservative append-only plan
        └─ restart-required rejection otherwise
                     │
                     ▼
managed byte plan
        │
        ├─ allocate every replacement object
        ├─ migrate every object into staged memory
        ├─ abort all staged work if any object fails
        ├─ publish all replacement addresses
        ├─ rewrite every proven pointer slot
        └─ free old storage
```

The caller must hold CVite's process-wide writer/quiescence gate. The managed domain mutex protects registry integrity; it does not stop arbitrary application threads from dereferencing old addresses.

## Safe case

A strict append-only record edit such as:

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

can be applied when every live `Player` allocation and every surviving pointer slot is managed. Existing fields are copied individually, the new object is zero-filled first, and `health` starts at zero.

## Restart cases

The coordinator requests restart when:

- the semantic record is missing from either generation;
- a field moved, changed type, or was removed;
- alignment changed;
- the edit uses a union, bit-field, flexible array, or old-padding insertion;
- any matching object escaped into untracked native state;
- a replacement allocation or object migrator fails;
- a tracked interior-pointer offset cannot exist in the new object.

It does not attempt to recover an arbitrary unmanaged C pointer graph. This bridge is useful precisely because it limits migration to the subset that can be proven transactional.
