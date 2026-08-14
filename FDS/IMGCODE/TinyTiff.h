#ifndef FDS_TINY_TIFF_H_INCLUDED
#define FDS_TINY_TIFF_H_INCLUDED

#include <cstddef>

// Minimal baseline-TIFF decoder for PBR texture packs (TextureCom /
// ambientCG ship .tif; stb_image has no TIFF support). Covers what those
// packs actually use: stripped (not tiled) images, PlanarConfig=1
// (chunky), 8/16-bit unsigned or 32-bit float samples, 1-4 samples per
// pixel, Photometric 0/1/2, Compression none(1) / LZW(5) / Deflate
// (8, 32946) / PackBits(32773), horizontal-differencing Predictor(2),
// both byte orders. Anything else returns null and the caller reports
// the normal load failure.
//
// Returns a malloc'd RGBA8 buffer (same layout stbi_load(...,4) gives —
// byte order R,G,B,A) or nullptr; caller frees with free(). 16-bit
// samples take the high byte; float samples clamp to [0,1]*255; gray
// replicates to RGB; missing alpha = 255.
unsigned char *TinyTiff_Decode(const unsigned char *buf, size_t len,
                               int *w, int *h);

// Whole-file convenience wrapper (nullptr if the file can't be read or
// isn't a decodable TIFF).
unsigned char *TinyTiff_DecodeFile(const char *path, int *w, int *h);

// Cheap magic-bytes check ("II*\0" / "MM\0*").
bool TinyTiff_Sniff(const unsigned char *buf, size_t len);

#endif // FDS_TINY_TIFF_H_INCLUDED
