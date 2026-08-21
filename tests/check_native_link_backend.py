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
    joined = "".join(output[-180:])
    raise AssertionError(f"{message}\n\n--- cvite output ---\n{joined}")


def atomic_write(path: Path, content: str) -> None:
    replacement = path.with_suffix(path.suffix + ".new")
    replacement.write_text(content, encoding="utf-8")
    os.replace(replacement, path)


def write_compile_database(build: Path, source: Path) -> None:
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
                str(build / "CMakeFiles" / "app.dir" / "main.c.o"),
            ],
            "file": str(source),
        }
    ]
    (build / "compile_commands.json").write_text(
        json.dumps(compile_commands), encoding="utf-8"
    )


def configure_file_api(
    root: Path,
    build: Path,
    source: Path,
    library: Path,
    cmake: Path,
    ninja: Path,
) -> None:
    query = build / ".cmake" / "api" / "v1" / "query" / "client-cvite"
    query.mkdir(parents=True)
    (query / "query.json").write_text(
        json.dumps({"requests": [{"kind": "codemodel", "version": 2}]}),
        encoding="utf-8",
    )
    cmake_lists = f'''cmake_minimum_required(VERSION 3.20)
project(cvite_backend_fixture C)
add_library(cvite_native SHARED IMPORTED GLOBAL)
set_target_properties(cvite_native PROPERTIES
    IMPORTED_LOCATION "{library.as_posix()}"
)
add_executable(app main.c)
target_link_libraries(app PRIVATE cvite_native)
'''
    (root / "CMakeLists.txt").write_text(cmake_lists, encoding="utf-8")
    subprocess.run(
        [
            str(cmake),
            "-S",
            str(root),
            "-B",
            str(build),
            "-G",
            "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def configure_ninja(build: Path, source: Path, library: Path) -> None:
    object_directory = build / "CMakeFiles" / "app.dir"
    object_directory.mkdir(parents=True)
    write_compile_database(build, source)
    ninja_text = f'''ninja_required_version = 1.3

rule cc
  command = clang-18 -std=c11 -c $in -o $out

rule link
  command = clang-18 $in -L{library.parent} -l:{library.name} -Wl,-rpath,{library.parent} -o $out

build CMakeFiles/app.dir/main.c.o: cc {source}
build app: link CMakeFiles/app.dir/main.c.o
default app
'''
    (build / "build.ninja").write_text(ninja_text, encoding="utf-8")


def run_scenario(
    mode: str,
    cvite: Path,
    fixture: Path,
    library: Path,
    cmake: Path,
    ninja: Path,
) -> None:
    output: list[str] = []
    expected_backend = {
        "cmake-file-api": "cmake-file-api",
        "ninja": "ninja-commands",
    }[mode]

    with tempfile.TemporaryDirectory(prefix=f"cvite-{mode}-") as directory:
        root = Path(directory)
        source = root / "main.c"
        build = root / "build"
        build.mkdir()
        shutil.copyfile(fixture, source)

        original = source.read_text(encoding="utf-8")
        edited = original.replace(
            "total += cvite_auto_native_bonus();",
            "total += cvite_auto_native_bonus() * 10;",
            1,
        )
        if edited == original:
            fail("fixture edit marker was not found", output)

        if mode == "cmake-file-api":
            configure_file_api(root, build, source, library, cmake, ninja)
        else:
            configure_ninja(build, source, library)

        environment = os.environ.copy()
        environment.pop("CVITE_PRELOAD", None)
        environment["CVITE_VERBOSE"] = "1"
        environment["CVITE_NINJA"] = str(ninja)
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
        backend_seen = False
        input_seen = False
        refreshed = False
        preserved_jump = False
        deadline = time.monotonic() + 55.0

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

                if f"native link backend: {expected_backend}" in line:
                    backend_seen = True
                if "native link input:" in line and library.name in line:
                    input_seen = True
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
                    and input_seen
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
            fail(f"did not observe backend {expected_backend}", output)
        if not input_seen:
            fail("discovered native input was not reported", output)
        if not refreshed or not preserved_jump:
            fail("did not observe a state-preserving refresh", output)


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: check_native_link_backend.py "
            "<cmake-file-api|ninja> <cvite> <app> <library> <cmake> <ninja>"
        )
    mode = sys.argv[1]
    if mode not in {"cmake-file-api", "ninja"}:
        raise SystemExit(f"unsupported backend: {mode}")
    run_scenario(
        mode,
        Path(sys.argv[2]).resolve(),
        Path(sys.argv[3]).resolve(),
        Path(sys.argv[4]).resolve(),
        Path(sys.argv[5]).resolve(),
        Path(sys.argv[6]).resolve(),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
