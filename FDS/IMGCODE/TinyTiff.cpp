// Minimal baseline-TIFF decoder — see TinyTiff.h for the supported subset.
// Deflate strips inflate through stb_image's public zlib API (stb_image.h
// is compiled in IMGCODE.CPP; we only need the declarations here).

#include "TinyTiff.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// stb_image public zlib helper (declarations only — the implementation
// lives in IMGCODE.CPP's STB_IMAGE_IMPLEMENTATION block).
#include "stb_image.h"

namespace {

struct Reader {
	const unsigned char *p;
	size_t len;
	bool be;              // MM (big-endian) file
	uint16_t u16(size_t off) const {
		if (off + 2 > len) return 0;
		return be ? uint16_t((p[off] << 8) | p[off + 1])
		          : uint16_t(p[off] | (p[off + 1] << 8));
	}
	uint32_t u32(size_t off) const {
		if (off + 4 > len) return 0;
		return be ? (uint32_t(p[off]) << 24) | (uint32_t(p[off+1]) << 16) |
		            (uint32_t(p[off+2]) << 8) | p[off+3]
		          : uint32_t(p[off]) | (uint32_t(p[off+1]) << 8) |
		            (uint32_t(p[off+2]) << 16) | (uint32_t(p[off+3]) << 24);
	}
};

struct Tag {
	uint16_t id, type;
	uint32_t count, valOff;   // valOff = raw 4-byte field offset in file
	size_t   fieldOff;        // offset of the 12-byte entry
};

// Per-element size for TIFF types we care about.
int typeSize(uint16_t t) {
	switch (t) {
	case 1: case 2: case 6: case 7: return 1;   // BYTE/ASCII/SBYTE/UNDEF
	case 3: case 8: return 2;                   // SHORT
	case 4: case 9: case 11: return 4;          // LONG / FLOAT
	case 5: case 10: case 12: return 8;         // RATIONAL / DOUBLE
	}
	return 0;
}

// i-th integer value of a tag (SHORT or LONG or BYTE).
uint32_t tagVal(const Reader &r, const Tag &t, uint32_t i) {
	const int es = typeSize(t.type);
	if (!es || i >= t.count) return 0;
	const size_t total = size_t(es) * t.count;
	// Values ≤ 4 bytes live inline in the value field (offset fieldOff+8).
	const size_t base = (total <= 4) ? t.fieldOff + 8 : t.valOff;
	const size_t off = base + size_t(es) * i;
	switch (es) {
	case 1: return (off < r.len) ? r.p[off] : 0;
	case 2: return r.u16(off);
	default: return r.u32(off);
	}
}

// ── PackBits ────────────────────────────────────────────────────────────
bool unpackBits(const unsigned char *src, size_t n, unsigned char *dst, size_t outN) {
	size_t si = 0, di = 0;
	while (si < n && di < outN) {
		const signed char c = (signed char)src[si++];
		if (c >= 0) {
			const size_t run = size_t(c) + 1;
			if (si + run > n || di + run > outN) return false;
			memcpy(dst + di, src + si, run);
			si += run; di += run;
		} else if (c != -128) {
			const size_t run = size_t(-c) + 1;
			if (si >= n || di + run > outN) return false;
			memset(dst + di, src[si++], run);
			di += run;
		}
	}
	return di == outN;
}

// ── TIFF-variant LZW (MSB-first codes, early change) ────────────────────
bool unlzw(const unsigned char *src, size_t n, unsigned char *dst, size_t outN) {
	struct Entry { int16_t prev; unsigned char ch; };
	std::vector<Entry> table(4096);
	std::vector<unsigned char> stack;
	stack.reserve(4096);
	const int kClear = 256, kEoi = 257;
	int next = 258, width = 9;
	size_t bitPos = 0, di = 0;
	int prevCode = -1;

	auto readCode = [&]() -> int {
		if ((bitPos + width) > n * 8) return kEoi;
		uint32_t v = 0;
		for (int i = 0; i < width; ++i) {
			const size_t bp = bitPos + i;
			v = (v << 1) | ((src[bp >> 3] >> (7 - (bp & 7))) & 1);
		}
		bitPos += width;
		return int(v);
	};
	auto emit = [&](int code) -> bool {
		// Callers validate code <= next (the just-inserted entry INDEX equals
		// next in the code==next case, so the guard here is only the table
		// bound + cycle protection).
		stack.clear();
		while (code >= 256) {           // walk the chain (256/257 never stored)
			if (code >= 4096 || stack.size() > 4096) return false;
			stack.push_back(table[code].ch);
			code = table[code].prev;
		}
		stack.push_back((unsigned char)code);
		for (size_t i = stack.size(); i-- > 0; ) {
			if (di >= outN) return true;   // extra data: strip padding, fine
			dst[di++] = stack[i];
		}
		return true;
	};
	auto firstChar = [&](int code) -> unsigned char {
		while (code >= 256) code = table[code].prev;
		return (unsigned char)code;
	};

	for (;;) {
		int code = readCode();
		if (code == kEoi) break;
		if (code == kClear) {
			next = 258; width = 9; prevCode = -1;
			continue;
		}
		if (prevCode < 0) {
			if (code > 255) return false;
			if (!emit(code)) return false;
		} else {
			if (code < next) {
				if (!emit(code)) return false;
				table[next] = { int16_t(prevCode), firstChar(code) };
			} else if (code == next) {
				table[next] = { int16_t(prevCode), firstChar(prevCode) };
				if (!emit(code)) return false;
			} else {
				return false;
			}
			++next;
			// TIFF "early change": bump the width one code early.
			if (next == (1 << width) - 1 && width < 12) ++width;
		}
		prevCode = code;
		if (di >= outN) break;
	}
	return di == outN;
}

} // namespace

bool TinyTiff_Sniff(const unsigned char *buf, size_t len) {
	if (len < 8) return false;
	return (buf[0] == 'I' && buf[1] == 'I' && buf[2] == 42 && buf[3] == 0) ||
	       (buf[0] == 'M' && buf[1] == 'M' && buf[2] == 0 && buf[3] == 42);
}

unsigned char *TinyTiff_Decode(const unsigned char *buf, size_t len,
                               int *w, int *h) {
	if (!TinyTiff_Sniff(buf, len)) return nullptr;
	Reader r{ buf, len, buf[0] == 'M' };

	const uint32_t ifdOff = r.u32(4);
	if (!ifdOff || ifdOff + 2 > len) return nullptr;
	const uint16_t nTags = r.u16(ifdOff);

	Tag tags[32];
	int nt = 0;
	for (uint16_t i = 0; i < nTags && nt < 32; ++i) {
		const size_t e = ifdOff + 2 + size_t(i) * 12;
		if (e + 12 > len) break;
		Tag t;
		t.fieldOff = e;
		t.id     = r.u16(e);
		t.type   = r.u16(e + 2);
		t.count  = r.u32(e + 4);
		t.valOff = r.u32(e + 8);
		tags[nt++] = t;
	}
	auto find = [&](uint16_t id) -> const Tag * {
		for (int i = 0; i < nt; ++i) if (tags[i].id == id) return &tags[i];
		return nullptr;
	};
	auto intOr = [&](uint16_t id, uint32_t dflt) -> uint32_t {
		const Tag *t = find(id);
		return t ? tagVal(r, *t, 0) : dflt;
	};

	const uint32_t W       = intOr(256, 0);
	const uint32_t H       = intOr(257, 0);
	const uint32_t comp    = intOr(259, 1);
	const uint32_t photo   = intOr(262, 1);
	const uint32_t spp     = intOr(277, 1);
	const uint32_t rowsPer = intOr(278, H ? H : 1);
	const uint32_t planar  = intOr(284, 1);
	const uint32_t predict = intOr(317, 1);
	const uint32_t sfmt    = intOr(339, 1);
	const Tag *tOffs  = find(273);
	const Tag *tCnts  = find(279);
	const Tag *tBits  = find(258);
	const uint32_t bps = tBits ? tagVal(r, *tBits, 0) : 1;

	if (!W || !H || !tOffs || !tCnts) return nullptr;
	if (find(322) || find(323)) {
		fprintf(stderr, "[TIFF] tiled TIFF not supported (strips only)\n");
		return nullptr;
	}
	if (planar != 1 || spp < 1 || spp > 4 || photo > 2 ||
	    (bps != 8 && bps != 16 && bps != 32) ||
	    (bps == 32 && sfmt != 3)) {
		fprintf(stderr, "[TIFF] unsupported layout (planar=%u spp=%u photo=%u bps=%u sfmt=%u)\n",
		        planar, spp, photo, bps, sfmt);
		return nullptr;
	}
	if (comp != 1 && comp != 5 && comp != 8 && comp != 32946 && comp != 32773) {
		fprintf(stderr, "[TIFF] unsupported compression %u (have: none/LZW/deflate/packbits)\n", comp);
		return nullptr;
	}

	const size_t bytesPerSample = bps / 8;
	const size_t rowBytes = size_t(W) * spp * bytesPerSample;
	const uint32_t nStrips = tOffs->count;
	if (tCnts->count != nStrips) return nullptr;

	std::vector<unsigned char> raster(rowBytes * H);
	uint32_t row = 0;
	for (uint32_t s = 0; s < nStrips && row < H; ++s) {
		const uint32_t sOff = tagVal(r, *tOffs, s);
		const uint32_t sLen = tagVal(r, *tCnts, s);
		if (size_t(sOff) + sLen > len) return nullptr;
		const uint32_t stripRows = (rowsPer < H - row) ? rowsPer : (H - row);
		const size_t outN = rowBytes * stripRows;
		unsigned char *dst = raster.data() + rowBytes * row;

		if (comp == 1) {
			if (sLen < outN) return nullptr;
			memcpy(dst, buf + sOff, outN);
		} else if (comp == 32773) {
			if (!unpackBits(buf + sOff, sLen, dst, outN)) return nullptr;
		} else if (comp == 5) {
			if (!unlzw(buf + sOff, sLen, dst, outN)) return nullptr;
		} else {                        // 8 / 32946 = zlib deflate
			int outLen = 0;
			char *z = stbi_zlib_decode_malloc_guesssize_headerflag(
				(const char *)(buf + sOff), int(sLen), int(outN), &outLen, 1);
			if (!z || size_t(outLen) < outN) { free(z); return nullptr; }
			memcpy(dst, z, outN);
			free(z);
		}

		// Horizontal-differencing predictor: cumulative sum per row/sample.
		if (predict == 2) {
			for (uint32_t y = 0; y < stripRows; ++y) {
				unsigned char *rp = dst + rowBytes * y;
				if (bps == 8) {
					for (size_t i = spp; i < rowBytes; ++i) rp[i] = (unsigned char)(rp[i] + rp[i - spp]);
				} else if (bps == 16) {
					// 16-bit predictor operates on sample values in FILE byte order.
					for (size_t i = spp; i < size_t(W) * spp; ++i) {
						const size_t a = (i - spp) * 2, b = i * 2;
						uint16_t pv = r.be ? uint16_t((rp[a] << 8) | rp[a+1]) : uint16_t(rp[a] | (rp[a+1] << 8));
						uint16_t cv = r.be ? uint16_t((rp[b] << 8) | rp[b+1]) : uint16_t(rp[b] | (rp[b+1] << 8));
						cv = uint16_t(cv + pv);
						if (r.be) { rp[b] = (unsigned char)(cv >> 8); rp[b+1] = (unsigned char)cv; }
						else      { rp[b] = (unsigned char)cv; rp[b+1] = (unsigned char)(cv >> 8); }
					}
				}
			}
		}
		row += stripRows;
	}
	if (row < H) return nullptr;

	// Convert to RGBA8 (stb layout: R,G,B,A byte order).
	unsigned char *out = (unsigned char *)malloc(size_t(W) * H * 4);
	if (!out) return nullptr;
	for (size_t i = 0; i < size_t(W) * H; ++i) {
		const unsigned char *sp = raster.data() + i * spp * bytesPerSample;
		unsigned char v[4] = { 0, 0, 0, 255 };
		for (uint32_t c = 0; c < spp && c < 4; ++c) {
			const unsigned char *cp = sp + c * bytesPerSample;
			unsigned char b8;
			if (bps == 8) b8 = *cp;
			else if (bps == 16) b8 = r.be ? cp[0] : cp[1];   // high byte
			else {                                            // 32-bit float
				uint32_t u = r.be ? (uint32_t(cp[0]) << 24) | (uint32_t(cp[1]) << 16) |
				                    (uint32_t(cp[2]) << 8) | cp[3]
				                  : uint32_t(cp[0]) | (uint32_t(cp[1]) << 8) |
				                    (uint32_t(cp[2]) << 16) | (uint32_t(cp[3]) << 24);
				float f;
				memcpy(&f, &u, 4);
				if (f < 0.0f) f = 0.0f;
				if (f > 1.0f) f = 1.0f;
				b8 = (unsigned char)(f * 255.0f + 0.5f);
			}
			v[c] = b8;
		}
		unsigned char R, G, B, A = 255;
		if (spp <= 2) {                       // gray (+alpha)
			unsigned char g = v[0];
			if (photo == 0) g = (unsigned char)(255 - g);   // WhiteIsZero
			R = G = B = g;
			if (spp == 2) A = v[1];
		} else {
			R = v[0]; G = v[1]; B = v[2];
			if (spp == 4) A = v[3];
		}
		out[i*4+0] = R; out[i*4+1] = G; out[i*4+2] = B; out[i*4+3] = A;
	}
	*w = int(W);
	*h = int(H);
	fprintf(stderr, "[TIFF] decoded %ux%u spp=%u bps=%u comp=%u pred=%u\n",
	        W, H, spp, bps, comp, predict);
	return out;
}

unsigned char *TinyTiff_DecodeFile(const char *path, int *w, int *h) {
	FILE *f = fopen(path, "rb");
	if (!f) return nullptr;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) { fclose(f); return nullptr; }
	std::vector<unsigned char> buf(static_cast<size_t>(n));
	const size_t got = fread(buf.data(), 1, size_t(n), f);
	fclose(f);
	if (got != size_t(n)) return nullptr;
	return TinyTiff_Decode(buf.data(), got, w, h);
}
