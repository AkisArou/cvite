if(NOT TARGET cvite_runtime)
    message(FATAL_ERROR "CViteManagedMemory requires cvite_runtime")
endif()

target_sources(cvite_runtime PRIVATE
    src/runtime/managed_memory.c
)

if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(cvite_test_managed_memory
        tests/test_managed_memory.c
    )
    target_compile_features(cvite_test_managed_memory PRIVATE c_std_11)
    target_link_libraries(cvite_test_managed_memory PRIVATE
        cvite_runtime
        Threads::Threads
    )
    target_compile_options(cvite_test_managed_memory PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
    )
    if(CVITE_WARNINGS_AS_ERRORS)
        target_compile_options(cvite_test_managed_memory PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Werror>
        )
    endif()
    add_test(NAME managed-memory-transaction COMMAND cvite_test_managed_memory)
    set_tests_properties(managed-memory-transaction PROPERTIES TIMEOUT 30)
endif()
