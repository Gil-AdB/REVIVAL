#include "Snapshot.h"

#include "CITY.H"
#include "FillerTest.h"
#include "GLAT.H"
#include "Rev.h"
#include "Scenes.h"
#include "SceneTick.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <FILLERS/Mekalele.h>
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
#ifdef _WIN32
    _mkdir(outDir.c_str());
#else
    mkdir(outDir.c_str(), 0755);
#endif
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
            Transform_Objects(CurScene);
            std::fprintf(stderr, "[FNTSNAP] post-Transform CAll=%d CurScene=%p\n",
                int(CAll), (void*)CurScene);
            if (CAll) {
                Radix_SortingASM(FList, SList, CAll);
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
        Transform_Objects(sc);
        if (CAll) {
            Radix_SortingASM(FList, SList, CAll);
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
    static std::unique_ptr<Face*[]> fListStorage =
        std::make_unique<Face*[]>(64);
    static std::unique_ptr<Face*[]> sListStorage =
        std::make_unique<Face*[]>(64);
    FList = fListStorage.get();
    SList = sListStorage.get();

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
        Transform_Objects(sc);
        if (CAll) {
            Radix_SortingASM(FList, SList, CAll);
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
            Transform_Objects(sc);
            if (CAll) {
                Radix_SortingASM(FList, SList, CAll);
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
        Transform_Objects(sc);
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
            Radix_SortingASM(FList, SList, CAll);
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
    auto t0 = clock::now();
    for (int i = 0; i < cfg.iters; ++i) {
        std::srand(0);
        Timer = cfg.ts;
        driver->tick();
    }
    auto t1 = clock::now();

    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double mean_ms = cfg.iters > 0 ? total_ms / cfg.iters : 0.0;

    std::fprintf(stderr,
                 "[BENCH] scene=%s t=%d iters=%d total=%.2f ms  mean=%.3f ms/iter\n",
                 cfg.scene.c_str(), cfg.ts, cfg.iters, total_ms, mean_ms);
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
