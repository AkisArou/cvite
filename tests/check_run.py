#!/usr/bin/env python3

from __future__ import annotations

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
        diagnostics[-160:]
        + (["\n--- recent application values ---\n"] if values else [])
        + values[-60:]
    )
    raise AssertionError(f"{message}\n\n--- cvite output ---\n{joined}")


def atomic_write(path: Path, content: str) -> None:
    replacement = path.with_name(f".{path.name}.cvite-new")
    replacement.write_text(content, encoding="utf-8")
    os.replace(replacement, path)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: check_run.py <cvite> <fixture>")

    cvite = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()
    fixture_header = fixture.with_name("run_step.h")
    fixture_next_header = fixture.with_name("run_step_next.h")
    output: list[str] = []

    with tempfile.TemporaryDirectory(prefix="cvite-run-test-") as directory:
        source = Path(directory) / "main.c"
        header = Path(directory) / "run_step.h"
        next_header = Path(directory) / "run_step_next.h"
        shutil.copyfile(fixture, source)
        shutil.copyfile(fixture_header, header)
        shutil.copyfile(fixture_next_header, next_header)

        original_header = header.read_text(encoding="utf-8")
        original_next_header = next_header.read_text(encoding="utf-8")
        broken_header = original_header.replace(
            "#define CVITE_STEP 1",
            "#define CVITE_STEP",
            1,
        )
        fixed_header = original_header.replace(
            "#define CVITE_STEP 1",
            '#include "run_step_next.h"\n#define CVITE_STEP CVITE_NEXT_STEP',
            1,
        )
        updated_next_header = original_next_header.replace(
            "#define CVITE_NEXT_STEP 10",
            "#define CVITE_NEXT_STEP 20",
            1,
        )
        if (
            broken_header == original_header
            or fixed_header == original_header
            or updated_next_header == original_next_header
        ):
            fail("fixture edit marker was not found", output)

        environment = os.environ.copy()
        environment["CVITE_VERBOSE"] = "1"
        process = subprocess.Popen(
            [str(cvite), "run", str(source)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert process.stdout is not None

        values: list[int] = []
        broken_written = False
        compile_error_seen = False
        values_at_error = 0
        fixed_written = False
        refresh_count = 0
        first_refresh_behavior = False
        next_header_written = False
        second_refresh_behavior = False
        old_code_after_error = False
        dynamic_graph_seen = False
        deadline = time.monotonic() + 40.0

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

                if "candidate compilation failed" in line:
                    compile_error_seen = True
                    values_at_error = len(values)

                if "[cvite] refreshed" in line:
                    refresh_count += 1

                if "[cvite] watching 3 files" in line:
                    dynamic_graph_seen = True

                match = VALUE.match(line.rstrip("\n"))
                if match:
                    current = int(match.group(1))
                    difference = current - values[-1] if values else 0

                    if compile_error_seen and not fixed_written and difference == 1:
                        old_code_after_error = True

                    if (
                        fixed_written
                        and refresh_count >= 1
                        and not first_refresh_behavior
                        and difference >= 10
                    ):
                        first_refresh_behavior = True

                    if (
                        first_refresh_behavior
                        and dynamic_graph_seen
                        and not next_header_written
                    ):
                        atomic_write(next_header, updated_next_header)
                        next_header_written = True

                    if (
                        next_header_written
                        and refresh_count >= 2
                        and difference >= 20
                    ):
                        second_refresh_behavior = True

                    values.append(current)

                    if len(values) >= 4 and not broken_written:
                        atomic_write(header, broken_header)
                        broken_written = True

                    if (
                        compile_error_seen
                        and not fixed_written
                        and len(values) >= values_at_error + 2
                    ):
                        atomic_write(header, fixed_header)
                        fixed_written = True

                if (
                    compile_error_seen
                    and old_code_after_error
                    and refresh_count >= 2
                    and first_refresh_behavior
                    and dynamic_graph_seen
                    and second_refresh_behavior
                    and len(values) >= 12
                ):
                    break

            if not compile_error_seen:
                fail("did not observe header compile-error recovery", output)
            if not old_code_after_error:
                fail("old code did not keep running after the header error", output)
            if refresh_count < 1 or not first_refresh_behavior:
                fail(
                    "did not observe a header-triggered refresh with preserved state",
                    output,
                )
            if not dynamic_graph_seen:
                fail("candidate dependency graph was not installed", output)
            if not next_header_written:
                fail("test never edited the newly introduced header", output)
            if refresh_count < 2 or not second_refresh_behavior:
                fail(
                    "newly introduced header did not trigger a second refresh",
                    output,
                )
        finally:
            try:
                return_code = process.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    return_code = process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    return_code = process.wait(timeout=2.0)

        if return_code != 0:
            fail(f"cvite exited with status {return_code}", output)
        if not broken_written or not fixed_written:
            fail("test did not complete both primary-header edits", output)
        if len(values) < 12:
            fail(f"too few application observations: {values!r}", output)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
