// ParticleReplay — the GPU half of DUMP-AND-REPLAY for fountain's spray.
//
// WHY THIS EXISTS, and why it is not an ingest.
// ---------------------------------------------------------------------------
// fountain's 8,250 water sprays are NOT scene data and are NOT reachable from
// FDS. Verified by reading, not assumed:
//
//   * `Scene::Pcl` / `Scene::NumOfParticles` / `Scene::PclExec` are FDS FIELDS
//     (Base/Scene.h:38-40) and FDS transforms them (Transform.cpp:2788-2825) —
//     but NOTHING in FDS ever fills them. `LoadFLD` leaves them null: the array
//     is allocated and populated by `Initialize_Particles`
//     (DEMO/FOUNTAIN.CPP:245-444) and advanced by `Particle_Kinematics`
//     (ibid. :446), both DEMO-side. GpuBench links FDS and not DEMO.
//   * So the answer to "is the particle state reachable from FDS alone?" is
//     NO — and it is not a walk this ingest is failing to do, it is data that
//     does not exist in the process.
//   * Re-deriving it is worse than useless for an ORACLE: `Particle_Kinematics`
//     is a stateful per-frame integrator with random respawn, so reproducing it
//     means reproducing a whole RNG history. Any drift silently turns a
//     RENDERING comparison into a SIMULATION comparison, which is the one thing
//     this instrument exists to avoid.
//
// So: the CPU writes the per-frame particle state it actually rendered, and
// this arm reads it back. Identical particle positions on both sides means any
// image difference is RENDERING, not simulation. That property is the whole
// point.
//
// ---------------------------------------------------------------------------
// THE FILE FORMAT — v1, little-endian, self-describing enough to diff by hand
// ---------------------------------------------------------------------------
//
//   header
//     char     magic[8]     "FDSPCL1"        (NUL-terminated, 8 bytes)
//     uint32   version      1
//     uint32   frameCount
//     uint32   reserved[2]  0
//
//   frameCount x frame
//     uint32   magic        'PFRM' = 0x4D524650   (resync marker; a truncated
//                                                  dump is then detectable)
//     float    timer        DEMO's `Timer` (centiseconds) at this frame
//     float    curFrame     FDS `CurFrame` at this frame
//     float    imageSize    the global `ImageSize` at this frame (fountain 10.0)
//     float    perspX       `View->PerspX` — INFORMATIONAL. The replay projects
//                           with its own camera; this is here so a mismatched
//                           resolution between dump and replay is visible.
//     uint32   count        number of ACTIVE particles in this frame
//     uint32   reserved     0
//     count x particle  (20 bytes each)
//       float  px, py, pz   `Particle::V.Pos`, WORLD space
//       float  flareSize    `Particle::F.FlareSize`
//       uint8  r, g, b      `Particle::V.LR / LG / LB`, 0..255, ALREADY LIT
//       uint8  tex          which `PclT` material (0 or 1)
//
// Field choice is driven by what the CPU's sprite blitter actually consumes,
// read out of FILLERS.CPP:2324-2374 (`TBR_Render`'s sprite branch):
//     edgeLen = ImageSize * V->RZ * View->PerspX * F->FlareSize * 2
//     Color   = (V->LR << 16) | (V->LG << 8) | V->LB
//     SpriterRT<32>(V->PX, V->PY, edgeLen, edgeLen, F->Txtr->Txtr->Data, ...)
// with a reversed-Z TEST against ZPage16 and NO Z-write, compositing through
// `Spriter`'s HDRAccum path (FILLERS.CPP:1395-1409) as `h[c] += texel*color`
// into the float radiance buffer — i.e. ADDITIVE, hence order-independent
// against other sprites and needing no peel.
//
// The TEXTURE is deliberately NOT in the file: both `PclT` materials are the
// same procedurally generated 32x32 radial disc, built inline at
// FOUNTAIN.CPP:310-330 as
//     texel = (fx*fx + fy*fy >= 1) ? 0 : 0x010101 * ((1 - (fx*fx+fy*fy)) * 255)
// over fx,fy in [-1,1). This arm regenerates it from that expression, so there
// is no asset to keep in sync.
//
// ---------------------------------------------------------------------------
// THE DEMO-SIDE DUMP THIS NEEDS — SPECIFIED HERE, NOT WRITTEN
// ---------------------------------------------------------------------------
// DEMO/ is owned by other threads, so the writer is a follow-up request. It is
// small and its placement is not a matter of taste:
//
//   FILE      DEMO/FOUNTAIN.CPP
//   SITE      immediately after `Particle_Kinematics(FntSc);` in
//             `FountainScene::tick()` (currently :2732).
//   WHY THERE  `CurFrame` is already set (:2712), `Animate_Objects` has run
//             (:2726), and `Particle_Kinematics` is the last writer of
//             `V.Pos`, `V.LR/LG/LB` and `Flags` for the frame (it assigns the
//             colour at :551). Everything after it touches omnis, not
//             particles. So the state dumped there is exactly the state
//             `Transform_Objects` will project and `TBR_Render` will blit.
//   GATE      a flag, e.g. `--pcl_dump=PATH[,t0,t1]`, default off — 8,250
//             particles x 20 B x 1,500 frames is ~250 MB, so a frame RANGE
//             matters. For a pinned-pose pair one frame is enough.
//   BODY      walk `Sc->Pcl[0 .. Sc->NumOfParticles)`, skip entries whose
//             `Flags & Particle_Active` is clear (the same gate
//             Transform.cpp:2806 applies), and write the record above with
//             `ImageSize` and `View->PerspX` in the frame header.
//
// The replay side below is written against exactly that, and is testable
// WITHOUT it: `--pcl_synth=PATH` writes a conforming synthetic dump, so the
// reader, the instance build and the additive pass are all exercised today and
// the two halves meet the moment the writer exists.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gpubench {

struct PclParticle {
    float   pos[3];
    float   flareSize;
    uint8_t rgb[3];
    uint8_t tex;
};

struct PclFrame {
    float                    timer = 0.0f;
    float                    curFrame = 0.0f;
    float                    imageSize = 1.0f;
    float                    perspX = 0.0f;
    std::vector<PclParticle> particles;
};

struct PclDump {
    std::vector<PclFrame> frames;
    // Index of the frame whose curFrame is nearest `want`, or -1 when empty.
    int nearestByCurFrame(float want) const;
};

// Returns false and prints why on a bad magic / version / truncation. A
// truncated dump is reported with the frame index it died on rather than
// silently yielding a short list — a replay that quietly renders 3 particles
// instead of 8,250 would read as "the GPU is too dark", which is precisely the
// class of mistake this arm keeps paying for.
bool PclLoad(const std::string &path, PclDump &out, bool verbose);

// Write a CONFORMING synthetic dump: `n` particles on a fountain-shaped
// ballistic spray around `centre`, one frame at the given curFrame. Exists so
// the replay path is testable before the DEMO-side writer lands, and so the
// format above has an executable definition rather than only a comment.
bool PclWriteSynthetic(const std::string &path, int n, const float centre[3],
                       float curFrame, float imageSize);

// The 32x32 radial disc both PclT materials carry, regenerated from
// FOUNTAIN.CPP:310-330. RGBA8, row-major, 32*32*4 bytes.
void PclBuildSpriteTexture(std::vector<uint8_t> &rgba, int &w, int &h);

}  // namespace gpubench
