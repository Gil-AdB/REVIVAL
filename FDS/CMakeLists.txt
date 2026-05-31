set(PROJECT_NAME FDS)

################################################################################
# Source groups (kept explicit; globbing is a later cleanup)
################################################################################
set(no_group_source_files
    Clipper.cpp
    Clipper.h
    Gradient.h)
source_group("" FILES ${no_group_source_files})

set(3DS
    3DS/3DS_READ.CPP
    3DS/FUTILS.CPP
    3DS/FUtils.h
    3DS/I_defs.h)
source_group("3DS" FILES ${3DS})

set(Base
    Base/BaseDefs.h
    Base/Camera.h
    Base/Color.h
    Base/Edge.h
    Base/Face.h
    Base/FDS_DECS.H
    Base/FDS_DEFS.H
    Base/FDS_VARS.H
    Base/CameraContext.h
    Base/FaceListContext.h
    Base/FeatureFlags.cpp
    Base/FeatureFlags.def
    Base/FeatureFlags.h
    Base/FPContract.h
    Base/FrameState.cpp
    Base/FrameState.h
    Base/RenderStats.cpp
    Base/RenderStats.h
    Base/RenderTarget.h
    Base/KeyFrame.h
    Base/Material.h
    Base/Matrix.h
    Base/Object.h
    Base/Omni.h
    Base/Quaternion.h
    Base/Scene.h
    Base/Spline.h
    Base/SpotLight.h
    Base/TBR.h
    Base/Texture.cpp
    Base/Texture.h
    Base/TriMesh.h
    Base/Vector.h
    Base/VertexFrame.cpp
    Base/VertexFrame.h
    Base/VertexScratch.cpp
    Base/VertexScratch.h
    Base/Vertex.h
    Threads.h)
source_group("Base" FILES ${Base})

set(Cameras CAMERAS/CAMERAS.CPP)
source_group("Cameras" FILES ${Cameras})

set(DPMI DPMI/DPMI.CPP)
source_group("DPMI" FILES ${DPMI})

set(ISR ISR/ISR.CPP)
source_group("ISR" FILES ${ISR})

set(FLD
    FLD/FLD_CONV.CPP
    FLD/FLD_MAT.CPP
    FLD/FLD_READ.CPP
    FLD/FLD_READ.H
    FLD/LWREAD.H)
source_group("FLD" FILES ${FLD})

set(FRUSTRUM FRUSTRUM.H FRUSTRUM/FRUSTRUM.CPP)
source_group("FRUSTRUM" FILES ${FRUSTRUM})

set(Fillers
    FILLERS/F4Vec.cpp
    FILLERS/F4Vec.h
    FILLERS/FILLERS.CPP
    FILLERS/FILLERS.H
    FILLERS/Mekalele.cpp
    FILLERS/RasterizerInst.cpp
    FILLERS/ShadowMap.cpp
    FILLERS/ShadowMap.h
    FILLERS/SimdHelpers.h
    FILLERS/TheOtherBarry.h)
source_group("Fillers" FILES ${Fillers})

set(IMGCODE
    IMGCODE/IMGCODE.CPP
    IMGCODE/QUANTUM.H
    IMGCODE/stb_image.h)
source_group("IMGCODE" FILES ${IMGCODE})

set(IMGGENR
    Gradient.cpp
    IMGGENR/IMGGENR.CPP
    IMGGENR/IMGGENR.H)
source_group("IMGGENR" FILES ${IMGGENR})

set(IMGPROC IMGPROC/Imgproc.cpp)
source_group("IMGPROC" FILES ${IMGPROC})

set(MATH MATH/BASEMATH.H MATH/MATH.CPP)
source_group("MATH" FILES ${MATH})

set(MISC
    MISC/Memmgr.cpp
    MISC/mmreg.inl
    MISC/PREPROC.CPP
    MISC/TABLES.CPP
    MISC/TxtrLib.cpp)
source_group("MISC" FILES ${MISC})

set(RADIO RADIO/RADIO.CPP)
source_group("RADIO" FILES ${RADIO})

set(RENDER
    RENDER/DeferredLighting.cpp
    RENDER/GENERAL.H
    RENDER/Lighting.cpp
    RENDER/LightmapBake.cpp
    RENDER/LightmapBake.h
    RENDER/RENDER.CPP
    RENDER/RenderInner.cpp
    RENDER/RenderPipeline.h
    RENDER/Shadows.cpp
    RENDER/SORTS.H
    RENDER/SQRTTBL.H
    RENDER/Transform.cpp)
source_group("RENDER" FILES ${RENDER})

set(SIMD
    simd/instrset.h
    simd/instrset_detect.cpp
    simd/vector_convert.h
    simd/vectorclass.h
    simd/vectorf128.h
    simd/vectorf256.h
    simd/vectorf256e.h
    simd/vectorf512.h
    simd/vectorf512e.h
    simd/vectori128.h
    simd/vectori256.h
    simd/vectori256e.h
    simd/vectori512.h
    simd/vectori512e.h
    simd/vectori512s.h
    simd/vectori512se.h
    simd/vectormath_common.h
    simd/vectormath_exp.h
    simd/vectormath_hyp.h
    simd/vectormath_lib.h
    simd/vectormath_trig.h)
source_group("SIMD" FILES ${SIMD})

set(Skycube SkyCube/SkyCube.cpp SkyCube/SkyCube.h)
source_group("Skycube" FILES ${Skycube})

set(V3D V3D/M_DEFS.H V3D/OBJECTS.H V3D/V3D_READ.CPP)
source_group("V3D" FILES ${V3D})

set(VESA
    VESA/FONTS.CPP
    VESA/VESA.CPP
    VESA/Vesa.h
    VESA/VESA_TABLES.CPP)
source_group("VESA" FILES ${VESA})

set(ALL_FILES
    ${no_group_source_files}
    ${3DS}
    ${Base}
    ${Cameras}
    ${DPMI}
    ${ISR}
    ${FLD}
    ${FRUSTRUM}
    ${Fillers}
    ${IMGCODE}
    ${IMGGENR}
    ${IMGPROC}
    ${MATH}
    ${MISC}
    ${RADIO}
    ${RENDER}
    ${SIMD}
    ${Skycube}
    ${V3D}
    ${VESA})

################################################################################
# Target
################################################################################
add_library(${PROJECT_NAME} STATIC ${ALL_FILES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    _CRT_SECURE_NO_WARNINGS
    _LIB
    _MBCS
    SIMDE_ENABLE_NATIVE_ALIASES)

# Targeted bug-catcher warnings — promoted to errors on the bug class
# we hit during phase 6 (self-assign in a mechanical sed pass turning
# `x = y` into `x = x`, leaving x uninitialized; missing return in a
# control-flow branch). Project-wide `-Wall` would surface ~90
# mostly-cosmetic warnings (unused-var, missing-braces, mismatched-tags)
# that we don't have appetite to triage yet — punt that to a follow-up.
# `-Wno-parentheses` silences ~200 vendored-simd warnings (the
# `& has lower precedence than >` shape inside vectorclass) that we
# don't own; without this the bug-catcher warnings drown in noise.
target_compile_options(${PROJECT_NAME} PUBLIC
    -Wself-assign
    -Wuninitialized
    -Wsometimes-uninitialized
    -Werror=return-type
    -Wno-parentheses)

# -ffp-contract=fast is enabled GLOBALLY at the top-level CMakeLists.
# No per-file pinning needed. FDS/Base/FPContract.h provides
# block-scoped opt-out macros (FP_CONTRACT_OFF) for any future code
# that's genuinely FMA-sensitive — but no such site exists today
# (the previously suspected rasterizer issue traced to a NaN bug in
# TheOtherBarry's fog code, fixed at the source).

if(EMSCRIPTEN)
    # std::thread / mutex / condition_variable need -pthread at compile time
    # for the PROXY_TO_PTHREAD mode we use in DEMO.
    target_compile_options(${PROJECT_NAME} PUBLIC -pthread)
    # getenv() is unreliable in the browser, so bake the experimental
    # rendering toggles ON at compile time for WASM. The env var still
    # wins when set (e.g. when running DEMO_snapshot under node, which
    # has a real process environment), so FDS_DEFERRED=0 / FDS_SHADOWS=0
    # can still disable them for snapshot comparisons.
    target_compile_definitions(${PROJECT_NAME} PUBLIC
        FDS_DEFERRED_DEFAULT_ON=1
        FDS_SHADOWS_DEFAULT_ON=1
        FDS_SHADOW_POLYID_DEFAULT_ON=1)
endif()

# Compile-time-baked CLI args. Settable from the configure-time environment
# (`FDS_BAKED_ARGS="--deferred --shadows ..." cmake ...`) or as a CMake cache
# variable (`-DFDS_BAKED_ARGS="..."`). The string runs through the normal
# parseArgs path at startup (see FeatureFlags.cpp state() initializer), so
# anything valid on the CLI works here — bools, --no-foo negations, and
# --flag=value for floats/ints. Mainly for wasm builds where argv and getenv
# aren't routinely available, but works on native too. Runtime parseArgs
# fires AFTER the baked args, so command-line flags still override.
if(NOT DEFINED FDS_BAKED_ARGS AND DEFINED ENV{FDS_BAKED_ARGS})
    set(FDS_BAKED_ARGS "$ENV{FDS_BAKED_ARGS}" CACHE STRING
        "CLI args baked into the build (see FDS/CMakeLists.txt)")
else()
    set(FDS_BAKED_ARGS "${FDS_BAKED_ARGS}" CACHE STRING
        "CLI args baked into the build (see FDS/CMakeLists.txt)")
endif()
if(FDS_BAKED_ARGS)
    message(STATUS "FDS_BAKED_ARGS = ${FDS_BAKED_ARGS}")
    target_compile_definitions(${PROJECT_NAME} PUBLIC
        "FDS_BAKED_ARGS=\"${FDS_BAKED_ARGS}\"")
endif()
