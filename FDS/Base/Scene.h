#ifndef REVIVAL_SCENE_H
#define REVIVAL_SCENE_H

#pragma pack(push, 1)

#include "TriMesh.h"
#include "SpotLight.h"
#include "Surface.h"
#include "TBR.h"
#include "Object.h"
#include "Camera.h"
#include "Omni.h"

#include <vector>

// [56 Bytes]
// Total: [(56 + 16 * Objects + (220 + 84 * Vertices + 56 * Faces + 16 *
//         (Spline + Hide Keys)) per TriMesh + 216 + (16 * Spline Keys) per
//         Camera + 200 * Omnies + 68 * SpotLights + 192 * Particles) +
//         44 * Materials + (64K * CharsperPixel * Textures) + 1Kb * Textures
//         (only if BPP==8) Bytes] = ALOT.
struct Scene
{
    // 16 Byte Aligned Members
    Color			 Ambient;			// scene's Ambient illumination

    Object         * ObjectHead;
    TriMesh        * TriMeshHead;
    Camera         * CameraHead;
    Omni           * OmniHead;
    SpotLight      * SpotLightHead;
    Surface        * Surfaces;			//<tss> oh yeah, good stuff..
    DWord            NumOfSurf;

    float            StartFrame;		// Scene start frame
    float            EndFrame;			// Scene end frame
    DWord            NumOfObjs;			// Number of objects
    struct Particle *Pcl;				// Particle Dynamic array.
    void          (* PclExec)();		// Particle Sys. Kinematics and Effects.
    DWord            NumOfParticles;	// Number of particles
    Palette        * Pal;				// Collective palette
    DWord            Flags;				// Scene flags, for optimization
    float            FZP;				// Far-Z clipping plane
    float			 NZP;				// Near-Z clipping plane
    float			 PathingMinVelocity;// at this this velocity is required for objects to change heading.

    TBREntry		*SBuffer;
    dword			 SBufferCur;
    dword			 SBufferSize;
    sdword			*SBufferHead;
    dword			 NumTiles;

    // Per-scene policy for the deferred lighting kernel. The OuterVec
    // kernel wins on scenes dominated by matte (non-spec, non-water)
    // materials because the outer 8-wide vec lighting body fires for
    // most pixels. On spec/nmap-heavy scenes (greets), pixels bail to
    // the scalar fallback and the vec body is wasted. Initialize_<scene>
    // sets this; runtime can still override with FDS_DEFERRED_OUTER_VEC.
    // Default false = use the standard kernel.
    dword            PreferOuterVec;

    // Scene's preference for the unified volumetric pass (one Beer-
    // Lambert ray-march for fog + cones + halos vs three separate
    // passes). Default 0 = legacy multi-pass. Scenes with strong fog
    // aesthetic (fountain, greets) set 1; bright-and-clear scenes
    // (city) leave 0 to preserve the 1998-era look. Runtime can
    // still override with FDS_VOLUMETRIC_UNIFIED.
    dword            PreferVolumetricUnified;

    // Transparent depth-peel passes PER SIDE for this scene (total stacked
    // layers = 2x). 0/1 = legacy single front/back peel; scenes with nested
    // glass (the fountain spire orbs) set higher so >2 overlapping transparent
    // layers stack. The CLI/env flag --xpar_peel_passes overrides this when
    // explicitly set. Read via xparPeelPassesEffective() in the xpar dispatch.
    dword            XparPeelPasses;

    // AUTHORED scene-wide env-reflection defaults, loaded from the FLD
    // scene header (LWS "FdsSceneEnvRefl" / "FdsSceneEnvBakeRes" via
    // tools/lwsread — Scene_EnvDefaults conditional payload; 0 = unset).
    // Consumed by EnvReflection_FramePrep (EnvBake.cpp):
    //   EnvReflSceneMode  -1/0/1 — scene-wide default for materials whose
    //                     per-surface EnvReflMode is 0 (auto); the
    //                     env_refl_scene_mode flag (CLI/editor) overrides
    //                     it when explicitly set, per-surface always wins.
    //   EnvBakeResScene   default probe FACE res for the scene; sits below
    //                     an explicit --env-bake-res and per-surface
    //                     EnvBakeRes, above the legacy env_refl_res sizing.
    sdword           EnvReflSceneMode;
    sdword           EnvBakeResScene;

    // Authored backdrop gradient (LWS ZenithColor/SkyColor/GroundColor/
    // NadirColor → FLD Scene_SkyColors payload → here). Painted into
    // void/sky pixels top→bottom by the default-off sky_gradient pass
    // (Render_SkyGradient). HasSkyGradient == 0 → unset (legacy black void).
    // Stored as QColor (BGRA byte) — the four vertical stops, zenith at the
    // top of the frame, nadir at the bottom.
    sdword           HasSkyGradient;
    QColor           SkyZenith;   // straight up  (top of frame)
    QColor           SkySky;      // upper sky
    QColor           SkyGround;   // lower / toward horizon-ground
    QColor           SkyNadir;    // straight down (bottom of frame)

    // Static-shadow lightmap table populated by LightmapBake_Static.
    // Index 0 is reserved sentinel (nullptr) so Mekalele's per-pixel
    // staticLMMeshId == 0 means "no lightmap for this pixel". Indices
    // 1..N correspond to TriMeshes whose Pos/Rotate splines are flat
    // through the parent chain. The deferred kernel uses this to look
    // up the lightmap for the pixel's owning mesh.
    //
    // Pointer (not in-place vector) so Scene remains a trivially-zeroable
    // struct — Snapshot.cpp memset()s fresh Scenes before the loader
    // fills them in.
    std::vector<struct TriMesh*> *staticLMTable = nullptr;

};

#pragma pack(pop)

#endif //REVIVAL_SCENE_H
