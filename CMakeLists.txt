cmake_minimum_required(VERSION 3.20)

project(
    cvite
    VERSION 0.1.0
    DESCRIPTION "Fast Refresh for ordinary C"
    LANGUAGES C
)

include(CTest)
include(GNUInstallDirs)
include(cmake/CViteWarnings.cmake)

option(CVITE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CVITE_BUILD_EXAMPLES "Build CVite examples" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(cvite_runtime STATIC
    src/runtime/runtime_core.c
    src/runtime/runtime_function.c
    src/runtime/runtime_patch.c
    src/runtime/runtime_storage.c
    src/runtime/status.c
    src/protocol/protocol.c
)
add_library(cvite::runtime ALIAS cvite_runtime)

target_include_directories(cvite_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(cvite_runtime PRIVATE _POSIX_C_SOURCE=200112L)
cvite_set_warnings(cvite_runtime)

add_executable(cvite src/cli/main.c)
target_link_libraries(cvite PRIVATE cvite::runtime)
cvite_set_warnings(cvite)

if(CVITE_BUILD_EXAMPLES)
    add_executable(cvite_runtime_transaction examples/runtime_transaction.c)
    target_link_libraries(cvite_runtime_transaction PRIVATE cvite::runtime)
    cvite_set_warnings(cvite_runtime_transaction)
endif()

if(BUILD_TESTING)
    find_package(Threads REQUIRED)

    add_executable(cvite_test_runtime tests/test_runtime.c)
    target_link_libraries(cvite_test_runtime PRIVATE cvite::runtime)
    cvite_set_warnings(cvite_test_runtime)
    add_test(NAME runtime COMMAND cvite_test_runtime)

    add_executable(cvite_test_protocol tests/test_protocol.c)
    target_link_libraries(cvite_test_protocol PRIVATE cvite::runtime)
    cvite_set_warnings(cvite_test_protocol)
    add_test(NAME protocol COMMAND cvite_test_protocol)

    add_executable(cvite_test_concurrency tests/test_concurrency.c)
    target_link_libraries(cvite_test_concurrency PRIVATE cvite::runtime Threads::Threads)
    cvite_set_warnings(cvite_test_concurrency)
    add_test(NAME concurrency COMMAND cvite_test_concurrency)
    set_tests_properties(concurrency PROPERTIES TIMEOUT 30)
endif()

install(TARGETS cvite cvite_runtime
    EXPORT CViteTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
