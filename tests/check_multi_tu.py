#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

VALUE = re.compile(r"^value=(\d+)$")


def fail(message: str, output: list[str]) -> None:
    diagnostics = [line for line in output if VALUE.match(line.rstrip("\n")) is None]
    values = [line for line in output if VALUE.match(line.rstrip("\n")) is not None]
    joined = "".join(
        diagnostics[-180:]
        + (["\n--- recent application values ---\n"] if values else [])
        + values[-80:]
    )
    raise AssertionError(f"{message}\n\n--- cvite output ---\n{joined}")


def atomic_write(path: Path, content: str) -> None:
    replacement = path.with_name(f".{path.name}.cvite-new")
    replacement.write_text(content, encoding="utf-8")
    os.replace(replacement, path)


def write_compile_database(project: Path, sources: list[Path]) -> None:
    commands = []
    for source in sources:
        commands.append(
            {
                "directory": str(project),
                "file": str(source),
                "arguments": [
                    "clang-18",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-I",
                    str(project),
                    "-c",
                    str(source),
                    "-o",
                    str(project / f"{source.stem}.o"),
                ],
            }
        )
    atomic_write(
        project / "compile_commands.json",
        json.dumps(commands, indent=2) + "\n",
    )


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: check_multi_tu.py <cvite> <main-fixture> "
            "<worker-fixture> <header-fixture>"
        )

    cvite = Path(sys.argv[1]).resolve()
    main_fixture = Path(sys.argv[2]).resolve()
    worker_fixture = Path(sys.argv[3]).resolve()
    header_fixture = Path(sys.argv[4]).resolve()
    output: list[str] = []

    with tempfile.TemporaryDirectory(prefix="cvite-multi-tu-test-") as directory:
        project = Path(directory)
        main_source = project / "main.c"
        worker_source = project / "worker.c"
        header = project / "run_multi_shared.h"
        shutil.copyfile(main_fixture, main_source)
        shutil.copyfile(worker_fixture, worker_source)
        shutil.copyfile(header_fixture, header)
        write_compile_database(project, [main_source, worker_source])

        original_worker = worker_source.read_text(encoding="utf-8")
        updated_worker = original_worker.replace(
            "total += CVITE_MULTI_STEP + same_name();",
            "total += CVITE_MULTI_STEP * 10 + same_name();",
            1,
        )
        original_header = header.read_text(encoding="utf-8")
        broken_header = original_header.replace(
            "#define CVITE_MULTI_STEP 1",
            "#define CVITE_MULTI_STEP (",
            1,
        )
        fixed_header = original_header.replace(
            "#define CVITE_MULTI_STEP 1",
            "#define CVITE_MULTI_STEP 2",
            1,
        )
        if (
            updated_worker == original_worker
            or broken_header == original_header
            or fixed_header == original_header
        ):
            fail("fixture edit marker was not found", output)

        environment = os.environ.copy()
        environment["CVITE_VERBOSE"] = "1"
        process = subprocess.Popen(
            [str(cvite), "run", str(project)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert process.stdout is not None

        values: list[int] = []
        worker_written = False
        one_of_two_seen = False
        worker_refresh_seen = False
        worker_behavior_seen = False
        broken_written = False
        compile_error_seen = False
        values_at_error = 0
        old_code_after_error = False
        fixed_written = False
        two_of_two_seen = False
        header_refresh_seen = False
        header_behavior_seen = False
        refresh_count = 0
        deadline = time.monotonic() + 65.0

        try:
            while time.monotonic() < deadline:
                ready, _, _ = select.select([process.stdout], [], [], 0.25)
                if not ready:
                    if process.poll() is not None:
                        break
                    continue

                line = process.stdout.readline()
                if line == "":
                    if process.poll() is not None:
                        break
                    continue
                output.append(line)

                if "recompiled 1/2 translation units" in line:
                    one_of_two_seen = True
                if "recompiled 2/2 translation units" in line:
                    two_of_two_seen = True
                if "candidate compilation failed" in line:
                    compile_error_seen = True
                    values_at_error = len(values)
                if "[cvite] refreshed" in line:
                    refresh_count += 1
                    if one_of_two_seen and not worker_refresh_seen:
                        worker_refresh_seen = True
                    elif two_of_two_seen:
                        header_refresh_seen = True

                match = VALUE.match(line.rstrip("\n"))
                if match:
                    current = int(match.group(1))
                    difference = current - values[-1] if values else 0
                    values.append(current)

                    if len(values) >= 5 and not worker_written:
                        atomic_write(worker_source, updated_worker)
                        worker_written = True

                    if worker_refresh_seen and difference >= 10:
                        worker_behavior_seen = True

                    if worker_behavior_seen and not broken_written:
                        atomic_write(header, broken_header)
                        broken_written = True

                    if compile_error_seen and not fixed_written and difference == 10:
                        old_code_after_error = True

                    if (
                        compile_error_seen
                        and old_code_after_error
                        and not fixed_written
                        and len(values) >= values_at_error + 2
                    ):
                        atomic_write(header, fixed_header)
                        fixed_written = True

                    if header_refresh_seen and difference >= 20:
                        header_behavior_seen = True

                if (
                    worker_written
                    and one_of_two_seen
                    and worker_refresh_seen
                    and worker_behavior_seen
                    and compile_error_seen
                    and old_code_after_error
                    and fixed_written
                    and two_of_two_seen
                    and header_refresh_seen
                    and header_behavior_seen
                    and refresh_count >= 2
                    and len(values) >= 18
                ):
                    break
        finally:
            try:
                return_code = process.wait(timeout=30.0)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    return_code = process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    return_code = process.wait(timeout=2.0)

        if return_code != 0:
            fail(f"cvite exited with status {return_code}", output)
        if not one_of_two_seen or not worker_refresh_seen or not worker_behavior_seen:
            fail("worker-only edit did not use the one-TU cache path", output)
        if not compile_error_seen or not old_code_after_error:
            fail("cross-TU compile failure did not preserve old code", output)
        if not two_of_two_seen or not header_refresh_seen or not header_behavior_seen:
            fail("shared-header edit did not invalidate both translation units", output)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
