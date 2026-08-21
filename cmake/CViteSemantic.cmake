# Clang-backed source semantic metadata. This intentionally consumes the
# libclang installation already discovered by the root compiler-tool block.

if(
    CVITE_BUILD_COMPILER AND
    CVITE_LIBCLANG_INCLUDE_DIR AND
    CVITE_LIBCLANG_LIBRARY AND
    NOT TARGET cvite_semantic_index
)
    add_library(cvite_semantic_index STATIC
        ${PROJECT_SOURCE_DIR}/src/compiler/semantic_index.cpp
    )
    target_include_directories(cvite_semantic_index
        PUBLIC ${PROJECT_SOURCE_DIR}/src/compiler
    )
    target_include_directories(cvite_semantic_index
        SYSTEM PRIVATE ${CVITE_LIBCLANG_INCLUDE_DIR}
    )
    target_link_libraries(
        cvite_semantic_index
        PRIVATE ${CVITE_LIBCLANG_LIBRARY}
    )
    cvite_set_warnings(cvite_semantic_index)

    add_executable(cvite_semantic_index_cli
        ${PROJECT_SOURCE_DIR}/src/compiler/semantic_index_main.cpp
    )
    set_target_properties(
        cvite_semantic_index_cli
        PROPERTIES OUTPUT_NAME cvite-semantic-index
    )
    target_link_libraries(
        cvite_semantic_index_cli
        PRIVATE cvite_semantic_index
    )
    cvite_set_warnings(cvite_semantic_index_cli)
    install(
        TARGETS cvite_semantic_index_cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    if(BUILD_TESTING)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        if(NOT TEST semantic-index)
            add_test(
                NAME semantic-index
                COMMAND
                    ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/check_semantic_index.py
                    $<TARGET_FILE:cvite_semantic_index_cli>
            )
        endif()
    endif()
endif()
