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

    // Floor: y=0, 20x20.
    const Vector floorV[4] = {
        Vector(-10.0f, 0.0f, -10.0f), Vector( 10.0f, 0.0f, -10.0f),
        Vector( 10.0f, 0.0f,  10.0f), Vector(-10.0f, 0.0f,  10.0f),
    };
    b.AddQuad("floor", floorV, matFloor);

    // Mirror panel IS the back wall at z=+5, 20 wide × 8 tall. No separate
    // back wall behind — the mirror system's clone faces sit BEHIND the
    // mirror plane (z>5) and would Z-fail against anything closer to
    // the camera at the same screen pixels.
    const Vector mirrorV[4] = {
        Vector( 10.0f, 0.0f, 5.0f), Vector(-10.0f, 0.0f, 5.0f),
        Vector(-10.0f, 8.0f, 5.0f), Vector( 10.0f, 8.0f, 5.0f),
    };
    b.AddQuad("mirror_panel", mirrorV, matMirror);

    // Half-silvered side mirror panel at x=-7 (left wall). The room is
    // bigger than the mirror so the side mirror has space to live.
    // Uses Mat_Transparent + textured material → BuildMirror path A.
    const Vector xparMirrorV[4] = {
        Vector(-7.0f, 0.0f,  5.0f), Vector(-7.0f, 0.0f, -5.0f),
        Vector(-7.0f, 7.0f, -5.0f), Vector(-7.0f, 7.0f,  5.0f),
    };
    b.AddQuad("xpar_mirror_panel", xparMirrorV, matXparMirror);

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
