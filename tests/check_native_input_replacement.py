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
    joined = "".join(output[-220:])
    raise AssertionError(f"{message}\n\n--- cvite output ---\n{joined}")


def configure_project(
    root: Path,
    build: Path,
    live_library: Path,
    cmake: Path,
    ninja: Path,
) -> None:
    query = build / ".cmake" / "api" / "v1" / "query" / "client-cvite"
    query.mkdir(parents=True)
    (query / "query.json").write_text(
        json.dumps({"requests": [{"kind": "codemodel", "version": 2}]}),
        encoding="utf-8",
    )
    (root / "CMakeLists.txt").write_text(
        f'''cmake_minimum_required(VERSION 3.20)
project(cvite_replacement_fixture C)
add_library(cvite_native SHARED IMPORTED GLOBAL)
set_target_properties(cvite_native PROPERTIES
    IMPORTED_LOCATION "{live_library.as_posix()}"
)
add_executable(app main.c)
target_link_libraries(app PRIVATE cvite_native)
''',
        encoding="utf-8",
    )
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


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: check_native_input_replacement.py "
            "<cvite> <app> <library-v1> <library-v2> <cmake> <ninja>"
        )

    cvite = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()
    library_v1 = Path(sys.argv[3]).resolve()
    library_v2 = Path(sys.argv[4]).resolve()
    cmake = Path(sys.argv[5]).resolve()
    ninja = Path(sys.argv[6]).resolve()
    output: list[str] = []

    with tempfile.TemporaryDirectory(prefix="cvite-native-replacement-") as directory:
        root = Path(directory)
        build = root / "build"
        build.mkdir()
        source = root / "main.c"
        shutil.copyfile(fixture, source)
        native_directory = root / "native"
        native_directory.mkdir()
        live_library = native_directory / "libcvite_live_native.so"
        shutil.copy2(library_v1, live_library)
        configure_project(root, build, live_library, cmake, ninja)

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

        values_before_restart: list[int] = []
        values_after_restart: list[int] = []
        backend_seen = False
        input_seen = False
        replaced = False
        restart_seen = False
        new_library_seen = False
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

                if "native link backend: cmake-file-api" in line:
                    backend_seen = True
                if "native link input:" in line and live_library.name in line:
                    input_seen = True
                if "a native link input changed; restarting the process" in line:
                    restart_seen = True

                match = VALUE.match(line.rstrip("\n"))
                if match:
                    current = int(match.group(1))
                    if restart_seen:
                        values_after_restart.append(current)
                        if current == 7 or (
                            len(values_after_restart) >= 2
                            and values_after_restart[-1] - values_after_restart[-2] == 7
                        ):
                            new_library_seen = True
                    else:
                        values_before_restart.append(current)

                    if (
                        backend_seen
                        and input_seen
                        and len(values_before_restart) >= 5
                        and not replaced
                    ):
                        replacement = live_library.with_suffix(".replacement.so")
                        shutil.copy2(library_v2, replacement)
                        os.replace(replacement, live_library)
                        replaced = True

                if (
                    replaced
                    and restart_seen
                    and new_library_seen
                    and len(values_after_restart) >= 4
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
        if not backend_seen or not input_seen:
            fail("CMake File API native input was not discovered", output)
        if not replaced:
            fail("test never replaced the library", output)
        if not restart_seen:
            fail("same-path native library replacement did not restart CVite", output)
        if not new_library_seen:
            fail("restarted process did not execute the replacement library", output)
        if not values_before_restart or not values_after_restart:
            fail("missing observations around the controlled restart", output)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
