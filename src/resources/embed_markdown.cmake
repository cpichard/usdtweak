# Embed a markdown file in a cpp as a const char *.
#
# Unlike resources/embed_text.cmake (which quotes the source line by line) this
# keeps the text verbatim by wrapping it in raw string literals, so quotes and
# backslashes — both common in UTQL examples — survive untouched.
#
# The text is split into several adjacent literals: MSVC caps a single string
# literal at 16380 bytes, and adjacent literals are concatenated by the compiler.
function(embed_markdown source destination variable_name)
    file(READ ${source} text)

    # A raw string ends at the first `)DELIM"`, so the delimiter must not appear
    # in the text. `MD` is enough for markdown, but check rather than corrupt.
    if (text MATCHES "\\)MD\"")
        message(FATAL_ERROR "${source} contains the raw string delimiter )MD\" — pick another delimiter")
    endif()

    string(LENGTH "${text}" text_length)
    set(chunk_size 8000)
    set(cpp_source "")
    set(offset 0)
    while (offset LESS text_length)
        math(EXPR remaining "${text_length} - ${offset}")
        if (remaining LESS chunk_size)
            set(this_chunk ${remaining})
        else()
            set(this_chunk ${chunk_size})
        endif()
        string(SUBSTRING "${text}" ${offset} ${this_chunk} chunk)
        string(APPEND cpp_source "R\"MD(${chunk})MD\"\n")
        math(EXPR offset "${offset} + ${this_chunk}")
    endwhile()

    file(WRITE "${destination}" "// Generated from ${source} — do not edit.
const char *${variable_name} =
${cpp_source};
")
endfunction()

embed_markdown(${SOURCE_FILE} ${DESTINATION_FILE} ${VARIABLE_NAME})
