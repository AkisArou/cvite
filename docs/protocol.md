# Patch protocol

CVite patch packages use an explicitly encoded little-endian wire format. C
structure layout is never used as an IPC format.

## Header v1

The fixed 64-byte header contains:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `CVITEPKG` magic |
| 8 | 2 | major version |
| 10 | 2 | minor version |
| 12 | 2 | header size |
| 14 | 2 | flags |
| 16 | 8 | expected/base generation |
| 24 | 8 | candidate generation |
| 32 | 4 | record count |
| 36 | 4 | reserved |
| 40 | 8 | record-region size |
| 48 | 8 | payload-region size |
| 56 | 8 | checksum |

The bootstrap checksum is FNV-1a-64 over all bytes after the header. It detects
accidental corruption; it is not an authenticity mechanism. A local transport
must still enforce peer identity and filesystem/socket permissions.

## Records

The record region is a sequence of aligned TLVs. Each record starts with:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | record type |
| 2 | 2 | flags |
| 4 | 4 | total encoded record size |

Unknown non-critical records can be skipped using their encoded size. Unknown
critical records reject the package. Concrete manifest and object records will
be added only with tests and versioning rules.
