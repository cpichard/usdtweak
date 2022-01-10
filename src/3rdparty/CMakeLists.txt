# ImGui has code to compile
add_subdirectory(imgui)

target_include_directories(usdtweak PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/filesystem
    ${CMAKE_CURRENT_SOURCE_DIR}/iconfontcppheaders
    ${CMAKE_CURRENT_SOURCE_DIR}/fontawesome
    ${CMAKE_CURRENT_SOURCE_DIR}/wildcards)
