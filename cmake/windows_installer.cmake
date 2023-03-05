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

# Packager
string(TIMESTAMP VERSION_MAJOR "%Y")
string(TIMESTAMP VERSION_MINOR "%m")
string(TIMESTAMP VERSION_PATCH "%d-prealpha")
set(CPACK_PACKAGE_NAME "usdtweak")
set(CPACK_PACKAGE_VENDOR "cpichard.github")
set(CPACK_PACKAGE_DESCRIPTION "USD editor")
SET(CPACK_PACKAGE_VERSION_MAJOR ${VERSION_MAJOR})
SET(CPACK_PACKAGE_VERSION_MINOR ${VERSION_MINOR})
SET(CPACK_PACKAGE_VERSION_PATCH ${VERSION_PATCH})
set(CPACK_PROJECT_HOMEPAGE_URL "https://github.com/cpichard/usdtweak")
set(CPACK_PACKAGE_ICON "${PROJECT_SOURCE_DIR}\\\\src\\\\resources\\\\package_icon.bmp")
set(CPACK_NSIS_INSTALLED_ICON_NAME "${PROJECT_SOURCE_DIR}/src/resources/app.ico")
set(CPACK_NSIS_MUI_ICON "${PROJECT_SOURCE_DIR}/src/resources/app.ico")
#set(CPACK_NSIS_MUI_WELCOMEFINISHPAGE_BITMAP "${PROJECT_SOURCE_DIR}/src/resources/test.bmp")
#set(CPACK_NSIS_MUI_HEADERIMAGE_BITMAP "${PROJECT_SOURCE_DIR}/src/resources/test.bmp")
set(CPACK_NSIS_CONTACT "cpichard.github@gmail.com")
set(CPACK_PACKAGE_EXECUTABLES "usdtweak" "UsdTweak")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
#set(CPACK_CREATE_DESKTOP_LINKS "usdtweak")

include(CPack)
