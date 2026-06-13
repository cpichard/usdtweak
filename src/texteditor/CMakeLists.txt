target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/SpanTree.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SpanTree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdaWriter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdaWriter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdaLexer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdaLexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdaParsed.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdaParser.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdaParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SpecDiff.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SpecDiff.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TextDocument.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TextDocument.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TextEditorManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TextEditorManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TextEditorStyle.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TextEditorWidget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TextEditorWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TextEditorWindow.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TextEditorWindow.cpp
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
