// "Invisible mirror" cloak — the real 4-mirror periscope, per the user's
// verified sketch. TWO 90-degree V-corners side by side, BOTH opening +x
// (addVCorner joins two panels at a right-angle vertex; DOUBLE-SIDED:
// mirror front + opaque colored back so the layout reads from above):
//   LEFT V  @ (-6,12): mirror on the INNER 90° faces — a retroreflector.
//   RIGHT V @ ( 2,12): mirror on the OUTER 270° faces — two 45° turns.
// The viewer sits below the right V looking UP (+z). A viewer ray relays:
//   up → right V lower-OUTER (deflect left) → left corridor → left V
//   INNER corner (retroreflect) → right corridor → right V upper-OUTER
//   (deflect up) → background.
// It detours around the right V's mouth, where the hero object hides —
// unhittable by any viewer ray (2D-ray-trace verified: 41/41 rays reach
// the background, 0 hit the object). The see-through needs 4 reflection
// orders to fully resolve, so this doubles as a recursion-depth test.
//
// Default: interactive free-cam from the viewer pose. FDS_CLOAK_DUMP=1 =
// headless pose dump (viewer / overhead / reveal) to /tmp/cloak_view_*.ppm.
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

// A 90-degree V-corner: two mirror panels joined at vertex (vx,vz), arms
// at openDeg±45 (so 90deg between them), opening toward openDeg. Each panel
// carries an opaque back. mirrorInner=true → the reflecting face is the
// INNER concave side (facing the bisector); false → the OUTER convex side.
void addVCorner(fds::scene_builder::SceneBuilder &b, const char *prefix,
                float vx, float vz, float openDeg, float arm, float h,
                bool mirrorInner, Material *mirA, Material *mirB, Material *back) {
    const float br = openDeg * 3.14159265f / 180.0f;
    const float ndx = std::cos(br), ndz = std::sin(br);   // bisector (opening dir)
    const float sgn = mirrorInner ? 1.0f : -1.0f;         // mirror faces sgn*bisector
    const float gap = 0.06f;                              // opaque back set-back
    Material *mir[2] = { mirA, mirB };
    const char *suf[2] = { "_a", "_b" };
    float ex[2], ez[2], nx[2], nz[2];  // arm ends + unit front normals
    // 1) mirror fronts. Order (R,L) so the mirror normal faces sgn*bisector.
    for (int i = 0; i < 2; ++i) {
        const float a = (openDeg + (i ? -45.0f : 45.0f)) * 3.14159265f / 180.0f;
        ex[i] = vx + arm * std::cos(a); ez[i] = vz + arm * std::sin(a);
        const float dotVR = (vz - ez[i]) * (sgn*ndx) + (ex[i] - vx) * (sgn*ndz);
        char nm[64]; std::snprintf(nm, sizeof nm, "%s%s", prefix, suf[i]);
        float rx, rz, lx, lz;
        if (dotVR > 0.0f) { rx=vx; rz=vz; lx=ex[i]; lz=ez[i]; }   // R=vertex
        else              { rx=ex[i]; rz=ez[i]; lx=vx; lz=vz; }   // R=end
        addWall(b, nm, rx, rz, lx, lz, h, mir[i]);
        float n0 = rz - lz, n1 = lx - rx, L = std::sqrt(n0*n0 + n1*n1);
        nx[i] = n0 / L; nz[i] = n1 / L;   // placed front normal (faces the mirror side)
    }
    // 2) opaque backs meeting at ONE shared point behind the vertex (avoids
    // the two per-arm offsets crossing at the joined vertex). Back normal
    // faces AWAY from the mirror (reversed winding).
    float ax = nx[0]+nx[1], az = nz[0]+nz[1], al = std::sqrt(ax*ax+az*az);
    if (al > 1e-6f) { ax/=al; az/=al; }
    const float vbx = vx - ax*gap, vbz = vz - az*gap;   // shared back-vertex
    for (int i = 0; i < 2; ++i) {
        const float ebx = ex[i] - nx[i]*gap, ebz = ez[i] - nz[i]*gap;
        char nm[64]; std::snprintf(nm, sizeof nm, "%s%s_back", prefix, suf[i]);
        addWall(b, nm, ebx, ebz, vbx, vbz, h, back);   // reversed vs front → back-facing
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

    // Prettier palette (pbrtest-style): warm checker floor, cool checker
    // walls, a distinct colored back wall for the mirrors to reflect.
    Texture *wallTex  = makeChecker(0xFF9AA6C0u, 0xFF4A5578u);   // cool blue-grey
    Texture *floorTex = makeChecker(0xFFB0A090u, 0xFF5E5348u);   // warm taupe
    Texture *bgTex    = makeChecker(0xFF70C0A0u, 0xFF2E6B54u);   // teal back wall
    Material *matWall  = b.AddMaterial("wall_mat",  wallTex,  {154,166,192,255}, 0);
    Material *matFloor = b.AddMaterial("floor_mat", floorTex, {176,160,144,255}, 0);
    matFloor->Specular = 0.25f; matFloor->Glossiness = 32;
    Material *matBg    = b.AddMaterial("bg_mat",    bgTex,    {112,192,160,255}, 0);
    Texture *mirTex = b.AddSolidColorTexture(8, 8, 0xFFC8CCD4u);
    // One mirror material PER PANEL (BuildMirror derives one plane per mat).
    Material *mL_a = b.AddMaterial("mL_a", mirTex, {200,204,212,255}, 0);
    Material *mL_b = b.AddMaterial("mL_b", mirTex, {200,204,212,255}, 0);
    Material *mR_a = b.AddMaterial("mR_a", mirTex, {200,204,212,255}, 0);
    Material *mR_b = b.AddMaterial("mR_b", mirTex, {200,204,212,255}, 0);
    // Opaque backs: one color per V so orientation reads from above.
    Material *backL = b.AddMaterial("backL", b.AddSolidColorTexture(8,8,0xFF3060FFu), {48,96,255,255}, 0);
    Material *backR = b.AddMaterial("backR", b.AddSolidColorTexture(8,8,0xFFFF9020u), {255,144,32,255}, 0);
    // The cloaked "hero" object: a glossy reflective sphere.
    Material *matHero = b.AddMaterial("hero", b.AddSolidColorTexture(8,8,0xFFE04030u), {224,64,48,255}, 0);
    matHero->Specular = 0.9f; matHero->Glossiness = 96; matHero->Reflection = 40.0f;

    // Floor spans the whole arena.
    const Vector floorV[4] = {
        Vector(-16.0f, 0.0f, -16.0f), Vector(-16.0f, 0.0f, 30.0f),
        Vector( 16.0f, 0.0f,  30.0f), Vector( 16.0f, 0.0f, -16.0f),
    };
    b.AddQuad("floor", floorV, matFloor);
    // Ceiling (normal down) so mirrors that reflect UPWARD hit a surface
    // instead of the empty void (the black patches in the mirror view).
    const Vector ceilV[4] = {
        Vector(-16.0f, 8.0f, -16.0f), Vector( 16.0f, 8.0f, -16.0f),
        Vector( 16.0f, 8.0f,  30.0f), Vector(-16.0f, 8.0f,  30.0f),
    };
    b.AddQuad("ceiling", ceilV, matWall);

    // Room shell, height 8, normals into the room. Back wall (z=28) is the
    // teal the cloak's see-through should reveal.
    const float H = 8.0f;
    addWall(b, "wall_back",   16.0f, 28.0f, -16.0f, 28.0f, H, matBg);    // faces -z (teal)
    addWall(b, "wall_left",  -16.0f, 28.0f, -16.0f, -16.0f, H, matWall); // faces +x
    addWall(b, "wall_right",  16.0f, -16.0f, 16.0f,  28.0f, H, matWall); // faces -x

    // "wall" from the sketch: hides the LEFT V (and the left corridor) from
    // any direct front view — the left V only appears via the right V's
    // reflection. At z=8 (below the corridors at z~10.5/13.5 so it doesn't
    // clip the relay), x from -15 to -1, facing -z.
    addWall(b, "wall_hide", -1.0f, 8.0f, -15.0f, 8.0f, H, matWall);

    // BACKGROUND reference object: a bright green pillar behind the whole
    // contraption on the viewer's axis (x=3.5, z=26). This is the "visible
    // behind the mirrors" object — the relayed see-through should show it,
    // while the hero in the mouth (below) disappears.
    Material *matBgObj = b.AddMaterial("bg_obj", b.AddSolidColorTexture(8,8,0xFF30E060u), {48,224,96,255}, 0);
    b.AddCube("bg_obj", Vector(3.5f, 2.5f, 26.0f), 1.6f, matBgObj);

    // ── The cloak: two 90-degree V-corners, both open +x (validated in a
    // 2D ray-trace — viewer↑ relays around the right V's OUTER faces via
    // two corridors and returns to its original line; the object in the
    // right V's mouth is never hit). ──────────────────────────────────
    //   LEFT V  @ (-6,12): mirror on INNER 90° faces (the retroreflector).
    //   RIGHT V @ ( 2,12): mirror on OUTER 270° faces (the two 45° turns).
    // arm=4 → arm x/z extent 2.83, matching the traced geometry.
    const float mh = 5.0f, arm = 4.0f;
    addVCorner(b, "vL", -6.0f, 12.0f, 0.0f, arm, mh, /*mirrorInner=*/true,  mL_a, mL_b, backL);
    addVCorner(b, "vR",  2.0f, 12.0f, 0.0f, arm, mh, /*mirrorInner=*/false, mR_a, mR_b, backR);

    // Hero object hidden in the RIGHT V's mouth. Small + centered in the
    // shielded wedge: at column x the lower arm sits at z=14-x and the upper
    // at z=10+x, so a box in x[3,4] must live in z(11,13) — cube half 0.5 at
    // (3.5,·,12) fits with margin (ray-trace confirmed 0 viewer-ray hits).
    b.AddCube("hero", Vector(3.5f, 0.9f, 12.0f), 0.5f, matHero);

    // Colored key/fill lights (pbrtest-style warm + cool).
    b.AddOmni(Vector(-8.0f, 7.0f, 2.0f),  {255,232,200,0}, 1.0f, 90.0f);   // warm
    b.AddOmni(Vector( 9.0f, 7.0f, 16.0f), {200,224,255,0}, 0.9f, 90.0f);   // cool
    b.AddOmni(Vector( 3.5f, 6.0f, 6.0f),  {255,255,255,0}, 0.7f, 60.0f);   // fill on the mouth

    // Viewer sits below the RIGHT V's lower arm, looking straight up (+z).
    b.SetCamera(Vector(3.5f, 2.5f, 4.0f), Vector(3.5f, 2.5f, 40.0f), 55.0f);

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
        // The money shot: the viewer pose, looking up into the right V.
        { Vector( 3.5f, 2.5f, 4.0f), Vector( 3.5f, 2.5f, 40.0f), "viewer"   },
        // Overhead from the -x side (the V backs face -x): shows both V's
        // + the corridors + the hero in the right V's mouth. Positioning.
        { Vector(-17.0f, 20.0f, 12.0f), Vector( 0.0f, 0.0f, 12.0f), "overhead" },
        // Reveal: the hero sphere in the right V's mouth, from the +x side.
        { Vector( 12.0f, 5.0f, 12.0f),  Vector( 3.6f, 1.0f, 12.0f), "reveal"   },
        // User break poses: probe1 = a mirror shows the hero (should be
        // hidden); probe2 = mirror-face intersection.
        { Vector(1.70f,2.50f,11.65f), Vector(-8.30f,2.50f,11.84f), "probe1" },
        { Vector(3.56f,9.70f,11.62f), Vector(-0.56f,0.59f,11.69f), "probe2" },
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

        // 'G' (rising edge): dump the current camera pose as a ready-to-paste
        // Pose{} line (eye + lookAt one unit along forward) so a break pose
        // can be reproduced exactly headless (FDS_CLOAK_DUMP + add to poses[]).
        {
            static bool gPrev = false;
            const bool gNow = Keyboard[ScG] != 0;
            if (gNow && !gPrev) {
                const Vector &e = FC.ISource;
                const float fx = FC.Mat[2][0], fy = FC.Mat[2][1], fz = FC.Mat[2][2];
                std::fprintf(stderr,
                    "[CLOAK-POSE] { Vector(%.2ff,%.2ff,%.2ff), "
                    "Vector(%.2ff,%.2ff,%.2ff), \"probe\" },  // fwd=(%.2f,%.2f,%.2f)\n",
                    e.x, e.y, e.z, e.x+fx*10, e.y+fy*10, e.z+fz*10, fx, fy, fz);
            }
            gPrev = gNow;
        }

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
                      "WASD/QE move, arrows look, G=dump pose, ESC exit",
                      FC.ISource.x, FC.ISource.y, FC.ISource.z,
                      cs.mirrors.size(), cs.rttSlots.size());
        OutTextXY(VPage, 0, 0, msg, 255);
        Flip(MainSurf);
    }
}
