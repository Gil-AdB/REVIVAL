#include "Snapshot.h"

#include "CITY.H"
#include "FillerTest.h"
#include "GLAT.H"
#include "Rev.h"
#include "Scenes.h"
#include "SceneTick.h"
#include "SpotlightCones.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>
#include <FILLERS/Mekalele.h>
#include <FILLERS/ShadowMap.h>
#include <RENDER/LightmapBake.h>
#include <Threads.h>
#include <VESA/Vesa.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/stat.h>

#ifndef __EMSCRIPTEN__
#include <SDL.h>
#include "SDL2.h"
#endif

extern dword g_profilerActive;

// Test-harness hook implemented in GREETS.CPP. Computes centroids of
// the greets generator's intermediate buffers (CodeImage, CodeBuf,
// ScaledBuf, OldBuf) for diffing native vs wasm output.
extern "C" void Greets_DumpStageCentroids(FILE *out, const char *tag);

namespace {

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

void parse_timestamps(std::string_view s, std::vector<int32_t>& out) {
    // Accepts "N1,N2,N3"
    std::size_t pos = 0;
    while (pos <= s.size()) {
        std::size_t end = s.find(',', pos);
        if (end == std::string_view::npos) end = s.size();
        if (end > pos) {
            std::string token(s.substr(pos, end - pos));
            char* endp = nullptr;
            long v = std::strtol(token.c_str(), &endp, 10);
            if (endp && *endp == '\0') {
                out.push_back(static_cast<int32_t>(v));
            } else {
                std::fprintf(stderr, "[SNAPSHOT] ignoring non-integer timestamp '%s'\n",
                             token.c_str());
            }
        }
        pos = end + 1;
    }
}

void write_ppm(const char* path, const byte* bgra, int xres, int yres, int bpsl) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr, "[SNAPSHOT] fopen('%s') failed: %s\n", path, std::strerror(errno));
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", xres, yres);
    std::vector<unsigned char> row(xres * 3);
    for (int y = 0; y < yres; ++y) {
        const dword* src = reinterpret_cast<const dword*>(bgra + y * bpsl);
        for (int x = 0; x < xres; ++x) {
            // VPage is ARGB8888 dwords; PPM wants RGB.
            dword px = src[x];
            row[x * 3 + 0] = (px >> 16) & 0xFF; // R
            row[x * 3 + 1] = (px >>  8) & 0xFF; // G
            row[x * 3 + 2] = (px      ) & 0xFF; // B
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[SNAPSHOT] wrote %s\n", path);
}

void write_pgm16(const char* path, const word* z, int xres, int yres) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr, "[SNAPSHOT] fopen('%s') failed: %s\n", path, std::strerror(errno));
        return;
    }
    std::fprintf(f, "P5\n%d %d\n65535\n", xres, yres);
    std::vector<unsigned char> row(xres * 2);
    for (int y = 0; y < yres; ++y) {
        for (int x = 0; x < xres; ++x) {
            // PGM 16-bit is big-endian.
            word v = z[y * xres + x];
            row[x * 2 + 0] = (v >> 8) & 0xFF;
            row[x * 2 + 1] = v & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[SNAPSHOT] wrote %s\n", path);
}

void noop_flip(VESA_Surface*) {}

// Bootstrap the FDS/VESA pipeline + ThreadPool without touching SDL or
// Modplayer. Both city and glat snapshot modes share this. Returns false
// on failure.
bool initSnapshotEnvironment(int xres, int yres) {
    if (!FDS_Init(static_cast<unsigned short>(xres),
                  static_cast<unsigned short>(yres), 32)) {
        std::fprintf(stderr, "[SNAPSHOT] FDS_Init failed\n");
        return false;
    }

    static VESA_Surface surf = {};
    surf.X = xres;
    surf.Y = yres;
    surf.BPP = 32;
    surf.CPP = 4;
    surf.BPSL = surf.CPP * surf.X;
    surf.PageSize = surf.BPSL * surf.Y;
    const std::size_t zSize = sizeof(word) * static_cast<std::size_t>(xres) * yres;
    surf.Data = static_cast<byte*>(std::malloc(surf.PageSize));
    surf.Z16  = static_cast<byte*>(std::malloc(zSize));
    if (!surf.Data || !surf.Z16) {
        std::fprintf(stderr, "[SNAPSHOT] malloc framebuffer / Z16 failed\n");
        return false;
    }
    std::memset(surf.Data, 0, surf.PageSize);
    std::memset(surf.Z16,  0, zSize);
    surf.Flip = &noop_flip;

    VESA_VPageExternal(&surf);
    VESA_Surface2Global(MainSurf);

    // Match V_Create's lifecycle: size the deferred G-buffer to the
    // snapshot dimensions so RenderInnerMekalele has storage to write.
    EngineGBuffer_Resize(xres, yres);

    Generate_RGBFlares();
    InitPolyStats(200);
    ThreadPool::instance().init([]() {
        InitPolyStats(200);
        FPU_LPrecision();
    });
    FPU_LPrecision();

    g_profilerActive = 0;
    return true;
}

void ensureOutDir(const std::string& outDir) {
    if (outDir.empty() || outDir == ".") return;
    // Recursive mkdir -p: walk the path creating each component. Plain
    // mkdir() fails if any ancestor is missing, which broke ctest paths
    // like build/smoke-out/city_fwd where the intermediate dir didn't
    // exist yet.
    std::string acc;
    acc.reserve(outDir.size());
    for (size_t i = 0; i <= outDir.size(); ++i) {
        if (i == outDir.size() || outDir[i] == '/') {
            if (!acc.empty() && acc != ".") {
#ifdef _WIN32
                _mkdir(acc.c_str());
#else
                mkdir(acc.c_str(), 0755);
#endif
            }
        }
        if (i < outDir.size()) acc.push_back(outDir[i]);
    }
}

} // namespace

bool ParseSnapshotArgs(int argc, const char* argv[], SnapshotConfig& cfg) {
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (starts_with(a, "--snapshot=")) {
            std::string_view rest = a.substr(strlen("--snapshot="));
            std::size_t at = rest.find('@');
            if (at == std::string_view::npos) {
                cfg.scene = std::string(rest);
            } else {
                cfg.scene = std::string(rest.substr(0, at));
                std::string_view tail = rest.substr(at + 1);
                if (starts_with(tail, "t=")) {
                    parse_timestamps(tail.substr(2), cfg.timestamps);
                }
            }
            found = true;
        } else if (starts_with(a, "--out=")) {
            cfg.outDir = std::string(a.substr(strlen("--out=")));
        }
    }
    // If no timestamps were passed, RunCitySnapshot picks an even sweep
    // across the scene's playable range once it knows CTPartTime.
    return found;
}

static void buildLookAt(const Vector& eye, const Vector& target, Matrix outM);

int RunFountainSnapshot(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;
    // Fountain's tick uses RenderSkyCube(SkySc, ...) but SkySc is created
    // inside Initialize_City. Snapshot harness has to init both — the
    // interactive demo goes through Initialize_City first naturally.
    Initialize_City();
    Initialize_Fountain();

    std::vector<int32_t> timestamps = cfg.timestamps;
    if (timestamps.empty()) {
        // Default sweep: a few well-spaced moments across the fountain
        // scene's run.
        timestamps = {500, 1500, 2500, 3500, 4500};
    }

    // Optional camera override via env vars. Set FNTSNAP_POS / FNTSNAP_FWD
    // / FNTSNAP_FOV as comma-separated triples / scalar to pin the camera
    // to a known position regardless of where the scripted spline would
    // place it. Useful for reproducing user-reported view issues.
    bool overrideCam = false;
    Vector camPos(0,0,0), camFwd(0,0,1);
    float  camFOV = 60.0f;
    auto parseVec = [](const char* s, Vector& out) -> bool {
        float a, b, c;
        return s && std::sscanf(s, "%f,%f,%f", &a, &b, &c) == 3
            && (out = Vector(a, b, c), true);
    };
    if (const char* s = std::getenv("FNTSNAP_POS")) {
        if (parseVec(s, camPos)) overrideCam = true;
    }
    if (const char* s = std::getenv("FNTSNAP_FWD")) {
        parseVec(s, camFwd);
    }
    if (const char* s = std::getenv("FNTSNAP_FOV")) {
        camFOV = float(std::atof(s));
    }
    if (overrideCam) {
        std::fprintf(stderr,
            "[FNTSNAP] override cam pos=(%.1f,%.1f,%.1f) fwd=(%.3f,%.3f,%.3f) fov=%.1f\n",
            camPos.x, camPos.y, camPos.z, camFwd.x, camFwd.y, camFwd.z, camFOV);
    }

    auto driver = createFountainScene();
    driver->init();

    int produced = 0;
    for (int32_t ts : timestamps) {
        std::srand(0);
        Timer = ts;
        std::memset((void*)Keyboard, 0, sizeof(Keyboard));

        bool more = driver->tick();
        (void)more;

        if (overrideCam) {
            View->ISource = camPos;
            Vector_Norm(&camFwd);
            buildLookAt(camPos, Vector(camPos.x + camFwd.x,
                                        camPos.y + camFwd.y,
                                        camPos.z + camFwd.z), View->Mat);
            View->IFOV = camFOV;
            CalcPersp(View);
            FOVX = View->PerspX;
            FOVY = View->PerspY;
            std::fprintf(stderr,
                "[FNTSNAP] post-override View->Mat row 2 = (%.3f, %.3f, %.3f) ISource=(%.1f,%.1f,%.1f)\n",
                View->Mat[2][0], View->Mat[2][1], View->Mat[2][2],
                View->ISource.x, View->ISource.y, View->ISource.z);
            std::memset(VPage,   0, PageSize);
            std::memset(ZPage16, 0, XRes * YRes * sizeof(word));
            Transform_Objects(CurScene, fds::g_mainCamera, fds::g_mainFaces);
            std::fprintf(stderr, "[FNTSNAP] post-Transform CAll=%d CurScene=%p\n",
                int(CAll), (void*)CurScene);
            if (CAll) {
                Radix_Sort(FList, SList, CAll);
                Render();
            }
        }

        char colorPath[1024];
        std::snprintf(colorPath, sizeof(colorPath), "%s/fountain_t%06d_color.ppm",
                      cfg.outDir.c_str(), ts);
        write_ppm(colorPath, MainSurf->Data, xres, yres, MainSurf->BPSL);
        ++produced;
    }

    driver->cleanup();
    driver.reset();
    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}

int RunGreetsSnapshot(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;
    // Greets is self-contained — loads its own FLD, walks MatLib filtered
    // by RelScene==GreetSc, no SkySc references. (The earlier "needs
    // Initialize_City" comment was Fountain-pasted. The real hidden
    // coupling — global ::Polys not being set, breaking shadow per-light
    // face buffer sizing in Shadows.cpp — was fixed by removing the
    // local ::Polys shadow in GREETS.CPP:777.)
    Initialize_Greets();

    std::vector<int32_t> timestamps = cfg.timestamps;
    if (timestamps.empty()) {
        // Default sweep: one frame from each greet round so the harness
        // catches lighting issues across the full scene timeline.
        // Greets uses CurFrame which is derived from Timer; round boundaries
        // live around 350 / 730 / 900 / 1200 / 2000 / 2500 frames.
        timestamps = {100, 600, 1000, 1500, 2100};
    }

    auto driver = createGreetsScene();
    driver->init();

    int produced = 0;
    for (int32_t ts : timestamps) {
        std::srand(0);
        Timer = ts;
        std::memset((void*)Keyboard, 0, sizeof(Keyboard));

        bool more = driver->tick();
        (void)more;

        char colorPath[1024];
        std::snprintf(colorPath, sizeof(colorPath), "%s/greets_t%06d_color.ppm",
                      cfg.outDir.c_str(), ts);
        write_ppm(colorPath, MainSurf->Data, xres, yres, MainSurf->BPSL);
        std::fprintf(stderr, "[GREETSSNAP] t=%d -> %s\n", ts, colorPath);
        ++produced;
    }

    driver->cleanup();
    driver.reset();
    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}

int RunCitySnapshot(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    Initialize_City();

    const int32_t ctPart = getCityCTPartTime();
    std::fprintf(stderr, "[SNAPSHOT] City CTPartTime = %d (Timer must be < this for tick to render)\n",
                 ctPart);

    std::vector<int32_t> timestamps = cfg.timestamps;
    if (timestamps.empty() && ctPart > 0) {
        // Even sweep across the scene's playable range.
        for (int i = 1; i <= 9; i += 2) {
            timestamps.push_back(ctPart * i / 10);
        }
    }

    auto driver = createCityScene();
    driver->init();

    int produced = 0;
    for (int32_t ts : timestamps) {
        if (ctPart > 0 && ts >= ctPart) {
            std::fprintf(stderr,
                         "[SNAPSHOT] timestamp %d >= CTPartTime %d; clamping to %d\n",
                         ts, ctPart, ctPart - 1);
            ts = ctPart - 1;
        }
        // Reset rand() to a fixed seed each frame. Omni-light flicker and
        // similar effects use rand(); without this the snapshot is
        // non-deterministic across invocations even on the same build.
        std::srand(0);
        // TTrd = Timer makes dTime = 0 in tick(); avoids any branch tied to
        // wall-clock motion.
        Timer = ts;
        // Best-effort: clear any stale Keyboard state between frames.
        std::memset((void*)Keyboard, 0, sizeof(Keyboard));

        bool more = driver->tick();
        (void)more;

        char colorPath[1024];
        char zPath[1024];
        std::snprintf(colorPath, sizeof(colorPath), "%s/city_t%06d_color.ppm",
                      cfg.outDir.c_str(), ts);
        std::snprintf(zPath, sizeof(zPath), "%s/city_t%06d_z.pgm",
                      cfg.outDir.c_str(), ts);

        write_ppm(colorPath, MainSurf->Data, xres, yres, MainSurf->BPSL);
        write_pgm16(zPath,
                    reinterpret_cast<const word*>(MainSurf->Z16),
                    xres, yres);
        ++produced;
    }

    driver->cleanup();
    driver.reset();

    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}

namespace {
// Records every GlatoScene::tick() invocation; flushed to CSV at the end.
std::vector<GlatoTraceSample> g_glatoTraceBuf;
void glatoTraceCollector(const GlatoTraceSample& s) {
    g_glatoTraceBuf.push_back(s);
}
} // namespace

int RunGlatTrace(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    Initialize_Glato();

    // Default sweep: every 10 ticks across Glat's playable range (Timer
    // < 3500). Override with --snapshot=glat-trace@t=N1,N2,...
    std::vector<int32_t> timestamps = cfg.timestamps;
    if (timestamps.empty()) {
        for (int32_t t = 0; t < 3500; t += 10) timestamps.push_back(t);
    }

    g_glatoTraceBuf.clear();
    g_glatoTraceBuf.reserve(timestamps.size());
    g_glatoTraceHook = &glatoTraceCollector;

    auto driver = createGlatoScene();
    driver->init();

    for (int32_t ts : timestamps) {
        std::srand(0);
        Timer = ts;
        std::memset((void*)Keyboard, 0, sizeof(Keyboard));
        driver->tick();
    }

    driver->cleanup();
    driver.reset();
    g_glatoTraceHook = nullptr;

    char csvPath[1024];
    std::snprintf(csvPath, sizeof(csvPath), "%s/glat_trace.csv", cfg.outDir.c_str());
    std::FILE* f = std::fopen(csvPath, "w");
    if (!f) {
        std::fprintf(stderr, "[SNAPSHOT] fopen('%s') failed: %s\n", csvPath, std::strerror(errno));
        ThreadPool::instance().close();
        return 4;
    }
    std::fprintf(f, "timer,st,rx,ry,rz,camX,camY,camZ\n");
    // %.9g preserves enough digits to round-trip a float exactly so a
    // single-bit divergence shows up as different text.
    for (const auto& s : g_glatoTraceBuf) {
        std::fprintf(f, "%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
                     s.timer, s.st, s.rx, s.ry, s.rz, s.camX, s.camY, s.camZ);
    }
    std::fclose(f);
    std::fprintf(stderr, "[SNAPSHOT] wrote %s (%zu rows)\n", csvPath, g_glatoTraceBuf.size());

    ThreadPool::instance().close();
    return 0;
}

int RunFillerTestSnapshot(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    FillerTestSnapshotInit(xres, yres);

    std::vector<int32_t> seeds = cfg.timestamps;
    if (seeds.empty()) seeds = {0};

    int produced = 0;
    for (int32_t seed : seeds) {
        FillerTestSnapshotRender(seed);

        char colorPath[1024];
        char zPath[1024];
        std::snprintf(colorPath, sizeof(colorPath), "%s/filler_t%06d_color.ppm",
                      cfg.outDir.c_str(), seed);
        std::snprintf(zPath, sizeof(zPath), "%s/filler_t%06d_z.pgm",
                      cfg.outDir.c_str(), seed);

        write_ppm(colorPath, MainSurf->Data, xres, yres, MainSurf->BPSL);
        write_pgm16(zPath,
                    reinterpret_cast<const word*>(MainSurf->Z16),
                    xres, yres);
        ++produced;
    }

    FillerTestSnapshotCleanup();
    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}

// Helper for the reflection-test mode: build a row-major 3×3 rotation
// matrix that maps a world point onto a camera looking from `eye` at
// `target`. The engine convention is camera-z = world forward (Identity
// camera looks at world +z) and +y is "down" (engine quirk discovered
// during reflection debugging — sky cube faces +y up but the rendered
// panorama has sky at top while panorama y=0 → d.y=−1, so the engine
// is rendering with +y as the down direction). Pick a world-up that
// avoids gimbal-lock for the top/bottom views.
static void buildLookAt(const Vector& eye, const Vector& target, Matrix outM) {
    Vector forward = target - eye;
    Vector_Norm(&forward);
    // Engine convention: world +y is "up" (water plane at y=0 with
    // normal (0,1,0); buildings have bsCenter y > 0). Set worldUp to
    // +y so the rendered screen-up matches world-up.
    Vector worldUp(0.0f, 1.0f, 0.0f);
    if (std::fabs(forward.y) > 0.95f) worldUp = Vector(0.0f, 0.0f, 1.0f);
    Vector right;
    Cross_Product(&worldUp, &forward, &right);
    Vector_Norm(&right);
    Vector up;
    Cross_Product(&forward, &right, &up);
    // Row-major: each row is the camera-axis direction in world coords.
    // (Identity matches +x right, +y down, +z forward.)
    outM[0][0] = right.x;   outM[0][1] = right.y;   outM[0][2] = right.z;
    outM[1][0] = up.x;      outM[1][1] = up.y;      outM[1][2] = up.z;
    outM[2][0] = forward.x; outM[2][1] = forward.y; outM[2][2] = forward.z;
}

int RunReflectionTest(const SnapshotConfig& cfg, int xres, int yres) {
    // Force the synthetic quadrant-coloured panorama. The cube-map bake
    // still runs; we just paint over its output with a known-direction
    // image so each pixel of the eventual reflection has an unambiguous
    // (eu, ev) → colour mapping.
#ifdef _WIN32
    _putenv_s("FDS_DEBUG_PANORAMA", "1");
#else
    setenv("FDS_DEBUG_PANORAMA", "1", 1);
#endif

    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    Initialize_City();
    Scene* sc = getCityScene();
    if (!sc) {
        std::fprintf(stderr, "[REFLTEST] getCityScene() returned null\n");
        return 4;
    }

    // Pick a target building. b1.lwo is the first numbered building in
    // City; the bake puts a panorama on it. Fall back to any b<digit>.lwo
    // if the loader renames things.
    Object* target = nullptr;
    for (Object* o = sc->ObjectHead; o; o = o->Next) {
        if (o->Type != Obj_TriMesh || !o->Name) continue;
        if (o->Name[0] == 'b' && '0' <= o->Name[1] && o->Name[1] <= '9'
            && std::strcmp(o->Name + 2, ".lwo") == 0) {
            target = o;
            if (std::strcmp(o->Name, "b1.lwo") == 0) break;
        }
    }
    if (!target) {
        std::fprintf(stderr, "[REFLTEST] no target building found\n");
        return 4;
    }
    TriMesh* T = (TriMesh*)target->Data;
    Vector centerWorld;
    {
        Vector bsLocal;
        MatrixXVector(T->RotMat, &T->BSphereCtr, &bsLocal);
        centerWorld.x = T->IPos.x + bsLocal.x;
        centerWorld.y = T->IPos.y + bsLocal.y;
        centerWorld.z = T->IPos.z + bsLocal.z;
    }
    const float dist = std::sqrt(T->BSphereRad) * 4.0f + 50.0f;

    std::fprintf(stderr,
        "[REFLTEST] target=%s center=(%.1f,%.1f,%.1f) bsRad²=%.1f camDist=%.1f\n",
        target->Name, centerWorld.x, centerWorld.y, centerWorld.z,
        T->BSphereRad, dist);

    // Animation state: drive one tick at t=0 to populate Vtx->LR/LG/LB
    // and the omni splines. Subsequent renders use the camera we override
    // and skip animation, so reflections won't change frame-to-frame.
    Timer = 0;
    auto driver = createCityScene();
    driver->init();

    // Use the first scene camera as our rendering View; we'll just
    // overwrite ISource/Mat between snaps. Render() reads the global
    // `View` (set by SetCurrentScene → done in driver->init() / tick()).
    if (!View) View = sc->CameraHead;

    // 6 camera positions: each `(name, offsetDir)` places the camera at
    // centerWorld + offsetDir*dist looking back toward centerWorld. The
    // building face most directly facing the camera reflects a known
    // direction.
    struct CamPose { const char* name; Vector dir; };
    const CamPose poses[] = {
        {"pos_x",   Vector( 1.0f,  0.0f,  0.0f)},
        {"neg_x",   Vector(-1.0f,  0.0f,  0.0f)},
        {"pos_z",   Vector( 0.0f,  0.0f,  1.0f)},
        {"neg_z",   Vector( 0.0f,  0.0f, -1.0f)},
        {"pos_y",   Vector( 0.0f,  1.0f,  0.0f)},
        {"neg_y",   Vector( 0.0f, -1.0f,  0.0f)},
    };

    int produced = 0;
    for (const CamPose& p : poses) {
        // Position + look-at
        View->ISource.x = centerWorld.x + p.dir.x * dist;
        View->ISource.y = centerWorld.y + p.dir.y * dist;
        View->ISource.z = centerWorld.z + p.dir.z * dist;
        buildLookAt(View->ISource, centerWorld, View->Mat);
        std::srand(0);

        // Manual frame: clear, transform, sort, render.
        std::memset(VPage,   0, PageSize);
        std::memset(ZPage16, 0, XRes * YRes * sizeof(word));
        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
        if (CAll) {
            Radix_Sort(FList, SList, CAll);
            Render(RenderPath::ForceForward);
        }

        char colorPath[1024];
        std::snprintf(colorPath, sizeof(colorPath),
                      "%s/refltest_%s_color.ppm", cfg.outDir.c_str(), p.name);
        write_ppm(colorPath, MainSurf->Data, xres, yres, MainSurf->BPSL);
        std::fprintf(stderr,
            "[REFLTEST] %s: cam=(%.1f,%.1f,%.1f) → %s\n",
            p.name, View->ISource.x, View->ISource.y, View->ISource.z,
            colorPath);
        ++produced;
    }

    driver->cleanup();
    driver.reset();
    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}

namespace {

// Synthetic panorama painted BY WORLD DIRECTION (per the panorama
// generator's own lat/lon convention). At pixel (eu, ev), compute the
// direction d the generator INTENDS to store there:
//   lon = 2π·eu, lat = π·ev − π/2
//   d   = (cos(lat)·sin(lon), sin(lat), cos(lat)·cos(lon))
// then colour the pixel by the dominant axis of d:
//   +z YELLOW   -z GREEN
//   +x RED      -x BLUE
//   +y MAGENTA  -y WHITE      (engine y-convention encoded by the engine
//                              itself; we just label both poles)
//
// This tests REFLECTION-DIRECTION CORRECTNESS, not just self-
// consistency. If the reflected-ray-to-(eu, ev) formula maps a ray
// pointing in direction X to the pixel that the generator labels as
// world-direction X, the rendered face shows X's colour. If it maps
// to a pixel labelled Y, we see Y — direct evidence of a wrong-axis
// or wrong-sign bug in the lookup, independent of any bake quirks.
void buildSyntheticPanorama(uint32_t* data, int w, int h) {
    // Paint each pixel with the colour of the WORLD DIRECTION that the
    // engine's reflective lookup formula maps that pixel to. Inverting
    // the lookup (lat = asin(d.y), lon = atan2(-d.z, -d.x), then
    //   eu = 0.5 + 0.5*(lon + π/2)/π, ev = 0.5 - 0.5*lat/(π/2)):
    //   lat =  π/2 - π*ev          → d.y =  sin(lat)
    //   lon =  2π*eu - 3π/2        → d.x = -cos(lon)*cos(lat)
    //                                d.z = -sin(lon)*cos(lat)
    // After --snapshot=cuberefl: each rendered cube face (whose visible
    // reflected ray for the centre pixel is its outward normal) shows
    // exactly the colour of that face's outward-normal direction.
    for (int y = 0; y < h; ++y) {
        const float ev  = (float(y) + 0.5f) / float(h);
        const float lat = float(PI) * 0.5f - float(PI) * ev;
        const float dy  = std::sin(lat);
        const float xz  = std::cos(lat);
        for (int x = 0; x < w; ++x) {
            const float eu  = (float(x) + 0.5f) / float(w);
            const float lon = 2.0f * float(PI) * eu - 1.5f * float(PI);
            const float dx  = -xz * std::cos(lon);
            const float dz  = -xz * std::sin(lon);

            const float adx = std::fabs(dx);
            const float ady = std::fabs(dy);
            const float adz = std::fabs(dz);

            uint32_t c;
            if (ady > adx && ady > adz) {
                c = (dy > 0.0f) ? 0x00FF00FFu  // +y MAGENTA
                                : 0x00FFFFFFu; // -y WHITE
            } else if (adx > adz) {
                c = (dx > 0.0f) ? 0x00FF0000u  // +x RED
                                : 0x000000FFu; // -x BLUE
            } else {
                c = (dz > 0.0f) ? 0x00FFFF00u  // +z YELLOW
                                : 0x0000FF00u; // -z GREEN
            }

            data[y * w + x] = c | 0xFF000000u;
        }
    }
}

// Build a Scene with one outward-facing reflective cube at origin and
// one camera. The cube's faces all share a Material whose EnvTexture
// (and per-face ReflectionTexture) is a synthetic quadrant-coloured
// panorama, so each rendered face's colour identifies the (eu, ev)
// the lookup formula resolved to for its reflected ray.
//
// Memory: leaks intentionally — this is a one-shot harness, the
// process exits at the end of RunCubeReflTest.
Scene* buildReflTestCubeScene() {
    constexpr int PANO_W = 1024;
    constexpr int PANO_H = 1024;
    constexpr int PANO_LSIZE_X = 10; // log2(1024)
    constexpr int PANO_LSIZE_Y = 10;

    Scene* Sc = (Scene*)getAlignedBlock(sizeof(Scene), 16);
    std::memset(Sc, 0, sizeof(Scene));
    Sc->NZP   = 1.0f;
    Sc->FZP   = 200.0f;
    Sc->Flags = Scn_ZBuffer | Scn_StaticLighting; // skip Lighting() — we set Vtx->L* by hand
    Sc->Ambient.R = Sc->Ambient.G = Sc->Ambient.B = 200;

    // Synthetic panorama texture
    Texture* PanoTex = new Texture;
    std::memset(PanoTex, 0, sizeof(Texture));
    uint32_t* panoData = (uint32_t*)_aligned_malloc(PANO_W * PANO_H * 4, 16);
    buildSyntheticPanorama(panoData, PANO_W, PANO_H);
    PanoTex->Data    = (byte*)panoData;
    PanoTex->BPP     = 32;
    PanoTex->SizeX   = PANO_W; PanoTex->LSizeX = PANO_LSIZE_X;
    PanoTex->SizeY   = PANO_H; PanoTex->LSizeY = PANO_LSIZE_Y;
    PanoTex->Flags   = Txtr_Nomip | Txtr_Tiled;
    Sachletz((dword*)PanoTex->Data, PANO_W, PANO_H);
    PanoTex->Mipmap[0]   = PanoTex->Data;
    PanoTex->numMipmaps  = 1;

    // Tiny black base texture so the TheOtherBarry<TEXTURETEXTURE>
    // colorize step's `env + base/2` shows pure env content.
    constexpr int BASE_SZ = 16;
    Texture* BaseTex = new Texture;
    std::memset(BaseTex, 0, sizeof(Texture));
    uint32_t* baseData = (uint32_t*)_aligned_malloc(BASE_SZ * BASE_SZ * 4, 16);
    for (int i = 0; i < BASE_SZ * BASE_SZ; ++i) baseData[i] = 0xFF000000u; // opaque black
    BaseTex->Data   = (byte*)baseData;
    BaseTex->BPP    = 32;
    BaseTex->SizeX  = BASE_SZ; BaseTex->LSizeX = 4; // log2(16)
    BaseTex->SizeY  = BASE_SZ; BaseTex->LSizeY = 4;
    BaseTex->Flags  = Txtr_Nomip | Txtr_Tiled;
    Sachletz((dword*)BaseTex->Data, BASE_SZ, BASE_SZ);
    BaseTex->Mipmap[0]   = BaseTex->Data;
    BaseTex->numMipmaps  = 1;

    // Material — base = black, EnvTexture = panorama, reflective.
    Material* M = getAlignedType<Material>(16);
    M->Txtr        = BaseTex;
    M->EnvTexture  = PanoTex;
    M->BaseCol.B   = M->BaseCol.G = M->BaseCol.R = 255;
    M->BaseCol.A   = 255;
    M->Diffuse     = 1.0f;
    M->Reflection  = 50.0f; // 0..100 scale per Mat_TABLE dump; only matters for the deferred env branch (not used here)
    M->Luminosity  = 0.0f;
    M->Flags       = Mat_TwoSided | Mat_RGBInterp;
    M->RelScene    = Sc;
    M->ID          = 0;
    M->Name        = strdup("test_cube");

    // TriMesh + Object
    TriMesh* T = (TriMesh*)getAlignedBlock(sizeof(TriMesh), 16);
    std::memset(T, 0, sizeof(TriMesh));

    Object* Obj = new Object;
    std::memset(Obj, 0, sizeof(Object));
    Obj->Name = strdup("test_cube");
    Obj->Type = Obj_TriMesh;
    Obj->Data = T;
    Obj->Rot  = &T->RotMat;
    Obj->Pos  = &T->IPos;
    Vector_Form(&Obj->Pivot, 0, 0, 0);
    Obj->Next = nullptr;
    Sc->ObjectHead = Obj;
    T->Next = nullptr;
    Sc->TriMeshHead = T;

    // Outward-facing cube: 6 faces, 4 verts/face (separate UVs), 12 tris.
    constexpr float S = 1.0f; // half-edge length
    T->VIndex = 24;
    T->Verts  = new Vertex[T->VIndex];
    std::memset(T->Verts, 0, sizeof(Vertex) * T->VIndex);
    T->FIndex = 12;
    T->Faces  = new Face[T->FIndex];
    std::memset(T->Faces, 0, sizeof(Face) * T->FIndex);

    // Per-face setup. Each face: 4 verts + 2 tris.
    // Outward normals:
    //   face 0: +z, viewed from +z, vertices CCW
    //   face 1: +x
    //   face 2: -z
    //   face 3: -x
    //   face 4: +y (engine "down")
    //   face 5: -y (engine "up")
    struct FaceQuad {
        Vector n;
        Vector p[4]; // vertex positions, CCW from outside
    };
    const FaceQuad quads[6] = {
        // +z face: looking from +z (outside), CCW → 4,5,7,6 in [-S,S]^3 box
        { Vector( 0, 0, 1), { Vector(-S,-S, S), Vector( S,-S, S), Vector( S, S, S), Vector(-S, S, S) } },
        // +x face: looking from +x, CCW
        { Vector( 1, 0, 0), { Vector( S,-S, S), Vector( S,-S,-S), Vector( S, S,-S), Vector( S, S, S) } },
        // -z face: looking from -z, CCW
        { Vector( 0, 0,-1), { Vector( S,-S,-S), Vector(-S,-S,-S), Vector(-S, S,-S), Vector( S, S,-S) } },
        // -x face: looking from -x, CCW
        { Vector(-1, 0, 0), { Vector(-S,-S,-S), Vector(-S,-S, S), Vector(-S, S, S), Vector(-S, S,-S) } },
        // +y face (engine down): looking from +y outside, CCW
        { Vector( 0, 1, 0), { Vector(-S, S, S), Vector( S, S, S), Vector( S, S,-S), Vector(-S, S,-S) } },
        // -y face (engine up): looking from -y outside, CCW
        { Vector( 0,-1, 0), { Vector(-S,-S,-S), Vector( S,-S,-S), Vector( S,-S, S), Vector(-S,-S, S) } },
    };

    for (int fi = 0; fi < 6; ++fi) {
        const FaceQuad& q = quads[fi];
        // 4 verts for this face
        for (int k = 0; k < 4; ++k) {
            Vertex* V = &T->Verts[fi * 4 + k];
            V->Pos = q.p[k];
            V->N   = q.n;             // per-vertex normal = face normal (faceted)
            V->LR  = V->LG = V->LB = 250; // fully bright; skip Lighting()
            V->LA  = 255;
            // Base UVs (don't matter much; cover the 16x16 base)
            V->U = (k == 1 || k == 2) ? 1.0f - 1.0f/16.0f : 1.0f/16.0f;
            V->V = (k == 2 || k == 3) ? 1.0f - 1.0f/16.0f : 1.0f/16.0f;
        }
        // Two triangles for this face (split the quad)
        for (int tri = 0; tri < 2; ++tri) {
            Face* F = &T->Faces[fi * 2 + tri];
            // Quad (0,1,2,3) → triangles (0,1,2) and (0,2,3)
            int idx[3] = { 0, (tri == 0 ? 1 : 2), (tri == 0 ? 2 : 3) };
            F->A = &T->Verts[fi * 4 + idx[0]];
            F->B = &T->Verts[fi * 4 + idx[1]];
            F->C = &T->Verts[fi * 4 + idx[2]];
            F->N = q.n;
            F->NormProd = -Dot_Product(&F->A->Pos, &F->N);
            F->Txtr = M;
            F->ReflectionTexture = PanoTex;
            F->Flags = Face_Reflective;
            F->uvFromVertices();
        }
    }

    // Position / orientation at world origin, identity rotation, scale 1.
    Matrix_Identity(T->RotMat);
    Vector_Form(&T->IPos, 0, 0, 0);
    Vector_Form(&T->IScale, 1, 1, 1);
    Vector_Form(&T->BSphereCtr, 0, 0, 0);
    T->BSphereRad    = 4.0f * S * S;     // |corner|² = 3·S²; pad
    T->BSphereRadius = 2.0f * S;
    T->Flags = HTrack_Visible;

    // Splines: one keyframe each at origin / identity / unit scale so
    // Animate_Objects (if it ever runs) leaves things alone.
    T->Pos.NumKeys = 1;
    T->Pos.Keys = new SplineKey[1];
    std::memset(T->Pos.Keys, 0, sizeof(SplineKey));
    Quaternion_Form(&T->Pos.Keys->Pos, 0, 0, 0, 0);
    T->Scale.NumKeys = 1;
    T->Scale.Keys = new SplineKey[1];
    std::memset(T->Scale.Keys, 0, sizeof(SplineKey));
    Quaternion_Form(&T->Scale.Keys->Pos, 1, 1, 1, 0);
    T->Rotate.NumKeys = 1;
    T->Rotate.Keys = new SplineKey[1];
    std::memset(T->Rotate.Keys, 0, sizeof(SplineKey));
    Quaternion_Form(&T->Rotate.Keys->Pos, 0, 0, 0, 1);

    // Camera. ISource and Mat overwritten per pose by the harness.
    Camera* Cam = (Camera*)getAlignedBlock(sizeof(Camera), 16);
    std::memset(Cam, 0, sizeof(Camera));
    Vector_Form(&Cam->ISource, 0, 0, -5);
    Matrix_Identity(Cam->Mat);
    Cam->IFOV = 60.0f; // CalcPersp expects DEGREES (does its own deg→rad)
    Sc->CameraHead = Cam;

    return Sc;
}

} // namespace

int RunCubeReflTest(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    Scene* sc = buildReflTestCubeScene();
    SetCurrentScene(sc);
    View = sc->CameraHead;

    // Allocate the global FList/SList that Transform_Objects writes
    // into. CityScene::init does this; without an FLD scene driver we
    // need to do it ourselves. Cube has 12 faces + headroom for omnis
    // (none in our test).
    fds::g_mainFaces.resize(64);

    // 6 camera poses around the cube
    struct CamPose { const char* name; Vector dir; };
    const CamPose poses[] = {
        {"pos_x",   Vector( 1.0f,  0.0f,  0.0f)},
        {"neg_x",   Vector(-1.0f,  0.0f,  0.0f)},
        {"pos_z",   Vector( 0.0f,  0.0f,  1.0f)},
        {"neg_z",   Vector( 0.0f,  0.0f, -1.0f)},
        {"pos_y",   Vector( 0.0f,  1.0f,  0.0f)},
        {"neg_y",   Vector( 0.0f, -1.0f,  0.0f)},
    };

    const Vector center = Vector(0, 0, 0);
    const float dist = 5.0f;

    int produced = 0;
    for (const CamPose& p : poses) {
        View->ISource.x = center.x + p.dir.x * dist;
        View->ISource.y = center.y + p.dir.y * dist;
        View->ISource.z = center.z + p.dir.z * dist;
        buildLookAt(View->ISource, center, View->Mat);

        // CalcPersp populates View->PerspX/PerspY from IFOV and the
        // current XRes/YRes, then we copy into the FOVX/FOVY globals
        // Transform_Objects reads. Animate_Objects normally does this;
        // we skip Animate_Objects so do it inline.
        CalcPersp(View);
        FOVX = View->PerspX;
        FOVY = View->PerspY;

        // Manual frame: clear, transform, sort, render.
        std::memset(VPage,   0, PageSize);
        std::memset(ZPage16, 0, XRes * YRes * sizeof(word));
        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
        if (CAll) {
            Radix_Sort(FList, SList, CAll);
            Render(RenderPath::ForceForward);
        }

        char colorPath[1024];
        std::snprintf(colorPath, sizeof(colorPath),
                      "%s/cuberefl_%s_color.ppm", cfg.outDir.c_str(), p.name);
        write_ppm(colorPath, MainSurf->Data, xres, yres, MainSurf->BPSL);
        std::fprintf(stderr,
            "[CUBEREFL] %s: cam=(%.1f,%.1f,%.1f) → %s\n",
            p.name, View->ISource.x, View->ISource.y, View->ISource.z,
            colorPath);
        ++produced;
    }

    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}

// ============================================================
// XPAR-TEST: isolated transparent-rendering harness
// ============================================================
//
// Minimal programmatic scenes designed to test transparent rendering
// behavior independent of any FLD-loaded scene's quirks. Each case is
// a self-contained set of opaque + transparent surfaces with a single
// known omni light. The harness renders each case from 6 fixed camera
// poses around the scene; comparing across poses surfaces lighting
// drift, missing triangles, Z-occlusion failures, and double-blend
// errors.
//
// Cases (selected via @case=N):
//   1 = single transparent quad in front of opaque ground + one omni
//   2 = opaque wall blocking a transparent panel behind it
//   3 = two transparent panels at different depths (layered blend)
//   4 = glass cube (6 transparent faces, Mat_TwoSided)
//
// Run with FDS_DEFERRED=0/1 to compare forward vs deferred paths.
namespace {

// Link a Material into the global MatLib chain. The deferred path's
// Scene_GetMatTable scans MatLib filtered by RelScene — orphan materials
// (not in MatLib) won't appear in the per-scene matTable and the lighting
// kernel will skip every pixel that references them.
static void linkMatToLib(Material* M) {
    if (!MatLib) { MatLib = M; M->Prev = nullptr; M->Next = nullptr; return; }
    Material* tail = MatLib;
    while (tail->Next) tail = tail->Next;
    tail->Next = M;
    M->Prev = tail;
    M->Next = nullptr;
}

// Small helper: allocate + initialise a Material with a single 16x16
// solid-colour texture and the requested material flags.
static Material* makeSolidColorMat(Scene* Sc, const char* name,
                                    byte r, byte g, byte bb, byte a,
                                    dword flags, byte matID)
{
    constexpr int SZ = 16;
    Texture* T = new Texture;
    std::memset(T, 0, sizeof(Texture));
    uint32_t* data = (uint32_t*)_aligned_malloc(SZ * SZ * 4, 16);
    const uint32_t packed = (uint32_t(a) << 24) | (uint32_t(r) << 16)
                          | (uint32_t(g) << 8) | uint32_t(bb);
    for (int i = 0; i < SZ * SZ; ++i) data[i] = packed;
    T->Data   = (byte*)data;
    T->BPP    = 32;
    T->SizeX  = SZ; T->LSizeX = 4;
    T->SizeY  = SZ; T->LSizeY = 4;
    T->Flags  = Txtr_Nomip | Txtr_Tiled;
    Sachletz((dword*)T->Data, SZ, SZ);
    T->Mipmap[0]   = T->Data;
    T->numMipmaps  = 1;

    Material* M = getAlignedType<Material>(16);
    M->Txtr        = T;
    M->BaseCol.R   = r; M->BaseCol.G = g; M->BaseCol.B = bb; M->BaseCol.A = a;
    M->Diffuse     = 1.0f;
    M->Luminosity  = 0.0f;
    M->Flags       = flags;
    M->RelScene    = Sc;
    M->ID          = matID;
    M->Name        = strdup(name);
    return M;
}

// Append a programmatically-created quad to a scene.
// `n` should be the outward (winding-derived) normal; vertices are CCW.
struct QuadDef { Vector p[4]; Vector n; };
static void appendQuad(Scene* Sc, TriMesh* T, int vBase, int fBase,
                        const QuadDef& q, Material* mat, RasterFunc filler)
{
    for (int k = 0; k < 4; ++k) {
        Vertex* V = &T->Verts[vBase + k];
        V->Pos = q.p[k];
        V->N   = q.n;
        V->TN  = q.n; // placeholder; Transform_Objects will update
        V->LR  = V->LG = V->LB = 200;
        V->LA  = 255;
        V->U = (k == 1 || k == 2) ? 1.0f - 1.0f/16.0f : 1.0f/16.0f;
        V->V = (k == 2 || k == 3) ? 1.0f - 1.0f/16.0f : 1.0f/16.0f;
    }
    for (int tri = 0; tri < 2; ++tri) {
        Face* F = &T->Faces[fBase + tri];
        int idx[3] = { 0, (tri == 0 ? 1 : 2), (tri == 0 ? 2 : 3) };
        F->A = &T->Verts[vBase + idx[0]];
        F->B = &T->Verts[vBase + idx[1]];
        F->C = &T->Verts[vBase + idx[2]];
        F->N = q.n;
        F->NormProd = -Dot_Product(&F->A->Pos, &F->N);
        F->Txtr   = mat;
        F->Filler = filler;
        F->Flags  = 0;
        F->uvFromVertices();
    }
}

// Allocate + append a stationary point omni to a scene's OmniHead chain.
static Omni* appendTestOmni(Scene* Sc, const Vector& pos,
                             float r, float g, float bb,
                             float intensity, float range)
{
    Omni* O = (Omni*)getAlignedBlock(sizeof(Omni), 16);
    std::memset(O, 0, sizeof(Omni));
    O->IPos   = pos;
    O->IRange = range;
    O->rRange = 1.0f / range;
    O->ISize  = intensity;
    O->L.R = r; O->L.G = g; O->L.B = bb; O->L.A = 1.0f;
    O->Flags  = Omni_Active | Omni_Stationary;
    auto initSingleKey = [](Spline& sp, float val) {
        sp.NumKeys = 1;
        sp.Keys = new SplineKey;
        std::memset(sp.Keys, 0, sizeof(SplineKey));
        sp.Keys[0].Pos.x = val;
        sp.Keys[0].Pos.y = val;
        sp.Keys[0].Pos.z = val;
        sp.Flags = 0;
        sp.CurKey = 0;
    };
    initSingleKey(O->Pos, 0.0f);
    O->Pos.Keys[0].Pos.x = pos.x;
    O->Pos.Keys[0].Pos.y = pos.y;
    O->Pos.Keys[0].Pos.z = pos.z;
    initSingleKey(O->Size, intensity);
    initSingleKey(O->Range, range);
    O->F.A = O->F.B = O->F.C = &O->V;
    // Render()'s post-FList sprite loop calls F->Filler unconditionally
    // for any face whose A==B (treated as particle/omni flare). We don't
    // want the flare rendered — install a no-op so the loop doesn't
    // dereference a null pointer.
    O->F.Filler = [](Face*, Vertex**, dword, dword,
                     const fds::RenderTarget&,
                     const fds::CameraContext&) {};

    if (!Sc->OmniHead) {
        Sc->OmniHead = O;
    } else {
        Omni* tail = Sc->OmniHead;
        while (tail->Next) tail = tail->Next;
        tail->Next = O;
        O->Prev = tail;
    }
    return O;
}

// Make a TriMesh + Object pair attached to the scene; you fill in
// Verts and Faces after.
static TriMesh* appendTriMesh(Scene* Sc, const char* name,
                               int numVerts, int numFaces)
{
    TriMesh* T = (TriMesh*)getAlignedBlock(sizeof(TriMesh), 16);
    std::memset(T, 0, sizeof(TriMesh));
    T->VIndex = numVerts;
    T->Verts  = new Vertex[numVerts];
    std::memset(T->Verts, 0, sizeof(Vertex) * numVerts);
    T->FIndex = numFaces;
    T->Faces  = new Face[numFaces];
    std::memset(T->Faces, 0, sizeof(Face) * numFaces);
    Matrix_Identity(T->RotMat);
    Vector_Form(&T->IPos, 0, 0, 0);
    Vector_Form(&T->IScale, 1, 1, 1);
    Vector_Form(&T->BSphereCtr, 0, 0, 0);
    T->BSphereRad    = 10000.0f;
    T->BSphereRadius = 100.0f;
    T->Flags = HTrack_Visible;
    auto initKey1 = [](Spline& sp, float a, float b, float c, float d) {
        sp.CurKey = 0;
        sp.NumKeys = 1;
        sp.Keys = new SplineKey[1];
        std::memset(sp.Keys, 0, sizeof(SplineKey));
        Quaternion_Form(&sp.Keys->Pos, a, b, c, d);
    };
    initKey1(T->Pos,    0, 0, 0, 0);
    initKey1(T->Scale,  1, 1, 1, 0);
    initKey1(T->Rotate, 0, 0, 0, 1);

    Object* Obj = new Object;
    std::memset(Obj, 0, sizeof(Object));
    Obj->Name = strdup(name);
    Obj->Type = Obj_TriMesh;
    Obj->Data = T;
    Obj->Rot  = &T->RotMat;
    Obj->Pos  = &T->IPos;
    Vector_Form(&Obj->Pivot, 0, 0, 0);

    if (!Sc->ObjectHead) Sc->ObjectHead = Obj;
    else {
        Object* tail = Sc->ObjectHead;
        while (tail->Next) tail = tail->Next;
        tail->Next = Obj; Obj->Prev = tail;
    }
    if (!Sc->TriMeshHead) Sc->TriMeshHead = T;
    else {
        TriMesh* tail = Sc->TriMeshHead;
        while (tail->Next) tail = tail->Next;
        tail->Next = T; T->Prev = tail;
    }
    return T;
}

// Build a per-case scene: returns the scene + a short name string.
static Scene* buildXparTestScene(int testCase) {
    Scene* Sc = (Scene*)getAlignedBlock(sizeof(Scene), 16);
    std::memset(Sc, 0, sizeof(Scene));
    Sc->NZP   = 1.0f;
    Sc->FZP   = 200.0f;
    Sc->Flags = Scn_ZBuffer;
    Sc->Ambient.R = Sc->Ambient.G = Sc->Ambient.B = 40;
    Sc->Ambient.A = 255;

    // Camera placeholder. Harness rewrites ISource/Mat per pose.
    Camera* Cam = (Camera*)getAlignedBlock(sizeof(Camera), 16);
    std::memset(Cam, 0, sizeof(Camera));
    Vector_Form(&Cam->ISource, 0, 0, -5);
    Matrix_Identity(Cam->Mat);
    Cam->IFOV = 60.0f;
    Sc->CameraHead = Cam;

    // One bright omni above the scene. Position chosen so it lights
    // both the ground and any front-of-quad surfaces.
    appendTestOmni(Sc, Vector(0, 5, 0), 1.0f, 1.0f, 0.6f, 200.0f, 30.0f);

    Material* matGround = makeSolidColorMat(Sc, "test_ground",
                                            120, 80, 80, 255,
                                            Mat_RGBInterp, 0);
    matGround->Diffuse = 1.0f;
    Material* matXpar = makeSolidColorMat(Sc, "test_xpar",
                                          80, 180, 220, 255,
                                          Mat_TwoSided | Mat_RGBInterp | Mat_Transparent, 1);
    matXpar->Diffuse = 1.0f;
    linkMatToLib(matGround);
    linkMatToLib(matXpar);

    // All cases include an opaque ground plane.
    {
        TriMesh* ground = appendTriMesh(Sc, "xpar_ground", 4, 2);
        QuadDef q = {
            { Vector(-10, 0, -10), Vector( 10, 0, -10),
              Vector( 10, 0,  10), Vector(-10, 0,  10) },
            Vector(0, 1, 0)
        };
        // Filler set in PREPROC normally; for snapshot we hand-pick
        // an opaque textured filler. TheOtherBarry<OVERWRITE,NORMAL>
        // works for opaque base.
        appendQuad(Sc, ground, 0, 0, q, matGround,
                   TheOtherBarry<barry::TBlendMode::OVERWRITE,
                                 barry::TTextureMode::NORMAL>);
    }

    if (testCase == 1) {
        // Single upright transparent quad at center, facing toward camera
        // approaches. Two-sided so both sides render.
        TriMesh* xpar = appendTriMesh(Sc, "xpar_quad", 4, 2);
        QuadDef q = {
            { Vector(-2, 0.5f, 0), Vector( 2, 0.5f, 0),
              Vector( 2, 4.5f, 0), Vector(-2, 4.5f, 0) },
            Vector(0, 0, 1)
        };
        appendQuad(Sc, xpar, 0, 0, q, matXpar,
                   TheOtherBarry<barry::TBlendMode::TRANSPARENT,
                                 barry::TTextureMode::NORMAL>);
    }

    if (testCase == 2) {
        // OPAQUE wall in front (z=+2), transparent panel behind (z=-2).
        // Camera looking from +z sees the wall fully covering the panel;
        // transparent panel should NOT show through. Camera from sides
        // sees both with wall in front.
        Material* matWall = makeSolidColorMat(Sc, "test_wall",
                                              180, 180, 180, 255,
                                              Mat_RGBInterp, 2);
        matWall->Diffuse = 1.0f;
        linkMatToLib(matWall);

        // Opaque wall (centered, smaller than the transparent so the
        // panel pokes out the sides — gives a clear "through wall" test).
        TriMesh* wall = appendTriMesh(Sc, "wall", 4, 2);
        QuadDef qw = {
            { Vector(-2, 0.5f,  2), Vector( 2, 0.5f,  2),
              Vector( 2, 4.5f,  2), Vector(-2, 4.5f,  2) },
            Vector(0, 0, 1)
        };
        appendQuad(Sc, wall, 0, 0, qw, matWall,
                   TheOtherBarry<barry::TBlendMode::OVERWRITE,
                                 barry::TTextureMode::NORMAL>);

        // Transparent panel BEHIND the wall (larger so it sticks out).
        TriMesh* xpar = appendTriMesh(Sc, "xpar_behind", 4, 2);
        QuadDef qx = {
            { Vector(-4, 0.5f, -2), Vector( 4, 0.5f, -2),
              Vector( 4, 5.5f, -2), Vector(-4, 5.5f, -2) },
            Vector(0, 0, 1)
        };
        appendQuad(Sc, xpar, 0, 0, qx, matXpar,
                   TheOtherBarry<barry::TBlendMode::TRANSPARENT,
                                 barry::TTextureMode::NORMAL>);
    }

    if (testCase == 3) {
        // Two parallel transparent panels at different depths.
        // Closer one (z=0), further one (z=-3). Both same material.
        // Expected: from +z, both visible with closer blended over the
        // further-blended-on-ground stack.
        TriMesh* close = appendTriMesh(Sc, "xpar_close", 4, 2);
        QuadDef qc = {
            { Vector(-2, 0.5f, 0), Vector( 2, 0.5f, 0),
              Vector( 2, 4.5f, 0), Vector(-2, 4.5f, 0) },
            Vector(0, 0, 1)
        };
        appendQuad(Sc, close, 0, 0, qc, matXpar,
                   TheOtherBarry<barry::TBlendMode::TRANSPARENT,
                                 barry::TTextureMode::NORMAL>);

        // A second transparent material so we can tell them apart.
        Material* matXpar2 = makeSolidColorMat(Sc, "test_xpar2",
                                               220, 100, 80, 255,
                                               Mat_TwoSided | Mat_RGBInterp | Mat_Transparent, 3);
        matXpar2->Diffuse = 1.0f;
        linkMatToLib(matXpar2);

        TriMesh* far = appendTriMesh(Sc, "xpar_far", 4, 2);
        QuadDef qf = {
            { Vector(-3, 0.5f, -3), Vector( 3, 0.5f, -3),
              Vector( 3, 5.0f, -3), Vector(-3, 5.0f, -3) },
            Vector(0, 0, 1)
        };
        appendQuad(Sc, far, 0, 0, qf, matXpar2,
                   TheOtherBarry<barry::TBlendMode::TRANSPARENT,
                                 barry::TTextureMode::NORMAL>);
    }

    if (testCase == 4) {
        // Glass cube — 6 Mat_TwoSided transparent faces. Tests front/back
        // classification and missing-triangle behavior. Cube at origin
        // sized so the camera (dist=10) sees it comfortably.
        TriMesh* cube = appendTriMesh(Sc, "glass_cube", 24, 12);
        constexpr float S = 1.5f;
        const QuadDef quads[6] = {
            { { Vector(-S, 0.5f+S*0, S), Vector( S, 0.5f+S*0, S),
                Vector( S, 0.5f+S*2, S), Vector(-S, 0.5f+S*2, S) },
              Vector(0, 0, 1) },   // +z
            { { Vector( S, 0.5f+S*0, S), Vector( S, 0.5f+S*0,-S),
                Vector( S, 0.5f+S*2,-S), Vector( S, 0.5f+S*2, S) },
              Vector(1, 0, 0) },   // +x
            { { Vector( S, 0.5f+S*0,-S), Vector(-S, 0.5f+S*0,-S),
                Vector(-S, 0.5f+S*2,-S), Vector( S, 0.5f+S*2,-S) },
              Vector(0, 0,-1) },   // -z
            { { Vector(-S, 0.5f+S*0,-S), Vector(-S, 0.5f+S*0, S),
                Vector(-S, 0.5f+S*2, S), Vector(-S, 0.5f+S*2,-S) },
              Vector(-1, 0, 0) },  // -x
            { { Vector(-S, 0.5f+S*2, S), Vector( S, 0.5f+S*2, S),
                Vector( S, 0.5f+S*2,-S), Vector(-S, 0.5f+S*2,-S) },
              Vector(0, 1, 0) },   // +y (top)
            { { Vector(-S, 0.5f+S*0,-S), Vector( S, 0.5f+S*0,-S),
                Vector( S, 0.5f+S*0, S), Vector(-S, 0.5f+S*0, S) },
              Vector(0,-1, 0) },   // -y (bottom)
        };
        for (int fi = 0; fi < 6; ++fi) {
            appendQuad(Sc, cube, fi * 4, fi * 2, quads[fi], matXpar,
                       TheOtherBarry<barry::TBlendMode::TRANSPARENT,
                                     barry::TTextureMode::NORMAL>);
        }
    }

    return Sc;
}

// Build a programmatic specular-test scene. The goal is to compare
// forward (per-vertex) vs deferred (per-pixel) specular highlight
// rendering on simple, repeatable geometry. The scene contains:
//
//   * an opaque ground plane (matte; no specular contribution),
//   * a high-poly UV sphere centred at (0, 2, 0) with a shiny material
//     (Specular > 0, Glossiness sharp) — canonical surface for
//     observing a moving highlight,
//   * a tessellated upright quad ("wall") with the same shiny material
//     so we can also see the highlight on a flat surface (where
//     interpolation artefacts are easiest to spot),
//   * two stationary omnis: a bright warm key light positioned off to
//     the side / above so the highlight sits OFF-CENTRE on each
//     object, plus a dim cool fill from the opposite side.
//
// `case` selects highlight sharpness (Glossiness):
//   1 = broad lobe   (Glossiness =  4)
//   2 = medium lobe  (Glossiness = 32)
//   3 = sharp lobe   (Glossiness = 128)
static Scene* buildSpecTestScene(int glossCase) {
    Scene* Sc = (Scene*)getAlignedBlock(sizeof(Scene), 16);
    std::memset(Sc, 0, sizeof(Scene));
    Sc->NZP   = 1.0f;
    Sc->FZP   = 200.0f;
    Sc->Flags = Scn_ZBuffer;
    // Low ambient so the specular lobe is unambiguous against the
    // background diffuse term.
    Sc->Ambient.R = Sc->Ambient.G = Sc->Ambient.B = 30;
    Sc->Ambient.A = 255;

    // Camera placeholder. Harness rewrites ISource/Mat per pose.
    Camera* Cam = (Camera*)getAlignedBlock(sizeof(Camera), 16);
    std::memset(Cam, 0, sizeof(Camera));
    Vector_Form(&Cam->ISource, 0, 0, -5);
    Matrix_Identity(Cam->Mat);
    Cam->IFOV = 60.0f;
    Sc->CameraHead = Cam;

    // Key light: above + to the +x side. Placement chosen so the
    // mirror reflection of this light off the (0, 2, 0) sphere is
    // visible from a +z camera but NOT centred on the sphere — gives
    // a clearly off-centre highlight that should slide predictably as
    // the camera orbits.
    appendTestOmni(Sc, Vector( 6.0f, 7.0f, -2.0f),
                   1.0f, 0.95f, 0.85f,    // warm white
                   500.0f, 40.0f);
    // Fill: opposite side, dimmer, cool. Gives a second highlight to
    // verify multi-omni accumulation, also lifts the unlit hemisphere
    // so the sphere reads as 3D even outside the key lobe.
    appendTestOmni(Sc, Vector(-6.0f, 5.0f,  3.0f),
                   0.6f, 0.7f, 1.0f,
                   200.0f, 35.0f);

    // Materials.
    Material* matGround = makeSolidColorMat(Sc, "spec_ground",
                                            110, 90, 70, 255,
                                            Mat_RGBInterp, 0);
    matGround->Diffuse    = 1.0f;
    matGround->Specular   = 0.0f;  // matte
    matGround->Glossiness = 0;

    // Pick a Phong exponent per case. Glossiness == 0 ⇒ deferred falls
    // back to 32, but we want the case-1 broad lobe to actually look
    // broader, so we authoritative-set the value (always nonzero here).
    unsigned short gloss = 32;
    if (glossCase == 1) gloss = 4;
    if (glossCase == 2) gloss = 32;
    if (glossCase == 3) gloss = 128;

    // Sphere material: mid-tone neutral grey base so the highlight
    // (added on top of the texture-modulated diffuse) is easy to see.
    Material* matSphere = makeSolidColorMat(Sc, "spec_shiny_sphere",
                                            120, 120, 120, 255,
                                            Mat_RGBInterp, 1);
    matSphere->Diffuse    = 1.0f;
    matSphere->Specular   = 1.0f;
    matSphere->Glossiness = gloss;

    // Wall material: same params, different base colour so we can tell
    // the two surfaces apart in the dump.
    Material* matWall = makeSolidColorMat(Sc, "spec_shiny_wall",
                                          80, 110, 160, 255,
                                          Mat_RGBInterp, 2);
    matWall->Diffuse    = 1.0f;
    matWall->Specular   = 1.0f;
    matWall->Glossiness = gloss;

    linkMatToLib(matGround);
    linkMatToLib(matSphere);
    linkMatToLib(matWall);

    // Ground plane.
    {
        TriMesh* ground = appendTriMesh(Sc, "spec_ground", 4, 2);
        QuadDef q = {
            { Vector(-15, 0, -15), Vector( 15, 0, -15),
              Vector( 15, 0,  15), Vector(-15, 0,  15) },
            Vector(0, 1, 0)
        };
        appendQuad(Sc, ground, 0, 0, q, matGround,
                   TheOtherBarry<barry::TBlendMode::OVERWRITE,
                                 barry::TTextureMode::NORMAL>);
    }

    // UV sphere centred at (0, 2, 0), radius 1.5. Tessellation chosen
    // to be dense enough that per-vertex Blinn-Phong (forward, were it
    // implemented) would show a recognisable highlight; per-pixel
    // (deferred) will be smoother either way. nLat × nLon vertices on
    // the body + two pole verts; (nLat-1) × nLon × 2 triangles between
    // adjacent latitude rings (cap rings use degenerate / strip tris).
    {
        const int nLat = 24;  // exclusive of poles
        const int nLon = 32;
        const int numVerts = nLat * nLon + 2;          // +2 poles
        const int numFaces = 2 * nLon * nLat;          // body + 2 cap rings
        const float R = 1.5f;
        const Vector C = Vector(0, 2, 0);

        TriMesh* sph = appendTriMesh(Sc, "spec_sphere", numVerts, numFaces);

        auto setVert = [&](int idx, const Vector& n) {
            Vertex* V = &sph->Verts[idx];
            V->Pos.x = C.x + R * n.x;
            V->Pos.y = C.y + R * n.y;
            V->Pos.z = C.z + R * n.z;
            V->N  = n;       // outward unit normal
            V->TN = n;       // placeholder; Transform_Objects rewrites
            // Solid ambient lighting baseline; Lighting() rewrites
            // each frame (forward path) or it's irrelevant (deferred).
            V->LR = V->LG = V->LB = 30;
            V->LA = 255;
            // Spherical UV — irrelevant for solid-colour texture but
            // populated to avoid garbage.
            const float pi = 3.14159265358979f;
            float lon = std::atan2(n.z, n.x);
            float lat = std::asin(n.y);
            V->U = 0.5f + lon / (2.0f * pi);
            V->V = 0.5f - lat / pi;
        };

        // Body verts: row-major (lat, lon).
        for (int li = 0; li < nLat; ++li) {
            // latitude from just-below-north-pole down to just-above-south.
            const float pi = 3.14159265358979f;
            float t = float(li + 1) / float(nLat + 1);  // (0, 1)
            float theta = t * pi;                       // (0, pi)
            float sinT = std::sin(theta);
            float cosT = std::cos(theta);  // y from +1 down to -1
            for (int lo = 0; lo < nLon; ++lo) {
                float phi = 2.0f * pi * float(lo) / float(nLon);
                Vector n(sinT * std::cos(phi), cosT, sinT * std::sin(phi));
                setVert(li * nLon + lo, n);
            }
        }
        // Poles.
        const int northIdx = nLat * nLon + 0;
        const int southIdx = nLat * nLon + 1;
        setVert(northIdx, Vector(0,  1, 0));
        setVert(southIdx, Vector(0, -1, 0));

        // Faces. Body: two triangles per (lat, lon) quad spanning rings
        // li and li+1. Caps: fan from each pole to the adjacent ring.
        int fi = 0;
        auto fillTri = [&](int v0, int v1, int v2) {
            Face* F = &sph->Faces[fi++];
            F->A = &sph->Verts[v0];
            F->B = &sph->Verts[v1];
            F->C = &sph->Verts[v2];
            // Face normal from cross product of edges (outward).
            Vector e1, e2, fn;
            Vector_Sub(&F->B->Pos, &F->A->Pos, &e1);
            Vector_Sub(&F->C->Pos, &F->A->Pos, &e2);
            Cross_Product(&e1, &e2, &fn);
            Vector_Norm(&fn);
            F->N = fn;
            F->NormProd = -Dot_Product(&F->A->Pos, &F->N);
            F->Txtr   = matSphere;
            F->Filler = TheOtherBarry<barry::TBlendMode::OVERWRITE,
                                      barry::TTextureMode::NORMAL>;
            F->Flags  = 0;
            F->uvFromVertices();
        };
        for (int li = 0; li < nLat - 1; ++li) {
            for (int lo = 0; lo < nLon; ++lo) {
                int lo2 = (lo + 1) % nLon;
                int a = (li    ) * nLon + lo;
                int b = (li    ) * nLon + lo2;
                int c = (li + 1) * nLon + lo2;
                int d = (li + 1) * nLon + lo;
                // CCW outward winding.
                fillTri(a, d, c);
                fillTri(a, c, b);
            }
        }
        // North cap fan (pole connects to ring 0).
        for (int lo = 0; lo < nLon; ++lo) {
            int lo2 = (lo + 1) % nLon;
            fillTri(northIdx, lo, lo2);
        }
        // South cap fan.
        for (int lo = 0; lo < nLon; ++lo) {
            int lo2 = (lo + 1) % nLon;
            fillTri(southIdx,
                    (nLat - 1) * nLon + lo2,
                    (nLat - 1) * nLon + lo);
        }
        sph->FIndex = fi;  // exact count
    }

    // Tessellated upright wall behind the sphere — a 6x4 grid of quads.
    // Per-vertex interpolation will show banding/diamond artefacts on
    // forward IF it ever does specular at the vertex level; per-pixel
    // deferred should show a clean smooth lobe.
    {
        const int nx = 6, ny = 4;
        const int numVerts = (nx + 1) * (ny + 1);
        const int numFaces = nx * ny * 2;
        const float w = 8.0f, h = 5.0f;
        const float z = -5.0f;
        const float y0 = 0.5f;

        TriMesh* wall = appendTriMesh(Sc, "spec_wall", numVerts, numFaces);
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                Vertex* V = &wall->Verts[j * (nx + 1) + i];
                float u = float(i) / float(nx);
                float v = float(j) / float(ny);
                V->Pos = Vector(-w * 0.5f + u * w, y0 + v * h, z);
                V->N   = Vector(0, 0, 1);
                V->TN  = V->N;
                V->LR  = V->LG = V->LB = 30;
                V->LA  = 255;
                V->U = u; V->V = v;
            }
        }
        int fi = 0;
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                int v0 = (j    ) * (nx + 1) + i;
                int v1 = (j    ) * (nx + 1) + i + 1;
                int v2 = (j + 1) * (nx + 1) + i + 1;
                int v3 = (j + 1) * (nx + 1) + i;
                int idx[2][3] = {{v0, v1, v2}, {v0, v2, v3}};
                for (int t = 0; t < 2; ++t) {
                    Face* F = &wall->Faces[fi++];
                    F->A = &wall->Verts[idx[t][0]];
                    F->B = &wall->Verts[idx[t][1]];
                    F->C = &wall->Verts[idx[t][2]];
                    F->N = Vector(0, 0, 1);
                    F->NormProd = -Dot_Product(&F->A->Pos, &F->N);
                    F->Txtr   = matWall;
                    F->Filler = TheOtherBarry<barry::TBlendMode::OVERWRITE,
                                              barry::TTextureMode::NORMAL>;
                    F->Flags  = 0;
                    F->uvFromVertices();
                }
            }
        }
    }

    return Sc;
}

} // namespace

int RunXparTest(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    // Parse @case= via the existing timestamps slot: --snapshot=xpartest@t=1
    // (re-using the t= field; default = 1)
    int testCase = 1;
    if (!cfg.timestamps.empty()) testCase = cfg.timestamps[0];

    std::fprintf(stderr, "[XPARTEST] case=%d\n", testCase);
    Scene* sc = buildXparTestScene(testCase);
    SetCurrentScene(sc);
    View = sc->CameraHead;
    // Deferred path looks up Material* by matID via the per-scene matTable
    // built from MatLib filtered by RelScene. Our test materials are
    // already linked into MatLib (linkMatToLib in buildXparTestScene);
    // this rebuild assigns Material::ID and registers them with the scene.
    Scene_RebuildMatTable(sc);

    fds::g_mainFaces.resize(256);

    struct CamPose { const char* name; Vector dir; };
    const CamPose poses[] = {
        {"pos_z",   Vector( 0.0f,  0.3f,  1.0f)},
        {"pos_xz",  Vector( 0.7f,  0.3f,  0.7f)},
        {"pos_x",   Vector( 1.0f,  0.3f,  0.0f)},
        {"neg_xz",  Vector(-0.7f,  0.3f,  0.7f)},
        {"neg_x",   Vector(-1.0f,  0.3f,  0.0f)},
        {"high",    Vector( 0.0f,  1.0f,  0.5f)},
    };

    const Vector center = Vector(0, 2, 0);
    const float dist = 10.0f;

    int produced = 0;
    for (const CamPose& p : poses) {
        Vector dir = p.dir;
        Vector_Norm(&dir);
        View->ISource.x = center.x + dir.x * dist;
        View->ISource.y = center.y + dir.y * dist;
        View->ISource.z = center.z + dir.z * dist;
        buildLookAt(View->ISource, center, View->Mat);
        CalcPersp(View);
        FOVX = View->PerspX;
        FOVY = View->PerspY;

        std::memset(VPage,   0, PageSize);
        std::memset(ZPage16, 0, XRes * YRes * sizeof(word));

        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
        Lighting(sc);
        if (CAll) {
            Radix_Sort(FList, SList, CAll);

            // Post-sort cube-triangle limit (XPARTEST_TRI_COUNT). Walk the
            // sorted FList, keep all non-cube faces (ground, markers, etc.)
            // and the first N cube faces (identified by "tri_" material
            // name prefix). Compacts in place so Render() iterates exactly
            // those.
            int triLimit = 12;
            if (const char* env = std::getenv("XPARTEST_TRI_COUNT")) {
                triLimit = std::atoi(env);
                if (triLimit < 0) triLimit = 0;
                if (triLimit > 12) triLimit = 12;
            }
            int cubeSeen = 0;
            int kept = 0;
            for (int i = 0; i < CAll; ++i) {
                Face* F = FList[i].face;
                const bool isCube = F && F->Txtr && F->Txtr->Name &&
                                    std::strncmp(F->Txtr->Name, "tri_", 4) == 0;
                if (isCube) {
                    if (cubeSeen >= triLimit) continue;
                    ++cubeSeen;
                }
                FList[kept++] = FList[i];
            }
            std::fprintf(stderr, "[XPARTEST] triLimit=%d kept=%d/%d\n",
                         triLimit, kept, CAll);
            CAll = kept;

            Render();
        }

        char colorPath[1024];
        std::snprintf(colorPath, sizeof(colorPath),
                      "%s/xpartest_c%d_%s.ppm", cfg.outDir.c_str(),
                      testCase, p.name);
        write_ppm(colorPath, MainSurf->Data, xres, yres, MainSurf->BPSL);
        std::fprintf(stderr, "[XPARTEST] case=%d %s cam=(%.1f,%.1f,%.1f) -> %s\n",
            testCase, p.name,
            View->ISource.x, View->ISource.y, View->ISource.z,
            colorPath);
        ++produced;
    }

    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}

// Specular-highlight isolation harness. Builds a self-contained scene
// (ground + tessellated sphere + tessellated wall, all opaque) lit by a
// bright off-axis omni so the highlight sits OFF-CENTRE and slides
// predictably as the camera orbits. For each camera pose we render
// TWICE — once with the deferred path forced off (forward filler) and
// once with the deferred path enabled — and dump both PPMs side by
// side, so any divergence in highlight position / size / intensity is
// directly observable as a pair of files.
//
// Usage:
//   DEMO --snapshot=spectest          (default: gloss case 2, all 3 cases)
//   DEMO --snapshot=spectest@t=1      (only gloss case 1: broad lobe)
//   DEMO --snapshot=spectest@t=2      (only gloss case 2: medium lobe)
//   DEMO --snapshot=spectest@t=3      (only gloss case 3: sharp lobe)
//
// FDS_DEFERRED is set per-pose internally; the env var the user passes
// is irrelevant (we run both paths regardless).
//
// Outputs: <outDir>/spec_g<gloss>_<pose>_<mode>.ppm
//   gloss: 4 / 32 / 128
//   pose:  pos_z, pos_xz, pos_x, neg_xz, neg_x, high, low_z
//   mode:  fwd / def
int RunSpecTest(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    // Tone factors. Forward Lighting() ignores Specular_Factor (vertex
    // shader is Lambertian-only), so this only matters for the
    // deferred path — we enable it so deferred actually computes the
    // Blinn-Phong term.
    Ambient_Factor   = 0.25f;
    Diffusive_Factor = 1.0f;
    Specular_Factor  = 1.0f;
    Range_Factor     = 1.0f;

    // Cases to render: t=N selects a single case, no t=  means all.
    std::vector<int> cases;
    if (cfg.timestamps.empty()) {
        cases = {1, 2, 3};
    } else {
        for (int32_t t : cfg.timestamps) {
            if (t >= 1 && t <= 3) cases.push_back(int(t));
        }
        if (cases.empty()) cases = {2};
    }

    fds::g_mainFaces.resize(8192);

    struct CamPose { const char* name; Vector dir; };
    // Camera orbits around the sphere centre at (0, 2, 0). Six off-axis
    // directions plus two pure-axis poses. The key omni at (6, 7, -2)
    // means the highlight on the sphere should appear roughly along
    // the +x / +y / -z hemisphere; from a +z camera it should sit
    // slightly above and to the +x side of the sphere centre.
    const CamPose poses[] = {
        {"pos_z",  Vector( 0.0f,  0.2f,  1.0f)},  // straight on
        {"pos_xz", Vector( 0.7f,  0.2f,  0.7f)},  // 45° to +x
        {"pos_x",  Vector( 1.0f,  0.2f,  0.0f)},  // looking down -x
        {"neg_xz", Vector(-0.7f,  0.2f,  0.7f)},  // 45° to -x
        {"neg_x",  Vector(-1.0f,  0.2f,  0.0f)},  // looking down +x
        {"neg_z",  Vector( 0.0f,  0.2f, -1.0f)},  // looking down +z
        {"high",   Vector( 0.0f,  0.95f, 0.3f)},  // looking down
        {"low_z",  Vector( 0.0f, -0.2f,  1.0f)},  // looking up slightly
    };

    const Vector center = Vector(0, 2, 0);
    const float dist = 8.0f;

    int produced = 0;
    for (int gc : cases) {
        unsigned short glossVal =
            (gc == 1) ? 4 : (gc == 3) ? 128 : 32;
        std::fprintf(stderr, "[SPECTEST] gloss case=%d (Glossiness=%u)\n",
                     gc, unsigned(glossVal));
        Scene* sc = buildSpecTestScene(gc);
        SetCurrentScene(sc);
        View = sc->CameraHead;
        Scene_RebuildMatTable(sc);

        for (const CamPose& p : poses) {
            Vector dir = p.dir;
            Vector_Norm(&dir);
            View->ISource.x = center.x + dir.x * dist;
            View->ISource.y = center.y + dir.y * dist;
            View->ISource.z = center.z + dir.z * dist;
            buildLookAt(View->ISource, center, View->Mat);
            CalcPersp(View);
            FOVX = View->PerspX;
            FOVY = View->PerspY;

            // Render both modes per pose. deferredEnabled() reads
            // FDS_DEFERRED once and caches, so a single-process toggle
            // can't go via the env var; instead we pass an explicit
            // RenderPath override on each call.
            for (int mode = 0; mode < 2; ++mode) {
                const bool wantDef = (mode == 1);

                std::memset(VPage,   0, PageSize);
                std::memset(ZPage16, 0, XRes * YRes * sizeof(word));

                Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
                Lighting(sc);
                if (CAll) {
                    Radix_Sort(FList, SList, CAll);
                    Render(wantDef ? RenderPath::ForceDeferred
                                   : RenderPath::ForceForward);
                }

                char colorPath[1024];
                std::snprintf(colorPath, sizeof(colorPath),
                              "%s/spec_g%u_%s_%s.ppm",
                              cfg.outDir.c_str(), unsigned(glossVal),
                              p.name, wantDef ? "def" : "fwd");
                write_ppm(colorPath, MainSurf->Data, xres, yres,
                          MainSurf->BPSL);
                std::fprintf(stderr,
                    "[SPECTEST] gloss=%u %-7s %s cam=(%.2f,%.2f,%.2f) -> %s\n",
                    unsigned(glossVal), p.name,
                    wantDef ? "def" : "fwd",
                    View->ISource.x, View->ISource.y, View->ISource.z,
                    colorPath);
                ++produced;
            }
        }
    }

    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}

// Volumetric-cone test scene: one downward-pointing spot + a ground
// plane + a wall behind. Coordinate scale matches city (units in ~cm)
// so cone_strength default tunings transfer.
//
// Layout:
//   Spot at (0, 400, 0), dir (0,-1,0), range 800, hot 12°, outer 35°,
//   warm orange. Cone shaft extends from y=400 down to ~y=-400 (the
//   range sphere). Ground at y=0 catches the cone's "footprint".
//   A wall at z=-600 gives a reflective backdrop for cone bleed.
static Scene* buildConeTestScene() {
    Scene* Sc = (Scene*)getAlignedBlock(sizeof(Scene), 16);
    std::memset(Sc, 0, sizeof(Scene));
    Sc->NZP   = 5.0f;
    Sc->FZP   = 4000.0f;
    Sc->Flags = Scn_ZBuffer;
    Sc->Ambient.R = Sc->Ambient.G = Sc->Ambient.B = 20;
    Sc->Ambient.A = 255;

    Camera* Cam = (Camera*)getAlignedBlock(sizeof(Camera), 16);
    std::memset(Cam, 0, sizeof(Camera));
    Vector_Form(&Cam->ISource, 0, 200, -800);
    Matrix_Identity(Cam->Mat);
    Cam->IFOV = 60.0f;
    Sc->CameraHead = Cam;

    // The cone — uses production MakeSpotLight so the splines + flags
    // match what the demo actually authors. Cone direction normalized
    // inside the volumetric pass.
    fds::MakeSpotLight(Sc,
        /*R*/255, /*G*/200, /*B*/100,
        /*intensity*/3.0f, /*range*/800.0f,
        /*pos*/Vector(0, 400, 0),
        /*dir*/Vector(0, -1, 0),
        /*hot*/12.0f, /*outer*/35.0f,
        /*shadowMapRes*/0,
        /*castsShadow*/false);

    Material* matGround = makeSolidColorMat(Sc, "cone_ground",
                                            80, 80, 90, 255,
                                            Mat_RGBInterp, 0);
    matGround->Diffuse  = 1.0f;
    matGround->Specular = 0.0f;
    Material* matWall = makeSolidColorMat(Sc, "cone_wall",
                                          50, 50, 60, 255,
                                          Mat_RGBInterp, 1);
    matWall->Diffuse  = 1.0f;
    matWall->Specular = 0.0f;
    linkMatToLib(matGround);
    linkMatToLib(matWall);

    {
        TriMesh* ground = appendTriMesh(Sc, "cone_ground", 4, 2);
        QuadDef q = {
            { Vector(-2000, 0, -2000), Vector( 2000, 0, -2000),
              Vector( 2000, 0,  2000), Vector(-2000, 0,  2000) },
            Vector(0, 1, 0)
        };
        appendQuad(Sc, ground, 0, 0, q, matGround,
                   TheOtherBarry<barry::TBlendMode::OVERWRITE,
                                 barry::TTextureMode::NORMAL>);
    }
    // (back wall geometry intentionally omitted while diagnosing the
    // banding artifact — see the conetest comments above)
    return Sc;
}

// ─── Halo test scene ──────────────────────────────────────────────────
// Single omni at world (0, 200, 0), range 600, on top of a dark ground
// plane. The poses below cover the cases that matter for halo math:
// camera inside the range sphere (where sphereDisc cull never fires
// and every pixel does the per-sample/analytic math), camera just
// outside (sphere fills most of view), and far outside (sphere as a
// small projected ball). Each pose is rendered with whichever halo
// path the runtime flags select (default analytic; --no-vol_halo_analytic
// for the ray-march fallback). Run twice for A/B.
static Scene* buildHaloTestScene() {
    Scene* Sc = (Scene*)getAlignedBlock(sizeof(Scene), 16);
    std::memset(Sc, 0, sizeof(Scene));
    Sc->NZP   = 5.0f;
    Sc->FZP   = 8000.0f;
    Sc->Flags = Scn_ZBuffer;
    Sc->Ambient.R = Sc->Ambient.G = Sc->Ambient.B = 10;
    Sc->Ambient.A = 255;

    Camera* Cam = (Camera*)getAlignedBlock(sizeof(Camera), 16);
    std::memset(Cam, 0, sizeof(Camera));
    Vector_Form(&Cam->ISource, 0, 200, -1500);
    Matrix_Identity(Cam->Mat);
    Cam->IFOV = 60.0f;
    Sc->CameraHead = Cam;

    // Single cool-blue omni — wide enough to give meaningful halo,
    // narrow enough that the camera can clearly be outside the sphere
    // for the far poses. Range 600 → sphere ⌀1200 centered at (0,200,0).
    Omni* O = (Omni*)getAlignedBlock(sizeof(Omni), 16);
    std::memset(O, 0, sizeof(Omni));
    O->L.R = 180.0f; O->L.G = 220.0f; O->L.B = 255.0f; O->L.A = 1.0f;
    O->ISize  = 8.0f;       // boosted from 2.5 so halo is visible at
                            // default halo_strength=0.5 (small-range
                            // omni needs higher ISize to compensate
                            // for the smaller integration interval).
    O->IRange = 600.0f;
    O->rRange = 1.0f / O->IRange;
    O->IPos   = Vector(0, 200, 0);
    O->Type   = Light_Omni;
    O->Flags  = Omni_Active;
    // Flare-pass plumbing — Transform_Objects's flare pass dereferences
    // F.A/B/C and calls F.Filler unconditionally, even for non-flare
    // lights. Without this it hangs/crashes silently. (Cribbed from
    // MakeSpotLight in DEMO/SpotlightCones.cpp.)
    O->F.A = &O->V;
    O->F.B = &O->V;
    O->F.C = &O->V;
    O->F.Filler = [](Face*, Vertex**, dword, dword,
                     const fds::RenderTarget&,
                     const fds::CameraContext&) {};
    auto initKey = [](Spline& sp, float a, float b, float c) {
        sp.NumKeys = 1; sp.Keys = new SplineKey;
        std::memset(sp.Keys, 0, sizeof(SplineKey));
        sp.Keys[0].Pos.x = a; sp.Keys[0].Pos.y = b; sp.Keys[0].Pos.z = c;
        sp.Flags = 0; sp.CurKey = 0;
    };
    initKey(O->Pos,   O->IPos.x, O->IPos.y, O->IPos.z);
    initKey(O->Size,  O->ISize,  O->ISize,  O->ISize);
    initKey(O->Range, O->IRange, O->IRange, O->IRange);
    O->Next = Sc->OmniHead;
    Sc->OmniHead = O;

    // Ground + back wall so the halo has surfaces to composite against.
    Material* matGround = makeSolidColorMat(Sc, "halo_ground",
                                            60, 60, 70, 255,
                                            Mat_RGBInterp, 0);
    matGround->Diffuse  = 1.0f;
    matGround->Specular = 0.0f;
    Material* matWall = makeSolidColorMat(Sc, "halo_wall",
                                          40, 40, 50, 255,
                                          Mat_RGBInterp, 1);
    matWall->Diffuse  = 1.0f;
    matWall->Specular = 0.0f;
    linkMatToLib(matGround);
    linkMatToLib(matWall);

    {
        // Ground at ±1500 — small enough that the single 2-triangle quad
        // doesn't span huge near-far depth in view space (which produces
        // non-monotone z / dropped coverage from the rasterizer's
        // perspective interpolation, manifesting as horizontal black
        // bands in the halo composite — see agent diagnosis 2026-05-19).
        TriMesh* ground = appendTriMesh(Sc, "halo_ground", 4, 2);
        QuadDef q = {
            { Vector(-1500, 0, -1500), Vector( 1500, 0, -1500),
              Vector( 1500, 0,  1500), Vector(-1500, 0,  1500) },
            Vector(0, 1, 0)
        };
        appendQuad(Sc, ground, 0, 0, q, matGround,
                   TheOtherBarry<barry::TBlendMode::OVERWRITE,
                                 barry::TTextureMode::NORMAL>);
    }
    (void)matWall;  // unused without back wall (caused hang separately)
    return Sc;
}

int RunHaloTest(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    if (fds::FeatureFlags::omni_halo_strength() <= 0.0f) {
        std::fprintf(stderr,
            "[HALOTEST] WARNING: --omni_halo_strength is 0; halo pass\n"
            "[HALOTEST] will be a no-op. Re-run with --deferred "
            "--omni_halo_strength=0.5.\n");
    }

    Ambient_Factor   = 0.25f;
    Diffusive_Factor = 1.0f;
    Specular_Factor  = 1.0f;
    Range_Factor     = 1.0f;

    fds::g_mainFaces.resize(8192);

    Scene* sc = buildHaloTestScene();
    SetCurrentScene(sc);
    View = sc->CameraHead;
    Scene_RebuildMatTable(sc);

    // Omni at world (0, 200, 0), range 600 → sphere ⌀1200.
    // Poses chosen to exercise:
    //   A inside_center     — camera at omni center: every pixel inside
    //                         sphere, sphereDisc cull never fires
    //   B inside_offset     — inside sphere, off-center: typical "near
    //                         the light" camera position
    //   C inside_edge       — just inside sphere: near-edge cases for
    //                         the analytic atan args (large |arg|)
    //   D outside_close     — just outside sphere: sphere fills most of
    //                         view, but the bottom edge starts to clip
    //   E outside_mid       — outside, sphere ≈ 1/4 of view
    //   F outside_far       — far outside, sphere a small ball
    //   G outside_side      — sphere off-screen-center
    //   H outside_below     — looking up at the sphere (omni above camera)
    struct HaloPose {
        const char* name;
        Vector      eye;
        Vector      target;
        const char* desc;
    };
    const HaloPose poses[] = {
        {"A_inside_offset", Vector( 200,  200, -200), Vector(  0, 200,    0),
            "inside sphere, off-center, looking at omni"},
        {"B_inside_edge",   Vector( 550,  200,    0), Vector(  0, 200,    0),
            "just inside sphere boundary, looking at omni"},
        {"C_outside_close", Vector( 700,  200,    0), Vector(  0, 200,    0),
            "just outside sphere (~100 units), sphere fills view"},
        {"D_outside_mid",   Vector(1500,  400,    0), Vector(  0, 200,    0),
            "outside (~900 units off), sphere ≈ 1/4 of view"},
        {"E_outside_far",   Vector(3000,  600,    0), Vector(  0, 200,    0),
            "far outside, sphere is a small ball in view"},
        {"F_outside_side",  Vector(1500,  400, 1000), Vector(800, 200, 1000),
            "outside, sphere off-screen-center (lateral)"},
        {"G_outside_below", Vector(   0, -400,  -1200), Vector(  0, 200,    0),
            "below+behind, looking up at the omni"},
    };

    int produced = 0;
    for (const HaloPose& p : poses) {
        View->ISource = p.eye;
        buildLookAt(View->ISource, p.target, View->Mat);
        CalcPersp(View);
        FOVX = View->PerspX;
        FOVY = View->PerspY;

        std::memset(VPage,   0, PageSize);
        std::memset(ZPage16, 0, XRes * YRes * sizeof(word));

        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
        Lighting(sc);
        if (CAll) {
            Radix_Sort(FList, SList, CAll);
            Render(RenderPath::ForceDeferred);
        }

        char colorPath[1024];
        std::snprintf(colorPath, sizeof(colorPath),
                      "%s/halotest_%s.ppm", cfg.outDir.c_str(), p.name);
        write_ppm(colorPath, MainSurf->Data, xres, yres, MainSurf->BPSL);
        std::fprintf(stderr,
            "[HALOTEST] %-16s eye=(%5.0f,%5.0f,%6.0f) tgt=(%5.0f,%5.0f,%5.0f)  %s -> %s\n",
            p.name,
            p.eye.x, p.eye.y, p.eye.z,
            p.target.x, p.target.y, p.target.z,
            p.desc, colorPath);
        ++produced;
    }

    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}

int RunConeTest(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    if (!fds::FeatureFlags::draw_cones()) {
        std::fprintf(stderr,
            "[CONETEST] WARNING: --draw_cones is not set; the cone pass\n"
            "[CONETEST] will be a no-op. Re-run with --deferred --draw_cones.\n");
    }

    Ambient_Factor   = 0.25f;
    Diffusive_Factor = 1.0f;
    Specular_Factor  = 1.0f;
    Range_Factor     = 1.0f;

    fds::g_mainFaces.resize(8192);

    Scene* sc = buildConeTestScene();
    SetCurrentScene(sc);
    View = sc->CameraHead;
    Scene_RebuildMatTable(sc);

    // Six canonical viewpoints A–F. Spot apex at world (0,400,0)
    // shining DOWN (cone direction = -Y world). All cameras look in
    // the cone direction (-Y); a small forward offset on the target
    // avoids buildLookAt's degenerate up×forward case (world UP=+Y).
    //
    // Predictions (assuming 35° half-angle, range=800):
    //   A in-front   inside cone, apex behind → uniform bright (was
    //                the circle-hole bug)
    //   B behind     ground at y=0 is 600 below apex (within 800 range),
    //                expect to see the cone column going down past us
    //   C side       above ground, off to side → cone visible past
    //                lateral_offset × cot(35°) along ray
    //   D above      camera above apex height → see cone passing below
    //   E inside     identical kinematics to A; included as the
    //                explicit "inside, looking down" test
    //   F inside_up  camera in beam, looking BACK at apex → see cone
    //                converging upward at the apex on screen
    // plus G/H/I a "behind, varying distance" sweep so we can watch
    // the cone-cuts-off-as-you-back-up behavior the user described.
    struct ConePose {
        const char* name;
        Vector      eye;
        Vector      target;
        const char* desc;
    };
    const ConePose poses[] = {
        // (A) In front of cone (inside beam, ahead of apex along D=-Y).
        // Camera at y=200 inside cone (apex at y=400, beam goes down).
        // Looking down with small forward tilt for non-degenerate lookAt.
        {"A_in_front",     Vector(  0, 200,  10), Vector( 0, 100, 11),
            "in front (inside beam), looking down beam direction"},

        // (B) Behind cone (behind apex, opposite to D). Camera at y=700
        // ABOVE apex. Looking down. Apex is in front of us, cone
        // extends past apex toward ground at y=0.
        {"B_behind",       Vector(  0, 700,  10), Vector( 0,   0, 11),
            "behind apex, looking forward into beam direction"},

        // (C) Side of cone, looking with beam. Camera at x=600 offset
        // (beyond beam radius), at apex height. Looking straight down.
        // Per math: cone visible past d·cot(35°) ≈ 856 below.
        {"C_side",         Vector(600, 400,  10), Vector(600,   0, 11),
            "side (perpendicular offset), looking down beam direction"},

        // (D) Above cone, looking with beam. Camera at y=900 well above
        // apex, perpendicular offset 200. Looking down. Same family
        // as (C) but offset is in y instead of x.
        {"D_above",        Vector(200, 900,  10), Vector(200,   0, 11),
            "above + offset, looking down (sweep through cone)"},

        // (E) Inside cone, looking with beam. Same as (A) explicitly
        // for the test-name "inside_down" the user has been referencing.
        // (kept distinct to make it obvious the math equates them)
        {"E_inside",       Vector(  0, 300,  10), Vector( 0, 100, 11),
            "inside cone (deeper than A), looking down beam direction"},

        // (F) Inside cone, looking AGAINST beam direction. Camera at
        // y=200, looking UP at the apex. Apex above. Cone is between
        // camera and apex; should be visible converging at the apex
        // projected on screen.
        {"F_inside_up",    Vector(  0, 200,  10), Vector( 0, 700,  9),
            "inside cone, looking BACK at apex (against beam)"},

        // Sweep "behind apex" at increasing distance to reproduce the
        // user-reported "cone cuts off as we back up" behavior.
        // Apex at y=400; with range=800, cone reaches down to y=-400.
        // Ground at y=0 caps the visible cone, so as the camera moves
        // higher above the apex the camera→ground rays for the lower
        // half of the cone start exceeding the range sphere → cone
        // shrinks from the bottom up.
        {"G_behind_near",  Vector(  0, 500,  10), Vector( 0,   0, 11),
            "behind apex, close (y=500): full cone visible"},
        {"H_behind_mid",   Vector(  0, 900,  10), Vector( 0,   0, 11),
            "behind apex, mid  (y=900): top of cone visible, bottom cut"},
        {"I_behind_far",   Vector(  0,1400,  10), Vector( 0,   0, 11),
            "behind apex, far  (y=1400): cone shrinking, mostly gone"},

        // Look down AT the cone target (the ground spot the spotlight
        // illuminates). User asked for these — they're the poses where
        // the "bottom circle brighter than cone shaft" discontinuity
        // is most visible.
        // Target = (0, 0, 0) — directly under the spot apex on the
        // ground. Camera placed above/behind/beside, looking at target.
        {"J_above_target", Vector(   0, 1200,  10), Vector( 0,   0,  0),
            "above the cone, looking down at its ground target"},
        {"K_behind_target",Vector(   0,  800,  900), Vector( 0,   0,  0),
            "behind+above, looking down at the cone's ground target"},
        {"L_beside_target",Vector( 900,  800,    0), Vector( 0,   0,  0),
            "beside+above, looking down at the cone's ground target"},
    };

    int produced = 0;
    for (const ConePose& p : poses) {
        View->ISource = p.eye;
        buildLookAt(View->ISource, p.target, View->Mat);
        CalcPersp(View);
        FOVX = View->PerspX;
        FOVY = View->PerspY;

        std::memset(VPage,   0, PageSize);
        std::memset(ZPage16, 0, XRes * YRes * sizeof(word));

        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
        Lighting(sc);
        if (CAll) {
            Radix_Sort(FList, SList, CAll);
            Render(RenderPath::ForceDeferred);
        }

        char colorPath[1024];
        std::snprintf(colorPath, sizeof(colorPath),
                      "%s/conetest_%s.ppm", cfg.outDir.c_str(), p.name);
        write_ppm(colorPath, MainSurf->Data, xres, yres, MainSurf->BPSL);
        std::fprintf(stderr,
            "[CONETEST] %-13s eye=(%5.0f,%4.0f,%5.0f) tgt=(%5.0f,%4.0f,%5.0f)  %s -> %s\n",
            p.name,
            p.eye.x, p.eye.y, p.eye.z,
            p.target.x, p.target.y, p.target.z,
            p.desc, colorPath);
        ++produced;
    }

    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}

// Reproduce the user-reported seaside-view bug: stand outside the city
// over open water, look back at the coastline. The reflective windows on
// the water-facing side of the nearest building should reflect SKY/SEA
// (the empty hemisphere behind the camera), not buildings.
//
// We don't force the synthetic panorama; the bake runs normally so we
// see the actual reflected scenery.
int RunCitySeasideTest(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    Initialize_City();
    Scene* sc = getCityScene();
    if (!sc) {
        std::fprintf(stderr, "[SEASIDE] getCityScene() returned null\n");
        return 4;
    }

    Timer = 0;
    auto driver = createCityScene();
    driver->init();
    if (!View) View = sc->CameraHead;

    // Find the city's xz extent by scanning building positions, and
    // identify a RECTANGULAR reflective building near each compass
    // extreme. "Rectangular" = has reflective faces with axis-aligned
    // normals on at least 3 of the 4 compass sides; this filters out
    // angular/hexagonal buildings (like b5) where every face is tilted
    // and no clean side-on reflection sample exists.
    float xmin = 1e30f, xmax = -1e30f, zmin = 1e30f, zmax = -1e30f;
    auto isRectangular = [](Object* o) {
        TriMesh* T = (TriMesh*)o->Data;
        bool px=false, nx=false, pz=false, nz=false;
        for (int fi = 0; fi < T->FIndex; ++fi) {
            Face* F = &T->Faces[fi];
            if (!(F->Flags & Face_Reflective)) continue;
            if (F->N.x >  0.95f) px = true;
            if (F->N.x < -0.95f) nx = true;
            if (F->N.z >  0.95f) pz = true;
            if (F->N.z < -0.95f) nz = true;
        }
        return (int(px) + int(nx) + int(pz) + int(nz)) >= 3;
    };
    Object* extZpos = nullptr; float extZposVal = -1e30f;
    Object* extZneg = nullptr; float extZnegVal =  1e30f;
    Object* extXpos = nullptr; float extXposVal = -1e30f;
    Object* extXneg = nullptr; float extXnegVal =  1e30f;
    for (Object* o = sc->ObjectHead; o; o = o->Next) {
        if (o->Type != Obj_TriMesh || !o->Name) continue;
        if (!(o->Name[0] == 'b' && std::isdigit((unsigned char)o->Name[1]))) continue;
        if (!o->Reflection) continue;
        TriMesh* T = (TriMesh*)o->Data;
        Vector bs;
        MatrixXVector(T->RotMat, &T->BSphereCtr, &bs);
        float wx = T->IPos.x + bs.x;
        float wz = T->IPos.z + bs.z;
        if (wx < xmin) xmin = wx;
        if (wx > xmax) xmax = wx;
        if (wz < zmin) zmin = wz;
        if (wz > zmax) zmax = wz;
        if (!isRectangular(o)) continue;
        if (wz > extZposVal) { extZposVal = wz; extZpos = o; }
        if (wz < extZnegVal) { extZnegVal = wz; extZneg = o; }
        if (wx > extXposVal) { extXposVal = wx; extXpos = o; }
        if (wx < extXnegVal) { extXnegVal = wx; extXneg = o; }
    }
    std::fprintf(stderr,
        "[SEASIDE] city extent x=[%.0f,%.0f] z=[%.0f,%.0f]\n",
        xmin, xmax, zmin, zmax);

    // Dump all rectangular reflective buildings + their positions so we
    // can let the user pick one by name.
    std::fprintf(stderr, "[SEASIDE] rectangular reflective buildings:\n");
    for (Object* o = sc->ObjectHead; o; o = o->Next) {
        if (o->Type != Obj_TriMesh || !o->Name) continue;
        if (!(o->Name[0] == 'b' && std::isdigit((unsigned char)o->Name[1]))) continue;
        if (!o->Reflection) continue;
        if (!isRectangular(o)) continue;
        TriMesh* T = (TriMesh*)o->Data;
        Vector bs;
        MatrixXVector(T->RotMat, &T->BSphereCtr, &bs);
        std::fprintf(stderr,
            "[SEASIDE]   %-12s pos=(%6.0f,%6.0f,%6.0f) rad=%.0f\n",
            o->Name, T->IPos.x + bs.x, T->IPos.y + bs.y, T->IPos.z + bs.z,
            std::sqrt(T->BSphereRad));
    }

    auto bsCenter = [](Object* o) {
        TriMesh* T = (TriMesh*)o->Data;
        Vector bs; MatrixXVector(T->RotMat, &T->BSphereCtr, &bs);
        return Vector(T->IPos.x + bs.x, T->IPos.y + bs.y, T->IPos.z + bs.z);
    };

    // Pick one representative of each rectangular mesh type so we can
    // see what each building shape looks like head-on.
    Object* sampleB1 = nullptr;
    Object* sampleB3 = nullptr;
    Object* sampleB4 = nullptr;
    for (Object* o = sc->ObjectHead; o; o = o->Next) {
        if (o->Type != Obj_TriMesh || !o->Name) continue;
        if (!isRectangular(o)) continue;
        if (!sampleB1 && std::strcmp(o->Name, "b1.lwo") == 0) sampleB1 = o;
        if (!sampleB3 && std::strcmp(o->Name, "b3.lwo") == 0) sampleB3 = o;
        if (!sampleB4 && std::strcmp(o->Name, "b4.lwo") == 0) sampleB4 = o;
    }

    // Pick b3 instances at different city-edge extremes for the
    // compare-with-back-view test: from one camera position, shoot
    // forward (at building) and backward (away over the sea).
    auto pickB3 = [&](auto pred, float startVal) -> Object* {
        Object* best = nullptr;
        float bestVal = startVal;
        for (Object* o = sc->ObjectHead; o; o = o->Next) {
            if (o->Type != Obj_TriMesh || !o->Name) continue;
            if (std::strcmp(o->Name, "b3.lwo") != 0) continue;
            if (!o->Reflection) continue;
            TriMesh* T = (TriMesh*)o->Data;
            Vector bs;
            MatrixXVector(T->RotMat, &T->BSphereCtr, &bs);
            float wx = T->IPos.x + bs.x;
            float wz = T->IPos.z + bs.z;
            if (pred(wx, wz, bestVal)) { bestVal = pred(wx, wz, bestVal) ? (std::fabs(wx) > std::fabs(wz) ? wx : wz) : bestVal; best = o; }
        }
        return best;
    };
    // Simpler: 3 specific extreme picks.
    Object* b3xpos = nullptr;  float v = -1e30f;
    Object* b3xneg = nullptr;  float v2 =  1e30f;
    Object* b3zpos = nullptr;  float v3 = -1e30f;
    Object* b3zneg = nullptr;  float v4 =  1e30f;
    for (Object* o = sc->ObjectHead; o; o = o->Next) {
        if (o->Type != Obj_TriMesh || !o->Name) continue;
        if (std::strcmp(o->Name, "b3.lwo") != 0) continue;
        if (!o->Reflection) continue;
        TriMesh* T = (TriMesh*)o->Data;
        Vector bs;
        MatrixXVector(T->RotMat, &T->BSphereCtr, &bs);
        float wx = T->IPos.x + bs.x;
        float wz = T->IPos.z + bs.z;
        if (wx > v ) { v  = wx; b3xpos = o; }
        if (wx < v2) { v2 = wx; b3xneg = o; }
        if (wz > v3) { v3 = wz; b3zpos = o; }
        if (wz < v4) { v4 = wz; b3zneg = o; }
    }

    struct Pose { const char* name; Object* tgt; Vector outDir; };
    Pose poses[] = {
        {"zpos_seaside", extZpos, Vector(0,0, 1)},
        {"zneg_seaside", extZneg, Vector(0,0,-1)},
        {"xpos_seaside", extXpos, Vector( 1,0,0)},
        {"xneg_seaside", extXneg, Vector(-1,0,0)},
        {"sample_b1",    sampleB1, Vector(0,0,-1)},
        {"sample_b3",    sampleB3, Vector(0,0,-1)},
        {"sample_b4",    sampleB4, Vector(0,0,-1)},
        // Front/back compare for several b3 instances at city extremes.
        // _front looks at the building's seaward face; _back is rotated
        // 180° and shows what's actually behind the camera.
        {"b3xpos_front", b3xpos, Vector( 1,0,0)},
        {"b3xpos_back",  b3xpos, Vector( 1,0,0)},
        {"b3xneg_front", b3xneg, Vector(-1,0,0)},
        {"b3xneg_back",  b3xneg, Vector(-1,0,0)},
        {"b3zpos_front", b3zpos, Vector(0,0, 1)},
        {"b3zpos_back",  b3zpos, Vector(0,0, 1)},
        {"b3zneg_front", b3zneg, Vector(0,0,-1)},
        {"b3zneg_back",  b3zneg, Vector(0,0,-1)},
    };

    // Distance-sweep diagnostic at user-reported camera position
    // (-3682, 530, -3238) fwd=(0.974, 0.177, -0.142). Render the SAME
    // view at multiple distances along the forward axis to capture the
    // "reflection changes wildly with small distance change" behaviour.
    {
        const Vector userPos(-3682.0f, 530.0f, -3238.0f);
        const Vector userFwd(0.974f, 0.177f, -0.142f);
        const float deltas[] = {
              0.0f,   200.0f,   400.0f,   600.0f,   800.0f,
           1000.0f,  1100.0f,  1200.0f,  1300.0f,  1400.0f,
           1450.0f,  1475.0f,  1500.0f,  1525.0f,  1550.0f,
           1575.0f,  1600.0f,  1700.0f,  1800.0f,  2000.0f,
        };
        char buf[64];
        for (float d : deltas) {
            std::srand(0);
            View->ISource.x = userPos.x + userFwd.x * d;
            View->ISource.y = userPos.y + userFwd.y * d;
            View->ISource.z = userPos.z + userFwd.z * d;
            Vector lookAt(View->ISource.x + userFwd.x,
                          View->ISource.y + userFwd.y,
                          View->ISource.z + userFwd.z);
            buildLookAt(View->ISource, lookAt, View->Mat);
            std::memset(VPage,   0, PageSize);
            std::memset(ZPage16, 0, XRes * YRes * sizeof(word));
            Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
            if (CAll) {
                Radix_Sort(FList, SList, CAll);
                Render(RenderPath::ForceForward);
            }
            std::snprintf(buf, sizeof(buf), "%s/distsweep_%+05d.ppm",
                          cfg.outDir.c_str(), int(d));
            write_ppm(buf, MainSurf->Data, xres, yres, MainSurf->BPSL);
            std::fprintf(stderr, "[SEASIDE] distsweep d=%+5.0f cam=(%.0f,%.0f,%.0f) -> %s\n",
                d, View->ISource.x, View->ISource.y, View->ISource.z, buf);
        }
    }

    // Dump each target building's actual baked panorama once, before
    // any cameras are positioned. We can then read off what content the
    // lookup formula resolves to for the seaward reflected ray.
    auto dumpPano = [&](Object* o, const char* tag) {
        if (!o || !o->Reflection || !o->Reflection->Txtr || !o->Reflection->Txtr->Data) return;
        Texture* T = o->Reflection->Txtr;
        const int W = T->SizeX, H = T->SizeY;
        // The bake stores in tile-swizzled layout (Sachletz). The raw
        // Data buffer is post-swizzle so a direct row-by-row dump shows
        // tile-shuffled output, not human-readable content. To keep this
        // diagnostic simple just sample using the same packed_tile_u/v
        // path the rasterizer uses (LogWidth=LogHeight=10) and write a
        // de-swizzled PPM.
        char path[1024];
        std::snprintf(path, sizeof(path),
                      "%s/seaside_pano_%s_%s.ppm",
                      cfg.outDir.c_str(), tag, o->Name);
        std::FILE* f = std::fopen(path, "wb");
        if (!f) return;
        std::fprintf(f, "P6\n%d %d\n255\n", W, H);
        std::vector<unsigned char> row(W * 3);
        const dword* data = (const dword*)T->Data;
        const int LogW = T->LSizeX;
        const int LogH = T->LSizeY;
        const int umask = (1 << LogW) - 1;
        const int vmask = (1 << LogH) - 1;
        // Re-derive the same packed-tile address as packed_tile_u/v
        // (vbits = LogH). offset = (u&3) | ((u<<vbits)&swizzled_umask)
        //                       + ((v&vmask) << 2)
        const int swz_umask = (umask >> 2) << (2 + LogH);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int tu = (x & 3) | ((x << LogH) & swz_umask);
                const int tv = (y & vmask) << 2;
                dword px = data[tu + tv];
                row[x*3+0] = (px >> 16) & 0xFF;
                row[x*3+1] = (px >>  8) & 0xFF;
                row[x*3+2] = (px      ) & 0xFF;
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        std::fprintf(stderr, "[SEASIDE] dumped panorama -> %s\n", path);
    };

    int produced = 0;
    for (const Pose& p : poses) {
        if (!p.tgt) continue;
        dumpPano(p.tgt, p.name);
        Vector c = bsCenter(p.tgt);
        // Cam at the building's mid-height, ~600 units further out
        // along the outward axis. Look-at the building's center, so
        // the camera is level (no upward tilt) and the seaward face
        // is rendered head-on.
        const float dist = 600.0f;
        View->ISource.x = c.x + p.outDir.x * dist;
        View->ISource.y = c.y;
        View->ISource.z = c.z + p.outDir.z * dist;
        // _back variant: same camera position but rotated 180° to face
        // outward (away from the building) instead of inward.
        const bool isBack = std::strstr(p.name, "_back") != nullptr;
        Vector lookAt = isBack
            ? Vector(View->ISource.x + p.outDir.x,
                     View->ISource.y,
                     View->ISource.z + p.outDir.z)
            : c;
        buildLookAt(View->ISource, lookAt, View->Mat);

        std::fprintf(stderr,
            "[SEASIDE] %s: target=%s tgt=(%.0f,%.0f,%.0f) cam=(%.0f,%.0f,%.0f)\n",
            p.name, p.tgt->Name, c.x, c.y, c.z,
            View->ISource.x, View->ISource.y, View->ISource.z);

        std::srand(0);
        std::memset(VPage,   0, PageSize);
        std::memset(ZPage16, 0, XRes * YRes * sizeof(word));
        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
        // Diagnostic: walk the target building's faces. For each
        // reflective face that's both front-facing and visible (i.e.,
        // not all 3 verts share a frustum-cull bit), print its post-
        // Reflective_Mapper EU/EV. The cv-pull hack distorts the
        // reflected ray significantly; reading the actual EU/EV
        // is the only way to know what slot of the bake is sampled.
        {
            TriMesh* T = (TriMesh*)p.tgt->Data;
            // AP = camera in object space (object has identity rot
            // assumption — fine for buildings). Used for backface test.
            Vector AP = View->ISource - T->IPos;
            int printed = 0;
            for (int fi = 0; fi < T->FIndex && printed < 3; ++fi) {
                Face* F = &T->Faces[fi];
                if (!(F->Flags & Face_Reflective)) continue;
                // Skip back-faces (front-face: AP·N >= NormProd, since
                // NormProd = -dot(A.Pos, N) so plane eq is dot(P,N) =
                // -NormProd; AP is on +N side iff AP·N > -NormProd =>
                // AP·N + NormProd > 0).
                float side = AP.x*F->N.x + AP.y*F->N.y + AP.z*F->N.z + F->NormProd;
                if (side <= 0) continue;
                // Skip faces where Reflective_Mapper didn't write
                // (EU1=EV1=EU2=...=0 sentinel).
                if (F->EU1 == 0 && F->EV1 == 0 && F->EU2 == 0 && F->EV2 == 0) continue;
                // Only print faces aligned with the seaward direction.
                float d = F->N.x*p.outDir.x + F->N.y*p.outDir.y + F->N.z*p.outDir.z;
                if (d < 0.3f) continue;
                std::fprintf(stderr,
                    "[SEASIDE]   face idx=%d N=(%.2f,%.2f,%.2f) align=%.2f side=%.1f "
                    "EU=(%.3f,%.3f,%.3f) EV=(%.3f,%.3f,%.3f)\n",
                    fi, F->N.x, F->N.y, F->N.z, d, side,
                    F->EU1, F->EU2, F->EU3, F->EV1, F->EV2, F->EV3);
                ++printed;
            }
        }
        if (CAll) {
            Radix_Sort(FList, SList, CAll);
            Render(RenderPath::ForceForward);
        }
        char colorPath[1024];
        std::snprintf(colorPath, sizeof(colorPath),
                      "%s/seaside_%s_color.ppm", cfg.outDir.c_str(), p.name);
        write_ppm(colorPath, MainSurf->Data, xres, yres, MainSurf->BPSL);
        ++produced;
    }

    driver->cleanup();
    driver.reset();
    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}

bool ParseBenchArgs(int argc, const char* argv[], BenchConfig& cfg) {
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (!starts_with(a, "--bench=")) continue;
        std::string_view rest = a.substr(strlen("--bench="));
        std::size_t at = rest.find('@');
        if (at == std::string_view::npos) {
            cfg.kind = std::string(rest);
        } else {
            cfg.kind = std::string(rest.substr(0, at));
            std::string_view tail = rest.substr(at + 1);
            // Parse comma-separated key=value pairs.
            while (!tail.empty()) {
                auto comma = tail.find(',');
                std::string_view kv = (comma == std::string_view::npos)
                    ? tail : tail.substr(0, comma);
                auto eq = kv.find('=');
                if (eq != std::string_view::npos) {
                    std::string_view k = kv.substr(0, eq);
                    std::string_view v = kv.substr(eq + 1);
                    if (k == "scene") {
                        cfg.scene = std::string(v);
                    } else {
                        long lv = std::strtol(std::string(v).c_str(), nullptr, 10);
                        if (k == "iters") cfg.iters = static_cast<int>(lv);
                        else if (k == "seed") cfg.seed = static_cast<int>(lv);
                        else if (k == "t") cfg.ts = static_cast<int32_t>(lv);
                        else if (k == "tend") cfg.tend = static_cast<int32_t>(lv);
                        else if (k == "xres") cfg.xres = static_cast<int>(lv);
                        else if (k == "yres") cfg.yres = static_cast<int>(lv);
                    }
                }
                tail = (comma == std::string_view::npos)
                    ? std::string_view{} : tail.substr(comma + 1);
            }
        }
        found = true;
    }
    return found;
}

int RunRasterBench(const BenchConfig& cfg, int xres, int yres) {
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    FillerTestSnapshotInit(xres, yres);

    // Warmup — first iteration tends to include first-touch cache misses
    // and any lazy initialization that hides under timing if not excluded.
    for (int i = 0; i < 3; ++i) FillerTestSnapshotRender(cfg.seed);

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    for (int i = 0; i < cfg.iters; ++i) {
        FillerTestSnapshotRender(cfg.seed);
    }
    auto t1 = clock::now();

    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double mean_ms = total_ms / cfg.iters;
    // Pixels processed per iter is the screen area (the fixture covers a big
    // quad — close enough for relative comparison; absolute Mpx/s is approx).
    double mpx_per_iter = static_cast<double>(xres) * yres / 1e6;
    double mpx_per_sec = mean_ms > 0 ? mpx_per_iter * 1000.0 / mean_ms : 0.0;

    std::printf("[BENCH] raster: iters=%d  res=%dx%d  seed=%d\n",
                cfg.iters, xres, yres, cfg.seed);
    std::printf("[BENCH] total=%.2f ms  mean=%.3f ms/iter  ~%.1f Mpx/s\n",
                total_ms, mean_ms, mpx_per_sec);
    std::fflush(stdout);

    FillerTestSnapshotCleanup();
    ThreadPool::instance().close();
    return 0;
}

int RunSceneBench(const BenchConfig& cfg, int xres, int yres) {
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    std::unique_ptr<SceneDriver> driver;
    if (cfg.scene == "city") {
        Initialize_City();
        driver = createCityScene();
    } else if (cfg.scene == "greets") {
        Initialize_Greets();
        driver = createGreetsScene();
    } else {
        std::fprintf(stderr, "[BENCH] scene='%s' not supported (try city, greets)\n",
                     cfg.scene.c_str());
        ThreadPool::instance().close();
        return 2;
    }

    driver->init();

    // Warm-up: one tick at the bench Timer value to populate any lazy
    // first-touch state (mipmaps, model caches) so it doesn't skew iter 0.
    std::srand(0);
    Timer = cfg.ts;
    std::memset((void*)Keyboard, 0, sizeof(Keyboard));
    driver->tick();

    using clock = std::chrono::high_resolution_clock;
    const bool sweep = (cfg.tend > cfg.ts);
    auto t0 = clock::now();
    for (int i = 0; i < cfg.iters; ++i) {
        std::srand(0);
        if (sweep) {
            // Linearly map iter i ∈ [0, iters-1] → Timer ∈ [ts, tend].
            const int32_t span = cfg.tend - cfg.ts;
            Timer = cfg.ts + int32_t((int64_t(span) * i) / std::max(1, cfg.iters - 1));
        } else {
            Timer = cfg.ts;
        }
        driver->tick();
    }
    auto t1 = clock::now();

    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double mean_ms = cfg.iters > 0 ? total_ms / cfg.iters : 0.0;

    if (sweep) {
        std::fprintf(stderr,
                     "[BENCH] scene=%s t=%d..%d iters=%d total=%.2f ms  mean=%.3f ms/iter\n",
                     cfg.scene.c_str(), cfg.ts, cfg.tend, cfg.iters, total_ms, mean_ms);
    } else {
        std::fprintf(stderr,
                     "[BENCH] scene=%s t=%d iters=%d total=%.2f ms  mean=%.3f ms/iter\n",
                     cfg.scene.c_str(), cfg.ts, cfg.iters, total_ms, mean_ms);
    }
    std::fflush(stderr);

    // For greets, dump centroid stats so native vs wasm runs can be
    // compared exactly. Centered text + symmetric smear should put the
    // centroid of every stage near (128.0, 128.0).
    if (cfg.scene == "greets") {
        Greets_DumpStageCentroids(stderr, "post-bench");
    }

    driver->cleanup();
    driver.reset();

    ThreadPool::instance().close();
    return 0;
}

// Synthetic centered glyph stamp for the greets pipe bench. Center pixel
// at (xc, yc), an `s`-pixel-radius square, all white, alpha 255. Fills a
// 256x256 ARGB8888 buffer.
static void fillCenteredStamp(uint32_t *buf, int w, int h, int xc, int yc, int s) {
    std::memset(buf, 0, sizeof(uint32_t) * w * h);
    for (int y = yc - s; y <= yc + s; ++y) {
        if (y < 0 || y >= h) continue;
        for (int x = xc - s; x <= xc + s; ++x) {
            if (x < 0 || x >= w) continue;
            buf[y * w + x] = 0xFFFFFFFF;
        }
    }
}

static void centroid256(const uint32_t *buf, double &cx, double &cy, double &mass) {
    cx = 0; cy = 0; mass = 0;
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            uint32_t px = buf[y * 256 + x];
            int b = px & 0xFF;
            int g = (px >> 8) & 0xFF;
            int r = (px >> 16) & 0xFF;
            double lum = r + g + b;
            cx += x * lum;
            cy += y * lum;
            mass += lum;
        }
    }
    if (mass > 0) { cx /= mass; cy /= mass; }
}

int RunGreetsPipeBench(const BenchConfig& cfg, int /*xres*/, int /*yres*/) {
    if (!initSnapshotEnvironment(256, 256)) return 3;

    constexpr int W = 256;
    constexpr int H = 256;
    constexpr int GRID = 33;

    auto allocAligned = [](size_t bytes) -> uint32_t* {
        return static_cast<uint32_t*>(_aligned_malloc(bytes, 16));
    };

    uint32_t *codeImage = allocAligned(W * H * 4);
    uint32_t *codeBuf   = allocAligned(W * H * 4);
    uint32_t *oldBuf    = allocAligned(W * H * 4);
    uint32_t *scaledBuf = allocAligned(W * H * 4);
    GridPointT *codeGP  = new GridPointT[GRID * GRID];
    GridPointT *smearGP = new GridPointT[GRID * GRID];

    // Centered 16x16 stamp at exact center (124..139 covers center 128).
    fillCenteredStamp(codeImage, W, H, 128, 128, 4);
    std::memset(oldBuf,    0, W * H * 4);
    std::memset(scaledBuf, 0, W * H * 4);

    Image codeImg{};  codeImg.Data = (dword*)codeImage; codeImg.x = W; codeImg.y = H;
    Image oldImg{};   oldImg.Data  = (dword*)oldBuf;    oldImg.x  = W; oldImg.y  = H;

    int iters = cfg.iters > 0 ? cfg.iters : 100;

    // Mirror the production wobbler exactly. scalex=scaley=0 to match the
    // GREET3 phase where the formula reduces to (x/256 - 0.5) * 0.25 + 0.5
    // for every grid point — every value is integer-exact in IEEE float.
    const float scalex = 0.0f;
    const float scaley = 0.0f;

    auto fillGrid = [](GridPointT *out, float ax, float ay, float bx, float by) {
        int j = 0;
        for (int y = 0; y <= 256; y += 8) {
            for (int x = 0; x <= 256; x += 8) {
                float u = (x / 256.0f - 0.5f) * (ax) + bx;
                float v = (y / 256.0f - 0.5f) * (ay) + by;
                out[j].u = static_cast<int32_t>(u * 65536.0);
                out[j].v = static_cast<int32_t>(v * 65536.0);
                if (out[j].u > 65535) out[j].u = 65535;
                if (out[j].v > 65535) out[j].v = 65535;
                if (out[j].u < 0) out[j].u = 0;
                if (out[j].v < 0) out[j].v = 0;
                ++j;
            }
        }
    };

    std::fprintf(stderr,
        "[GREETS-PIPE] iters=%d scalex=%.6f scaley=%.6f stamp@(128,128)\n",
        iters, scalex, scaley);

    // Print a 5x5 sample of the wobbler grid integers — deterministic
    // across iterations since scalex/scaley are fixed. Diff this between
    // native and wasm.
    fillGrid(codeGP, 0.25f + scalex, 0.25f + scaley, 0.5f, 0.5f);
    std::fprintf(stderr, "[GREETS-PIPE] Code_GP sample (gx,gy → u,v):\n");
    for (int gy = 0; gy < GRID; gy += 8) {
        for (int gx = 0; gx < GRID; gx += 8) {
            int idx = gy * GRID + gx;
            std::fprintf(stderr, "[GREETS-PIPE]   (%d,%d) u=%d v=%d\n",
                         gx, gy, codeGP[idx].u, codeGP[idx].v);
        }
    }

    // Constant alpha-blend ratios — keep them deterministic so the trail
    // converges identically across runs.
    DWord pSrc = 0xFFFFFFFFu;
    DWord pDst = 0x80808080u;

    for (int i = 0; i < iters; ++i) {
        fillGrid(codeGP,  0.25f + scalex, 0.25f + scaley, 0.5f, 0.5f);
        fillGrid(smearGP, 0.98f,           0.95f,          0.5f, 0.5f);

        GridRendererT(codeGP,  &codeImg, (dword*)codeBuf,   W, H);
        GridRendererT(smearGP, &oldImg,  (dword*)scaledBuf, W, H);

        AlphaBlend((byte*)codeBuf, (byte*)scaledBuf, pSrc, pDst, W * H * 4);
        std::memcpy(oldBuf, scaledBuf, W * H * 4);
    }

    double cx, cy, mass;
    centroid256(codeImage, cx, cy, mass);
    std::fprintf(stderr, "[GREETS-PIPE] CodeImage  cx=%9.4f cy=%9.4f mass=%.0f\n", cx, cy, mass);
    centroid256(codeBuf, cx, cy, mass);
    std::fprintf(stderr, "[GREETS-PIPE] CodeBuf    cx=%9.4f cy=%9.4f mass=%.0f\n", cx, cy, mass);
    centroid256(scaledBuf, cx, cy, mass);
    std::fprintf(stderr, "[GREETS-PIPE] ScaledBuf  cx=%9.4f cy=%9.4f mass=%.0f\n", cx, cy, mass);
    centroid256(oldBuf, cx, cy, mass);
    std::fprintf(stderr, "[GREETS-PIPE] OldBuf     cx=%9.4f cy=%9.4f mass=%.0f\n", cx, cy, mass);
    std::fflush(stderr);

    _aligned_free(codeImage);
    _aligned_free(codeBuf);
    _aligned_free(oldBuf);
    _aligned_free(scaledBuf);
    delete[] codeGP;
    delete[] smearGP;

    ThreadPool::instance().close();
    return 0;
}

int RunFlipBench(const BenchConfig& cfg, int xres, int yres) {
#ifdef __EMSCRIPTEN__
    (void)cfg; (void)xres; (void)yres;
    std::fprintf(stderr, "[BENCH] flip is native-only — emscripten SDL2 needs a real canvas/DOM\n");
    return 4;
#else
    if (cfg.xres > 0) xres = cfg.xres;
    if (cfg.yres > 0) yres = cfg.yres;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[BENCH] SDL_Init: %s\n", SDL_GetError());
        return 3;
    }
    SDL_Window *window = SDL_CreateWindow("flip-bench",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        xres, yres,
        SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "[BENCH] SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 3;
    }
    // Force software renderer so native numbers track the wasm path's
    // cost model (where SDL_RENDERER_ACCELERATED isn't usable).
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        std::fprintf(stderr, "[BENCH] SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 3;
    }
    SDL_Texture *texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        xres, yres);
    if (!texture) {
        std::fprintf(stderr, "[BENCH] SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 3;
    }

    // dst rect = full renderer (canvas == engine size, matches the
    // post-fix V_Flip path where letterbox bars come from CSS).
    int rw = 0, rh = 0;
    SDL_GetRendererOutputSize(renderer, &rw, &rh);
    SDL_Rect dst{0, 0, rw, rh};

    using clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;

    double tUnlock = 0, tCopy = 0, tPresent = 0, tLock = 0;
    int iters = cfg.iters > 0 ? cfg.iters : 200;

    // Warmup — first iter pays first-touch cache + GPU/canvas-side
    // first-frame work.
    for (int i = 0; i < 3; ++i) {
        void *pixels = nullptr;
        int pitch = 0;
        SDL_LockTexture(texture, NULL, &pixels, &pitch);
        std::memset(pixels, 0xab, static_cast<std::size_t>(pitch) * rh);
        SDL_UnlockTexture(texture);
        SDL_RenderCopy(renderer, texture, NULL, &dst);
        SDL_RenderPresent(renderer);
    }

    // Initial lock so the loop body can write before unlock-cycle starts.
    void *lockedPixels = nullptr;
    int lockedPitch = 0;
    SDL_LockTexture(texture, NULL, &lockedPixels, &lockedPitch);

    for (int i = 0; i < iters; ++i) {
        // "render" — write a frame's worth of pattern. Same byte cost as
        // an engine clear; we attribute this OUTSIDE the FLIP timings so
        // the per-stage numbers reflect just the SDL pipeline.
        std::memset(lockedPixels, static_cast<int>(i & 0xFF),
                    static_cast<std::size_t>(lockedPitch) * rh);

        auto t0 = clock::now();
        SDL_UnlockTexture(texture);
        auto t1 = clock::now();
        SDL_RenderCopy(renderer, texture, NULL, &dst);
        auto t2 = clock::now();
        SDL_RenderPresent(renderer);
        auto t3 = clock::now();
        SDL_LockTexture(texture, NULL, &lockedPixels, &lockedPitch);
        auto t4 = clock::now();

        tUnlock  += ms(t1 - t0).count();
        tCopy    += ms(t2 - t1).count();
        tPresent += ms(t3 - t2).count();
        tLock    += ms(t4 - t3).count();
    }

    SDL_UnlockTexture(texture);

    double divIters = static_cast<double>(iters);
    double mUnlock  = tUnlock  / divIters;
    double mCopy    = tCopy    / divIters;
    double mPresent = tPresent / divIters;
    double mLock    = tLock    / divIters;
    double mTotal   = mUnlock + mCopy + mPresent + mLock;

    std::fprintf(stderr,
        "[BENCH] flip: iters=%d  size=%dx%d  renderer=software\n"
        "[BENCH] stage           mean_ms     %%\n"
        "[BENCH] SDL_Unlock      %7.4f  %5.1f%%\n"
        "[BENCH] SDL_RenderCopy  %7.4f  %5.1f%%\n"
        "[BENCH] SDL_Present     %7.4f  %5.1f%%\n"
        "[BENCH] SDL_Lock        %7.4f  %5.1f%%\n"
        "[BENCH] TOTL            %7.4f\n",
        iters, rw, rh,
        mUnlock,  mTotal > 0 ? 100.0 * mUnlock  / mTotal : 0,
        mCopy,    mTotal > 0 ? 100.0 * mCopy    / mTotal : 0,
        mPresent, mTotal > 0 ? 100.0 * mPresent / mTotal : 0,
        mLock,    mTotal > 0 ? 100.0 * mLock    / mTotal : 0,
        mTotal);
    std::fflush(stderr);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
#endif
}

// ─── Lightmap test scene ────────────────────────────────────────────────────
// Synthetic shadow lightmap reproducer. One ground quad tessellated to
// subdiv × subdiv cells, one small occluder above casting a shadow, one
// static omni. Subdiv is the @t parameter so the same harness sweeps
// "2 big faces" → "many small faces" in one invocation.
//
// Harness is single-mode: it honors --shadow-lightmap on the CLI and
// produces one PPM per subdiv. Run twice (without and with the flag) and
// diff the output dirs to compare cube-tap vs lightmap on the same scene.

static Scene* buildLightmapTestScene(int subdiv) {
    if (subdiv < 1) subdiv = 1;
    if (subdiv > 128) subdiv = 128;  // cap so we don't allocate millions of verts

    Scene* Sc = (Scene*)getAlignedBlock(sizeof(Scene), 16);
    std::memset(Sc, 0, sizeof(Scene));
    Sc->NZP   = 5.0f;
    Sc->FZP   = 8000.0f;
    Sc->Flags = Scn_ZBuffer;
    Sc->Ambient.R = Sc->Ambient.G = Sc->Ambient.B = 20;
    Sc->Ambient.A = 255;

    Camera* Cam = (Camera*)getAlignedBlock(sizeof(Camera), 16);
    std::memset(Cam, 0, sizeof(Camera));
    Vector_Form(&Cam->ISource, 0, 1400, -2200);
    Matrix_Identity(Cam->Mat);
    Cam->IFOV = 60.0f;
    Sc->CameraHead = Cam;

    // Per-face hue via a single 256x256 texture containing a 16×16 grid
    // of solid-color tiles. Each tile is 16x16 px and a unique hue at
    // constant luminance (V=200/255, S=1). Face index k maps to tile
    // (k%16, (k/16)%16) — wraps after 256 faces, which is plenty for
    // debug subdivs (1..16 has ≤512 faces; the hue just repeats above).
    //
    // The deferred path stores matID + UV per pixel; lighting kernel
    // samples texture[matID] at UV. By giving each face a UV inside
    // its own tile, we get per-face color without needing per-face
    // materials. Lighting darkens the hue uniformly in shadow.
    constexpr int kHueTexSize = 256;
    constexpr int kHueTile    = 16;        // 16 tiles per axis, 16px each
    constexpr int kNumTiles   = kHueTile * kHueTile;
    Texture* hueTex = new Texture;
    std::memset(hueTex, 0, sizeof(Texture));
    uint32_t* hueData = (uint32_t*)_aligned_malloc(kHueTexSize * kHueTexSize * 4, 16);
    auto hsvToBgra = [](float h) -> uint32_t {
        const float V = 240.0f;
        float hh = h - std::floor(h);
        float f = hh * 6.0f; int i = int(f); float frac = f - float(i);
        float q = V * (1.0f - frac);
        float t = V * frac;
        float rr=0, gg=0, bb=0;
        switch (i % 6) {
            case 0: rr=V; gg=t; break;
            case 1: rr=q; gg=V; break;
            case 2: gg=V; bb=t; break;
            case 3: gg=q; bb=V; break;
            case 4: rr=t; bb=V; break;
            case 5: rr=V; bb=q; break;
        }
        return (uint32_t(255) << 24) | (uint32_t(byte(rr)) << 16)
             | (uint32_t(byte(gg)) << 8) | uint32_t(byte(bb));
    };
    for (int ty = 0; ty < kHueTile; ++ty) {
        for (int tx = 0; tx < kHueTile; ++tx) {
            const int tile = ty * kHueTile + tx;
            const uint32_t c = hsvToBgra(float(tile) / float(kNumTiles));
            for (int yy = 0; yy < kHueTile; ++yy) {
                for (int xx = 0; xx < kHueTile; ++xx) {
                    const int px = (ty * kHueTile + yy) * kHueTexSize
                                 + (tx * kHueTile + xx);
                    hueData[px] = c;
                }
            }
        }
    }
    hueTex->Data   = (byte*)hueData;
    hueTex->BPP    = 32;
    hueTex->SizeX  = kHueTexSize; hueTex->LSizeX = 8;
    hueTex->SizeY  = kHueTexSize; hueTex->LSizeY = 8;
    hueTex->Flags  = Txtr_Nomip | Txtr_Tiled;
    Sachletz((dword*)hueTex->Data, kHueTexSize, kHueTexSize);
    hueTex->Mipmap[0]  = hueTex->Data;
    hueTex->numMipmaps = 1;

    Material* matGround = getAlignedType<Material>(16);
    matGround->Txtr        = hueTex;
    matGround->BaseCol.R   = 255; matGround->BaseCol.G = 255;
    matGround->BaseCol.B   = 255; matGround->BaseCol.A = 255;
    matGround->Diffuse     = 1.0f;
    matGround->Luminosity  = 0.0f;
    matGround->Specular    = 0.0f;
    matGround->Flags       = Mat_RGBInterp;
    matGround->RelScene    = Sc;
    matGround->ID          = 0;
    matGround->Name        = strdup("lm_ground");

    // Occluder is mid-gray so it's visually distinct from the rainbow.
    Material* matOcc = makeSolidColorMat(Sc, "lm_occluder",
                                         80, 80, 80, 255,
                                         Mat_RGBInterp, 1);
    matOcc->Diffuse  = 1.0f;
    matOcc->Specular = 0.0f;
    linkMatToLib(matGround);
    linkMatToLib(matOcc);

    // Ground subdivided N×N. Spans [-1500, 1500] in X/Z at y=0.
    // subdiv=1 → 6 verts, 2 triangles (the "polys are too big" case).
    // subdiv=64 → 24576 verts, 8192 triangles.
    //
    // VERTEX DUPLICATION: each face gets its own 3 vertices (not shared
    // across faces), so we can stamp a face-unique vertex color and
    // visually see which triangle owns each pixel. Without this, any
    // per-face shadow artifact looks like a generic dark patch.
    //
    // Color scheme: HSV(hue = faceIdx/totalFaces, S=1, V=200/255). All
    // triangles share the same luminance ~200/255, so shading darkens
    // them uniformly — what changes is only the *hue*, not the brightness.
    // The shadow factor shows up as a luminance drop on top of the hue.
    {
        const float halfExtent = 1500.0f;
        const int   N = subdiv;
        const int   numFaces = N * N * 2;
        const int   numVerts = numFaces * 3;
        char name[32];
        std::snprintf(name, sizeof(name), "lm_ground_s%d", subdiv);
        TriMesh* ground = appendTriMesh(Sc, name, numVerts, numFaces);

        const Vector nrm(0, 1, 0);
        for (int iz = 0; iz < N; ++iz) {
            for (int ix = 0; ix < N; ++ix) {
                // Corner positions of this cell.
                const float u0 = float(ix    ) / float(N);
                const float u1 = float(ix + 1) / float(N);
                const float v0 = float(iz    ) / float(N);
                const float v1 = float(iz + 1) / float(N);
                Vector p00(-halfExtent + 2*halfExtent*u0, 0, -halfExtent + 2*halfExtent*v0);
                Vector p10(-halfExtent + 2*halfExtent*u1, 0, -halfExtent + 2*halfExtent*v0);
                Vector p01(-halfExtent + 2*halfExtent*u0, 0, -halfExtent + 2*halfExtent*v1);
                Vector p11(-halfExtent + 2*halfExtent*u1, 0, -halfExtent + 2*halfExtent*v1);

                const int cellIdx = iz * N + ix;

                // Two triangles per cell, each with 3 unique verts.
                for (int tri = 0; tri < 2; ++tri) {
                    const int faceIdx = cellIdx * 2 + tri;
                    const int vBase   = faceIdx * 3;
                    Vector vp[3];
                    if (tri == 0) { vp[0] = p00; vp[1] = p10; vp[2] = p11; }
                    else          { vp[0] = p00; vp[1] = p11; vp[2] = p01; }

                    // Spread hues across the whole tile grid based on this
                    // mesh's face count — otherwise at low subdiv all
                    // faces land in the first few tiles (all reds).
                    // tileIdx ≈ faceIdx * 256 / numFaces.
                    const int tileIdx = (faceIdx * kNumTiles) / std::max(numFaces, 1);
                    const int tileX = tileIdx % kHueTile;
                    const int tileY = (tileIdx / kHueTile) % kHueTile;
                    const float uMid = (float(tileX) + 0.5f) / float(kHueTile);
                    const float vMid = (float(tileY) + 0.5f) / float(kHueTile);

                    for (int k = 0; k < 3; ++k) {
                        Vertex* V = &ground->Verts[vBase + k];
                        V->Pos = vp[k];
                        V->N   = nrm;
                        V->TN  = nrm;
                        V->LR  = 255;  // unused in deferred — Mekalele
                        V->LG  = 255;  // reads texture, not vertex color
                        V->LB  = 255;
                        V->LA  = 255;
                        V->U   = uMid;  // all 3 verts → same tile center
                        V->V   = vMid;
                    }

                    Face* F = &ground->Faces[faceIdx];
                    F->A = &ground->Verts[vBase + 0];
                    F->B = &ground->Verts[vBase + 1];
                    F->C = &ground->Verts[vBase + 2];
                    F->N = nrm;
                    F->NormProd = -Dot_Product(&F->A->Pos, &F->N);
                    F->Txtr   = matGround;
                    F->Filler = TheOtherBarry<barry::TBlendMode::OVERWRITE,
                                               barry::TTextureMode::NORMAL>;
                    F->Flags  = 0;
                    F->uvFromVertices();
                }
            }
        }
    }

    // Occluder — a horizontal quad floating at y=400, [-300, 300]² in X/Z,
    // facing -Y (down). The omni above casts a clear square shadow onto
    // the ground below. Always 2 triangles regardless of ground subdiv —
    // we want the *ground* tessellation as the variable, not the occluder.
    {
        TriMesh* occ = appendTriMesh(Sc, "lm_occluder", 4, 2);
        QuadDef q = {
            // Wound so the normal faces -Y (down toward ground).
            { Vector(-300, 400,  300), Vector( 300, 400,  300),
              Vector( 300, 400, -300), Vector(-300, 400, -300) },
            Vector(0, -1, 0)
        };
        appendQuad(Sc, occ, 0, 0, q, matOcc,
                   TheOtherBarry<barry::TBlendMode::OVERWRITE,
                                 barry::TTextureMode::NORMAL>);
    }

    // Static omni high up at (0, 1500, 0) so falloff across the
    // 3000×3000 ground is gentler — at y=800 the corners are ~2x
    // farther than the center, which dims the rainbow at the edges.
    // Range 4000, intensity 50.
    Omni* O = appendTestOmni(Sc, Vector(0, 1500, 0),
                              /*r,g,b=*/ 1.0f, 1.0f, 1.0f,
                              /*intensity*/ 50.0f,
                              /*range*/ 4000.0f);
    O->Flags |= Omni_CastsShadow | Omni_StaticShadow;
    O->shadowMapRes = 512;

    return Sc;
}

int RunLightmapTest(const SnapshotConfig& cfg, int xres, int yres) {
    ensureOutDir(cfg.outDir);
    if (!initSnapshotEnvironment(xres, yres)) return 3;

    if (!fds::FeatureFlags::deferred()) {
        std::fprintf(stderr,
            "[LMTEST] WARNING: --deferred is not set; lightmap path won't\n"
            "[LMTEST] run. Re-run with --deferred --shadows [--shadow-lightmap].\n");
    }
    if (!fds::FeatureFlags::shadows()) {
        std::fprintf(stderr,
            "[LMTEST] WARNING: --shadows is not set; cube shadow bake skipped.\n");
    }
    const bool lmOn = fds::FeatureFlags::shadow_lightmap();
    std::fprintf(stderr,
        "[LMTEST] mode = %s (toggle with --shadow-lightmap; run twice and diff)\n",
        lmOn ? "LIGHTMAP" : "CUBE-TAP");

    // Low ambient so shadows aren't washed out; high diffuse so lit
    // pixels read at full color. The whole point is to make the shadow
    // boundary obvious against each triangle's distinct hue.
    Ambient_Factor   = 0.10f;
    Diffusive_Factor = 2.0f;
    Specular_Factor  = 0.0f;
    Range_Factor     = 1.0f;

    fds::g_mainFaces.resize(32768);

    std::vector<int32_t> subdivs = cfg.timestamps;
    if (subdivs.empty()) { subdivs = {1, 4, 16, 64}; }

    int produced = 0;
    for (int32_t subdiv : subdivs) {
        Scene* sc = buildLightmapTestScene(int(subdiv));
        SetCurrentScene(sc);
        View = sc->CameraHead;
        buildLookAt(View->ISource, Vector(0, 0, 0), View->Mat);
        CalcPersp(View);
        FOVX = View->PerspX;
        FOVY = View->PerspY;
        Scene_RebuildMatTable(sc);

        // Populate the global Polys (worst-case face count) and resize
        // g_mainFaces accordingly. Render_DeferredShadowMaps sizes its
        // per-light FList to Polys and SEGVs if that's still 0.
        // City calls FList_Allocate during Initialize_City; harness
        // scenes that don't run shadows can get away without it, but
        // the bake path needs it.
        FList_Allocate(sc);
        Animate_Objects(sc, true);
        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);

        // Shadow init — mirror GREETS.CPP ordering exactly so the
        // reproducer exercises the same code path the real scene hits.
        ShadowMaps_Rebuild(sc, 1024);
        CubeShadowMaps_Rebuild(sc, 512);
        ShadowMaps_BakeStatic(sc);
        fds::LightmapStampOrigBary(sc);  // no-op if --shadow-lightmap off
        fds::LightmapBake_Static(sc);    // no-op if --shadow-lightmap off

        std::memset(VPage,   0, PageSize);
        std::memset(ZPage16, 0, XRes * YRes * sizeof(word));

        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
        Lighting(sc);
        if (CAll) {
            Radix_Sort(FList, SList, CAll);
            Render(RenderPath::ForceDeferred);
        }

        char colorPath[1024];
        std::snprintf(colorPath, sizeof(colorPath),
                      "%s/lmtest_%s_subdiv%03d.ppm",
                      cfg.outDir.c_str(),
                      lmOn ? "lm" : "cube",
                      int(subdiv));
        write_ppm(colorPath, MainSurf->Data, xres, yres, MainSurf->BPSL);
        std::fprintf(stderr,
            "[LMTEST] subdiv=%-4d  ground=%d tris  -> %s\n",
            int(subdiv), int(subdiv) * int(subdiv) * 2, colorPath);
        ++produced;
    }

    ThreadPool::instance().close();
    return produced > 0 ? 0 : 5;
}
