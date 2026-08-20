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
option(CVITE_BUILD_COMPILER "Build the libclang semantic-manifest tool" OFF)
option(CVITE_BUILD_LLVM_PASS "Build the version-matched LLVM lowering pass" OFF)

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

if(CVITE_BUILD_COMPILER)
    enable_language(CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    set(CVITE_LIBCLANG_ROOT "" CACHE PATH "Optional libclang installation prefix")
    find_path(CVITE_LIBCLANG_INCLUDE_DIR
        NAMES clang-c/Index.h
        HINTS
            ${CVITE_LIBCLANG_ROOT}/include
            /usr/lib/llvm-18/include
            /usr/lib/llvm-19/include
            /usr/local/include
    )
    find_library(CVITE_LIBCLANG_LIBRARY
        NAMES clang libclang clang-18 clang-19
        HINTS
            ${CVITE_LIBCLANG_ROOT}/lib
            ${CVITE_LIBCLANG_ROOT}/lib64
            /usr/lib/llvm-18/lib
            /usr/lib/llvm-19/lib
            /usr/local/lib
    )

    if(NOT CVITE_LIBCLANG_INCLUDE_DIR OR NOT CVITE_LIBCLANG_LIBRARY)
        message(FATAL_ERROR
            "CVITE_BUILD_COMPILER requires libclang headers and library. "
            "Install libclang-dev or set CVITE_LIBCLANG_ROOT, "
            "CVITE_LIBCLANG_INCLUDE_DIR, and CVITE_LIBCLANG_LIBRARY.")
    endif()

    add_executable(cvite_manifest src/compiler/manifest_main.cpp)
    set_target_properties(cvite_manifest PROPERTIES OUTPUT_NAME cvite-manifest)
    target_include_directories(cvite_manifest PRIVATE ${CVITE_LIBCLANG_INCLUDE_DIR})
    target_link_libraries(cvite_manifest PRIVATE ${CVITE_LIBCLANG_LIBRARY})
    cvite_set_warnings(cvite_manifest)

    install(TARGETS cvite_manifest RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    if(BUILD_TESTING)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(
            NAME manifest
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_manifest.py
                $<TARGET_FILE:cvite_manifest>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/manifest_input.c
                ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endif()
endif()

if(CVITE_BUILD_LLVM_PASS)
    enable_language(CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    find_package(LLVM 18 REQUIRED CONFIG)
    list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
    include(AddLLVM)

    add_llvm_pass_plugin(CViteLoweringPass
        src/transform/lowering_pass.cpp
    )
    cvite_set_warnings(CViteLoweringPass)

    if(BUILD_TESTING)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        find_program(CVITE_CLANG_EXECUTABLE
            NAMES clang-18 clang
            HINTS ${LLVM_TOOLS_BINARY_DIR}
            REQUIRED
        )
        find_program(CVITE_OPT_EXECUTABLE
            NAMES opt-18 opt
            HINTS ${LLVM_TOOLS_BINARY_DIR}
            REQUIRED
        )
        add_test(
            NAME lowering-pass
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_lowering.py
                ${CVITE_CLANG_EXECUTABLE}
                ${CVITE_OPT_EXECUTABLE}
                $<TARGET_FILE:CViteLoweringPass>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/lowering_input.c
                ${CMAKE_CURRENT_BINARY_DIR}
        )
    endif()
endif()

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
