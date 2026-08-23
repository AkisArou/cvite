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
    joined = "".join(output[-160:])
    raise AssertionError(f"{message}\n\n--- cvite output ---\n{joined}")


def atomic_write(path: Path, content: str) -> None:
    replacement = path.with_suffix(path.suffix + ".new")
    replacement.write_text(content, encoding="utf-8")
    os.replace(replacement, path)


def library_link_name(path: Path) -> str:
    name = path.name
    if name.startswith("lib"):
        name = name[3:]
    marker = name.find(".so")
    if marker >= 0:
        name = name[:marker]
    elif name.endswith(".dylib"):
        name = name[:-6]
    return name


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: check_auto_link.py <cvite> <app-fixture> <shared-library>"
        )

    cvite = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()
    library = Path(sys.argv[3]).resolve()
    output: list[str] = []

    with tempfile.TemporaryDirectory(prefix="cvite-auto-link-") as directory:
        root = Path(directory)
        source = root / "main.c"
        build = root / "build"
        object_directory = build / "CMakeFiles" / "app.dir"
        object_directory.mkdir(parents=True)
        shutil.copyfile(fixture, source)

        original = source.read_text(encoding="utf-8")
        edited = original.replace(
            "total += cvite_auto_native_bonus();",
            "total += cvite_auto_native_bonus() * 10;",
            1,
        )
        if edited == original:
            fail("fixture edit marker was not found", output)

        compile_commands = [
            {
                "directory": str(build),
                "arguments": [
                    "clang-18",
                    "-std=c11",
                    "-O2",
                    "-c",
                    str(source),
                    "-o",
                    str(object_directory / "main.c.o"),
                ],
                "file": str(source),
            }
        ]
        (build / "compile_commands.json").write_text(
            json.dumps(compile_commands), encoding="utf-8"
        )

        link_name = library_link_name(library)
        link_command = (
            f"clang-18 CMakeFiles/app.dir/main.c.o "
            f"-L{library.parent} -l{link_name} "
            f"-Wl,-rpath,{library.parent} -o app\n"
        )
        (object_directory / "link.txt").write_text(
            link_command, encoding="utf-8"
        )

        environment = os.environ.copy()
        environment.pop("CVITE_PRELOAD", None)
        environment["CVITE_VERBOSE"] = "1"
        process = subprocess.Popen(
            [str(cvite), "run", str(root)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert process.stdout is not None

        values: list[int] = []
        edited_written = False
        refreshed = False
        preserved_jump = False
        automatic_input_seen = False
        backend_seen = False
        deadline = time.monotonic() + 45.0

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

                if "native link backend: cmake-link-txt" in line:
                    backend_seen = True
                if "native link input:" in line and str(library.parent) in line:
                    automatic_input_seen = True
                if "[cvite] refreshed" in line:
                    refreshed = True

                match = VALUE.match(line.rstrip("\n"))
                if match:
                    current = int(match.group(1))
                    if values and edited_written and current - values[-1] >= 30:
                        preserved_jump = True
                    values.append(current)
                    if len(values) >= 5 and not edited_written:
                        atomic_write(source, edited)
                        edited_written = True

                if (
                    backend_seen
                    and automatic_input_seen
                    and refreshed
                    and preserved_jump
                    and len(values) >= 12
                ):
                    break
        finally:
            try:
                return_code = process.wait(timeout=15.0)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    return_code = process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    return_code = process.wait(timeout=3.0)

        if return_code != 0:
            fail(f"cvite exited with status {return_code}", output)
        if not backend_seen:
            fail("CMake link.txt backend was not selected", output)
        if not automatic_input_seen:
            fail("automatic native link input was not reported", output)
        if not refreshed or not preserved_jump:
            fail("did not observe state-preserving refresh", output)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
