#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import queue
import shutil
import subprocess
import threading
import time
from pathlib import Path


def reader_thread(process: subprocess.Popen[str], lines: queue.Queue[str]) -> None:
    assert process.stdout is not None
    for line in process.stdout:
        lines.put(line.rstrip("\n"))


def drain_values(lines: queue.Queue[str], transcript: list[str]) -> list[int]:
    values: list[int] = []
    while True:
        try:
            line = lines.get_nowait()
        except queue.Empty:
            break
        transcript.append(line)
        if line.startswith("VALUE="):
            try:
                values.append(int(line.split("=", 1)[1]))
            except ValueError:
                pass
    return values


def wait_for(
    predicate,
    process: subprocess.Popen[str],
    lines: queue.Queue[str],
    transcript: list[str],
    values: list[int],
    timeout: float,
) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        values.extend(drain_values(lines, transcript))
        if predicate(values):
            return
        if process.poll() is not None:
            values.extend(drain_values(lines, transcript))
            raise RuntimeError(
                "cvite process exited before condition was met\n"
                + "\n".join(transcript[-100:])
            )
        time.sleep(0.05)
    values.extend(drain_values(lines, transcript))
    raise RuntimeError(
        "timed out waiting for managed refresh condition\n"
        + "\n".join(transcript[-100:])
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cvite", required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()

    work = Path(args.work)
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    source = work / "main.c"
    shutil.copy2(args.source, source)
    (work / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.20)
project(cvite_managed_live C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_executable(cvite_managed_live main.c)
""",
        encoding="utf-8",
    )
    subprocess.run(
        [
            args.cmake,
            "-S",
            str(work),
            "-B",
            str(work / "build"),
            "-G",
            "Ninja",
            "-DCMAKE_C_COMPILER=clang-18",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
    )

    environment = dict(os.environ)
    environment["CVITE_DISABLE_AUTO_RESTART"] = "1"
    process = subprocess.Popen(
        [args.cvite, "run", str(work)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=environment,
    )
    lines: queue.Queue[str] = queue.Queue()
    reader = threading.Thread(
        target=reader_thread,
        args=(process, lines),
        daemon=True,
    )
    reader.start()
    transcript: list[str] = []
    values: list[int] = []

    try:
        wait_for(
            lambda observed: len(observed) >= 8 and observed[-1] >= 8,
            process,
            lines,
            transcript,
            values,
            20.0,
        )
        original = source.read_text(encoding="utf-8")
        if "player->score += 1;" not in original:
            raise RuntimeError("managed fixture edit marker was not found")
        source.write_text(
            original.replace(
                "player->score += 1;",
                "player->score += 10;",
                1,
            ),
            encoding="utf-8",
        )

        def refreshed(observed: list[int]) -> bool:
            return any(
                current - previous >= 10
                for previous, current in zip(observed, observed[1:])
            )

        wait_for(
            refreshed,
            process,
            lines,
            transcript,
            values,
            25.0,
        )
        if values != sorted(values):
            raise RuntimeError(
                "managed state reset or regressed across refresh: "
                + repr(values)
            )
        if any(value <= 0 for value in values):
            raise RuntimeError("managed application reported allocation failure")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5.0)
        reader.join(timeout=2.0)
        values.extend(drain_values(lines, transcript))

    print(
        "live managed allocation refresh passed; "
        f"observed {len(values)} values"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
