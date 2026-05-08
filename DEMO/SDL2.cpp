#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include "SDL2.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include "../Modplayer/Modplayer.h"
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

void SDLTexDeleter::operator()(SDL_Texture *t) const noexcept {
    if (!t) return;
    fprintf(stderr, "[SDL] -tex tag=%s ptr=%p\n", tag ? tag : "?", (void *)t);
    SDL_DestroyTexture(t);
}

SDLTex SDL2_MakeTexture(SDL_Renderer *r, int X, int Y, const char *tag) {
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING, X, Y);
    fprintf(stderr, "[SDL] +tex tag=%s ptr=%p size=%dx%d%s\n",
            tag ? tag : "?", (void *)t, X, Y,
            t ? "" : " (FAILED)");
    return SDLTex(t, SDLTexDeleter{tag});
}


static VESA_Surface SDL_MainSurf;
static SDL_Window *sdl_window;
// Owns the engine display texture. Reset (-> destroy) and reassigned
// (-> create) on every resize. SDL_MainSurf.Handle is just s_engineTex.get().
static SDLTex s_engineTex;

// Per-stage FLIP profiler. On wasm we're spending most of a frame in the
// SDL present path; this accumulates ms across N frames and dumps to
// stderr so we can see exactly which stage is the bottleneck. Gated on
// g_profilerActive (the same flag that turns on the on-screen overlay).
// Throwaway instrumentation — remove once the wasm FLIP rewrite lands.
namespace {
struct FlipStageProfiler {
	using clock = std::chrono::steady_clock;
	using ms_d  = std::chrono::duration<double, std::milli>;
	double unlockMs = 0, barsMs = 0, copyMs = 0, presentMs = 0, lockMs = 0;
	int frames = 0;
	static constexpr int kDumpEvery = 60;
	void tick(double u, double b, double c, double p, double l) {
		unlockMs += u; barsMs += b; copyMs += c; presentMs += p; lockMs += l;
		if (++frames < kDumpEvery) return;
		double d = (double)frames;
		fprintf(stderr,
			"[FLIP] frames=%d  unlock=%.3f  bars=%.3f  copy=%.3f  present=%.3f  lock=%.3f  total=%.3f ms\n",
			frames, unlockMs/d, barsMs/d, copyMs/d, presentMs/d, lockMs/d,
			(unlockMs+barsMs+copyMs+presentMs+lockMs)/d);
		unlockMs = barsMs = copyMs = presentMs = lockMs = 0;
		frames = 0;
	}
};
}

extern "C" dword g_profilerActive;

#ifdef __EMSCRIPTEN__
// One-time WebGL2 setup on the SDL canvas. Must run before any other
// rendering context is requested on it (canvas allows only one type at a
// time). On wasm we skip SDL_CreateRenderer entirely so the canvas is
// free for WebGL2 here.
//
// The fragment shader handles two engine quirks:
//   1. The framebuffer is laid out as ARGB8888 packed = bytes BGRA in LE.
//      WebGL uploaded as gl.RGBA reads those bytes as R=B, G=G, B=R, A=A.
//      We swizzle .bgra to undo it.
//   2. The rasterizer never writes alpha (always 0), so canvas would be
//      fully transparent. We force alpha = 1.0 in the shader.
static int Wasm_InitGL()
{
	// Sync proxy to the browser main thread (where the canvas lives).
	// The OffscreenCanvas-to-worker route deadlocks against main's
	// SDL_WaitEvent loop, so we stay on the browser main thread for
	// the WebGL work. Cost: ~5-7 ms cross-thread proxy per flip.
	return MAIN_THREAD_EM_ASM_INT({
		var canvas = Module.canvas;
		if (!canvas) return 1;
		var gl = canvas.getContext('webgl2', {
			alpha: false,
			antialias: false,
			depth: false,
			stencil: false,
			premultipliedAlpha: false,
			preserveDrawingBuffer: false,
			powerPreference: 'high-performance'
		});
		if (!gl) return 2;
		Module.__floodGL = gl;

		var vsSrc =
			'#version 300 es\n' +
			'out vec2 vUV;\n' +
			'void main() {\n' +
			'  vec2 corners[4];\n' +
			'  corners[0] = vec2(-1.0, -1.0);\n' +
			'  corners[1] = vec2( 1.0, -1.0);\n' +
			'  corners[2] = vec2(-1.0,  1.0);\n' +
			'  corners[3] = vec2( 1.0,  1.0);\n' +
			'  vec2 uvs[4];\n' +
			'  uvs[0] = vec2(0.0, 1.0);\n' +
			'  uvs[1] = vec2(1.0, 1.0);\n' +
			'  uvs[2] = vec2(0.0, 0.0);\n' +
			'  uvs[3] = vec2(1.0, 0.0);\n' +
			'  vUV = uvs[gl_VertexID];\n' +
			'  gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);\n' +
			'}';
		var fsSrc =
			'#version 300 es\n' +
			'precision highp float;\n' +
			'in vec2 vUV;\n' +
			'uniform sampler2D uTex;\n' +
			'out vec4 oColor;\n' +
			'void main() {\n' +
			'  vec4 c = texture(uTex, vUV);\n' +
			'  oColor = vec4(c.b, c.g, c.r, 1.0);\n' +
			'}';
		function compile(type, src) {
			var s = gl.createShader(type);
			gl.shaderSource(s, src);
			gl.compileShader(s);
			if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
				console.error('flood gl shader: ' + gl.getShaderInfoLog(s));
			}
			return s;
		}
		var prog = gl.createProgram();
		gl.attachShader(prog, compile(gl.VERTEX_SHADER, vsSrc));
		gl.attachShader(prog, compile(gl.FRAGMENT_SHADER, fsSrc));
		gl.linkProgram(prog);
		if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
			console.error('flood gl link: ' + gl.getProgramInfoLog(prog));
		}
		Module.__floodProg = prog;
		Module.__floodTex = gl.createTexture();
		Module.__floodTexW = 0;
		Module.__floodTexH = 0;
		// VAO with a dummy attribute at location 0 — the vertex shader
		// synthesizes the quad from gl_VertexID and uses no attributes,
		// but desktop GL drivers (Mac in particular) fall into a slow
		// emulation path if attribute 0 is unbound. A 4-byte buffer is
		// enough to satisfy them; the shader never reads from it.
		Module.__floodVAO = gl.createVertexArray();
		gl.bindVertexArray(Module.__floodVAO);
		var dummyVBO = gl.createBuffer();
		gl.bindBuffer(gl.ARRAY_BUFFER, dummyVBO);
		gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0]), gl.STATIC_DRAW);
		gl.enableVertexAttribArray(0);
		gl.vertexAttribPointer(0, 1, gl.FLOAT, false, 0, 0);
		gl.bindVertexArray(null);
		gl.bindTexture(gl.TEXTURE_2D, Module.__floodTex);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
		gl.disable(gl.DEPTH_TEST);
		return 0;
	});
}

// Upload `pixels` (srcW x srcH, BGRA byte order) into the WebGL texture
// and present a fullscreen quad letterboxed inside the canvas.
static void Wasm_PresentGL(const uint8_t *pixels, int srcW, int srcH)
{
	static bool s_initialized = false;
	static bool s_initFailed  = false;
	if (!s_initialized && !s_initFailed) {
		int rc = Wasm_InitGL();
		if (rc != 0) {
			fprintf(stderr, "[SDL] Wasm_InitGL failed rc=%d\n", rc);
			s_initFailed = true;
			return;
		}
		s_initialized = true;
	}
	if (s_initFailed) return;

	MAIN_THREAD_EM_ASM({
		var gl = Module.__floodGL;
		if (!gl) return;
		var canvas = Module.canvas;
		var cw = canvas.width;
		var ch = canvas.height;
		gl.bindTexture(gl.TEXTURE_2D, Module.__floodTex);
		if (Module.__floodTexW !== $1 || Module.__floodTexH !== $2) {
			gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, $1, $2, 0,
			              gl.RGBA, gl.UNSIGNED_BYTE, null);
			Module.__floodTexW = $1;
			Module.__floodTexH = $2;
		}
		// WebGL2 texSubImage2D accepts SAB-backed Uint8Array views directly,
		// so this is a single GPU upload — no intermediate JS-side memcpy.
		var view = new Uint8Array(HEAPU8.buffer, $0, $1 * $2 * 4);
		gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, $1, $2,
		                 gl.RGBA, gl.UNSIGNED_BYTE, view);

		// Letterbox inside the canvas — preserve src aspect ratio. One var
		// per line: the C preprocessor splits MAIN_THREAD_EM_ASM body args
		// on top-level commas (parens protect, braces don't), so multi-var
		// declarations get tokenized as separate macro arguments.
		var srcAR = $1 / $2;
		var canAR = cw / ch;
		var dx = 0;
		var dy = 0;
		var dw = cw;
		var dh = ch;
		if (canAR > srcAR) {
			dw = (ch * srcAR) | 0;
			dx = (cw - dw) >> 1;
		} else {
			dh = (cw / srcAR) | 0;
			dy = (ch - dh) >> 1;
		}
		gl.viewport(0, 0, cw, ch);
		gl.clearColor(0, 0, 0, 1);
		gl.clear(gl.COLOR_BUFFER_BIT);
		gl.viewport(dx, dy, dw, dh);
		gl.useProgram(Module.__floodProg);
		gl.bindVertexArray(Module.__floodVAO);
		gl.activeTexture(gl.TEXTURE0);
		gl.bindTexture(gl.TEXTURE_2D, Module.__floodTex);
		gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
	}, (uintptr_t)pixels, srcW, srcH);
}
#endif

static void V_Flip(VESA_Surface *VS)
{
	using clock = std::chrono::steady_clock;
	using ms_d  = std::chrono::duration<double, std::milli>;
	static FlipStageProfiler s_flipProf;
	const bool profileFlip = g_profilerActive != 0;
	auto stamp = [&]() { return profileFlip ? clock::now() : clock::time_point{}; };

	// Top-right resolution overlay. Draw into the surface's data buffer
	// just before pushing to the SDL_Texture so it shows up regardless of
	// which scene's surface is being flipped (MainSurf vs Glat's FinalSurf).
	if (VS->Data && VS->X > 0 && VS->Y > 0) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%dx%d", (int)VS->X, (int)VS->Y);
		// Approximate per-char width is ~10 px at the native font; scales
		// with g_fontScale (HiDPI auto-doubles glyph dimensions).
		const int scale = g_fontScale > 0 ? g_fontScale : 1;
		int textPx = (int)strlen(buf) * 10 * scale;
		int x = (int)VS->X - textPx - 8 * scale;
		if (x < 0) x = 0;
		OutTextXY(VS->Data, x, 4 * scale, buf, 255, (int)VS->X, (int)VS->Y);
	}
	SDL_Renderer *renderer = static_cast<SDL_Renderer*>(VS->Renderer);
	SDL_Texture  *texture  = static_cast<SDL_Texture*>(VS->Handle);
	const bool lockMode = (VS->Flags & VSurf_LockRender) != 0;

	auto t0 = stamp();
#ifdef __EMSCRIPTEN__
	// Wasm path: SDL renderer is never created on this build, so we can't
	// (and don't need to) touch SDL_RenderCopy / SDL_RenderPresent. The
	// pixel buffer at VS->Data is a plain malloc; ship it to WebGL2 via
	// texSubImage2D + a fullscreen quad. Handles both lockMode (engine
	// surface) and non-lockMode (Glat's FinalSurf) — same upload, same
	// shader, just different src dims.
	(void)lockMode;
	Wasm_PresentGL(VS->Data, (int)VS->X, (int)VS->Y);
	if (profileFlip) {
		auto tEnd = stamp();
		s_flipProf.tick(0.0, 0.0, 0.0, ms_d(tEnd - t0).count(), 0.0);
	}
	return;
#endif
	if (lockMode) {
		// Engine wrote directly into the texture's locked pixel buffer
		// this frame. Unlock to commit (no-op for the SW renderer
		// beyond an internal flag), then RenderCopy/Present.
		SDL_UnlockTexture(texture);
		VS->Data = nullptr;
	} else {
		// Legacy path used by Glat's FinalSurf: VS->Data is a separate
		// malloc (FinalPage) that we copy into the texture each frame.
		// Engine surfaces use lockMode and skip this memcpy.
		SDL_UpdateTexture(texture, NULL, VS->Data, VS->BPSL);
	}
	auto tUnlock = stamp();

	// Letterbox: preserve the surface's aspect ratio inside the window.
	// The source is VS->X * VS->Y (e.g. Glat snaps to /8 multiples while
	// the window may not), the destination is the renderer's pixel size.
	// Scale uniformly to fit, center, fill remainder with black. When the
	// ARs match (true mid-flight resize where MainSurf == window), this
	// reduces to a full-window blit with no bars.
	int rw = 0, rh = 0;
	SDL_GetRendererOutputSize(renderer, &rw, &rh);
	SDL_Rect dst{0, 0, rw, rh};
	bool useFullDst = (rw <= 0 || rh <= 0 || VS->X <= 0 || VS->Y <= 0);
	if (!useFullDst) {
		float sx = (float)rw / (float)VS->X;
		float sy = (float)rh / (float)VS->Y;
		float s  = sx < sy ? sx : sy;
		dst.w = (int)((float)VS->X * s);
		dst.h = (int)((float)VS->Y * s);
		dst.x = (rw - dst.w) / 2;
		dst.y = (rh - dst.h) / 2;

		// Only fill the letterbox bar regions — clearing the whole
		// renderer output is a 20+ MB write per frame on wasm software
		// renderer, which dwarfs everything else in V_Flip when the
		// bars are tiny. Pixels under the engine area are about to be
		// overwritten by RenderCopy anyway.
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
		SDL_Rect bars[4];
		int n = 0;
		if (dst.y > 0) {                    // top
			bars[n++] = SDL_Rect{0, 0, rw, dst.y};
		}
		if (dst.y + dst.h < rh) {           // bottom
			bars[n++] = SDL_Rect{0, dst.y + dst.h, rw, rh - (dst.y + dst.h)};
		}
		if (dst.x > 0) {                    // left
			bars[n++] = SDL_Rect{0, dst.y, dst.x, dst.h};
		}
		if (dst.x + dst.w < rw) {           // right
			bars[n++] = SDL_Rect{dst.x + dst.w, dst.y, rw - (dst.x + dst.w), dst.h};
		}
		if (n > 0) SDL_RenderFillRects(renderer, bars, n);
	}
	auto tBars = stamp();
	SDL_RenderCopy(renderer, texture, NULL, useFullDst ? NULL : &dst);
	auto tCopy = stamp();
	SDL_RenderPresent(renderer);
	auto tPresent = stamp();

	if (lockMode) {
		// Re-lock for the next frame's writes. Update VS->Data + engine
		// globals so the next tick renders into the new lock pointer
		// (which can in principle differ each frame, though in practice
		// for SW renderer it's the same surface->pixels). Only do this
		// for engine surfaces — Glat's FinalSurf has its own malloc'd
		// FinalPage and shouldn't have its globals touched here.
		void *pixels = nullptr;
		int pitch = 0;
		if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0) {
			VS->Data = static_cast<byte *>(pixels);
			VS->BPSL = pitch;
			VESA_Surface2Global(VS);
		} else {
			fprintf(stderr, "[SDL] LockTexture failed in V_Flip: %s\n", SDL_GetError());
		}
	}
	if (profileFlip) {
		auto tLock = stamp();
		s_flipProf.tick(
			ms_d(tUnlock  - t0       ).count(),
			ms_d(tBars    - tUnlock  ).count(),
			ms_d(tCopy    - tBars    ).count(),
			ms_d(tPresent - tCopy    ).count(),
			ms_d(tLock    - tPresent ).count());
	}
}

static dword V_Create(VESA_Surface *VS, SDL_Renderer * renderer)
{
	VS->CPP = (VS->BPP+1)>>3;
	VS->BPSL = VS->CPP * VS->X;
	VS->PageSize = VS->BPSL * VS->Y;

	// Z-buffer in its own malloc, sized X*Y*sizeof(word). Used to be the
	// tail of VS->Data; split out so VS->Data can be the SDL_Texture's
	// locked pixel buffer (zero-copy framebuffer write).
	dword ZBufferSize = sizeof(word) * VS->X * VS->Y;
	if (!(VS->Z16 = (byte *)malloc(ZBufferSize))) return 1;
	memset(VS->Z16, 0, ZBufferSize);

#ifdef __EMSCRIPTEN__
	// Wasm: don't go through SDL_CreateTexture / SDL_LockTexture — there's
	// no SDL renderer on this build. Allocate the framebuffer ourselves;
	// V_Flip uploads it via WebGL2 each frame.
	(void)renderer;
	VS->Data = (byte *)malloc(VS->BPSL * VS->Y);
	if (!VS->Data) return 1;
	memset(VS->Data, 0, VS->BPSL * VS->Y);
	VS->Handle = nullptr;
	VS->Flags |= VSurf_LockRender;
#else
	s_engineTex = SDL2_MakeTexture(renderer, VS->X, VS->Y, "engine");
	VS->Handle = static_cast<void *>(s_engineTex.get());

	// Mark this surface as lock-render so V_Flip writes the engine
	// framebuffer directly into the texture's pixel buffer — child
	// surfaces (Glat's FinalSurf) don't set this flag and stay on the
	// legacy SDL_UpdateTexture path.
	VS->Flags |= VSurf_LockRender;

	// Lock the texture and point VS->Data at the lock pointer. The engine
	// then renders straight into the texture each frame; V_Flip just
	// unlocks (commit), presents, and re-locks for the next frame.
	// SW renderer's lock is essentially free (returns surface->pixels with
	// no copy and unlock is a no-op).
	void *pixels = nullptr;
	int pitch = 0;
	if (SDL_LockTexture(s_engineTex.get(), nullptr, &pixels, &pitch) != 0) {
		fprintf(stderr, "[SDL] LockTexture failed in V_Create: %s\n", SDL_GetError());
		return 1;
	}
	VS->Data = static_cast<byte *>(pixels);
	VS->BPSL = pitch;
	memset(VS->Data, 0, pitch * VS->Y);
#endif

	return 0;
}


SDLTex SDL2_CreateChildTexture(int X, int Y, const char *tag)
{
#ifdef __EMSCRIPTEN__
	// Wasm has no SDL renderer; child surfaces (Glat's FinalSurf) get a
	// null Handle and present through the same WebGL path as the engine
	// surface (V_Flip uploads VS->Data regardless of lockMode).
	(void)X; (void)Y;
	return SDLTex(nullptr, SDLTexDeleter{tag});
#else
	// Same renderer the engine display uses; assumes SDL2_InitDisplay ran.
	if (!SDL_MainSurf.Renderer || X <= 0 || Y <= 0) return SDLTex(nullptr, SDLTexDeleter{tag});
	return SDL2_MakeTexture(static_cast<SDL_Renderer *>(SDL_MainSurf.Renderer), X, Y, tag);
#endif
}

// Tear down + reallocate the SDL-side framebuffer / Z-buffer / SDL_Texture
// at new dimensions, then re-install into the engine globals via
// VESA_VPageExternal (which calls Build_YOffs_Table + VESA_Surface2Global).
// Called from the demo thread at a frame boundary by EngineResize.
// TheOtherBarry's apply_exact iterates a hard-coded 8 rows per tile, so the
// engine surface and Z-buffer must be sized in multiples of TILE_SIZE
// otherwise the last tile row walks past the buffer end and trips the
// rasterizer ~6 rows out (lldb: ldr q30 from a wild pointer). The actual
// window can be any size — V_Flip's letterbox absorbs the 0–7px mismatch.
static constexpr int kEngineSnap = 8;
static int snapEngineDim(int v) {
	v &= ~(kEngineSnap - 1);
	return v < kEngineSnap ? kEngineSnap : v;
}

// Demo's authoring aspect ratio (from rev.cfg's ResolutionX/Y). The engine
// surface always renders at this AR; V_Flip letterboxes the slot inside
// the renderer output. Falls back to 16:9 if the configuration globals
// haven't been initialized yet (FDS_Init path).
extern int32_t g_demoXRes, g_demoYRes;
static float demoAR() {
	if (g_demoXRes > 0 && g_demoYRes > 0) {
		return (float)g_demoXRes / (float)g_demoYRes;
	}
	return 16.0f / 9.0f;
}

// Clamp a window size to the demo's authoring aspect ratio. The result is
// the largest demoAR rectangle that fits inside (winX, winY); any window
// pixels outside that rectangle become black bars in V_Flip's letterbox.
static void clampToDemoAR(int winX, int winY, int &outX, int &outY) {
	const float ar = demoAR();
	const float winAR = (float)winX / (float)winY;
	if (winAR > ar) {
		// Window is wider than demo AR — height limits, bars on left/right.
		outX = (int)((float)winY * ar);
		outY = winY;
	} else {
		// Window is taller (or equal) — width limits, bars on top/bottom.
		outX = winX;
		outY = (int)((float)winX / ar);
	}
}

void SDL2_HandleResize(int newX, int newY)
{
	const int rawX = newX;
	const int rawY = newY;
	// Letterbox engine surface to the demo's authoring AR before snapping.
	clampToDemoAR(newX, newY, newX, newY);
	newX = snapEngineDim(newX);
	newY = snapEngineDim(newY);
	fprintf(stderr, "[RESIZE] window %dx%d → engine %dx%d (AR %.3f), current %dx%d\n",
	        rawX, rawY, newX, newY, demoAR(), SDL_MainSurf.X, SDL_MainSurf.Y);
	if (newX <= 0 || newY <= 0) return;
	if (newX == SDL_MainSurf.X && newY == SDL_MainSurf.Y) return;

	// Free the engine-side YOffs that came from the previous
	// Build_YOffs_Table(MainSurf); VESA_VPageExternal stomps the pointer
	// via memcpy below, so freeing it here avoids the leak.
	if (MainSurf && MainSurf->YTable) {
		delete[] MainSurf->YTable;
		MainSurf->YTable = nullptr;
	}

#ifdef __EMSCRIPTEN__
	// Wasm: framebuffer is a plain malloc — free and zero. WebGL texture
	// gets resized lazily on the next Wasm_PresentGL call.
	if (SDL_MainSurf.Data) {
		free(SDL_MainSurf.Data);
		SDL_MainSurf.Data = nullptr;
	}
	SDL_MainSurf.Handle = nullptr;
#else
	// VS->Data is the texture's locked pixel buffer — unlock it before
	// destroying the texture (SDL_DestroyTexture on a still-locked
	// streaming texture is undefined). The deleter (RAII) actually frees;
	// we just need to release the lock + alias.
	if (SDL_MainSurf.Data && SDL_MainSurf.Handle) {
		SDL_UnlockTexture(static_cast<SDL_Texture *>(SDL_MainSurf.Handle));
	}
	SDL_MainSurf.Data = nullptr;
	s_engineTex.reset();
	SDL_MainSurf.Handle = nullptr;
#endif
	// Z-buffer is its own allocation now.
	if (SDL_MainSurf.Z16) {
		free(SDL_MainSurf.Z16);
		SDL_MainSurf.Z16 = nullptr;
	}

	SDL_MainSurf.X = newX;
	SDL_MainSurf.Y = newY;
	V_Create(&SDL_MainSurf, static_cast<SDL_Renderer *>(SDL_MainSurf.Renderer));

	VESA_VPageExternal(&SDL_MainSurf);
}

dword SDL2_InitDisplay(SDL_Window *window)
{
	sdl_window = window;
	SDL_MainSurf.BPP = 32;

	// Fill in the secondary surface VSurf structure

#ifdef __EMSCRIPTEN__
	// Wasm: skip SDL_CreateRenderer entirely. The SDL software renderer's
	// SDL_RenderPresent was costing ~28 ms per flip; we present via
	// WebGL2 + texSubImage2D + a fullscreen quad instead. WebGL must be
	// the only context on the canvas, so don't let SDL grab a 2D one.
	// Wasm_InitGL is deferred until the first V_Flip — by then the canvas
	// has been transferred to the CodeEntry pthread, which is where the
	// flip-side WebGL context needs to live.
	SDL_Renderer *renderer = nullptr;
	SDL_MainSurf.Renderer = nullptr;
	int px = 0, py = 0;
	SDL_GetWindowSize(sdl_window, &px, &py);
#else
	// Native: real SDL renderer with vsync; software fallback isn't needed.
	SDL_Renderer * renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_PRESENTVSYNC);
	SDL_MainSurf.Renderer = renderer;
	int px = 0, py = 0;
	SDL_GetRendererOutputSize(renderer, &px, &py);
#endif
	// Letterbox to demo AR + snap to TILE_SIZE — same as SDL2_HandleResize,
	// just for the boot path.
	int engX = px, engY = py;
	clampToDemoAR(px, py, engX, engY);
	SDL_MainSurf.X = snapEngineDim(engX);
	SDL_MainSurf.Y = snapEngineDim(engY);

	V_Create(&SDL_MainSurf, renderer);

	SDL_MainSurf.Flip = V_Flip;

	VESA_VPageExternal(&SDL_MainSurf);

	V_Flip(MainSurf);

	FPU_LPrecision();

#ifdef __EMSCRIPTEN__
	// Fire the JS-side resize handler so the canvas backing snaps to the
	// actual browser window size; emscripten's SDL2 will subsequently emit
	// SDL_WINDOWEVENT_SIZE_CHANGED, the demo thread's poll_pending_resize
	// picks it up at the next frame top, and EngineResize redoes the
	// framebuffer at the new dims. Until that fires, the demo runs one
	// frame at g_demoXRes/Y.
	//
	// Must run on the main browser thread — under PROXY_TO_PTHREAD this
	// function is on a pthread worker where `window` is undefined.
	MAIN_THREAD_EM_ASM({ window.dispatchEvent(new Event('resize')); });
#endif

	return 0;
}

#ifdef __EMSCRIPTEN__
static SDL_AudioDeviceID g_audio_dev = 0;
static int g_audio_cb_count = 0;
// Mute flag toggled from the JS shell's button. We still pull samples from
// the modplayer (so position keeps advancing) and just silence the output —
// that way unmute resumes mid-track instead of restarting.
static std::atomic<bool> g_mute{false};

extern "C" EMSCRIPTEN_KEEPALIVE void SDL2_SetMute(int muted)
{
	g_mute.store(muted != 0);
	fprintf(stderr, "[AUDIO] mute=%d\n", muted);
}

// Tell SDL the canvas backing size (in physical pixels) has changed.
// JS-side toggles (HiDPI button, orientationchange, window drag at a new
// DPR) pick the desired backing size and call into here. SDL_SetWindowSize
// updates window->w/h, calls emscripten_set_canvas_element_size internally
// (which actually resizes the canvas DOM element), and fires
// SDL_WINDOWEVENT_SIZE_CHANGED — the demo thread's resize event handler
// then queues g_pendingResize and EngineResize redoes the framebuffer.
//
// Setting canvas.width/height from JS alone doesn't get there: emscripten's
// SDL2 listens to window 'resize' but reads window.innerWidth/innerHeight,
// not the canvas dims, so HiDPI toggles never reached SDL. Routing through
// SDL_SetWindowSize is the canonical fix.
extern "C" EMSCRIPTEN_KEEPALIVE void SDL2_RequestSize(int w, int h)
{
	if (!sdl_window || w <= 0 || h <= 0) return;
	// Pre-clamp to the engine surface dims (AR + /8 snap) so the canvas
	// backing equals the engine output. That kills SDL_RenderCopy's
	// scale-blit in V_Flip — the renderer output and the engine texture
	// are now the same size, the copy is at most a 1:1 memcpy, and the
	// letterbox bars come from CSS instead of an extra fill pass.
	int engX = w, engY = h;
	clampToDemoAR(w, h, engX, engY);
	engX = snapEngineDim(engX);
	engY = snapEngineDim(engY);
	fprintf(stderr, "[SDL] RequestSize viewport %dx%d -> canvas %dx%d\n", w, h, engX, engY);
	SDL_SetWindowSize(sdl_window, engX, engY);
}

static void wasm_audio_callback(void* userdata, Uint8* stream, int len)
{
	if (g_audio_cb_count < 3) {
		fprintf(stderr, "[AUDIO] callback #%d len=%d ud=%p\n",
		        g_audio_cb_count, len, userdata);
	}
	g_audio_cb_count++;
	// 512 frames × 2 channels × sizeof(float) = 4096 bytes per callback.
	Modplayer_FillBuffer((ModplayerHandle)userdata,
	                     reinterpret_cast<float*>(stream),
	                     len / (2 * sizeof(float)));
	if (g_mute.load(std::memory_order_relaxed)) {
		memset(stream, 0, len);
	}
}

// Runs on the browser main thread (proxied via emscripten_sync_run_in_main_runtime_thread).
// SDL2's emscripten audio implementation references a JS-side `SDL2` global
// that only exists in the main thread's JS context, so the open call must
// happen there.
static void open_audio_main_thread(void* modplayerHandle)
{
	SDL_AudioSpec want = {};
	SDL_AudioSpec have = {};
	want.freq = 48000;
	want.format = AUDIO_F32SYS;
	want.channels = 2;
	want.samples = 512;  // matches xmplayer's AUDIO_BUF_FRAMES
	want.callback = wasm_audio_callback;
	want.userdata = modplayerHandle;
	g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	fprintf(stderr, "[AUDIO] OpenAudioDevice -> dev=%u (err=%s)\n",
	        (unsigned)g_audio_dev, g_audio_dev ? "ok" : SDL_GetError());
	if (g_audio_dev) {
		fprintf(stderr, "[AUDIO] have: freq=%d fmt=%04x ch=%d samples=%d\n",
		        have.freq, have.format, have.channels, have.samples);
		SDL_PauseAudioDevice(g_audio_dev, 0);
		fprintf(stderr, "[AUDIO] device unpaused\n");
	}
}

void SDL2_StartMusic(void* modplayerHandle)
{
	if (g_audio_dev || !modplayerHandle) return;
	emscripten_sync_run_in_main_runtime_thread(
		EM_FUNC_SIG_VI, &open_audio_main_thread, modplayerHandle);
}

void SDL2_StopMusic()
{
	if (!g_audio_dev) return;
	SDL_CloseAudioDevice(g_audio_dev);
	g_audio_dev = 0;
}
#endif

