# Managed memory migration core

CVite's normal source model remains ordinary C. This internal runtime exists for
compiler-instrumented allocations whose provenance stays under CVite's control.
It is not a public application framework or replacement allocator API.

The managed domain assigns stable object identities, records typed allocation
metadata, and tracks pointer slots. A pointer slot inside another managed object
is stored as an owner object plus byte offset rather than as an absolute address.
This lets a migration update cycles and interior pointers when both the pointer
owner and target move in the same transaction.

A type migration has two phases:

1. allocate and populate every replacement object;
2. resolve every future pointer-slot address and target address;
3. publish object addresses and pointer writes with no remaining fallible work;
4. free superseded storage.

If allocation, validation, or a migrator fails, live addresses and pointer slots
are unchanged. Objects marked as escaped into untracked native state reject
migration and require CVite's controlled restart path.

This core does not claim that arbitrary C memory is movable. Stack objects,
foreign-library pointers, uninstrumented pointer copies, custom allocators, and
untracked interior pointers remain outside the safe migration boundary.
