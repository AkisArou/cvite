if(NOT TARGET cvite_runtime)
    message(FATAL_ERROR "CViteManagedMemory requires cvite_runtime")
endif()

find_package(Threads REQUIRED)

target_sources(cvite_runtime PRIVATE
    src/runtime/managed_memory.c
)
target_link_libraries(cvite_runtime PUBLIC Threads::Threads)

if(BUILD_TESTING)
    add_executable(cvite_test_managed_memory
        tests/test_managed_memory.c
    )
    target_link_libraries(cvite_test_managed_memory PRIVATE cvite::runtime)
    cvite_set_warnings(cvite_test_managed_memory)
    add_test(NAME managed-memory-transaction COMMAND cvite_test_managed_memory)
    set_tests_properties(managed-memory-transaction PROPERTIES TIMEOUT 30)
endif()
