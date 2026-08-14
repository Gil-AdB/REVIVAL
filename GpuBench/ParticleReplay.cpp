// ParticleReplay — reader + synthetic writer for the fountain particle dump.
// The format, the rationale and the DEMO-side writer this is built against are
// all specified in ParticleReplay.h. Nothing here touches DEMO or FDS.

#include "ParticleReplay.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace gpubench {

namespace {

constexpr char     kMagic[8] = {'F', 'D', 'S', 'P', 'C', 'L', '1', '\0'};
constexpr uint32_t kFrameMagic = 0x4D524650u;   // 'PFRM'
constexpr uint32_t kVersion = 1u;

template <typename T>
bool rd(FILE *f, T &v) { return std::fread(&v, sizeof(T), 1, f) == 1; }
template <typename T>
bool wr(FILE *f, const T &v) { return std::fwrite(&v, sizeof(T), 1, f) == 1; }

}  // namespace

int PclDump::nearestByCurFrame(float want) const {
    if (frames.empty()) return -1;
    int best = 0;
    float bestD = std::fabs(frames[0].curFrame - want);
    for (size_t i = 1; i < frames.size(); ++i) {
        const float d = std::fabs(frames[i].curFrame - want);
        if (d < bestD) { bestD = d; best = int(i); }
    }
    return best;
}

bool PclLoad(const std::string &path, PclDump &out, bool verbose) {
    out.frames.clear();
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[PCL] cannot open %s\n", path.c_str());
        return false;
    }
    char magic[8] = {};
    uint32_t version = 0, frameCount = 0, reserved[2] = {};
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, kMagic, 8) != 0) {
        std::fprintf(stderr, "[PCL] %s is not a particle dump (bad magic)\n", path.c_str());
        std::fclose(f); return false;
    }
    if (!rd(f, version) || !rd(f, frameCount) || !rd(f, reserved[0]) || !rd(f, reserved[1])) {
        std::fprintf(stderr, "[PCL] %s: truncated header\n", path.c_str());
        std::fclose(f); return false;
    }
    if (version != kVersion) {
        std::fprintf(stderr, "[PCL] %s: version %u, this build reads %u\n",
                     path.c_str(), version, kVersion);
        std::fclose(f); return false;
    }
    out.frames.reserve(frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        uint32_t fm = 0, count = 0, rsv = 0;
        PclFrame fr;
        if (!rd(f, fm) || fm != kFrameMagic) {
            // A resync marker that does not resync is a TRUNCATED or corrupt
            // dump. Reported with the frame index, never silently accepted:
            // a replay that quietly renders a fraction of the spray reads as
            // "the GPU is too dark", which is the exact mistake this arm has
            // already paid for twice.
            std::fprintf(stderr, "[PCL] %s: frame %u/%u has no PFRM marker — "
                                 "truncated or corrupt dump\n", path.c_str(), i, frameCount);
            std::fclose(f); return false;
        }
        if (!rd(f, fr.timer) || !rd(f, fr.curFrame) || !rd(f, fr.imageSize) ||
            !rd(f, fr.perspX) || !rd(f, count) || !rd(f, rsv)) {
            std::fprintf(stderr, "[PCL] %s: frame %u header truncated\n", path.c_str(), i);
            std::fclose(f); return false;
        }
        fr.particles.resize(count);
        for (uint32_t p = 0; p < count; ++p) {
            PclParticle &q = fr.particles[p];
            if (std::fread(q.pos, sizeof(float), 3, f) != 3 || !rd(f, q.flareSize) ||
                std::fread(q.rgb, 1, 3, f) != 3 || !rd(f, q.tex)) {
                std::fprintf(stderr, "[PCL] %s: frame %u truncated at particle %u/%u\n",
                             path.c_str(), i, p, count);
                std::fclose(f); return false;
            }
        }
        out.frames.push_back(std::move(fr));
    }
    std::fclose(f);
    if (verbose && !out.frames.empty()) {
        size_t total = 0;
        for (const auto &fr : out.frames) total += fr.particles.size();
        std::fprintf(stderr,
            "[PCL] %s: %zu frame(s), curFrame %.1f..%.1f, %zu particle record(s), "
            "ImageSize %.3f\n",
            path.c_str(), out.frames.size(), out.frames.front().curFrame,
            out.frames.back().curFrame, total, out.frames.front().imageSize);
    }
    return true;
}

bool PclWriteSynthetic(const std::string &path, int n, const float centre[3],
                       float curFrame, float imageSize) {
    if (n < 0) n = 0;
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "[PCL] cannot write %s\n", path.c_str()); return false; }
    std::fwrite(kMagic, 1, 8, f);
    const uint32_t frameCount = 1, rsv = 0;
    wr(f, kVersion); wr(f, frameCount); wr(f, rsv); wr(f, rsv);
    wr(f, kFrameMagic);
    const float timer = 2500.0f;
    wr(f, timer); wr(f, curFrame); wr(f, imageSize);
    const float perspX = 0.0f;
    wr(f, perspX);
    const uint32_t count = uint32_t(n);
    wr(f, count); wr(f, rsv);
    // A ballistic fan: deterministic (a fixed LCG), so the synthetic file is
    // reproducible and a replay regression is a byte comparison.
    uint32_t s = 0x1234567u;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return float(s >> 8) / float(1 << 24); };
    for (int i = 0; i < n; ++i) {
        const float a = rnd() * 6.2831853f;
        const float sp = 20.0f + rnd() * 45.0f;      // horizontal reach
        const float t = rnd();                        // flight fraction
        const float px = centre[0] + std::cos(a) * sp * t;
        const float pz = centre[2] + std::sin(a) * sp * t;
        const float py = centre[1] + 60.0f * t - 70.0f * t * t;
        PclParticle q{};
        q.pos[0] = px; q.pos[1] = py; q.pos[2] = pz;
        q.flareSize = 0.06f * 20.0f / 10.0f;          // FOUNTAIN.CPP:358
        const float fade = 1.0f - t * 0.6f;
        q.rgb[0] = uint8_t(150.0f * fade);
        q.rgb[1] = uint8_t(190.0f * fade);
        q.rgb[2] = uint8_t(255.0f * fade);
        q.tex = uint8_t(i & 1);
        std::fwrite(q.pos, sizeof(float), 3, f);
        wr(f, q.flareSize);
        std::fwrite(q.rgb, 1, 3, f);
        wr(f, q.tex);
    }
    std::fclose(f);
    std::fprintf(stderr, "[PCL] wrote synthetic dump %s: 1 frame, %d particle(s), "
                         "curFrame %.1f, ImageSize %.3f\n",
                 path.c_str(), n, curFrame, imageSize);
    return true;
}

void PclBuildSpriteTexture(std::vector<uint8_t> &rgba, int &w, int &h) {
    // FOUNTAIN.CPP:310-330, term for term: a radial falloff disc, greyscale,
    // zero outside the unit circle. Both PclT materials generate this same
    // image (the loop body does not depend on I), so one texture covers both.
    w = h = 32;
    rgba.assign(size_t(w) * size_t(h) * 4, 0);
    const float dx = 2.0f / 32.0f, dy = -2.0f / 32.0f;
    float fx = -1.0f;
    for (int x = 0; x < 32; ++x, fx += dx) {
        float fy = 1.0f;
        for (int y = 0; y < 32; ++y, fy += dy) {
            const float r2 = fx * fx + fy * fy;
            const uint8_t v = (r2 >= 1.0f) ? 0 : uint8_t((1.0f - r2) * 255.0f);
            uint8_t *p = &rgba[(size_t(y) * 32 + size_t(x)) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
}

}  // namespace gpubench
