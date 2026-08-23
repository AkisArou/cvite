if(NOT TARGET cvite_allocation_index OR
   NOT TARGET cvite_allocation_annotate OR
   NOT TARGET CViteLoweringPass)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(CVITE_PROXY_REAL_CLANG NAMES clang-18 clang REQUIRED)
find_program(CVITE_PROXY_OPT NAMES opt-18 opt REQUIRED)

add_executable(cvite_clang_wrapper
    src/compiler/clang_wrapper.cpp
)
set_target_properties(cvite_clang_wrapper PROPERTIES
    OUTPUT_NAME cvite-clang
)
target_compile_features(cvite_clang_wrapper PRIVATE cxx_std_17)
target_compile_definitions(cvite_clang_wrapper PRIVATE
    CVITE_REAL_CLANG_PATH="${CVITE_PROXY_REAL_CLANG}"
    CVITE_ALLOCATION_INDEX_PATH="$<TARGET_FILE:cvite_allocation_index>"
    CVITE_ALLOCATION_ANNOTATOR_PATH="$<TARGET_FILE:cvite_allocation_annotate>"
)
target_compile_options(cvite_clang_wrapper PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
)
if(CVITE_WARNINGS_AS_ERRORS)
    target_compile_options(cvite_clang_wrapper PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
    )
endif()
add_dependencies(cvite_clang_wrapper
    cvite_allocation_index
    cvite_allocation_annotate
)

if(TARGET cvite)
    add_dependencies(cvite cvite_clang_wrapper)
endif()

if(BUILD_TESTING)
    add_test(
        NAME invisible-clang-wrapper
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_clang_wrapper.py
            --wrapper $<TARGET_FILE:cvite_clang_wrapper>
            --opt ${CVITE_PROXY_OPT}
            --plugin $<TARGET_FILE:CViteLoweringPass>
            --source ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/managed_allocation_input.c
            --work ${CMAKE_CURRENT_BINARY_DIR}/clang-wrapper-test
    )
    set_tests_properties(invisible-clang-wrapper PROPERTIES TIMEOUT 60)
endif()

install(TARGETS
    cvite_clang_wrapper
    cvite_allocation_index
    cvite_allocation_annotate
    RUNTIME DESTINATION bin
)
