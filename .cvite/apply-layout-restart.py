from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(path: str, old: str, new: str) -> None:
    file = ROOT / path
    text = file.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker missing in {path}: {old[:120]!r}")
    file.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/cli/run_internal.h",
    '''int cvite_native_link_poll(\n    cvite_orc_loader *loader,\n    const char *source_path,\n    const char *clang_path);\n''',
    '''int cvite_native_link_poll(\n    cvite_orc_loader *loader,\n    const char *source_path,\n    const char *clang_path);\n\nint cvite_restart_current_process(const char *reason);\n''',
)

replace_once(
    "src/cli/native_link.cpp",
    '''} // namespace\n\nextern "C" int cvite_native_link_prepare(\n''',
    '''} // namespace\n\nextern "C" int cvite_restart_current_process(const char *reason)\n{\n    const char *effective_reason =\n        reason != nullptr && reason[0] != '\\0'\n        ? reason\n        : "native state is incompatible with the candidate";\n    return restartCurrentProcess(effective_reason);\n}\n\nextern "C" int cvite_native_link_prepare(\n''',
)

replace_once(
    "src/cli/watcher.c",
    '''        if (error.status == CVITE_STATUS_LAYOUT_MISMATCH) {\n            (void)cvite_semantic_cache_report_pending();\n        }\n        (void)fprintf(stderr, "[cvite] previous code remains active\\n");\n''',
    '''        if (error.status == CVITE_STATUS_LAYOUT_MISMATCH) {\n            (void)cvite_semantic_cache_report_pending();\n            (void)cvite_restart_current_process(\n                "persistent storage layout changed");\n        }\n        (void)fprintf(stderr, "[cvite] previous code remains active\\n");\n''',
)
