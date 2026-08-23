#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import subprocess
from pathlib import Path


def run(command: list[str], *, env: dict[str, str] | None = None) -> None:
    result = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wrapper", required=True)
    parser.add_argument("--opt", required=True)
    parser.add_argument("--plugin", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()

    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    annotated = work / "proxy.annotated.ll"
    transformed = work / "proxy.transformed.ll"
    disabled = work / "proxy.disabled.ll"

    common = [
        args.wrapper,
        "-std=c11",
        "-O0",
        "-g",
        "-fno-discard-value-names",
        "-S",
        "-emit-llvm",
        args.source,
    ]
    run(common + ["-o", str(annotated)])
    annotated_text = annotated.read_text(encoding="utf-8")
    if "!cvite.managed.alloc" not in annotated_text:
        raise RuntimeError("cvite-clang did not annotate typed allocation calls")

    run(
        [
            args.opt,
            f"-load-pass-plugin={args.plugin}",
            "-passes=cvite-lowering,verify",
            "-S",
            str(annotated),
            "-o",
            str(transformed),
        ]
    )
    transformed_text = transformed.read_text(encoding="utf-8")
    for symbol in [
        "__cvite_host_managed_allocate",
        "__cvite_host_managed_callocate",
        "__cvite_host_managed_track_pointer",
        "__cvite_host_managed_escape_pointer",
        "__cvite_host_managed_free",
    ]:
        if symbol not in transformed_text:
            raise RuntimeError(f"cvite-lowering omitted {symbol}")

    disabled_environment = dict(os.environ)
    disabled_environment["CVITE_DISABLE_MANAGED_ALLOCATIONS"] = "1"
    run(common + ["-o", str(disabled)], env=disabled_environment)
    if "!cvite.managed.alloc" in disabled.read_text(encoding="utf-8"):
        raise RuntimeError("managed allocation opt-out did not bypass annotation")

    print("invisible cvite-clang allocation pipeline checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
