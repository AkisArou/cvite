if(NOT TARGET cvite_semantic OR NOT TARGET cvite_runtime)
    return()
endif()

if(TARGET cvite_allocation_annotate)
    target_sources(cvite_allocation_annotate PRIVATE
        src/compiler/managed_type_emitter.cpp
    )
endif()

add_library(cvite_managed_type_diff STATIC
    src/host/managed_type_diff.cpp
)
target_compile_features(cvite_managed_type_diff PUBLIC cxx_std_17)
target_include_directories(cvite_managed_type_diff PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(cvite_managed_type_diff PUBLIC
    cvite_runtime
)
target_compile_options(cvite_managed_type_diff PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
)
if(CVITE_WARNINGS_AS_ERRORS)
    target_compile_options(cvite_managed_type_diff PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
    )
endif()

if(BUILD_TESTING)
    add_executable(cvite_test_managed_type_diff
        tests/test_managed_type_diff.cpp
    )
    target_compile_features(cvite_test_managed_type_diff PRIVATE cxx_std_17)
    target_link_libraries(cvite_test_managed_type_diff PRIVATE
        cvite_managed_type_diff
    )
    target_compile_options(cvite_test_managed_type_diff PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2;-Wundef>
    )
    if(CVITE_WARNINGS_AS_ERRORS)
        target_compile_options(cvite_test_managed_type_diff PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
        )
    endif()
    add_test(NAME managed-type-manifest-diff COMMAND cvite_test_managed_type_diff)
endif()
