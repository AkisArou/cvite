from pathlib import Path

root = Path(__file__).resolve().parents[2]
script = root / "tests/check_managed_live.py"
if not script.exists():
    raise SystemExit("managed live test was not copied")
script.chmod(0o755)

module_path = root / "cmake/CViteCompilerProxy.cmake"
module = module_path.read_text(encoding="utf-8")
marker = """    set_tests_properties(invisible-clang-wrapper PROPERTIES TIMEOUT 60)
endif()
"""
replacement = """    set_tests_properties(invisible-clang-wrapper PROPERTIES TIMEOUT 60)

    add_test(
        NAME run-managed-allocation-refresh
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_managed_live.py
            --cvite $<TARGET_FILE:cvite>
            --cmake ${CMAKE_COMMAND}
            --source ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/run_managed_app.c
            --work ${CMAKE_CURRENT_BINARY_DIR}/managed-live-test
    )
    set_tests_properties(run-managed-allocation-refresh PROPERTIES TIMEOUT 60)
endif()
"""
if "run-managed-allocation-refresh" not in module:
    if marker not in module:
        raise SystemExit("compiler proxy test marker was not found")
    module = module.replace(marker, replacement, 1)
module_path.write_text(module, encoding="utf-8")
