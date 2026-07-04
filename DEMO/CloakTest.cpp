// "Invisible mirror" cloak test — TWO 90-degree V-corners side by side
// (per the user's sketch). Each V = two mirror panels joined at a right-
// angle vertex (addVCorner), DOUBLE-SIDED: mirror front + an opaque
// colored back (blue = left V, orange = right V) so the layout reads
// from above. Both V's open toward the viewer (-z). The LEFT V is what
// the viewer sees; the RIGHT V hides behind an occluder wall and appears
// only inside the left V's reflection. A red cube tucked behind the left
// V's vertex is cloaked (viewer sees the mirror, not the cube).
//
// FIRST-DRAFT positioning — tune from the overhead views. The exact
// see-through optics (relay depth, reflection content) are still being
// dialed in with the user against the /tmp/cloak_view_overhead.ppm +
// birdseye renders.
//
// Default: interactive free-cam. FDS_CLOAK_DUMP=1 = headless pose dump
// (overhead / birdseye / viewer / reveal) to /tmp/cloak_view_*.ppm.
// FDS_CLOAK_NOMIRROR=1 = geometry ground truth.  ./DEMO --scene-cloaktest

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

// A double-sided mirror strip: the mirror face (R,L order → normal faces
// the incoming side) PLUS an opaque back quad offset behind it, so the
// panel is a visible solid from behind / above instead of an invisible
// or garbage-rendered mirror. `backMat` colors the back (per-V color so
// the two corners are distinguishable from above).
void addMirror2(fds::scene_builder::SceneBuilder &b, const char *name,
                float xR, float zR, float xL, float zL, float h,
                Material *mir, Material *backMat) {
    addWall(b, name, xR, zR, xL, zL, h, mir);
    // Back = reversed winding (opposite normal), nudged 0.06 along -normal.
    // normal(front) = normalize(zR-zL, 0, xL-xR).
    float nx = zR - zL, nz = xL - xR;
    const float L = std::sqrt(nx*nx + nz*nz);
    if (L > 1e-6f) { nx /= L; nz /= L; }
    const float e = 0.06f;
    char bn[64]; std::snprintf(bn, sizeof bn, "%s_back", name);
    addWall(b, bn, xL - nx*e, zL - nz*e, xR - nx*e, zR - nz*e, h, backMat);
}

// A 90-degree V-corner: two mirror panels joined at vertex (vx,vz), arms
// at openDeg±45 (so 90deg between them), opening toward openDeg. Each
// panel's mirror face points into the opening (toward the bisector), and
// carries an opaque back. Panel material names: "<prefix>_a" / "<prefix>_b".
void addVCorner(fds::scene_builder::SceneBuilder &b, const char *prefix,
                float vx, float vz, float openDeg, float arm, float h,
                Material *mirA, Material *mirB, Material *back) {
    const float br = openDeg * 3.14159265f / 180.0f;
    const float ndx = std::cos(br), ndz = std::sin(br);   // desired normal (bisector)
    struct { float a; const char *suf; Material *m; } arms[2] = {
        { openDeg + 45.0f, "_a", mirA }, { openDeg - 45.0f, "_b", mirB },
    };
    for (auto &ar : arms) {
        const float a = ar.a * 3.14159265f / 180.0f;
        const float ex = vx + arm * std::cos(a), ez = vz + arm * std::sin(a);
        // Order (R,L) so addWall normal (zR-zL, xL-xR) dots + with bisector.
        // Try R=vertex,L=end: n=(vz-ez, ex-vx).
        float nx = vz - ez, nz = ex - vx;
        char nm[64];
        std::snprintf(nm, sizeof nm, "%s%s", prefix, ar.suf);
        if (nx * ndx + nz * ndz >= 0.0f)
            addMirror2(b, nm, vx, vz, ex, ez, h, ar.m, back);   // R=vertex
        else
            addMirror2(b, nm, ex, ez, vx, vz, h, ar.m, back);   // R=end
    }
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
    // One mirror material PER PANEL (BuildMirror derives one plane per
    // material). Left V = mL_a/mL_b, right V = mR_a/mR_b.
    Material *mL_a = b.AddMaterial("mL_a", mirTex, {180,180,180,255}, 0);
    Material *mL_b = b.AddMaterial("mL_b", mirTex, {180,180,180,255}, 0);
    Material *mR_a = b.AddMaterial("mR_a", mirTex, {180,180,180,255}, 0);
    Material *mR_b = b.AddMaterial("mR_b", mirTex, {180,180,180,255}, 0);
    // Opaque backs: one color per V so orientation reads from above.
    Material *backL = b.AddMaterial("backL", b.AddSolidColorTexture(8,8,0xFF3060FFu), {48,96,255,255}, 0);
    Material *backR = b.AddMaterial("backR", b.AddSolidColorTexture(8,8,0xFFFF9020u), {255,144,32,255}, 0);
    Texture *redTex = b.AddSolidColorTexture(8, 8, 0xFFFF2020u);
    Material *matRed = b.AddMaterial("hidden_red", redTex, {255,32,32,255}, 0);

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

    // ── Two 90-degree V-corners, side by side (per the sketch) ────────
    // Each V = two mirrors joined at a right angle; both open toward the
    // viewer (-z). LEFT V (blue backs) is what the viewer sees; RIGHT V
    // (orange backs) hides behind the occluder wall and shows only inside
    // the left V's reflection. FIRST-DRAFT placement — tune from the
    // overhead view (FDS_CLOAK_DUMP writes /tmp/cloak_view_overhead.ppm).
    const float mh = 5.0f, arm = 4.0f;
    addVCorner(b, "vL", -3.0f, 15.0f, 0.0f, arm, mh, mL_a, mL_b, backL);
    addVCorner(b, "vR",  3.0f, 15.0f, 0.0f, arm, mh, mR_a, mR_b, backR);

    // Occluder: a z=5 wall (x 0.5..10, faces -z) hiding the RIGHT V from
    // the viewer while leaving the left V and the inter-V reflection open.
    addWall(b, "wall_occluder", 10.0f, 5.0f, 0.5f, 5.0f, H, matWall);

    // The cloaked object: red cube tucked behind the LEFT V's vertex.
    b.AddCube("hidden_red", Vector(-3.0f, 1.2f, 17.0f), 1.2f, matRed);

    // Lights.
    b.AddOmni(Vector(-6.0f, 7.0f, -4.0f), {255,255,255,0}, 1.0f, 80.0f);
    b.AddOmni(Vector( 6.0f, 7.0f,  6.0f), {255,255,255,0}, 1.0f, 80.0f);
    b.AddOmni(Vector( 0.0f, 7.0f, 20.0f), {255,255,255,0}, 0.9f, 80.0f);

    b.SetCamera(Vector(0.0f, 2.5f, -12.0f), Vector(0.0f, 2.5f, 12.0f), 60.0f);

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
        for (const char *nm : { "mL_a", "mL_b", "mR_a", "mR_b" }) {
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
        // Overhead from BEHIND (+z, high): the V opens toward -z so its
        // opaque backs face +z — from here they read as solid blue (left V)
        // + orange (right V) V-shapes. THE positioning view.
        { Vector( 0.0f, 22.0f, 30.0f), Vector( 0.0f, 0.0f, 13.0f), "overhead" },
        // Lower bird's-eye from behind for depth cue.
        { Vector( 7.0f, 12.0f, 27.0f), Vector( 0.0f, 1.0f, 13.0f), "birdseye" },
        // The viewer's shot: looking into the left V.
        { Vector( 0.0f, 2.5f, -12.0f), Vector( 0.0f, 2.5f, 14.0f), "viewer"   },
        // Reveal: red cube behind the left V vertex.
        { Vector(-10.0f, 6.0f, 4.0f),  Vector(-3.0f, 1.0f, 17.0f), "reveal"   },
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
