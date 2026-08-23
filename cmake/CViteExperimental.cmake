# Development-only CVite extensions that are intentionally kept separate from
# the core runtime target graph. This file is included at the end of the root
# CMakeLists.txt after the base targets have been declared.

option(
    CVITE_BUILD_SOAK_TESTS
    "Build long-running multithreaded live-refresh soak tests"
    OFF
)

if(TARGET cvite_semantic_index AND NOT TARGET cvite_layout_plan)
    add_library(cvite_layout_plan STATIC
        ${PROJECT_SOURCE_DIR}/src/compiler/layout_plan.cpp
    )
    target_include_directories(cvite_layout_plan
        PUBLIC ${PROJECT_SOURCE_DIR}/src/compiler
    )
    target_link_libraries(cvite_layout_plan PUBLIC cvite_semantic_index)
    cvite_set_warnings(cvite_layout_plan)

    add_executable(cvite_layout_plan_cli
        ${PROJECT_SOURCE_DIR}/src/compiler/layout_plan_main.cpp
    )
    set_target_properties(
        cvite_layout_plan_cli
        PROPERTIES OUTPUT_NAME cvite-layout-plan
    )
    target_link_libraries(cvite_layout_plan_cli PRIVATE cvite_layout_plan)
    cvite_set_warnings(cvite_layout_plan_cli)
    install(
        TARGETS cvite_layout_plan_cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(
    TARGET cvite AND
    TARGET cvite_semantic_index AND
    CVITE_BUILD_LLVM_PASS AND
    CVITE_BUILD_ORC_LOADER
)
    get_target_property(_cvite_runner_sources cvite SOURCES)
    list(FIND _cvite_runner_sources
        "${PROJECT_SOURCE_DIR}/src/cli/semantic_cache.cpp"
        _cvite_semantic_cache_absolute_index
    )
    list(FIND _cvite_runner_sources
        "src/cli/semantic_cache.cpp"
        _cvite_semantic_cache_relative_index
    )
    if(
        _cvite_semantic_cache_absolute_index EQUAL -1 AND
        _cvite_semantic_cache_relative_index EQUAL -1
    )
        target_sources(cvite PRIVATE
            ${PROJECT_SOURCE_DIR}/src/cli/semantic_cache.cpp
        )
    endif()
    target_link_libraries(cvite PRIVATE cvite_semantic_index)
    target_compile_definitions(cvite PRIVATE CVITE_SEMANTIC_CACHE_ENABLED=1)
endif()

if(BUILD_TESTING)
    find_package(Threads REQUIRED)

    if(TARGET cvite_layout_plan AND NOT TARGET cvite_test_layout_plan)
        add_executable(cvite_test_layout_plan
            ${PROJECT_SOURCE_DIR}/tests/test_layout_plan.cpp
        )
        target_link_libraries(
            cvite_test_layout_plan
            PRIVATE cvite_layout_plan
        )
        cvite_set_warnings(cvite_test_layout_plan)
        if(NOT TEST layout-plan)
            add_test(NAME layout-plan COMMAND cvite_test_layout_plan)
        endif()
    endif()

    if(NOT TARGET cvite_test_runtime_stress)
        add_executable(cvite_test_runtime_stress
            ${PROJECT_SOURCE_DIR}/tests/test_runtime_stress.c
        )
        target_link_libraries(
            cvite_test_runtime_stress
            PRIVATE cvite::runtime Threads::Threads
        )
        target_compile_definitions(
            cvite_test_runtime_stress
            PRIVATE _POSIX_C_SOURCE=200809L
        )
        cvite_set_warnings(cvite_test_runtime_stress)
        if(NOT TEST runtime-stress)
            add_test(NAME runtime-stress COMMAND cvite_test_runtime_stress)
            set_tests_properties(runtime-stress PROPERTIES TIMEOUT 45)
        endif()
    endif()

    if(
        TARGET cvite AND
        TARGET cvite_semantic_index AND
        CVITE_BUILD_LLVM_PASS AND
        CVITE_BUILD_ORC_LOADER
    )
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        if(NOT TEST run-semantic-layout-restart)
            add_test(
                NAME run-semantic-layout-restart
                COMMAND
                    ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/check_semantic_restart.py
                    $<TARGET_FILE:cvite>
            )
            set_tests_properties(
                run-semantic-layout-restart
                PROPERTIES TIMEOUT 75
            )
        endif()
        if(CVITE_BUILD_SOAK_TESTS AND NOT TEST run-live-refresh-soak)
            add_test(
                NAME run-live-refresh-soak
                COMMAND
                    ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/soak_live_refresh.py
                    $<TARGET_FILE:cvite>
            )
            set_tests_properties(
                run-live-refresh-soak
                PROPERTIES TIMEOUT 150
            )
        endif()
    endif()
endif()
