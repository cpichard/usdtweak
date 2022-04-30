#
# Installation of usdtweak and its dependencies on windows
#
# This is provided as an example and might not work on your configuration, but this code attempts to 
# install the compiled version of usdtweak, the usd libraries and all their dependencies in a directory
# such that the application is self contained and does not rely on other libraries installed on the system.
# This should ultimately make this installation portable.
#
# To run the installer, use the following command in a shell and in the build directory, (the directory
# containing CMakeCache.txt)
# cmake --install . --prefix <where_you_want_to_install_usdtweak> --config RelWithDebInfo

# Search for python path on windows (this is expecting the "where" command is installed)
# this might not work if you don't have python install and compiled usd without python
execute_process(COMMAND where python OUTPUT_VARIABLE python_out)
string(REGEX REPLACE "\n" ";" python_out "${python_out}")
list(GET python_out 0 python_first_location) # get the first of the list
get_filename_component(PYTHON_DLL_DIR ${python_first_location} DIRECTORY)
message(STATUS "Assuming python dir is ${PYTHON_DLL_DIR}, please double check")

# Add the usd and python dlls to the install
file(GLOB USD_DLLS ${PXR_CMAKE_DIR}/lib/*.dll ${PXR_CMAKE_DIR}/bin/*.dll ${PYTHON_DLL_DIR}/*.dll)
install(FILES ${USD_DLLS} DESTINATION bin CONFIGURATIONS RelWithDebInfo)
# Copy the plugin, lib/python and lib/usd as they seem necessary for usd to run properly
install(DIRECTORY ${PXR_CMAKE_DIR}/plugin DESTINATION . PATTERN "*.pdb" EXCLUDE)
install(DIRECTORY ${PXR_CMAKE_DIR}/lib/python DESTINATION bin PATTERN "*.pyc" EXCLUDE)
install(DIRECTORY ${PXR_CMAKE_DIR}/lib/usd DESTINATION bin)
install(TARGETS usdtweak)
