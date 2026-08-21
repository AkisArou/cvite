from __future__ import annotations

from pathlib import Path

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


if "CVITE_CANDIDATE_FLAG_RESTART_TLS" not in read("include/cvite/candidate.h"):
    raise SystemExit("native-state boundary milestone has not landed")

# Give restart and pinning decisions actionable native-state explanations.
orc = read("src/host/orc_loader.cpp")
if "generation is pinned because function addresses may have escaped" not in orc:
    marker = '''    candidate.pinned =
        (manifest->compatibility_flags &
         CVITE_CANDIDATE_FLAG_SAFE_TO_RECLAIM) == 0U;
'''
    diagnostics = r'''    if (traceLifecycleEnabled() && candidate.pinned) {
        if ((manifest->compatibility_flags &
             CVITE_CANDIDATE_FLAG_ADDRESS_ESCAPES) != 0U) {
            (void)std::fprintf(
                stderr,
                "[cvite] generation is pinned because function addresses "
                "may have escaped\n");
        }
        if ((manifest->compatibility_flags &
             CVITE_CANDIDATE_FLAG_INLINE_ASSEMBLY) != 0U) {
            (void)std::fprintf(
                stderr,
                "[cvite] generation is pinned because inline assembly "
                "prevents complete code-reference analysis\n");
        }
    }
'''
    orc = replace_once(orc, marker, marker + diagnostics, "ORC pin diagnostics")
write("src/host/orc_loader.cpp", orc)

host = read("src/host/host_runtime.c")
old_message = '''        (void)snprintf(
            error->message,
            sizeof(error->message),
            "storage '%s' changed layout",
            existing->debug_name);'''
new_message = '''        (void)snprintf(
            error->message,
            sizeof(error->message),
            "storage '%s' changed layout: size %zu -> %zu, "
            "alignment %zu -> %zu",
            existing->debug_name,
            existing->size,
            definition->size,
            existing->alignment,
            definition->alignment);'''
host = replace_once(host, old_message, new_message, "storage layout diagnostic")
write("src/host/host_runtime.c", host)

write(
    "tests/fixtures/constructor_restart.c",
    r'''#include <stdio.h>
#include <time.h>

static int initialized = 0;

__attribute__((constructor))
static void initialize_program(void)
{
    initialized = 40;
}

static int step(void)
{
    return initialized + 1;
}

int main(void)
{
    struct timespec delay = {0, 20000000L};
    for (int iteration = 0; iteration < 700; ++iteration) {
        (void)printf("constructor=%d\n", step());
        (void)fflush(stdout);
        (void)nanosleep(&delay, NULL);
    }
    return 0;
}
''',
)

write(
    "tests/check_constructor_restart.py",
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
        raise SystemExit("usage: check_constructor_restart.py CVITE FIXTURE")
    cvite = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="cvite-constructor-") as directory:
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
            wait_for("constructor=41")
            text = source.read_text(encoding="utf-8")
            source.write_text(
                text.replace("return initialized + 1;", "return initialized + 2;"),
                encoding="utf-8",
            )
            wait_for("native constructors or destructors; process restart required")
            wait_for("restarting")
            wait_for("constructor=42")
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
if "NAME run-constructor-restart" not in cmake:
    cmake += r'''

if(CVITE_BUILD_LLVM_PASS AND CVITE_BUILD_ORC_LOADER AND BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME run-constructor-restart
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_constructor_restart.py
            $<TARGET_FILE:cvite>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/constructor_restart.c
    )
    set_tests_properties(run-constructor-restart PROPERTIES TIMEOUT 70)
endif()
'''
write("CMakeLists.txt", cmake)

doc = read("docs/candidate-objects.md")
if "Pinning diagnostics" not in doc:
    doc += r'''

### Pinning diagnostics

With `CVITE_TRACE_LIFECYCLE=1`, CVite explains why a compatible generation is
retained after its published implementations have been superseded. Address-
taken functions and inline assembly pin code conservatively; TLS and native
constructors instead take the controlled restart path.

Persistent-storage rejection messages include old/new size and alignment so the
fallback is actionable rather than an opaque fingerprint mismatch.
'''
write("docs/candidate-objects.md", doc)

print("refresh diagnostics and constructor boundary test applied")
