#include "Snapshot.h"

#include "CITY.H"
#include "Rev.h"
#include "Scenes.h"
#include "SceneTick.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Threads.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/stat.h>

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
    if (cfg.scene != "city") {
        std::fprintf(stderr, "[SNAPSHOT] only --snapshot=city is implemented (got '%s')\n",
                     cfg.scene.c_str());
        return 2;
    }

    // mkdir -p outDir (best effort).
    if (!cfg.outDir.empty() && cfg.outDir != ".") {
#ifdef _WIN32
        _mkdir(cfg.outDir.c_str());
#else
        mkdir(cfg.outDir.c_str(), 0755);
#endif
    }

    if (!FDS_Init(static_cast<unsigned short>(xres),
                  static_cast<unsigned short>(yres), 32)) {
        std::fprintf(stderr, "[SNAPSHOT] FDS_Init failed\n");
        return 3;
    }

    // Allocate a real framebuffer + Z-buffer surface so the rasterizer has
    // somewhere to write. No window, no SDL — the Flip callback is a no-op.
    VESA_Surface surf = {};
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
        return 4;
    }
    std::memset(surf.Data, 0, surf.PageSize + zSize);
    surf.Flip = &noop_flip;

    VESA_VPageExternal(&surf);
    VESA_Surface2Global(MainSurf);

    // Match the production init sequence used by CodeEntry, minus the audio
    // / scene-sequence / SDL bits. ThreadPool is required because Render()
    // dispatches tile work onto its workers.
    Generate_RGBFlares();
    InitPolyStats(200);
    ThreadPool::instance().init([]() {
        InitPolyStats(200);
        FPU_LPrecision();
    });
    FPU_LPrecision();

    // Suppress profiler overlay so it doesn't appear in the dumped frame.
    g_profilerActive = 0;

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
