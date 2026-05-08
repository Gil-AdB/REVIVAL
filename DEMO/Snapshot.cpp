#include "Snapshot.h"

#include "CITY.H"
#include "FillerTest.h"
#include "GLAT.H"
#include "Rev.h"
#include "Scenes.h"
#include "SceneTick.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Threads.h>

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
    surf.Data = static_cast<byte*>(std::malloc(surf.PageSize + zSize));
    if (!surf.Data) {
        std::fprintf(stderr, "[SNAPSHOT] malloc framebuffer failed\n");
        return false;
    }
    std::memset(surf.Data, 0, surf.PageSize + zSize);
    surf.Flip = &noop_flip;

    VESA_VPageExternal(&surf);
    VESA_Surface2Global(MainSurf);

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
                    reinterpret_cast<const word*>(MainSurf->Data + MainSurf->PageSize),
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
                    reinterpret_cast<const word*>(MainSurf->Data + MainSurf->PageSize),
                    xres, yres);
        ++produced;
    }

    FillerTestSnapshotCleanup();
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

    driver->cleanup();
    driver.reset();

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
