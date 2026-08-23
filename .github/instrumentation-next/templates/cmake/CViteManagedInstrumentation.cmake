if(NOT CVITE_BUILD_LLVM_PASS OR NOT TARGET CViteLoweringPass)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(CVITE_MANAGED_TEST_CLANG NAMES clang-18 clang REQUIRED)
find_program(CVITE_MANAGED_TEST_OPT NAMES opt-18 opt REQUIRED)

# Add the development-only allocation/provenance transform to the existing
# pass plugin. Production builds that do not load this pass remain ordinary C.
target_sources(CViteLoweringPass PRIVATE
    src/transform/managed_allocation_pass.cpp
)

if(TARGET cvite AND TARGET cvite_semantic)
    target_sources(cvite PRIVATE
        src/host/managed_host.c
    )
    set_target_properties(cvite PROPERTIES ENABLE_EXPORTS ON)

    llvm_map_components_to_libnames(
        CVITE_ALLOCATION_ANNOTATOR_LLVM_LIBS
        Core
        IRReader
        Support
    )
    add_executable(cvite_allocation_annotate
        src/compiler/allocation_annotator.cpp
    )
    set_target_properties(cvite_allocation_annotate PROPERTIES
        OUTPUT_NAME cvite-allocation-annotate
    )
    target_compile_features(cvite_allocation_annotate PRIVATE cxx_std_17)
    target_include_directories(cvite_allocation_annotate SYSTEM PRIVATE
        ${LLVM_INCLUDE_DIRS}
    )
    target_link_libraries(cvite_allocation_annotate PRIVATE
        cvite_semantic
        ${CVITE_ALLOCATION_ANNOTATOR_LLVM_LIBS}
    )
    target_compile_options(cvite_allocation_annotate PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
    )
    if(CVITE_WARNINGS_AS_ERRORS)
        target_compile_options(cvite_allocation_annotate PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
        )
    endif()
endif()

if(BUILD_TESTING AND TARGET cvite_allocation_index AND
   TARGET cvite_allocation_annotate)
    add_test(
        NAME managed-allocation-pass
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_managed_allocation_pass.py
            --clang ${CVITE_MANAGED_TEST_CLANG}
            --opt ${CVITE_MANAGED_TEST_OPT}
            --allocation-index $<TARGET_FILE:cvite_allocation_index>
            --annotator $<TARGET_FILE:cvite_allocation_annotate>
            --plugin $<TARGET_FILE:CViteLoweringPass>
            --source ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/managed_allocation_input.c
            --work ${CMAKE_CURRENT_BINARY_DIR}/managed-allocation-pass-test
    )
    set_tests_properties(managed-allocation-pass PROPERTIES TIMEOUT 60)
endif()
