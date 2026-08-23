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

check_script = root / "tests/check_managed_allocation_pass.py"
if check_script.exists():
    check_script.chmod(0o755)

# Add address-to-object resolution to the hidden managed runtime.
header_path = root / "include/cvite/managed_memory.h"
header = header_path.read_text(encoding="utf-8")
resolve_decl = """cvite_managed_status cvite_managed_resolve_pointer(
    cvite_managed_domain *domain,
    const void *address,
    cvite_managed_object *object,
    size_t *offset,
    cvite_managed_error *error);

"""
track_marker = "cvite_managed_status cvite_managed_track_pointer(\n"
if "cvite_managed_resolve_pointer(" not in header:
    if track_marker not in header:
        raise SystemExit("managed pointer tracking declaration marker missing")
    header = header.replace(track_marker, resolve_decl + track_marker, 1)
header_path.write_text(header, encoding="utf-8")

source_path = root / "src/runtime/managed_memory.c"
source = source_path.read_text(encoding="utf-8")
resolve_definition = r'''cvite_managed_status cvite_managed_resolve_pointer(
    cvite_managed_domain *domain,
    const void *address,
    cvite_managed_object *object,
    size_t *offset,
    cvite_managed_error *error)
{
    if (domain == NULL || address == NULL || object == NULL || offset == NULL) {
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            CVITE_MANAGED_OBJECT_INVALID,
            "invalid managed pointer resolution request");
    }
    *object = CVITE_MANAGED_OBJECT_INVALID;
    *offset = 0U;
    const uintptr_t requested = (uintptr_t)address;
    (void)pthread_mutex_lock(&domain->mutex);
    size_t index = 0U;
    for (index = 0U; index < domain->object_count; ++index) {
        const cvite_managed_record *record = &domain->objects[index];
        const uintptr_t base = (uintptr_t)record->address;
        if (requested < base) {
            continue;
        }
        const uintptr_t difference = requested - base;
        if (difference >= (uintptr_t)record->size) {
            continue;
        }
        *object = record->id;
        *offset = (size_t)difference;
        (void)pthread_mutex_unlock(&domain->mutex);
        cvite_managed_error_clear(error);
        return CVITE_MANAGED_OK;
    }
    (void)pthread_mutex_unlock(&domain->mutex);
    return cvite_managed_fail(
        error,
        CVITE_MANAGED_NOT_FOUND,
        CVITE_MANAGED_OBJECT_INVALID,
        "pointer does not refer to a managed object");
}

'''
source_marker = "cvite_managed_status cvite_managed_track_pointer(\n"
if "cvite_managed_resolve_pointer(" not in source:
    if source_marker not in source:
        raise SystemExit("managed pointer tracking definition marker missing")
    source = source.replace(source_marker, resolve_definition + source_marker, 1)
source_path.write_text(source, encoding="utf-8")

# Register the new pass in the existing LLVM pass plugin.
lowering_path = root / "src/transform/lowering_pass.cpp"
lowering = lowering_path.read_text(encoding="utf-8")
include_marker = '#include "candidate_pass.h"\n'
managed_include = '#include "managed_allocation_pass.h"\n'
if managed_include not in lowering:
    if include_marker not in lowering:
        raise SystemExit("lowering include marker missing")
    lowering = lowering.replace(
        include_marker, include_marker + managed_include, 1)
register_marker = "    cviteRegisterCandidatePass(builder);\n"
register_call = "    cviteRegisterManagedAllocationPass(builder);\n"
if register_call not in lowering:
    if register_marker not in lowering:
        raise SystemExit("lowering registration marker missing")
    lowering = lowering.replace(
        register_marker, register_marker + register_call, 1)
lowering_path.write_text(lowering, encoding="utf-8")

# Correct StringRef comparison regardless of operand order.
annotator_path = root / "src/compiler/allocation_annotator.cpp"
annotator = annotator_path.read_text(encoding="utf-8")
old_comparison = """            cvite::semantic::allocationKindName(site.kind) !=
                callee.getName()) {
"""
new_comparison = """            callee.getName() !=
                cvite::semantic::allocationKindName(site.kind)) {
"""
if old_comparison in annotator:
    annotator = annotator.replace(old_comparison, new_comparison, 1)
elif new_comparison not in annotator:
    raise SystemExit("allocation annotator comparison marker missing")
annotator_path.write_text(annotator, encoding="utf-8")

cmake_path = root / "CMakeLists.txt"
cmake = cmake_path.read_text(encoding="utf-8")
line = "include(cmake/CViteManagedInstrumentation.cmake)"
if line not in cmake:
    cmake_path.write_text(
        cmake.rstrip()
        + "\n\n# Invisible typed-allocation and pointer-provenance instrumentation.\n"
        + line
        + "\n",
        encoding="utf-8",
    )

readme_path = root / "README.md"
readme = readme_path.read_text(encoding="utf-8")
section = """

## Invisible managed-allocation instrumentation

A development-only LLVM pass can now consume Clang-derived allocation metadata,
replace high-confidence ordinary `malloc`/`calloc` calls with hidden managed
runtime calls, track persistent global pointer slots, and conservatively mark
unknown foreign-call, return, pointer-to-integer, and unmanaged-store escapes.

The pass is intentionally conservative: an escaped object is no longer eligible
for live movement and takes the restart path. Application source remains normal
C. See [`docs/allocation-tracking.md`](docs/allocation-tracking.md) and
[`docs/managed-memory.md`](docs/managed-memory.md).
"""
if "## Invisible managed-allocation instrumentation" not in readme:
    readme_path.write_text(readme.rstrip() + section + "\n", encoding="utf-8")
