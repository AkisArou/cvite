from pathlib import Path

fixture = Path("tests/fixtures/run_app.c")
text = fixture.read_text(encoding="utf-8")
old = "for (iteration = 0; iteration < 350; ++iteration)"
new = "for (iteration = 0; iteration < 600; ++iteration)"
if old not in text:
    raise SystemExit("run_app iteration marker was not found")
fixture.write_text(text.replace(old, new, 1), encoding="utf-8")

test = Path("tests/check_run.py")
text = test.read_text(encoding="utf-8")
old = "deadline = time.monotonic() + 40.0"
new = "deadline = time.monotonic() + 50.0"
if old not in text:
    raise SystemExit("check_run deadline marker was not found")
test.write_text(text.replace(old, new, 1), encoding="utf-8")
