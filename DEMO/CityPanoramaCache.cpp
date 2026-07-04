#include "CityPanoramaCache.h"

#include <Base/Object.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fds {
namespace {

constexpr char     kMagic[8]      = {'C','I','T','Y','E','N','V','M'};
// v2 stored post-Sachletz tiled bytes — re-running them through Materialize
// on load double-tiled the texture and scrambled reflections. v3 stores
// raw (pre-tile) RGBA and the caller Materializes once. Old v2 files
// automatically invalidate via the version mismatch in TryLoad.
constexpr uint32_t kFormatVersion = 3;
constexpr const char* kCacheDir   = "cache";
constexpr const char* kCachePath  = "cache/city_envmap.bin";

struct Header {
    char     magic[8];
    uint32_t version;
    uint32_t panoramaX;
    uint32_t panoramaY;
    uint32_t buildingCount;
    uint64_t key;
    uint32_t reserved[6];
};
static_assert(sizeof(Header) == 8 + 4*4 + 8 + 24, "Header layout drift");

uint64_t fnv1a(const void* data, size_t bytes, uint64_t seed = 0xcbf29ce484222325ULL)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = seed;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

}  // namespace

uint64_t ComputeCityPanoramaCacheKey(const char* fldPath,
                                     int panoramaX, int panoramaY,
                                     int cubeMapX, int cubeMapY,
                                     const std::vector<BuildingPanoramaEntry>& buildings,
                                     uint32_t formatSalt)
{
    std::ifstream f(fldPath, std::ios::binary);
    if (!f) return 0;
    // Hash file contents in 64 KB blocks. FLD is a few MB at most, so this
    // is well under a millisecond.
    std::vector<char> buf(64 * 1024);
    uint64_t h = 0xcbf29ce484222325ULL;
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        const std::streamsize got = f.gcount();
        if (got <= 0) break;
        h = fnv1a(buf.data(), size_t(got), h);
    }
    // Fold in the dims so a panorama-size change invalidates the cache.
    const uint32_t dims[4] = { uint32_t(panoramaX), uint32_t(panoramaY),
                                uint32_t(cubeMapX),  uint32_t(cubeMapY) };
    h = fnv1a(dims, sizeof(dims), h);
    // Fold in the storage-format salt ONLY when set (env_cube). Zero (the
    // legacy equirect call) leaves the historical key untouched.
    if (formatSalt) h = fnv1a(&formatSalt, sizeof(formatSalt), h);
    // Fold in the building names — adding/removing/renaming buildings
    // shifts panorama indices and must invalidate.
    for (const auto& b : buildings) {
        h = fnv1a(b.name.data(), b.name.size(), h);
        const char sep = '\0';
        h = fnv1a(&sep, 1, h);
    }
    return h;
}

bool TryLoadCityPanoramaCache(uint64_t key,
                               int panoramaX, int panoramaY,
                               std::vector<BuildingPanoramaEntry>& buildings,
                               const char* cacheFile)
{
    const char* path = cacheFile ? cacheFile : kCachePath;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    Header hdr{};
    f.read((char*)&hdr, sizeof(hdr));
    if (!f) return false;
    if (std::memcmp(hdr.magic, kMagic, 8) != 0) return false;
    if (hdr.version != kFormatVersion) return false;
    if (hdr.key != key) return false;
    if (int(hdr.panoramaX) != panoramaX || int(hdr.panoramaY) != panoramaY) return false;
    if (hdr.buildingCount != buildings.size()) return false;

    const size_t panoramaBytes = size_t(panoramaX) * size_t(panoramaY) * 4;

    for (size_t i = 0; i < buildings.size(); ++i) {
        // Per-entry: name length (u16), name bytes, raw panorama bytes.
        uint16_t nameLen = 0;
        f.read((char*)&nameLen, sizeof(nameLen));
        if (!f) return false;
        std::string name(nameLen, '\0');
        f.read(name.data(), nameLen);
        if (!f) return false;
        if (name != buildings[i].name) return false;
        buildings[i].rawPanorama.resize(panoramaBytes);
        f.read((char*)buildings[i].rawPanorama.data(), std::streamsize(panoramaBytes));
        if (!f) {
            // Wipe partial fills so the caller doesn't see junk.
            for (auto &b : buildings) b.rawPanorama.clear();
            return false;
        }
    }

    std::fprintf(stderr,
        "[CITY-CACHE] loaded %zu building panoramas from %s\n",
        buildings.size(), path);
    return true;
}

void WriteCityPanoramaCache(uint64_t key,
                             int panoramaX, int panoramaY,
                             const std::vector<BuildingPanoramaEntry>& buildings,
                             const char* cacheFile)
{
    const char* path = cacheFile ? cacheFile : kCachePath;
    std::error_code ec;
    std::filesystem::create_directories(kCacheDir, ec);
    // Best-effort: if we can't create the dir, fall through to fstream
    // open which will fail cleanly.

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr,
            "[CITY-CACHE] WARN: cannot open %s for write — cache disabled this run\n",
            path);
        return;
    }

    Header hdr{};
    std::memcpy(hdr.magic, kMagic, 8);
    hdr.version       = kFormatVersion;
    hdr.panoramaX     = uint32_t(panoramaX);
    hdr.panoramaY     = uint32_t(panoramaY);
    hdr.buildingCount = uint32_t(buildings.size());
    hdr.key           = key;
    f.write((const char*)&hdr, sizeof(hdr));

    const size_t panoramaBytes = size_t(panoramaX) * size_t(panoramaY) * 4;
    for (const auto& b : buildings) {
        const uint16_t nameLen = uint16_t(b.name.size());
        f.write((const char*)&nameLen, sizeof(nameLen));
        f.write(b.name.data(), nameLen);
        if (b.rawPanorama.size() != panoramaBytes) {
            std::fprintf(stderr,
                "[CITY-CACHE] WARN: building '%s' has rawPanorama=%zu bytes (expected %zu); cache aborted\n",
                b.name.c_str(), b.rawPanorama.size(), panoramaBytes);
            return;
        }
        f.write((const char*)b.rawPanorama.data(), std::streamsize(panoramaBytes));
    }
    if (!f) {
        std::fprintf(stderr,
            "[CITY-CACHE] WARN: write to %s failed mid-stream\n", path);
        return;
    }
    std::fprintf(stderr,
        "[CITY-CACHE] wrote %zu building panoramas to %s (%.1f MiB)\n",
        buildings.size(), path,
        double(sizeof(Header) + buildings.size() * (panoramaBytes + 8)) / (1024.0 * 1024.0));
}

}  // namespace fds
