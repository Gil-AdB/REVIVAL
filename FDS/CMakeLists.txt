set(PROJECT_NAME FDS)

################################################################################
# Source groups
################################################################################
set(no_group_source_files
    "Clipper.cpp"
    "Clipper.h"
    "Gradient.h"
)
source_group("" FILES ${no_group_source_files})

set(3DS
    "3DS/3DS_READ.CPP"
    "3DS/FUTILS.CPP"
    "3DS/FUtils.h"
    "3DS/I_defs.h"
#    "3DS/MATERIAL.CPP"
#    "3DS/TRACKS.CPP"
#    "3DS/WORLD.CPP"
)
source_group("3DS" FILES ${3DS})

set(Base
    "Base/BaseDefs.h"
    "Base/Camera.h"
    "Base/Color.h"
    "Base/Edge.h"
    "Base/Face.h"
    "Base/FDS_DECS.H"
    "Base/FDS_DEFS.H"
    "Base/FDS_VARS.H"
    "Base/KeyFrame.h"
    "Base/Material.h"
    "Base/Matrix.h"
    "Base/Object.h"
    "Base/Omni.h"
    "Base/Quaternion.h"
    "Base/Scene.h"
    "Base/Spline.h"
    "Base/SpotLight.h"
    "Base/TBR.h"
    "Base/Texture.cpp"
    "Base/Texture.h"
    "Base/TriMesh.h"
    "Base/Vector.h"
    "Base/Vertex.h"
    "Threads.h"
)
source_group("Base" FILES ${Base})

set(Cameras
    "CAMERAS/CAMERAS.CPP"
)
source_group("Cameras" FILES ${Cameras})

set(DPMI
    "DPMI/DPMI.CPP"
    "DPMI/DPMI.H"
)
source_group("DPMI" FILES ${DPMI})

set(FLD
    "FLD/FLD_CONV.CPP"
    "FLD/FLD_MAT.CPP"
    "FLD/FLD_READ.CPP"
    "FLD/FLD_READ.H"
    "FLD/LWREAD.H"
)
source_group("FLD" FILES ${FLD})

set(FRUSTRUM
    "FRUSTRUM.H"
    "FRUSTRUM/FRUSTRUM.CPP"
)
source_group("FRUSTRUM" FILES ${FRUSTRUM})

set(Fillers
    "FILLERS/F4Vec.cpp"
    "FILLERS/F4Vec.h"
    "FILLERS/FILLERS.CPP"
    "FILLERS/FILLERS.H"
    "FILLERS/IX.cpp"
    "FILLERS/IX.h"
    "FILLERS/IXFZ.cpp"
    "FILLERS/IXGZ.cpp"
    "FILLERS/IXTGZ.cpp"
    "FILLERS/IXTZ.cpp"
    "FILLERS/SimdHelpers.h"
    "FILLERS/TheOtherBarry.h"
        FILLERS/Mekalele.cpp
)
source_group("Fillers" FILES ${Fillers})

set(IMGCODE
    "IMGCODE/IMGCODE.CPP"
    "IMGCODE/QUANTUM.H"
    "IMGCODE/stb_image.h"
)
source_group("IMGCODE" FILES ${IMGCODE})

set(IMGGENR
    "Gradient.cpp"
    "IMGGENR/IMGGENR.CPP"
    "IMGGENR/IMGGENR.H"
)
source_group("IMGGENR" FILES ${IMGGENR})

set(IMGPROC
    "IMGPROC/Imgproc.cpp"
)
source_group("IMGPROC" FILES ${IMGPROC})

set(ISR
    "ISR/ISR.CPP"
)
source_group("ISR" FILES ${ISR})

set(MATH
    "MATH/BASEMATH.H"
    "MATH/MATH.CPP"
)
source_group("MATH" FILES ${MATH})

set(MIDAS
    "MDSPLAY/MDSPLAY.CPP"
    "MDSPLAY/MIDASDLL.H"
)
source_group("MIDAS" FILES ${MIDAS})

set(MISC
    "MISC/Memmgr.cpp"
    "MISC/mmreg.inl"
    "MISC/MOUSE.CPP"
    "MISC/PREPROC.CPP"
    "MISC/TABLES.CPP"
    "MISC/TxtrLib.cpp"
)
source_group("MISC" FILES ${MISC})

set(PCLSYS
    "PCLSYS/PCLSYS.CPP"
)
source_group("PCLSYS" FILES ${PCLSYS})

set(RADIO
    "RADIO/RADIO.CPP"
)
source_group("RADIO" FILES ${RADIO})

set(RENDER
    "RENDER/GENERAL.H"
    "RENDER/RENDER.CPP"
    "RENDER/SORTS.H"
    "RENDER/SQRTTBL.H"
)
source_group("RENDER" FILES ${RENDER})

set(SIMD
    "simd/instrset.h"
    "simd/instrset_detect.cpp"
    "simd/vector_convert.h"
    "simd/vectorclass.h"
    "simd/vectorf128.h"
    "simd/vectorf256.h"
    "simd/vectorf256e.h"
    "simd/vectorf512.h"
    "simd/vectorf512e.h"
    "simd/vectori128.h"
    "simd/vectori256.h"
    "simd/vectori256e.h"
    "simd/vectori512.h"
    "simd/vectori512e.h"
    "simd/vectori512s.h"
    "simd/vectori512se.h"
    "simd/vectormath_common.h"
    "simd/vectormath_exp.h"
    "simd/vectormath_hyp.h"
    "simd/vectormath_lib.h"
    "simd/vectormath_trig.h"
)
source_group("SIMD" FILES ${SIMD})

set(Skycube
    "SkyCube/SkyCube.cpp"
    "SkyCube/SkyCube.h"
)
source_group("Skycube" FILES ${Skycube})

set(V3D
    "V3D/M_DEFS.H"
    "V3D/OBJECTS.H"
    "V3D/V3D_READ.CPP"
)
source_group("V3D" FILES ${V3D})

set(VESA
    "VESA/FONTS.CPP"
    "VESA/VESA.CPP"
    "VESA/Vesa.h"
    "VESA/VESA_TABLES.CPP"
)
source_group("VESA" FILES ${VESA})

set(ALL_FILES
    ${no_group_source_files}
    ${3DS}
    ${Base}
    ${Cameras}
    ${DPMI}
    ${FLD}
    ${FRUSTRUM}
    ${Fillers}
    ${IMGCODE}
    ${IMGGENR}
    ${IMGPROC}
    ${ISR}
    ${MATH}
    ${MIDAS}
    ${MISC}
    ${PCLSYS}
    ${RADIO}
    ${RENDER}
    ${SIMD}
    ${Skycube}
    ${V3D}
    ${VESA}
        FILLERS/Mekalele.cpp
)

################################################################################
# Target
################################################################################
add_library("FDS" STATIC ${ALL_FILES})

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    "PORTABLE_CODE=1;"
    "_CRT_SECURE_NO_WARNINGS;"
    "_LIB;"
    "_MBCS"
    "SIMDE_ENABLE_NATIVE_ALIASES")


################################################################################
# Post build events
################################################################################
# if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
#     add_custom_command_if(
#         TARGET ${PROJECT_NAME}
#         POST_BUILD
#         COMMANDS
#         COMMAND $<CONFIG:Release> copy IXTGZ*.obj ..\\Runtime\\Fillers
#     )
# endif()

