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

# std::uint64_t is part of the public C++ helper API.
diff_header = root / "include/cvite/managed_type_diff.hpp"
text = diff_header.read_text(encoding="utf-8")
if "#include <cstdint>" not in text:
    text = text.replace(
        '#include "cvite/managed_type.h"\n',
        '#include "cvite/managed_type.h"\n\n#include <cstdint>\n',
        1,
    )
diff_header.write_text(text, encoding="utf-8")

# Extend the IR annotator with a semantic-index input and emit the target-side
# type manifest before writing the module.
annotator_path = root / "src/compiler/allocation_annotator.cpp"
annotator = annotator_path.read_text(encoding="utf-8")
if '#include "managed_type_emitter.hpp"' not in annotator:
    annotator = annotator.replace(
        '#include "cvite/allocation_index.hpp"\n',
        '#include "cvite/allocation_index.hpp"\n'
        '#include "managed_type_emitter.hpp"\n',
        1,
    )
old_usage = (
    '              << " --input module.ll --index sites.cvaidx --output annotated.ll\\n";'
)
new_usage = (
    '              << " --input module.ll --index sites.cvaidx --semantic records.cvsidx "\n'
    '                 "--output annotated.ll\\n";'
)
if old_usage in annotator:
    annotator = annotator.replace(old_usage, new_usage, 1)

old_options = """    std::string input;
    std::string index_path;
    std::string output;
    if (!optionValue(argc, argv, "--input", input) ||
        !optionValue(argc, argv, "--index", index_path) ||
        !optionValue(argc, argv, "--output", output)) {
"""
new_options = """    std::string input;
    std::string index_path;
    std::string semantic_path;
    std::string output;
    if (!optionValue(argc, argv, "--input", input) ||
        !optionValue(argc, argv, "--index", index_path) ||
        !optionValue(argc, argv, "--semantic", semantic_path) ||
        !optionValue(argc, argv, "--output", output)) {
"""
if old_options in annotator:
    annotator = annotator.replace(old_options, new_options, 1)
elif new_options not in annotator:
    raise SystemExit("annotator option marker missing")

allocation_read = """    if (!cvite::semantic::readAllocationIndex(
            index_path, allocation_index, error)) {
        std::cerr << error << '\\n';
        return 1;
    }

    llvm::LLVMContext context;
"""
semantic_read = """    if (!cvite::semantic::readAllocationIndex(
            index_path, allocation_index, error)) {
        std::cerr << error << '\\n';
        return 1;
    }
    cvite::semantic::SemanticIndex semantic_index;
    if (!cvite::semantic::readIndex(semantic_path, semantic_index, error)) {
        std::cerr << error << '\\n';
        return 1;
    }

    llvm::LLVMContext context;
"""
if "cvite::semantic::SemanticIndex semantic_index;" not in annotator:
    if allocation_read not in annotator:
        raise SystemExit("annotator allocation-index read marker missing")
    annotator = annotator.replace(allocation_read, semantic_read, 1)

write_marker = """    std::error_code output_error;
    llvm::raw_fd_ostream stream(output, output_error);
"""
emit_block = """    if (!cvite::compiler::emitManagedTypeManifest(
            *module, allocation_index, semantic_index, error)) {
        std::cerr << error << '\\n';
        return 1;
    }

    std::error_code output_error;
    llvm::raw_fd_ostream stream(output, output_error);
"""
if "emitManagedTypeManifest(" not in annotator:
    if write_marker not in annotator:
        raise SystemExit("annotator output marker missing")
    annotator = annotator.replace(write_marker, emit_block, 1)
annotator_path.write_text(annotator, encoding="utf-8")

# Generate a semantic sidecar in the transparent compiler proxy and pass it to
# the annotator together with the allocation-site index.
wrapper_path = root / "src/compiler/clang_wrapper.cpp"
wrapper = wrapper_path.read_text(encoding="utf-8")
if "CVITE_SEMANTIC_INDEX_PATH" not in wrapper:
    wrapper = wrapper.replace(
        '#ifndef CVITE_ALLOCATION_ANNOTATOR_PATH\n'
        '#error "CVITE_ALLOCATION_ANNOTATOR_PATH is required"\n'
        '#endif\n',
        '#ifndef CVITE_ALLOCATION_ANNOTATOR_PATH\n'
        '#error "CVITE_ALLOCATION_ANNOTATOR_PATH is required"\n'
        '#endif\n'
        '#ifndef CVITE_SEMANTIC_INDEX_PATH\n'
        '#error "CVITE_SEMANTIC_INDEX_PATH is required"\n'
        '#endif\n',
        1,
    )

old_paths = """    const std::string index_path =
        temporaryPath(invocation.output, ".alloc.cvaidx");
    const std::string annotated_path =
        temporaryPath(invocation.output, ".annotated.ll");
"""
new_paths = """    const std::string index_path =
        temporaryPath(invocation.output, ".alloc.cvaidx");
    const std::string semantic_path =
        temporaryPath(invocation.output, ".semantic.cvsidx");
    const std::string annotated_path =
        temporaryPath(invocation.output, ".annotated.ll");
"""
if old_paths in wrapper:
    wrapper = wrapper.replace(old_paths, new_paths, 1)
elif new_paths not in wrapper:
    raise SystemExit("compiler proxy temporary-path marker missing")

annotator_command = """    status = runProcess({
        CVITE_ALLOCATION_ANNOTATOR_PATH,
        "--input",
        invocation.output,
        "--index",
        index_path,
        "--output",
        annotated_path,
    });
"""
semantic_and_annotator = """    std::vector<std::string> semantic_command = {
        CVITE_SEMANTIC_INDEX_PATH,
        "index",
        "--source",
        invocation.source,
        "--output",
        semantic_path,
        "--",
    };
    semantic_command.insert(
        semantic_command.end(),
        invocation.semantic_arguments.begin(),
        invocation.semantic_arguments.end());
    status = runProcess(semantic_command);
    if (status != 0) {
        removeQuietly(index_path);
        removeQuietly(semantic_path);
        removeQuietly(annotated_path);
        return status;
    }

    status = runProcess({
        CVITE_ALLOCATION_ANNOTATOR_PATH,
        "--input",
        invocation.output,
        "--index",
        index_path,
        "--semantic",
        semantic_path,
        "--output",
        annotated_path,
    });
"""
if '"--semantic",\n        semantic_path,' not in wrapper:
    if annotator_command not in wrapper:
        raise SystemExit("compiler proxy annotator command marker missing")
    wrapper = wrapper.replace(
        annotator_command, semantic_and_annotator, 1)

# Ensure every exit path removes the additional sidecar.
wrapper = wrapper.replace(
    "        removeQuietly(index_path);\n        removeQuietly(annotated_path);",
    "        removeQuietly(index_path);\n"
    "        removeQuietly(semantic_path);\n"
    "        removeQuietly(annotated_path);",
)
cleanup_marker = """    removeQuietly(index_path);
    if (error) {
"""
cleanup_replacement = """    removeQuietly(index_path);
    removeQuietly(semantic_path);
    if (error) {
"""
if cleanup_marker in wrapper:
    wrapper = wrapper.replace(cleanup_marker, cleanup_replacement, 1)
wrapper_path.write_text(wrapper, encoding="utf-8")

# Add the semantic-index tool path to the proxy target and dependency graph.
proxy_module_path = root / "cmake/CViteCompilerProxy.cmake"
proxy_module = proxy_module_path.read_text(encoding="utf-8")
old_definitions = """    CVITE_ALLOCATION_INDEX_PATH=\"$<TARGET_FILE:cvite_allocation_index>\"
    CVITE_ALLOCATION_ANNOTATOR_PATH=\"$<TARGET_FILE:cvite_allocation_annotate>\"
)
"""
new_definitions = """    CVITE_ALLOCATION_INDEX_PATH=\"$<TARGET_FILE:cvite_allocation_index>\"
    CVITE_ALLOCATION_ANNOTATOR_PATH=\"$<TARGET_FILE:cvite_allocation_annotate>\"
    CVITE_SEMANTIC_INDEX_PATH=\"$<TARGET_FILE:cvite_semantic_index>\"
)
"""
if old_definitions in proxy_module:
    proxy_module = proxy_module.replace(old_definitions, new_definitions, 1)
elif new_definitions not in proxy_module:
    raise SystemExit("compiler proxy definitions marker missing")
old_dependencies = """    cvite_allocation_index
    cvite_allocation_annotate
)
"""
new_dependencies = """    cvite_allocation_index
    cvite_allocation_annotate
    cvite_semantic_index
)
"""
if "    cvite_semantic_index\n)" not in proxy_module:
    if old_dependencies not in proxy_module:
        raise SystemExit("compiler proxy dependency marker missing")
    proxy_module = proxy_module.replace(
        old_dependencies, new_dependencies, 1)
proxy_module_path.write_text(proxy_module, encoding="utf-8")

# Extend the focused annotation test with a real semantic sidecar.
check_path = root / "tests/check_managed_allocation_pass.py"
check = check_path.read_text(encoding="utf-8")
if 'parser.add_argument("--semantic-index"' not in check:
    check = check.replace(
        '    parser.add_argument("--allocation-index", required=True)\n',
        '    parser.add_argument("--allocation-index", required=True)\n'
        '    parser.add_argument("--semantic-index", required=True)\n',
        1,
    )
    check = check.replace(
        '    allocation_index = work / "managed.cvaidx"\n',
        '    allocation_index = work / "managed.cvaidx"\n'
        '    semantic_index = work / "managed.cvsidx"\n',
        1,
    )
    insertion = """    run(
        [
            args.semantic_index,
            "index",
            "--source",
            args.source,
            "--output",
            str(semantic_index),
            "--",
            "-std=c11",
        ]
    )
"""
    marker = """    annotate_output = run(
"""
    check = check.replace(marker, insertion + marker, 1)
    check = check.replace(
        '            "--output",\n            str(annotated_ir),\n',
        '            "--semantic",\n'
        '            str(semantic_index),\n'
        '            "--output",\n'
        '            str(annotated_ir),\n',
        1,
    )
    check = check.replace(
        '    require(transformed, "@__cvite_host_managed_allocate")\n',
        '    require(annotated_ir.read_text(encoding="utf-8"), '
        '"@__cvite_managed_type_manifest")\n'
        '    require(transformed, "@__cvite_host_managed_allocate")\n',
        1,
    )
check_path.write_text(check, encoding="utf-8")

instrumentation_module_path = root / "cmake/CViteManagedInstrumentation.cmake"
instrumentation_module = instrumentation_module_path.read_text(encoding="utf-8")
if "--semantic-index $<TARGET_FILE:cvite_semantic_index>" not in instrumentation_module:
    instrumentation_module = instrumentation_module.replace(
        "            --allocation-index $<TARGET_FILE:cvite_allocation_index>\n",
        "            --allocation-index $<TARGET_FILE:cvite_allocation_index>\n"
        "            --semantic-index $<TARGET_FILE:cvite_semantic_index>\n",
        1,
    )
instrumentation_module_path.write_text(instrumentation_module, encoding="utf-8")

# The wrapper test should prove that the manifest is emitted automatically.
wrapper_check_path = root / "tests/check_clang_wrapper.py"
wrapper_check = wrapper_check_path.read_text(encoding="utf-8")
if "__cvite_managed_type_manifest" not in wrapper_check:
    wrapper_check = wrapper_check.replace(
        '    if "!cvite.managed.alloc" not in annotated_text:\n'
        '        raise RuntimeError("cvite-clang did not annotate typed allocation calls")\n',
        '    if "!cvite.managed.alloc" not in annotated_text:\n'
        '        raise RuntimeError("cvite-clang did not annotate typed allocation calls")\n'
        '    if "@__cvite_managed_type_manifest" not in annotated_text:\n'
        '        raise RuntimeError("cvite-clang did not emit managed type metadata")\n',
        1,
    )
wrapper_check_path.write_text(wrapper_check, encoding="utf-8")

cmake_path = root / "CMakeLists.txt"
cmake = cmake_path.read_text(encoding="utf-8")
line = "include(cmake/CViteManagedType.cmake)"
if line not in cmake:
    cmake_path.write_text(
        cmake.rstrip()
        + "\n\n# Target-side managed type manifests and compatibility planning.\n"
        + line
        + "\n",
        encoding="utf-8",
    )

readme_path = root / "README.md"
readme = readme_path.read_text(encoding="utf-8")
section = """

## Self-describing managed type manifests

The transparent compiler proxy now embeds a compact native manifest for every
record type used by a high-confidence managed allocation site. The manifest
contains stable type/field identities, layout fingerprints, offsets, sizes, and
unsupported-shape flags. This lets the running ORC host classify a candidate as
identical, append-only migratable, or restart-required without reparsing C in
the application process.
"""
if "## Self-describing managed type manifests" not in readme:
    readme_path.write_text(readme.rstrip() + section + "\n", encoding="utf-8")
