#!/usr/bin/env python3

from __future__ import annotations

import os
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time


OLD_SOURCE = r'''
typedef struct PlayerState {
    int score;
} PlayerState;

int printf(const char *, ...);
int fflush(void *);
int usleep(unsigned int);

static PlayerState player;

static int update_player(void)
{
    player.score += 1;
    return player.score;
}

int main(void)
{
    for (;;) {
        printf("score=%d\n", update_player());
        fflush((void *)0);
        usleep(100000U);
    }
}
'''

NEW_SOURCE = r'''
typedef struct PlayerState {
    int score;
    int health;
} PlayerState;

int printf(const char *, ...);
int fflush(void *);
int usleep(unsigned int);

static PlayerState player;

static int update_player(void)
{
    player.score += 1;
    return player.score;
}

int main(void)
{
    for (;;) {
        printf("score=%d\n", update_player());
        fflush((void *)0);
        usleep(100000U);
    }
}
'''


def replace(path: pathlib.Path, text: str) -> None:
    temporary = path.with_suffix(".next")
    temporary.write_text(text, encoding="utf-8")
    os.replace(temporary, path)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_semantic_restart.py <cvite>")
    cvite = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="cvite-layout-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "app.c"
        source.write_text(OLD_SOURCE, encoding="utf-8")
        environment = os.environ.copy()
        environment["CVITE_TRACE_SEMANTICS"] = "1"
        process = subprocess.Popen(
            [str(cvite), "run", str(source)],
            cwd=root,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=1,
        )
        assert process.stdout is not None
        lines: queue.Queue[str] = queue.Queue()
        transcript: list[str] = []

        def read_output() -> None:
            for line in process.stdout:
                lines.put(line)

        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()

        def wait_for(marker: str, timeout: float) -> None:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    line = lines.get(timeout=0.2)
                except queue.Empty:
                    if process.poll() is not None:
                        break
                    continue
                transcript.append(line)
                if marker in "".join(transcript):
                    return
            raise AssertionError(
                f"did not observe {marker!r}; transcript:\n"
                + "".join(transcript)
            )

        try:
            wait_for("score=", 20.0)
            replace(source, NEW_SOURCE)
            wait_for("semantic explanation for rejected native layout", 30.0)
            for marker in (
                "PlayerState",
                "+ field health",
                "append-only layout candidate",
                "restart",
                "score=1",
            ):
                wait_for(marker, 20.0)
        finally:
            process.terminate()
            try:
                process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5.0)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
