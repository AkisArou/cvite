from __future__ import annotations

from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text and old not in text:
        return text
    if old not in text:
        raise SystemExit(f"missing marker for {label}: {old[:120]!r}")
    return text.replace(old, new, 1)


# Recover the preceding milestone if its verification workflow exposed a
# compiler issue before it could commit.
if "cvite_patch_commit_callback" not in read("include/cvite/runtime.h"):
    previous = ROOT / ".cvite/apply-filter-reclaim.py"
    if not previous.exists():
        raise SystemExit("filter/reclamation implementation is not available")
    patcher = previous.read_text(encoding="utf-8")
    old_patterns = '''        type_patterns = [
            "        {i64, i64, pointer, i64, pointer},",
            "        {i64, pointer, i64, pointer},",
        ]'''
    new_patterns = '''        type_patterns = [
            "        {i64, i64, pointer, i64, pointer},",
            "        {i64, pointer, i64, pointer},",
            "        {i64, pointer, i64, pointer, i64},",
            "        {i64, i64, pointer, i64, pointer, i64},",
        ]'''
    if old_patterns in patcher:
        patcher = patcher.replace(old_patterns, new_patterns, 1)
        previous.write_text(patcher, encoding="utf-8")
    subprocess.run(["python3", str(previous)], cwd=ROOT, check=True)

# Normalize C++ compilation against LLVM headers and repair strict-warning
# issues without weakening warnings for CVite's own code.
cmake = read("CMakeLists.txt")
cmake = cmake.replace(
    "target_include_directories(cvite PRIVATE ${CVITE_LIBCLANG_INCLUDE_DIR})",
    "target_include_directories(cvite SYSTEM PRIVATE ${CVITE_LIBCLANG_INCLUDE_DIR})",
)
write("CMakeLists.txt", cmake)

orc = read("src/host/orc_loader.cpp")
for header in ["<algorithm>", "<cinttypes>", "<cstdio>", "<cstdlib>", "<cstring>"]:
    include = f"#include {header}"
    if include not in orc:
        first_std = orc.find("#include <")
        if first_std < 0:
            raise SystemExit("ORC standard include block not found")
        orc = orc[:first_std] + include + "\n" + orc[first_std:]

if "cvite_status failLLVMRemoval" not in orc and "llvm::toString(std::move(removal))" in orc:
    anchor = orc.find("const PreparedFingerprint *findFingerprint(")
    if anchor < 0:
        raise SystemExit("ORC helper anchor not found")
    helper = '''cvite_status failLLVMRemoval(cvite_error *error, llvm::Error removal)
{
    const std::string message = llvm::toString(std::move(removal));
    return fail(error, CVITE_STATUS_INVALID_STATE, message.c_str());
}

'''
    orc = orc[:anchor] + helper + orc[anchor:]
    orc = re.sub(
        r"return fail\(\s*error,\s*CVITE_STATUS_INVALID_STATE,\s*llvm::toString\(std::move\(removal\)\)\s*\);",
        "return failLLVMRemoval(error, std::move(removal));",
        orc,
    )
write("src/host/orc_loader.cpp", orc)

# Candidate flags explain native-state boundaries instead of treating all
# non-reclaimable code alike.
candidate_h = read("include/cvite/candidate.h")
if "CVITE_CANDIDATE_FLAG_RESTART_TLS" not in candidate_h:
    marker = "#define CVITE_CANDIDATE_FLAG_SAFE_TO_RECLAIM UINT64_C(1)"
    candidate_h = replace_once(
        candidate_h,
        marker,
        marker
        + "\n#define CVITE_CANDIDATE_FLAG_RESTART_TLS UINT64_C(2)"
        + "\n#define CVITE_CANDIDATE_FLAG_RESTART_CONSTRUCTORS UINT64_C(4)"
        + "\n#define CVITE_CANDIDATE_FLAG_ADDRESS_ESCAPES UINT64_C(8)"
        + "\n#define CVITE_CANDIDATE_FLAG_INLINE_ASSEMBLY UINT64_C(16)",
        "candidate native-state flags",
    )
write("include/cvite/candidate.h", candidate_h)

candidate_cpp = read("src/transform/candidate_pass.cpp")
if "kRestartTlsFlag" not in candidate_cpp:
    marker = "constexpr std::uint64_t kSafeToReclaimFlag = UINT64_C(1);"
    candidate_cpp = replace_once(
        candidate_cpp,
        marker,
        marker
        + "\nconstexpr std::uint64_t kRestartTlsFlag = UINT64_C(2);"
        + "\nconstexpr std::uint64_t kRestartConstructorsFlag = UINT64_C(4);"
        + "\nconstexpr std::uint64_t kAddressEscapesFlag = UINT64_C(8);"
        + "\nconstexpr std::uint64_t kInlineAssemblyFlag = UINT64_C(16);",
        "candidate native-state constants",
    )

    old_analysis = '''        bool safe_to_reclaim = module.getModuleInlineAsm().empty() &&
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
        }'''
    new_analysis = '''        std::uint64_t compatibility_flags = UINT64_C(0);
        const bool has_constructors =
            module.getNamedGlobal("llvm.global_ctors") != nullptr ||
            module.getNamedGlobal("llvm.global_dtors") != nullptr;
        if (has_constructors) {
            compatibility_flags |= kRestartConstructorsFlag;
        }
        if (!module.getModuleInlineAsm().empty()) {
            compatibility_flags |= kInlineAssemblyFlag;
        }
        for (const llvm::GlobalVariable &global : module.globals()) {
            if (global.isThreadLocal()) {
                compatibility_flags |= kRestartTlsFlag;
            }
        }
        for (const llvm::Function *function : candidates) {
            if (function->hasAddressTaken()) {
                compatibility_flags |= kAddressEscapesFlag;
            }
            if (containsInlineAssembly(*function)) {
                compatibility_flags |= kInlineAssemblyFlag;
            }
        }
        if ((compatibility_flags &
             (kRestartTlsFlag | kRestartConstructorsFlag |
              kAddressEscapesFlag | kInlineAssemblyFlag)) == 0U) {
            compatibility_flags |= kSafeToReclaimFlag;
        }'''
    candidate_cpp = replace_once(
        candidate_cpp,
        old_analysis,
        new_analysis,
        "candidate compatibility analysis",
    )
    candidate_cpp = replace_once(
        candidate_cpp,
        "safe_to_reclaim ? kSafeToReclaimFlag : UINT64_C(0)",
        "compatibility_flags",
        "candidate compatibility manifest value",
    )
write("src/transform/candidate_pass.cpp", candidate_cpp)

orc = read("src/host/orc_loader.cpp")
if "candidate uses thread-local storage" not in orc:
    marker = '''    candidate.pinned =
        (manifest->compatibility_flags &
         CVITE_CANDIDATE_FLAG_SAFE_TO_RECLAIM) == 0U;
'''
    check = '''    const std::uint64_t restart_flags =
        manifest->compatibility_flags &
        (CVITE_CANDIDATE_FLAG_RESTART_TLS |
         CVITE_CANDIDATE_FLAG_RESTART_CONSTRUCTORS);
    if ((restart_flags & CVITE_CANDIDATE_FLAG_RESTART_TLS) != 0U) {
        return fail(
            error,
            CVITE_STATUS_LAYOUT_MISMATCH,
            "candidate uses thread-local storage; process restart required");
    }
    if ((restart_flags &
         CVITE_CANDIDATE_FLAG_RESTART_CONSTRUCTORS) != 0U) {
        return fail(
            error,
            CVITE_STATUS_LAYOUT_MISMATCH,
            "candidate contains native constructors or destructors; "
            "process restart required");
    }
'''
    orc = replace_once(orc, marker, marker + check, "ORC restart boundary checks")
write("src/host/orc_loader.cpp", orc)

# A normal C program with TLS must restart rather than receive a fresh hidden
# TLS allocation in a live candidate generation.
write(
    "tests/fixtures/tls_restart.c",
    r'''#include <stdio.h>
#include <time.h>

static _Thread_local int tls_counter = 0;

static int step(void)
{
    tls_counter += 1;
    return tls_counter;
}

int main(void)
{
    struct timespec delay = {0, 20000000L};
    for (int iteration = 0; iteration < 700; ++iteration) {
        (void)printf("tls=%d\n", step());
        (void)fflush(stdout);
        (void)nanosleep(&delay, NULL);
    }
    return 0;
}
''',
)

write(
    "tests/check_tls_restart.py",
    r'''#!/usr/bin/env python3
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
        raise SystemExit("usage: check_tls_restart.py CVITE FIXTURE")
    cvite = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="cvite-tls-") as directory:
        source = Path(directory) / "app.c"
        shutil.copy2(fixture, source)
        environment = os.environ.copy()
        environment.pop("CVITE_DISABLE_AUTO_RESTART", None)
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
        lines: queue.Queue[str] = queue.Queue()
        transcript: list[str] = []

        def reader() -> None:
            for line in process.stdout:
                transcript.append(line)
                lines.put(line)

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()

        def wait_for(fragment: str, timeout: float = 30.0) -> str:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    line = lines.get(timeout=0.5)
                except queue.Empty:
                    if process.poll() is not None:
                        break
                    continue
                if fragment in line:
                    return line
            raise AssertionError(
                f"did not observe {fragment!r}\n{''.join(transcript[-200:])}"
            )

        try:
            wait_for("tls=1")
            text = source.read_text(encoding="utf-8")
            source.write_text(
                text.replace("tls_counter += 1;", "tls_counter += 2;"),
                encoding="utf-8",
            )
            wait_for("thread-local storage; process restart required")
            wait_for("restarting")
            # Re-exec keeps the CVite process identity, but the new baseline TLS
            # begins from its initializer and immediately reflects the edit.
            wait_for("tls=2")
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
''',
)

cmake = read("CMakeLists.txt")
if "NAME run-tls-restart" not in cmake:
    cmake += r'''

if(CVITE_BUILD_LLVM_PASS AND CVITE_BUILD_ORC_LOADER AND BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME run-tls-restart
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_tls_restart.py
            $<TARGET_FILE:cvite>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/tls_restart.c
    )
    set_tests_properties(run-tls-restart PROPERTIES TIMEOUT 70)
endif()
'''
write("CMakeLists.txt", cmake)

doc = read("docs/candidate-objects.md")
if "Thread-local and constructor boundaries" not in doc:
    doc += r'''

## Thread-local and constructor boundaries

Candidate schema 4 classifies native state that cannot safely be introduced into
an already-running process. `_Thread_local` definitions and native
constructors/destructors request the controlled full-restart path. Address-taken
functions and inline assembly do not automatically reject a compatible patch,
but they pin the owning JIT generation because code addresses may have escaped
LLVM's observable graph.
'''
write("docs/candidate-objects.md", doc)

print("native-state safety boundaries applied")
