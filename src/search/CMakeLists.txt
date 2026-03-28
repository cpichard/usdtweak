
target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/StringSearchIndex.h
    ${CMAKE_CURRENT_SOURCE_DIR}/StringSearchIndex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SearchWidget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SearchWidget.cpp
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
