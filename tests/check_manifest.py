#!/usr/bin/env python3
from __future__ import annotations

import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
from typing import Any


def fail(message: str) -> None:
    raise AssertionError(message)


def invoke_manifest(
    tool: pathlib.Path,
    source: pathlib.Path,
    root: pathlib.Path,
    output: pathlib.Path,
) -> subprocess.CompletedProcess[str]:
    command = [
        str(tool),
        "--source",
        str(source),
        "--project-root",
        str(root),
        "--project-id",
        "cvite-manifest-test",
        "--output",
        str(output),
        "--",
        "-std=c11",
        f"-I{source.parent}",
    ]
    return subprocess.run(command, text=True, capture_output=True, check=False)


def run_manifest(
    tool: pathlib.Path,
    source: pathlib.Path,
    root: pathlib.Path,
    output: pathlib.Path,
) -> dict[str, Any]:
    completed = invoke_manifest(tool, source, root, output)
    if completed.returncode != 0:
        fail(
            f"cvite-manifest failed with {completed.returncode}\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    return json.loads(output.read_bytes())


def assert_id(value: object, label: str) -> None:
    if not isinstance(value, str) or len(value) != 32:
        fail(f"{label} is not a 128-bit hexadecimal ID: {value!r}")
    int(value, 16)


def entity_by_name(entities: list[dict[str, Any]], name: str) -> dict[str, Any]:
    for entity in entities:
        if entity.get("name") == name:
            return entity
    fail(f"missing entity {name!r}; found {[entity.get('name') for entity in entities]}")


def entities_by_name(entities: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {str(entity["name"]): entity for entity in entities}


def validate_baseline(manifest: dict[str, Any]) -> None:
    if manifest.get("schema") != 1:
        fail(f"unexpected schema: {manifest.get('schema')!r}")
    if manifest.get("project_id") != "cvite-manifest-test":
        fail("project identity was not preserved")
    if not manifest.get("target_triple"):
        fail("target triple is empty")
    if manifest.get("pointer_width") not in (32, 64):
        fail(f"unexpected pointer width: {manifest.get('pointer_width')!r}")

    dependencies = manifest.get("dependencies")
    if not isinstance(dependencies, list):
        fail("dependencies is not a list")
    for expected in (
        "tests/fixtures/manifest_input.c",
        "tests/fixtures/manifest_input.h",
    ):
        if expected not in dependencies:
            fail(f"missing project dependency {expected!r}: {dependencies!r}")

    functions = manifest.get("functions")
    records = manifest.get("records")
    globals_ = manifest.get("globals")
    if (
        not isinstance(functions, list)
        or not isinstance(records, list)
        or not isinstance(globals_, list)
    ):
        fail("one or more entity collections are not arrays")

    game_update = entity_by_name(functions, "game_update")
    helper = entity_by_name(functions, "helper")
    assert_id(game_update.get("id"), "game_update.id")
    assert_id(
        game_update.get("semantic_fingerprint"),
        "game_update.semantic_fingerprint",
    )
    if helper.get("linkage") != "internal":
        fail(f"helper should have internal linkage: {helper!r}")

    player_record = entity_by_name(records, "Player")
    assert_id(player_record.get("id"), "Player.id")
    assert_id(player_record.get("layout_fingerprint"), "Player.layout_fingerprint")
    if player_record.get("size_bytes") != 12:
        fail(f"unexpected Player size: {player_record.get('size_bytes')!r}")
    fields = player_record.get("fields")
    if not isinstance(fields, list):
        fail("Player.fields is not an array")
    offsets = {field.get("name"): field.get("offset_bits") for field in fields}
    if offsets != {"x": 0, "y": 32, "health": 64}:
        fail(f"unexpected Player field offsets: {offsets!r}")

    external_counter = entity_by_name(globals_, "external_counter")
    player_global = entity_by_name(globals_, "player")
    calls = entity_by_name(globals_, "calls")
    pending_count = entity_by_name(globals_, "pending_count")
    if external_counter.get("linkage") != "external":
        fail(f"external_counter should have external linkage: {external_counter!r}")
    if player_global.get("storage_class") != "static":
        fail(f"player should be static: {player_global!r}")
    if calls.get("static_local") is not True:
        fail(f"calls should be recognized as a static local: {calls!r}")
    if pending_count.get("storage_class") != "static":
        fail(f"tentative static definition was not preserved: {pending_count!r}")

    identities: dict[str, str] = {}
    for collection in (functions, records, globals_):
        for entity in collection:
            identifier = entity.get("id")
            identity = entity.get("identity")
            if not isinstance(identifier, str) or not isinstance(identity, str):
                fail(f"entity lacks ID/identity: {entity!r}")
            previous = identities.setdefault(identifier, identity)
            if previous != identity:
                fail(f"ID collision between {previous!r} and {identity!r}")


def assert_body_edit_is_green(
    baseline: dict[str, Any],
    edited: dict[str, Any],
) -> None:
    for collection_name, fingerprint_name in (
        ("functions", "semantic_fingerprint"),
        ("records", "layout_fingerprint"),
        ("globals", "layout_fingerprint"),
    ):
        before = entities_by_name(baseline[collection_name])
        after = entities_by_name(edited[collection_name])
        if before.keys() != after.keys():
            fail(f"body edit changed {collection_name} membership")
        for name in before:
            if before[name]["id"] != after[name]["id"]:
                fail(f"body edit changed stable ID for {collection_name}.{name}")
            if before[name][fingerprint_name] != after[name][fingerprint_name]:
                fail(
                    f"body edit changed {fingerprint_name} for "
                    f"{collection_name}.{name}"
                )


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: check_manifest.py <tool> <source> <project-root>", file=sys.stderr)
        return 2

    tool = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    root = pathlib.Path(sys.argv[3]).resolve()
    relative_source = source.relative_to(root)
    relative_header = relative_source.with_suffix(".h")

    with tempfile.TemporaryDirectory(prefix="cvite-manifest-") as temporary:
        temporary_path = pathlib.Path(temporary)
        first_path = temporary_path / "first.json"
        second_path = temporary_path / "second.json"

        baseline = run_manifest(tool, source, root, first_path)
        run_manifest(tool, source, root, second_path)
        if first_path.read_bytes() != second_path.read_bytes():
            fail("manifest output is not deterministic across identical runs")
        validate_baseline(baseline)

        copied_root = temporary_path / "project"
        copied_source = copied_root / relative_source
        copied_header = copied_root / relative_header
        copied_source.parent.mkdir(parents=True)
        shutil.copy2(source, copied_source)
        shutil.copy2(root / relative_header, copied_header)

        copied_baseline = run_manifest(
            tool,
            copied_source,
            copied_root,
            temporary_path / "copied-baseline.json",
        )

        body_text = copied_source.read_text(encoding="utf-8")
        body_text = body_text.replace("player.x += 1.0f;", "player.x += 7.0f;")
        copied_source.write_text(body_text, encoding="utf-8")
        body_edited = run_manifest(
            tool,
            copied_source,
            copied_root,
            temporary_path / "body-edited.json",
        )
        assert_body_edit_is_green(copied_baseline, body_edited)

        header_text = copied_header.read_text(encoding="utf-8")
        header_text = header_text.replace(
            "    CVITE_FIELD(int, health);",
            "    CVITE_FIELD(int, health);\n    float speed;",
        )
        copied_header.write_text(header_text, encoding="utf-8")
        layout_edited = run_manifest(
            tool,
            copied_source,
            copied_root,
            temporary_path / "layout-edited.json",
        )
        old_player = entity_by_name(copied_baseline["records"], "Player")
        new_player = entity_by_name(layout_edited["records"], "Player")
        if old_player["id"] != new_player["id"]:
            fail("layout edit changed Player identity")
        if old_player["layout_fingerprint"] == new_player["layout_fingerprint"]:
            fail("layout edit did not change Player layout fingerprint")
        if new_player["size_bytes"] != 16:
            fail(f"unexpected edited Player size: {new_player['size_bytes']!r}")

        broken_text = copied_source.read_text(encoding="utf-8").replace(
            "return target->health;",
            "return target->health",
        )
        copied_source.write_text(broken_text, encoding="utf-8")
        broken_output = temporary_path / "broken.json"
        broken = invoke_manifest(tool, copied_source, copied_root, broken_output)
        if broken.returncode == 0:
            fail("syntax-error candidate unexpectedly produced a manifest")
        if "error:" not in broken.stderr:
            fail(f"syntax-error diagnostic was not reported: {broken.stderr!r}")
        if broken_output.exists() and broken_output.stat().st_size != 0:
            fail("syntax-error candidate wrote a publishable manifest")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
