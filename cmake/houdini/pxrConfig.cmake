# This is a usd configuration file for getting the usd libraries out of Houdini, as sidefx doesn't provide a pxrConfig.cmake for people who want to use
# houdini's usd build. This has been tested only on windows and with houdini 20, it probably won't work out of the box on other platforms/configurations.
# Get in touch if you need to compile usdtweak with a different houdini USD platform or version.
#
# Launch a houdini shell with hcmd.exe, then procede as usual, create a build directory, cd to to build directory
# then run the following command:
#
#     cmake -Dpxr_DIR=<USDTWEAK_DIR>\cmake\houdini ..
#
# Replace <USDTWEAK_DIR> with the directory pointing to the usdtweak project. Then run:
#
#     cmake --build . -j8 --config RelWithDebInfo -- /p:CL_MPcount=8
#
# usdtweak should now be built in the RelWithDebInfo directory

find_package(Houdini REQUIRED)

# PXR_INCLUDE_DIRS
get_property(PXR_INCLUDE_DIRS TARGET Houdini PROPERTY INTERFACE_INCLUDE_DIRECTORIES)

# PXR_LIBRARIES
# Ideally we should just look for the usd libraries houdini ships, but for now we use the ones that are already defined in HoudiniConfig.cmake
set(PXR_LIBRARIES usdUtils;ar;arch;gf;js;kind;ndr;pcp;plug;sdf;sdr;tf;trace;usd;usdGeom;usdHydra;usdLux;usdRender;usdRi;usdShade;usdSkel;usdVol;vt;work;usdImaging;cameraUtil;hd;hf;pxOsd)
foreach (lib ${PXR_LIBRARIES})
    add_library(${lib} ALIAS Houdini::Dep::pxr_${lib})
endforeach ()

# The following libraries are shipped with houdini but not configured in HoudiniConfig.cmake, we add them "manually" here as usdtweak needs them
set(EXTRA_PXR_LIBRARIES usdImagingGL;glf;hgiGL;garch;usdAppUtils)
foreach(extra_lib ${EXTRA_PXR_LIBRARIES})
    add_library(${extra_lib} SHARED IMPORTED)
    set_target_properties(${extra_lib} PROPERTIES IMPORTED_IMPLIB ${_houdini_install_root}/custom/houdini/dsolib/libpxr_${extra_lib}L.lib)
    list(APPEND PXR_LIBRARIES ${extra_lib})
endforeach()

# We add USD transitive dependencies in PXR_LIBRARIES
list(APPEND PXR_LIBRARIES Houdini::Dep::python${_houdini_python_version};Houdini::Dep::hboost_python;Houdini::Dep::tbb;Houdini::Dep::tbbmalloc)

# PXR_VERSION
find_file(pxr_h pxr/pxr.h PATHS ${PXR_INCLUDE_DIRS})
foreach (xxx MAJOR MINOR PATCH)
    file(STRINGS
        ${pxr_h}
        value
        REGEX "#define PXR_${xxx}_VERSION .*$")
    string(REGEX MATCHALL "[0-9]+" PXR_${xxx}_VERSION ${value})
endforeach ()
set(PXR_VERSION ${PXR_MAJOR_VERSION}.${PXR_MINOR_VERSION}.${PXR_PATCH_VERSION})

# We add the compilation definitions and compiler options
add_compile_options(${_houdini_compile_options})
add_compile_definitions(${_houdini_defines} -DMAKING_DSO)