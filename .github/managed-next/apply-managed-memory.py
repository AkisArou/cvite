from __future__ import annotations

import shutil
from pathlib import Path

root = Path(__file__).resolve().parents[2]
templates = Path(__file__).resolve().parent / "templates"

for source in sorted(templates.rglob("*")):
    if source.is_dir():
        continue
    destination = root / source.relative_to(templates)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)

cmake_path = root / "CMakeLists.txt"
cmake = cmake_path.read_text(encoding="utf-8")
line = "include(cmake/CViteManagedMemory.cmake)"
if line not in cmake:
    cmake_path.write_text(
        cmake.rstrip()
        + "\n\n# Hidden compiler-managed allocation and migration experiment.\n"
        + line
        + "\n",
        encoding="utf-8",
    )

readme_path = root / "README.md"
readme = readme_path.read_text(encoding="utf-8")
section = """

## Managed native-memory experiment

CVite now contains an internal transactional registry for compiler-managed
allocations and tracked direct/interior pointer slots. It can stage replacement
objects, execute a conservative semantic byte plan, atomically rewrite proven
pointer slots, and roll back completely on allocation or migration failure.

This is compiler/runtime infrastructure, not a source API. Objects that escape
into untracked native or foreign-library state remain restart-only. See
[`docs/managed-memory.md`](docs/managed-memory.md).
"""
if "## Managed native-memory experiment" not in readme:
    readme_path.write_text(readme.rstrip() + section + "\n", encoding="utf-8")
