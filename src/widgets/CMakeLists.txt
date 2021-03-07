
target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/CompositionEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CompositionEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/FileBrowser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/FileBrowser.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LayerEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LayerEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ModalDialogs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ModalDialogs.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PrimSpecEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PrimSpecEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PropertyEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PropertyEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RendererSettings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RendererSettings.h
    ${CMAKE_CURRENT_SOURCE_DIR}/StageOutliner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StageOutliner.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Theater.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Theater.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Timeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Timeline.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ValueEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ValueEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/VariantEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VariantEditor.h
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
