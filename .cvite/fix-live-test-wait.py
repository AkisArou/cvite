from pathlib import Path

fixture = Path("tests/fixtures/run_app.c")
text = fixture.read_text(encoding="utf-8")
if "iteration < 350" in text:
    text = text.replace("iteration < 350", "iteration < 600", 1)
elif "iteration < 600" not in text:
    raise SystemExit("unexpected run_app iteration count")
fixture.write_text(text, encoding="utf-8")

test = Path("tests/check_run.py")
text = test.read_text(encoding="utf-8")
if "deadline = time.monotonic() + 40.0" in text:
    text = text.replace(
        "deadline = time.monotonic() + 40.0",
        "deadline = time.monotonic() + 50.0",
        1,
    )
elif "deadline = time.monotonic() + 50.0" not in text:
    raise SystemExit("unexpected check_run deadline")

old = "return_code = process.wait(timeout=10.0)"
new = "return_code = process.wait(timeout=25.0)"
if old in text:
    text = text.replace(old, new, 1)
elif new not in text:
    raise SystemExit("check_run natural-exit wait marker was not found")

test.write_text(text, encoding="utf-8")
