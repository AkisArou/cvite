#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile


def fail(message: str) -> None:
    raise AssertionError(message)


def run(command: list[str]) -> None:
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        fail(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )


def manifest_flags(ir: str) -> int:
    match = re.search(
        r"@__cvite_candidate_manifest\s*=.*?\{\s*i64\s+3,\s*i64\s+(-?\d+)",
        ir,
        re.DOTALL,
    )
    if match is None:
        fail("candidate manifest flags are missing")
    value = int(match.group(1))
    return value if value >= 0 else value + (1 << 64)


def transform(
    clang: pathlib.Path,
    opt: pathlib.Path,
    plugin: pathlib.Path,
    source: pathlib.Path,
    directory: pathlib.Path,
) -> str:
    raw_ir = directory / f"{source.stem}.raw.ll"
    transformed_ir = directory / f"{source.stem}.ll"
    run([
        str(clang), "-std=c11", "-O0", "-S", "-emit-llvm",
        str(source), "-o", str(raw_ir),
    ])
    run([
        str(opt), f"-load-pass-plugin={plugin}",
        "-passes=cvite-lowering,cvite-candidate,verify",
        "-S", str(raw_ir), "-o", str(transformed_ir),
    ])
    return transformed_ir.read_text(encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 8:
        print(
            "usage: check_candidate_boundaries.py <clang> <opt> <plugin> "
            "<tls-source> <constructor-source> <destructor-source> <work-dir>",
            file=sys.stderr,
        )
        return 2

    clang = pathlib.Path(sys.argv[1]).resolve()
    opt = pathlib.Path(sys.argv[2]).resolve()
    plugin = pathlib.Path(sys.argv[3]).resolve()
    sources = [pathlib.Path(value).resolve() for value in sys.argv[4:7]]
    work_root = pathlib.Path(sys.argv[7]).resolve()

    expected = [2, 4, 8]
    with tempfile.TemporaryDirectory(prefix="cvite-boundaries-", dir=work_root) as temp:
        directory = pathlib.Path(temp)
        for source, flag in zip(sources, expected, strict=True):
            flags = manifest_flags(transform(clang, opt, plugin, source, directory))
            if (flags & flag) == 0:
                fail(f"{source.name} did not set compatibility flag {flag}: {flags}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
