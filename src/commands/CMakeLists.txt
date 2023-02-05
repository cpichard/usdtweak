
target_sources(usdtweak PRIVATE
#    ${CMAKE_CURRENT_SOURCE_DIR}/Commands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Commands.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandsImpl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandsImpl.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandStack.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandStack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Shortcuts.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfCommandGroup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfCommandGroup.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfLayerInstructions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfLayerInstructions.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfCommandGroupRecorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfCommandGroupRecorder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UndoLayerStateDelegate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UndoLayerStateDelegate.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PrimCommands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AttributeCommands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LayerCommands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorCommands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfUndoRedoRecorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfUndoRedoRecorder.h
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

