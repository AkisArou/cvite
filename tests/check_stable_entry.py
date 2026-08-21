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
            "usage: check_stable_entry.py <clang> <opt> <plugin> <source> <work-dir>",
            file=sys.stderr,
        )
        return 2

    clang = pathlib.Path(sys.argv[1]).resolve()
    opt = pathlib.Path(sys.argv[2]).resolve()
    plugin = pathlib.Path(sys.argv[3]).resolve()
    source = pathlib.Path(sys.argv[4]).resolve()
    work_root = pathlib.Path(sys.argv[5]).resolve()

    with tempfile.TemporaryDirectory(prefix="cvite-stable-entry-", dir=work_root) as temp:
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
                "-passes=cvite-lowering,cvite-baseline,cvite-baseline-manifest,cvite-baseline-manifest,verify",
                "-S",
                str(raw_ir),
                "-o",
                str(transformed_ir),
            ]
        )
        ir = transformed_ir.read_text(encoding="utf-8")

        if "cvite.baseline.schema" not in ir:
            fail("baseline module flag is missing")
        if "cvite.baseline.manifest.schema" not in ir:
            fail("baseline manifest module flag is missing")
        if "@llvm.global_ctors" not in ir:
            fail("registration constructor is missing")
        if "@__cvite_host_register_function" not in ir:
            fail("generated constructor does not register functions")
        if "@__cvite_host_register_storage" not in ir:
            fail("generated constructor does not register persistent storage")
        if "@__cvite_host_target_at" not in ir:
            fail("stable entries do not resolve the active target")
        if "@__cvite_host_call_enter" not in ir or "@__cvite_host_call_leave" not in ir:
            fail("stable entries do not bracket JIT execution with call scopes")
        if "!cvite.implementation" not in ir:
            fail("baseline implementations are missing change fingerprints")
        if "@__cvite_baseline_manifest" not in ir:
            fail("JIT baseline manifest is missing")
        if "@__cvite_baseline_records" not in ir:
            fail("JIT baseline function records are missing")
        if "@__cvite_baseline_storage_records" not in ir:
            fail("JIT baseline storage records are missing")
        if not re.search(r"define [^{@]*@__cvite_program_main\(i32", ir):
            fail("canonical program main wrapper is missing")

        implementations = re.findall(rf"@__cvite_impl\.({ID})", ir)
        slots = re.findall(rf"@__cvite_slot\.({ID})", ir)
        baseline_names = re.findall(rf"@__cvite_baseline_name\.({ID})", ir)
        if len(set(implementations)) != 2:
            fail(f"expected two versioned implementations: {implementations!r}")
        if set(implementations) != set(slots):
            fail("every implementation must have one stable dispatch slot")
        if set(implementations) != set(baseline_names):
            fail("baseline manifest records do not match stable implementations")

        storage_names = set(
            re.findall(rf"@__cvite_baseline_storage_name\.({ID})", ir)
        )
        if len(storage_names) != 2:
            fail(f"expected global and static-local storage records: {storage_names!r}")
        if re.search(rf"@__cvite_storage\.{ID}\s*=", ir):
            fail("baseline storage was replaced instead of retaining its native address")
        if "!cvite.storage" not in ir:
            fail("baseline globals are missing persistent-storage metadata")

        if not re.search(r"define [^{@]*@app_update\(", ir):
            fail("app_update stable entry is missing")
        if not re.search(r"define internal [^{@]*@helper\(", ir):
            fail("static helper stable entry is missing")
        if not re.search(r"define [^{@]*@main\(", ir):
            fail("main disappeared")
        if re.search(rf"@__cvite_impl\.{ID}[^\n]*main", ir):
            fail("main must not be wrapped")

        app_body = re.search(
            r"define [^{@]*@app_update\([^)]*\)[^{]*\{(?P<body>.*?)^\}",
            ir,
            re.MULTILINE | re.DOTALL,
        )
        if app_body is None or "@__cvite_host_target_at" not in app_body.group("body"):
            fail("app_update is not a stable dispatch entry")
        app_dispatch = app_body.group("body")
        enter = app_dispatch.find("@__cvite_host_call_enter")
        target = app_dispatch.find("@__cvite_host_target_at")
        leave = app_dispatch.find("@__cvite_host_call_leave")
        if not (0 <= enter < target < leave):
            fail("stable dispatch call scope does not cover target resolution and call")

        implementation_bodies = re.findall(
            rf"define internal [^@]*@__cvite_impl\.{ID}\([^)]*\)[^{{]*\{{(.*?)^\}}",
            ir,
            re.MULTILINE | re.DOTALL,
        )
        if not any("call" in body and "@helper" in body for body in implementation_bodies):
            fail("inter-function calls were not redirected through stable entries")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
