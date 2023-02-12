 
# Start with the git revision
find_package(Git)
if(GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
      WORKING_DIRECTORY "${SRC_DIR}"
      OUTPUT_VARIABLE GIT_HASH 
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message( STATUS "usdtweak revision: ${GIT_HASH}")
else()
    message(WARNING "git was not found")
    set(GIT_HASH "unknown revision")
endif()

# Get the current time
string(TIMESTAMP BUILD_DATE "%Y.%m.%d")
configure_file(${SRC_DIR}/cmake/stamp.h.in ${DST_DIR}/Stamp.h @ONLY)
 
 
