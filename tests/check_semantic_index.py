#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


def run(*arguments: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        check=check,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_semantic_index.py <cvite-semantic-index>")
    tool = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="cvite-semantic-") as temporary:
        root = pathlib.Path(temporary)
        old_source = root / "old.c"
        appended_source = root / "appended.c"
        incompatible_source = root / "incompatible.c"
        old_index = root / "old.cvsidx"
        appended_index = root / "appended.cvsidx"
        incompatible_index = root / "incompatible.cvsidx"

        old_source.write_text(
            "typedef struct Player { int score; float speed; } Player;\n",
            encoding="utf-8",
        )
        appended_source.write_text(
            "typedef struct Player { int score; float speed; int health; } Player;\n",
            encoding="utf-8",
        )
        incompatible_source.write_text(
            "typedef struct Player { int score; double speed; int health; } Player;\n",
            encoding="utf-8",
        )

        for source, output in (
            (old_source, old_index),
            (appended_source, appended_index),
            (incompatible_source, incompatible_index),
        ):
            run(
                str(tool),
                "index",
                "--source",
                str(source),
                "--output",
                str(output),
                "--",
                "-std=c11",
            )

        appended = run(
            str(tool),
            "diff",
            "--old",
            str(old_index),
            "--new",
            str(appended_index),
        ).stdout
        for marker in (
            "struct Player",
            "+ field health",
            "append-only layout candidate",
            "size:",
        ):
            if marker not in appended:
                raise AssertionError(
                    f"append-only diff missing {marker!r}:\n{appended}"
                )

        incompatible = run(
            str(tool),
            "diff",
            "--old",
            str(old_index),
            "--new",
            str(incompatible_index),
        ).stdout
        for marker in (
            "~ field speed",
            "float",
            "double",
            "incompatible native layout",
        ):
            if marker not in incompatible:
                raise AssertionError(
                    f"incompatible diff missing {marker!r}:\n{incompatible}"
                )

        unchanged = run(
            str(tool),
            "diff",
            "--old",
            str(old_index),
            "--new",
            str(old_index),
        ).stdout
        if unchanged != "no semantic record-layout changes\n":
            raise AssertionError(f"unexpected unchanged output: {unchanged!r}")

        failed = run(
            str(tool),
            "diff",
            "--old",
            str(old_index),
            "--new",
            str(appended_index),
            "--fail-on-change",
            check=False,
        )
        if failed.returncode != 4:
            raise AssertionError(
                f"--fail-on-change returned {failed.returncode}, expected 4"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
