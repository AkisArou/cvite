#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def run(command: list[str], *, capture: bool = False) -> str:
    result = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )
    if result.returncode != 0:
        output = result.stdout or ""
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n{output}"
        )
    return result.stdout or ""


def require(text: str, needle: str) -> None:
    if needle not in text:
        raise RuntimeError(f"transformed IR is missing {needle!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", required=True)
    parser.add_argument("--opt", required=True)
    parser.add_argument("--allocation-index", required=True)
    parser.add_argument("--annotator", required=True)
    parser.add_argument("--plugin", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()

    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    raw_ir = work / "managed.raw.ll"
    allocation_index = work / "managed.cvaidx"
    annotated_ir = work / "managed.annotated.ll"
    transformed_ir = work / "managed.transformed.ll"

    run(
        [
            args.clang,
            "-std=c11",
            "-O0",
            "-g",
            "-fno-discard-value-names",
            "-S",
            "-emit-llvm",
            args.source,
            "-o",
            str(raw_ir),
        ]
    )
    run(
        [
            args.allocation_index,
            "index",
            "--source",
            args.source,
            "--output",
            str(allocation_index),
            "--",
            "-std=c11",
        ]
    )
    annotate_output = run(
        [
            args.annotator,
            "--input",
            str(raw_ir),
            "--index",
            str(allocation_index),
            "--output",
            str(annotated_ir),
        ],
        capture=True,
    )
    if "annotated 2 of 2 typed allocation sites" not in annotate_output:
        raise RuntimeError(f"unexpected annotator result: {annotate_output}")

    run(
        [
            args.opt,
            f"-load-pass-plugin={args.plugin}",
            "-passes=cvite-managed-allocation,verify",
            "-S",
            str(annotated_ir),
            "-o",
            str(transformed_ir),
        ]
    )
    transformed = transformed_ir.read_text(encoding="utf-8")
    require(transformed, "@__cvite_host_managed_allocate")
    require(transformed, "@__cvite_host_managed_callocate")
    require(transformed, "@__cvite_host_managed_track_pointer")
    require(transformed, "@__cvite_host_managed_escape_pointer")
    require(transformed, "@__cvite_host_managed_free")
    require(transformed, "call ptr @malloc")
    require(transformed, "call void @foreign_retain")
    require(transformed, '!"cvite.managed-allocation.schema"')

    print("managed allocation annotation and rewrite checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
