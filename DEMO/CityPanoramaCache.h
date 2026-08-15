#pragma once

// On-disk cache for City's per-building env-map panoramas.
//
// City init renders 6 cube faces per "b*.lwo" building and reprojects to a
// panorama (PANORAMA_XRES x PANORAMA_YRES RGBA). That step dominates city
// init wall time (~4.4 s of a 4.8 s total, vs 0.4 s with --skip-cubemap).
// Output is deterministic given (CITY.FLD, panorama dims, building set),
// so we serialize the panoramas to disk keyed by an FNV-1a of those
// inputs. Subsequent runs with the same key skip the bake entirely.
//
// Cache file: Runtime/cache/city_envmap.bin (single file, self-validating
// header). Disable via --no-city-envmap-cache.

#include <cstdint>
#include <string>
#include <vector>

struct Object;
struct Scene;

namespace fds {

struct BuildingPanoramaEntry {
    Object*  obj;
    std::string name;            // copy of obj->Name for the cache record
    // Raw RGBA panorama BEFORE Sachletz tiling. The cache stores this
    // (not the post-Materialize Txtr->Data) because Materialize/Sachletz
    // is one-way: round-tripping through it would tile twice and scramble
    // the texture. Caller fills this from the bake or from the cache,
    // then runs Materialize once to install Obj->Reflection.
    std::vector<uint8_t> rawPanorama;
    // --env_live_water: per-TEXEL water VERDICT bitmap of this building's cube
    // bake — ONE BIT per baked texel at the face resolution (6 x
    // waterMaskRes²/8 bytes, EnvCube face order, face-major then row-major,
    // LSB first; the layout fds::EnvLiveWater_MaskBit indexes). Produced by
    // the fresh bake only and deliberately NOT part of the on-disk cache
    // record: env_live_water bypasses the cube cache anyway (the mode's bake
    // differs), and adding a plane to the format would invalidate every
    // existing ~450 MB artifact for a feature that never reads them.
    std::vector<uint8_t> waterMask;
    int                  waterMaskRes = 0;
};

// Compute the cache key from CITY.FLD bytes + panorama/cube dims +
// building name list. Returns 0 if the FLD file cannot be read.
// formatSalt distinguishes storage formats that share the same dims/names
// (e.g. env_cube's six padded faces vs the legacy equirect panorama). It is
// folded into the key ONLY when non-zero, so the legacy equirect call
// (formatSalt == 0) keeps its historical key and existing caches still load.
uint64_t ComputeCityPanoramaCacheKey(const char* fldPath,
                                     int panoramaX, int panoramaY,
                                     int cubeMapX, int cubeMapY,
                                     const std::vector<BuildingPanoramaEntry>& buildings,
                                     uint32_t formatSalt = 0);

// Try to fill each entry's rawPanorama from the disk cache. Returns true
// iff every requested building was found and loaded. On true return,
// the caller should Materialize each entry to install Obj->Reflection.
// cacheFile overrides the on-disk file (default = the equirect
// cache/city_envmap.bin). env_cube passes a SEPARATE file so the two formats
// never overwrite each other's single-record cache — a flag-off (equirect) run
// after a flag-on (cube) run keeps hitting its own intact cache.
bool TryLoadCityPanoramaCache(uint64_t key,
                               int panoramaX, int panoramaY,
                               std::vector<BuildingPanoramaEntry>& buildings,
                               const char* cacheFile = nullptr);

// Write every entry's rawPanorama to the cache file. Caller must have
// filled rawPanorama via the live bake first.
void WriteCityPanoramaCache(uint64_t key,
                             int panoramaX, int panoramaY,
                             const std::vector<BuildingPanoramaEntry>& buildings,
                             const char* cacheFile = nullptr);

}  // namespace fds
