// "Invisible mirror" cloak test — a 4-mirror periscope relay (the classic
// diagram; geometry validated in a 2D ray-trace: 0 viewer rays hit the
// object, central rays do the full 4-bounce detour). Two 45deg V-corners
// SIDE BY SIDE: the LEFT V (x=0) is what the viewer sees; the RIGHT V
// (x=5) hides behind an occluder wall and appears only in the left V's
// reflection. A viewer ray at x~0 relays:
//   A1 (+z->+x) -> A2 (+x->+z) -> A3 (+z->-x) -> A4 (-x->+z) -> back wall
// detouring laterally AROUND a red cube parked in the left V's notch,
// then returning to its original x. So the cube is cloaked by GEOMETRY
// (any mirror order — even order 0 the near panel just occludes it).
//
// What it MEASURES is the recursion CEILING: the back wall behind the
// cloak axis carries a MAGENTA marker with no direct sight-line (A1
// blocks it). It only shows through when all four reflection bounces
// resolve:
//   order-1 clones     -> A1 reflects the corridor; right V = dead silver
//   --mirror-rtt (o-2) -> A1's window onto A2 shows A3 as dead silver
//   depth-4 recursion  -> magenta appears (the see-through completes)
// Metrics: red px on-axis = 0 (cloak holds); magenta px = recursion depth.
//
// Default: interactive free-cam. FDS_CLOAK_DUMP=1 = headless 4-pose dump
// to /tmp/cloak_view_*.ppm. FDS_CLOAK_NOMIRROR=1 = geometry ground truth.
//   ./DEMO --scene-cloaktest [--mirror-rtt]

#include "FrameProfiler.h"
#include <RENDER/GreetsMirror.h>
#include "Rev.h"
#include "MeshOps.h"          // Scene_MakeTiledTexture (checker walls)
#include "SceneBuilder.h"
#include "SceneTick.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>
#include <Base/Object.h>
#include <Base/TriMesh.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern void Scene_RebuildMatTable(Scene *Sc);

namespace {

// Checkered texture (block-tiled — hand-built linear data reads swizzled
// otherwise). Same cell size everywhere so side-wall reflections are
// indistinguishable from the back wall.
Texture *makeChecker(uint32_t a, uint32_t b) {
    const int W = 64, cell = 16;
    std::vector<uint32_t> px(size_t(W) * W);
    for (int y = 0; y < W; ++y)
        for (int x = 0; x < W; ++x)
            px[size_t(y) * W + x] = (((x / cell) + (y / cell)) & 1) ? a : b;
    return Scene_MakeTiledTexture(W, W, px.data(), /*buildMips=*/true);
}

// Vertical wall strip, y in [0, h]. Mirrortest winding convention:
// (right, left, left-top, right-top) as seen from the side the normal
// faces, where "right" = up x facing (facing +z: right=+x; facing -x:
// right=+z; facing +x: right=-z). FIRST point = the RIGHT end.
void addWall(fds::scene_builder::SceneBuilder &b, const char *name,
             float xR, float zR, float xL, float zL, float h, Material *m) {
    const Vector v[4] = {
        Vector(xR, 0.0f, zR), Vector(xL, 0.0f, zL),
        Vector(xL, h,    zL), Vector(xR, h,    zR),
    };
    b.AddQuad(name, v, m);
}

struct CloakScene {
    Scene *sc = nullptr;
    std::vector<fds::Mirror> mirrors;
    std::vector<fds::MirrorRttSlot> rttSlots;
};

CloakScene buildCloakScene() {
    using namespace fds::scene_builder;
    SceneBuilder b;
    b.SetNearFar(0.5f, 300.0f);
    b.SetAmbient(110, 110, 110);

    Texture *wallTex  = makeChecker(0xFFB0B0B0u, 0xFF606060u);
    Texture *floorTex = makeChecker(0xFF8A8A8Au, 0xFF4A4A4Au);
    Material *matWall  = b.AddMaterial("wall_mat",  wallTex,  {176,176,176,255}, 0);
    Material *matFloor = b.AddMaterial("floor_mat", floorTex, {138,138,138,255}, 0);
    Texture *mirTex = b.AddSolidColorTexture(8, 8, 0xFFC0C0C0u);
    // One material PER PANEL — BuildMirror derives one plane per material.
    Material *matM[4];
    const char *mNames[4] = { "vis_l_lo", "hid_r_lo", "hid_r_hi", "vis_l_hi" };
    for (int i = 0; i < 4; ++i)
        matM[i] = b.AddMaterial(mNames[i], mirTex, {180,180,180,255}, 0);
    Texture *redTex = b.AddSolidColorTexture(8, 8, 0xFFFF2020u);
    Material *matRed = b.AddMaterial("hidden_red", redTex, {255,32,32,255}, 0);
    Texture *magTex = b.AddSolidColorTexture(8, 8, 0xFFE020E0u);
    Material *matMagenta = b.AddMaterial("recursion_marker", magTex, {224,32,224,255}, 0);

    // Floor spans the whole arena.
    const Vector floorV[4] = {
        Vector(-14.0f, 0.0f, -14.0f), Vector(-14.0f, 0.0f, 26.0f),
        Vector( 14.0f, 0.0f,  26.0f), Vector( 14.0f, 0.0f, -14.0f),
    };
    b.AddQuad("floor", floorV, matFloor);

    // Room shell, height 8, normals into the room.
    const float H = 8.0f;
    addWall(b, "wall_back",   14.0f, 24.0f, -14.0f, 24.0f, H, matWall);   // faces -z
    addWall(b, "wall_left",  -14.0f, 24.0f, -14.0f, -14.0f, H, matWall);  // faces +x
    addWall(b, "wall_right",  14.0f, -14.0f, 14.0f,  24.0f, H, matWall);  // faces -x

    // MAGENTA recursion marker on the back wall, dead ahead of the cloak
    // axis (x -2..2, z=23.9). The relayed central rays return to their
    // original x and hit the back wall here — so this is what a CORRECT
    // 4-bounce see-through shows. Direct sight-lines to it are blocked by
    // panel A1, so it can ONLY appear through the full detour.
    addWall(b, "recursion_marker", 2.0f, 23.9f, -2.0f, 23.9f, 5.0f, matMagenta);

    // ── The periscope relay (validated in tools 2D ray-trace) ─────────
    // Two 45deg V-corners SIDE BY SIDE: the LEFT V at x=0 (panels A1 z=8,
    // A4 z=16) is what the viewer sees; the RIGHT V at x=5 (A2 z=8, A3
    // z=16) is hidden behind the occluder and appears only in A1's
    // reflection. A viewer ray at x~0 relays A1(+z->+x) -> A2(+x->+z) ->
    // A3(+z->-x) -> A4(-x->+z) -> back wall, detouring around the object
    // in the LEFT V's notch. Endpoints ordered (R,L) so each panel's
    // normal (addWall: n=(zR-zL,0,xL-xR)) faces its incoming leg.
    const float mh = 5.0f;
    // A1 @ (0,8) slope+1, n=(+x,-z): deflect viewer +z -> +x.
    addWall(b, "vis_l_lo", 1.414f, 9.414f, -1.414f, 6.586f, mh, matM[0]);
    // A2 @ (5,8) slope+1, n=(-x,+z): deflect +x -> +z.
    addWall(b, "hid_r_lo", 3.586f, 6.586f, 6.414f, 9.414f, mh, matM[1]);
    // A3 @ (5,16) slope-1, n=(-x,-z): deflect +z -> -x.
    addWall(b, "hid_r_hi", 6.414f, 14.586f, 3.586f, 17.414f, mh, matM[2]);
    // A4 @ (0,16) slope-1, n=(+x,+z): deflect -x -> +z (back on axis).
    addWall(b, "vis_l_hi", -1.414f, 17.414f, 1.414f, 14.586f, mh, matM[3]);

    // Occluder: a z=2 wall (x 2..9, faces -z) that hides the RIGHT V from
    // the viewer without touching the relay (whose legs are at z=8/z=16,
    // beyond it) or the A1 aperture (x < 1.5). This IS the "wall the other
    // mirror hides behind."
    addWall(b, "wall_occluder", 9.0f, 2.0f, 2.0f, 2.0f, H, matWall);

    // The cloaked object: red cube in the LEFT V's notch (x=0, z=12),
    // between A1 (z=8) and A4 (z=16). A1 blocks the direct view; the light
    // detours around it out through the right V.
    b.AddCube("hidden_red", Vector(0.0f, 1.2f, 12.0f), 1.2f, matRed);

    // Lights — symmetric-ish so both V's are lit comparably.
    b.AddOmni(Vector(-4.0f, 7.0f, -6.0f), {255,255,255,0}, 1.0f, 80.0f);
    b.AddOmni(Vector( 9.0f, 7.0f,  8.0f), {255,255,255,0}, 1.0f, 80.0f);
    b.AddOmni(Vector( 0.0f, 7.0f, 20.0f), {255,255,255,0}, 0.9f, 80.0f);

    b.SetCamera(Vector(0.0f, 2.5f, -12.0f), Vector(0.0f, 2.5f, 8.0f), 60.0f);

    b.Finalize();
    CloakScene cs;
    cs.sc = b.scene();
    return cs;
}

void renderPoseToPPM(CloakScene &cs, Vector eye, Vector lookAt, const char *path) {
    FC.ISource = eye;
    Vector look = lookAt;
    Kick_Camera(&eye, &look, 0.0f, FC.Mat);
    FC.IFOV = 60.0f;
    CalcPersp(&FC);
    View = &FC;
    std::memset(VPage, 0, PageSize);
    std::memset(ZPage16, 0, size_t(XRes) * size_t(YRes) * sizeof(word));
    Animate_Objects(cs.sc, View);
    fds::RenderSecondOrderMirrors(cs.sc, cs.mirrors, cs.rttSlots);
    Transform_Objects(cs.sc, fds::g_mainCamera, fds::g_mainFaces);
    fds::UpdateAllMirrors(cs.sc, cs.mirrors);
    fds::StampMirrorMasks(cs.sc, cs.mirrors);
    Lighting(cs.sc);
    if (CAll) {
        Radix_Sort(FList, SList, CAll);
        Render(RenderPath::ForceDeferred);
    }
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return;
    const int xr = (int)XRes, yr = (int)YRes;
    std::fprintf(f, "P6\n%d %d\n255\n", xr, yr);
    std::vector<unsigned char> row(size_t(xr) * 3);
    for (int y = 0; y < yr; ++y) {
        const dword *src = (const dword*)((const byte*)VPage + y * (int)VESA_BPSL);
        for (int x = 0; x < xr; ++x) {
            const dword px = src[x];
            row[x*3+0] = (px >> 16) & 0xFF;
            row[x*3+1] = (px >>  8) & 0xFF;
            row[x*3+2] = px & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[CLOAKTEST] eye=(%.1f,%.1f,%.1f) -> %s\n",
                 eye.x, eye.y, eye.z, path);
}

} // namespace

void Run_CloakTest() {
    CloakScene cs = buildCloakScene();
    SetCurrentScene(cs.sc);
    Calibrate_FreeCamera_ForScene(cs.sc->FZP, cs.sc->CameraHead);

    // FDS_CLOAK_NOMIRROR=1: skip mirror construction — pure-geometry
    // ground truth for debugging pose/winding questions.
    if (!std::getenv("FDS_CLOAK_NOMIRROR"))
        for (const char *nm : { "vis_l_lo", "hid_r_lo", "hid_r_hi", "vis_l_hi" }) {
            fds::Mirror m = fds::BuildMirror(cs.sc, nm);
            if (m.cloneMesh) cs.mirrors.push_back(std::move(m));
            else std::fprintf(stderr, "[CLOAKTEST] BuildMirror('%s') FAILED\n", nm);
        }
    std::fprintf(stderr, "[CLOAKTEST] %zu mirrors built\n", cs.mirrors.size());
    fds::TagFacesBehindMirrors(cs.sc, cs.mirrors);

    // Face-list sizing must cover the 4 clone meshes.
    DWord polys = 0;
    for (TriMesh *T = cs.sc->TriMeshHead; T; T = T->Next) polys += T->FIndex;
    for (Omni *O = cs.sc->OmniHead; O; O = O->Next) ++polys;
    fds::g_mainFaces.resize(polys * 2 + 64);
    View = cs.sc->CameraHead;
    C_FZP = cs.sc->FZP;
    C_rFZP = 1.0f / C_FZP;
    Scene_RebuildMatTable(cs.sc);
    // Second-order slots (adjacent panels see each other at the seams).
    fds::PrepareSecondOrderMirrorRtt(cs.sc, cs.mirrors, cs.rttSlots);
    std::fprintf(stderr, "[CLOAKTEST] %zu second-order RTT slots\n", cs.rttSlots.size());

    Ambient_Factor = 1.0f;
    Diffusive_Factor = 1.0f;
    Specular_Factor = 1.0f;
    ImageSize = 1;

    // FDS_CLOAK_DUMP=1: headless pose-dump mode (CI / verification).
    // Default = INTERACTIVE free-cam so the scene can be explored live.
    if (std::getenv("FDS_CLOAK_DUMP")) {
    struct Pose { Vector eye, lookAt; const char *tag; };
    const Pose poses[] = {
        // Sweet spot: the left V fills the view; cube cloaked, reflection
        // shows the detour (right V dead-silver at order-1; magenta if
        // depth-4 recursion resolves).
        { Vector( 0.0f, 2.5f, -12.0f), Vector( 0.0f, 2.5f,  8.0f), "onaxis"    },
        // Look toward the right V (x=5): the occluder wall must hide it.
        { Vector(-2.0f, 2.5f, -10.0f), Vector( 5.0f, 2.5f,  8.0f), "wallcheck" },
        // High reveal: whole apparatus + the red cube in the notch.
        { Vector(-11.0f, 12.0f, -6.0f), Vector( 3.0f, 0.0f, 12.0f), "reveal"    },
        // Down the relay corridor (x=5, +z): looks along the A2->A3 leg.
        { Vector( 5.0f, 2.5f,  4.0f),  Vector( 5.0f, 2.5f, 16.0f), "corridor"  },
    };
    for (const Pose &p : poses) {
        char path[64];
        std::snprintf(path, sizeof(path), "/tmp/cloak_view_%s.ppm", p.tag);
        renderPoseToPPM(cs, p.eye, p.lookAt, path);
    }
    return;
    }

    // ── Interactive loop ──────────────────────────────────────────────
    // Free-cam from the on-axis sweet spot: standing here the cubes are
    // invisible; strafe sideways (A/D) and they appear in the panels'
    // seams. Same per-frame mirror flow as the mirrortest driver.
    FC.ISource = Vector(0.0f, 2.5f, -12.0f);
    {
        Vector eye = FC.ISource, look(0.0f, 2.5f, 8.0f);
        Kick_Camera(&eye, &look, 0.0f, FC.Mat);
    }
    FC.IFOV = 60.0f;
    CalcPersp(&FC);
    View = &FC;
    std::fprintf(stderr,
        "[CLOAKTEST] interactive — WASD/QE move, arrows look, ESC/Backspace exit. "
        "Stand on-axis: cubes invisible; strafe: illusion breaks.%s\n",
        cs.rttSlots.empty() ? " (pass --mirror-rtt for the seam retroreflection)" : "");
    char msg[160];
    float TTrd = -1.0f;
    while (!Keyboard[ScESC] && !Keyboard[ScBackSpace] && !g_shouldQuit.load()) {
        g_FrameTime = Timer;
        if (TTrd > 0) dTime = (Timer - TTrd) * 0.25f;
        else          dTime = 0;
        TTrd = Timer;
        Dynamic_Camera();
        CalcPersp(&FC);
        View = &FC;

        std::memset(VPage, 0, PageSize);
        std::memset(ZPage16, 0, size_t(XRes) * size_t(YRes) * sizeof(word));
        Animate_Objects(cs.sc, View);
        fds::RenderSecondOrderMirrors(cs.sc, cs.mirrors, cs.rttSlots);
        Transform_Objects(cs.sc, fds::g_mainCamera, fds::g_mainFaces);
        fds::UpdateAllMirrors(cs.sc, cs.mirrors);
        fds::StampMirrorMasks(cs.sc, cs.mirrors);
        Lighting(cs.sc);
        if (CAll) {
            Radix_Sort(FList, SList, CAll);
            Render(RenderPath::ForceDeferred);   // clone mask = deferred-only
        }
        std::snprintf(msg, sizeof(msg),
                      "CLOAK  Cam=(%.1f,%.1f,%.1f)  mirrors:%zu rtt:%zu  "
                      "WASD/QE move, arrows look, ESC exit",
                      FC.ISource.x, FC.ISource.y, FC.ISource.z,
                      cs.mirrors.size(), cs.rttSlots.size());
        OutTextXY(VPage, 0, 0, msg, 255);
        Flip(MainSurf);
    }
}
