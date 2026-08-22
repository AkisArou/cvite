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


def read_output(process: subprocess.Popen[str], output: queue.Queue[str]) -> None:
    assert process.stdout is not None
    for line in process.stdout:
        output.put(line.rstrip("\n"))


def drain(
    output: queue.Queue[str],
    transcript: list[str],
    values: list[int],
) -> None:
    while True:
        try:
            line = output.get_nowait()
        except queue.Empty:
            return
        transcript.append(line)
        if line.startswith("VALUE="):
            try:
                values.append(int(line.split("=", 1)[1]))
            except ValueError:
                pass


def wait_until(
    predicate,
    process: subprocess.Popen[str],
    output: queue.Queue[str],
    transcript: list[str],
    values: list[int],
    timeout: float,
) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        drain(output, transcript, values)
        if predicate(values, transcript):
            return
        if process.poll() is not None:
            drain(output, transcript, values)
            raise RuntimeError(
                "cvite process exited before migration completed\n"
                + "\n".join(transcript[-150:])
            )
        time.sleep(0.05)
    drain(output, transcript, values)
    raise RuntimeError(
        "timed out waiting for live layout migration\n"
        + "\n".join(transcript[-150:])
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
project(cvite_managed_layout C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_executable(cvite_managed_layout main.c)
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
    environment["CVITE_TRACE_MANAGED"] = "1"
    process = subprocess.Popen(
        [args.cvite, "run", str(work)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=environment,
    )
    output: queue.Queue[str] = queue.Queue()
    thread = threading.Thread(
        target=read_output,
        args=(process, output),
        daemon=True,
    )
    thread.start()
    transcript: list[str] = []
    values: list[int] = []

    try:
        wait_until(
            lambda observed, _: len(observed) >= 8 and observed[-1] >= 8000,
            process,
            output,
            transcript,
            values,
            20.0,
        )
        old_source = source.read_text(encoding="utf-8")
        old_record = """typedef struct Player {
    int score;
    float speed;
} Player;"""
        new_record = """typedef struct Player {
    int score;
    float speed;
    int health;
} Player;"""
        if old_record not in old_source:
            raise RuntimeError("layout edit marker was not found")
        candidate = old_source.replace(old_record, new_record, 1)
        candidate = candidate.replace(
            "player->score += 1;\n    return player->score * 1000;",
            "player->score += 10;\n"
            "    player->health += 3;\n"
            "    return player->score * 1000 + player->health;",
            1,
        )
        source.write_text(candidate, encoding="utf-8")

        def migrated(observed: list[int], lines: list[str]) -> bool:
            saw_new_behavior = any(value > 0 and value % 1000 != 0 for value in observed)
            saw_migration = any("migrated" in line and "objects" in line for line in lines)
            return saw_new_behavior and saw_migration

        wait_until(
            migrated,
            process,
            output,
            transcript,
            values,
            30.0,
        )
        if values != sorted(values):
            raise RuntimeError(
                "managed layout refresh reset or regressed state: "
                + repr(values)
            )
        new_values = [value for value in values if value % 1000 != 0]
        if not new_values or new_values[0] % 1000 != 3:
            raise RuntimeError(
                "appended field was not zero-initialized before first update: "
                + repr(new_values)
            )
    finally:
        process.terminate()
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5.0)
        thread.join(timeout=2.0)
        drain(output, transcript, values)

    print(
        "append-only managed heap refresh passed; "
        f"observed {len(values)} values"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
