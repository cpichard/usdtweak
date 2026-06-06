
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
