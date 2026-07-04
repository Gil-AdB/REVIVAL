// "Invisible mirror" (magician's Sphinx cabinet) test scene — the classic
// recursive-mirror stressor. Four mirror panels form a W: two V-pairs whose
// panels sit at 45° to the viewing axis. Each panel reflects the SIDE wall,
// which carries the same checker as the back wall — so from the on-axis
// sweet spot the panels read as empty room and the wedge BEHIND each V
// (where a bright cube hides) is invisible. Where the two Vs meet in the
// middle, the inner panels face each other at 90°: rays there bounce
// mirror→mirror, which only renders correctly with second-order
// reflections (--mirror-rtt) — that seam is the recursion test.
//
// Snapshot-only driver (no interactivity): builds the scene, renders a
// fixed pose set to /tmp/cloak_view_*.ppm, exits.
//   ./DEMO --scene-cloaktest [--mirror-rtt]
// Poses: onaxis (illusion holds, cubes hidden), offside (illusion breaks),
// overhead (cubes plainly visible), seam (close-up on the 90° corner).

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
    const char *mNames[4] = { "cloak_m1", "cloak_m2", "cloak_m3", "cloak_m4" };
    for (int i = 0; i < 4; ++i)
        matM[i] = b.AddMaterial(mNames[i], mirTex, {180,180,180,255}, 0);
    Texture *redTex   = b.AddSolidColorTexture(8, 8, 0xFFFF2020u);
    Texture *greenTex = b.AddSolidColorTexture(8, 8, 0xFF20FF20u);
    Material *matRed   = b.AddMaterial("hidden_red",   redTex,   {255,32,32,255}, 0);
    Material *matGreen = b.AddMaterial("hidden_green", greenTex, {32,255,32,255}, 0);

    // Floor 28x28 (winding per mirrortest: CCW from above → N=+Y).
    const Vector floorV[4] = {
        Vector(-14.0f, 0.0f, -14.0f), Vector(-14.0f, 0.0f, 14.0f),
        Vector( 14.0f, 0.0f,  14.0f), Vector( 14.0f, 0.0f, -14.0f),
    };
    b.AddQuad("floor", floorV, matFloor);

    // Room walls, height 8, normals INTO the room. Back wall faces -z
    // (viewer side): left→right seen from the room is +x→-x at z=12.
    const float H = 8.0f;
    addWall(b, "wall_back",  14.0f, 12.0f, -14.0f, 12.0f, H, matWall);
    // Left wall at x=-14 faces +x: seen from inside, left→right is z=12→z=-14.
    addWall(b, "wall_left", -14.0f, 12.0f, -14.0f, -14.0f, H, matWall);
    // Right wall at x=+14 faces -x.
    addWall(b, "wall_right", 14.0f, -14.0f, 14.0f, 12.0f, H, matWall);
    // BLUE wall BEHIND the camera (z=-14, faces +z). The central seam is
    // a 90° retroreflector: rays that bounce panel->panel return toward
    // the viewer side and land HERE. Single-bounce clones can't produce
    // it — blue in the seam = second-order reflections working. Facing
    // (0,0,+1): right = up x facing = -x, so right end first = x=-14.
    Texture *blueTex = makeChecker(0xFF2040C0u, 0xFF102060u);
    Material *matBlue = b.AddMaterial("wall_behind", blueTex, {32,64,192,255}, 0);
    addWall(b, "wall_behind", -14.0f, -14.0f, 14.0f, -14.0f, H, matBlue);

    // The W: V apexes toward the viewer at (±4, z=2); wings back to z=6.
    // Panels at 45° to the z axis, height 5. Normals face the VIEWER
    // (roughly -z): left→right as seen from the viewer.
    const float mh = 5.0f;
    addWall(b, "cloak_p1", -4.0f, 2.0f, -8.0f, 6.0f, mh, matM[0]);
    addWall(b, "cloak_p2",  0.0f, 6.0f, -4.0f, 2.0f, mh, matM[1]);
    addWall(b, "cloak_p3",  4.0f, 2.0f,  0.0f, 6.0f, mh, matM[2]);
    addWall(b, "cloak_p4",  8.0f, 6.0f,  4.0f, 2.0f, mh, matM[3]);

    // Hidden cubes in the wedges behind each V (between the panels and
    // z=6): invisible from on-axis if the illusion holds.
    b.AddCube("hidden_red",   Vector(-4.0f, 1.2f, 4.6f), 1.2f, matRed);
    b.AddCube("hidden_green", Vector( 4.0f, 1.2f, 4.6f), 1.2f, matGreen);

    // Lights: symmetric pair so the side walls are lit like the back wall
    // (an asymmetric key light would break the illusion by brightness).
    b.AddOmni(Vector(-7.0f, 7.0f, -6.0f), {255,255,255,0}, 1.0f, 60.0f);
    b.AddOmni(Vector( 7.0f, 7.0f, -6.0f), {255,255,255,0}, 1.0f, 60.0f);

    b.SetCamera(Vector(0.0f, 2.5f, -10.0f), Vector(0.0f, 2.5f, 6.0f), 60.0f);

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
        for (const char *nm : { "cloak_m1", "cloak_m2", "cloak_m3", "cloak_m4" }) {
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

    struct Pose { Vector eye, lookAt; const char *tag; };
    const Pose poses[] = {
        // The sweet spot: panels mirror the side walls ≈ back wall,
        // cubes hidden. THE money shot.
        { Vector( 0.0f, 2.5f, -10.0f), Vector(0.0f, 2.5f,  6.0f), "onaxis"   },
        // Off to the side: the reflection angle no longer matches the
        // background — the illusion breaks (panels visibly mirrors).
        { Vector(-11.0f, 3.5f, -8.0f), Vector(0.0f, 2.0f,  5.0f), "offside"  },
        // Overhead: look down into the wedges — both cubes plainly visible.
        { Vector( 0.0f, 12.0f, -4.0f), Vector(0.0f, 0.0f,  4.5f), "overhead" },
        // Close on the central 90° corner (p2/p3 seam): rays bounce
        // mirror->mirror; correct only with --mirror-rtt (A/B this pose).
        { Vector( 0.0f, 2.5f, -3.0f),  Vector(0.0f, 2.5f,  6.0f), "seam"     },
    };
    for (const Pose &p : poses) {
        char path[64];
        std::snprintf(path, sizeof(path), "/tmp/cloak_view_%s.ppm", p.tag);
        renderPoseToPPM(cs, p.eye, p.lookAt, path);
    }
}
