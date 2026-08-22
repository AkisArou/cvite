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

module = root / "cmake/CViteSemantic.cmake"
text = module.read_text(encoding="utf-8")
source_marker = """add_library(cvite_semantic STATIC
    src/compiler/semantic_index.cpp
    src/compiler/layout_plan.cpp
)
"""
source_replacement = """add_library(cvite_semantic STATIC
    src/compiler/semantic_index.cpp
    src/compiler/layout_plan.cpp
    src/compiler/allocation_index.cpp
)
"""
if source_replacement not in text:
    if source_marker not in text:
        raise SystemExit("cvite_semantic source marker was not found")
    text = text.replace(source_marker, source_replacement, 1)

cli_marker = """if(BUILD_TESTING)
    add_executable(cvite_test_layout_plan
"""
cli_block = """add_executable(cvite_allocation_index
    src/compiler/allocation_index_main.cpp
)
set_target_properties(cvite_allocation_index PROPERTIES
    OUTPUT_NAME cvite-allocation-index
)
target_compile_features(cvite_allocation_index PRIVATE cxx_std_17)
target_link_libraries(cvite_allocation_index PRIVATE cvite_semantic)
target_compile_options(cvite_allocation_index PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
)
if(CVITE_WARNINGS_AS_ERRORS)
    target_compile_options(cvite_allocation_index PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
    )
endif()

if(BUILD_TESTING)
    add_executable(cvite_test_layout_plan
"""
if "add_executable(cvite_allocation_index" not in text:
    if cli_marker not in text:
        raise SystemExit("semantic test marker was not found")
    text = text.replace(cli_marker, cli_block, 1)

end_marker = """    add_test(NAME semantic-layout-plan COMMAND cvite_test_layout_plan)
endif()
"""
end_replacement = """    add_test(NAME semantic-layout-plan COMMAND cvite_test_layout_plan)

    add_executable(cvite_test_allocation_index
        tests/test_allocation_index.cpp
    )
    target_compile_features(cvite_test_allocation_index PRIVATE cxx_std_17)
    target_link_libraries(cvite_test_allocation_index PRIVATE cvite_semantic)
    target_compile_definitions(cvite_test_allocation_index PRIVATE
        CVITE_ALLOCATION_SOURCE=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/allocation_sites.c\"
    )
    target_compile_options(cvite_test_allocation_index PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
    )
    if(CVITE_WARNINGS_AS_ERRORS)
        target_compile_options(cvite_test_allocation_index PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
        )
    endif()
    add_test(NAME typed-allocation-index COMMAND cvite_test_allocation_index)
endif()
"""
if "add_executable(cvite_test_allocation_index" not in text:
    if end_marker not in text:
        raise SystemExit("semantic module closing marker was not found")
    text = text.replace(end_marker, end_replacement, 1)
module.write_text(text, encoding="utf-8")

readme_path = root / "README.md"
readme = readme_path.read_text(encoding="utf-8")
section = """

## Typed allocation analysis

`cvite-allocation-index` uses Clang's AST to identify conservative ordinary-C
allocation patterns such as `malloc(sizeof(Player))` and
`calloc(n, sizeof(Player))`. Unknown sites remain unknown; no source-text
heuristics or type guesses are used.

This index is the semantic input for future invisible LLVM allocation and
pointer-provenance instrumentation. It does not by itself make arbitrary heap
objects movable. See [`docs/allocation-tracking.md`](docs/allocation-tracking.md).
"""
if "## Typed allocation analysis" not in readme:
    readme_path.write_text(readme.rstrip() + section + "\n", encoding="utf-8")
