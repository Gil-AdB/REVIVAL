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
// FDS_DISPLACETEST_JUNCTION=1: fan↔edge seam-hole A/B (seam_union off vs on).
// FDS_DISPLACETEST_NEIGHBOR=1: cross-material coincident-seam A/B
// (neighbor_pin off vs on; numeric mid-line assert + lit PPMs).
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
#include <Base/Spline.h>
#include <Base/Texture.h>
#include <Base/TriMesh.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

extern void Scene_RebuildMatTable(Scene *Sc);
extern void Compute_FaceVertexIndices(TriMesh *T);
// SceneBuilder::Finalize does NOT run Scene_Computations, so rig meshes have no
// object-space tangents — and the parallax/POM march needs them (a zero tangent
// normalizes to NaN, which silently poisons every marched UV). Called explicitly
// by the shell arm below.
extern void Compute_Vertex_Tangents(TriMesh *T);
extern void StampSingleKey(Spline &sp, float x, float y, float z, float w);

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
Texture *makeSyntheticTiled32(const std::vector<uint8_t> &grid) {
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
    return t;
}

Texture *makeSyntheticHeight8(const std::vector<uint8_t> &grid) {
    return MakeHeight8(makeSyntheticTiled32(grid));
}

// The SAME field as a visible grayscale ALBEDO. Without it the rig's wall is a
// solid colour, and then per-pixel parallax is INVISIBLE by construction (the
// march shifts the UV of a uniform texture) — the geometric bake shows relief
// only because its normals are real. Any honest tessellation-vs-POM comparison
// needs surface detail the UV shift can move.
Texture *makeSyntheticAlbedo(int mapId) {
    std::vector<uint8_t> grid(size_t(kMapSize) * kMapSize);
    switch (mapId) {
        case 1: genRamp(grid); break;
        case 3: genRunningBond(grid); break;
        default: genBlocks(grid); break;
    }
    // Lift + spread so lighting has somewhere to go (grooves ~60, blocks ~230).
    for (auto &b : grid) b = uint8_t(40 + (int(b) * 200) / 255);
    return makeSyntheticTiled32(grid);
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
DTestScene build(int mapId, float span, int vizMode, bool bake = true,
                 bool backdrop = false, bool blockAlbedo = false) {
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
    if (blockAlbedo && mapId != 2) mat->Txtr = makeSyntheticAlbedo(mapId);

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

    // S1b SHELL rig: a loud BACKDROP a few units behind the wall, wider and
    // taller than it. Anything that shows through the relief — a silhouette
    // gap at the wall's edge, a discarded ray — reads as saturated green, so
    // "does the geometry BEHIND show?" is a colour test, not a judgement call.
    if (backdrop) {
        Texture *btex = b.AddSolidColorTexture(8, 8, 0xFF00C000u);
        Material *bmat = b.AddMaterial("dtestback", btex, {0, 192, 0, 255}, 0);
        // Same winding sense as the wall (4,0,0)(-4,0,0)(-4,8,0)(4,8,0) so the
        // normal points -z, toward the camera, and it isn't back-face culled.
        const Vector backV[4] = {
            Vector( 14.0f,  0.0f, 6.0f), Vector(-14.0f,  0.0f, 6.0f),
            Vector(-14.0f, 14.0f, 6.0f), Vector( 14.0f, 14.0f, 6.0f),
        };
        b.AddQuad("dtest_back", backV, bmat);
        // Second panel at x = -12 facing +x: the edge-on poses look ALONG the
        // wall (down -x), where the z = 6 panel is invisible, so without this
        // the silhouette has a black background and the green metric reads 0.
        const Vector sideV[4] = {
            Vector(-12.0f,  0.0f, -12.0f), Vector(-12.0f,  0.0f, 12.0f),
            Vector(-12.0f, 14.0f, 12.0f),  Vector(-12.0f, 14.0f, -12.0f),
        };
        b.AddQuad("dtest_backside", sideV, bmat);
    }

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
    if (bake)
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

// ── S4a fan↔edge SEAM-HOLE rig ───────────────────────────────────────────────
// The single-quad scene above pairs its two triangles into ONE edge-aligned
// quad — it can never make a fan↔edge junction. This builds TWO quads in ONE
// TriMesh (so they share vertex indices → the bake registers the shared side):
// a LEFT quad with axis-aligned UVs (→ edge-aligned path) and a RIGHT quad with
// 45°-ROTATED UVs (→ adaptive fan path), sharing the vertical edge at x=0.
// Their shared-side param lists (groove rows vs i/2^L) have no subset relation,
// so the pre-fix heal leaves a hairline hole; --greets_displace_seam_union welds
// them. buildJunction reuses AddQuad for the LEFT quad's full engine setup
// (splines, Filler, Object registration) then extends the arrays to 6 verts /
// 4 faces and stamps the two per-quad UV charts by hand.
DTestScene buildJunction(int mapId, int vizMode) {
    using namespace fds::scene_builder;
    FF::setDefault(FF::IntId::displace_viz, vizMode);

    SceneBuilder b;
    b.SetNearFar(0.5f, 300.0f);
    b.SetAmbient(60, 60, 60);

    Texture *hm = makeHeight8(mapId);
    Texture *tex = b.AddSolidColorTexture(8, 8, 0xFFB0B0B0u);
    Material *mat = b.AddMaterial("dtest", tex, {176, 176, 176, 255}, 0);
    mat->HeightMap = hm;

    // LEFT quad x∈[-4,0], y∈[0,8], facing -z. AddQuad sets up the full mesh.
    const Vector leftV[4] = {
        Vector(0.0f, 0.0f, 0.0f), Vector(-4.0f, 0.0f, 0.0f),
        Vector(-4.0f, 8.0f, 0.0f), Vector(0.0f, 8.0f, 0.0f),
    };
    TriMesh *T = b.AddQuad("junction", leftV, mat);

    // Extend to 6 verts (add v4=(4,8,0), v5=(4,0,0)) and 4 faces (RIGHT quad
    // verts {0,3,4,5} → tris (0,3,4),(0,4,5), also facing -z).
    Vertex *nv = new Vertex[6];
    std::memcpy(nv, T->Verts, sizeof(Vertex) * 4);
    delete[] T->Verts;
    T->Verts = nv; T->VIndex = 6;
    const Vector rExtra[2] = { Vector(4.0f, 8.0f, 0.0f), Vector(4.0f, 0.0f, 0.0f) };
    for (int k = 0; k < 2; ++k) {
        Vertex &V = nv[4 + k];
        std::memset(&V, 0, sizeof(Vertex));
        V.Pos = rExtra[k];
        V.N = nv[0].N; V.TN = nv[0].N;             // -z, same as the left quad
        V.LR = V.LG = V.LB = 200; V.LA = 255;
    }
    Face *nf = new Face[4];
    std::memcpy(nf, T->Faces, sizeof(Face) * 2);
    delete[] T->Faces;
    T->Faces = nf; T->FIndex = 4;
    // rebind the 2 left faces to the new Verts array (indices unchanged 0..3)
    const int leftIdx[2][3] = { {0,1,2}, {0,2,3} };
    for (int f = 0; f < 2; ++f) {
        nf[f].A = nv + leftIdx[f][0]; nf[f].B = nv + leftIdx[f][1]; nf[f].C = nv + leftIdx[f][2];
    }
    // LEFT per-face UVs: axis-aligned u=-x/4, v=y/8 (edge-aligned path).
    auto uvL = [](const Vector &p, float &u, float &v){ u = -p.x * 0.25f; v = p.y * 0.125f; };
    // RIGHT per-face UVs: 45°-rotated u=(x+y)/8, v=(y-x)/8 (fails axis-align → fan).
    auto uvR = [](const Vector &p, float &u, float &v){ u = (p.x + p.y) * 0.125f; v = (p.y - p.x) * 0.125f; };
    auto stampF = [&](Face &F, int ai, int bi, int ci, bool rot){
        F.A = nv + ai; F.B = nv + bi; F.C = nv + ci;
        F.Txtr = mat; F.Filler = nf[0].Filler; F.frame = nullptr;
        auto uv = rot ? uvR : uvL;
        uv(nv[ai].Pos, F.U1, F.V1); uv(nv[bi].Pos, F.U2, F.V2); uv(nv[ci].Pos, F.U3, F.V3);
        F.EU1=F.U1;F.EV1=F.V1; F.EU2=F.U2;F.EV2=F.V2; F.EU3=F.U3;F.EV3=F.V3;
        F.N = nv[0].N;
        F.NormProd = -(F.N.x*F.A->Pos.x + F.N.y*F.A->Pos.y + F.N.z*F.A->Pos.z);
    };
    stampF(nf[0], 0, 1, 2, false); stampF(nf[1], 0, 2, 3, false);   // LEFT axis-aligned
    stampF(nf[2], 0, 3, 4, true);  stampF(nf[3], 0, 4, 5, true);    // RIGHT rotated
    // loose bsphere over all 6
    Vector ctr{0,0,0}; for (int i=0;i<6;++i){ ctr.x+=nv[i].Pos.x; ctr.y+=nv[i].Pos.y; ctr.z+=nv[i].Pos.z; }
    ctr.x/=6; ctr.y/=6; ctr.z/=6; float radSq=0;
    for (int i=0;i<6;++i){ const float dx=nv[i].Pos.x-ctr.x,dy=nv[i].Pos.y-ctr.y,dz=nv[i].Pos.z-ctr.z; radSq=std::max(radSq,dx*dx+dy*dy+dz*dz); }
    T->BSphereCtr=ctr; T->BSphereRad=radSq; T->BSphereRadius=std::sqrt(radSq);
    Compute_FaceVertexIndices(T);

    // Dim floor for orientation (own material — bake never touches it).
    Texture *ftex = b.AddSolidColorTexture(8, 8, 0xFF404040u);
    Material *fmat = b.AddMaterial("dtestfloor", ftex, {64, 64, 64, 255}, 0);
    const Vector floorV[4] = {
        Vector(-20.0f, 0.0f, -20.0f), Vector(-20.0f, 0.0f, 40.0f),
        Vector( 20.0f, 0.0f,  40.0f), Vector( 20.0f, 0.0f, -20.0f),
    };
    b.AddQuad("dtest_floor", floorV, fmat);
    b.AddOmni(Vector(11.0f, 9.0f, -6.0f), {255, 255, 255, 0}, 1.6f, 80.0f);
    b.SetCamera(Vector(0.0f, 4.0f, -13.0f), Vector(0.0f, 4.0f, 0.0f), 55.0f);
    b.Finalize();

    DTestScene d; d.sc = b.scene(); d.wall = T; d.hm = hm;

    const int   L     = FF::greets_stone_subdiv();
    const float amp   = FF::greets_displace_amp();
    const int   mip   = FF::greets_displace_mip();
    const float adapt = FF::greets_displace_adapt();
    const float cpb   = FF::greets_displace_cpb();
    DisplaceStoneSubdiv(d.sc, "dtest", L, amp, mip, adapt, cpb);
    MakeFacesIndependentByAngle(d.sc, 30.0f);
    Scene_RebuildMatTable(d.sc);
    return d;
}

// ── SPLIT-VERTEX CORNER rig — the t=5968 pier arris, isolated ────────────────
// TWO sheets in ONE TriMesh meeting along the vertical edge at (0,y,0) with the
// exact greets topology: the corner columns are POSITION-COINCIDENT and
// INDEX-DISTINCT (each sheet owns its own copy — the [STONE-JUNC] split-seam
// population), the sheet normals are 59° apart like the real pier (A=(+1,0,0),
// B=(+0.514,0,+0.858)), and the u phases CONFLICT at the border by
// construction: B's border column sits exactly ON a vertical mortar groove of
// the synthetic block map (u=0.5), A's just OFF one (u=0.7455) — so one sheet
// reads "joint", the other "stone", at every course. That is the maximal
// version of the disagreement measured at world (17.898,y,-58.014). Scale
// matches greets: 0.167 tiles/world-u → 1.5 u block courses at amp 0.3.
// Green backdrops sit behind the wedge so corner punch-through is a colour
// count, not a judgement call.
struct CornerMetrics {
    float gapMax = -1.0f, gapMean = -1.0f;   // between the two displaced border polylines
    int   nGapSamples = 0;
    int   nFlips = 0;                        // twisted strip faces (geo normal vs authored)
    int   nEdgeA = 0, nEdgeB = 0;            // classified boundary edges in the corner cylinder
};
DTestScene buildCorner(int mapId, CornerMetrics *M) {
    using namespace fds::scene_builder;
    FF::setDefault(FF::IntId::displace_viz, 0);

    SceneBuilder b;
    b.SetNearFar(0.5f, 300.0f);
    b.SetAmbient(60, 60, 60);

    Texture *hm = makeHeight8(mapId);
    Texture *tex = b.AddSolidColorTexture(8, 8, 0xFFB0B0B0u);
    Material *mat = b.AddMaterial("dtest", tex, {176, 176, 176, 255}, 0);
    mat->HeightMap = hm;

    // Sheet A via AddQuad (full engine setup): plane x=0, N=(+1,0,0). The
    // sheets SEGMENT the corner differently — A breaks at y=3.7, B at
    // y=2.6/5.9 — because that is what makes the greets corner a SPLIT seam:
    // the endpoint weld can only merge coincident AUTHORED verts, so
    // mismatched interior breaks leave each sheet its own index-distinct
    // corner column (the t=1088 "different segmenting along the corner").
    // A single full-height edge per sheet would weld into ONE shared interior
    // edge and the rig would be vacuous (measured: dihedral 59.08 len 8.0,
    // 0 split seams).
    const Vector aV[4] = {
        Vector(0.0f, 0.0f, 0.0f), Vector(0.0f, 0.0f, -6.0f),
        Vector(0.0f, 3.7f, -6.0f), Vector(0.0f, 3.7f, 0.0f),
    };
    TriMesh *T = b.AddQuad("corner", aV, mat);

    // Sheet B: N = normalize(0.514,0,0.858), extends from the corner along
    // (-0.858,0,+0.514) — the real pier's second wall. Its corner column
    // DUPLICATES A's positions with fresh indices (split seam).
    float nBx = 0.514f, nBz = 0.858f;
    { const float l = std::sqrt(nBx*nBx + nBz*nBz); nBx /= l; nBz /= l; }
    const Vector nB{nBx, 0.0f, nBz};
    const Vector farCol{-nBz * 6.0f, 0.0f, nBx * 6.0f};   // (-5.148, y, 3.084)

    // 14 verts: A = corner col y{0,3.7,8} + far col y{0,3.7,8} (0..5),
    // B = corner col y{0,2.6,5.9,8} (6..9) + far col y{0,2.6,5.9,8} (10..13).
    Vertex *nv = new Vertex[14];
    std::memcpy(nv, T->Verts, sizeof(Vertex) * 4);
    delete[] T->Verts;
    T->Verts = nv; T->VIndex = 14;
    const float aBreak[3] = { 0.0f, 3.7f, 8.0f };
    const float bBreak[4] = { 0.0f, 2.6f, 5.9f, 8.0f };
    const Vector extraV[10] = {
        Vector(0.0f, 8.0f, 0.0f), Vector(0.0f, 8.0f, -6.0f),          // 4,5: A top
        Vector(0.0f, bBreak[0], 0.0f), Vector(0.0f, bBreak[1], 0.0f), // 6,7: B corner col
        Vector(0.0f, bBreak[2], 0.0f), Vector(0.0f, bBreak[3], 0.0f), // 8,9
        Vector(farCol.x, bBreak[0], farCol.z), Vector(farCol.x, bBreak[1], farCol.z),  // 10,11: B far col
        Vector(farCol.x, bBreak[2], farCol.z), Vector(farCol.x, bBreak[3], farCol.z),  // 12,13
    };
    (void)aBreak;
    for (int k = 0; k < 10; ++k) {
        Vertex &V = nv[4 + k];
        std::memset(&V, 0, sizeof(Vertex));
        V.Pos = extraV[k];
        const bool isB = k >= 2;
        V.N = isB ? nB : Vector{1.0f, 0.0f, 0.0f};
        V.TN = V.N;
        V.LR = V.LG = V.LB = 200; V.LA = 255;
    }
    // 10 faces: A quad y0-3.7 (from AddQuad, rebound) + A quad 3.7-8 + B's 3 quads.
    Face *nf = new Face[10];
    std::memcpy(nf, T->Faces, sizeof(Face) * 2);
    delete[] T->Faces;
    T->Faces = nf; T->FIndex = 10;
    const int aIdx[2][3] = { {0,1,2}, {0,2,3} };
    for (int f = 0; f < 2; ++f) {
        nf[f].A = nv + aIdx[f][0]; nf[f].B = nv + aIdx[f][1]; nf[f].C = nv + aIdx[f][2];
    }
    // Per-face UVs. Course scale 0.167 tiles/u both axes; v runs down with y
    // (greets convention). A: u = 0.7455 - z*0.167 (border z=0 → 0.7455, just
    // off the 0.75 groove). B: u = 0.5 + 0.167*(in-plane distance from the
    // corner) (border → 0.5000, ON a groove).
    auto uvA = [](const Vector &p, float &u, float &v){ u = 0.7455f - p.x * 0.0f - p.z * 0.167f; v = 2.0f - p.y * 0.167f; };
    auto uvB = [&](const Vector &p, float &u, float &v){
        const float w = std::sqrt(p.x*p.x + p.z*p.z);   // distance from the corner line
        u = 0.5f + w * 0.167f; v = 2.0f - p.y * 0.167f; };
    auto stampF = [&](Face &F, int ai, int bi, int ci, bool sheetB){
        F.A = nv + ai; F.B = nv + bi; F.C = nv + ci;
        F.Txtr = mat; F.Filler = nf[0].Filler; F.frame = nullptr;
        if (sheetB) { uvB(nv[ai].Pos, F.U1, F.V1); uvB(nv[bi].Pos, F.U2, F.V2); uvB(nv[ci].Pos, F.U3, F.V3); }
        else        { uvA(nv[ai].Pos, F.U1, F.V1); uvA(nv[bi].Pos, F.U2, F.V2); uvA(nv[ci].Pos, F.U3, F.V3); }
        F.EU1=F.U1;F.EV1=F.V1; F.EU2=F.U2;F.EV2=F.V2; F.EU3=F.U3;F.EV3=F.V3;
        F.N = sheetB ? nB : Vector{1.0f, 0.0f, 0.0f};
        F.NormProd = -(F.N.x*F.A->Pos.x + F.N.y*F.A->Pos.y + F.N.z*F.A->Pos.z);
    };
    // Vert order is CLOCKWISE per the FLD convention the bake measures against
    // (MeshOps 2026-08-13: authored cross(e1,e2) points INTO the wall; the veto
    // negates it for the rendered side). AddQuad's own convention is the
    // opposite — stamped counter-clockwise, the veto's convexity test read
    // sheet B as IN FRONT of sheet A (a concave inside corner) and pinned the
    // whole corner (measured: OPEN-border(pins) far-side:dtest on every
    // segment). F.N stays the render normal.
    stampF(nf[0], 0, 2, 1, false); stampF(nf[1], 0, 3, 2, false);   // A y 0..3.7
    stampF(nf[2], 3, 5, 2, false); stampF(nf[3], 3, 4, 5, false);   // A y 3.7..8
    stampF(nf[4], 6, 11, 7, true); stampF(nf[5], 6, 10, 11, true);  // B y 0..2.6
    stampF(nf[6], 7, 12, 8, true); stampF(nf[7], 7, 11, 12, true);  // B y 2.6..5.9
    stampF(nf[8], 8, 13, 9, true); stampF(nf[9], 8, 12, 13, true);  // B y 5.9..8
    Vector ctr{0,0,0}; for (int i=0;i<14;++i){ ctr.x+=nv[i].Pos.x; ctr.y+=nv[i].Pos.y; ctr.z+=nv[i].Pos.z; }
    ctr.x/=14; ctr.y/=14; ctr.z/=14; float radSq=0;
    for (int i=0;i<14;++i){ const float dx=nv[i].Pos.x-ctr.x,dy=nv[i].Pos.y-ctr.y,dz=nv[i].Pos.z-ctr.z; radSq=std::max(radSq,dx*dx+dy*dy+dz*dz); }
    T->BSphereCtr=ctr; T->BSphereRad=radSq; T->BSphereRadius=std::sqrt(radSq);
    Compute_FaceVertexIndices(T);

    // Green backdrops behind the wedge: punch-through at the corner reads as
    // saturated green from any camera in front of the pier.
    Texture *btex = b.AddSolidColorTexture(8, 8, 0xFF00C000u);
    Material *bmat = b.AddMaterial("dtestback", btex, {0, 192, 0, 255}, 0);
    // Backdrops INSET inside the sheets' footprint (y∈[0.1,7.9]) so no ray
    // AROUND the pier can see green — any green is punch-through. backA must
    // stay inside SHEET A's own span z∈[-6,0]: its first cut reached z=+2.8
    // and the 2.8 u sticking past the wall's open corner edge was a constant
    // 667 px "punch-through floor" bit-identical across every mesh arm — a
    // rig bug adjudicated as if it were the unwelded end courses.
    const Vector backA[4] = {   // behind sheet A, facing +x
        Vector(-0.8f, 0.1f, -0.2f), Vector(-0.8f, 0.1f, -5.8f),
        Vector(-0.8f, 7.9f, -5.8f), Vector(-0.8f, 7.9f, -0.2f),
    };
    b.AddQuad("dtest_backA", backA, bmat);
    const Vector bOff{-nBx * 0.8f, 0.0f, -nBz * 0.8f};   // behind sheet B, facing +nB
    const Vector backB[4] = {
        Vector(bOff.x + 0.2f*nBz, 0.1f, bOff.z - 0.2f*nBx),
        Vector(bOff.x + 0.2f*nBz, 7.9f, bOff.z - 0.2f*nBx),
        Vector(bOff.x + farCol.x*0.95f, 7.9f, bOff.z + farCol.z*0.95f),
        Vector(bOff.x + farCol.x*0.95f, 0.1f, bOff.z + farCol.z*0.95f),
    };
    b.AddQuad("dtest_backB", backB, bmat);
    Texture *ftex = b.AddSolidColorTexture(8, 8, 0xFF404040u);
    Material *fmat = b.AddMaterial("dtestfloor", ftex, {64, 64, 64, 255}, 0);
    const Vector floorV[4] = {
        Vector(-20.0f, -0.02f, -20.0f), Vector(-20.0f, -0.02f, 40.0f),
        Vector( 20.0f, -0.02f,  40.0f), Vector( 20.0f, -0.02f, -20.0f),
    };
    b.AddQuad("dtest_floor", floorV, fmat);
    // Light + camera on the FRONT side of both sheets (positive dot with both
    // render normals (1,0,0) and (0.514,0,0.858)) — the pier read.
    b.AddOmni(Vector(5.0f, 9.0f, 4.0f), {255, 255, 255, 0}, 1.6f, 80.0f);
    b.SetCamera(Vector(2.3f, 3.4f, 1.3f), Vector(0.0f, 3.2f, 0.0f), 55.0f);
    b.Finalize();

    DTestScene d; d.sc = b.scene(); d.wall = T; d.hm = hm;

    DisplaceStoneSubdiv(d.sc, "dtest", FF::greets_stone_subdiv(),
                        FF::greets_displace_amp(), FF::greets_displace_mip(),
                        FF::greets_displace_adapt(), FF::greets_displace_cpb());

    // ── metrics, BEFORE MakeFacesIndependentByAngle touches normals ──
    if (M) {
        TriMesh *W = d.wall;
        // face sheet classifier: the u VALUE at the face's corner-nearest vert.
        // The rig authors the borders at exact u: sheet A 0.7455, sheet B
        // 0.5000, both increasing INTO the sheet — so u < 0.62 near the corner
        // is B, else A. (A u-GRADIENT classifier was tried first and shuffles
        // on ladder cells, whose u-span is a few milli-tiles.)
        auto sheetOf = [&](const Face &F) -> int {
            const Vector *P[3] = { &F.A->Pos, &F.B->Pos, &F.C->Pos };
            const float  U[3] = { F.U1, F.U2, F.U3 };
            int best = 0; float bestR = 1e30f;
            for (int k = 0; k < 3; ++k) {
                const float r = P[k]->x*P[k]->x + P[k]->z*P[k]->z;
                if (r < bestR) { bestR = r; best = k; }
            }
            return (U[best] < 0.62f) ? 1 : 0;   // 0=A, 1=B
        };
        // boundary edges via index pairs
        std::map<uint64_t, int> edgeCnt;
        std::map<uint64_t, int> edgeSheet;
        auto vidx = [&](const Vertex *v) -> uint32_t { return uint32_t(v - W->Verts); };
        for (int f = 0; f < W->FIndex; ++f) {
            const Face &F = W->Faces[f];
            const uint32_t i0=vidx(F.A), i1=vidx(F.B), i2=vidx(F.C);
            const int s = sheetOf(F);
            const uint32_t e[3][2] = {{i0,i1},{i1,i2},{i2,i0}};
            for (int k = 0; k < 3; ++k) {
                const uint64_t key = (uint64_t(std::min(e[k][0],e[k][1])) << 32) | std::max(e[k][0],e[k][1]);
                edgeCnt[key]++;
                if (s >= 0) edgeSheet[key] = s;
            }
            // twisted-face census inside the corner cylinder
            auto nearC = [](const Vector &P){ return P.x*P.x + P.z*P.z < 0.8f*0.8f && P.y > 0.4f && P.y < 7.6f; };
            if (nearC(F.A->Pos) || nearC(F.B->Pos) || nearC(F.C->Pos)) {
                const float ex1=F.B->Pos.x-F.A->Pos.x, ey1=F.B->Pos.y-F.A->Pos.y, ez1=F.B->Pos.z-F.A->Pos.z;
                const float ex2=F.C->Pos.x-F.A->Pos.x, ey2=F.C->Pos.y-F.A->Pos.y, ez2=F.C->Pos.z-F.A->Pos.z;
                const float cx=ey1*ez2-ez1*ey2, cy=ez1*ex2-ex1*ez2, cz=ex1*ey2-ey1*ex2;
                if (cx*F.N.x + cy*F.N.y + cz*F.N.z < 0.0f) ++M->nFlips;
            }
        }
        // border polylines: boundary edges inside the cylinder, split by sheet
        struct Seg { Vector a, b; };
        std::vector<Seg> segA, segB;
        // y-window [0.5,7.0]: excludes the top-border free-relief cluster at
        // y≈7.5-8 and the floor-pinned course, so the number is the CORNER's.
        auto inCyl = [](const Vector &P){ return P.x*P.x + P.z*P.z < 0.6f*0.6f && P.y > 0.5f && P.y < 7.0f; };
        // exclude POSITION-SEALED pairs: two index-distinct boundary edges at
        // the same midpoint are a sealed seam (the ladder's chord-pinned inner
        // chain, or a perfectly welded corner), not a gap.
        std::map<std::array<int64_t,3>, int> midCnt;
        auto midKey = [](const Vector &A, const Vector &B){
            return std::array<int64_t,3>{
                int64_t(std::llround(double(A.x+B.x)*5000.0)),
                int64_t(std::llround(double(A.y+B.y)*5000.0)),
                int64_t(std::llround(double(A.z+B.z)*5000.0)) };
        };
        for (const auto &kv : edgeCnt) {
            if (kv.second != 1) continue;
            const uint32_t ia = uint32_t(kv.first >> 32), ib = uint32_t(kv.first & 0xFFFFFFFFu);
            midCnt[midKey(W->Verts[ia].Pos, W->Verts[ib].Pos)]++;
        }
        for (const auto &kv : edgeCnt) {
            if (kv.second != 1) continue;
            const uint32_t ia = uint32_t(kv.first >> 32), ib = uint32_t(kv.first & 0xFFFFFFFFu);
            const Vector &A = W->Verts[ia].Pos, &B = W->Verts[ib].Pos;
            if (!inCyl(A) && !inCyl(B)) continue;
            if (midCnt[midKey(A, B)] >= 2) continue;   // sealed pair
            auto it = edgeSheet.find(kv.first);
            if (it == edgeSheet.end()) continue;
            (it->second == 0 ? segA : segB).push_back({A, B});
        }
        M->nEdgeA = int(segA.size()); M->nEdgeB = int(segB.size());
        if (std::getenv("FDS_DISPLACETEST_CORNER_DUMP")) {
            for (const Seg &S : segA)
                std::fprintf(stderr, "[DTEST-CORNER-EDGE] A (%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f)\n",
                    double(S.a.x),double(S.a.y),double(S.a.z),double(S.b.x),double(S.b.y),double(S.b.z));
            for (const Seg &S : segB)
                std::fprintf(stderr, "[DTEST-CORNER-EDGE] B (%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f)\n",
                    double(S.a.x),double(S.a.y),double(S.a.z),double(S.b.x),double(S.b.y),double(S.b.z));
        }
        auto ptSegDist = [](const Vector &P, const Seg &S) -> float {
            const float dx=S.b.x-S.a.x, dy=S.b.y-S.a.y, dz=S.b.z-S.a.z;
            const float l2 = dx*dx+dy*dy+dz*dz;
            float t = l2 > 1e-12f ? ((P.x-S.a.x)*dx + (P.y-S.a.y)*dy + (P.z-S.a.z)*dz)/l2 : 0.0f;
            t = std::max(0.0f, std::min(1.0f, t));
            const float qx=S.a.x+t*dx-P.x, qy=S.a.y+t*dy-P.y, qz=S.a.z+t*dz-P.z;
            return std::sqrt(qx*qx+qy*qy+qz*qz);
        };
        float gmx = 0.0f; double gsum = 0.0; int gn = 0;
        auto scan = [&](const std::vector<Seg> &from, const std::vector<Seg> &to){
            for (const Seg &S : from)
                for (const Vector *P : {&S.a, &S.b}) {
                    if (!inCyl(*P)) continue;
                    float best = 1e30f;
                    for (const Seg &Q : to) best = std::min(best, ptSegDist(*P, Q));
                    if (best < 1e29f) { gmx = std::max(gmx, best); gsum += best; ++gn; }
                }
        };
        if (!segA.empty() && !segB.empty()) { scan(segA, segB); scan(segB, segA); }
        M->gapMax = gn ? gmx : -1.0f;
        M->gapMean = gn ? float(gsum / gn) : -1.0f;
        M->nGapSamples = gn;
    }

    MakeFacesIndependentByAngle(d.sc, 30.0f);
    Scene_RebuildMatTable(d.sc);
    return d;
}

// ── FOLD/INVERSION rig (--greets_displace_fold_relax) — the t=6097 sliver ────
// Reproduces the REAL greets mechanism (measured at the repro pose): a NARROW
// RETURN strip at a wall corner whose verts carry corner-SMOOTHED vertex
// normals (Preprocess averages across the 90° crease), so the mean-centered
// relief recesses adjacent verts along DIVERGING directions by different
// amounts and twists strip faces past 90° — the committed N then opposes the
// winding and the plane cull rejects the face while it still fronts the
// camera: a see-through sliver. Modeled faithfully: the REAL greets wall map
// (map 2), the SHIPPED amp 0.3 (not the rig's punchy 0.8), mean-centered
// displacement, a strip one small step deep (0.15 — the greets return was
// 0.127), corner column shared BY INDEX with 45°-averaged normals.
// NOT modeled (documented per review): the full greets context — multi-tile
// UV charts, the Piramid chunk split, shadows/POM/deferred shading, and the
// exact repro camera (the rig checks the MESH invariant instead: with
// fold_relax OFF the bake must produce inverted faces; ON must produce none —
// counted convention-free by majority winding sign per flat piece).
DTestScene buildFold() {
    using namespace fds::scene_builder;
    SceneBuilder b;
    b.SetNearFar(0.5f, 300.0f);
    b.SetAmbient(60, 60, 60);

    Texture *hm = makeHeight8(2);                      // REAL greets_wall_h.png
    Texture *tex = b.AddSolidColorTexture(8, 8, 0xFFB0B0B0u);
    Material *mat = b.AddMaterial("dtest", tex, {176, 176, 176, 255}, 0);
    mat->HeightMap = hm;

    // Front wall x∈[-4,4], y∈[0,5], z=0, facing -z.
    const Vector wV[4] = {
        Vector(4.0f, 0.0f, 0.0f), Vector(-4.0f, 0.0f, 0.0f),
        Vector(-4.0f, 5.0f, 0.0f), Vector(4.0f, 5.0f, 0.0f),
    };
    TriMesh *T = b.AddQuad("fold", wV, mat);

    // Extend: return strip at x=4, z∈[0,-0.15], sharing corner column verts
    // 0 (y=0) and 3 (y=5) BY INDEX. Corner normals = 45°-averaged (the
    // Preprocess smoothing greets has); far column pure +x.
    Vertex *nv = new Vertex[6];
    std::memcpy(nv, T->Verts, sizeof(Vertex) * 4);
    delete[] T->Verts;
    T->Verts = nv; T->VIndex = 6;
    const float s2 = 0.70710678f;
    nv[0].N = Vector(s2, 0.0f, -s2); nv[0].TN = nv[0].N;   // corner col: averaged
    nv[3].N = Vector(s2, 0.0f, -s2); nv[3].TN = nv[3].N;
    for (int k = 0; k < 2; ++k) {
        Vertex &V = nv[4 + k];
        std::memset(&V, 0, sizeof(Vertex));
        V.Pos = (k == 0) ? Vector(4.0f, 0.0f, -0.15f) : Vector(4.0f, 5.0f, -0.15f);
        V.N = Vector(1.0f, 0.0f, 0.0f); V.TN = V.N;
        V.LR = V.LG = V.LB = 200; V.LA = 255;
    }
    Face *nf = new Face[4];
    std::memcpy(nf, T->Faces, sizeof(Face) * 2);
    delete[] T->Faces;
    T->Faces = nf; T->FIndex = 4;
    const int wIdx[2][3] = { {0,1,2}, {0,2,3} };
    for (int f = 0; f < 2; ++f) {
        nf[f].A = nv + wIdx[f][0]; nf[f].B = nv + wIdx[f][1]; nf[f].C = nv + wIdx[f][2];
    }
    // wall UV: one tile over 8x5; strip UV: its own axis-aligned chart (u from
    // z so the strip footprint crosses the same horizontal grooves as greets').
    auto uvW = [](const Vector &p, float &u, float &v){ u = (4.0f - p.x) * 0.125f; v = p.y * 0.2f; };
    auto uvS = [](const Vector &p, float &u, float &v){ u = -p.z * 0.125f; v = p.y * 0.2f; };
    auto stampF = [&](Face &F, int ai, int bi, int ci, const Vector &N,
                      void (*uv)(const Vector&, float&, float&)){
        F.A = nv + ai; F.B = nv + bi; F.C = nv + ci;
        F.Txtr = mat; F.Filler = nf[0].Filler; F.frame = nullptr;
        uv(nv[ai].Pos, F.U1, F.V1); uv(nv[bi].Pos, F.U2, F.V2); uv(nv[ci].Pos, F.U3, F.V3);
        F.EU1=F.U1;F.EV1=F.V1; F.EU2=F.U2;F.EV2=F.V2; F.EU3=F.U3;F.EV3=F.V3;
        F.N = N;
        F.NormProd = -(F.N.x*F.A->Pos.x + F.N.y*F.A->Pos.y + F.N.z*F.A->Pos.z);
    };
    stampF(nf[0], 0, 1, 2, Vector(0,0,-1), uvW);
    stampF(nf[1], 0, 2, 3, Vector(0,0,-1), uvW);
    stampF(nf[2], 0, 4, 5, Vector(1,0,0), uvS);      // return strip
    stampF(nf[3], 0, 5, 3, Vector(1,0,0), uvS);
    Vector ctr{0,0,0}; for (int i=0;i<6;++i){ ctr.x+=nv[i].Pos.x; ctr.y+=nv[i].Pos.y; ctr.z+=nv[i].Pos.z; }
    ctr.x/=6; ctr.y/=6; ctr.z/=6; float radSq=0;
    for (int i=0;i<6;++i){ const float dx=nv[i].Pos.x-ctr.x,dy=nv[i].Pos.y-ctr.y,dz=nv[i].Pos.z-ctr.z; radSq=std::max(radSq,dx*dx+dy*dy+dz*dz); }
    T->BSphereCtr=ctr; T->BSphereRad=radSq; T->BSphereRadius=std::sqrt(radSq);
    Compute_FaceVertexIndices(T);

    b.AddOmni(Vector(11.0f, 9.0f, -6.0f), {255, 255, 255, 0}, 1.6f, 80.0f);
    b.SetCamera(Vector(0.0f, 2.5f, -13.0f), Vector(0.0f, 2.5f, 0.0f), 55.0f);
    b.Finalize();

    DTestScene d; d.sc = b.scene(); d.wall = T; d.hm = hm;
    DisplaceStoneSubdiv(d.sc, "dtest", FF::greets_stone_subdiv(),
                        FF::greets_displace_amp(), FF::greets_displace_mip(),
                        FF::greets_displace_adapt(), FF::greets_displace_cpb());
    MakeFacesIndependentByAngle(d.sc, 30.0f);
    Scene_RebuildMatTable(d.sc);
    return d;
}

// ── cross-material NEIGHBOR-seam rig (--greets_displace_neighbor_pin) ────────
// The bug class: a DISPLACED wall's INTERIOR edge is position-coincident with
// the edge of a separate NON-displaced piece (a lintel/shelf authored as its
// own geometry with DUPLICATE verts — not index-shared). The old border rule
// (edge used by one target face / non-target incidence by INDEX) cannot see the
// neighbour, so the wall's seam verts displace and the junction tears. The
// outer wall boundary can never test this (single-target-face edges are always
// pinned), so the rig makes the seam INTERIOR: a wall of TWO stacked quads
// sharing the horizontal mid edge at y=4, plus a slab whose attachment edge
// duplicates that line. Verification is NUMERIC (renders can't isolate it):
// with the pin the mid-line verts must not displace; without it they must.
DTestScene buildNeighbor(int mapId) {
    using namespace fds::scene_builder;
    SceneBuilder b;
    b.SetNearFar(0.5f, 300.0f);
    b.SetAmbient(60, 60, 60);

    Texture *hm = makeHeight8(mapId);
    Texture *tex = b.AddSolidColorTexture(8, 8, 0xFFB0B0B0u);
    Material *mat = b.AddMaterial("dtest", tex, {176, 176, 176, 255}, 0);
    mat->HeightMap = hm;
    Texture *ltex = b.AddSolidColorTexture(8, 8, 0xFF806040u);
    Material *lmat = b.AddMaterial("dtestlin", ltex, {128, 96, 64, 255}, 0);

    // LOWER wall quad W1: x∈[-4,4], y∈[0,4], facing -z (AddQuad full setup).
    const Vector w1V[4] = {
        Vector(4.0f, 0.0f, 0.0f), Vector(-4.0f, 0.0f, 0.0f),
        Vector(-4.0f, 4.0f, 0.0f), Vector(4.0f, 4.0f, 0.0f),
    };
    TriMesh *T = b.AddQuad("neighbor", w1V, mat);

    // Extend to 10 verts / 6 faces: UPPER quad W2 shares the mid edge (verts
    // 2,3) by INDEX (one wall — its mid edge is interior, 2 target faces);
    // the LINTEL duplicates the mid-line endpoints as verts 6,7 (coincident by
    // POSITION only) and hangs a slab toward -z.
    Vertex *nv = new Vertex[10];
    std::memcpy(nv, T->Verts, sizeof(Vertex) * 4);
    delete[] T->Verts;
    T->Verts = nv; T->VIndex = 10;
    const Vector extra[6] = {
        Vector(-4.0f, 8.0f, 0.0f), Vector(4.0f, 8.0f, 0.0f),           // 4,5 wall top
        Vector(-4.0f, 4.0f, 0.0f), Vector(4.0f, 4.0f, 0.0f),           // 6,7 lintel @ wall (DUPES of 2,3)
        Vector(4.0f, 4.0f, -2.0f), Vector(-4.0f, 4.0f, -2.0f),         // 8,9 lintel front
    };
    for (int k = 0; k < 6; ++k) {
        Vertex &V = nv[4 + k];
        std::memset(&V, 0, sizeof(Vertex));
        V.Pos = extra[k];
        V.N = (k < 2) ? nv[0].N : Vector(0.0f, 1.0f, 0.0f);
        V.TN = V.N;
        V.LR = V.LG = V.LB = 200; V.LA = 255;
    }
    Face *nf = new Face[6];
    std::memcpy(nf, T->Faces, sizeof(Face) * 2);
    delete[] T->Faces;
    T->Faces = nf; T->FIndex = 6;
    const int w1Idx[2][3] = { {0,1,2}, {0,2,3} };
    for (int f = 0; f < 2; ++f) {
        nf[f].A = nv + w1Idx[f][0]; nf[f].B = nv + w1Idx[f][1]; nf[f].C = nv + w1Idx[f][2];
    }
    // axis-aligned UVs, one tile over the 8x8 wall (u right-to-left like the
    // junction rig, v up): the edge-aligned bake path applies.
    auto uvW = [](const Vector &p, float &u, float &v){ u = (4.0f - p.x) * 0.125f; v = p.y * 0.125f; };
    auto stampF = [&](Face &F, int ai, int bi, int ci, Material *m, const Vector &N,
                      void (*uv)(const Vector&, float&, float&)){
        F.A = nv + ai; F.B = nv + bi; F.C = nv + ci;
        F.Txtr = m; F.Filler = nf[0].Filler; F.frame = nullptr;
        uv(nv[ai].Pos, F.U1, F.V1); uv(nv[bi].Pos, F.U2, F.V2); uv(nv[ci].Pos, F.U3, F.V3);
        F.EU1=F.U1;F.EV1=F.V1; F.EU2=F.U2;F.EV2=F.V2; F.EU3=F.U3;F.EV3=F.V3;
        F.N = N;
        F.NormProd = -(F.N.x*F.A->Pos.x + F.N.y*F.A->Pos.y + F.N.z*F.A->Pos.z);
    };
    auto uvL = [](const Vector &p, float &u, float &v){ u = (4.0f - p.x) * 0.125f; v = -p.z * 0.5f; };
    stampF(nf[0], 0, 1, 2, mat, nv[0].N, uvW);            // W1 (re-stamp keeps
    stampF(nf[1], 0, 2, 3, mat, nv[0].N, uvW);            //  UVs consistent)
    stampF(nf[2], 3, 2, 4, mat, nv[0].N, uvW);            // W2 upper quad
    stampF(nf[3], 3, 4, 5, mat, nv[0].N, uvW);
    stampF(nf[4], 6, 7, 8, lmat, Vector(0,1,0), uvL);     // lintel slab (top)
    stampF(nf[5], 6, 8, 9, lmat, Vector(0,1,0), uvL);
    Vector ctr{0,0,0}; for (int i=0;i<10;++i){ ctr.x+=nv[i].Pos.x; ctr.y+=nv[i].Pos.y; ctr.z+=nv[i].Pos.z; }
    ctr.x/=10; ctr.y/=10; ctr.z/=10; float radSq=0;
    for (int i=0;i<10;++i){ const float dx=nv[i].Pos.x-ctr.x,dy=nv[i].Pos.y-ctr.y,dz=nv[i].Pos.z-ctr.z; radSq=std::max(radSq,dx*dx+dy*dy+dz*dz); }
    T->BSphereCtr=ctr; T->BSphereRad=radSq; T->BSphereRadius=std::sqrt(radSq);
    Compute_FaceVertexIndices(T);

    b.AddOmni(Vector(11.0f, 9.0f, -6.0f), {255, 255, 255, 0}, 1.6f, 80.0f);
    b.SetCamera(Vector(0.0f, 4.0f, -13.0f), Vector(0.0f, 4.0f, 0.0f), 55.0f);
    b.Finalize();

    DTestScene d; d.sc = b.scene(); d.wall = T; d.hm = hm;
    DisplaceStoneSubdiv(d.sc, "dtest", FF::greets_stone_subdiv(),
                        FF::greets_displace_amp(), FF::greets_displace_mip(),
                        FF::greets_displace_adapt(), FF::greets_displace_cpb());
    MakeFacesIndependentByAngle(d.sc, 30.0f);
    Scene_RebuildMatTable(d.sc);
    return d;
}

// Count "enclosed background" pixels in ZPage16: z==0 pixels that have a
// rendered (z>0) pixel BOTH to their left and right in the same row — i.e.
// background showing THROUGH the geometry (a hole), not the silhouette. Also
// captures the z-buffer into `zOut` (XRes*YRes words) when non-null.
long scanEnclosedBg(word *zOut) {
    const int xr = (int)XRes, yr = (int)YRes;
    const word *z = ZPage16;
    if (zOut) std::memcpy(zOut, z, size_t(xr) * yr * sizeof(word));
    long n = 0;
    for (int y = 0; y < yr; ++y) {
        const word *row = z + size_t(y) * xr;
        int lo = -1, hi = -1;
        for (int x = 0; x < xr; ++x) if (row[x]) { if (lo < 0) lo = x; hi = x; }
        if (lo < 0) continue;
        for (int x = lo + 1; x < hi; ++x) if (row[x] == 0) ++n;
    }
    return n;
}

// Given a saved OFF z-buffer, count pixels the fix FILLED (off bg → on wall) and
// OPENED (off wall → on bg), restricted to enclosed columns (off had wall left
// and right in the row) so silhouette differences don't register.
void scanHoleDelta(const word *zOff, long &filled, long &opened) {
    const int xr = (int)XRes, yr = (int)YRes;
    const word *zOn = ZPage16;
    filled = opened = 0;
    for (int y = 0; y < yr; ++y) {
        const word *ro = zOff + size_t(y) * xr;
        const word *rn = zOn  + size_t(y) * xr;
        int lo = -1, hi = -1;
        for (int x = 0; x < xr; ++x) if (ro[x] || rn[x]) { if (lo < 0) lo = x; hi = x; }
        if (lo < 0) continue;
        for (int x = lo + 1; x < hi; ++x) {
            if (ro[x] == 0 && rn[x] != 0) ++filled;
            if (ro[x] != 0 && rn[x] == 0) ++opened;
        }
    }
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
                int drawVizMode, const char *path, bool deferred = false) {
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
        // Deferred is REQUIRED for anything per-pixel (POM / shell POM run in
        // the Mekalele G-buffer fill); the geometry arms stay forward so the
        // bake's raw triangles are what gets read.
        Render(deferred ? RenderPath::ForceDeferred : RenderPath::ForceForward);
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

    // ── FOLD/INVERSION headless A/B (FDS_DISPLACETEST_FOLD=1) ────────────────
    // Bakes the wall+return-strip scene (buildFold: REAL greets map, shipped
    // amp 0.3, corner-smoothed normals) twice — fold_relax OFF then ON — and
    // asserts on the MESH invariant: inverted faces (winding opposite the
    // majority sign of their flat piece — wall keyed by g_z, strip by g_x)
    // must exist with the relax OFF and be zero with it ON. Also renders a
    // grazing pose down the strip per mode and reports raw z==0 counts (the
    // visible sliver needs the exact greets view/occlusion context, so the
    // render numbers are REPORTED, not asserted).
    if (std::getenv("FDS_DISPLACETEST_FOLD")) {
        FF::setDefault(FF::FloatId::greets_displace_amp, 0.3f);   // the SHIPPED amp
        auto invertedCount = [](const DTestScene &d) -> int {
            // Group faces by their bake-stamped PARENT-PLANE ordinal (the
            // ShadowMatID transient tag; exact piece identity — a spatial
            // classifier misreads displaced cells near the corner), signed by
            // g·n̂_parent; the majority sign per group is the healthy winding
            // convention, the minority are inverted.
            std::map<uint16_t, std::pair<long,long>> pmCnt;   // ordinal → (pos,neg)
            std::vector<float>    sgn(d.wall->FIndex, 0.0f);
            std::vector<uint16_t> ord(d.wall->FIndex, 0);
            for (int i = 0; i < d.wall->FIndex; ++i) {
                const Face &F = d.wall->Faces[i];
                if (!F.A || !F.B || !F.C || !F.Txtr || !F.Txtr->Name ||
                    std::strcmp(F.Txtr->Name, "dtest")) continue;
                const StoneParentPlane *pp = MeshOps_StoneParentPlane("dtest", F.ShadowMatID);
                if (!pp) continue;
                const Vector &A=F.A->Pos, &B=F.B->Pos, &C=F.C->Pos;
                const float e1x=B.x-A.x,e1y=B.y-A.y,e1z=B.z-A.z;
                const float e2x=C.x-A.x,e2y=C.y-A.y,e2z=C.z-A.z;
                const float gx=e1y*e2z-e1z*e2y, gy=e1z*e2x-e1x*e2z, gz=e1x*e2y-e1y*e2x;
                const float s = gx*pp->nx + gy*pp->ny + gz*pp->nz;
                if (s == 0.0f) continue;
                sgn[i] = s; ord[i] = F.ShadowMatID;
                auto &pc = pmCnt[F.ShadowMatID];
                (s > 0 ? pc.first : pc.second)++;
            }
            int inv = 0;
            for (int i = 0; i < d.wall->FIndex; ++i) {
                if (sgn[i] == 0.0f) continue;
                const auto &pc = pmCnt[ord[i]];
                const float maj = (pc.first >= pc.second) ? 1.0f : -1.0f;
                if (sgn[i] * maj < 0.0f) ++inv;
            }
            return inv;
        };
        auto zZero = []() -> long {
            long n = 0;
            for (size_t i = 0, e = size_t(XRes) * YRes; i < e; ++i) if (!ZPage16[i]) ++n;
            return n;
        };
        int invOff = 0, invOn = 0; long zOff2 = 0, zOn2 = 0;
        std::fprintf(stderr, "[DTEST-FOLD] real map, amp=%.2f — baking fold_relax OFF\n",
                     (double)FF::greets_displace_amp());
        FF::setParamFromText("greets_displace_fold_relax", "0");
        {
            DTestScene d = buildFold();
            invOff = invertedCount(d);
            SetCurrentScene(d.sc); sizeFaceLists(d.sc);
            renderPose(d, Vector(5.2f, 4.2f, -3.0f), Vector(3.95f, 2.0f, -0.1f), 55.0f, 0,
                       "/tmp/displace_fold_graze_OFF.ppm");
            zOff2 = zZero();
        }
        std::fprintf(stderr, "[DTEST-FOLD] baking fold_relax ON\n");
        FF::setParamFromText("greets_displace_fold_relax", "1");
        {
            DTestScene d = buildFold();
            invOn = invertedCount(d);
            SetCurrentScene(d.sc); sizeFaceLists(d.sc);
            renderPose(d, Vector(5.2f, 4.2f, -3.0f), Vector(3.95f, 2.0f, -0.1f), 55.0f, 0,
                       "/tmp/displace_fold_graze_ON.ppm");
            zOn2 = zZero();
        }
        const bool pass = invOff > 0 && invOn == 0;
        std::fprintf(stderr, "[DTEST-FOLD] inverted faces OFF=%d ON=%d; grazing-pose "
                     "z==0 OFF=%ld ON=%ld — %s\n", invOff, invOn, zOff2, zOn2,
                     pass ? "PASS (bake reproduces the inversion; relax removes it)"
                          : (invOff == 0 ? "VACUOUS (rig produced no inversion — the "
                                           "mechanism needs conditions the rig lacks; "
                                           "greets t=6097 is the ground truth)"
                                         : "FAIL (relax left inverted faces)"));
        return;
    }

    // ── cross-material NEIGHBOR-seam headless A/B (FDS_DISPLACETEST_NEIGHBOR=1)
    // Bakes the wall+lintel scene twice (--greets_displace_neighbor_pin off,
    // then on) and asserts NUMERICALLY on the coincident interior mid-line
    // (y=4): its subdivision verts must displace with the pin OFF (the seam
    // would tear against the lintel) and must be PINNED to zero with it ON.
    // A vert is "on the line" when y≈4 and |z| below the lintel depth (wall
    // displacement is along -z, so line verts keep y; the lintel's own verts
    // sit at z=0 / z=-2 and never move — z=0 ones read 0 and can't fake a
    // failure). Renders a lit frontal + top-grazing PPM per mode for the eye.
    if (std::getenv("FDS_DISPLACETEST_NEIGHBOR")) {
        const int nmap = std::getenv("FDS_DISPLACETEST_MAP")
                             ? std::atoi(std::getenv("FDS_DISPLACETEST_MAP")) : 0;
        auto lineMaxDisp = [](const DTestScene &d, int &nLine) -> float {
            float mx = 0.0f; nLine = 0;
            for (int i = 0; i < d.wall->VIndex; ++i) {
                const Vector &p = d.wall->Verts[i].Pos;
                if (std::fabs(p.y - 4.0f) > 1e-3f) continue;   // not on the line
                if (std::fabs(p.x) > 3.999f) continue;         // corners pinned by the old rule anyway
                if (std::fabs(p.z) > 1.0f) continue;           // lintel front edge
                ++nLine;
                mx = std::max(mx, std::fabs(p.z));             // wall displaces along -z only
            }
            return mx;
        };
        float dOff = 0.0f, dOn = 0.0f; int nOff = 0, nOn = 0;
        std::fprintf(stderr, "[DTEST-NEIGHBOR] map=%d(%s) — baking neighbor_pin OFF\n",
                     nmap, mapName(nmap));
        FF::setParamFromText("greets_displace_neighbor_pin", "0");
        {
            DTestScene d = buildNeighbor(nmap);
            dOff = lineMaxDisp(d, nOff);
            SetCurrentScene(d.sc); sizeFaceLists(d.sc);
            renderPose(d, Vector(0,4,-13), Vector(0,4,0), 55.0f, 0, "/tmp/displace_neighbor_frontal_OFF.ppm");
            renderPose(d, Vector(0,7.5f,-9), Vector(0,3.2f,0), 55.0f, 0, "/tmp/displace_neighbor_graze_OFF.ppm");
        }
        std::fprintf(stderr, "[DTEST-NEIGHBOR] baking neighbor_pin ON\n");
        FF::setParamFromText("greets_displace_neighbor_pin", "1");
        {
            DTestScene d = buildNeighbor(nmap);
            dOn = lineMaxDisp(d, nOn);
            SetCurrentScene(d.sc); sizeFaceLists(d.sc);
            renderPose(d, Vector(0,4,-13), Vector(0,4,0), 55.0f, 0, "/tmp/displace_neighbor_frontal_ON.ppm");
            renderPose(d, Vector(0,7.5f,-9), Vector(0,3.2f,0), 55.0f, 0, "/tmp/displace_neighbor_graze_ON.ppm");
        }
        const bool pass = (dOn <= 1e-5f) && (dOff > 1e-3f);
        std::fprintf(stderr, "[DTEST-NEIGHBOR] mid-line verts OFF=%d ON=%d max|disp|: "
                     "OFF=%.4f ON=%.4f — %s\n",
                     nOff, nOn, dOff, dOn,
                     pass ? "PASS (pin zeroes the coincident seam; without it the seam tears)"
                          : (dOff <= 1e-3f ? "VACUOUS (line did not displace with the pin off — rig broken?)"
                                           : "FAIL (pin left the seam displaced)"));
        return;
    }

    // ── S4a fan↔edge SEAM-HOLE headless A/B (FDS_DISPLACETEST_JUNCTION=1) ──
    // Bakes the two-quad junction twice (--greets_displace_seam_union off, then
    // on), renders each pose, and reports per-pose FILLED (holes the fix closed)
    // vs OPENED (wall the fix removed — should be ~0). Also dumps lit PPMs.
    if (std::getenv("FDS_DISPLACETEST_JUNCTION")) {
        const int jmap = std::getenv("FDS_DISPLACETEST_MAP")
                             ? std::atoi(std::getenv("FDS_DISPLACETEST_MAP")) : 0;
        struct Pose { Vector eye, look; float fov; const char *tag; };
        const Pose poses[] = {
            { Vector(0.0f, 4.0f, -13.0f), Vector( 0.0f, 4.0f, 0.0f), 55.0f, "frontal" },
            { Vector(10.0f, 5.0f, -10.0f), Vector( 0.0f, 4.0f, 0.0f), 55.0f, "diag45" },
            { Vector(12.0f, 4.5f, -2.5f), Vector(-3.0f, 4.0f, 0.0f), 55.0f, "grazing" },
            { Vector(0.0f, 20.0f, 0.6f), Vector(0.0f, 3.5f, 0.0f), 40.0f, "topdown" },
        };
        const int nPoses = int(sizeof(poses) / sizeof(poses[0]));
        std::vector<std::vector<word>> zOff(nPoses);

        std::fprintf(stderr, "[DTEST-JUNCTION] map=%d(%s) — baking seam_union OFF\n",
                     jmap, mapName(jmap));
        FF::setParamFromText("greets_displace_seam_union", "0");
        {
            DTestScene d = buildJunction(jmap, /*vizMode=*/0);
            SetCurrentScene(d.sc); sizeFaceLists(d.sc);
            for (int p = 0; p < nPoses; ++p) {
                char path[96];
                std::snprintf(path, sizeof(path), "/tmp/displace_junction_%s_OFF.ppm", poses[p].tag);
                renderPose(d, poses[p].eye, poses[p].look, poses[p].fov, 0, path);
                zOff[p].resize(size_t(XRes) * YRes);
                std::memcpy(zOff[p].data(), ZPage16, zOff[p].size() * sizeof(word));
                std::fprintf(stderr, "[DTEST-JUNCTION] %-10s OFF enclosed-bg=%ld  (%s)\n",
                             poses[p].tag, scanEnclosedBg(nullptr), path);
            }
        }

        std::fprintf(stderr, "[DTEST-JUNCTION] baking seam_union ON\n");
        FF::setParamFromText("greets_displace_seam_union", "1");
        {
            DTestScene d = buildJunction(jmap, /*vizMode=*/0);
            SetCurrentScene(d.sc); sizeFaceLists(d.sc);
            long totFilled = 0, totOpened = 0;
            for (int p = 0; p < nPoses; ++p) {
                char path[96];
                std::snprintf(path, sizeof(path), "/tmp/displace_junction_%s_ON.ppm", poses[p].tag);
                renderPose(d, poses[p].eye, poses[p].look, poses[p].fov, 0, path);
                long filled = 0, opened = 0;
                scanHoleDelta(zOff[p].data(), filled, opened);
                totFilled += filled; totOpened += opened;
                std::fprintf(stderr, "[DTEST-JUNCTION] %-10s ON  enclosed-bg=%ld  "
                             "FILLED(off-bg→on-wall)=%ld  OPENED(off-wall→on-bg)=%ld  (%s)\n",
                             poses[p].tag, scanEnclosedBg(nullptr), filled, opened, path);
            }
            std::fprintf(stderr, "[DTEST-JUNCTION] TOTAL filled=%ld opened=%ld — "
                         "%s\n", totFilled, totOpened,
                         totFilled > 0 && totOpened == 0
                             ? "PASS (fix closed holes, removed no wall)"
                             : (totFilled == 0 ? "no holes detected at these poses"
                                               : "REVIEW (fix removed wall pixels)"));
        }
        return;
    }

    // ── SPLIT-VERTEX CORNER headless A/B (FDS_DISPLACETEST_CORNER=1) ────────
    // The t=5968 pier arris, isolated. Bakes the two-sheet corner under the
    // approved umbrella arm for --greets_displace_profile_agree = 0/1/2 and
    // prints per-mode: the max/mean gap between the two displaced border
    // polylines (the CONTINUITY number — this is what must be ~0), twisted
    // strip faces, and green punch-through px from a t=5968-like grazing
    // camera + a frontal. PASS = gap ≤ 0.002 u, 0 flips, 0 green.
    if (std::getenv("FDS_DISPLACETEST_CORNER")) {
        const int cmap = std::getenv("FDS_DISPLACETEST_MAP")
                             ? std::atoi(std::getenv("FDS_DISPLACETEST_MAP")) : 0;
        // the approved umbrella arm, forced explicitly so the rig matches greets
        FF::setParamFromText("greets_displace_free_edge",    "1");
        FF::setParamFromText("greets_displace_border_mean",  "2");
        FF::setParamFromText("greets_displace_seam_weld",    "1");
        FF::setParamFromText("greets_displace_plane_normal", "1");
        FF::setParamFromText("greets_displace_block_level",  "1");
        FF::setParamFromText("greets_displace_geom_bisector","1");
        FF::setParamFromText("greets_displace_amp",          "0.3");
        // Green counted only in the central 20 % of screen width — the corner
        // column. The sheets' OUTER free borders legitimately carve silhouette
        // relief that shows backdrop near the frame edges; that is greets
        // behavior, not corner punch-through.
        auto greenCount = [](const char *path) -> long {
            std::FILE *f = std::fopen(path, "rb");
            if (!f) return -1;
            int w = 0, h = 0, mx = 0;
            if (std::fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3) { std::fclose(f); return -1; }
            std::fgetc(f);
            std::vector<unsigned char> px(size_t(w) * size_t(h) * 3);
            const size_t got = std::fread(px.data(), 1, px.size(), f);
            std::fclose(f);
            long n = 0;
            const int x0 = w * 2 / 5, x1 = w * 3 / 5;
            for (int y = 0; y < h; ++y)
                for (int x = x0; x < x1; ++x) {
                    const size_t i = (size_t(y) * w + x) * 3;
                    if (i + 2 >= got) continue;
                    if (px[i+1] > 100 && px[i] < 60 && px[i+2] < 60) ++n;
                }
            return n;
        };
        std::fprintf(stderr, "[DTEST-CORNER] map=%d(%s) split-vertex 59° corner, "
                     "approved arm, amp 0.3 — modes: 0=double-valued 1=agree-MAX 2=agree-MIN\n",
                     cmap, mapName(cmap));
        for (int mode = 0; mode <= 2; ++mode) {
            char mstr[2] = { char('0' + mode), 0 };
            FF::setParamFromText("greets_displace_profile_agree", mstr);
            CornerMetrics M;
            DTestScene d = buildCorner(cmap, &M);
            SetCurrentScene(d.sc); sizeFaceLists(d.sc);
            char pGraze[96], pFront[96];
            std::snprintf(pGraze, sizeof(pGraze), "/tmp/displace_corner_graze_m%d.ppm", mode);
            std::snprintf(pFront, sizeof(pFront), "/tmp/displace_corner_frontal_m%d.ppm", mode);
            // Both poses on the FRONT side of both sheets. "graze" hugs sheet
            // B's plane (the t=5968 read: one face near edge-on, the other at
            // an angle); "front" looks down the wedge bisector.
            renderPose(d, Vector(2.6f, 3.6f, 0.2f), Vector(0.0f, 3.2f, 0.0f), 55.0f, 0, pGraze);
            const long gGraze = greenCount(pGraze);
            renderPose(d, Vector(2.3f, 4.0f, 1.3f), Vector(0.0f, 3.6f, 0.0f), 55.0f, 0, pFront);
            const long gFront = greenCount(pFront);
            const bool pass = M.gapMax >= 0.0f && M.gapMax <= 0.002f && M.nFlips == 0
                              && gGraze == 0 && gFront == 0;
            std::fprintf(stderr, "[DTEST-CORNER] mode=%d  border-gap max=%.4f mean=%.4f "
                         "(%d samples, edges A/B %d/%d)  flips=%d  green graze/front=%ld/%ld  — %s\n",
                         mode, double(M.gapMax), double(M.gapMean), M.nGapSamples,
                         M.nEdgeA, M.nEdgeB, M.nFlips, gGraze, gFront,
                         pass ? "PASS" : "FAIL");
        }
        return;
    }

    // ── S1b SHELL-POM comparison rig (FDS_DISPLACETEST_SHELL=1) ─────────────
    // The prize test, on ONE quad with a loud green backdrop 6 units behind it:
    // does the per-pixel shell open real silhouettes at the wall's edge, and
    // does what shows through match the tessellation bake? Four arms, same
    // poses, all rendered DEFERRED (POM only exists in the G-buffer fill):
    //   tess   — the geometric bake (today's shipping look), POM off
    //   flat   — flat quad + plain POM (no shell): relief, no silhouettes
    //   shell  — flat quad pushed out to the lid + shell march + discard
    //   shellnd— shell with --no-pom_shell_domain: lid + marched depth, NO
    //            discard. The (shell − shellnd) diff IS the discard viz.
    // Every arm writes /tmp/dtshell_<arm>_<pose>.ppm; the per-arm green-pixel
    // count (the backdrop showing through the wall's screen area) is the
    // numeric silhouette metric.
    if (std::getenv("FDS_DISPLACETEST_SHELL")) {
        struct Pose { Vector eye, look; float fov; const char *tag; };
        const Pose poses[] = {
            { Vector(0.0f, 4.0f, -13.0f),  Vector( 0.0f, 4.0f, 0.0f), 55.0f, "frontal" },
            { Vector(10.0f, 5.0f, -10.0f), Vector( 0.0f, 4.0f, 0.0f), 55.0f, "diag45"  },
            { Vector(10.0f, 4.5f, -5.0f),  Vector(-3.0f, 4.0f, 0.0f), 55.0f, "grazing" },
            // Edge-on from the +x side, looking ACROSS the wall: the relief
            // profile pokes past the authored edge here, so this is where a
            // silhouette either exists or doesn't.
            { Vector(9.0f, 4.0f, -2.5f),   Vector(-9.0f, 4.0f, -2.5f), 40.0f, "silhouette" },
            { Vector(0.0f, 20.0f, 0.6f),   Vector(0.0f, 3.5f, 0.0f),  40.0f, "topdown" },
        };
        // Count backdrop-green pixels: (g > 100) & (r < 60) & (b < 60).
        auto greenCount = [](const char *path) -> long {
            std::FILE *f = std::fopen(path, "rb");
            if (!f) return -1;
            int w = 0, h = 0, mx = 0;
            if (std::fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3) { std::fclose(f); return -1; }
            std::fgetc(f);
            std::vector<unsigned char> px(size_t(w) * size_t(h) * 3);
            const size_t got = std::fread(px.data(), 1, px.size(), f);
            std::fclose(f);
            long n = 0;
            for (size_t i = 0; i + 2 < got; i += 3)
                if (px[i+1] > 100 && px[i] < 60 && px[i+2] < 60) ++n;
            return n;
        };
        struct Arm { const char *tag; bool bake; bool shell; bool domain; bool pom; };
        const Arm arms[] = {
            { "tess",    true,  false, false, false },
            { "flat",    false, false, false, true  },
            { "shell",   false, true,  true,  true  },
            { "shellnd", false, true,  false, true  },
        };
        std::fprintf(stderr, "[DTSHELL] map=%d(%s) span=%.1f amp=%.2f "
                     "parallax_strength=%.2f pom=%d cone=%d cap=%.1f\n",
                     mapId, mapName(mapId), (double)span,
                     (double)FF::greets_displace_amp(),
                     (double)FF::parallax_strength(), FF::parallax_pom(),
                     (int)FF::parallax_pom_cone(), (double)FF::pom_shell_cap());
        for (const Arm &a : arms) {
            FF::setParamFromText("pom_shell", a.shell ? "1" : "0");
            FF::setParamFromText("pom_shell_domain", a.domain ? "1" : "0");
            FF::setParamFromText("parallax", a.pom ? "1" : "0");
            DTestScene d = build(mapId, span, /*vizMode=*/0, a.bake, /*backdrop=*/true,
                                 /*blockAlbedo=*/true);
            if (a.shell) {
                // Same effective strength the march runs at.
                const float amp = FF::parallax_strength()
                                * (d.wall->Faces[0].Txtr ? d.wall->Faces[0].Txtr->ParallaxScale : 1.0f);
                PomShell_Build(d.sc, "dtest", amp, /*pinCrossMaterial=*/false);
            }
            if (a.pom && FF::parallax_pom_cone() && d.wall->Faces[0].Txtr
                && d.wall->Faces[0].Txtr->HeightMap
                && !d.wall->Faces[0].Txtr->ConeMap) {
                d.wall->Faces[0].Txtr->ConeMap =
                    MakeConeMap(d.wall->Faces[0].Txtr->HeightMap);
            }
            // Tangents for EVERY arm (see the extern above): without them the
            // march's tangent-space view direction is NaN and every marched UV
            // is garbage — invisible on a solid-colour texture, fatal to the
            // shell's domain test.
            for (TriMesh *T = d.sc->TriMeshHead; T; T = T->Next)
                Compute_Vertex_Tangents(T);
            SetCurrentScene(d.sc);
            sizeFaceLists(d.sc);
            for (const Pose &p : poses) {
                char path[96];
                std::snprintf(path, sizeof(path), "/tmp/dtshell_%s_%s.ppm", a.tag, p.tag);
                renderPose(d, p.eye, p.look, p.fov, 0, path, /*deferred=*/true);
                std::fprintf(stderr, "[DTSHELL] %-8s %-11s green=%ld  %s\n",
                             a.tag, p.tag, greenCount(path), path);
            }
        }
        return;
    }

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
