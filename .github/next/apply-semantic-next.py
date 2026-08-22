from __future__ import annotations

import shutil
from pathlib import Path

root = Path(__file__).resolve().parents[2]
templates = Path(__file__).resolve().parent / "templates"

for source in sorted(templates.rglob("*")):
    if source.is_dir():
        continue
    relative = source.relative_to(templates)
    destination = root / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)

bootstrap = root / "scripts/bootstrap-ubuntu-24.04.sh"
if bootstrap.exists():
    bootstrap.chmod(0o755)

cmake_path = root / "CMakeLists.txt"
cmake = cmake_path.read_text(encoding="utf-8")
block = """

# Semantic layout analysis and experimental stress targets are kept in
# separate modules so the production runtime remains independent of libclang.
include(cmake/CViteSemantic.cmake)
include(cmake/CViteExperimental.cmake)
"""
if "include(cmake/CViteSemantic.cmake)" not in cmake:
    cmake_path.write_text(cmake.rstrip() + block + "\n", encoding="utf-8")

readme_path = root / "README.md"
readme = readme_path.read_text(encoding="utf-8")
section = """

## Semantic layout diagnostics

Development builds can create and compare Clang-derived record-layout indexes:

```console
cvite-semantic-index index --source old.c --output old.cvsidx -- -std=c11
cvite-semantic-index index --source new.c --output new.cvsidx -- -std=c11
cvite-semantic-index diff --old old.cvsidx --new new.cvsidx
```

The accompanying migration planner accepts only strict append-only layouts and
is executable only for memory owned by CVite with controlled pointer provenance.
Arbitrary native pointers and unmanaged allocations still take the safe restart
path. See [`docs/semantic-refresh.md`](docs/semantic-refresh.md).

For a pinned Ubuntu 24.04 development setup:

```console
./scripts/bootstrap-ubuntu-24.04.sh
```
"""
if "## Semantic layout diagnostics" not in readme:
    readme_path.write_text(readme.rstrip() + section + "\n", encoding="utf-8")
