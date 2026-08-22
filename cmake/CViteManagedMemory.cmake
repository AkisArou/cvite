# Internal development-runtime memory provenance and migration support. This
# stays separate from the source-level programming model: ordinary C code does
# not include or call the managed-memory API.

if(UNIX)
    set(_cvite_managed_memory_default ON)
else()
    set(_cvite_managed_memory_default OFF)
endif()

option(
    CVITE_BUILD_MANAGED_MEMORY
    "Build the experimental compiler-managed allocation runtime"
    ${_cvite_managed_memory_default}
)
unset(_cvite_managed_memory_default)

if(NOT CVITE_BUILD_MANAGED_MEMORY)
    return()
endif()

if(NOT TARGET cvite_runtime)
    message(FATAL_ERROR "CViteManagedMemory requires cvite_runtime")
endif()

if(NOT UNIX)
    message(FATAL_ERROR
        "CVITE_BUILD_MANAGED_MEMORY currently requires a POSIX threads backend")
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
    target_compile_features(cvite_test_managed_memory PRIVATE c_std_11)
    target_link_libraries(cvite_test_managed_memory PRIVATE
        cvite::runtime
    )
    cvite_set_warnings(cvite_test_managed_memory)
    add_test(NAME managed-memory-transaction COMMAND cvite_test_managed_memory)
    set_tests_properties(managed-memory-transaction PROPERTIES TIMEOUT 30)
endif()
