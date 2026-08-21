from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] if '.cvite' in Path(__file__).parts else Path.cwd()


def replace_once(path: str, old: str, new: str) -> None:
    file = ROOT / path
    text = file.read_text(encoding='utf-8')
    if new in text:
        return
    if old not in text:
        raise SystemExit(f'marker missing in {path}: {old[:120]!r}')
    file.write_text(text.replace(old, new, 1), encoding='utf-8')


replace_once(
    'CMakeLists.txt',
    '''if(CVITE_BUILD_EXAMPLES)\n    add_executable(cvite_runtime_transaction examples/runtime_transaction.c)\n''',
    '''# Clang-backed semantic analysis, conservative managed-layout planning,\n# and opt-in development stress tests are declared after the core/compiler\n# targets they extend.\ninclude(cmake/CViteSemantic.cmake)\ninclude(cmake/CViteExperimental.cmake)\n\nif(CVITE_BUILD_EXAMPLES)\n    add_executable(cvite_runtime_transaction examples/runtime_transaction.c)\n''',
)

replace_once(
    'cmake/CViteExperimental.cmake',
    '    target_link_libraries(cvite PRIVATE cvite_semantic_index)\n',
    '''    target_link_libraries(cvite PRIVATE cvite_semantic_index)\n    target_compile_definitions(cvite PRIVATE CVITE_SEMANTIC_CACHE_ENABLED=1)\n''',
)
replace_once(
    'cmake/CViteExperimental.cmake',
    '''        target_link_libraries(\n            cvite_test_runtime_stress\n            PRIVATE cvite::runtime Threads::Threads\n        )\n        cvite_set_warnings(cvite_test_runtime_stress)\n''',
    '''        target_link_libraries(\n            cvite_test_runtime_stress\n            PRIVATE cvite::runtime Threads::Threads\n        )\n        target_compile_definitions(\n            cvite_test_runtime_stress\n            PRIVATE _POSIX_C_SOURCE=200809L\n        )\n        cvite_set_warnings(cvite_test_runtime_stress)\n''',
)

replace_once(
    'tests/test_runtime_stress.c',
    '''static const cvite_id function_ids[STRESS_SLOT_COUNT] = {\n    CVITE_ID(UINT64_C(0xA100), UINT64_C(1)),\n    CVITE_ID(UINT64_C(0xA100), UINT64_C(2)),\n    CVITE_ID(UINT64_C(0xA100), UINT64_C(3)),\n    CVITE_ID(UINT64_C(0xA100), UINT64_C(4)),\n};\n\nstatic const cvite_id function_abi =\n    CVITE_ID(UINT64_C(0xB200), UINT64_C(1));\n''',
    '''static const cvite_id function_ids[STRESS_SLOT_COUNT] = {\n    CVITE_ID_INITIALIZER(UINT64_C(0xA100), UINT64_C(1)),\n    CVITE_ID_INITIALIZER(UINT64_C(0xA100), UINT64_C(2)),\n    CVITE_ID_INITIALIZER(UINT64_C(0xA100), UINT64_C(3)),\n    CVITE_ID_INITIALIZER(UINT64_C(0xA100), UINT64_C(4)),\n};\n\nstatic const cvite_id function_abi =\n    CVITE_ID_INITIALIZER(UINT64_C(0xB200), UINT64_C(1));\n''',
)

semantic_header = '''#ifndef CVITE_CLI_SEMANTIC_CACHE_H
#define CVITE_CLI_SEMANTIC_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Internal development-server API. Ordinary C programs never call it. */
#ifdef CVITE_SEMANTIC_CACHE_ENABLED
int cvite_semantic_cache_activate(
    const char *source_path,
    const char *active_ir_path);

int cvite_semantic_cache_stage(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path);

void cvite_semantic_cache_promote(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path);

void cvite_semantic_cache_discard(
    const char *source_path,
    const char *staged_ir_path);

unsigned cvite_semantic_cache_report_pending(void);
void cvite_semantic_cache_clear(void);
#else
static inline int cvite_semantic_cache_activate(
    const char *source_path,
    const char *active_ir_path)
{
    (void)source_path;
    (void)active_ir_path;
    return 0;
}

static inline int cvite_semantic_cache_stage(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path)
{
    (void)source_path;
    (void)active_ir_path;
    (void)staged_ir_path;
    return 0;
}

static inline void cvite_semantic_cache_promote(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path)
{
    (void)source_path;
    (void)active_ir_path;
    (void)staged_ir_path;
}

static inline void cvite_semantic_cache_discard(
    const char *source_path,
    const char *staged_ir_path)
{
    (void)source_path;
    (void)staged_ir_path;
}

static inline unsigned cvite_semantic_cache_report_pending(void)
{
    return 0U;
}

static inline void cvite_semantic_cache_clear(void)
{
}
#endif

#ifdef __cplusplus
}
#endif

#endif
'''
(ROOT / 'src/cli/semantic_cache.h').write_text(semantic_header, encoding='utf-8')

replace_once(
    'src/cli/semantic_cache.cpp',
    '''} // namespace\n\nextern "C" int cvite_semantic_cache_stage(''',
    '''} // namespace\n\nextern "C" int cvite_semantic_cache_activate(\n    const char *source_path,\n    const char *active_ir_path)\n{\n    if (source_path == nullptr || source_path[0] == '\\0') {\n        return -1;\n    }\n    const std::string active_index = indexPath(active_ir_path);\n    if (active_index.empty()) {\n        return -1;\n    }\n\n    cvite::semantic::Index index;\n    std::string error;\n    if (!cvite::semantic::buildIndex(\n            source_path,\n            compileArguments(source_path),\n            index,\n            error) ||\n        !cvite::semantic::writeIndex(index, active_index, error)) {\n        trace("could not activate '" + std::string(source_path) + "': " + error);\n        std::error_code remove_error;\n        fs::remove(active_index, remove_error);\n        return -1;\n    }\n\n    const std::string source_key =\n        fs::absolute(source_path).lexically_normal().string();\n    std::lock_guard<std::mutex> lock(cache_mutex);\n    cache_entries[source_key] = {active_index, std::string()};\n    return 0;\n}\n\nextern "C" int cvite_semantic_cache_stage(''',
)
replace_once(
    'src/cli/semantic_cache.cpp',
    '''extern "C" unsigned cvite_semantic_cache_report_pending(void)\n''',
    '''extern "C" void cvite_semantic_cache_discard(\n    const char *source_path,\n    const char *staged_ir_path)\n{\n    const std::string staged_index = indexPath(staged_ir_path);\n    if (!staged_index.empty()) {\n        std::error_code remove_error;\n        fs::remove(staged_index, remove_error);\n    }\n    if (source_path == nullptr || source_path[0] == '\\0') {\n        return;\n    }\n\n    const std::string source_key =\n        fs::absolute(source_path).lexically_normal().string();\n    std::lock_guard<std::mutex> lock(cache_mutex);\n    const auto found = cache_entries.find(source_key);\n    if (found == cache_entries.end()) {\n        return;\n    }\n    if (found->second.staged_index == staged_index) {\n        found->second.staged_index.clear();\n    }\n    if (found->second.active_index.empty() &&\n        found->second.staged_index.empty()) {\n        cache_entries.erase(found);\n    }\n}\n\nextern "C" unsigned cvite_semantic_cache_report_pending(void)\n''',
)

replace_once(
    'src/cli/toolchain.c',
    '#include "run_internal.h"\n',
    '#include "run_internal.h"\n#include "semantic_cache.h"\n',
)
replace_once(
    'src/cli/toolchain.c',
    '''        if ((kind == CVITE_BUILD_BASELINE\n                 ? cvite_active_ir_path(state, index, ir_path)\n                 : staged_ir_path(state, index, ir_path)) != 0 ||\n            (kind == CVITE_BUILD_BASELINE\n                 ? cvite_active_dependency_path(state, index, dependency_path)\n                 : staged_dependency_path(state, index, dependency_path)) != 0 ||\n            compile_translation_unit(\n                state,\n                index,\n                kind,\n                ir_path,\n                dependency_path) != 0) {\n            return -1;\n        }\n        ++compiled_count;\n''',
    '''        if ((kind == CVITE_BUILD_BASELINE\n                 ? cvite_active_ir_path(state, index, ir_path)\n                 : staged_ir_path(state, index, ir_path)) != 0 ||\n            (kind == CVITE_BUILD_BASELINE\n                 ? cvite_active_dependency_path(state, index, dependency_path)\n                 : staged_dependency_path(state, index, dependency_path)) != 0 ||\n            compile_translation_unit(\n                state,\n                index,\n                kind,\n                ir_path,\n                dependency_path) != 0) {\n            return -1;\n        }\n\n        if (kind == CVITE_BUILD_BASELINE) {\n            if (cvite_semantic_cache_activate(\n                    state->translation_units[index].source_path,\n                    ir_path) != 0) {\n                (void)fprintf(\n                    stderr,\n                    "[cvite] warning: semantic index unavailable for %s\\n",\n                    state->translation_units[index].source_path);\n            }\n        } else {\n            char active_ir[PATH_MAX];\n\n            if (cvite_active_ir_path(state, index, active_ir) != 0 ||\n                cvite_semantic_cache_stage(\n                    state->translation_units[index].source_path,\n                    active_ir,\n                    ir_path) != 0) {\n                (void)fprintf(\n                    stderr,\n                    "[cvite] warning: staged semantic index unavailable for %s\\n",\n                    state->translation_units[index].source_path);\n            }\n        }\n        ++compiled_count;\n''',
)
replace_once(
    'src/cli/toolchain.c',
    '''        if (staged_ir_path(state, index, staged_ir) != 0 ||\n            staged_dependency_path(state, index, staged_dependency) != 0 ||\n            cvite_active_ir_path(state, index, active_ir) != 0 ||\n            cvite_active_dependency_path(state, index, active_dependency) != 0 ||\n            rename(staged_dependency, active_dependency) != 0 ||\n            rename(staged_ir, active_ir) != 0) {\n            (void)fprintf(\n                stderr,\n                "[cvite] warning: could not commit TU cache %zu: %s\\n",\n                index,\n                strerror(errno));\n        }\n''',
    '''        if (staged_ir_path(state, index, staged_ir) != 0 ||\n            staged_dependency_path(state, index, staged_dependency) != 0 ||\n            cvite_active_ir_path(state, index, active_ir) != 0 ||\n            cvite_active_dependency_path(state, index, active_dependency) != 0 ||\n            rename(staged_dependency, active_dependency) != 0 ||\n            rename(staged_ir, active_ir) != 0) {\n            (void)fprintf(\n                stderr,\n                "[cvite] warning: could not commit TU cache %zu: %s\\n",\n                index,\n                strerror(errno));\n            continue;\n        }\n        cvite_semantic_cache_promote(\n            state->translation_units[index].source_path,\n            active_ir,\n            staged_ir);\n''',
)
replace_once(
    'src/cli/toolchain.c',
    '''        if (staged_ir_path(state, index, path) == 0) {\n            cvite_remove_if_present(path);\n        }\n''',
    '''        if (staged_ir_path(state, index, path) == 0) {\n            cvite_semantic_cache_discard(\n                state->translation_units[index].source_path,\n                path);\n            cvite_remove_if_present(path);\n        }\n''',
)

replace_once(
    'src/cli/watcher.c',
    '#include "run_internal.h"\n',
    '#include "run_internal.h"\n#include "semantic_cache.h"\n',
)
replace_once(
    'src/cli/watcher.c',
    '''    if (status != CVITE_STATUS_OK) {\n        cvite_print_runtime_error("candidate validation", &error);\n        (void)fprintf(stderr, "[cvite] previous code remains active\\n");\n''',
    '''    if (status != CVITE_STATUS_OK) {\n        cvite_print_runtime_error("candidate validation", &error);\n        if (error.status == CVITE_STATUS_LAYOUT_MISMATCH) {\n            (void)cvite_semantic_cache_report_pending();\n        }\n        (void)fprintf(stderr, "[cvite] previous code remains active\\n");\n''',
)

replace_once(
    'src/cli/run.c',
    '#include "run_internal.h"\n',
    '#include "run_internal.h"\n#include "semantic_cache.h"\n',
)
replace_once(
    'src/cli/run.c',
    '''    cvite_discard_candidate_build(&state);\n    cvite_free_project(&state);\n''',
    '''    cvite_discard_candidate_build(&state);\n    cvite_semantic_cache_clear();\n    cvite_free_project(&state);\n''',
)
