#include "MirrorTestDriver.h"

#include "FrameProfiler.h"
#include "GreetsMirror.h"
#include "Rev.h"
#include "SceneBuilder.h"
#include "SceneTick.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>

#include <cstdio>

extern void Scene_RebuildMatTable(Scene *Sc);

namespace {

// Builds the interactive test scene through SceneBuilder. Same geometry
// as the snapshot variant; an omni gets added so the deferred lighting
// kernel has something to do at the surfaces. Kept separate from the
// snapshot's MT_buildScene because the interactive one places the
// camera at a starting pose and adds a working light.
Scene *BuildInteractiveMirrorTestScene() {
    using namespace fds::scene_builder;
    SceneBuilder b;
    b.SetNearFar(0.5f, 200.0f);
    b.SetAmbient(96, 96, 96);

    Texture *floorTex = b.AddSolidColorTexture(8, 8, 0xFF606060u);
    Material *matFloor = b.AddMaterial("floor_mat", floorTex,
                                        {96, 96, 96, 255}, 0);
    Texture *mirTex = b.AddSolidColorTexture(8, 8, 0xFFC0C0C0u);
    Material *matMirror = b.AddMaterial("mirror_mat", mirTex,
                                         {180, 180, 180, 255}, 0);
    // Half-silvered side mirror — material is transparent FROM THE
    // START so BuildMirror takes path A (keep material untouched).
    // This is the case that broke greets's P_TEXT mirror: clones of
    // transparent source faces dispatch through Mekalele's transparent
    // target, and the transparent gbuffers' mirrorId planes were
    // never allocated, so the mask short-circuited and clones drew
    // anywhere on screen.
    Texture *xparTex = b.AddSolidColorTexture(8, 8, 0xFFB0B0FFu);
    Material *matXparMirror = b.AddMaterial("xpar_mirror_mat", xparTex,
                                             {160, 160, 220, 255},
                                             Mat_Transparent | Mat_TwoSided);
    matXparMirror->XparBlendAlpha = 0.3f;
    Texture *redTex = b.AddSolidColorTexture(8, 8, 0xFFE03030u);
    Material *matRed = b.AddMaterial("red_mat", redTex,
                                      {224, 32, 32, 255}, 0);
    Texture *greenTex = b.AddSolidColorTexture(8, 8, 0xFF30E030u);
    Material *matGreen = b.AddMaterial("green_mat", greenTex,
                                        {32, 224, 32, 255}, 0);
    Texture *yellowTex = b.AddSolidColorTexture(8, 8, 0xFFE0E020u);
    Material *matYellow = b.AddMaterial("yellow_mat", yellowTex,
                                         {224, 224, 32, 255}, 0);
    // Frame material — opaque bright orange. Used for the visual
    // border around each mirror panel so the mirror boundaries are
    // unmistakable; whatever's inside the frame is mirror surface,
    // everything outside is room.
    Texture *frameTex = b.AddSolidColorTexture(8, 8, 0xFFFF8000u);
    Material *matFrame = b.AddMaterial("frame_mat", frameTex,
                                        {255, 128, 0, 255}, 0);

    // Floor: y=0, 20x20. Winding wraps CCW as viewed from ABOVE so the
    // cross-product points +Y (up). The previous winding had it pointing
    // -Y, which made the floor back-facing for any camera above y=0 —
    // invisible in the actual scene, and visible AS A REFLECTED PLANE
    // in the mirror because the clone's normal kept the same sign and
    // the mirror viewpoint happened to put the camera on the visible
    // side.
    const Vector floorV[4] = {
        Vector(-10.0f, 0.0f, -10.0f), Vector(-10.0f, 0.0f,  10.0f),
        Vector( 10.0f, 0.0f,  10.0f), Vector( 10.0f, 0.0f, -10.0f),
    };
    b.AddQuad("floor", floorV, matFloor);

    // Back wall: opaque-source mirror at z=+5. BuildMirror path B —
    // synthesise a transparent silver material and retarget the wall
    // faces to it. Smaller than the room so the half-silvered side
    // mirror (below) is the dominant reflection from the default pose
    // and the overlap between the two mirrors' masks is minimal.
    const Vector mirrorV[4] = {
        Vector( 3.0f, 0.0f, 5.0f), Vector(-3.0f, 0.0f, 5.0f),
        Vector(-3.0f, 6.0f, 5.0f), Vector( 3.0f, 6.0f, 5.0f),
    };
    b.AddQuad("mirror_panel", mirrorV, matMirror);
    // Orange frame around the back mirror. Four thin coplanar strips at
    // z=5 surrounding the mirror quad. Same CCW winding as the mirror
    // panel so the frame's outward normal matches (N=(0,0,-1) toward the
    // room).
    constexpr float bF = 0.3f;  // back-mirror frame thickness
    const Vector bm_top[4] = {
        Vector( 3.0f+bF, 6.0f, 5.0f), Vector(-3.0f-bF, 6.0f, 5.0f),
        Vector(-3.0f-bF, 6.0f+bF, 5.0f), Vector( 3.0f+bF, 6.0f+bF, 5.0f),
    };
    const Vector bm_bot[4] = {
        Vector( 3.0f+bF, -bF, 5.0f), Vector(-3.0f-bF, -bF, 5.0f),
        Vector(-3.0f-bF, 0.0f, 5.0f), Vector( 3.0f+bF, 0.0f, 5.0f),
    };
    const Vector bm_left[4] = {
        Vector(-3.0f,    0.0f, 5.0f), Vector(-3.0f-bF, 0.0f, 5.0f),
        Vector(-3.0f-bF, 6.0f, 5.0f), Vector(-3.0f,    6.0f, 5.0f),
    };
    const Vector bm_right[4] = {
        Vector( 3.0f+bF, 0.0f, 5.0f), Vector( 3.0f,    0.0f, 5.0f),
        Vector( 3.0f,    6.0f, 5.0f), Vector( 3.0f+bF, 6.0f, 5.0f),
    };
    b.AddQuad("bm_frame_top",   bm_top,   matFrame);
    b.AddQuad("bm_frame_bot",   bm_bot,   matFrame);
    b.AddQuad("bm_frame_left",  bm_left,  matFrame);
    b.AddQuad("bm_frame_right", bm_right, matFrame);

    // Half-silvered side mirror panel at x=-7 (left wall). Uses
    // Mat_Transparent + textured material → BuildMirror path A
    // (source kept as-is). This is the path greets's P_TEXT screens
    // take, so verifying it here keeps the test honest.
    const Vector xparMirrorV[4] = {
        Vector(-7.0f, 0.0f,  5.0f), Vector(-7.0f, 0.0f, -5.0f),
        Vector(-7.0f, 7.0f, -5.0f), Vector(-7.0f, 7.0f,  5.0f),
    };
    b.AddQuad("xpar_mirror_panel", xparMirrorV, matXparMirror);
    // Orange frame around the side mirror. Same x=-7 plane, same CCW
    // winding pattern (N=(1,0,0) toward the room).
    constexpr float sF = 0.3f;  // side-mirror frame thickness
    const Vector sm_top[4] = {
        Vector(-7.0f, 7.0f,    5.0f+sF), Vector(-7.0f, 7.0f,   -5.0f-sF),
        Vector(-7.0f, 7.0f+sF,-5.0f-sF), Vector(-7.0f, 7.0f+sF, 5.0f+sF),
    };
    const Vector sm_bot[4] = {
        Vector(-7.0f, -sF, 5.0f+sF), Vector(-7.0f, -sF, -5.0f-sF),
        Vector(-7.0f, 0.0f,-5.0f-sF), Vector(-7.0f, 0.0f, 5.0f+sF),
    };
    const Vector sm_front[4] = {  // z>+5 strip
        Vector(-7.0f, 0.0f, 5.0f+sF), Vector(-7.0f, 0.0f, 5.0f),
        Vector(-7.0f, 7.0f, 5.0f),    Vector(-7.0f, 7.0f, 5.0f+sF),
    };
    const Vector sm_back[4] = {   // z<-5 strip
        Vector(-7.0f, 0.0f, -5.0f), Vector(-7.0f, 0.0f, -5.0f-sF),
        Vector(-7.0f, 7.0f, -5.0f-sF), Vector(-7.0f, 7.0f, -5.0f),
    };
    b.AddQuad("sm_frame_top",   sm_top,   matFrame);
    b.AddQuad("sm_frame_bot",   sm_bot,   matFrame);
    b.AddQuad("sm_frame_front", sm_front, matFrame);
    b.AddQuad("sm_frame_back",  sm_back,  matFrame);

    // Three distinct objects in front of the mirror so the reflection is
    // visually unambiguous: a red cube on the left, a green cube on the
    // right at different depth, a tall yellow pillar centered close.
    b.AddCube("red_cube",    Vector(-1.8f, 1.0f, 1.5f), 0.6f, matRed);
    b.AddCube("green_cube",  Vector( 1.8f, 1.0f, 3.0f), 0.6f, matGreen);
    b.AddCube("yellow_post", Vector( 0.0f, 1.5f, 2.0f), 0.3f, matYellow);

    // Single omni high in front so the cube faces are clearly lit and
    // the reflected cube faces in the mirror are clearly lit too.
    b.AddOmni(Vector(-2.0f, 5.0f, -1.0f),
              {255.0f, 240.0f, 220.0f, 255.0f},
              /*intensity=*/2.0f, /*range=*/30.0f);

    // Starting camera looking at the mirror panel.
    b.SetCamera(Vector(-3.5f, 3.0f, -3.0f),
                Vector(0.0f, 3.0f, 5.0f),
                60.0f);
    b.Finalize();
    return b.scene();
}

struct MirrorTestScene : SceneDriver {
    Scene *sc = nullptr;
    std::vector<fds::Mirror> mirrors;
    FrameProfiler prof{"mirrortest"};
    char MSGStr[192] = {0};
    float TTrd_ = -1.0f;
    // Labeled world positions — drawn each frame as "[O] name" at the
    // original position and "[R1 m<id>] name" at the position the
    // object reflects to through each mirror's plane. Lets us see at
    // a glance which clone on screen corresponds to which original,
    // and whether any clone lands outside its mirror's frame (which
    // would indicate a mask/projection bug).
    struct LabeledPos { Vector pos; const char *name; };
    std::vector<LabeledPos> labels;

    ~MirrorTestScene() override {
        // The builder-allocated scene contains a mix of new[]/aligned-
        // malloc objects that Destroy_Scene's FLD-tuned cleanup
        // doesn't know how to walk. Leak on exit is fine for an
        // interactive test scene.
    }

    void init() override {
        sc = BuildInteractiveMirrorTestScene();
        SetCurrentScene(sc);
        Calibrate_FreeCamera_ForScene(sc->FZP, sc->CameraHead);

        // Build the back-wall mirror (path B — opaque source synthesised
        // into transparent silver) and the side mirror (path A — already
        // transparent material, kept as half-silvered glass).
        fds::Mirror m = fds::BuildMirror(sc, "mirror_mat");
        if (m.cloneMesh) mirrors.push_back(std::move(m));
        fds::Mirror m2 = fds::BuildMirror(sc, "xpar_mirror_mat");
        if (m2.cloneMesh) mirrors.push_back(std::move(m2));

        // setupFaceLists wires the global View alias to sc->CameraHead
        // (Transform_Objects derefs view->Mat through that), sizes the
        // FList container, and stamps the scene's near/far for the
        // engine-wide C_FZP / C_rFZP. Without it Transform_Objects
        // crashes on a null view at MatrixXMatrix.
        setupFaceLists(sc, /*includeOmnisInCount=*/true);
        // Bump the FList capacity to also cover the mirror clone mesh.
        DWord polys = 0;
        for (TriMesh *T = sc->TriMeshHead; T; T = T->Next) polys += T->FIndex;
        for (Omni *O = sc->OmniHead; O; O = O->Next) ++polys;
        fds::g_mainFaces.resize(polys * 2 + 32);
        // BuildMirror cloned the wall material; refresh matTable.
        Scene_RebuildMatTable(sc);

        Ambient_Factor = 1.0f;
        Diffusive_Factor = 1.0f;
        Specular_Factor = 1.0f;
        ImageSize = 1;

        // Seed labeled positions matching the cubes added in
        // BuildInteractiveMirrorTestScene. Each gets an [O] label at
        // its original position and a [R1 m<id>] label at each
        // mirror's reflected position. (The current planar-mirror
        // system is single-bounce, so reflection count is always 0
        // or 1.)
        labels.push_back({Vector(-1.8f, 1.0f, 1.5f), "red"});
        labels.push_back({Vector( 1.8f, 1.0f, 3.0f), "green"});
        labels.push_back({Vector( 0.0f, 1.5f, 2.0f), "yellow"});

        // Start in free-cam. The whole point of an interactive mirror test is
        // to fly around the panel; defaulting to the scripted camera (no
        // splines) leaves the user wondering why WASD doesn't move them.
        // Seed FC from the scripted camera's pose so we start where the
        // scene was authored to look.
        FC.ISource = sc->CameraHead->ISource;
        FC.IFOV    = sc->CameraHead->IFOV;
        Matrix_Copy(FC.Mat, sc->CameraHead->Mat);
        CalcPersp(&FC);
        View = &FC;

        std::fprintf(stderr,
            "[MIRRORTEST] interactive scene ready — starting in FREE-CAM. "
            "WASD/QE to move, arrows to look, TAB to toggle scripted cam, "
            "ESC to exit.\n");
    }

    // Project a world point to screen, returns false if behind near
    // plane or off-screen.
    bool worldToScreen(const Vector &world, int &px, int &py) {
        const Vector rel = {world.x - View->ISource.x,
                            world.y - View->ISource.y,
                            world.z - View->ISource.z};
        Vector v;
        MatrixXVector(View->Mat, &rel, &v);
        if (v.z <= sc->NZP) return false;
        const float fx = v.x / v.z * FOVX + CntrEX;
        const float fy = -v.y / v.z * FOVY + CntrEY;
        if (fx < 0 || fx >= float(XRes) || fy < 0 || fy >= float(YRes))
            return false;
        px = int(fx); py = int(fy);
        return true;
    }

    // World-space label overlay. Draws "[O] name" at each labeled
    // object's actual position and "[R1 m<id>] name" at the position
    // it reflects to through each mirror's plane. Single-bounce, so
    // reflection count is always 0 or 1; the [R1 ...] prefix leaves
    // room for a future recursive-mirror pass to bump it to R2/R3.
    void drawLabels() {
        char lbl[64];
        int sx = 0, sy = 0;
        auto reflect = [](const Vector &p, const fds::Mirror &m) -> Vector {
            const float k = 2.0f * (m.plane.N.x * p.x + m.plane.N.y * p.y
                                  + m.plane.N.z * p.z + m.plane.d);
            return { p.x - k * m.plane.N.x,
                     p.y - k * m.plane.N.y,
                     p.z - k * m.plane.N.z };
        };
        for (const auto &L : labels) {
            std::snprintf(lbl, sizeof(lbl), "[O] %s", L.name);
            if (worldToScreen(L.pos, sx, sy)) OutTextXY(VPage, sx, sy, lbl, 255);
            for (const auto &mA : mirrors) {
                if (!mA.plane.valid) continue;
                // Single bounce: reflect L through mA.
                const Vector r1 = reflect(L.pos, mA);
                std::snprintf(lbl, sizeof(lbl), "[R1 m%d] %s",
                              (int)mA.id, L.name);
                if (worldToScreen(r1, sx, sy))
                    OutTextXY(VPage, sx, sy, lbl, 255);
                // Two-bounce: reflect through mB then through mA — the
                // position the object would appear at "inside the
                // reflected mirror" if the system did recursive
                // reflections. Useful for spotting where you'd expect
                // a 2nd-order reflection if the engine supported it.
                for (const auto &mB : mirrors) {
                    if (!mB.plane.valid || mB.id == mA.id) continue;
                    const Vector via = reflect(L.pos, mB);
                    const Vector r2 = reflect(via, mA);
                    std::snprintf(lbl, sizeof(lbl), "[R2 m%d>m%d] %s",
                                  (int)mB.id, (int)mA.id, L.name);
                    if (worldToScreen(r2, sx, sy))
                        OutTextXY(VPage, sx, sy, lbl, 255);
                }
            }
        }
    }

    // Render the scene one frame at the given camera pose, then dump
    // VPage to a PPM. Used by the multi-view sanity dump path.
    void renderPoseToPPM(Vector eye, Vector lookAt, const char *path) {
        FC.ISource = eye;
        Vector lookCopy = lookAt;
        Kick_Camera(&eye, &lookCopy, 0.0f, FC.Mat);
        CalcPersp(&FC);
        View = &FC;
        std::memset(VPage, 0, PageSize);
        std::memset(ZPage16, 0, size_t(XRes) * size_t(YRes) * sizeof(word));
        Animate_Objects(sc, View);
        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
        fds::UpdateAllMirrors(sc, mirrors);
        fds::StampMirrorMasks(sc, mirrors);
        Lighting(sc);
        if (CAll) {
            Radix_Sort(FList, SList, CAll);
            Render(RenderPath::ForceDeferred);
        }
        drawLabels();
        std::FILE *f = std::fopen(path, "wb");
        if (!f) return;
        const int xr = (int)XRes, yr = (int)YRes;
        std::fprintf(f, "P6\n%d %d\n255\n", xr, yr);
        std::vector<unsigned char> row(xr * 3);
        for (int y = 0; y < yr; ++y) {
            const dword *src = (const dword*)((const byte*)VPage
                              + y * (int)VESA_BPSL);
            for (int x = 0; x < xr; ++x) {
                dword px = src[x];
                row[x*3+0] = (px >> 16) & 0xFF;
                row[x*3+1] = (px >>  8) & 0xFF;
                row[x*3+2] = (px      ) & 0xFF;
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        std::fprintf(stderr,
            "[MIRRORTEST multi-dump] eye=(%.1f,%.1f,%.1f) -> %s\n",
            eye.x, eye.y, eye.z, path);
    }

    bool tick() override {
        if (Keyboard[ScESC] || Keyboard[ScBackSpace]) return false;
        // Multi-view sanity dump: ignores keyboard, renders 6 preset poses,
        // exits. Pose set chosen so the mirror panel is in view from
        // distinct angles (front, side, high, low, close, far) — eyeball
        // each to confirm the reflection lands sensibly.
        if (fds::FeatureFlags::mirrortest_multi_dump()) {
            struct Pose { Vector eye, lookAt; const char *tag; };
            const Pose poses[] = {
                { Vector(-3.5f, 3.0f, -3.0f), Vector( 0.0f, 3.0f,  5.0f), "default" },
                { Vector( 0.0f, 3.0f, -4.0f), Vector( 0.0f, 3.0f,  5.0f), "front" },
                { Vector(-7.0f, 3.0f,  1.0f), Vector( 0.0f, 3.0f,  5.0f), "left-side" },
                { Vector( 0.0f, 6.5f, -3.0f), Vector( 0.0f, 2.0f,  5.0f), "high" },
                { Vector( 0.0f, 1.0f, -3.0f), Vector( 0.0f, 4.0f,  5.0f), "low" },
                { Vector(-1.5f, 3.0f,  2.0f), Vector( 0.0f, 3.0f,  5.0f), "close" },
                // Camera crossed to the back side of the mirror (z > 5).
                // Reflections should NOT be visible here — the wall is
                // back-facing, and StampMirrorMasks's side-of-plane test
                // suppresses the mask entirely so Mekalele rejects every
                // clone pixel for this mirror.
                { Vector( 0.0f, 3.0f,  9.0f), Vector( 0.0f, 3.0f, -2.0f), "behind" },
                // Look at the half-silvered SIDE mirror from inside the
                // room. Side mirror is at x=-7 with N=(1,0,0); standing
                // at x=0 puts the viewer firmly in front of it.
                { Vector( 0.0f, 3.0f,  0.0f), Vector(-7.0f, 3.0f,  0.0f), "side-xpar" },
            };
            for (const Pose &p : poses) {
                char path[64];
                std::snprintf(path, sizeof(path), "/tmp/mt_view_%s.ppm", p.tag);
                renderPoseToPPM(p.eye, p.lookAt, path);
            }
            return false;
        }

        prof.beginFrame();
        prof.enter(PROF_ZCLR);

        g_FrameTime = Timer;

        tickTabToggle(sc, "mirrortest");
        clearFrame();

        prof.switchTo(PROF_ANIM);
        // Dynamic_Camera needs a sane dTime — it scales FV (free-cam
        // velocity vector) by dTime. Mimic RENDER.CPP:850's pattern:
        // delta since last tick * 0.25 (the legacy fudge that makes
        // every scene's WASD feel consistent).
        if (TTrd_ > 0) {
            dTime = (Timer - TTrd_) * 0.25f;
        } else {
            dTime = 0;
        }
        TTrd_ = Timer;
        if (View == &FC) Dynamic_Camera();
        if (Keyboard[ScC]) {
            FC.ISource = View->ISource;
            Matrix_Copy(FC.Mat, View->Mat);
            FC.IFOV = View->IFOV;
        }
        Animate_Objects(sc, View);

        prof.switchTo(PROF_XFRM);
        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
        fds::UpdateAllMirrors(sc, mirrors);
        fds::StampMirrorMasks(sc, mirrors);

        prof.switchTo(PROF_LGHT);
        Lighting(sc);
        prof.leave(PROF_LGHT);

        if (CAll) {
            prof.enter(PROF_SORT);
            Radix_Sort(FList, SList, CAll);
            prof.switchTo(PROF_RNDR);
            // Force deferred: the mirror system's clone-mask (Mekalele's
            // per-pixel gb.mirrorId == mirrorTag) only fires on the
            // deferred path. Forward renders the clone faces into the
            // opaque pass without any mask, then the back wall (closer
            // Z) overwrites them — the user sees solid wall instead of
            // a reflection. Native default for FDS_DEFERRED is OFF
            // (FDS_DEFERRED_DEFAULT_ON=1 is only set in the EMSCRIPTEN
            // branch of FDS/CMakeLists.txt), so we force it here rather
            // than asking the user to remember --deferred.
            Render(RenderPath::ForceDeferred);
            prof.leave(PROF_RNDR);
        }

        // Cheap on-screen HUD: camera mode + pose + active mirror count.
        std::snprintf(MSGStr, sizeof(MSGStr),
                      "[%s]  Mirrors:%zu  Cam=(%.1f,%.1f,%.1f)  "
                      "WASD/QE move, arrows look, TAB toggle, ESC exit",
                      (View == &FC) ? "FREE" : "SCRIPTED",
                      mirrors.size(),
                      View->ISource.x, View->ISource.y, View->ISource.z);
        OutTextXY(VPage, 0, 0, MSGStr, 255);
        drawLabels();
        // F2 toggles the mirrorId mask overlay. Lets you see exactly
        // which screen pixels belong to which mirror — when the frame
        // goes off-screen at close camera distances the mirror's
        // screen footprint isn't otherwise visible.
        {
            static bool sF2Prev = false;
            static bool sMaskOn = false;
            const bool f2Now = Keyboard[ScF2] != 0;
            if (f2Now && !sF2Prev) sMaskOn = !sMaskOn;
            sF2Prev = f2Now;
            if (sMaskOn) fds::DebugOverlayMirrorMask(sc);
        }

        prof.enter(PROF_FLIP);
        Flip(MainSurf);
        prof.leave(PROF_FLIP);
        prof.endFrame();
        return true;
    }

    void cleanup() override {
        if (g_profilerActive) prof.dump();
        waitBackspaceRelease();
    }
};

}  // namespace

std::unique_ptr<SceneDriver> createMirrorTestScene() {
    return std::make_unique<MirrorTestScene>();
}

void Run_MirrorTest() {
    auto scene = createMirrorTestScene();
    runSceneBlocking(*scene);
}
