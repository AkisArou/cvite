if(NOT BUILD_TESTING)
    return()
endif()

find_package(Threads REQUIRED)

add_executable(cvite_test_runtime_stress
    tests/test_runtime_stress.c
)
target_compile_features(cvite_test_runtime_stress PRIVATE c_std_11)
target_link_libraries(cvite_test_runtime_stress PRIVATE
    cvite_runtime
    Threads::Threads
)
target_compile_options(cvite_test_runtime_stress PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
)
if(CVITE_WARNINGS_AS_ERRORS)
    target_compile_options(cvite_test_runtime_stress PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Werror>
    )
endif()
add_test(NAME runtime-publication-stress COMMAND cvite_test_runtime_stress)
set_tests_properties(runtime-publication-stress PROPERTIES TIMEOUT 60)
