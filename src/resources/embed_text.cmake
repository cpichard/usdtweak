
# Basic function that reads the lines of a source and enclose them in double quotes
# after creating a const char variable having the name of the source file
function(embed_text source destination)
    file(STRINGS ${source} text)
    foreach (line ${text})
        string(APPEND cpp_source "\"${line}\\n\"\n" )
    endforeach()
    get_filename_component(variable_name ${source} NAME_WE)
    file(WRITE "${destination}" "\ 
const char *${variable_name} = 
${cpp_source};
")
endfunction()

embed_text(${SOURCE_FILE} ${DESTINATION_FILE})
