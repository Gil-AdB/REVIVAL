#include "MirrorTestDriver.h"

#include "FrameProfiler.h"
#include "GreetsMirror.h"
#include "Rev.h"
#include "SceneBuilder.h"
#include "SceneTick.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>

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
    Texture *wallTex = b.AddSolidColorTexture(8, 8, 0xFF2030C0u);
    Material *matWall = b.AddMaterial("wall_mat", wallTex,
                                       {192, 48, 32, 255}, 0);
    Texture *mirTex = b.AddSolidColorTexture(8, 8, 0xFFC0C0C0u);
    Material *matMirror = b.AddMaterial("mirror_mat", mirTex,
                                         {180, 180, 180, 255}, 0);
    Texture *cubeTex = b.AddSolidColorTexture(8, 8, 0xFF20E0E0u);
    Material *matCube = b.AddMaterial("cube_mat", cubeTex,
                                       {224, 224, 32, 255}, 0);

    // Floor: y=0, 20x20.
    const Vector floorV[4] = {
        Vector(-10.0f, 0.0f, -10.0f), Vector( 10.0f, 0.0f, -10.0f),
        Vector( 10.0f, 0.0f,  10.0f), Vector(-10.0f, 0.0f,  10.0f),
    };
    b.AddQuad("floor", floorV, matFloor);

    // Back wall at z=+6, 20 wide × 8 tall.
    const Vector wallV[4] = {
        Vector( 10.0f, 0.0f, 6.0f), Vector(-10.0f, 0.0f, 6.0f),
        Vector(-10.0f, 8.0f, 6.0f), Vector( 10.0f, 8.0f, 6.0f),
    };
    b.AddQuad("back_wall", wallV, matWall);

    // Mirror panel at z=+5, 4 wide × 5 tall.
    const Vector mirrorV[4] = {
        Vector( 2.0f, 1.0f, 5.0f), Vector(-2.0f, 1.0f, 5.0f),
        Vector(-2.0f, 6.0f, 5.0f), Vector( 2.0f, 6.0f, 5.0f),
    };
    b.AddQuad("mirror_panel", mirrorV, matMirror);

    // Test cube centered between camera and the mirror.
    b.AddCube("test_cube", Vector(0.0f, 1.6f, 2.0f), 0.8f, matCube);

    // Single omni in front + above so the cube + walls light up.
    b.AddOmni(Vector(-2.0f, 5.0f, -1.0f),
              {255.0f, 240.0f, 220.0f, 255.0f},
              /*intensity=*/1.5f, /*range=*/30.0f);

    // Starting camera looking at the mirror panel.
    b.SetCamera(Vector(-3.5f, 3.0f, -3.0f),
                Vector(0.0f, 3.0f, 3.0f),
                60.0f);
    b.Finalize();
    return b.scene();
}

struct MirrorTestScene : SceneDriver {
    Scene *sc = nullptr;
    std::vector<fds::Mirror> mirrors;
    FrameProfiler prof{"mirrortest"};
    char MSGStr[128] = {0};

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

        // Build the mirror across the mirror panel.
        fds::Mirror m = fds::BuildMirror(sc, "mirror_mat");
        if (m.cloneMesh) mirrors.push_back(std::move(m));

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

        std::fprintf(stderr,
            "[MIRRORTEST] interactive scene ready — TAB to free-cam, "
            "WASD/QE to move, arrows to look. ESC to exit.\n");
    }

    bool tick() override {
        if (Keyboard[ScESC] || Keyboard[ScBackSpace]) return false;

        prof.beginFrame();
        prof.enter(PROF_ZCLR);

        g_FrameTime = Timer;

        tickTabToggle(sc, "mirrortest");
        clearFrame();

        prof.switchTo(PROF_ANIM);
        Dynamic_Camera();
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
            Render();
            prof.leave(PROF_RNDR);
        }

        // Cheap on-screen HUD: camera pose + active mirror count.
        std::snprintf(MSGStr, sizeof(MSGStr),
                      "Mirrors: %zu  Cam=(%.1f,%.1f,%.1f)",
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
