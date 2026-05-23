# Linux mini installer
# The following command will create an unsigned tar.gz package
#    cmake --build . --config RelWithDebInfo --target package
#
# We install the usd libraries in the lib folder next to the binary in bin/.
# The RPATH below makes the binary find them at runtime without LD_LIBRARY_PATH.

set_target_properties(usdtweak PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib"
)

install(TARGETS usdtweak RUNTIME DESTINATION bin)

# Copy the usd shared libraries. USD on linux installs .so files under lib/.
file(GLOB USD_SO LIST_DIRECTORIES false
    ${PXR_CMAKE_DIR}/lib/*.so
    ${PXR_CMAKE_DIR}/lib/*.so.*
    ${PXR_CMAKE_DIR}/lib64/*.so
    ${PXR_CMAKE_DIR}/lib64/*.so.*
)
install(FILES ${USD_SO} DESTINATION lib)

# Copy the plugin folder, lib/python and lib/usd as they are necessary for usd to run properly
install(DIRECTORY ${PXR_CMAKE_DIR}/plugin DESTINATION .)
if(EXISTS ${PXR_CMAKE_DIR}/lib/python)
    install(DIRECTORY ${PXR_CMAKE_DIR}/lib/python DESTINATION lib PATTERN "*.pyc" EXCLUDE)
endif()
if(EXISTS ${PXR_CMAKE_DIR}/lib/usd)
    install(DIRECTORY ${PXR_CMAKE_DIR}/lib/usd DESTINATION lib)
endif()

# Packager
string(TIMESTAMP VERSION_MAJOR "%Y")
string(TIMESTAMP VERSION_MINOR "%m")
string(TIMESTAMP VERSION_PATCH "%d-prealpha")
set(CPACK_PACKAGE_NAME "usdtweak")
set(CPACK_PACKAGE_VENDOR "cpichard.github")
set(CPACK_PACKAGE_DESCRIPTION "USD editor")
set(CPACK_PACKAGE_VERSION_MAJOR ${VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${VERSION_PATCH})
set(CPACK_PROJECT_HOMEPAGE_URL "https://github.com/cpichard/usdtweak")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR TGZ)
set(CPACK_SYSTEM_NAME "linux-x86_64")

include(CPack)
