

target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ResourcesLoader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ResourcesLoader.h
)
target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# glsl to cpp
list(APPEND GLSL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/glsl/RotationManipulatorVert.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/glsl/RotationManipulatorFrag.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/glsl/PositionManipulatorVert.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/glsl/PositionManipulatorFrag.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/glsl/GridVert.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/glsl/GridFrag.glsl
)

# target_sources won't take the generated files for a target outside of the current directory
# so as a solution a glslcode library is created
add_library(glslcode STATIC "")

# Function to add a custom command to embed a glsl file in a cpp as a const char *
function(embed_glsl cpp_file glsl_file)
    add_custom_command(
        OUTPUT "${cpp_file}"
	DEPENDS "${glsl_file}"
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_FILE=${glsl_file}"
                "-DDESTINATION_FILE=${cpp_file}"
	        -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_text.cmake"
        COMMENT "Embedding ${glsl_file}"
    )
endfunction()

foreach (glsl_file ${GLSL_SOURCES})
    string(REGEX REPLACE "\\.glsl" ".cpp" cpp_file ${glsl_file})
    # Create a custom command to embed the glsl in the cpp
    embed_glsl(${cpp_file} ${glsl_file})
    # Save the variable name in a list
    get_filename_component(variable_name ${glsl_file} NAME_WE)
    string(APPEND glsl_header "extern const char *${variable_name};\n")
    # Add the generated cpp to the glslcode target
    target_sources(glslcode PRIVATE ${cpp_file})
endforeach()

# Write a header file
# TODO: add custom command/dependency etc.
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/GlslCode.h" "#pragma once\n
${glsl_header}"
)
