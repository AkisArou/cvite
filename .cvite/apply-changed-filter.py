from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]


def replace(path: str, old: str, new: str, count: int = 1) -> None:
    file = root / path
    text = file.read_text(encoding="utf-8")
    if new in text and old not in text:
        return
    if old not in text:
        raise SystemExit(f"marker missing in {path}: {old[:80]!r}")
    file.write_text(text.replace(old, new, count), encoding="utf-8")


# Candidate ABI v3 adds a deterministic implementation fingerprint.
replace(
    "include/cvite/candidate.h",
    "#define CVITE_CANDIDATE_MANIFEST_SCHEMA UINT64_C(2)",
    "#define CVITE_CANDIDATE_MANIFEST_SCHEMA UINT64_C(3)",
)
replace(
    "include/cvite/candidate.h",
    "    uint64_t abi_high;\n    uint64_t abi_low;\n    cvite_function_pointer target;",
    "    uint64_t abi_high;\n"
    "    uint64_t abi_low;\n"
    "    uint64_t implementation_high;\n"
    "    uint64_t implementation_low;\n"
    "    cvite_function_pointer target;",
)

replace(
    "include/cvite/orc_loader.h",
    "cvite_status cvite_orc_loader_discard_generation(\n",
    "cvite_status cvite_orc_loader_commit_patch(\n"
    "    cvite_orc_loader *loader,\n"
    "    cvite_orc_generation generation,\n"
    "    cvite_error *error);\n\n"
    "cvite_status cvite_orc_loader_discard_generation(\n",
)

replace(
    "src/transform/transform_support.h",
    'inline constexpr llvm::StringLiteral kFunctionIndex = "cvite.functions";\n',
    'inline constexpr llvm::StringLiteral kFunctionIndex = "cvite.functions";\n'
    'inline constexpr llvm::StringLiteral kImplementationSchema =\n'
    '    "cvite.implementation.v1";\n'
    'inline constexpr llvm::StringLiteral kImplementationMetadata =\n'
    '    "cvite.implementation";\n',
)
replace(
    "src/transform/transform_support.h",
    "inline std::string identitySeed(\n",
    "inline std::string implementationSeed(const llvm::Function &function)\n"
    "{\n"
    "    std::string seed;\n"
    "    llvm::raw_string_ostream output(seed);\n\n"
    "    output << kImplementationSchema << '\\n';\n"
    "    function.print(output, nullptr, false, false);\n"
    "    return output.str();\n"
    "}\n\n"
    "inline std::string identitySeed(\n",
)

replace(
    "src/transform/candidate_pass.cpp",
    "    Hash128 identity;\n    Hash128 abi;\n    std::string debug_name;",
    "    Hash128 identity;\n"
    "    Hash128 abi;\n"
    "    Hash128 implementation;\n"
    "    std::string debug_name;",
)
replace(
    "src/transform/candidate_pass.cpp",
    "void setStorageMetadata(\n",
    "void setImplementationMetadata(\n"
    "    llvm::Function &function,\n"
    "    const Hash128 &implementation)\n"
    "{\n"
    "    llvm::LLVMContext &context = function.getContext();\n"
    "    llvm::Type *i64 = llvm::Type::getInt64Ty(context);\n"
    "    llvm::Metadata *metadata[] = {\n"
    "        llvm::MDString::get(context, implementation.hex),\n"
    "        llvm::MDString::get(\n"
    "            context, cvite::transform::kImplementationSchema),\n"
    "        llvm::ConstantAsMetadata::get(\n"
    "            llvm::ConstantInt::get(i64, implementation.high)),\n"
    "        llvm::ConstantAsMetadata::get(\n"
    "            llvm::ConstantInt::get(i64, implementation.low)),\n"
    "    };\n"
    "    function.setMetadata(\n"
    "        cvite::transform::kImplementationMetadata,\n"
    "        llvm::MDNode::get(context, metadata));\n"
    "}\n\n"
    "void setStorageMetadata(\n",
)
replace(
    "src/transform/candidate_pass.cpp",
    "    const Hash128 abi = cvite::transform::hash128(\n"
    "        cvite::transform::abiSeed(module, function));\n",
    "    const Hash128 abi = cvite::transform::hash128(\n"
    "        cvite::transform::abiSeed(module, function));\n"
    "    const Hash128 implementation = cvite::transform::hash128(\n"
    "        cvite::transform::implementationSeed(function));\n",
)
replace(
    "src/transform/candidate_pass.cpp",
    "    setRefreshMetadata(function, identity, abi);\n"
    "    setRefreshMetadata(*entry, identity, abi);\n"
    "    return {&function, entry, identity, abi, original_name};",
    "    setRefreshMetadata(function, identity, abi);\n"
    "    setRefreshMetadata(*entry, identity, abi);\n"
    "    setImplementationMetadata(function, implementation);\n"
    "    return {\n"
    "        &function, entry, identity, abi, implementation, original_name};",
)
replace(
    "src/transform/candidate_pass.cpp",
    "        {i64, i64, i64, i64, pointer, pointer},\n",
    "        {i64, i64, i64, i64, i64, i64, pointer, pointer},\n",
)
replace(
    "src/transform/candidate_pass.cpp",
    "                llvm::ConstantInt::get(i64, function.abi.low),\n"
    "                function.implementation,",
    "                llvm::ConstantInt::get(i64, function.abi.low),\n"
    "                llvm::ConstantInt::get(\n"
    "                    i64, function.implementation.high),\n"
    "                llvm::ConstantInt::get(\n"
    "                    i64, function.implementation.low),\n"
    "                function.implementation,",
)
replace(
    "src/transform/candidate_pass.cpp",
    "constexpr unsigned kCandidateSchema = 2U;",
    "constexpr unsigned kCandidateSchema = 3U;",
)

loader = root / "src/host/orc_loader.cpp"
text = loader.read_text(encoding="utf-8")
if "struct PreparedFingerprint final" not in text:
    old = (
        "struct Generation final {\n"
        "    llvm::orc::JITDylib *dylib = nullptr;\n"
        "    llvm::orc::ResourceTrackerSP resources;\n"
        "    std::vector<cvite_function_update> updates;\n"
        "    bool baseline_prepared = false;\n"
        "};"
    )
    new = (
        "struct PreparedFingerprint final {\n"
        "    cvite_id id = CVITE_ID_ZERO;\n"
        "    cvite_id implementation = CVITE_ID_ZERO;\n"
        "};\n\n"
        "struct Generation final {\n"
        "    llvm::orc::JITDylib *dylib = nullptr;\n"
        "    llvm::orc::ResourceTrackerSP resources;\n"
        "    std::vector<cvite_function_update> updates;\n"
        "    std::vector<PreparedFingerprint> prepared_fingerprints;\n"
        "    bool baseline_prepared = false;\n"
        "    bool patch_prepared = false;\n"
        "    bool patch_committed = false;\n"
        "};"
    )
    if old not in text:
        raise SystemExit("ORC Generation marker missing")
    text = text.replace(old, new, 1)

if "std::vector<PreparedFingerprint> active_fingerprints;" not in text:
    old = (
        "    std::vector<HostFunction> host_functions;\n"
        "    std::unordered_map<cvite_orc_generation, Generation> generations;\n"
    )
    new = (
        "    std::vector<HostFunction> host_functions;\n"
        "    std::vector<PreparedFingerprint> active_fingerprints;\n"
        "    std::unordered_map<cvite_orc_generation, Generation> generations;\n"
    )
    if old not in text:
        raise SystemExit("ORC loader field marker missing")
    text = text.replace(old, new, 1)

if "const PreparedFingerprint *findFingerprint(" not in text:
    needle = """bool hasDuplicateId(const std::vector<cvite_id> &identities, cvite_id id)
{
    for (const cvite_id existing : identities) {
        if (cvite_id_equal(existing, id)) {
            return true;
        }
    }
    return false;
}
"""
    helper = needle + """
const PreparedFingerprint *findFingerprint(
    const std::vector<PreparedFingerprint> &fingerprints,
    cvite_id id)
{
    for (const PreparedFingerprint &fingerprint : fingerprints) {
        if (cvite_id_equal(fingerprint.id, id)) {
            return &fingerprint;
        }
    }
    return nullptr;
}

PreparedFingerprint *findFingerprint(
    std::vector<PreparedFingerprint> &fingerprints,
    cvite_id id)
{
    for (PreparedFingerprint &fingerprint : fingerprints) {
        if (cvite_id_equal(fingerprint.id, id)) {
            return &fingerprint;
        }
    }
    return nullptr;
}
"""
    if needle not in text:
        raise SystemExit("ORC duplicate-ID helper marker missing")
    text = text.replace(needle, helper, 1)

text = text.replace(
    "Generation{&candidate_dylib, std::move(resources), {}, false}",
    "Generation{\n"
    "            &candidate_dylib, std::move(resources), {}, {}, false, false, false}",
)

if "candidate.patch_prepared = true;" not in text:
    old = """    found->second.updates.clear();
    found->second.updates.reserve(
        static_cast<std::size_t>(manifest->function_count));
    for (std::uint64_t index = 0U;
         index < manifest->function_count;
         ++index) {
        const cvite_candidate_function &record = manifest->functions[index];
        const cvite_id id{record.id_high, record.id_low};
        const cvite_id abi{record.abi_high, record.abi_low};
        if (cvite_id_is_zero(id) || cvite_id_is_zero(abi) ||
            record.target == nullptr || record.debug_name == nullptr ||
            record.debug_name[0] == '\\0') {
            return fail(
                error,
                CVITE_STATUS_INVALID_PACKET,
                "candidate manifest contains an invalid function record");
        }
        if (hasDuplicateUpdate(found->second.updates, id)) {
            return fail(
                error,
                CVITE_STATUS_DUPLICATE_SYMBOL,
                "candidate manifest contains a duplicate function ID");
        }
        found->second.updates.push_back(cvite_function_update{
            id,
            abi,
            record.target,
        });
    }
"""
    new = """    Generation &candidate = found->second;
    if (candidate.patch_prepared) {
        return fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            "candidate generation was already prepared");
    }

    candidate.updates.clear();
    candidate.prepared_fingerprints.clear();
    candidate.updates.reserve(
        static_cast<std::size_t>(manifest->function_count));
    candidate.prepared_fingerprints.reserve(
        static_cast<std::size_t>(manifest->function_count));
    std::vector<cvite_id> candidate_identities;
    candidate_identities.reserve(
        static_cast<std::size_t>(manifest->function_count));
    for (std::uint64_t index = 0U;
         index < manifest->function_count;
         ++index) {
        const cvite_candidate_function &record = manifest->functions[index];
        const cvite_id id{record.id_high, record.id_low};
        const cvite_id abi{record.abi_high, record.abi_low};
        const cvite_id implementation{
            record.implementation_high,
            record.implementation_low,
        };
        if (cvite_id_is_zero(id) || cvite_id_is_zero(abi) ||
            cvite_id_is_zero(implementation) || record.target == nullptr ||
            record.debug_name == nullptr || record.debug_name[0] == '\\0') {
            return fail(
                error,
                CVITE_STATUS_INVALID_PACKET,
                "candidate manifest contains an invalid function record");
        }
        if (hasDuplicateId(candidate_identities, id)) {
            return fail(
                error,
                CVITE_STATUS_DUPLICATE_SYMBOL,
                "candidate manifest contains a duplicate function ID");
        }
        candidate_identities.push_back(id);

        const PreparedFingerprint *active =
            findFingerprint(loader->active_fingerprints, id);
        if (active != nullptr &&
            cvite_id_equal(active->implementation, implementation)) {
            continue;
        }

        candidate.updates.push_back(cvite_function_update{
            id,
            abi,
            record.target,
        });
        candidate.prepared_fingerprints.push_back(
            PreparedFingerprint{id, implementation});
    }
    candidate.patch_prepared = true;
"""
    if old not in text:
        raise SystemExit("ORC candidate loop marker missing")
    text = text.replace(old, new, 1)

text = text.replace(
    "    patch->functions = found->second.updates.data();\n"
    "    patch->function_count = found->second.updates.size();",
    "    patch->functions = candidate.updates.data();\n"
    "    patch->function_count = candidate.updates.size();",
)

if "cvite_orc_loader_commit_patch(" not in text:
    needle = 'extern "C" cvite_status cvite_orc_loader_discard_generation(\n'
    commit = """extern "C" cvite_status cvite_orc_loader_commit_patch(
    cvite_orc_loader *loader,
    cvite_orc_generation generation,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr || generation == CVITE_ORC_GENERATION_INVALID) {
        return fail(error, CVITE_STATUS_INVALID_ARGUMENT, "invalid patch commit");
    }

    std::lock_guard<std::mutex> guard(loader->mutex);
    const auto found = loader->generations.find(generation);
    if (found == loader->generations.end()) {
        return fail(error, CVITE_STATUS_INVALID_ARGUMENT, "unknown candidate generation");
    }
    Generation &candidate = found->second;
    if (!candidate.patch_prepared || candidate.patch_committed) {
        return fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            "candidate generation is not awaiting a publication commit");
    }

    loader->active_fingerprints.reserve(
        loader->active_fingerprints.size() +
        candidate.prepared_fingerprints.size());
    for (const PreparedFingerprint &prepared :
         candidate.prepared_fingerprints) {
        PreparedFingerprint *active =
            findFingerprint(loader->active_fingerprints, prepared.id);
        if (active == nullptr) {
            loader->active_fingerprints.push_back(prepared);
        } else {
            active->implementation = prepared.implementation;
        }
    }
    candidate.patch_committed = true;
    return CVITE_STATUS_OK;
}

"""
    if needle not in text:
        raise SystemExit("ORC discard marker missing")
    text = text.replace(needle, commit + needle, 1)
loader.write_text(text, encoding="utf-8")

watcher = root / "src/cli/watcher.c"
text = watcher.read_text(encoding="utf-8")
if 'cvite_orc_loader_commit_patch(' not in text:
    old = """    status = cvite_host_apply_patch(&patch, &error);
    if (status != CVITE_STATUS_OK) {
        cvite_print_runtime_error("patch publication", &error);
        (void)fprintf(stderr, "[cvite] previous code remains active\\n");
        (void)cvite_orc_loader_discard_generation(
            state->loader,
            generation,
            &error);
        return -1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
"""
    new = """    status = cvite_host_apply_patch(&patch, &error);
    if (status != CVITE_STATUS_OK) {
        cvite_print_runtime_error("patch publication", &error);
        (void)fprintf(stderr, "[cvite] previous code remains active\\n");
        (void)cvite_orc_loader_discard_generation(
            state->loader,
            generation,
            &error);
        return -1;
    }

    status = cvite_orc_loader_commit_patch(
        state->loader,
        generation,
        &error);
    if (status != CVITE_STATUS_OK) {
        cvite_print_runtime_error("patch acknowledgement", &error);
        abort();
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
"""
    if old not in text:
        raise SystemExit("watcher publication marker missing")
    text = text.replace(old, new, 1)
watcher.write_text(text, encoding="utf-8")

fixtures = {
    "tests/fixtures/filter_baseline.c": """int cvite_filter_unchanged(int value)
{
    return value + 1;
}

int cvite_filter_changing(int value)
{
    return cvite_filter_unchanged(value);
}

int main(int argc, char **argv)
{
    (void)argv;
    return cvite_filter_changing(argc);
}
""",
    "tests/fixtures/filter_candidate_one.c": """int cvite_filter_unchanged(int value)
{
    return value + 1;
}

int cvite_filter_changing(int value)
{
    return cvite_filter_unchanged(value) * 10;
}
""",
    "tests/fixtures/filter_candidate_two.c": """int cvite_filter_unchanged(int value)
{
    return value + 1;
}

int cvite_filter_changing(int value)
{
    return cvite_filter_unchanged(value) * 20;
}
""",
}
for name, content in fixtures.items():
    (root / name).write_text(content, encoding="utf-8")

(root / "tests/test_candidate_filter.c").write_text(
    r'''#include "cvite/host.h"
#include "cvite/orc_loader.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(CONDITION)                                                        \
    do {                                                                        \
        if (!(CONDITION)) {                                                      \
            (void)fprintf(                                                       \
                stderr,                                                          \
                "CHECK failed at %s:%d: %s\n",                                  \
                __FILE__,                                                       \
                __LINE__,                                                       \
                #CONDITION);                                                     \
            return EXIT_FAILURE;                                                 \
        }                                                                       \
    } while (0)

static int publish_candidate(
    cvite_orc_loader *loader,
    const char *path,
    uint64_t expected_generation,
    size_t expected_function_count,
    cvite_error *error)
{
    cvite_orc_generation generation = CVITE_ORC_GENERATION_INVALID;
    cvite_patch patch = {0};

    CHECK(cvite_orc_loader_stage_object(
              loader, path, &generation, error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_prepare_patch(
              loader,
              generation,
              expected_generation,
              expected_generation + 1U,
              &patch,
              error) == CVITE_STATUS_OK);
    CHECK(patch.function_count == expected_function_count);
    CHECK(cvite_host_apply_patch(&patch, error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_commit_patch(
              loader, generation, error) == CVITE_STATUS_OK);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    cvite_orc_loader *loader = NULL;
    cvite_orc_generation baseline = CVITE_ORC_GENERATION_INVALID;
    cvite_program_main program_main = NULL;
    cvite_error error = {0};
    char *program_argv[] = {(char *)"candidate-filter", (char *)"x", NULL};

    CHECK(argc == 4);
    CHECK(cvite_orc_loader_create(&loader, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_stage_object(
              loader, argv[1], &baseline, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_prepare_baseline(
              loader, baseline, &program_main, &error) == CVITE_STATUS_OK);
    CHECK(program_main != NULL);
    CHECK(program_main(2, program_argv) == 3);

    CHECK(publish_candidate(loader, argv[2], 0U, 2U, &error) == EXIT_SUCCESS);
    CHECK(cvite_host_generation() == 1U);
    CHECK(program_main(2, program_argv) == 30);

    CHECK(publish_candidate(loader, argv[3], 1U, 1U, &error) == EXIT_SUCCESS);
    CHECK(cvite_host_generation() == 2U);
    CHECK(program_main(2, program_argv) == 60);

    cvite_orc_loader_destroy(loader);
    return EXIT_SUCCESS;
}
''',
    encoding="utf-8",
)

cmake = root / "CMakeLists.txt"
text = cmake.read_text(encoding="utf-8")
if "NAME candidate-filter" not in text:
    marker = "\nif(CVITE_BUILD_EXAMPLES)\n"
    if marker not in text:
        raise SystemExit("CMake examples marker missing")
    block = r'''
if(CVITE_BUILD_LLVM_PASS AND CVITE_BUILD_ORC_LOADER AND BUILD_TESTING)
    set(CVITE_FILTER_TEST_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/candidate-filter-test")
    set(CVITE_FILTER_BASELINE_RAW_IR
        "${CVITE_FILTER_TEST_DIRECTORY}/baseline.raw.ll")
    set(CVITE_FILTER_BASELINE_IR
        "${CVITE_FILTER_TEST_DIRECTORY}/baseline.ll")
    set(CVITE_FILTER_BASELINE_OBJECT
        "${CVITE_FILTER_TEST_DIRECTORY}/baseline.o")
    set(CVITE_FILTER_ONE_RAW_IR
        "${CVITE_FILTER_TEST_DIRECTORY}/candidate-one.raw.ll")
    set(CVITE_FILTER_ONE_IR
        "${CVITE_FILTER_TEST_DIRECTORY}/candidate-one.ll")
    set(CVITE_FILTER_ONE_OBJECT
        "${CVITE_FILTER_TEST_DIRECTORY}/candidate-one.o")
    set(CVITE_FILTER_TWO_RAW_IR
        "${CVITE_FILTER_TEST_DIRECTORY}/candidate-two.raw.ll")
    set(CVITE_FILTER_TWO_IR
        "${CVITE_FILTER_TEST_DIRECTORY}/candidate-two.ll")
    set(CVITE_FILTER_TWO_OBJECT
        "${CVITE_FILTER_TEST_DIRECTORY}/candidate-two.o")

    add_custom_command(
        OUTPUT
            ${CVITE_FILTER_BASELINE_OBJECT}
            ${CVITE_FILTER_ONE_OBJECT}
            ${CVITE_FILTER_TWO_OBJECT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CVITE_FILTER_TEST_DIRECTORY}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -std=c11 -O0 -S -emit-llvm
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/filter_baseline.c
            -o ${CVITE_FILTER_BASELINE_RAW_IR}
        COMMAND
            ${CVITE_OPT_EXECUTABLE}
            -load-pass-plugin=$<TARGET_FILE:CViteLoweringPass>
            -passes=cvite-lowering,cvite-baseline,cvite-baseline-manifest,verify
            -S ${CVITE_FILTER_BASELINE_RAW_IR}
            -o ${CVITE_FILTER_BASELINE_IR}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -O0 -fPIC -c ${CVITE_FILTER_BASELINE_IR}
            -o ${CVITE_FILTER_BASELINE_OBJECT}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -std=c11 -O0 -S -emit-llvm
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/filter_candidate_one.c
            -o ${CVITE_FILTER_ONE_RAW_IR}
        COMMAND
            ${CVITE_OPT_EXECUTABLE}
            -load-pass-plugin=$<TARGET_FILE:CViteLoweringPass>
            -passes=cvite-lowering,cvite-candidate,verify
            -S ${CVITE_FILTER_ONE_RAW_IR}
            -o ${CVITE_FILTER_ONE_IR}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -O0 -fPIC -c ${CVITE_FILTER_ONE_IR}
            -o ${CVITE_FILTER_ONE_OBJECT}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -std=c11 -O0 -S -emit-llvm
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/filter_candidate_two.c
            -o ${CVITE_FILTER_TWO_RAW_IR}
        COMMAND
            ${CVITE_OPT_EXECUTABLE}
            -load-pass-plugin=$<TARGET_FILE:CViteLoweringPass>
            -passes=cvite-lowering,cvite-candidate,verify
            -S ${CVITE_FILTER_TWO_RAW_IR}
            -o ${CVITE_FILTER_TWO_IR}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -O0 -fPIC -c ${CVITE_FILTER_TWO_IR}
            -o ${CVITE_FILTER_TWO_OBJECT}
        DEPENDS
            CViteLoweringPass
            tests/fixtures/filter_baseline.c
            tests/fixtures/filter_candidate_one.c
            tests/fixtures/filter_candidate_two.c
        VERBATIM
    )
    add_custom_target(cvite_candidate_filter_test_objects
        DEPENDS
            ${CVITE_FILTER_BASELINE_OBJECT}
            ${CVITE_FILTER_ONE_OBJECT}
            ${CVITE_FILTER_TWO_OBJECT}
    )
    add_executable(cvite_test_candidate_filter tests/test_candidate_filter.c)
    target_link_libraries(cvite_test_candidate_filter PRIVATE cvite::orc_loader)
    cvite_set_warnings(cvite_test_candidate_filter)
    add_dependencies(
        cvite_test_candidate_filter
        cvite_candidate_filter_test_objects
    )
    add_test(
        NAME candidate-filter
        COMMAND
            cvite_test_candidate_filter
            ${CVITE_FILTER_BASELINE_OBJECT}
            ${CVITE_FILTER_ONE_OBJECT}
            ${CVITE_FILTER_TWO_OBJECT}
    )
endif()
'''
    text = text.replace(marker, "\n" + block + marker, 1)
    cmake.write_text(text, encoding="utf-8")

doc = root / "docs/candidate-objects.md"
text = doc.read_text(encoding="utf-8")
if "## Changed-implementation filtering" not in text:
    text = text.rstrip() + """

## Changed-implementation filtering

Candidate function records carry a deterministic implementation fingerprint in
addition to the stable declaration ID and lowered ABI fingerprint. The ORC
loader remembers fingerprints only after the corresponding runtime transaction
has been published successfully.

On later refreshes, a function whose implementation fingerprint is already
active is omitted from the `cvite_patch`. Storage and ABI validation still run
for the complete candidate before publication, so narrowing the update set does
not weaken the compatibility gate.

The first candidate after baseline startup currently seeds this table and may
publish every candidate function. From the second successful candidate onward,
only implementations that differ from the last published generation are sent
to the transactional runtime. Failed candidates never advance the table.
"""
    doc.write_text(text, encoding="utf-8")

# Remove no-longer-needed snapshot/applicator plumbing when present.
ci = root / ".github/workflows/ci.yml"
if ci.exists():
    text = ci.read_text(encoding="utf-8")
    text = re.sub(
        r"\n  source-snapshot:\n.*?(?=\n  [A-Za-z0-9_-]+:|\Z)",
        "",
        text,
        flags=re.S,
    )
    ci.write_text(text, encoding="utf-8")

print("changed-implementation filter applied")
