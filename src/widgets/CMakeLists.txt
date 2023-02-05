
target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/TextEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TextEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CompositionEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CompositionEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectionEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectionEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/EditListSelector.h
    ${CMAKE_CURRENT_SOURCE_DIR}/FileBrowser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/FileBrowser.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LauncherBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LauncherBar.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TableLayouts.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfLayerEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfLayerEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfLayerSceneGraphEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfLayerSceneGraphEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ModalDialogs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ModalDialogs.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfPrimEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfPrimEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdPrimEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdPrimEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfAttributeEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfAttributeEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/StageOutliner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StageOutliner.h
    ${CMAKE_CURRENT_SOURCE_DIR}/StageLayerEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/StageLayerEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ContentBrowser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ContentBrowser.h
    ${CMAKE_CURRENT_SOURCE_DIR}/VtArrayEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/VtArrayEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TextFilter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TextFilter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Timeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Timeline.h
    ${CMAKE_CURRENT_SOURCE_DIR}/VtValueEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VtValueEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/VtDictionaryEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VtDictionaryEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/VariantEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VariantEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TfTokenLabel.h
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
