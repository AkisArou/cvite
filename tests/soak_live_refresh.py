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


SOURCE_TEMPLATE = r'''
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static _Atomic unsigned long total;
static _Atomic int stop_workers;

static int hot_delta(void)
{{
    return {delta};
}}

static void *worker(void *unused)
{{
    (void)unused;
    while (atomic_load_explicit(&stop_workers, memory_order_relaxed) == 0) {{
        atomic_fetch_add_explicit(
            &total,
            (unsigned long)hot_delta(),
            memory_order_relaxed);
    }}
    return NULL;
}}

static void sleep_milliseconds(long milliseconds)
{{
    const struct timespec duration = {{
        milliseconds / 1000L,
        (milliseconds % 1000L) * 1000000L,
    }};
    (void)nanosleep(&duration, NULL);
}}

int main(void)
{{
    pthread_t threads[4];
    unsigned index = 0U;
    for (index = 0U; index < 4U; ++index) {{
        if (pthread_create(&threads[index], NULL, worker, NULL) != 0) {{
            return 2;
        }}
    }}
    for (;;) {{
        printf(
            "delta=%d total=%lu\n",
            hot_delta(),
            atomic_load_explicit(&total, memory_order_relaxed));
        fflush(stdout);
        sleep_milliseconds(80L);
    }}
}}
'''

BROKEN_SOURCE = SOURCE_TEMPLATE.format(delta="1 +")


def source(delta: int) -> str:
    return SOURCE_TEMPLATE.format(delta=delta)


def replace(path: pathlib.Path, text: str) -> None:
    temporary = path.with_suffix(".next")
    temporary.write_text(text, encoding="utf-8")
    os.replace(temporary, path)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: soak_live_refresh.py <cvite>")
    cvite = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="cvite-soak-") as temporary:
        root = pathlib.Path(temporary)
        app = root / "app.c"
        app.write_text(source(1), encoding="utf-8")
        environment = os.environ.copy()
        environment["CVITE_TRACE_LIFECYCLE"] = "1"
        process = subprocess.Popen(
            [str(cvite), "run", str(app)],
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

        def reader() -> None:
            for line in process.stdout:
                lines.put(line)

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()

        def wait_for(marker: str, timeout: float = 15.0) -> None:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                try:
                    line = lines.get(timeout=0.2)
                except queue.Empty:
                    continue
                transcript.append(line)
                if marker in line:
                    return
            raise AssertionError(
                f"did not observe {marker!r}; return={process.poll()};\n"
                + "".join(transcript[-120:])
            )

        try:
            wait_for("delta=1 ")
            for delta in range(2, 14):
                if delta in (5, 9, 13):
                    replace(app, BROKEN_SOURCE)
                    # The last-known-good implementation must remain alive.
                    wait_for(f"delta={delta - 1} ", 8.0)
                    if process.poll() is not None:
                        raise AssertionError("process died after an edit-time compiler error")
                replace(app, source(delta))
                wait_for(f"delta={delta} ", 18.0)
                if process.poll() is not None:
                    raise AssertionError("process restarted or crashed during compatible refresh")
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
