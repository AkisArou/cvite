from __future__ import annotations

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def load(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def save(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text and old not in text:
        return text
    if old not in text:
        raise SystemExit(f"missing marker for {label}: {old[:120]!r}")
    return text.replace(old, new, 1)


def regex_once(text: str, pattern: str, replacement: str, label: str, flags: int = 0) -> str:
    updated, count = re.subn(pattern, replacement, text, count=1, flags=flags)
    if count != 1:
        raise SystemExit(f"expected one regex match for {label}, found {count}")
    return updated


def function_span(text: str, name: str) -> tuple[int, int]:
    match = re.search(rf"\b{name}\s*\([^;{{]*\)\s*\{{", text, re.S)
    if match is None:
        raise SystemExit(f"function {name} not found")
    brace = text.find("{", match.start())
    depth = 0
    index = brace
    in_string = False
    in_char = False
    escaped = False
    line_comment = False
    block_comment = False
    while index < len(text):
        ch = text[index]
        nxt = text[index + 1] if index + 1 < len(text) else ""
        if line_comment:
            if ch == "\n":
                line_comment = False
        elif block_comment:
            if ch == "*" and nxt == "/":
                block_comment = False
                index += 1
        elif in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
        elif in_char:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == "'":
                in_char = False
        elif ch == "/" and nxt == "/":
            line_comment = True
            index += 1
        elif ch == "/" and nxt == "*":
            block_comment = True
            index += 1
        elif ch == '"':
            in_string = True
        elif ch == "'":
            in_char = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return match.start(), index + 1
        index += 1
    raise SystemExit(f"unterminated function {name}")


def transform_function(text: str, name: str, transform) -> str:
    start, end = function_span(text, name)
    body = text[start:end]
    updated = transform(body)
    if updated == body:
        raise SystemExit(f"function transform for {name} made no change")
    return text[:start] + updated + text[end:]


# ---------------------------------------------------------------------------
# Runtime patch commit callback: loader state changes only after publication.
# ---------------------------------------------------------------------------
runtime_h = load("include/cvite/runtime.h")
if "cvite_patch_commit_callback" not in runtime_h:
    marker = "typedef struct cvite_patch {"
    runtime_h = replace_once(
        runtime_h,
        marker,
        "typedef void (*cvite_patch_commit_callback)(void *context);\n\n" + marker,
        "runtime patch callback typedef",
    )
    pattern = r"(typedef struct cvite_patch\s*\{)(.*?)(\n\}\s*cvite_patch;)"
    match = re.search(pattern, runtime_h, re.S)
    if match is None:
        raise SystemExit("cvite_patch definition not found")
    body = match.group(2)
    body += (
        "\n    cvite_patch_commit_callback commit_callback;"
        "\n    void *commit_context;"
    )
    runtime_h = runtime_h[:match.start()] + match.group(1) + body + match.group(3) + runtime_h[match.end():]
save("include/cvite/runtime.h", runtime_h)

runtime_patch = load("src/runtime/runtime_patch.c")
if "patch->commit_callback(patch->commit_context)" not in runtime_patch:
    def add_runtime_commit_callback(fn: str) -> str:
        success = fn.rfind("return CVITE_STATUS_OK;")
        if success < 0:
            raise SystemExit("runtime apply success return not found")
        insertion = (
            "if (patch->commit_callback != NULL) {\n"
            "        patch->commit_callback(patch->commit_context);\n"
            "    }\n\n    "
        )
        return fn[:success] + insertion + fn[success:]
    runtime_patch = transform_function(
        runtime_patch, "cvite_runtime_apply_patch", add_runtime_commit_callback)
save("src/runtime/runtime_patch.c", runtime_patch)

# ---------------------------------------------------------------------------
# Invisible reader gate used by generated native entry wrappers.
# ---------------------------------------------------------------------------
host_h = load("include/cvite/host.h")
if "cvite_host_try_begin_collection" not in host_h:
    if "#include <stdbool.h>" not in host_h:
        host_h = host_h.replace("#include <stddef.h>", "#include <stdbool.h>\n#include <stddef.h>", 1)
    declarations = """
/* Compiler/runtime ABI. Application source must not call these directly. */
void __cvite_host_call_enter(void);
void __cvite_host_call_leave(void);

/* Internal quiescence gate used by the ORC generation collector. */
bool cvite_host_try_begin_collection(void);
void cvite_host_end_collection(void);
uint64_t cvite_host_active_call_count(void);

"""
    bottom = host_h.rfind("#ifdef __cplusplus")
    if bottom < 0:
        raise SystemExit("host header C++ footer not found")
    host_h = host_h[:bottom] + declarations + host_h[bottom:]
save("include/cvite/host.h", host_h)

host_runtime = load("src/host/host_runtime.c")
if "cvite_host_collection_active" not in host_runtime:
    marker = "static atomic_bool cvite_host_is_sealed = false;"
    host_runtime = replace_once(
        host_runtime,
        marker,
        marker
        + "\nstatic atomic_bool cvite_host_collection_active = false;"
        + "\nstatic atomic_uint_fast64_t cvite_host_active_calls = 0U;",
        "host quiescence atomics",
    )
    host_runtime += r'''

void __cvite_host_call_enter(void)
{
    for (;;) {
        while (atomic_load_explicit(
            &cvite_host_collection_active, memory_order_seq_cst)) {
            atomic_signal_fence(memory_order_seq_cst);
        }

        (void)atomic_fetch_add_explicit(
            &cvite_host_active_calls, UINT64_C(1), memory_order_seq_cst);
        if (!atomic_load_explicit(
                &cvite_host_collection_active, memory_order_seq_cst)) {
            return;
        }

        (void)atomic_fetch_sub_explicit(
            &cvite_host_active_calls, UINT64_C(1), memory_order_seq_cst);
    }
}

void __cvite_host_call_leave(void)
{
    const uint_fast64_t previous = atomic_fetch_sub_explicit(
        &cvite_host_active_calls, UINT64_C(1), memory_order_seq_cst);
    if (previous == 0U) {
        (void)fprintf(stderr, "cvite host: unbalanced native call scope\n");
        abort();
    }
}

bool cvite_host_try_begin_collection(void)
{
    bool expected = false;

    if (!atomic_compare_exchange_strong_explicit(
            &cvite_host_collection_active,
            &expected,
            true,
            memory_order_seq_cst,
            memory_order_seq_cst)) {
        return false;
    }

    if (atomic_load_explicit(
            &cvite_host_active_calls, memory_order_seq_cst) != 0U) {
        atomic_store_explicit(
            &cvite_host_collection_active, false, memory_order_seq_cst);
        return false;
    }
    return true;
}

void cvite_host_end_collection(void)
{
    bool expected = true;
    if (!atomic_compare_exchange_strong_explicit(
            &cvite_host_collection_active,
            &expected,
            false,
            memory_order_seq_cst,
            memory_order_seq_cst)) {
        (void)fprintf(stderr, "cvite host: invalid collection-gate release\n");
        abort();
    }
}

uint64_t cvite_host_active_call_count(void)
{
    return (uint64_t)atomic_load_explicit(
        &cvite_host_active_calls, memory_order_seq_cst);
}
'''
save("src/host/host_runtime.c", host_runtime)

# ---------------------------------------------------------------------------
# Candidate ABI v4: deterministic implementation identity and reclaimability.
# ---------------------------------------------------------------------------
candidate_h = load("include/cvite/candidate.h")
candidate_h = re.sub(
    r"#define CVITE_CANDIDATE_MANIFEST_SCHEMA UINT64_C\(\d+\)",
    "#define CVITE_CANDIDATE_MANIFEST_SCHEMA UINT64_C(4)",
    candidate_h,
    count=1,
)
if "CVITE_CANDIDATE_FLAG_SAFE_TO_RECLAIM" not in candidate_h:
    schema_line = "#define CVITE_CANDIDATE_MANIFEST_SCHEMA UINT64_C(4)"
    candidate_h = replace_once(
        candidate_h,
        schema_line,
        schema_line
        + "\n#define CVITE_CANDIDATE_FLAG_SAFE_TO_RECLAIM UINT64_C(1)",
        "candidate reclaimability flag",
    )
if "implementation_high" not in candidate_h:
    function_match = re.search(
        r"(typedef struct cvite_candidate_function\s*\{)(.*?)(\n\}\s*cvite_candidate_function;)",
        candidate_h,
        re.S,
    )
    if function_match is None:
        raise SystemExit("candidate function record not found")
    body = function_match.group(2)
    body = replace_once(
        body,
        "    uint64_t abi_low;",
        "    uint64_t abi_low;\n"
        "    uint64_t implementation_high;\n"
        "    uint64_t implementation_low;",
        "candidate implementation fingerprint fields",
    )
    candidate_h = (
        candidate_h[:function_match.start()]
        + function_match.group(1)
        + body
        + function_match.group(3)
        + candidate_h[function_match.end():]
    )
if "compatibility_flags" not in candidate_h:
    manifest_match = re.search(
        r"(typedef struct cvite_candidate_manifest\s*\{)(.*?)(\n\}\s*cvite_candidate_manifest;)",
        candidate_h,
        re.S,
    )
    if manifest_match is None:
        raise SystemExit("candidate manifest record not found")
    body = manifest_match.group(2) + "\n    uint64_t compatibility_flags;"
    candidate_h = (
        candidate_h[:manifest_match.start()]
        + manifest_match.group(1)
        + body
        + manifest_match.group(3)
        + candidate_h[manifest_match.end():]
    )
save("include/cvite/candidate.h", candidate_h)

support = load("src/transform/transform_support.h")
if "kImplementationSchema" not in support:
    marker = 'inline constexpr llvm::StringLiteral kFunctionIndex = "cvite.functions";\n'
    support = replace_once(
        support,
        marker,
        marker
        + 'inline constexpr llvm::StringLiteral kImplementationSchema =\n'
        + '    "cvite.implementation.v1";\n'
        + 'inline constexpr llvm::StringLiteral kImplementationMetadata =\n'
        + '    "cvite.implementation";\n',
        "implementation schema constants",
    )
    marker = "inline std::string identitySeed(\n"
    implementation_seed = r'''inline std::string implementationSeed(const llvm::Function &function)
{
    std::string seed;
    llvm::raw_string_ostream output(seed);

    output << kImplementationSchema << '\n';
    function.print(output, nullptr, false, false);
    return output.str();
}

'''
    support = replace_once(
        support,
        marker,
        implementation_seed + marker,
        "implementation seed helper",
    )
save("src/transform/transform_support.h", support)

# ---------------------------------------------------------------------------
# Generated stable and candidate entries hold an invisible native call scope.
# ---------------------------------------------------------------------------
def instrument_entry_pass(path: str, function_name: str, target_constant: str) -> None:
    text = load(path)
    if "__cvite_host_call_enter" not in text:
        const_marker = target_constant
        text = replace_once(
            text,
            const_marker,
            const_marker
            + 'constexpr llvm::StringLiteral kCallEnter = "__cvite_host_call_enter";\n'
            + 'constexpr llvm::StringLiteral kCallLeave = "__cvite_host_call_leave";\n',
            f"{path} call-scope constants",
        )
        helper_marker = "void copyArgumentNames("
        helper = r'''llvm::FunctionCallee getCallScopeHook(
    llvm::Module &module,
    llvm::StringRef name)
{
    llvm::FunctionType *type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(module.getContext()), false);
    return module.getOrInsertFunction(name, type);
}

'''
        text = replace_once(
            text,
            helper_marker,
            helper + helper_marker,
            f"{path} call-scope helper",
        )

        def instrument(fn: str) -> str:
            fn = replace_once(
                fn,
                "    llvm::IRBuilder<> builder(entry_block);",
                "    llvm::IRBuilder<> builder(entry_block);\n"
                "    builder.CreateCall(getCallScopeHook(module, kCallEnter), {});",
                f"{path} entry hook",
            )
            tail = "    call->setTailCallKind(llvm::CallInst::TCK_Tail);"
            fn = replace_once(
                fn,
                tail,
                "    builder.CreateCall(getCallScopeHook(module, kCallLeave), {});",
                f"{path} leave hook",
            )
            return fn

        text = transform_function(text, function_name, instrument)
    save(path, text)

instrument_entry_pass(
    "src/transform/stable_entry_pass.cpp",
    "wrapFunction",
    'constexpr llvm::StringLiteral kTargetAt = "__cvite_host_target_at";\n',
)
instrument_entry_pass(
    "src/transform/candidate_pass.cpp",
    "transformFunction",
    'constexpr llvm::StringLiteral kTargetFor = "__cvite_host_target_for";\n',
)

# ---------------------------------------------------------------------------
# Candidate pass emits implementation fingerprints and a conservative flag.
# ---------------------------------------------------------------------------
candidate_cpp = load("src/transform/candidate_pass.cpp")
if "Hash128 implementation;" not in candidate_cpp:
    candidate_cpp = replace_once(
        candidate_cpp,
        "    Hash128 identity;\n    Hash128 abi;\n    std::string debug_name;",
        "    Hash128 identity;\n"
        "    Hash128 abi;\n"
        "    Hash128 implementation;\n"
        "    std::string debug_name;",
        "candidate implementation member",
    )

    def add_implementation(fn: str) -> str:
        marker = (
            "    const Hash128 abi = cvite::transform::hash128(\n"
            "        cvite::transform::abiSeed(module, function));\n"
        )
        fn = replace_once(
            fn,
            marker,
            marker
            + "    const Hash128 implementation = cvite::transform::hash128(\n"
            + "        cvite::transform::implementationSeed(function));\n",
            "candidate implementation calculation",
        )
        old_return = "    return {&function, entry, identity, abi, original_name};"
        new_return = (
            "    return {\n"
            "        &function,\n"
            "        entry,\n"
            "        identity,\n"
            "        abi,\n"
            "        implementation,\n"
            "        original_name,\n"
            "    };"
        )
        fn = replace_once(fn, old_return, new_return, "candidate function return")
        return fn

    candidate_cpp = transform_function(candidate_cpp, "transformFunction", add_implementation)

    def update_function_records(fn: str) -> str:
        fn = replace_once(
            fn,
            "        {i64, i64, i64, i64, pointer, pointer},",
            "        {i64, i64, i64, i64, i64, i64, pointer, pointer},",
            "candidate function record LLVM type",
        )
        marker = (
            "                llvm::ConstantInt::get(i64, function.abi.low),\n"
            "                function.implementation,"
        )
        fn = replace_once(
            fn,
            marker,
            "                llvm::ConstantInt::get(i64, function.abi.low),\n"
            "                llvm::ConstantInt::get(\n"
            "                    i64, function.implementation.high),\n"
            "                llvm::ConstantInt::get(\n"
            "                    i64, function.implementation.low),\n"
            "                function.implementation,",
            "candidate function record initializer",
        )
        return fn

    candidate_cpp = transform_function(
        candidate_cpp, "createFunctionRecords", update_function_records)

if "compatibility_flags" not in candidate_cpp:
    candidate_cpp = replace_once(
        candidate_cpp,
        "constexpr unsigned kCandidateSchema = 2U;",
        "constexpr unsigned kCandidateSchema = 4U;\n"
        "constexpr std::uint64_t kSafeToReclaimFlag = UINT64_C(1);",
        "candidate schema version",
    ) if "constexpr unsigned kCandidateSchema = 2U;" in candidate_cpp else re.sub(
        r"constexpr unsigned kCandidateSchema = \d+U;",
        "constexpr unsigned kCandidateSchema = 4U;\n"
        "constexpr std::uint64_t kSafeToReclaimFlag = UINT64_C(1);",
        candidate_cpp,
        count=1,
    )

    helper_marker = "bool shouldTransform(const llvm::Function &function)"
    helper = r'''bool containsInlineAssembly(const llvm::Function &function)
{
    for (const llvm::BasicBlock &block : function) {
        for (const llvm::Instruction &instruction : block) {
            const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if (call != nullptr && call->isInlineAsm()) {
                return true;
            }
        }
    }
    return false;
}

'''
    candidate_cpp = replace_once(
        candidate_cpp,
        helper_marker,
        helper + helper_marker,
        "inline-assembly analysis helper",
    )

    def make_manifest_flagged(fn: str) -> str:
        signature_old = (
            "    llvm::ArrayRef<CandidateFunction> functions,\n"
            "    llvm::ArrayRef<CandidateStorage> storages)"
        )
        signature_new = (
            "    llvm::ArrayRef<CandidateFunction> functions,\n"
            "    llvm::ArrayRef<CandidateStorage> storages,\n"
            "    std::uint64_t compatibility_flags)"
        )
        fn = replace_once(fn, signature_old, signature_new, "manifest flags parameter")
        type_patterns = [
            "        {i64, i64, pointer, i64, pointer},",
            "        {i64, pointer, i64, pointer},",
        ]
        replaced_type = False
        for old in type_patterns:
            if old in fn:
                values = old[old.find("{") + 1:old.rfind("}")].strip()
                new = old.replace(values, values + ", i64")
                fn = fn.replace(old, new, 1)
                replaced_type = True
                break
        if not replaced_type:
            raise SystemExit("candidate manifest LLVM type marker not found")
        closing = re.search(
            r"(storage_records,\n\s*)(\}\));",
            fn,
        )
        if closing is None:
            raise SystemExit("candidate manifest initializer tail not found")
        replacement = (
            closing.group(1)
            + "llvm::ConstantInt::get(i64, compatibility_flags),\n        "
            + closing.group(2)
        )
        fn = fn[:closing.start()] + replacement + fn[closing.end():]
        return fn

    candidate_cpp = transform_function(candidate_cpp, "createManifest", make_manifest_flagged)

    def update_candidate_run(fn: str) -> str:
        transform_loop = re.search(
            r"(llvm::SmallVector<CandidateFunction,\s*32>\s+functions;.*?functions\.reserve\(candidates\.size\(\)\);)",
            fn,
            re.S,
        )
        if transform_loop is None:
            transform_loop = re.search(
                r"(llvm::SmallVector<CandidateFunction,\s*32>\s+transformed;.*?transformed\.reserve\(candidates\.size\(\)\);)",
                fn,
                re.S,
            )
        if transform_loop is None:
            raise SystemExit("candidate transformed-function vector not found")
        analysis = r'''

        bool safe_to_reclaim = module.getModuleInlineAsm().empty() &&
            module.getNamedGlobal("llvm.global_ctors") == nullptr &&
            module.getNamedGlobal("llvm.global_dtors") == nullptr;
        for (const llvm::GlobalVariable &global : module.globals()) {
            if (global.isThreadLocal()) {
                safe_to_reclaim = false;
                break;
            }
        }
        for (const llvm::Function *function : candidates) {
            if (function->hasAddressTaken() || containsInlineAssembly(*function)) {
                safe_to_reclaim = false;
                break;
            }
        }
'''
        insertion = transform_loop.end()
        fn = fn[:insertion] + analysis + fn[insertion:]
        call = re.search(r"createManifest\(module,\s*([^,]+),\s*([^\)]+)\);", fn)
        if call is None:
            raise SystemExit("candidate createManifest call not found")
        replacement = (
            f"createManifest(module, {call.group(1).strip()}, {call.group(2).strip()},\n"
            "            safe_to_reclaim ? kSafeToReclaimFlag : UINT64_C(0));"
        )
        fn = fn[:call.start()] + replacement + fn[call.end():]
        return fn

    candidate_cpp = transform_function(candidate_cpp, "run", update_candidate_run)
save("src/transform/candidate_pass.cpp", candidate_cpp)

# ---------------------------------------------------------------------------
# ORC ownership, transactional filtering, and generation collection.
# ---------------------------------------------------------------------------
orc_h = load("include/cvite/orc_loader.h")
if "cvite_orc_loader_collect_retired" not in orc_h:
    marker = "cvite_status cvite_orc_loader_discard_generation(\n"
    declaration = (
        "cvite_status cvite_orc_loader_collect_retired(\n"
        "    cvite_orc_loader *loader,\n"
        "    size_t *reclaimed_count,\n"
        "    cvite_error *error);\n\n"
    )
    orc_h = replace_once(
        orc_h,
        marker,
        declaration + marker,
        "ORC collection declaration",
    )
save("include/cvite/orc_loader.h", orc_h)

orc = load("src/host/orc_loader.cpp")
if "PreparedFingerprint" not in orc:
    old_generation = """struct Generation final {
    llvm::orc::JITDylib *dylib = nullptr;
    llvm::orc::ResourceTrackerSP resources;
    std::vector<cvite_function_update> updates;
    bool baseline_prepared = false;
};"""
    new_generation = """struct PreparedFingerprint final {
    cvite_id id = CVITE_ID_ZERO;
    cvite_id implementation = CVITE_ID_ZERO;
};

struct FunctionOwner final {
    cvite_id id = CVITE_ID_ZERO;
    cvite_orc_generation generation = 0U;
};

struct CommitContext final {
    cvite_orc_loader *loader = nullptr;
    cvite_orc_generation generation = 0U;
};

struct Generation final {
    llvm::orc::JITDylib *dylib = nullptr;
    llvm::orc::ResourceTrackerSP resources;
    std::vector<cvite_function_update> updates;
    bool baseline_prepared = false;
    std::vector<PreparedFingerprint> prepared_fingerprints;
    std::vector<cvite_id> owned_functions;
    std::unique_ptr<CommitContext> commit_context;
    bool patch_prepared = false;
    bool patch_committed = false;
    bool pinned = false;
    bool retired = false;
};"""
    orc = replace_once(orc, old_generation, new_generation, "ORC generation state")

    owner_marker = (
        "    std::vector<HostFunction> host_functions;\n"
        "    std::unordered_map<cvite_orc_generation, Generation> generations;"
    )
    orc = replace_once(
        orc,
        owner_marker,
        "    std::vector<HostFunction> host_functions;\n"
        "    std::vector<PreparedFingerprint> active_fingerprints;\n"
        "    std::vector<FunctionOwner> function_owners;\n"
        "    std::unordered_map<cvite_orc_generation, Generation> generations;",
        "ORC ownership fields",
    )

    helper_anchor = orc.find('extern "C" cvite_status cvite_orc_loader_create')
    if helper_anchor < 0:
        raise SystemExit("ORC public create function anchor not found")
    helpers = r'''
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

FunctionOwner *findOwner(cvite_orc_loader &loader, cvite_id id)
{
    for (FunctionOwner &owner : loader.function_owners) {
        if (cvite_id_equal(owner.id, id)) {
            return &owner;
        }
    }
    return nullptr;
}

bool hasOwnedFunction(const Generation &generation, cvite_id id)
{
    for (const cvite_id owned : generation.owned_functions) {
        if (cvite_id_equal(owned, id)) {
            return true;
        }
    }
    return false;
}

void removeOwnedFunction(Generation &generation, cvite_id id)
{
    generation.owned_functions.erase(
        std::remove_if(
            generation.owned_functions.begin(),
            generation.owned_functions.end(),
            [id](cvite_id owned) { return cvite_id_equal(owned, id); }),
        generation.owned_functions.end());
}

bool traceLifecycleEnabled()
{
    const char *value = std::getenv("CVITE_TRACE_LIFECYCLE");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

cvite_status removeGenerationInternal(
    cvite_orc_loader *loader,
    cvite_orc_generation generation,
    cvite_error *error)
{
    const auto found = loader->generations.find(generation);
    if (found == loader->generations.end()) {
        return fail(
            error,
            CVITE_STATUS_UNKNOWN_SYMBOL,
            "unknown ORC generation");
    }

    llvm::orc::JITDylib *dylib = found->second.dylib;
    if (found->second.resources != nullptr) {
        if (llvm::Error removal = found->second.resources->remove()) {
            return fail(
                error,
                CVITE_STATUS_INVALID_STATE,
                llvm::toString(std::move(removal)));
        }
        found->second.resources.reset();
    }

    if (dylib != nullptr) {
        if (llvm::Error removal =
                loader->jit->getExecutionSession().removeJITDylib(*dylib)) {
            return fail(
                error,
                CVITE_STATUS_INVALID_STATE,
                llvm::toString(std::move(removal)));
        }
    }
    loader->generations.erase(found);
    return CVITE_STATUS_OK;
}

cvite_status collectRetiredInternal(
    cvite_orc_loader *loader,
    size_t *reclaimed_count,
    cvite_error *error)
{
    *reclaimed_count = 0U;
    if (!cvite_host_try_begin_collection()) {
        return CVITE_STATUS_OK;
    }

    std::vector<cvite_orc_generation> retired;
    for (const auto &entry : loader->generations) {
        const Generation &generation = entry.second;
        if (generation.retired && !generation.pinned &&
            generation.owned_functions.empty() &&
            !generation.baseline_prepared) {
            retired.push_back(entry.first);
        }
    }
    std::sort(retired.begin(), retired.end());

    cvite_status status = CVITE_STATUS_OK;
    for (const cvite_orc_generation generation : retired) {
        status = removeGenerationInternal(loader, generation, error);
        if (status != CVITE_STATUS_OK) {
            break;
        }
        *reclaimed_count += 1U;
    }
    cvite_host_end_collection();
    return status;
}

void commitPreparedPatch(void *opaque)
{
    auto *context = static_cast<CommitContext *>(opaque);
    cvite_orc_loader *loader = context->loader;
    const cvite_orc_generation generation_id = context->generation;
    const auto found = loader->generations.find(generation_id);
    if (found == loader->generations.end()) {
        std::abort();
    }

    Generation &candidate = found->second;
    if (!candidate.patch_prepared || candidate.patch_committed) {
        std::abort();
    }

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

    for (const cvite_function_update &update : candidate.updates) {
        FunctionOwner *owner = findOwner(*loader, update.id);
        if (owner != nullptr) {
            const auto old = loader->generations.find(owner->generation);
            if (old != loader->generations.end()) {
                removeOwnedFunction(old->second, update.id);
                if (old->second.owned_functions.empty() &&
                    !old->second.pinned &&
                    !old->second.baseline_prepared) {
                    old->second.retired = true;
                }
            }
            owner->generation = generation_id;
        } else {
            loader->function_owners.push_back(
                FunctionOwner{update.id, generation_id});
        }
        if (!hasOwnedFunction(candidate, update.id)) {
            candidate.owned_functions.push_back(update.id);
        }
    }

    candidate.patch_committed = true;
    if (candidate.owned_functions.empty()) {
        candidate.pinned = false;
        candidate.retired = true;
    }

    if (traceLifecycleEnabled()) {
        (void)std::fprintf(
            stderr,
            "[cvite] generation %" PRIu64
            " published %zu changed implementation%s\n",
            static_cast<std::uint64_t>(generation_id),
            candidate.updates.size(),
            candidate.updates.size() == 1U ? "" : "s");
    }

    cvite_error collection_error;
    size_t reclaimed = 0U;
    const cvite_status collection_status =
        collectRetiredInternal(loader, &reclaimed, &collection_error);
    if (collection_status != CVITE_STATUS_OK) {
        if (traceLifecycleEnabled()) {
            (void)std::fprintf(
                stderr,
                "[cvite] generation collection deferred: %s\n",
                collection_error.message);
        }
        return;
    }
    if (traceLifecycleEnabled() && reclaimed != 0U) {
        (void)std::fprintf(
            stderr,
            "[cvite] reclaimed %zu retired generation%s\n",
            reclaimed,
            reclaimed == 1U ? "" : "s");
    }
}

'''
    orc = orc[:helper_anchor] + helpers + orc[helper_anchor:]

    # Track the baseline generation as the permanent owner of initial targets.
    def baseline_ownership(fn: str) -> str:
        marker = "    found->second.baseline_prepared = true;"
        insertion = r'''    Generation &baseline_generation = found->second;
    baseline_generation.pinned = true;
    for (std::uint64_t index = 0U;
         index < manifest->function_count;
         ++index) {
        const cvite_baseline_function &record = manifest->functions[index];
        const cvite_id id{record.id_high, record.id_low};
        if (!hasOwnedFunction(baseline_generation, id)) {
            baseline_generation.owned_functions.push_back(id);
        }
        FunctionOwner *owner = findOwner(*loader, id);
        if (owner == nullptr) {
            loader->function_owners.push_back(FunctionOwner{id, generation});
        } else {
            owner->generation = generation;
        }
    }

'''
        return replace_once(fn, marker, insertion + marker, "baseline ownership")

    orc = transform_function(orc, "cvite_orc_loader_prepare_baseline", baseline_ownership)

    # Replace the candidate function loop with full validation plus filtering.
    def filter_candidate(fn: str) -> str:
        start = fn.find("    found->second.updates.clear();")
        if start < 0:
            raise SystemExit("candidate update loop start not found")
        storage_ref = fn.find("manifest->storage_count", start)
        if storage_ref < 0:
            raise SystemExit("candidate storage validation not found")
        end = fn.rfind("    for (std::uint64_t index", start, storage_ref)
        if end < 0:
            raise SystemExit("candidate storage loop boundary not found")
        replacement = r'''    Generation &candidate = found->second;
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
    candidate.pinned =
        (manifest->compatibility_flags &
         CVITE_CANDIDATE_FLAG_SAFE_TO_RECLAIM) == 0U;

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
            record.debug_name == nullptr || record.debug_name[0] == '\0') {
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
        candidate.prepared_fingerprints.push_back(
            PreparedFingerprint{id, implementation});

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
    }

'''
        fn = fn[:start] + replacement + fn[end:]
        fn = fn.replace("found->second.updates.data()", "candidate.updates.data()")
        fn = fn.replace("found->second.updates.size()", "candidate.updates.size()")
        assignment = re.search(
            r"(patch->function_count\s*=\s*candidate\.updates\.size\(\);)",
            fn,
        )
        if assignment is None:
            raise SystemExit("candidate patch function count assignment not found")
        callback_setup = r'''
    candidate.patch_prepared = true;
    candidate.commit_context =
        std::make_unique<CommitContext>(CommitContext{loader, generation});
    patch->commit_callback = commitPreparedPatch;
    patch->commit_context = candidate.commit_context.get();'''
        insert = assignment.end()
        fn = fn[:insert] + callback_setup + fn[insert:]
        return fn

    orc = transform_function(orc, "cvite_orc_loader_prepare_patch", filter_candidate)

    # Keep absolute host symbols under the same resource tracker as object code.
    orc = orc.replace(
        ".define(llvm::orc::absoluteSymbols(std::move(symbols)))",
        ".define(\n                llvm::orc::absoluteSymbols(std::move(symbols)), resources)",
    )

# Add collection API and make failed-candidate discard ownership-aware.
if "cvite_status cvite_orc_loader_collect_retired(" not in orc:
    discard_start, _ = function_span(orc, "cvite_orc_loader_discard_generation")
    collection_api = r'''extern "C" cvite_status cvite_orc_loader_collect_retired(
    cvite_orc_loader *loader,
    size_t *reclaimed_count,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr || reclaimed_count == nullptr) {
        return fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "invalid ORC collection request");
    }
    return collectRetiredInternal(loader, reclaimed_count, error);
}

'''
    orc = orc[:discard_start] + collection_api + orc[discard_start:]

    def replace_discard(fn: str) -> str:
        signature_end = fn.find("{")
        signature = fn[:signature_end]
        body = r'''{
    cvite_error_clear(error);
    if (loader == nullptr || generation == 0U) {
        return fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "invalid ORC generation discard request");
    }
    const auto found = loader->generations.find(generation);
    if (found == loader->generations.end()) {
        return fail(
            error,
            CVITE_STATUS_UNKNOWN_SYMBOL,
            "unknown ORC generation");
    }
    if (!found->second.owned_functions.empty()) {
        return fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            "cannot discard a generation that owns published functions");
    }
    return removeGenerationInternal(loader, generation, error);
}'''
        return signature + body

    orc = transform_function(orc, "cvite_orc_loader_discard_generation", replace_discard)
save("src/host/orc_loader.cpp", orc)

# ---------------------------------------------------------------------------
# Tests: gate correctness, generated wrappers, and live filtered publication.
# ---------------------------------------------------------------------------
quiescence_test = r'''#include "cvite/host.h"

#include <assert.h>
#include <stdint.h>

int main(void)
{
    assert(cvite_host_active_call_count() == UINT64_C(0));
    assert(cvite_host_try_begin_collection());
    cvite_host_end_collection();

    __cvite_host_call_enter();
    assert(cvite_host_active_call_count() == UINT64_C(1));
    assert(!cvite_host_try_begin_collection());
    __cvite_host_call_leave();

    assert(cvite_host_active_call_count() == UINT64_C(0));
    assert(cvite_host_try_begin_collection());
    cvite_host_end_collection();
    return 0;
}
'''
save("tests/test_quiescence.c", quiescence_test)

call_scope_test = r'''#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def run(command: list[str]) -> None:
    subprocess.run(command, check=True, text=True, capture_output=True)


def verify(text: str, label: str) -> None:
    if "@__cvite_host_call_enter" not in text:
        raise AssertionError(f"{label}: missing call-enter hook")
    if "@__cvite_host_call_leave" not in text:
        raise AssertionError(f"{label}: missing call-leave hook")
    if "tail call" in "\n".join(
        line for line in text.splitlines() if "cvite.target" in line
    ):
        raise AssertionError(f"{label}: dispatch call must not be a tail call")


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit("usage: check_call_scope.py CLANG OPT PLUGIN INPUT OUTDIR")
    clang, opt, plugin, input_path, output_dir = sys.argv[1:]
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)
    raw = out / "call-scope.raw.ll"
    baseline = out / "call-scope.baseline.ll"
    candidate = out / "call-scope.candidate.ll"
    run([clang, "-std=c11", "-O0", "-S", "-emit-llvm", input_path, "-o", str(raw)])
    run([
        opt,
        f"-load-pass-plugin={plugin}",
        "-passes=cvite-lowering,cvite-baseline,verify",
        "-S",
        str(raw),
        "-o",
        str(baseline),
    ])
    run([
        opt,
        f"-load-pass-plugin={plugin}",
        "-passes=cvite-lowering,cvite-candidate,verify",
        "-S",
        str(raw),
        "-o",
        str(candidate),
    ])
    verify(baseline.read_text(encoding="utf-8"), "baseline")
    verify(candidate.read_text(encoding="utf-8"), "candidate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
'''
save("tests/check_call_scope.py", call_scope_test)

filtered_fixture = r'''#include <stdio.h>
#include <time.h>

static int state = 0;

static int stable_term(void)
{
    return 1000;
}

static int step(void)
{
    state += 1;
    return state + stable_term();
}

int main(void)
{
    struct timespec delay = {0, 20000000L};
    for (int iteration = 0; iteration < 900; ++iteration) {
        (void)printf("value=%d\n", step());
        (void)fflush(stdout);
        (void)nanosleep(&delay, NULL);
    }
    return 0;
}
'''
save("tests/fixtures/filtered_refresh.c", filtered_fixture)

filtered_test = r'''#!/usr/bin/env python3
from __future__ import annotations

import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: check_filtered_refresh.py CVITE FIXTURE")
    cvite = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="cvite-filter-") as directory:
        source = Path(directory) / "app.c"
        shutil.copy2(fixture, source)
        environment = os.environ.copy()
        environment["CVITE_TRACE_LIFECYCLE"] = "1"
        process = subprocess.Popen(
            [str(cvite), "run", str(source)],
            cwd=directory,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert process.stdout is not None
        output: queue.Queue[str] = queue.Queue()
        transcript: list[str] = []

        def reader() -> None:
            for line in process.stdout:
                transcript.append(line)
                output.put(line)

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()

        def wait_for(fragment: str, timeout: float = 25.0) -> str:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                remaining = max(0.05, deadline - time.monotonic())
                try:
                    line = output.get(timeout=min(0.5, remaining))
                except queue.Empty:
                    if process.poll() is not None:
                        break
                    continue
                if fragment in line:
                    return line
            joined = "".join(transcript[-200:])
            raise AssertionError(f"did not observe {fragment!r}\n{joined}")

        try:
            wait_for("value=")

            text = source.read_text(encoding="utf-8")
            source.write_text(text.replace("state += 1;", "state += 2;"), encoding="utf-8")
            wait_for("published 2 changed implementations")

            text = source.read_text(encoding="utf-8")
            source.write_text(text.replace("state += 2;", "state += 3;"), encoding="utf-8")
            wait_for("published 1 changed implementation")

            # A byte-identical save must produce a valid zero-function transaction.
            text = source.read_text(encoding="utf-8")
            source.write_text(text, encoding="utf-8")
            wait_for("published 0 changed implementations")

            # Replace the remaining implementation owned by the first candidate.
            text = source.read_text(encoding="utf-8")
            source.write_text(text.replace("return 1000;", "return 2000;"), encoding="utf-8")
            wait_for("published 1 changed implementation")
            wait_for("reclaimed 1 retired generation")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5.0)
            thread.join(timeout=2.0)

        if process.returncode not in (0, -15):
            raise AssertionError(
                f"cvite exited with {process.returncode}\n{''.join(transcript[-200:])}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
'''
save("tests/check_filtered_refresh.py", filtered_test)

cmake = load("CMakeLists.txt")
if "NAME quiescence-gate" not in cmake:
    cmake += r'''

if(BUILD_TESTING)
    add_executable(cvite_test_quiescence tests/test_quiescence.c)
    target_link_libraries(cvite_test_quiescence PRIVATE cvite::host)
    cvite_set_warnings(cvite_test_quiescence)
    add_test(NAME quiescence-gate COMMAND cvite_test_quiescence)
endif()

if(CVITE_BUILD_LLVM_PASS AND BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME generated-call-scope
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_call_scope.py
            ${CVITE_CLANG_EXECUTABLE}
            ${CVITE_OPT_EXECUTABLE}
            $<TARGET_FILE:CViteLoweringPass>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/stable_entry_input.c
            ${CMAKE_CURRENT_BINARY_DIR}/call-scope-test
    )
endif()

if(CVITE_BUILD_LLVM_PASS AND CVITE_BUILD_ORC_LOADER AND BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME run-filtered-refresh
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_filtered_refresh.py
            $<TARGET_FILE:cvite>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/filtered_refresh.c
    )
    set_tests_properties(run-filtered-refresh PROPERTIES TIMEOUT 70)
endif()
'''
save("CMakeLists.txt", cmake)

# Documentation is intentionally concise; the PR body carries the full status.
doc = load("docs/candidate-objects.md")
if "Implementation-level filtering" not in doc:
    doc += r'''

## Implementation-level filtering

Candidate manifest schema 4 carries a deterministic LLVM implementation
fingerprint for every refreshable function. The ORC loader validates the full
candidate ABI and persistent-storage contract, but publishes only records whose
implementation fingerprint differs from the last successfully committed
program. Fingerprint state advances through the runtime patch commit callback,
so a rejected transaction cannot poison the next comparison.

A byte-identical candidate is a valid zero-function transaction. Its object
namespace can be retired immediately after the compatibility gate succeeds.
'''
save("docs/candidate-objects.md", doc)

orc_doc = load("docs/orc-loader.md")
if "Quiescent generation reclamation" not in orc_doc:
    orc_doc += r'''

## Quiescent generation reclamation

The loader records which JIT generation owns each currently published function.
Ownership transfers only after the runtime atomically publishes the new dispatch
snapshot. A generation with no remaining functions becomes retired.

Compiler-generated entries bracket every native implementation call with an
invisible reader scope. Collection raises a writer gate and removes a retired
ORC resource tracker only when no reader can still be executing old machine
code. New callers are held outside the gate while removal occurs.

Generations are conservatively pinned when LLVM observes address-taken
refreshable functions, inline assembly, TLS, or native constructors/destructors.
This trades memory for safety when code addresses may have escaped compiler
control. `longjmp` or thread cancellation across a generated scope can likewise
delay collection, but cannot cause premature code removal.
'''
save("docs/orc-loader.md", orc_doc)

# The one-shot source-export workflow must not survive the verified feature commit.
print("filtering and reclamation patch applied")
