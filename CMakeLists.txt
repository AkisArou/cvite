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
option(CVITE_BUILD_ORC_LOADER "Build the LLVM ORC/JITLink object loader" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CVITE_BUILD_COMPILER OR CVITE_BUILD_LLVM_PASS OR CVITE_BUILD_ORC_LOADER)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

if(CVITE_BUILD_LLVM_PASS OR CVITE_BUILD_ORC_LOADER)
    find_package(LLVM REQUIRED CONFIG)
    if(LLVM_PACKAGE_VERSION VERSION_LESS 18 OR
       LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL 19)
        message(FATAL_ERROR
            "CVite's LLVM components currently require LLVM 18.x; "
            "found ${LLVM_PACKAGE_VERSION}.")
    endif()
    list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
    include(AddLLVM)
endif()

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

add_library(cvite_host STATIC src/host/host_runtime.c)
add_library(cvite::host ALIAS cvite_host)
target_include_directories(cvite_host
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cvite_host PUBLIC cvite::runtime)
cvite_set_warnings(cvite_host)

add_executable(cvite src/cli/main.c)
target_link_libraries(cvite PRIVATE cvite::runtime)
cvite_set_warnings(cvite)

if(CVITE_BUILD_COMPILER)
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
    find_program(CVITE_LLVM_LINK_EXECUTABLE
        NAMES llvm-link-18 llvm-link
        HINTS ${LLVM_TOOLS_BINARY_DIR}
        REQUIRED
    )

    add_llvm_pass_plugin(CViteLoweringPass
        src/transform/lowering_pass.cpp
        src/transform/stable_entry_pass.cpp
        src/transform/baseline_manifest_pass.cpp
        src/transform/candidate_pass.cpp
    )
    target_include_directories(CViteLoweringPass SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    cvite_set_warnings(CViteLoweringPass)
    install(TARGETS CViteLoweringPass
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/cvite
    )

    if(BUILD_TESTING)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
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
        add_test(
            NAME stable-entry-pass
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_stable_entry.py
                ${CVITE_CLANG_EXECUTABLE}
                ${CVITE_OPT_EXECUTABLE}
                $<TARGET_FILE:CViteLoweringPass>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/stable_entry_input.c
                ${CMAKE_CURRENT_BINARY_DIR}
        )
        add_test(
            NAME candidate-pass
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_candidate.py
                ${CVITE_CLANG_EXECUTABLE}
                ${CVITE_OPT_EXECUTABLE}
                $<TARGET_FILE:CViteLoweringPass>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/candidate_input.c
                ${CMAKE_CURRENT_BINARY_DIR}
        )
    endif()
endif()

if(CVITE_BUILD_ORC_LOADER)
    add_library(cvite_orc_loader STATIC src/host/orc_loader.cpp)
    add_library(cvite::orc_loader ALIAS cvite_orc_loader)
    target_include_directories(cvite_orc_loader
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_include_directories(cvite_orc_loader SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_link_libraries(cvite_orc_loader PUBLIC cvite::host)
    # libclang and ORC must share one LLVM runtime. Linking the ORC component
    # archives beside libclang's libLLVM registers LLVM command-line options
    # twice and aborts before the development server can start. LLVM's own
    # helper links libLLVM and adds only components omitted from that dylib.
    llvm_config(cvite_orc_loader USE_SHARED
        Core
        JITLink
        OrcJIT
        Support
        nativecodegen
    )
    cvite_set_warnings(cvite_orc_loader)
    install(TARGETS cvite_orc_loader
        EXPORT CViteTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    if(BUILD_TESTING)
        if(CVITE_BUILD_LLVM_PASS)
            set(CVITE_ORC_CLANG_EXECUTABLE ${CVITE_CLANG_EXECUTABLE})
        else()
            find_program(CVITE_ORC_CLANG_EXECUTABLE
                NAMES clang-18 clang
                HINTS ${LLVM_TOOLS_BINARY_DIR}
                REQUIRED
            )
        endif()

        set(CVITE_ORC_TEST_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/orc-test")
        set(CVITE_ORC_VALID_OBJECT "${CVITE_ORC_TEST_DIRECTORY}/candidate.o")
        set(CVITE_ORC_BROKEN_OBJECT "${CVITE_ORC_TEST_DIRECTORY}/broken.o")
        add_custom_command(
            OUTPUT ${CVITE_ORC_VALID_OBJECT} ${CVITE_ORC_BROKEN_OBJECT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CVITE_ORC_TEST_DIRECTORY}
            COMMAND
                ${CVITE_ORC_CLANG_EXECUTABLE}
                -std=c11 -O0 -fPIC -c
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/orc_candidate.c
                -o ${CVITE_ORC_VALID_OBJECT}
            COMMAND
                ${CVITE_ORC_CLANG_EXECUTABLE}
                -std=c11 -O0 -fPIC -c
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/orc_broken_candidate.c
                -o ${CVITE_ORC_BROKEN_OBJECT}
            DEPENDS
                tests/fixtures/orc_candidate.c
                tests/fixtures/orc_broken_candidate.c
            VERBATIM
        )
        add_custom_target(cvite_orc_test_objects
            DEPENDS ${CVITE_ORC_VALID_OBJECT} ${CVITE_ORC_BROKEN_OBJECT}
        )
        add_executable(cvite_test_orc_loader tests/test_orc_loader.c)
        target_link_libraries(cvite_test_orc_loader PRIVATE cvite::orc_loader)
        cvite_set_warnings(cvite_test_orc_loader)
        add_dependencies(cvite_test_orc_loader cvite_orc_test_objects)
        add_test(
            NAME orc-loader
            COMMAND
                cvite_test_orc_loader
                ${CVITE_ORC_VALID_OBJECT}
                ${CVITE_ORC_BROKEN_OBJECT}
        )
    endif()
endif()

if(CVITE_BUILD_LLVM_PASS AND CVITE_BUILD_ORC_LOADER)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The M1 cvite run loop currently requires Linux/inotify")
    endif()

    find_package(Threads REQUIRED)
    target_sources(cvite PRIVATE
        src/cli/run.c
        src/cli/toolchain.c
        src/cli/watcher.c
        src/cli/dependencies.c
        src/cli/compile_database.c
        src/cli/native_link.cpp
    )
    target_link_libraries(cvite PRIVATE cvite::orc_loader Threads::Threads)
    if(CVITE_BUILD_COMPILER)
        target_include_directories(cvite SYSTEM PRIVATE ${CVITE_LIBCLANG_INCLUDE_DIR})
        target_link_libraries(cvite PRIVATE ${CVITE_LIBCLANG_LIBRARY})
    endif()
    target_compile_definitions(cvite PRIVATE
        CVITE_RUNNER_ENABLED=1
        CVITE_COMPILE_DATABASE_ENABLED=$<BOOL:${CVITE_BUILD_COMPILER}>
        _POSIX_C_SOURCE=200809L
        _XOPEN_SOURCE=700
        "CVITE_CLANG_PATH=\"${CVITE_CLANG_EXECUTABLE}\""
        "CVITE_OPT_PATH=\"${CVITE_OPT_EXECUTABLE}\""
        "CVITE_LLVM_LINK_PATH=\"${CVITE_LLVM_LINK_EXECUTABLE}\""
        "CVITE_PASS_PLUGIN_BUILD_PATH=\"$<TARGET_FILE:CViteLoweringPass>\""
        "CVITE_PASS_PLUGIN_INSTALL_PATH=\"${CMAKE_INSTALL_FULL_LIBDIR}/cvite/$<TARGET_FILE_NAME:CViteLoweringPass>\""
    )
    set_property(TARGET cvite PROPERTY LINKER_LANGUAGE CXX)
    add_dependencies(cvite CViteLoweringPass)
endif()

if(CVITE_BUILD_LLVM_PASS AND CVITE_BUILD_ORC_LOADER AND BUILD_TESTING)
    set(CVITE_STABLE_TEST_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/stable-entry-test")
    set(CVITE_STABLE_BASELINE_RAW_IR
        "${CVITE_STABLE_TEST_DIRECTORY}/baseline.raw.ll")
    set(CVITE_STABLE_BASELINE_IR
        "${CVITE_STABLE_TEST_DIRECTORY}/baseline.ll")
    set(CVITE_STABLE_BASELINE_OBJECT
        "${CVITE_STABLE_TEST_DIRECTORY}/baseline.o")
    set(CVITE_STABLE_CANDIDATE_RAW_IR
        "${CVITE_STABLE_TEST_DIRECTORY}/candidate.raw.ll")
    set(CVITE_STABLE_CANDIDATE_IR
        "${CVITE_STABLE_TEST_DIRECTORY}/candidate.ll")
    set(CVITE_STABLE_CANDIDATE_OBJECT
        "${CVITE_STABLE_TEST_DIRECTORY}/candidate.o")

    add_custom_command(
        OUTPUT ${CVITE_STABLE_BASELINE_OBJECT} ${CVITE_STABLE_CANDIDATE_OBJECT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CVITE_STABLE_TEST_DIRECTORY}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -std=c11 -O0 -S -emit-llvm
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/stable_e2e_baseline.c
            -o ${CVITE_STABLE_BASELINE_RAW_IR}
        COMMAND
            ${CVITE_OPT_EXECUTABLE}
            -load-pass-plugin=$<TARGET_FILE:CViteLoweringPass>
            -passes=cvite-lowering,cvite-baseline,cvite-baseline-manifest,verify
            -S ${CVITE_STABLE_BASELINE_RAW_IR}
            -o ${CVITE_STABLE_BASELINE_IR}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -O0 -fPIC -c ${CVITE_STABLE_BASELINE_IR}
            -o ${CVITE_STABLE_BASELINE_OBJECT}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -std=c11 -O0 -S -emit-llvm
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/stable_e2e_candidate.c
            -o ${CVITE_STABLE_CANDIDATE_RAW_IR}
        COMMAND
            ${CVITE_OPT_EXECUTABLE}
            -load-pass-plugin=$<TARGET_FILE:CViteLoweringPass>
            -passes=cvite-lowering,cvite-candidate,verify
            -S ${CVITE_STABLE_CANDIDATE_RAW_IR}
            -o ${CVITE_STABLE_CANDIDATE_IR}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -O0 -fPIC -c ${CVITE_STABLE_CANDIDATE_IR}
            -o ${CVITE_STABLE_CANDIDATE_OBJECT}
        DEPENDS
            CViteLoweringPass
            tests/fixtures/stable_e2e_baseline.c
            tests/fixtures/stable_e2e_candidate.c
        VERBATIM
    )
    add_custom_target(cvite_stable_entry_test_objects
        DEPENDS ${CVITE_STABLE_BASELINE_OBJECT} ${CVITE_STABLE_CANDIDATE_OBJECT}
    )
    set_source_files_properties(${CVITE_STABLE_BASELINE_OBJECT}
        PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE
    )
    add_executable(cvite_test_stable_entry
        tests/test_stable_entry.c
        ${CVITE_STABLE_BASELINE_OBJECT}
    )
    target_link_libraries(cvite_test_stable_entry PRIVATE
        cvite::host
        cvite::orc_loader
    )
    cvite_set_warnings(cvite_test_stable_entry)
    add_dependencies(cvite_test_stable_entry cvite_stable_entry_test_objects)
    add_test(
        NAME stable-entry-e2e
        COMMAND cvite_test_stable_entry ${CVITE_STABLE_CANDIDATE_OBJECT}
    )

    set(CVITE_BASELINE_LOADER_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/baseline-loader-test")
    set(CVITE_BASELINE_LOADER_RAW_IR
        "${CVITE_BASELINE_LOADER_DIRECTORY}/baseline.raw.ll")
    set(CVITE_BASELINE_LOADER_IR
        "${CVITE_BASELINE_LOADER_DIRECTORY}/baseline.ll")
    set(CVITE_BASELINE_LOADER_OBJECT
        "${CVITE_BASELINE_LOADER_DIRECTORY}/baseline.o")
    add_custom_command(
        OUTPUT ${CVITE_BASELINE_LOADER_OBJECT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CVITE_BASELINE_LOADER_DIRECTORY}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -std=c11 -O0 -S -emit-llvm
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/jit_baseline.c
            -o ${CVITE_BASELINE_LOADER_RAW_IR}
        COMMAND
            ${CVITE_OPT_EXECUTABLE}
            -load-pass-plugin=$<TARGET_FILE:CViteLoweringPass>
            -passes=cvite-lowering,cvite-baseline,cvite-baseline-manifest,verify
            -S ${CVITE_BASELINE_LOADER_RAW_IR}
            -o ${CVITE_BASELINE_LOADER_IR}
        COMMAND
            ${CVITE_CLANG_EXECUTABLE}
            -O0 -fPIC -c ${CVITE_BASELINE_LOADER_IR}
            -o ${CVITE_BASELINE_LOADER_OBJECT}
        DEPENDS
            CViteLoweringPass
            tests/fixtures/jit_baseline.c
        VERBATIM
    )
    add_custom_target(cvite_baseline_loader_test_object
        DEPENDS ${CVITE_BASELINE_LOADER_OBJECT}
    )
    add_executable(cvite_test_baseline_loader tests/test_baseline_loader.c)
    target_link_libraries(cvite_test_baseline_loader PRIVATE cvite::orc_loader)
    cvite_set_warnings(cvite_test_baseline_loader)
    add_dependencies(
        cvite_test_baseline_loader
        cvite_baseline_loader_test_object
    )
    add_test(
        NAME baseline-loader
        COMMAND cvite_test_baseline_loader ${CVITE_BASELINE_LOADER_OBJECT}
    )

    add_test(
        NAME run-fast-refresh
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_run.py
            $<TARGET_FILE:cvite>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/run_app.c
    )
    set_tests_properties(run-fast-refresh PROPERTIES TIMEOUT 90)

    add_test(
        NAME run-multi-tu-refresh
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_multi_tu.py
            $<TARGET_FILE:cvite>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/run_multi_main.c
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/run_multi_worker.c
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/run_multi_shared.h
    )
    set_tests_properties(run-multi-tu-refresh PROPERTIES TIMEOUT 100)
endif()


if(CVITE_BUILD_LLVM_PASS AND CVITE_BUILD_ORC_LOADER AND BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_library(cvite_test_auto_native SHARED
        tests/fixtures/auto_link_library.c
    )
    set_target_properties(cvite_test_auto_native PROPERTIES
        OUTPUT_NAME cvite_auto_native
    )

    add_library(cvite_test_auto_native_next SHARED
        tests/fixtures/auto_link_library_next.c
    )
    set_target_properties(cvite_test_auto_native_next PROPERTIES
        OUTPUT_NAME cvite_auto_native_next
    )

    add_test(
        NAME run-auto-native-link
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_auto_link.py
            $<TARGET_FILE:cvite>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/auto_link_app.c
            $<TARGET_FILE:cvite_test_auto_native>
    )
    set_tests_properties(run-auto-native-link PROPERTIES TIMEOUT 60)

    foreach(CVITE_NATIVE_LINK_BACKEND IN ITEMS cmake-file-api ninja)
        add_test(
            NAME run-native-link-${CVITE_NATIVE_LINK_BACKEND}
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_link_backend.py
                ${CVITE_NATIVE_LINK_BACKEND}
                $<TARGET_FILE:cvite>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/auto_link_app.c
                $<TARGET_FILE:cvite_test_auto_native>
                ${CMAKE_COMMAND}
                ${CMAKE_MAKE_PROGRAM}
        )
        set_tests_properties(
            run-native-link-${CVITE_NATIVE_LINK_BACKEND}
            PROPERTIES TIMEOUT 70
        )
    endforeach()

    add_test(
        NAME run-native-input-replacement
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_input_replacement.py
            $<TARGET_FILE:cvite>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/auto_link_app.c
            $<TARGET_FILE:cvite_test_auto_native>
            $<TARGET_FILE:cvite_test_auto_native_next>
            ${CMAKE_COMMAND}
            ${CMAKE_MAKE_PROGRAM}
    )
    set_tests_properties(run-native-input-replacement PROPERTIES TIMEOUT 75)
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

    add_executable(cvite_test_host tests/test_host.c)
    target_link_libraries(cvite_test_host PRIVATE cvite::host)
    cvite_set_warnings(cvite_test_host)
    add_test(NAME host COMMAND cvite_test_host)

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

install(TARGETS cvite cvite_runtime cvite_host
    EXPORT CViteTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
