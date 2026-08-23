if(NOT TARGET cvite_semantic OR NOT TARGET cvite_runtime)
    return()
endif()

add_library(cvite_managed_refresh STATIC
    src/host/managed_refresh.cpp
)
target_compile_features(cvite_managed_refresh PUBLIC cxx_std_17)
target_include_directories(cvite_managed_refresh PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(cvite_managed_refresh PUBLIC
    cvite_semantic
    cvite_runtime
)
target_compile_options(cvite_managed_refresh PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
)
if(CVITE_WARNINGS_AS_ERRORS)
    target_compile_options(cvite_managed_refresh PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
    )
endif()

if(BUILD_TESTING)
    add_executable(cvite_test_managed_refresh
        tests/test_managed_refresh.cpp
    )
    target_compile_features(cvite_test_managed_refresh PRIVATE cxx_std_17)
    target_link_libraries(cvite_test_managed_refresh PRIVATE
        cvite_managed_refresh
    )
    target_compile_definitions(cvite_test_managed_refresh PRIVATE
        CVITE_LAYOUT_OLD_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/layout_old.c"
        CVITE_LAYOUT_APPEND_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/layout_append.c"
        CVITE_LAYOUT_BREAK_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/layout_break.c"
    )
    target_compile_options(cvite_test_managed_refresh PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
    )
    if(CVITE_WARNINGS_AS_ERRORS)
        target_compile_options(cvite_test_managed_refresh PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
        )
    endif()
    add_test(NAME semantic-managed-refresh COMMAND cvite_test_managed_refresh)
    set_tests_properties(semantic-managed-refresh PROPERTIES TIMEOUT 30)
endif()
