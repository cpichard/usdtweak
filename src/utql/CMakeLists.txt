
target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/UtqlTypes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Ast.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Lexer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Lexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Parser.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Binder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Binder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Executor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Executor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UtqlEngine.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UtqlEngine.cpp
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Off by default — opt in with `-DUSDTWEAK_BUILD_UTQL_TESTS=ON` at configure
# time (mirrors the Twiki test option). Tests the synchronous library path
# only, so no editor/commands sources are needed.
option(USDTWEAK_BUILD_UTQL_TESTS "Build UTQL unit tests" OFF)
if (USDTWEAK_BUILD_UTQL_TESTS)
    add_executable(test_utql_mutation
        ${CMAKE_CURRENT_SOURCE_DIR}/Lexer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Parser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Binder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Executor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestUtqlMutation.cpp
    )
    target_include_directories(test_utql_mutation PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PXR_INCLUDE_DIRS}
    )
    target_link_libraries(test_utql_mutation PRIVATE ${PXR_LIBRARIES})
    set_target_properties(test_utql_mutation PROPERTIES FOLDER "utql/tests")
    add_custom_target(run_test_utql_mutation
        COMMAND test_utql_mutation
        DEPENDS test_utql_mutation
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
endif()
