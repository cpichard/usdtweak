
target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/StringSearchIndex.h
    ${CMAKE_CURRENT_SOURCE_DIR}/StringSearchIndex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SearchWidget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SearchWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UtqlHelp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UtqlHelp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/help/UtqlHelp.md
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# help/UtqlHelp.md is the help page shown by the query panel; it is embedded in
# the binary as a const char * by src/resources (where all the embedding lives).
# It is not compiled here — listed above only so it shows up in the IDE.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/help/UtqlHelp.md
                            PROPERTIES HEADER_FILE_ONLY TRUE)
