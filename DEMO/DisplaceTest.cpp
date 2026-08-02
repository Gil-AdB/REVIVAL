// DisplaceTest.cpp — the SIMPLEST possible rig for the stone-displacement bake
// (docs/ENVDYN_DISPLACEMENT_PLAN.md workstream B; the machinery in
// DEMO/MeshOps.cpp DisplaceStoneSubdiv + EstimateBlockPitch). ONE flat quad,
// ONE material with an 8-bit height map, and the EXACT production bake greets
// runs on its 'rooms' wall — so "is the subdivision honouring the height map?"
// becomes a clear, analytically-checkable question instead of an argument about
// a 100-block multi-tile wall buried in a full scene.
//
// The height map is selectable (FDS_DISPLACETEST_MAP):
//   0 = SYNTHETIC BLOCKS (default): a 4×4 regular block grid per UV tile, sharp
//       32-texel mortar grooves (interior 230, mortar 30). The ground truth is
//       unambiguous: flat raised plateaus, straight recessed grooves, step
//       transitions exactly on the mortar lines. Edge-on it MUST be a square
//       wave.
//   1 = SYNTHETIC RAMP: a linear gradient along U. A pure plane carries zero
//       refinement error at level 0, so the adaptive bake must stay COARSE here
//       (the "don't over-subdivide smooth slopes" check).
//   2 = the REAL wall map (Runtime/TEXTURES/greets_wall_h.png) via the same
//       loader greets uses.
//   3 = SYNTHETIC RUNNING BOND: map 0 with alternate block-rows offset half a
//       block (8 distinct vertical mortar positions per tile, T-intersections)
//       — the closest synthetic analog of the real running-bond greets wall.
//
// FDS_DISPLACETEST_SPAN=N (default 1) tiles the SAME map across N UV tiles per
// axis on the one quad, so the map is exercised at N*4 blocks/axis (span 3 =
// 12 blocks/axis ≈ the real greets wall's multi-tile quads — the regime that
// broke the gray wall while single-tile reasoning "passed"). The bake's block-
// pitch machinery must land cells at block scale regardless of span.
//
// The bake reuses the greets flags VERBATIM (--greets_displace_amp/_mip/_adapt/
// _cpb, --greets_stone_subdiv) so tuning here transfers, except amp defaults up
// (0.8) so the relief reads on the 8-unit test wall. Nothing else is on: no
// shadows, no POM, no HDR, forward Gouraud — only the geometry the bake built.
//
// Default: interactive free-cam (WASD/QE move, arrows look, G dumps the pose,
// ESC/Backspace exit); --displace_viz=1/2 draws the bake overlay live.
// FDS_DISPLACETEST_DUMP=1: headless — prints the [DTEST] metric matrix over
// (map, span) combos, then renders the selected combo from 4 poses (frontal,
// 45°, grazing, edge-on SILHOUETTE) × 3 styles (lit, viz-1 wireframe, viz-2
// error) to /tmp/displacetest_<pose>_<style>.ppm and exits.
//
//   ./DEMO --scene-displacetest                       (interactive, map 0)
//   FDS_DISPLACETEST_DUMP=1 ./DEMO --scene-displacetest
//   FDS_DISPLACETEST_MAP=3 FDS_DISPLACETEST_SPAN=3 FDS_DISPLACETEST_DUMP=1 \
//       ./DEMO --scene-displacetest

#include "Rev.h"
#include "MeshOps.h"          // DisplaceStoneSubdiv, EstimateBlockPitch, MakeHeight8
#include "SceneBuilder.h"

#include <RENDER/WorldAabb.h>  // DisplaceViz_DrawOverlay
#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FDS_DEFS.H>     // DEFAULT_BLOCKSIZEX/Y
#include <Base/FeatureFlags.h>
#include <Base/Material.h>
#include <Base/Object.h>
#include <Base/Texture.h>
#include <Base/TriMesh.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern void Scene_RebuildMatTable(Scene *Sc);

namespace {

using FF = fds::FeatureFlags;

constexpr int kMapSize = 1024;   // match the real greets_wall_h.png scale, so
                                 // greets_displace_mip transfers 1:1 (mip 2 →
                                 // 256² working res on both synthetic + real).
constexpr int kBlocks  = 4;      // blocks per UV tile (real map = 4×4 / tile)
constexpr int kMortar  = 32;     // mortar groove width, texels (1/8 of a block)
constexpr uint8_t kInterior = 230, kGroove = 30;

const char *mapName(int m) {
    switch (m) { case 0: return "blocks"; case 1: return "ramp";
                 case 2: return "real";   case 3: return "runbond"; default: return "?"; }
}

// ── synthetic height fields (linear, 8-bit, one UV tile) ────────────────────
void genBlocks(std::vector<uint8_t> &g) {
    const int P = kMapSize / kBlocks;                 // block pitch, texels
    for (int y = 0; y < kMapSize; ++y)
        for (int x = 0; x < kMapSize; ++x) {
            const bool mx = (x % P) < kMortar;
            const bool my = (y % P) < kMortar;
            g[size_t(y) * kMapSize + x] = (mx || my) ? kGroove : kInterior;
        }
}
void genRamp(std::vector<uint8_t> &g) {
    // Symmetric TRIANGLE wave along x — piecewise-linear (a "ramp" up then down)
    // that is CONTINUOUS across the tile wrap, so it's a fair "smooth slope"
    // test. (A plain linear ramp is a SAWTOOTH once tiled — discontinuous at the
    // u=1 seam — which correctly forces the bake to subdivide at the seam; that's
    // a sampler-tiling artifact, not a smooth surface, so it's the wrong control.)
    for (int y = 0; y < kMapSize; ++y)
        for (int x = 0; x < kMapSize; ++x) {
            const float t = float(x) / float(kMapSize);       // 0..1
            const float tri = 1.0f - std::fabs(2.0f * t - 1.0f);  // 0→1→0
            g[size_t(y) * kMapSize + x] =
                uint8_t(kGroove + tri * (kInterior - kGroove) + 0.5f);
        }
}
void genRunningBond(std::vector<uint8_t> &g) {
    const int P = kMapSize / kBlocks;
    for (int y = 0; y < kMapSize; ++y) {
        const int blockRow = y / P;
        const int phase = (blockRow & 1) ? P / 2 : 0;  // half-block offset
        for (int x = 0; x < kMapSize; ++x) {
            const bool mx = ((x + phase) % P) < kMortar;
            const bool my = (y % P) < kMortar;
            g[size_t(y) * kMapSize + x] = (mx || my) ? kGroove : kInterior;
        }
    }
}

// Wrap a linear 8-bit height grid into a 32-bit tiled+mipmapped Texture with
// the SAME layout greets' GreetsLoadFullTexture produces (block-tiled via
// Generate_Mipmaps), then MakeHeight8 → the 8-bit HeightMap the bake wants.
// Bypasses Scene_MakeTiledTexture (which clamps to 256²) so the synthetic maps
// keep the real map's 1024² scale.
Texture *makeSyntheticHeight8(const std::vector<uint8_t> &grid) {
    Texture *t = new Texture;
    std::memset(t, 0, sizeof(Texture));
    t->BPP   = 32;
    t->SizeX = kMapSize; t->SizeY = kMapSize;
    t->LSizeX = 10; t->LSizeY = 10;               // log2(1024)
    t->OptClass = 0;
    dword *px = (dword *)getAlignedBlock(size_t(kMapSize) * kMapSize * 4);
    for (size_t i = 0; i < grid.size(); ++i) {
        const uint32_t h = grid[i];
        px[i] = 0xFF000000u | (h << 16) | (h << 8) | h;   // grayscale, low byte = h
    }
    t->Data = (byte *)px;
    t->Flags |= Txtr_Tiled;
    Generate_Mipmaps(t, DEFAULT_BLOCKSIZEX, DEFAULT_BLOCKSIZEY, 1);  // steals + retiles Data
    return MakeHeight8(t);
}

// Real map — exactly greets' GreetsLoadFullTexture path, then MakeHeight8.
Texture *makeRealHeight8(const char *path) {
    Texture *t = new Texture;
    std::memset(t, 0, sizeof(Texture));
    t->FileName = strdup(path);
    t->BPP = 0;
    if (!Load_Texture(t)) {
        std::fprintf(stderr, "[DTEST] cannot load real map %s\n", path);
        delete t; return nullptr;
    }
    if (t->BPP != 32) BPPConvert_Texture(t, 32);
    t->Flags |= Txtr_Tiled;
    Generate_Mipmaps(t, DEFAULT_BLOCKSIZEX, DEFAULT_BLOCKSIZEY, 1);
    return MakeHeight8(t);
}

Texture *makeHeight8(int mapId) {
    if (mapId == 2) return makeRealHeight8("TEXTURES/greets_wall_h.png");
    std::vector<uint8_t> grid(size_t(kMapSize) * kMapSize);
    switch (mapId) {
        case 1: genRamp(grid); break;
        case 3: genRunningBond(grid); break;
        default: genBlocks(grid); break;
    }
    return makeSyntheticHeight8(grid);
}

// mip0 low/hi/mean of an 8-bit map (raw byte scan over the mip0 block — swizzle
// order is irrelevant to the multiset).
void mapStats01(const Texture *hm, float &lo01, float &hi01, float &mean01) {
    const size_t n = size_t(hm->SizeX) * size_t(hm->SizeY);
    const byte *d = hm->Mipmap[0];
    byte lo = 255, hi = 0; uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) { byte b = d[i]; if (b < lo) lo = b; if (b > hi) hi = b; sum += b; }
    lo01 = lo / 255.0f; hi01 = hi / 255.0f; mean01 = double(sum) / double(n) / 255.0;
}

// ── the scene ───────────────────────────────────────────────────────────────
struct DTestScene {
    Scene   *sc   = nullptr;
    TriMesh *wall = nullptr;   // the 'dtest' quad, post-bake
    Texture *hm   = nullptr;   // the 8-bit height map on 'dtest'
};

// Stamp UVs 0..span on the (pre-bake) 2-triangle quad, both per-vertex and
// per-face (the bake reads per-FACE UVs).
void stampSpan(TriMesh *T, float span) {
    for (int i = 0; i < T->VIndex; ++i) { T->Verts[i].U *= span; T->Verts[i].V *= span; }
    for (int i = 0; i < T->FIndex; ++i) {
        Face &F = T->Faces[i];
        F.U1 = F.A->U; F.V1 = F.A->V;
        F.U2 = F.B->U; F.V2 = F.B->V;
        F.U3 = F.C->U; F.V3 = F.C->V;
    }
}

// Build the one-wall scene for (mapId, span), run the production bake on 'dtest'.
// vizMode gates what the bake RECORDS (2 = both magnitude + error fields).
DTestScene build(int mapId, float span, int vizMode) {
    using namespace fds::scene_builder;
    // The bake's viz recorders read this at bake time; set before the bake.
    FF::setDefault(FF::IntId::displace_viz, vizMode);

    SceneBuilder b;
    b.SetNearFar(0.5f, 300.0f);
    b.SetAmbient(60, 60, 60);

    Texture *hm = makeHeight8(mapId);

    // 'dtest': neutral mid-gray so LIT shading reads the GEOMETRY, not a texture.
    Texture *tex = b.AddSolidColorTexture(8, 8, 0xFFB0B0B0u);
    Material *mat = b.AddMaterial("dtest", tex, {176, 176, 176, 255}, 0);
    mat->HeightMap = hm;   // the bake displaces along this

    // The wall: 8×8, in the z=0 plane, normal facing -z (toward the camera + key
    // light on the -z side). Winding (4,0,0)(-4,0,0)(-4,8,0)(4,8,0) → N=(0,0,-1);
    // AddQuad seeds UVs 0..1, then stampSpan scales to 0..span.
    const Vector wallV[4] = {
        Vector( 4.0f, 0.0f, 0.0f), Vector(-4.0f, 0.0f, 0.0f),
        Vector(-4.0f, 8.0f, 0.0f), Vector( 4.0f, 8.0f, 0.0f),
    };
    TriMesh *wall = b.AddQuad("dtest_wall", wallV, mat);
    stampSpan(wall, span);

    // Dim floor for orientation (own material name so the bake never touches it).
    Texture *ftex = b.AddSolidColorTexture(8, 8, 0xFF404040u);
    Material *fmat = b.AddMaterial("dtestfloor", ftex, {64, 64, 64, 255}, 0);
    const Vector floorV[4] = {
        Vector(-20.0f, 0.0f, -20.0f), Vector(-20.0f, 0.0f, 40.0f),
        Vector( 20.0f, 0.0f,  40.0f), Vector( 20.0f, 0.0f, -20.0f),
    };
    b.AddQuad("dtest_floor", floorV, fmat);

    // ONE omni, placed high + off to the +x side so it GRAZES the wall face:
    // groove side-walls then catch/lose light and read as lines in the lit view
    // (a head-on light on shallow relief washes flat). Ambient lifts the grooves
    // off pure black so the geometry, not a shadow, is what's read.
    b.AddOmni(Vector(11.0f, 9.0f, -6.0f), {255, 255, 255, 0}, 1.6f, 80.0f);
    b.SetCamera(Vector(0.0f, 4.0f, -13.0f), Vector(0.0f, 4.0f, 0.0f), 55.0f);
    b.Finalize();

    DTestScene d; d.sc = b.scene(); d.wall = wall; d.hm = hm;

    // ── the ACTUAL production bake (same call greets makes on 'rooms') ──
    const int   L     = FF::greets_stone_subdiv();
    const float amp   = FF::greets_displace_amp();
    const int   mip   = FF::greets_displace_mip();
    const float adapt = FF::greets_displace_adapt();
    const float cpb   = FF::greets_displace_cpb();
    DisplaceStoneSubdiv(d.sc, "dtest", L, amp, mip, adapt, cpb);
    // Facet the relief (per-face normals) so the lit view reads the ACTUAL
    // subdivided geometry — each cell shows its true plane, grooves pop. This is
    // greets' first post-bake pass. Greets then runs DisplaceStoneSmoothNormals
    // to round the relief for the final look; here that's OFF by default (it
    // washes the geometry we want to SEE) — opt in with FDS_DISPLACETEST_SMOOTH=1
    // to preview the production shading.
    MakeFacesIndependentByAngle(d.sc, 30.0f);
    if (std::getenv("FDS_DISPLACETEST_SMOOTH"))
        DisplaceStoneSmoothNormals(d.sc, "dtest", FF::greets_displace_smooth());

    Scene_RebuildMatTable(d.sc);
    return d;
}

// ── [DTEST] metrics — the pass/fail essence, computed from the baked mesh ────
void reportMetrics(const DTestScene &d, int mapId, float span) {
    const float amp = FF::greets_displace_amp();
    const int   mip = FF::greets_displace_mip();

    float px0 = 0, py0 = 0, pxm = 0, pym = 0;
    const bool have0 = EstimateBlockPitch(d.hm, 0, px0, py0);
    const bool havem = EstimateBlockPitch(d.hm, std::min(mip, int(d.hm->numMipmaps) - 1), pxm, pym);

    float lo01 = 0, hi01 = 1, mean01 = 0.5f;
    mapStats01(d.hm, lo01, hi01, mean01);

    // Displacement is along the (±z) wall normal, so Pos.z IS the signed
    // displacement: protruding block tops sit at dz<0 (toward the -z camera),
    // recessed mortar at dz>0. Authored-border verts are pinned at dz≈0.
    const int nVerts = d.wall->VIndex;
    float dzMin = 1e30f, dzMax = -1e30f;
    for (int i = 0; i < nVerts; ++i) {
        const float z = d.wall->Verts[i].Pos.z;
        if (z < dzMin) dzMin = z; if (z > dzMax) dzMax = z;
    }
    const float p2vMeas = dzMax - dzMin;
    const float p2vExp  = amp * (hi01 - lo01);

    // FLAT-FACE fraction — the honest "flat plateaus vs domes" test, measured on
    // the FACES (a fan cell's *verts* look bimodal — groove corners + plateau
    // centre — but every *triangle* slopes corner→peak, i.e. the block is a
    // dome with NO flat top). A correct square wave has large FLAT regions: the
    // plateau tops and groove floors are flat faces; only the thin groove walls
    // slope. So: fraction of target faces whose 3 verts share a level (z-spread
    // < 10% of the relief). ~0% = every face slopes = all domes (the cpb=1
    // failure); high = real flat plateaus/floors present.
    const float range = std::max(p2vMeas, 1e-6f);
    const float flatEps = 0.10f * range;
    const float loBand = dzMin + 0.25f * range, hiBand = dzMax - 0.25f * range;
    int nFlatPlateau = 0, nFlatGroove = 0, nFlatBorder = 0, nSloped = 0;
    // FRONTAL-PROJECTED-area flatness alongside the per-face count: an edge-
    // aligned mesh spends its flat budget on a few LARGE plateau/floor faces
    // and its sloped budget on many NARROW step faces, so the count under-reads
    // the surface (a perfect square wave with 2-triangle plateaus sits near
    // ~50% by count). Raw 3D area ALSO under-reads: at test amps the step walls
    // are tall (amp-dependent), so their slant area dominates. Projecting onto
    // the wall's base plane (weight |n·z| = the |cz| cross term) measures what
    // the eye sees head-on — steps project to ~nothing, plateaus + floors to
    // their footprint — and is amp-invariant. A true square wave reads ~85%+;
    // all-domes reads ~0%.
    double areaFlat = 0.0, areaTotal = 0.0;
    for (int i = 0; i < d.wall->FIndex; ++i) {
        const Face &F = d.wall->Faces[i];
        if (!F.A || !F.B || !F.C) continue;
        const float za = F.A->Pos.z, zb = F.B->Pos.z, zc = F.C->Pos.z;
        const float fmin = std::min(za, std::min(zb, zc));
        const float fmax = std::max(za, std::max(zb, zc));
        const float favg = (za + zb + zc) / 3.0f;
        const Vector &pa = F.A->Pos, &pb = F.B->Pos, &pc = F.C->Pos;
        const float e1x = pb.x-pa.x, e1y = pb.y-pa.y, e1z = pb.z-pa.z;
        const float e2x = pc.x-pa.x, e2y = pc.y-pa.y, e2z = pc.z-pa.z;
        const float cz = e1x*e2y-e1y*e2x;    // projected onto the base (z=0) plane
        const double area = 0.5 * std::fabs(double(cz));
        areaTotal += area;
        if (fmax - fmin >= flatEps) { ++nSloped; continue; }
        areaFlat += area;
        if      (favg <= loBand)          ++nFlatPlateau;   // flat block top
        else if (favg >= hiBand)          ++nFlatGroove;    // flat groove floor
        else                              ++nFlatBorder;    // flat at authored level (~0)
    }
    const int nF = nFlatPlateau + nFlatGroove + nFlatBorder + nSloped;
    const float flatPct = nF > 0 ? 100.0f * (nF - nSloped) / nF : 0.0f;
    const float flatAreaPct = areaTotal > 0.0 ? float(100.0 * areaFlat / areaTotal) : 0.0f;

    std::fprintf(stderr,
        "[DTEST] map=%d(%s) span=%.1f amp=%.2f mip=%d\n"
        "        pitch mip0 = %.0fx%.0f tex %s | pitch mip%d = %.0fx%.0f tex %s\n"
        "        map01: lo=%.3f hi=%.3f mean=%.3f\n"
        "        mesh:  verts=%d faces=%d\n"
        "        disp z: [%+.3f .. %+.3f]  p2v=%.3f (expect %.3f = %.0f%%)\n"
        "        faces: flat plateau=%d groove=%d border=%d | sloped=%d "
        "-> FLAT-TOP frac=%.0f%% (square-wave high, all-dome ~0%%) | "
        "flat FRONTAL-AREA frac=%.0f%%\n",
        mapId, mapName(mapId), span, amp, mip,
        px0, py0, have0 ? "OK" : "NONE", mip, pxm, pym, havem ? "OK" : "NONE",
        lo01, hi01, mean01,
        nVerts, d.wall->FIndex,
        dzMin, dzMax, p2vMeas, p2vExp, p2vExp > 1e-6f ? 100.0f * p2vMeas / p2vExp : 0.0f,
        nFlatPlateau, nFlatGroove, nFlatBorder, nSloped, flatPct, flatAreaPct);
}

// ── headless pose render ────────────────────────────────────────────────────
void renderPose(DTestScene &d, Vector eye, Vector lookAt, float fov,
                int drawVizMode, const char *path) {
    FF::setDefault(FF::IntId::displace_viz, drawVizMode);   // draw-time overlay mode

    FC.ISource = eye;
    Vector look = lookAt;
    Kick_Camera(&eye, &look, 0.0f, FC.Mat);
    FC.IFOV = fov;
    CalcPersp(&FC);
    View = &FC;

    std::memset(VPage, 0, PageSize);
    std::memset(ZPage16, 0, size_t(XRes) * size_t(YRes) * sizeof(word));
    Animate_Objects(d.sc, View);
    Transform_Objects(d.sc, fds::g_mainCamera, fds::g_mainFaces);
    Lighting(d.sc);
    if (CAll) {
        Radix_Sort(FList, SList, CAll);
        Render(RenderPath::ForceForward);   // no deferred/HDR — read raw geometry
    }
    if (drawVizMode)   // viz-1/2 overlay over the finished frame
        fds::DisplaceViz_DrawOverlay(d.sc);

    std::FILE *f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "[DTEST] cannot write %s\n", path); return; }
    const int xr = (int)XRes, yr = (int)YRes;
    std::fprintf(f, "P6\n%d %d\n255\n", xr, yr);
    std::vector<unsigned char> row(size_t(xr) * 3);
    for (int y = 0; y < yr; ++y) {
        const dword *src = (const dword *)((const byte *)VPage + y * (int)VESA_BPSL);
        for (int x = 0; x < xr; ++x) {
            const dword px = src[x];
            row[x * 3 + 0] = (px >> 16) & 0xFF;
            row[x * 3 + 1] = (px >>  8) & 0xFF;
            row[x * 3 + 2] =  px        & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

void sizeFaceLists(Scene *sc) {
    DWord polys = 0;
    for (TriMesh *T = sc->TriMeshHead; T; T = T->Next) polys += T->FIndex;
    for (Omni *O = sc->OmniHead; O; O = O->Next) ++polys;
    fds::g_mainFaces.resize(polys * 2 + 64);
    View = sc->CameraHead;
    C_FZP = sc->FZP; C_rFZP = 1.0f / C_FZP;
}

}  // namespace

void Run_DisplaceTest() {
    const int   mapId = std::getenv("FDS_DISPLACETEST_MAP")
                            ? std::atoi(std::getenv("FDS_DISPLACETEST_MAP")) : 0;
    const float span  = std::getenv("FDS_DISPLACETEST_SPAN")
                            ? std::max(1.0f, float(std::atof(std::getenv("FDS_DISPLACETEST_SPAN")))) : 1.0f;

    // Test-clarity defaults (honoured only if the user didn't pass the flag):
    // amp up so the relief reads on the 8-unit wall; the rest = greets defaults
    // so cpb/adapt/mip tuning transfers 1:1.
    FF::setDefault(FF::FloatId::greets_displace_amp, 0.8f);

    Ambient_Factor = 1.0f; Diffusive_Factor = 1.0f; Specular_Factor = 1.0f;
    ImageSize = 1;

    if (std::getenv("FDS_DISPLACETEST_DUMP")) {
        if (FF::isSet(FF::IntId::displace_viz))
            std::fprintf(stderr, "[DTEST] note: --displace_viz was set explicitly; "
                "dump can't retarget the per-render overlay mode (all renders use "
                "your value). Omit it to get the lit/viz-1/viz-2 triplet.\n");

        // 1) Metric matrix over (map, span) — bake-only, no render. The span-3
        //    running-bond row (map 3) is the truest analog of the real wall.
        std::fprintf(stderr, "[DTEST] ===== metric matrix =====\n");
        const int   maps[]  = {0, 1, 2, 3};
        const float spans[] = {1.0f, 2.0f, 3.0f};
        for (int m : maps)
            for (float s : spans) {
                DTestScene md = build(m, s, /*vizMode=*/0);
                reportMetrics(md, m, s);
            }

        // 2) Render the SELECTED combo: 4 poses × 3 styles. Bake once at viz=2
        //    (records BOTH overlay fields); the render toggles the draw mode.
        std::fprintf(stderr, "[DTEST] ===== rendering map=%d(%s) span=%.1f =====\n",
                     mapId, mapName(mapId), span);
        DTestScene d = build(mapId, span, /*vizMode=*/2);
        SetCurrentScene(d.sc);
        sizeFaceLists(d.sc);
        reportMetrics(d, mapId, span);

        struct Pose { Vector eye, look; float fov; const char *tag; };
        const Pose poses[] = {
            { Vector(0.0f, 4.0f, -13.0f), Vector( 0.0f, 4.0f, 0.0f), 55.0f, "frontal"   },
            { Vector(10.0f, 5.0f, -10.0f), Vector( 0.0f, 4.0f, 0.0f), 55.0f, "diag45"    },
            { Vector(12.0f, 4.5f, -2.5f), Vector(-3.0f, 4.0f, 0.0f), 55.0f, "grazing"   },
            // Edge-on SIDE: camera in the wall plane on the +x side, looking
            // ACROSS the surface toward -x so the ±z relief pokes out as a
            // profile against the black background (the z-vs-y envelope).
            { Vector(9.0f, 4.0f, 0.0f), Vector(-9.0f, 4.0f, 0.0f), 40.0f, "silhouette" },
            // TOP-DOWN: camera above the wall looking almost straight down, so
            // screen-horizontal = x (block index) and the relief profile z(x)
            // reads directly — for map 0 the near edge is a SQUARE WAVE.
            { Vector(0.0f, 20.0f, 0.6f), Vector(0.0f, 3.5f, 0.0f), 40.0f, "topdown" },
        };
        struct Style { int viz; const char *tag; };
        const Style styles[] = { {0, "lit"}, {1, "viz1"}, {2, "viz2"} };
        for (const Pose &p : poses)
            for (const Style &st : styles) {
                char path[96];
                std::snprintf(path, sizeof(path), "/tmp/displacetest_%s_%s.ppm", p.tag, st.tag);
                renderPose(d, p.eye, p.look, p.fov, st.viz, path);
                std::fprintf(stderr, "[DTEST] wrote %s\n", path);
            }
        return;
    }

    // ── interactive free-cam (map/span from env, viz from --displace_viz) ──
    const int viz = FF::displace_viz();
    DTestScene d = build(mapId, span, viz);
    SetCurrentScene(d.sc);
    sizeFaceLists(d.sc);
    Calibrate_FreeCamera_ForScene(d.sc->FZP, d.sc->CameraHead);
    reportMetrics(d, mapId, span);

    FC.ISource = Vector(0.0f, 4.0f, -13.0f);
    { Vector eye = FC.ISource, look(0.0f, 4.0f, 0.0f); Kick_Camera(&eye, &look, 0.0f, FC.Mat); }
    FC.IFOV = 55.0f;
    CalcPersp(&FC);
    View = &FC;
    std::fprintf(stderr,
        "[DTEST] interactive map=%d(%s) span=%.1f — WASD/QE move, arrows look, "
        "G dump pose, ESC/Backspace exit%s\n",
        mapId, mapName(mapId), span, viz ? " (displace_viz overlay ON)" : "");

    char msg[192];
    float TTrd = -1.0f;
    while (!Keyboard[ScESC] && !Keyboard[ScBackSpace] && !g_shouldQuit.load()) {
        g_FrameTime = Timer;
        dTime = (TTrd > 0) ? (Timer - TTrd) * 0.25f : 0;
        TTrd = Timer;
        Dynamic_Camera();
        CalcPersp(&FC);
        View = &FC;

        // 'G' (rising edge): print the current pose as a ready-to-paste Pose{}
        // line (eye + a lookAt one unit along forward) for headless repro.
        {
            static bool gPrev = false;
            const bool gNow = Keyboard[ScG] != 0;
            if (gNow && !gPrev) {
                const Vector &e = FC.ISource;
                const float fx = FC.Mat[2][0], fy = FC.Mat[2][1], fz = FC.Mat[2][2];
                std::fprintf(stderr, "[DTEST-POSE] { Vector(%.2ff,%.2ff,%.2ff), "
                    "Vector(%.2ff,%.2ff,%.2ff), 55.0f, \"probe\" },\n",
                    e.x, e.y, e.z, e.x + fx * 10, e.y + fy * 10, e.z + fz * 10);
            }
            gPrev = gNow;
        }

        std::memset(VPage, 0, PageSize);
        std::memset(ZPage16, 0, size_t(XRes) * size_t(YRes) * sizeof(word));
        Animate_Objects(d.sc, View);
        Transform_Objects(d.sc, fds::g_mainCamera, fds::g_mainFaces);
        Lighting(d.sc);
        if (CAll) {
            Radix_Sort(FList, SList, CAll);
            Render(RenderPath::ForceForward);
        }
        if (viz) fds::DisplaceViz_DrawOverlay(d.sc);

        std::snprintf(msg, sizeof(msg),
            "DISPLACETEST map=%d(%s) span=%.1f  Cam=(%.1f,%.1f,%.1f)  "
            "WASD/QE arrows, G=pose, ESC exit",
            mapId, mapName(mapId), span,
            FC.ISource.x, FC.ISource.y, FC.ISource.z);
        OutTextXY(VPage, 0, 0, msg, 255);
        Flip(MainSurf);
    }
}
