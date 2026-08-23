from __future__ import annotations

import re
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

check = root / "tests/check_clang_wrapper.py"
if check.exists():
    check.chmod(0o755)

# Remove a helper that is deliberately unnecessary after the argument parser
# was simplified; warnings are errors in the verifier.
wrapper_path = root / "src/compiler/clang_wrapper.cpp"
wrapper = wrapper_path.read_text(encoding="utf-8")
wrapper = re.sub(
    r"\nbool consumesNext\(const std::string &argument\)\n\{.*?\n\}\n",
    "\n",
    wrapper,
    count=1,
    flags=re.S,
)
wrapper_path.write_text(wrapper, encoding="utf-8")

# Make the managed allocation pass part of the normal cvite-lowering pipeline,
# while retaining its explicit pass name for focused tests.
header_path = root / "src/transform/managed_allocation_pass.h"
header = header_path.read_text(encoding="utf-8")
if '#include "llvm/IR/PassManager.h"' not in header:
    header = header.replace(
        '#define CVITE_MANAGED_ALLOCATION_PASS_H\n',
        '#define CVITE_MANAGED_ALLOCATION_PASS_H\n\n'
        '#include "llvm/IR/PassManager.h"\n',
        1,
    )
append_decl = (
    "void cviteAppendManagedAllocationPass("
    "llvm::ModulePassManager &manager);\n"
)
if append_decl not in header:
    header = header.replace(
        "void cviteRegisterManagedAllocationPass(llvm::PassBuilder &builder);\n",
        "void cviteRegisterManagedAllocationPass(llvm::PassBuilder &builder);\n"
        + append_decl,
        1,
    )
header_path.write_text(header, encoding="utf-8")

pass_path = root / "src/transform/managed_allocation_pass.cpp"
pass_source = pass_path.read_text(encoding="utf-8")
append_definition = """
void cviteAppendManagedAllocationPass(llvm::ModulePassManager &manager)
{
    manager.addPass(CViteManagedAllocationPass());
}

"""
if "void cviteAppendManagedAllocationPass(" not in pass_source:
    marker = "void cviteRegisterManagedAllocationPass(llvm::PassBuilder &builder)\n"
    if marker not in pass_source:
        raise SystemExit("managed pass registration marker missing")
    pass_source = pass_source.replace(marker, append_definition + marker, 1)
pass_path.write_text(pass_source, encoding="utf-8")

lowering_path = root / "src/transform/lowering_pass.cpp"
lowering = lowering_path.read_text(encoding="utf-8")
old_add = "            manager.addPass(CViteLoweringPass());\n"
new_add = (
    "            manager.addPass(CViteLoweringPass());\n"
    "            cviteAppendManagedAllocationPass(manager);\n"
)
if new_add not in lowering:
    if old_add not in lowering:
        raise SystemExit("explicit lowering pipeline marker missing")
    lowering = lowering.replace(old_add, new_add, 1)
old_start = "            manager.addPass(CViteLoweringPass());\n"
new_start = (
    "            manager.addPass(CViteLoweringPass());\n"
    "            cviteAppendManagedAllocationPass(manager);\n"
)
# A second occurrence belongs to the pipeline-start extension callback.
if lowering.count("cviteAppendManagedAllocationPass(manager);") < 2:
    position = lowering.find(old_start, lowering.find(new_add) + len(new_add))
    if position < 0:
        raise SystemExit("pipeline-start lowering marker missing")
    lowering = (
        lowering[:position]
        + new_start
        + lowering[position + len(old_start):]
    )
lowering_path.write_text(lowering, encoding="utf-8")

cmake_path = root / "CMakeLists.txt"
cmake = cmake_path.read_text(encoding="utf-8")
lines = cmake.splitlines()
replaced = False
for index, line in enumerate(lines):
    if "CVITE_CLANG_PATH=" not in line:
        continue
    indentation = line[: len(line) - len(line.lstrip())]
    lines[index] = (
        indentation
        + 'CVITE_CLANG_PATH="$<TARGET_FILE:cvite_clang_wrapper>"'
    )
    replaced = True
    break
if not replaced:
    raise SystemExit("CVITE_CLANG_PATH compile definition was not found")
cmake = "\n".join(lines) + "\n"
include_line = "include(cmake/CViteCompilerProxy.cmake)"
if include_line not in cmake:
    cmake = (
        cmake.rstrip()
        + "\n\n# Compiler proxy that invisibly emits typed allocation metadata.\n"
        + include_line
        + "\n"
    )
cmake_path.write_text(cmake, encoding="utf-8")

readme_path = root / "README.md"
readme = readme_path.read_text(encoding="utf-8")
section = """

## Transparent compiler proxy

`cvite run` uses a generated `cvite-clang` proxy in development builds. The
proxy forwards the exact project Clang invocation, then transparently builds a
Clang allocation index and annotates the emitted LLVM IR before the normal
CVite lowering pipeline. Developers still invoke only:

```console
cvite run .
```

Set `CVITE_DISABLE_MANAGED_ALLOCATIONS=1` to disable allocation annotation while
debugging the compiler pipeline. Production builds do not use the proxy.
"""
if "## Transparent compiler proxy" not in readme:
    readme_path.write_text(readme.rstrip() + section + "\n", encoding="utf-8")
