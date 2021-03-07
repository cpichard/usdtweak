
target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Commands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Commands.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfCommandGroup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfCommandGroup.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfLayerInstructions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfLayerInstructions.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfUndoRecorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SdfUndoRecorder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UndoLayerStateDelegate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UndoLayerStateDelegate.h
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

