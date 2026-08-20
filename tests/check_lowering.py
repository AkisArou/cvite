#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile


FINGERPRINT_PATTERN = re.compile(
    r'!\{!"(?P<name>[^"]+)", !"(?P<fingerprint>[0-9a-f]{32})", '
    r'i32 -?[0-9]+, i64 -?[0-9]+, i64 -?[0-9]+\}'
)


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


def transform(
    clang: pathlib.Path,
    opt: pathlib.Path,
    plugin: pathlib.Path,
    source: pathlib.Path,
    output: pathlib.Path,
) -> str:
    raw_ir = output.with_suffix(".raw.ll")
    run(
        [
            str(clang),
            "-std=c11",
            "-O0",
            "-S",
            "-emit-llvm",
            str(source),
            "-o",
            str(raw_ir),
        ]
    )
    run(
        [
            str(opt),
            f"-load-pass-plugin={plugin}",
            "-passes=cvite-lowering,cvite-lowering",
            "-S",
            str(raw_ir),
            "-o",
            str(output),
        ]
    )
    return output.read_text(encoding="utf-8")


def fingerprints(ir: str) -> dict[str, str]:
    return {
        match.group("name"): match.group("fingerprint")
        for match in FINGERPRINT_PATTERN.finditer(ir)
    }


def main() -> int:
    if len(sys.argv) != 6:
        print(
            "usage: check_lowering.py <clang> <opt> <plugin> <source> <work-dir>",
            file=sys.stderr,
        )
        return 2

    clang = pathlib.Path(sys.argv[1]).resolve()
    opt = pathlib.Path(sys.argv[2]).resolve()
    plugin = pathlib.Path(sys.argv[3]).resolve()
    source = pathlib.Path(sys.argv[4]).resolve()
    work_root = pathlib.Path(sys.argv[5]).resolve()

    with tempfile.TemporaryDirectory(prefix="cvite-lowering-", dir=work_root) as temp:
        directory = pathlib.Path(temp)
        baseline_source = directory / "baseline.c"
        baseline_source.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
        baseline_ir = transform(
            clang, opt, plugin, baseline_source, directory / "baseline.ll"
        )

        if "!cvite.functions" not in baseline_ir:
            fail("lowered function index metadata is missing")
        if "cvite.lowered-abi.schema" not in baseline_ir:
            fail("lowered ABI module flag is missing")
        if "!cvite.abi" not in baseline_ir:
            fail("per-function ABI metadata is missing")
        if "noinline" not in baseline_ir:
            fail("refresh boundaries were not protected from inlining")

        baseline = fingerprints(baseline_ir)
        if set(baseline) != {"add", "helper", "make_pair"}:
            fail(f"unexpected lowered function index: {baseline!r}")

        body_source = directory / "body.c"
        body_source.write_text(
            source.read_text(encoding="utf-8").replace(
                "return value * 2;", "return value * 9;"
            ),
            encoding="utf-8",
        )
        body = fingerprints(
            transform(clang, opt, plugin, body_source, directory / "body.ll")
        )
        if body != baseline:
            fail(f"body-only edit changed lowered ABI fingerprints: {body!r}")

        signature_source = directory / "signature.c"
        signature_source.write_text(
            source.read_text(encoding="utf-8")
            .replace("int add(int left, int right)", "long add(long left, int right)")
            .replace("return helper(left) + right;", "return helper((int)left) + right;"),
            encoding="utf-8",
        )
        signature = fingerprints(
            transform(
                clang,
                opt,
                plugin,
                signature_source,
                directory / "signature.ll",
            )
        )
        if signature["add"] == baseline["add"]:
            fail("signature edit did not change the lowered ABI fingerprint")
        if signature["helper"] != baseline["helper"]:
            fail("unrelated helper ABI changed")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
