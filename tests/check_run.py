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
        diagnostics[-120:]
        + (["\n--- recent application values ---\n"] if values else [])
        + values[-40:]
    )
    raise AssertionError(f"{message}\n\n--- cvite output ---\n{joined}")


def atomic_write(path: Path, content: str) -> None:
    replacement = path.with_suffix(".new.c")
    replacement.write_text(content, encoding="utf-8")
    os.replace(replacement, path)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: check_run.py <cvite> <fixture>")

    cvite = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()
    output: list[str] = []

    with tempfile.TemporaryDirectory(prefix="cvite-run-test-") as directory:
        source = Path(directory) / "main.c"
        shutil.copyfile(fixture, source)
        original = source.read_text(encoding="utf-8")
        broken = original.replace("total += 1;", "total += ;", 1)
        fixed = original.replace("total += 1;", "total += 10;", 1)
        if broken == original or fixed == original:
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
        refreshed = False
        preserved_jump = False
        old_code_after_error = False
        deadline = time.monotonic() + 35.0

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
                    refreshed = True

                match = VALUE.match(line.rstrip("\n"))
                if match:
                    current = int(match.group(1))
                    if values:
                        difference = current - values[-1]
                        if compile_error_seen and not fixed_written and difference == 1:
                            old_code_after_error = True
                        if fixed_written and difference >= 10:
                            preserved_jump = True
                    values.append(current)

                    if len(values) >= 4 and not broken_written:
                        atomic_write(source, broken)
                        broken_written = True

                    if (
                        compile_error_seen
                        and not fixed_written
                        and len(values) >= values_at_error + 2
                    ):
                        atomic_write(source, fixed)
                        fixed_written = True

                if (
                    compile_error_seen
                    and old_code_after_error
                    and refreshed
                    and preserved_jump
                    and len(values) >= 10
                ):
                    break

            if not compile_error_seen:
                fail("did not observe candidate compile-error recovery", output)
            if not old_code_after_error:
                fail("old code did not keep running after the compile error", output)
            if not refreshed or not preserved_jump:
                fail(
                    "did not observe a successful refresh with preserved state",
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
            fail("test did not complete both source edits", output)
        if len(values) < 10:
            fail(f"too few application observations: {values!r}", output)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
