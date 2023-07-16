# macOS mini installer
# The following command will create an unsigned DMG package
#    cmake --build . --config RelWithDebInfo --target package
#

# We install the usd libraries in the lib folder
set_target_properties(usdtweak PROPERTIES
  INSTALL_RPATH @executable_path/../lib
)
install(TARGETS usdtweak DESTINATION .)
# RUNTIME_ARTIFACTS is not working properly or I don't understand what it is meant to do
#if(APPLE AND ${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.21.0")
#    install(IMPORTED_RUNTIME_ARTIFACTS ${PXR_LIBRARIES} 
#        DESTINATION usdtweak.app/Contents/lib
# so we just copy the usd libs
file(GLOB USD_DLL LIST_DIRECTORIES false ${PXR_CMAKE_DIR}/lib/*.dylib ${PXR_CMAKE_DIR}/bin/*.dylib)
#install(FILES ${USD_DLL} DESTINATION usdtweak.app/Contents/lib CONFIGURATIONS RelWithDebInfo)
install(FILES ${USD_DLL} DESTINATION usdtweak.app/Contents/lib)

# Copy the plugin folder, lib/python and lib/usd as they are necessary for usd to run properly
install(DIRECTORY ${PXR_CMAKE_DIR}/plugin DESTINATION usdtweak.app/Contents PATTERN "*.pdb" EXCLUDE)
install(DIRECTORY ${PXR_CMAKE_DIR}/lib/python DESTINATION usdtweak.app/Contents/lib PATTERN "*.pyc" EXCLUDE)
install(DIRECTORY ${PXR_CMAKE_DIR}/lib/usd DESTINATION usdtweak.app/Contents/lib)
set(CPACK_GENERATOR DragNDrop)
set(CPACK_COMPONENTS_GROUPING ONE_PER_GROUP)
include(CPack)

