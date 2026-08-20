#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile


ID = r"[0-9a-f]{32}"


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


def main() -> int:
    if len(sys.argv) != 6:
        print(
            "usage: check_candidate.py <clang> <opt> <plugin> <source> <work-dir>",
            file=sys.stderr,
        )
        return 2

    clang = pathlib.Path(sys.argv[1]).resolve()
    opt = pathlib.Path(sys.argv[2]).resolve()
    plugin = pathlib.Path(sys.argv[3]).resolve()
    source = pathlib.Path(sys.argv[4]).resolve()
    work_root = pathlib.Path(sys.argv[5]).resolve()

    with tempfile.TemporaryDirectory(prefix="cvite-candidate-", dir=work_root) as temp:
        directory = pathlib.Path(temp)
        raw_ir = directory / "raw.ll"
        transformed_ir = directory / "transformed.ll"
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
                "-passes=cvite-lowering,cvite-candidate,cvite-candidate,verify",
                "-S",
                str(raw_ir),
                "-o",
                str(transformed_ir),
            ]
        )
        ir = transformed_ir.read_text(encoding="utf-8")

        if "cvite.candidate.schema" not in ir:
            fail("candidate module flag is missing")
        if "@__cvite_candidate_manifest" not in ir:
            fail("candidate manifest is missing")
        if "@__cvite_candidate_records" not in ir:
            fail("candidate function records are missing")
        if "@__cvite_host_target_for" not in ir:
            fail("candidate entries do not resolve stable targets by ID")

        implementations = re.findall(rf"@__cvite_patch\.({ID})", ir)
        if len(set(implementations)) != 2:
            fail(f"expected two candidate implementations: {implementations!r}")

        if not re.search(r"define [^{@]*@app_update\(", ir):
            fail("app_update candidate entry is missing")
        if not re.search(r"define internal [^{@]*@helper\(", ir):
            fail("static helper candidate entry is missing")
        if not re.search(r"define [^{@]*@main\(", ir):
            fail("main disappeared")
        if re.search(rf"@__cvite_patch\.{ID}[^\n]*main", ir):
            fail("main must not be emitted as a candidate implementation")

        app_entry = re.search(
            r"define [^{@]*@app_update\([^)]*\)[^{]*\{(?P<body>.*?)^\}",
            ir,
            re.MULTILINE | re.DOTALL,
        )
        if app_entry is None or "@__cvite_host_target_for" not in app_entry.group("body"):
            fail("app_update does not route through the active stable target")

        implementation_bodies = re.findall(
            rf"define internal [^@]*@__cvite_patch\.{ID}\([^)]*\)[^{{]*\{{(.*?)^\}}",
            ir,
            re.MULTILINE | re.DOTALL,
        )
        if not any("call" in body and "@helper" in body for body in implementation_bodies):
            fail("candidate inter-function calls bypass generated stable entries")

        record_ids = re.findall(rf"@__cvite_candidate_name\.({ID})", ir)
        if set(record_ids) != set(implementations):
            fail("manifest records do not match candidate implementations")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
