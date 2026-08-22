from pathlib import Path

root = Path(__file__).resolve().parents[2]
module = root / "cmake/CViteSemantic.cmake"
text = module.read_text(encoding="utf-8")
old = """target_include_directories(cvite_semantic
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    SYSTEM PRIVATE
        ${CVITE_LIBCLANG_INCLUDE_DIR}
)
"""
new = """target_include_directories(cvite_semantic
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(cvite_semantic SYSTEM PRIVATE
    ${CVITE_LIBCLANG_INCLUDE_DIR}
)
"""
if old in text:
    text = text.replace(old, new, 1)
elif new not in text:
    raise SystemExit("unexpected CViteSemantic include block")
module.write_text(text, encoding="utf-8")
