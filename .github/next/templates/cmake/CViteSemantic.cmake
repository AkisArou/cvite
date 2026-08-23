if(NOT CVITE_BUILD_COMPILER)
    return()
endif()

if(NOT CVITE_LIBCLANG_INCLUDE_DIR OR NOT CVITE_LIBCLANG_LIBRARY)
    message(FATAL_ERROR "CVite semantic tools require libclang")
endif()

add_library(cvite_semantic STATIC
    src/compiler/semantic_index.cpp
    src/compiler/layout_plan.cpp
)

target_compile_features(cvite_semantic PUBLIC cxx_std_17)
target_include_directories(cvite_semantic
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    SYSTEM PRIVATE
        ${CVITE_LIBCLANG_INCLUDE_DIR}
)
target_link_libraries(cvite_semantic PUBLIC ${CVITE_LIBCLANG_LIBRARY})

target_compile_options(cvite_semantic PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
)
if(CVITE_WARNINGS_AS_ERRORS)
    target_compile_options(cvite_semantic PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
    )
endif()

add_executable(cvite_semantic_index
    src/compiler/semantic_index_main.cpp
)
set_target_properties(cvite_semantic_index PROPERTIES
    OUTPUT_NAME cvite-semantic-index
)
target_compile_features(cvite_semantic_index PRIVATE cxx_std_17)
target_link_libraries(cvite_semantic_index PRIVATE cvite_semantic)
target_compile_options(cvite_semantic_index PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
)
if(CVITE_WARNINGS_AS_ERRORS)
    target_compile_options(cvite_semantic_index PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
    )
endif()

if(BUILD_TESTING)
    add_executable(cvite_test_layout_plan
        tests/test_layout_plan.cpp
    )
    target_compile_features(cvite_test_layout_plan PRIVATE cxx_std_17)
    target_link_libraries(cvite_test_layout_plan PRIVATE cvite_semantic)
    target_compile_definitions(cvite_test_layout_plan PRIVATE
        CVITE_LAYOUT_OLD_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/layout_old.c"
        CVITE_LAYOUT_APPEND_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/layout_append.c"
        CVITE_LAYOUT_BREAK_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/layout_break.c"
    )
    target_compile_options(cvite_test_layout_plan PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
    )
    if(CVITE_WARNINGS_AS_ERRORS)
        target_compile_options(cvite_test_layout_plan PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
        )
    endif()
    add_test(NAME semantic-layout-plan COMMAND cvite_test_layout_plan)
endif()
