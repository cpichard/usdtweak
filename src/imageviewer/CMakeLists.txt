target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/FileImageSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/FileImageSource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ImageBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ImageBuffer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ImageCompositor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ImageCompositor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ImageViewer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ImageViewer.h
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
