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
line = "include(cmake/CViteManagedRefresh.cmake)"
if line not in cmake:
    cmake_path.write_text(
        cmake.rstrip()
        + "\n\n# Semantic layout plans applied to compiler-managed storage.\n"
        + line
        + "\n",
        encoding="utf-8",
    )

readme_path = root / "README.md"
readme = readme_path.read_text(encoding="utf-8")
section = """

## Managed semantic refresh coordinator

CVite can now connect an active and candidate Clang record-layout index to the
transactional managed-memory domain. A strict append-only layout is converted
into a zero-and-field-copy plan, staged across every matching object, and
published only when every migration succeeds. Escaped pointers and incompatible
layouts still request restart. See
[`docs/managed-refresh-coordinator.md`](docs/managed-refresh-coordinator.md).
"""
if "## Managed semantic refresh coordinator" not in readme:
    readme_path.write_text(readme.rstrip() + section + "\n", encoding="utf-8")
