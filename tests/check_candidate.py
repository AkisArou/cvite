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


def canonical_numeric_id(digest: str) -> str:
    raw = bytes.fromhex(digest)
    low = int.from_bytes(raw[:8], "little")
    high = int.from_bytes(raw[8:], "little")
    return f"{high:016x}{low:016x}"


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
        baseline_ir_path = directory / "baseline.ll"
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
                "-passes=cvite-lowering,cvite-baseline,cvite-baseline-manifest,verify",
                "-S",
                str(raw_ir),
                "-o",
                str(baseline_ir_path),
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
        baseline_ir = baseline_ir_path.read_text(encoding="utf-8")
        ir = transformed_ir.read_text(encoding="utf-8")

        if "cvite.candidate.schema" not in ir:
            fail("candidate module flag is missing")
        if "@__cvite_candidate_manifest" not in ir:
            fail("candidate manifest is missing")
        if "@__cvite_candidate_records" not in ir:
            fail("candidate function records are missing")
        if "@__cvite_candidate_storage_records" not in ir:
            fail("candidate storage requirements are missing")
        if "@__cvite_host_target_for" not in ir:
            fail("candidate entries do not resolve stable targets by ID")

        implementations = re.findall(rf"@__cvite_patch\.({ID})", ir)
        if len(set(implementations)) != 2:
            fail(f"expected two candidate implementations: {implementations!r}")

        storage_symbols = set(re.findall(rf"@__cvite_storage\.({ID})", ir))
        storage_records = {
            canonical_numeric_id(value)
            for value in re.findall(rf"@__cvite_candidate_storage_name\.({ID})", ir)
        }
        if len(storage_symbols) != 2:
            fail(f"expected global and static-local storage proxies: {storage_symbols!r}")
        if storage_symbols != storage_records:
            fail("storage manifest records do not match generated proxies")
        baseline_storage = {
            canonical_numeric_id(value)
            for value in re.findall(rf"@__cvite_baseline_storage_name\.({ID})", baseline_ir)
        }
        if storage_symbols != baseline_storage:
            fail(
                "baseline and candidate transforms disagree on stable storage IDs: "
                f"{baseline_storage!r} != {storage_symbols!r}"
            )
        for storage_id in storage_symbols:
            if not re.search(
                rf"@__cvite_storage\.{storage_id}\s*=\s*external\s+global",
                ir,
            ):
                fail(f"storage proxy {storage_id} is not an external data symbol")
        if "!cvite.storage" not in ir:
            fail("candidate storage proxies are missing layout metadata")

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
        if not any("@__cvite_storage." in body for body in implementation_bodies):
            fail("candidate implementations do not use persistent baseline storage")

        record_ids = re.findall(rf"@__cvite_candidate_name\.({ID})", ir)
        if set(record_ids) != set(implementations):
            fail("manifest records do not match candidate implementations")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
