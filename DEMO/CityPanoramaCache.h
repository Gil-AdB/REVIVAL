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
    std::string name;        // copy of obj->Name for the cache record
};

// Compute the cache key from CITY.FLD bytes + panorama/cube dims +
// building name list. Returns 0 if the FLD file cannot be read.
uint64_t ComputeCityPanoramaCacheKey(const char* fldPath,
                                     int panoramaX, int panoramaY,
                                     int cubeMapX, int cubeMapY,
                                     const std::vector<BuildingPanoramaEntry>& buildings);

// Try to populate each entry's Obj->Reflection from the disk cache.
// Returns true iff every requested building was found and loaded.
// On false return the caller must run the live bake.
bool TryLoadCityPanoramaCache(uint64_t key,
                               int panoramaX, int panoramaY,
                               const std::vector<BuildingPanoramaEntry>& buildings);

// Write the panorama for every entry to the cache file. Caller must have
// already populated Obj->Reflection->Txtr->Data via the live bake.
void WriteCityPanoramaCache(uint64_t key,
                             int panoramaX, int panoramaY,
                             const std::vector<BuildingPanoramaEntry>& buildings);

}  // namespace fds
