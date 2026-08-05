/******************************************************************************
	Source code for REVIVAL / FLOOD.

	Final version: MOV' 98 (dec' 98)
	
	Revising started at Jan 2002 using FDS v0.76A
	In progress: eval. 70% Complete
 
	FDS version promoted to 0.84 at 22.04.02
	FDS version promoted to 0.85 at 05.07.02
	

Rendering pipeline optimization
-------------------------------
current rendering pipeline uses:
1. Triangle meshes : supported
1.1. arrays of fixed size vertex structures.
1.1.1. Vertex position, normal, U, V, EU, EV
1.1.2. Vertex T.position, RZ, UZ, VZ, EUZ, EVZ
1.1.3. Vertex lighting [float R, G, B]
1.1.4. Vertex visibility Flags
1.2. model / instance seperation : not supported
2. Transform pipeline with bounding spheres and backface culling, floating point 4x3 transform.
3. Lighting pipeline is non-optimized, supports ambient and diffuse lighting.
4. Painter's algorithm: base-256 Radix back-to-front sorting.
5. Frustrum clipper : supported
5.1. stage : immediately before rasterization
5.2. input : a triangle
5.3. output : a set of triangles
6. Polygon rasterization routines:
6.1. Polygon support : triangles only
6.2. ZBuffering : no support
6.3. Gouraud : supported
6.3.1. perspective correction : not supported
6.3.2. Modulation with screen : supported, uses MMX machine code.
6.4. Texture mapping : supported
6.4.1. perspective correction : piecewise linear per 16 pixels, floating point.
6.4.2. 8bit textures onto 16/32bit framebuffer : not supported
6.4.3. Mipmapping : not supported.
6.4.4. Tiled textures : not supported.
6.4.5. Maginification filter : point
6.4.6. Alpha Blending : constant 50% (aka 'Transparent' texture)
6.4.7. Intrinsics : SMC, uses FPU per 16 pixels.
6.4.8. UV Wrapping : supported

planned rendering pipeline uses:
1. Triangle meshes : supported
1.1. structure contains arrays of vertex components.
1.1.1. Vertex position, normal, U, V, Flags.
1.1.2. It is yet unclear if vertices should be formed before or after clipping,
	   such vertices should contain px,py, 1/z, [dword ARGB].
1.2. model / instance separation : feasible, but not supported.
2. Transform pipeline with bounding boxes, spheres and backface culling, 
   floating point 4x3 and SSE 4x4 strided structure transform.
3. Optimized lighting pipeline with ambient, diffuse and possibly specular [using MMX modulate 2x].
4 Z-Buffering : base-256 Radix front-to-back, texture-based grouping sort.
				all transparent/additive objects should be rendered last.
5. Frustrum clipper : supported
5.1. stage : immediately before rasterization OR batched during/after transform pipeline
5.2. input : a triangle
5.2. output : an ngon
6. Polygon rasterization
6.1. Polygon support : simple convex ngon
6.2. ZBuffering : supported
6.2.1. precision : 16bit
6.2.2. interpolation : integer, scaled fixed pt. 1/z.
6.2.3. comparision : integer compare, could configurate with SMC.
6.2.4. comparision impl. : conditional jump across inner loop
6.2.5. clear : either configurate compare, or full Z-buffer clear per frame.
6.3. Gouraud : supported
6.3.1. perspective correction : per scanline, or automatic constant-z polygon fragmentation
6.3.2. Modulation with screen : not supported
6.4. Texture mapping : supported
6.4.1. perspective correction : piecewise linear per 16 pixels, FP or SSE.
	   check possibility for unrolling to utilize SSE 4 parallel division units
6.4.2. 8bit textures onto 32bit framebuffer : supported
6.4.3. Mipmapping : supported for both 8bit and 32bit, [max width 256 pixels]. 
6.4.3.1. Minification Filter : point, possibily linear.
6.4.3.2. Check if separate palettes are required.
6.4.4. Tiled textures : supported, configurable tile size. 
		Cannot cooperate with bilinear filtering
6.4.5. Maginification Filter : point. (test speed for bilinear)
6.4.6. Alpha Blending : configurable constant, gouraud alpha, or texture/page alpha channel
6.4.7. Initrinsics : ngon section traversing written in C. 
		uses SSE per 16/32 pixels and MMX within the inner loop.
		Z-Buffer pointer omitted (use SMC and an immediate delta from framebuffer ptr)
		Texture/Mipmap palette pointer omitted (use constant delta from texture ptr)

Revised Pipeline design
1.Data storage
1.1. SCENE contains an array of Objects<-->Trimeshes, and an array of Models.
Renderer object operates over scene, and contains an index list, and an allocated
SOA vertex buffer large enough to contain all vertices in scene + clipping extra.
VB contains:
transformed X, Y, Z, RGB, U, V
screen X, screen Y, 1/Z, RGB, U/Z, V/Z


1.2. Models 
1.2.1. contain an SOA vertex buffer with the following data types
position X,Y,Z. normal X,Y,Z. U, V.
1.2.2. contain an SOA face information
index list
material global ID or ptr

1.3. Trimeshes have a pointer to a model, position and rotation status.

	Coding log since 11 Feb 2002 (earlier entries may be out of order of occurrence or missing)
	===========================================================================================

	[late DEC 2001-early JAN 2002] 
	REVIVAL Project reopened
	
	- Removed JPegLibD.lib and MIDAS.lib from the VC6 build.
	- Added matching __asm blocks for all #pragma aux. to remove dependency in wcc
	- Added support for external initialization of logical video unit in FDS
	- Removed unconditional usage of DPMI routines in FDS
	- Changed MMX detection to use CPUID instead of INT 6h (invalid opcode trap)
	- Fixed a bug in the FLD reader which caused it to incorrectly count #faces.
	- Fixed a bug in the splines derivative calculation code which caused out of array bounds write.
	- Changed external assembly functions naming convention to support both wcc and vc6.
	- Fixed bug caused by assuming char type to be unsigned.
	- Added support for the win32 platform in the demo.
	- Added support for GDI-based display.
	- Added support for windows multimedia timer (winmm.lib).
	- Fixed optimized VC6 builds by wrapping assembly function calls with pushad/popad.
	- Fixed a memory leak in image manipulation code.
	- Removed the automatic application of the Reset_XForm function.
	- Fixed texture coordinate wrapping support
	- Removed a read from null (address 0) in the FOUNTAIN demo part.
	- Fixed calculation of the FOV spline from ZoomFactor envelope in FLD reader.
	- Added support for IJLD, Intel's JPeg decoding library.
	- Added support for the FMod sound system.
	- Added padding to fix an otherwise unstable read in the Run_Distort, CITY demo part.
	- Added full raster support in arbitrary resolution.
	- Added Wobbler(Grid renderer) support in arbitrary resolution.
	- Added automatic Mipmap generation for textures
	- Changed sprite scaling to be proportional to the display resolution.
	- Changed Flare generation code to be invariant of target image resolution.
	- Improved Flare generation parameters.
	- Fixed Far Z frustrum clipping.
	- Changed application of fog to the pre-filler stage.
	- Added a fogged version for transparent textured surfaces. (two-pass prefiller)
	- Changed automatic flare image generation to globally used single flare
	- Changed Distortion parameters for CITY demo part.
	- Added 32x32 MMX-based point filtering shaded-additive image scaler.
	- Added Skycube generation code to FDS.
	- Added support for particle global and omnilight interpolated size in scaler (prefiller)
	- Added support for particle color before calling the 32x32 shaded scaler.
	- Increased Far Z clipping plane in CITY demo part from 2500 to 5000.
	- Added temporal antialiasing for particle generation in FOUNTAIN demo part.
	- Increased particle velocity, spawn rate and maximal amount in FOUNTAIN demo part.
	- Added support for arbitrary resolution in GLAT demo part.
	- Fixed cross-fade rate overflow bug in GLAT demo part.
	- Optimized grid calculations in GLAT demo part.
	- Optimized framebuffer modulation (Alphablending) code.
	- Added fill rate (pixels/sec) measuring code.
	- Improved Lighting model.
	- Fixed a bug with backface culling for scaled objects
	- Fixed a bug with lighting for scaled objects.
	- Added hardcoded fixup for point light range in GREETS demo part.

	11/02/02
	- Coding log created
	- Recorded all known entries dated before 11/02/02

	12/02/02
	- Added DirectDraw 6 windowed/fullscreen support

	13/02/02
	- Fixed Framerate dependency in GLAT demo part.
	- Separated initialization sequence in the GLAT demo part.
	- Fixed a bug in the page modulation routine.
	- Adjusted outer particle generation and rendering parameters in FOUNTAIN demo part.
	- Added Motion blur to FOUNTAIN demo part.

	14/02/02
	- Added support in per-particle imagesize.
	- Corrections to the lighting parameters are now made in GREETS demo part, 
	  removed hardcoded fixup from lighting module.
	- Fixed 'align to path' type animation.
	- Fixed an allocation bug in the FDS messaging system.

	15/02/02
	- Added Direct3D 8 windowed/fullscreen support [inoperative]
	- Attempted to correct DirectX memory allocation errors. Those also occur in
	  MSR_Proj and when using DX8, and don't appear to cause anything to malfunction.
	  It seems to be associated with usage of two separate threads within the program's
	  scope, or the order by which the system is shut down.

	18/02/02
	- Added RGB color support for flat shader.
	- Optimized flat shader innerloop using MOVNTQ

	8/03/02
	- Added rain FX prototype
	- Fixed FP control word bug when using DDraw fullscreen.

	29/03/02
	- Fixed mistakenly assumed unsigned char type in mipmap generation

	30/03/02
	- Added C texture mapper prototype (1)

	31/03/02
	- Fixed bounding sphere / frustrum intersection tests.
	- Fixed Vertex visibility flag calculation across rendering pipeline
	- Added convex polygon clipper.
	- Added modifiable near-Z clipping plane support.

	01/04/02
	- Fixed ZBuffer interpolation errors
	- Written MMX assembly code for IX mapper inner-inner loop 
	- CITY demo part : 512x384 14 FPS...
	- Normalized UV texture coordinates
	- Added mipmapping/block tiling support
	- zbuffer imprecision within subinnerloop routine should be fixed
	- Added 'luminosity' support
	- Fixed sorting for ZBuffered scenes
	- Added Transparent/Fogged versions of new Texture/Gouraud mapper
	- Added Arbitrary Texture resolution support in IX mapper (should code texture read (actually, the entire innerloop) in assembly)

	02/04/02
	- Added ZBuffer support for sprite scalers
	- FOUNTAIN demo part has a marginal framerate decrease when using Z-Buffering mappers.
	- profiled FOUNTAIN: almost 100% rasterization runtime. image scaler rountine needs to be
	  optimized. 

    06/04/02
	- Added Polygon/mipmap clipper. This has a poor implementation, and is therefore executed
	  only on polygons that occupy 10% or more of the screen surface.

	08/04/02
	- Fixed Polygon/mipmap clipper, log/exp tables
	- Added FF/Rew/Pause to all demo parts
	- Fixed Reflection transform, Reflected backface culling
	- Reverted to standard lighting pipeline in CITY demo part.
	- NOTE: ZBuffered flat/gouraud [with Trans/Fog support] only should be coded soon.
	+ CITY demo part related issues
		- NOTE: Monorail/tracks are not properly attached to sea level (h + 86 units)
		- NOTE: Sea level in original scene file decreases from 20 to 0 throughout runtime.
				right now sea level was changed to be constantly 0.

	13/04/02
	- Added IX polygon mappers with Z-buffering and transparency support for texture only and
	  gouraud only.
	- Polygon/mipmap clipper is more efficient and can be used on polygons that cover 2%
	  or more screen area with minimal performance loss
	- Fixed a minor sorting bug with transparent objects
	- Mounted CHASE scene, in principle it is operative with the availability of Z-buffering
	- Fixed a bug with flat-shaded polygons, which could have 'Noshading' flag set.	

	14/04/02
	- Added generic MMX register emulation class. 

    18/04/02
	- Rewritten lighting model to conform to LW3D.
	- GREETS demo part : 512x384 21 FPS...
	
	19/04/02
	- Added support for single (central) Z-compare for particles and flares.
	- Changed Face structure to allow convenient storage of flare related information
	- Fixed bug in GLATO demo part when YRes isn't a multiple of 8.
	- Added new C functions for wobbler f/x used in GLATO demo part.

	20/04/02
	- Added precalculation of static lighting over static (stationary) objects
	- GREETS demo part : 512x384 22 FPS...

	21/04/02
	- Added tile-based rendering for particle sprites.
	- FOUNTAIN demo part : minimal 512x384 FPS went from 8.9 to 10.2.

	22/04/02
	- Fixed an out of array bounds read bug in ConvertBPP, but the fixup is
	  hardcoded for 24->32bit conversions.

    23/04/02
	- Added Rain prototype for CITY scene (2)

	??/04/02
	- Fixed Rain positioning so each drop has nonzero screen time.

	- LAZYNESS STRIKE 

	27/06/02
	- Added IXTGZM mapper, integrated with FDS. currently draws flatpoly spans. (3)

	02/07/02
	- Improved IXTGZM mapper, draws gouraud shaded polygons with partial zbuffering support

	03/07/02
	- Fixed IXTGMZ mapper, renders correct zBuffered gouraud.

	04/07/02
	- Added full texture coordinate interpolation in IXTGZM mapper.
	- IXTGZM : Gouraud only, 20M pixels/second, 40% of the maximal temporal bus write. (Pentium 3 700MHZ?)

	22/07/02
	- Fixed texture coordinate interpolation bug
	- Added SMC support for arbitrarily sized textures up to 256x256.	
	- IXTGZM : Texture/Gouraud, 15M pixels/second, 30% of the maximal temporal bus write.

    26/07/02
	- Added 4x4 block-tiled texturing support (improves cache utilization)
	- Added a transparent version of IXTGZM
	- ITGZM : Texture/Gouraud, 17M pixels/second
	+ GREETS demo part : 512x384 25FPS...
	+ FOUNTAIN demo part : 512x384 12FPS minimum
		- NOTE: Modified vortex texture generation to create a 4x4 block-tiled texture

	27/07/02
	+ Added a profiler prototype within scene code
		- NOTE: frustrum/mapper now take only 50-60% of the total running time.
			CITY,GREETS: xform+lighting take 25-30% together
			flip takes over 15%

	01/08/02
	- Added bounding sphere elimination test to the lighting model
	+ SSE'ed the lighting model code
		- flip was marked as the next target :)
	- Added 1 bit of Zbuffer interpolation precision by rounding instead of truncation
	- Added a convenient aligned memory allocator, aligned texture addrs to 64

	- HUGE LAZYNESS STRIKE 

	07/02/03
	- Removed all memory frees because program appears to be damaging the heap. The
	malfunctioning code is yet to be found.

	- HUGE LAZYNESS STRIKE 
	02/07/03
	- Imported project to VC7
	- Corrected DirectDraw 6 fullscreen display code

	- HUGE LAZYNESS STRIKE 
	10/01/04
	- Project Resumed
	- Added configuration DB.

	
	Remarks
	(1): prototype C mapper has the following specs.
	Scan-conversion type: Counterclockwise convex polygon
	Perspective correction: Bilinear upto 16/32 pixel subspans
	Limited tiling support
	Limited arbitrary texture size support (powers of 2 only)
	Z-Buffering supported
	Z-Buffer:
		std. (1/z) W-buffer, fp. upto subspans, 16bit Z-Bu,ffer precision
	Gouraud:
		Edges: 8:16 Fixed point, Affine interpolation.
		Spans: 8:8 Fixed point, Affine interpolation.
		d/dX: per scanline
	Textures: 
		Supported Pixelformats: 32bit RGB color
		Scribbled blocks: unsupported
	32bit Texture/Gouraud rendering
	Inner loop:
		MMX assembly code.
	Mods:
		Transparency, Z-shading (fog).
	Code generation:
		MMX/SSE register emulation classes: in progress
	Performance analysis:
	GREETS:
	Runs about 30% slower than the old Texture/Gouraud assembly code.
	with Z-Buffering: Almost exactly the same speed, althrough without complete z-buffer
	support in all mappers this is somewhat a speculation.
	CITY:
	with Z-Buffering: Runs FASTER than equivalent Texture/Gouraud assembly code. Kickass.	

	- full engine front-end support in mipmapping should be added to measure performance gains.
	- new ngon frustrum clipper should be written along with a routine to break up polygons 
	  into pieces each with its own constant mipmapping level.

	(2): looks more or less like a piece of crap. and it's pretty slow, too. unless the
	alpha blending version gonna be da bomb, and a significantly higher % of the raindrops
	are to be presented, this is gonna be taken out of the demo

	(3): related coding logged within IX.ASM.

	TODO list.

	18.04.02
		Seperate omni's Flare size (Size track) from Intensity. FLD only has an Intensity
		track, Flare size should be either set manually or based on a new track read from
		LW3D.
	20.04.02
		general engineering upgrades to FDS - Viewports, renderer, naming conventions,
		and framework to allow hardware rendering. 3d object class, remove memory leaks.
		(impl: ongoing low priority)

		FlatPoly with efficient Z testing

		Fast (2 pixels per r/w) spriter

	24.04.02
		Try to predict if raindrops are out of frustrum for their entire lifespan and
		replace them until they have a viewable position. This effort should be made because
		less than 10% of the raindrop particles are visible at a time.

		Render rain particles using alpha blending

	03.07.02
		Use better methods of tile-based rendering to further improve the performance
		of v.high overdraw sprite rendering. 

    27.07.02
		try using a separate doublepixel (320x240) surface to draw all flares on, and
		then use 'bilinear' addative x2 image enlargement to write all contents 
		to framebuffer. this means spriters are going to support variable zBuffer ptrs.

		SSE the transform pipeline.
		separate clipping flags calc from xforming

    02.08.02
	    write nonaligned bounding boxes
		use pages AGP memory and DDraw's Blt

	02.07.03
		Lighting takes too much time (20%) on pentium 4, in CITY and GREETS scenes.

	10.01.04
		Add Configurator program; run configurator before demo.
		Add support for objects with multiple instances. This can be supported because we now employ a zBuffer.

	12.01.04
	  Add rendering pipeline for single object instances. FDS now supports meshes 
	  with multiple instances.

	+ more stuff
	 - omni 'size' spline should be called intensity, flare size should be a different parameter
	 - tmapper in assembly
	 
	+ stuff
	 - process mutex (only 1 running demo process at once)
	 - remove all writes to disk (runtime log, video unit log etc)
	 - fix frustrum clipper. [right/bottom]
	 - correct right/bottom edges in bilinear scaler
	 - initialization progress indicator
	 - sleep while window inactive (or not... music sync)
	+ 3D engine upgrades
	 + redesign/recode pipeline with strided arrays, early 3d-clipping (?), and z-buffer optimized sorting.
		- deemed ineffective, this can be mostly ignored
	 - Z-buffering, mipmapping, [tiled?] [palettized?] texture/gouraud single pass mapper based on SSE assembly

	+ demo visuals
	 - GFX improve for lightning and vortex
	 - light projection toward vortex in FOUNTAIN
	 - fountain spawned particles lose/reverse gravity and spin when vortex opens
	 - rain over CITY
	 - skycube over CITY
	 - volumetric (fire, metaballs, ?) in central room GREETS.
	 - shadowing in GREETS
	 - additional 'traffic' animations for CITY

******************************************************************************/
#define POINTER_64
#include "Rev.h"
#include "FillerTest.h"
#include "ImageCompression.h"
#include "PhotonTracer.h"
#include "Snapshot.h"
#include "MaterialImport.h"
#include "Resize.h"
#include <float.h>
#include <Base/TriMesh.h>
#include <Base/FeatureFlags.h>
#include <RENDER/EnvCube.h>
#include <RENDER/ChunkOcclusion.h>  // g_occlSnapshotInert: pin dumps keep the cull inert
#include <RENDER/VizCycle.h>        // VizCycle_Step: the X / Shift+X debug-viz cycle
#include <Base/TuneServer.h>
#include "Config.h"
#include "Threads.h"
#include "../Modplayer/Modplayer.h"
#include "SDL2.h"
#include <SDL.h>
#include <csignal>
#include <utility>
#include <iostream>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "MainLoop.h"
#endif
#ifndef __EMSCRIPTEN__
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

// Locate the executable's directory and chdir into the asset root so
// the demo's CWD-relative fopen()s (rev.cfg, SCENES/, TEXTURES/,
// FONTS/, Revival.xM) resolve no matter where you launched it from.
// Tries: <bindir>, <bindir>/../Runtime, <bindir>/../../Runtime — the
// first one that has rev.cfg wins. No-op on emscripten (assets are
// preloaded at "/").
static void ChdirToAssetRoot(const char* argv0)
{
#ifndef __EMSCRIPTEN__
	char exe[4096] = {0};
#ifdef __APPLE__
	uint32_t size = sizeof(exe);
	if (_NSGetExecutablePath(exe, &size) != 0) {
		strncpy(exe, argv0, sizeof(exe) - 1);
	}
#elif defined(__linux__)
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n <= 0) strncpy(exe, argv0, sizeof(exe) - 1);
#else
	strncpy(exe, argv0, sizeof(exe) - 1);
#endif

	char resolved[4096];
	if (realpath(exe, resolved)) {
		strncpy(exe, resolved, sizeof(exe) - 1);
		exe[sizeof(exe) - 1] = 0;
	}

	char* dir = dirname(exe);
	const char* suffixes[] = { "", "/../Runtime", "/../../Runtime" };
	for (const char* sfx : suffixes) {
		char cand[4096];
		snprintf(cand, sizeof(cand), "%s%s/rev.cfg", dir, sfx);
		if (access(cand, R_OK) == 0) {
			char target[4096];
			snprintf(target, sizeof(target), "%s%s", dir, sfx);
			if (chdir(target) != 0) {
				fprintf(stderr, "ChdirToAssetRoot: chdir(%s) failed\n", target);
			}
			return;
		}
	}
	// No rev.cfg found anywhere obvious — leave CWD alone and let
	// the next fopen() report the error in context.
#else
	(void)argv0;
#endif
}

// global configuration db
static ConfigurationDB *g_cfg;

// global status flags
dword g_playMusic;
dword g_profilerActive;
int32_t g_demoXRes;
int32_t g_demoYRes;
int32_t g_fullScreenMode;
int32_t g_displayType;
int32_t g_hiDPI;

// See Rev.h for semantics.
std::atomic<bool> g_shouldQuit{false};
std::atomic<bool> g_skipScene{false};
// g_forceMipLevel / g_vizMipLevel are defined in FDS/RENDER/DeferredLighting.cpp
// (so the FDS library doesn't have an unresolved symbol from DEMO/).
// Declared in Rev.h.

float dTime;

char *ModuleBuf;
//dword g_RevModuleHandle;
ModplayerHandle g_RevModuleHandle;
//FModWrapper	module;
void Destroy_Scene(Scene *Sc)
{
	return;
	TriMesh *Tri;
	int32_t total = 0,f;

//	printf("Shootin' scene...\n");
	for(Tri=Sc->TriMeshHead;Tri;Tri=Tri->Next)
	{
		if (Tri->VIndex>5||Tri->FIndex>5)
		{
			delete Tri->Verts;
			delete Tri->Faces;
			f = sizeof(Vertex)*Tri->VIndex+sizeof(Face)*Tri->FIndex;
			total += f;
//			printf("Freed %d Bytes from mesh killing\n",f);
		}
	}
	for(Material *Mat = MatLib;Mat;Mat=Mat->Next)
	{
		if (Mat->RelScene == Sc)
			if (Mat->Txtr&&Mat->Txtr->Data)
			{
				 freeAlignedBlock(Mat->Txtr->Data);
				 Mat->Txtr->Data = NULL;
				 total += 262144;
//				 printf("Freed 262144 Bytes from Texture killing\n");
			}
	}
//	printf("%d Frags\n",total);
	freeAlignedBlock(Sc);
}


static SDL_TimerID TimerID = 0;
static int32_t Timer__NA = -1;

static char G_TitleStr[MAX_GSTRING] = "Revival / Flood";
static char G_WindowClassStr[MAX_GSTRING] = "Revival";
static char G_AppTitle[MAX_GSTRING];

Uint32 TimerProc(Uint32 interval, void *param)
{
	// Review scrub: hold F4 / F3 to run the scene clock fast forward /
	// backward at --scrub_speed x (default 4). NOT PgUp/PgDn or -/= : all four
	// are Dynamic_Camera fly keys (MainLoop.cpp anyFreeCamKey; -/= translate to
	// ScGrayMinus/ScGrayPlus), and the Mac laptop needs fn for PgUp/PgDn.
	// F3/F4 were added to MainLoop's scancode table for this. The clock IS Timer —
	// RENDER.CPP:1553 derives CurFrame from it linearly — so scaling it
	// scales camera splines, object animation and scene progress together,
	// which is exactly what reviewing a pose needs. Clamped at 1: t<0 makes
	// CurFrame extrapolate below the scene's StartFrame.
	// Runs on SDL's timer thread; the Keyboard byte array is written by the
	// main thread's event pump — a torn read here just costs one tick at the
	// wrong speed, which is what every other Keyboard consumer already
	// accepts.
	int32_t step = 1;
	const int32_t mult = fds::FeatureFlags::scrub_speed();
	if (mult > 1) {
		if      (Keyboard[ScF4]) step =  mult;
		else if (Keyboard[ScF3]) step = -mult;
	}
	if (step >= 0) {
		Timer += step;
	} else {
		const int32_t nxt = Timer.load(std::memory_order_relaxed) + step;
		Timer.store(nxt < 1 ? 1 : nxt, std::memory_order_relaxed);
	}
	return interval;
}

int TimerInit(int32_t Freq)
{
	Timer_Freq = Freq;
	uint32_t msInterval = 1000/Timer_Freq; //millisecond accuracy.
	uint32_t wTimerRes = 0;

//	TimerID = timeSetEvent(msInterval,wTimerRes,TimerProc,NULL,TIME_PERIODIC);
	TimerID = SDL_AddTimer(msInterval, TimerProc, nullptr);

	if (!TimerID)
	{
		return 0;
	}

	Timer = 0;
	return 1;
}

void TimerRemove(void)
{
	if (TimerID)
	{
		SDL_RemoveTimer(TimerID);
		TimerID = 0;
	}
}

/*void Run_RotationTest()
{
	Image Im;
	New_Image(&Im, 256, 256);
//	Complex C,R1,R2;
//	Complex_Form(&C, 0.5, 0.3);
//	Complex_Form(&R1, -2.0,-2.0);
//	Complex_Form(&R2,  2.0, 2.0);
//	Generate_Julia_Fractal(&Im, &C, &R1, &R2);
	int32_t i,j;
	for(i=0; i<256; i++)
		for(j=0; j<256; j++)
			Im.Data[i+j*256] = (  ((i>>2) ^ (j>>2)) <<3) * 0x010101;

	Image IRot;
	New_Image(&IRot, 640, 480);

	dword *vpage = (dword *)VPage;
	while (Timer < 3000)
	{
		// clear target.
		memset(IRot.Data, 0, 640*480*sizeof(dword));
		RotateImage(IRot,Im, 320, 240, (float)Timer*0.003, 1.0);

		dword *p = IRot.Data;
		for(j=0; j<480; j++)
			for(i=0; i<640; i++)
				vpage[i+j*XRes] = *p++;

		Flip(MainSurf);

		if (Keyboard[ScESC]) break;
	}
	while (Keyboard[ScESC]) continue;
	if (Timer > 3000)
		Timer -= 3000;
	else
		Timer = 0;
}*/

struct Raindrop
{
	Vector pos;
};

dword numDrops;
Raindrop *RD;
//static float dTime;

inline void fist(int32_t &r, float x)
{
	r = static_cast<int32_t>(x);
}

static void rainmapper(float x1, float y1, float x2, float y2)
{
	if (y1<0 || y2 >= YRes || x1<2 || x2<2 || x1 >= XRes-3 || x2 >= XRes-3) return;	

	float rdy = 1.0 / (y2-y1);
	float dxdy = (x2-x1) * rdy;

	int32_t iy1,iy2, ix;
	fist(iy1, y1);
	fist(iy2, y2);
	float x = x1 + (iy1-y1) * dxdy;

	dword *scanline = (dword *)VPage;
	dword j = XRes;
	scanline += iy1 * j;

	float lt = (iy1-y1) * rdy;
	dword R, G, B, a, A, pixel;

	for(int32_t y=iy1; y<iy2; y++)
	{
		fist(ix, x);

		if (lt < 0.7)
		{
//			R = lt * 0xCF / 0.8;
//			G = lt * 0xCF / 0.8;
//			B = lt * 0xFF / 0.8;
			A = lt * 0xFF / 0.7;
		} else {
//			R = (1.0-lt) * 0xCF / 0.2;
//			G = (1.0-lt) * 0xCF / 0.2;
//			B = (1.0-lt) * 0xFF / 0.2;
			A = (1.0-lt) * 0xFF / 0.3;
		}

		//dword P2 = ((R>>2)<<16) + ((G>>2)<<8) + (B>>2);
		//dword P1 = ((R>>1)<<16) + ((G>>1)<<8) + (B>>1);
		//dword P0 = ((R   )<<16) + ((G   )<<8) + (B);
		
		// modulate(scanline[ix-2], 0x7F7F7F, A, 255-A);
		
/*		for (int dx=-2; dx<=2; dx++)
		{
			pixel = scanline[ix+dx];

			R = (pixel>>16);
			G = ((pixel & 0xff00)>>8);
			B = (pixel & 0xff);

			a = A >> abs(dx);
			R = (R * (255 - a) + a * 0x7f) >> 8;
			G = (G * (255 - a) + a * 0x7f) >> 8;
			B = (B * (255 - a) + a * 0x7f) >> 8;
			scanline[ix+dx] = ((R   )<<16) + ((G   )<<8) + (B);
		}*/

		pixel = scanline[ix-2];

		R = ((pixel & 0xff0000)>>16);
		G = ((pixel & 0xff00)>>8);
		B = (pixel & 0xff);

		a = A >> 2;
		R = (R * (255 - a) + a * 0x7f) >> 8;
		G = (G * (255 - a) + a * 0x8f) >> 8;
		B = (B * (255 - a) + a * 0xaf) >> 8;
		scanline[ix-2] = ((R   )<<16) + ((G   )<<8) + (B);

		pixel = scanline[ix-1];

		R = ((pixel & 0xff0000)>>16);
		G = ((pixel & 0xff00)>>8);
		B = (pixel & 0xff);

		a = A*3 >> 2;
		R = (R * (255 - a) + a * 0x7f) >> 8;
		G = (G * (255 - a) + a * 0x8f) >> 8;
		B = (B * (255 - a) + a * 0xaf) >> 8;
		scanline[ix-1] = ((R   )<<16) + ((G   )<<8) + (B);

		pixel = scanline[ix];

		R = ((pixel & 0xff0000)>>16);
		G = ((pixel & 0xff00)>>8);
		B = (pixel & 0xff);

		a = A;
		R = (R * (255 - a) + a * 0x7f) >> 8;
		G = (G * (255 - a) + a * 0x8f) >> 8;
		B = (B * (255 - a) + a * 0xaf) >> 8;
		scanline[ix] = ((R   )<<16) + ((G   )<<8) + (B);

		pixel = scanline[ix+1];

		R = ((pixel & 0xff0000)>>16);
		G = ((pixel & 0xff00)>>8);
		B = (pixel & 0xff);

		a = A*3 >> 2;
		R = (R * (255 - a) + a * 0x7f) >> 8;
		G = (G * (255 - a) + a * 0x8f) >> 8;
		B = (B * (255 - a) + a * 0xaf) >> 8;
		scanline[ix+1] = ((R   )<<16) + ((G   )<<8) + (B);

		pixel = scanline[ix+2];

		R = ((pixel & 0xff0000)>>16);
		G = ((pixel & 0xff00)>>8);
		B = (pixel & 0xff);

		a = A >> 2;
		R = (R * (255 - a) + a * 0x7f) >> 8;
		G = (G * (255 - a) + a * 0x8f) >> 8;
		B = (B * (255 - a) + a * 0xaf) >> 8;
		scanline[ix+2] = ((R   )<<16) + ((G   )<<8) + (B);

		//scanline[ix-2] = 0x7f7f7f;
		//scanline[ix-1] = 0x7f7f7f;
		//scanline[ix  ] = 0x7f7f7f;
		//scanline[ix+1] = 0x7f7f7f;
		//scanline[ix+2] = 0x7f7f7f;
		
		lt += rdy;
		x += dxdy;
		scanline += j;
	}
}

static Vector 
	TexOrigin(-1.0, 1.5, 6.0), 
	TexU(3.0, 0.0,-1.0), 
	TexV(-1.0,-3.0, 0.0);

static Vector TexNorm;
static float TexOffs;
static float TexDxy;
static float MaxVolDepth = 4.0f;

void VectorGetUV(Vector *V, float &u, float &v)
{
	Vector P;
	Vector_LComb(V, &TexNorm, 1.0, -Dot_Product(V,&TexNorm), &P);

	float a, b;

	u = (P.x * TexV.y - P.y * TexV.x) * TexDxy;
	v = (TexU.x * P.y - TexU.y * P.x) * TexDxy;
}

void PointGetUV(Vector *V, float &u, float &v)
{
	Vector P;
	Vector_Sub(V, &TexOrigin, &P);
	VectorGetUV(&P, u, v);
}

dword TexGetColor(float u, float v)
{
	int32_t U = u*16.0;
	int32_t V = v*16.0;
	int32_t U2 = u*256.0;
	int32_t V2 = v*256.0;
	return ((U^V)&1)*0xFF0000 + (U2<<8) + V2;
}

void VolTrace(Image &Im)
{

	mword i, j;
	float x, y;
	float dx = 2.0 / (float)Im.x, dy = 2.0 / (float)Im.y;

	Cross_Product(&TexU, &TexV, &TexNorm);
	Vector_Norm(&TexNorm);

	TexOffs = Dot_Product(&TexOrigin, &TexNorm);

	TexDxy = 1.0 / (TexU.x * TexV.y - TexU.y * TexV.x);

	// initial height over texture plane
	float hOrig =-TexOffs;

	float UOrig, VOrig;
	float UDir, VDir;

	// calculate UOrig, VOrig
	Vector zero(0,0,0);
	PointGetUV(&zero, UOrig, VOrig);


	for(j=0,y=1.0; j<Im.y; j++, y-=dy)
	{
		for(i=0,x=-1.0; i<Im.x; i++, x+=dx)
		{
			Vector V;
			V.x = x;
			V.y = y;
			V.z = 1.0;
			Vector_Norm(&V);

			// (du, dv) along ray
			float rdu, rdv;
			VectorGetUV(&V, rdu, rdv);

			// delta-height over plane along ray
			float rdh = Dot_Product(&V, &TexNorm);			

			float scanres = 1.0/(256*sqrt(rdu*rdu + rdv*rdv)); // scanning resolution
			if (scanres > 1.0/64.0) scanres = 1.0/64.0;

			// interval to clip
			float t, t0, t1;
			t0 = 0.0;
			t1 = 1E+05;

			// vol. Clipping
			float i_rdh = 1.0/rdh;
			if (rdh<0)
			{			
				t = -hOrig*i_rdh;
				if (t1>t) t1 = t;

				t = (MaxVolDepth-hOrig)*i_rdh;
				if (t0<t) t0 = t;
			} else {
				t = (MaxVolDepth-hOrig)*i_rdh;
				if (t1>t) t1 = t;

				// initial height cannot be negative
//				t = -hOrig*i_rdh;
//				if (t0<t) t0 = t;
			}
			if (t0>=t1) continue;
			
			if (y<0)
			{
				int banana = 1;
			}
			float i_rdu = 1.0/rdu;
			if (rdu<0)
			{			
				t = -UOrig*i_rdu;
				if (t1>t) t1 = t;

				t = (1.0-UOrig)*i_rdu;
				if (t0<t) t0 = t;
			} else {
				t = (1.0-UOrig)*i_rdu;
				if (t1>t) t1 = t;

				t = -UOrig*i_rdu;
				if (t0<t) t0 = t;
			}
			if (t0>=t1) continue;
			float i_rdv = 1.0/rdv;
			if (rdv<0)
			{			
				t = -VOrig*i_rdv;
				if (t1>t) t1 = t;

				t = (1.0-VOrig)*i_rdv;
				if (t0<t) t0 = t;
			} else {
				t = (1.0-VOrig)*i_rdv;
				if (t1>t) t1 = t;

				t = -VOrig*i_rdv;
				if (t0<t) t0 = t;
			}
			if (t0>=t1) continue;
			
			float u = UOrig + t0 * rdu;
			float v = VOrig + t0 * rdv;
			float h = hOrig + t0 * rdh;

			float tex_r, tex_g, tex_b;
			float res_r = 0.0, res_g = 0.0, res_b = 0.0;
			dword TexColor;

			/*if (fabs(rdu) > fabs(rdv))
			{
				if (rdu>0.0)
				{					
				} else {
				}
			} else {
				if (rdv>0.0)
				{
				} else {

				}
			}*/
			for(t=t0; t<t1; t+=scanres)
			{
				TexColor = TexGetColor(u, v);

				tex_r = ((TexColor&0xFF0000)>>16);
				tex_g = ((TexColor&0x00FF00)>>8);
				tex_b = (TexColor&0xFF);

				float lumin = 1.0*exp(-h*0.3);
				if (lumin > 1.0) lumin = 1.0;

				float ls = lumin * scanres;
				res_r += lumin * tex_r * ls;
				res_g += lumin * tex_g * ls;
				res_b += lumin * tex_b * ls;

				u += rdu * scanres;
				v += rdv * scanres;
				h += rdh * scanres;
			}
			int32_t res_R = res_r; if (res_R>255) res_R = 255;
			int32_t res_G = res_g; if (res_G>255) res_G = 255;
			int32_t res_B = res_b; if (res_B>255) res_B = 255;
			Im.Data[i+j*Im.x] = (res_R<<16)+(res_G<<8)+res_B;
		}
	}
}

struct ThreadPoolCleanup {
	~ThreadPoolCleanup() {
		ThreadPool::instance().close();
	}
};

template <typename F>
struct scope_guard {
	scope_guard(F &&f): f(std::forward<F>(f)) {}
	~scope_guard() {f();}
	F f;
};

void CodeEntry(void *var)
{
	// Demo thread does the per-frame timing work; on Apple Silicon we
	// want it on a P-core, not an E-core. No-op on wasm/Linux.
	HintHighPerfThread();

	Generate_RGBFlares(); // should be in fds_init

	//if (g_playMusic)
	//	FModInit();

	// device init parameters
	// g_DIP.hInst = G_hInst;
	// g_DIP.hWnd = G_hWnd;
	// g_DIP.x = internalXRes;
	// g_DIP.y = internalYRes;
	// g_DIP.bpp = 32;
	// g_DIP.flags = 0;

	// if (g_fullScreenMode)
	// 	g_DIP.flags |= DISP_FULLSCREEN;

	// request display initialization
	// Thread_Interlock_Mutex = 1;
	// PostMessage(G_hWnd,WM_USER+1,0,0); //DEVICE INIT SIGNAL  
	// while (Thread_Interlock_Mutex)
	// {		
	// 	Sleep(10);
	// }
	// direct display initialization
//	InitDisplayDevice(cfg);
	InitPolyStats(200);

	//TestCoff();

	//dword sh = 0;
	ModplayerHandle sh = 0;
	fprintf(stderr, "[DEMO] g_playMusic=%u\n", (unsigned)g_playMusic);
	if (g_playMusic)
	{
		//sh = FModLoadModule("Revival.XM");
		//module.Load("Revival.xM");
		fprintf(stderr, "[DEMO] before Modplayer_Create\n");
		sh = Modplayer_Create("REVIVAL.XM");
		fprintf(stderr, "[DEMO] after Modplayer_Create sh=%p\n", sh);
		// We don't render a visualizer; skip the per-buffer FFT + scope copy.
		if (sh) Modplayer_SetDisplay(sh, false);
	}
	g_RevModuleHandle = sh;

	ThreadPool::instance().init([]() {
		InitPolyStats(200);
		// _control87(_PC_24 | _RC_UP, _MCW_PC | _MCW_RC);
		FPU_LPrecision();
	});
	ThreadPoolCleanup _guard;
	scope_guard guard([](){
		SDL_QuitEvent q = {SDL_QUIT, SDL_GetTicks()};
		int ecode = SDL_PushEvent((SDL_Event*)&q);
		//std::cerr << "push event: " << ecode << std::endl;
	});

	FPU_LPrecision();
	if (g_playMusic)
	{
		fprintf(stderr, "[DEMO] before Modplayer_Start\n");
		if (sh) Modplayer_Start(sh);
		fprintf(stderr, "[DEMO] after Modplayer_Start\n");
#ifdef __EMSCRIPTEN__
		if (sh) SDL2_StartMusic(sh);
#endif
	}
	Timer = 0;

	// Timer = 0;
	// FillerTest();
	// return;

	Timer = 0;
	// RT rt;

	// while (!Keyboard[ScESC]) 
	// {
	// 	rt.RunFrame(((float)Timer)/100.0);
	// }

	// return;

	Initialize_Glato(); //9.3
	//Initialize_Nova();

	std::thread t1([]() {
		InitPolyStats(200);
		// _control87(_PC_24 | _RC_UP, _MCW_PC | _MCW_RC);
		FPU_LPrecision();

		// Greets first so its slow lightmap bake spawns early — the
		// bake itself runs on its own thread inside Initialize_Greets
		// and parallelizes with the remaining inits + glat playback.
		// Reordering instead of running on a separate thread because
		// LoadFLD reads through a global FILE* (FLD_READ.CPP:25); two
		// concurrent LoadFLD calls would race + corrupt the version
		// check and exit(1).
		Initialize_Greets(); //5.6
		Initialize_City();
		Initialize_Chase();
		Initialize_Fountain(); //8.3
		Initialize_Crash();
	});// (); //9.3

//	Initialize_Koch();
//	ImageCompressionTestCode();
//	TestPhotonTracer();

	if (g_playMusic)
	{
		//if (sh) FModPlayModule(sh);
		if (sh) Modplayer_Start(sh);
		//module.Start();
	}
	Timer = 0;

	bool skipToFountain = fds::FeatureFlags::skip_to_fountain();
	// Single-scene dispatch: if the user passed --scene-mirrortest,
	// skip the whole demo sequence and run only that scene. Useful
	// for poking at the mirror system with free-cam.
	if (fds::FeatureFlags::scene_cloaktest()) {
		extern void Run_CloakTest();
		if (t1.joinable()) t1.join();   // same FList-realloc race as mirrortest
		Run_CloakTest();
		goto demo_exit;
	}
	if (fds::FeatureFlags::scene_mirrortest()) {
		extern void Run_MirrorTest();
		// Join the init thread BEFORE running the mirror test scene.
		// Initialize_Greets / _City / etc each call
		// fds::g_mainFaces.resize(...), which reallocates the global
		// FList storage. If the mirror-test scene's tick is rendering
		// against that vector concurrently, the realloc frees its
		// FListEntry array out from under the active render — ASan
		// flagged it as heap-use-after-free in RenderInner.cpp:76.
		if (t1.joinable()) t1.join();
		Run_MirrorTest();
		goto demo_exit;
	}
	if (fds::FeatureFlags::scene_greets()) {
		// Same join-first ordering as scene_mirrortest — Initialize_Greets
		// must finish before Run_Greets's render loop touches g_mainFaces.
		if (t1.joinable()) t1.join();
		Run_Greets();
		goto demo_exit;
	}
	if (fds::FeatureFlags::scene_conetest()) {
		// Interactive volumetric-cone / fog / in-scatter test rig. Join the
		// init thread first (same g_mainFaces realloc race as the others).
		extern void Run_ConeTest();
		if (t1.joinable()) t1.join();
		Run_ConeTest();
		goto demo_exit;
	}
	if (fds::FeatureFlags::scene_displacetest()) {
		// Minimal stone-displacement bake rig (one quad + a height map). Join
		// the init thread first (same g_mainFaces realloc race as the others).
		extern void Run_DisplaceTest();
		if (t1.joinable()) t1.join();
		Run_DisplaceTest();
		goto demo_exit;
	}
	if (fds::FeatureFlags::scene_chase()) {
		// Chase-only review mode (loop alignment / light-drama judging).
		// Join-first for the same g_mainFaces realloc race as the others.
		// Music (started above when enabled) plays from song start — fine
		// for look review. --chase-start=T warps the clock so the loop
		// (t~500) or the island stretch (t~1200) is reachable immediately;
		// a cold jump skips pre-T accumulated state (bolts/events), which
		// is acceptable here and touches nothing outside this branch.
		if (t1.joinable()) t1.join();
		if (fds::FeatureFlags::chase_start() > 0)
			Timer = fds::FeatureFlags::chase_start();
		Run_Chase();
		goto demo_exit;
	}
	// FDS_SKIP_INTRO=1 (dev/debug only): skip the Glato intro + its
	// wall-clock Timer<4000 gate and jump straight to City. Used to make
	// the per-frame city-texture-oscillation repro captures cheap (the
	// intro is ~150s of wall time). No effect on a normal run. Inlined
	// (no local) so it doesn't cross the earlier goto demo_exit jumps.
	if (!skipToFountain && std::getenv("FDS_SKIP_INTRO") == nullptr) {
	Run_Glato();

	t1.join();

	while (Timer<4000) { if (g_shouldQuit.load()) goto demo_exit; }
	} else {
	t1.join();
	}


	Timer = 0;
	if (Timer<=0) Timer = 1;
	if (g_shouldQuit.load()) goto demo_exit;
	if (!skipToFountain) Run_City();
//	SavePolyStats("City.stat");
//
	if (Timer <= 0) Timer = 1;
	if (g_shouldQuit.load()) goto demo_exit;
	if (!skipToFountain) Run_Chase();

	if (Timer<=0) Timer = 1;
	if (g_shouldQuit.load()) goto demo_exit;
	Run_Fountain();

	if (Timer <= 0) Timer = 1;
	Timer = 1;
	if (g_shouldQuit.load()) goto demo_exit;
	Run_Crash();
//
//	SavePolyStats("Fountain.stat");
	if (Timer<=0) Timer = 1;
	if (g_shouldQuit.load()) goto demo_exit;
	Run_Greets();
demo_exit:;
	// Join the greets lightmap bake thread before global destructors fire.
	// If we got here via SDL_QUIT/ESC before Run_Greets joined it, the
	// static std::thread is still joinable and ~thread() would call
	// std::terminate at process exit.
	Greets_ShutdownBakeThread();
//

	//	SavePolyStats("Greets.stat");
//	Run_RainFX();
//	Run_Koch();
//	Run_Nova();

	if (g_playMusic)
	{
#ifdef __EMSCRIPTEN__
		SDL2_StopMusic();
#endif
		//if (sh) FModFreeModule(sh);
		//FModClose();
		//module.Stop();
		if (sh) Modplayer_Stop(sh);
	}
}

int main(int argc, const char *argv[])
{
	ChdirToAssetRoot(argc > 0 ? argv[0] : "");

	// Headless modes (--snapshot / --bench) must never pop an SDL window,
	// steal desktop focus, or play music when run in the background (the byte
	// gate, perf benches, agents). Detect them up front so we can (a) force
	// SDL's built-in dummy video+audio drivers BEFORE any SDL_Init and (b)
	// hard-disable music below regardless of rev.cfg. The snapshot path never
	// touches SDL anyway, but the raster/scene/flip bench opens a real window —
	// dummy makes it offscreen. Interactive runs (no --snapshot/--bench) are
	// untouched and still open a real window with audio. We match the
	// --snapshot / --bench *prefix*, which is a superset of the --snapshot= /
	// --bench= forms the arg parsers accept, so every headless invocation is
	// covered.
	bool headless = false;
	for (int i = 1; i < argc; ++i) {
		if (argv[i] && (std::strncmp(argv[i], "--snapshot", 10) == 0 ||
		                std::strncmp(argv[i], "--bench",    7) == 0)) {
			headless = true;
			break;
		}
	}
	if (headless) {
		setenv("SDL_VIDEODRIVER", "dummy", 1);
		setenv("SDL_AUDIODRIVER", "dummy", 1);
	}
	// --material-import=MAT:DIR specs aren't FeatureFlags; pre-scan argv (like
	// --snapshot above). Scenes apply them at init via MaterialImport_Apply.
	fds::MaterialImport_ParseArgs(argc, argv);
	ConfigurationDB cfg;
	g_cfg = &cfg;
	cfg.fromFile("rev.cfg");
	g_playMusic = cfg.extractInteger("MusicEnable");
	// Headless snapshot/bench runs return before CodeEntry (where the music
	// thread is created), so music never starts today — but make the contract
	// explicit so a future earlier music-init can't sneak audio into a
	// background byte-gate / bench run.
	if (headless) g_playMusic = 0;
	// rev.cfg seeds the runtime `profiler` flag (left UNMARKED, so scene
	// scripts and the tune console can toggle the overlay live). An
	// explicit --profiler / --no-profiler wins over the cfg.
	if (cfg.extractInteger("ProfilerEnable") &&
	    !fds::FeatureFlags::isSet(fds::FeatureFlags::BoolId::profiler)) {
		fds::FeatureFlags::setParamFromText("profiler", "1");
		fds::FeatureFlags::clearSetMark("profiler");
	}
	g_profilerActive = fds::FeatureFlags::profiler();
	g_demoXRes = cfg.extractInteger("ResolutionX");
	g_demoYRes = cfg.extractInteger("ResolutionY");
	g_fullScreenMode = cfg.extractInteger("FullScreenMode");
	// HiDPI=1 in rev.cfg adds SDL_WINDOW_ALLOW_HIGHDPI so we render at the
	// physical backing-store size on retina displays (sharper, ~4× pixel
	// load on 2× scale). Default 0 keeps the cheap point-sized render.
	g_hiDPI = cfg.extractInteger("HiDPI");

	bool helpRequested = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--help") == 0 ||
		    std::strcmp(argv[i], "-h")     == 0) {
			helpRequested = true;
			break;
		}
		// `--print-completion[=bash|=zsh]` emits a shell completion script
		// to stdout. The intent is `source <(./DEMO --print-completion)`
		// once per shell session, after which `./DEMO --<TAB>` works.
		if (std::strncmp(argv[i], "--print-completion", 18) == 0) {
			const char *eq = std::strchr(argv[i], '=');
			fds::FeatureFlags::printCompletion(stdout, eq ? eq + 1 : "bash");
			return 0;
		}
	}
	if (helpRequested) {
		std::printf(
"REVIVAL / FLOOD demo\n"
"\n"
"USAGE\n"
"  ./DEMO                                   run the demo interactively\n"
"  ./DEMO --snapshot=<scene>[@t=T1,T2,...] [--out=DIR]\n"
"  ./DEMO --bench=<kind>[@iters=N,seed=N,...]\n"
"  ./DEMO [--<flag> | --no-<flag> | --<flag>=VALUE]...\n"
"  ./DEMO --help | -h                       this message\n"
"  ./DEMO --print-completion[=bash|zsh]     emit shell completion script\n"
"\n"
"SNAPSHOT SCENES (headless deterministic PPM dump, no SDL window)\n"
"  city           City scene driver at specified Timer values\n"
"  fountain       Fountain scene (auto-inits City first for SkyCube)\n"
"  greets         Greets scene at specified Timer values\n"
"  refltest       City reflection direction diagnostic (forces panorama)\n"
"  cuberefl       Single reflective cube, no city geometry\n"
"  seaside       \"Looking back at the coast\" reflection bug repro\n"
"  xpartest@t=N   Transparent test cases: 1 single panel, 2 z-occlusion,\n"
"                 3 multi-layer, 4 glass cube\n"
"  spectest@t=N   Specular lobe test: case 1=gloss4, 2=gloss32, 3=gloss128\n"
"  conetest       Volumetric-cone test: spot + ground, multi-pose snapshots\n"
"                 (requires --deferred --draw_cones)\n"
"  lightmaptest@t=N1,N2,...  Shadow-lightmap reproducer: ground subdivided\n"
"                 NxN, occluder above, static omni. Subdiv=1 is 2 big\n"
"                 triangles; subdiv=64 is 8192 small ones. Run twice and\n"
"                 diff: once without --shadow-lightmap (cube tap), once\n"
"                 with (atlas lookup).\n"
"  filler         Rasterizer edge / mask fixture\n"
"  glat-trace     Glat scene Timer sweep -> CSV\n"
"\n"
"BENCH KINDS\n"
"  raster         FillerTest synthetic raster, ms/iter + Mpx/s\n"
"  scene@scene=NAME,t=T,iters=N   Drive a real scene's tick() N times\n"
"  flip           SDL present-path microbench\n"
"  greets-pipe    Greets dynamic-text-texture pipeline harness\n"
"\n"
"INTERACTIVE KEYS\n"
"  ESC                exit demo immediately\n"
"  Backspace          skip to next scene\n"
"  J                  toggle hide fountain inner sphere (\"f in shpere\")\n"
"  K                  toggle hide fountain outer sphere (\"f_sphere\")\n"
"  I                  print camera pos + fwd to stderr (city / greets)\n"
"  O                  dump active omnis to stderr (fountain)\n"
"  C                  capture current view to free-cam FC slot\n"
"  W / A / S / D      free-cam strafe (when free-cam enabled)\n"
"  Q / E              free-cam zoom\n"
"  P                  pause Timer (city / greets / fountain)\n"
"  U                  reverse Timer flow (city / greets / fountain)\n"
"  V                  cycle shadow-map debug thumbnail (off -> sm0 -> sm1 -> ...)\n"
"\n"
"COMMON RECIPES\n"
"  Deferred snapshot of greets:\n"
"    ./DEMO --deferred --snapshot=greets --out=/tmp/g\n"
"  A/B normal-map effect:\n"
"    ./DEMO --deferred --snapshot=greets --out=/tmp/with\n"
"    ./DEMO --deferred --no-nmap --snapshot=greets --out=/tmp/without\n"
"  Glass cube test, deferred 2-deep:\n"
"    ./DEMO --deferred --snapshot=xpartest@t=4 --out=/tmp/cube\n"
"\n"
"SNAPSHOT / TEST ENV (not feature flags)\n"
"  FNTSNAP_POS=X,Y,Z           Pin fountain snapshot camera position\n"
"  FNTSNAP_FWD=X,Y,Z           Pin fountain snapshot camera forward vector\n"
"  FNTSNAP_FOV=F               Pin fountain snapshot field-of-view\n"
"  XPARTEST_TRI_COUNT=N        Limit cube-glass test to first N sorted tris\n"
"\n"
		);
		fds::FeatureFlags::printHelp(stdout);
		return 0;
	}

	// Consume feature-flag args (--flag / --no-flag / --flag=value). Unknown
	// flags fall through to snapshot/bench parsing; those parsers ignore
	// anything they don't recognise.
	fds::FeatureFlags::parseArgs(argc, argv);

	// Flag implication: city_env_pixel routes the city glass through the
	// deferred env-specular compose, which only has data when env_refl's
	// FramePrep bakes ran — without it the glass is flat (loses even the
	// legacy sheet reflections). Default env_refl ON in that case; an
	// explicit --no-env-refl still wins (setDefault never overrides a
	// user-set flag).
	if (fds::FeatureFlags::city_env_pixel() && !fds::FeatureFlags::env_refl()) {
		fds::FeatureFlags::setDefault(fds::FeatureFlags::BoolId::env_refl, true);
		if (fds::FeatureFlags::env_refl())
			std::fprintf(stderr, "[FLAGS] city_env_pixel: enabling env_refl "
			             "(required for the per-pixel glass compose)\n");
	}

	// Env-cube convention self-test (FDS_ENV_CUBE_SELFTEST=1): dir→face/uv→dir
	// round-trip + central uv→dir→uv check. Aborts on gross failure so an
	// orientation bug is caught before any real bake. No-op otherwise.
	if (std::getenv("FDS_ENV_CUBE_SELFTEST")) {
		if (!fds::EnvCube_SelfTest()) {
			std::fprintf(stderr, "[ENVCUBE-SELFTEST] FAILED — aborting.\n");
			return 3;
		}
	}

	fds::TuneServer_Start();

	SnapshotConfig snap;
	if (ParseSnapshotArgs(argc, argv, snap)) {
		// Headless deterministic dump path — no SDL window, no music.
		// Used to diff native vs wasm at the pixel level / per-tick state.
		// Integer scene clock only: captures must be a pure function of the
		// pinned Timer, not of wall-clock fractions (see g_FrameTimeF).
		g_fineSceneClock = false;
		// Pin dumps must stay a pure function of Timer: the prev-frame chunk
		// occlusion cull (ChunkOcclusion.h) would make ticks 2..N of a multi-t
		// recipe depend on the PREVIOUS timestamp's depth — recipe-fragile
		// pins would move. Force it inert for every snapshot scene; the bench
		// harness (timing + verify sweep) keeps it live. --chunk_occl_snapshot_force
		// ([dev]) re-enables it for the temporal-pop ground-truth pixel rig.
		fds::g_occlSnapshotInert = !fds::FeatureFlags::chunk_occl_snapshot_force();
		// --snapshot_ss (DIAGNOSTIC, default 1): render the snapshot at N times
		// the configured resolution per axis. Used to build a SUPERSAMPLED
		// converged reference for the displacement inventory
		// (docs/S1_DISCREPANCY_INVENTORY.md) — the arms under test always render
		// at 1x, and the 4x reference is box-downsampled offline. Snapshot path
		// only; it never touches the live window's resolution.
		const int ssN = fds::FeatureFlags::snapshot_ss();
		if (ssN > 1) {
			g_demoXRes *= ssN;
			g_demoYRes *= ssN;
			std::fprintf(stderr, "[SNAPSHOT] --snapshot_ss=%d -> %dx%d\n",
			             ssN, (int)g_demoXRes, (int)g_demoYRes);
		}
		if (snap.scene == "city") {
			return RunCitySnapshot(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "glat-trace") {
			return RunGlatTrace(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "filler") {
			return RunFillerTestSnapshot(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "refltest") {
			return RunReflectionTest(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "cuberefl") {
			return RunCubeReflTest(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "seaside") {
			return RunCitySeasideTest(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "fountain") {
			return RunFountainSnapshot(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "greets") {
			return RunGreetsSnapshot(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "crash") {
			return RunCrashSnapshot(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "pbrtest") {
			return RunPBRTestSnapshot(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "chase") {
			return RunChaseSnapshot(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "xpartest") {
			return RunXparTest(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "spectest") {
			return RunSpecTest(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "conetest") {
			return RunConeTest(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "halotest") {
			return RunHaloTest(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "lightmaptest") {
			return RunLightmapTest(snap, g_demoXRes, g_demoYRes);
		}
		if (snap.scene == "mirrortest") {
			return RunMirrorTest(snap, g_demoXRes, g_demoYRes);
		}
		fprintf(stderr, "[SNAPSHOT] unknown scene '%s' (try city, glat-trace, filler, refltest, cuberefl, seaside, fountain, greets, xpartest, spectest, conetest, halotest, lightmaptest, mirrortest)\n",
		        snap.scene.c_str());
		return 2;
	}

	BenchConfig bench;
	if (ParseBenchArgs(argc, argv, bench)) {
		g_fineSceneClock = false;   // deterministic clock (see snapshot path)
		// Headless rasterizer benchmark — no SDL window, no music.
		// Same flow as the snapshot harness but the inner loop times instead
		// of dumping. Run under `node --cpu-prof DEMO_snapshot.js --bench=...`
		// for an automatic V8 cpuprofile next to the output.
		if (bench.kind == "raster") {
			return RunRasterBench(bench, g_demoXRes, g_demoYRes);
		}
		if (bench.kind == "scene") {
			return RunSceneBench(bench, g_demoXRes, g_demoYRes);
		}
		if (bench.kind == "flip") {
			return RunFlipBench(bench, g_demoXRes, g_demoYRes);
		}
		if (bench.kind == "greets-pipe") {
			return RunGreetsPipeBench(bench, g_demoXRes, g_demoYRes);
		}
		fprintf(stderr, "[BENCH] unknown kind '%s' (try raster, scene)\n",
		        bench.kind.c_str());
		return 2;
	}

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    // Ctrl-C handling. SDL2 installs its own SIGINT handler in some builds,
    // but on this macOS configuration we observed SIGINT being silently
    // swallowed (process kept running, SDL_WaitEvent never returned an
    // SDL_QUIT). Install our own handler that flips g_shouldQuit and pushes
    // a user-event to wake SDL_WaitEvent. The handler must be async-signal
    // safe — std::atomic<bool>::store with relaxed order is, and
    // SDL_PushEvent is documented as thread-safe (it takes an internal
    // mutex; that's not strictly signal-safe but the SDL community ships
    // demos doing exactly this and it works in practice on POSIX).
    struct SigCtl {
        static void handler(int) {
            g_shouldQuit.store(true, std::memory_order_relaxed);
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = SDL_QUIT;
            SDL_PushEvent(&ev);
        }
    };
    std::signal(SIGINT, &SigCtl::handler);
    std::signal(SIGTERM, &SigCtl::handler);

    Uint32 windowFlags = SDL_WINDOW_RESIZABLE;
#ifndef __EMSCRIPTEN__
    // On native (macOS retina, Windows DPI scaling, Linux fractional
    // scaling) we honor the rev.cfg HiDPI flag by asking SDL for a
    // backing-store-sized renderer. On emscripten the canvas backing
    // size is owned by the JS shell (devicePixelRatio when ?hidpi=1 in
    // the URL); SDL_WINDOW_ALLOW_HIGHDPI there causes emscripten's SDL2
    // port to silently multiply canvas dims by devicePixelRatio on top
    // of what the JS shell already set, which doubles the effective
    // resolution and torpedoes framerate.
    if (g_hiDPI) windowFlags |= SDL_WINDOW_ALLOW_HIGHDPI;
#endif
    auto windowDeleter = [](SDL_Window *w) {
        if (!w) return;
        fprintf(stderr, "[SDL] -window ptr=%p\n", (void *)w);
        SDL_DestroyWindow(w);
    };
    std::unique_ptr<SDL_Window, decltype(windowDeleter)> window(
        SDL_CreateWindow("",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            g_demoXRes, g_demoYRes,
            windowFlags),
        windowDeleter);
    fprintf(stderr, "[SDL] +window ptr=%p size=%dx%d\n",
            (void *)window.get(), g_demoXRes, g_demoYRes);

    SDL_RaiseWindow(window.get());

    // SDL2 starts with text input ACTIVE on macOS, routing key events through
    // the Cocoa text-input (NSTextInputClient) path — so press-and-hold on a
    // key (camera movement etc.) pops the macOS accent/glyph picker instead of
    // repeating. The demo reads raw scancodes (Keyboard[]) and never needs text
    // events, so stop text input: kills the accent popup, clean key repeats.
    // Harmless on other platforms.
    SDL_StopTextInput();

	// _controlfp(_EM_INEXACT, _MCW_EM);
	FPU_LPrecision();
	const int internalXRes = g_demoXRes;
	const int internalYRes = g_demoYRes;
	LogFile = fopen("Runtime.LOG","wt");

	if (!FDS_Init(internalXRes, internalYRes,32))    // Initialize FDS
	{
		return -1;
	}

	fprintf(stderr, "[DEMO] before SDL2_InitDisplay\n");
	SDL2_InitDisplay(window.get());
	fprintf(stderr, "[DEMO] after SDL2_InitDisplay, MainSurf=%p\n", (void*)MainSurf);

	memset(VPage, 0xcd, MainSurf->BPSL * YRes);
	fprintf(stderr, "[DEMO] memset VPage done\n");
	Flip(MainSurf);
	fprintf(stderr, "[DEMO] first flip done\n");

	TimerInit(100);
	fprintf(stderr, "[DEMO] TimerInit done\n");
	// Route stdout to logfile.txt; on Emscripten this goes to MEMFS and
	// keeps the browser console quiet (our diagnostics use stderr).
	freopen("logfile.txt", "w+", stdout);

#ifdef __EMSCRIPTEN__
	// Wasm: main() runs on the actual browser main thread. We can't block
	// here — that'd freeze the page. Instead, set up audio + ThreadPool +
	// modplayer synchronously (cheap), arm the user-gesture wait + spawn
	// the background init thread via DemoBoot, register the rAF callback,
	// and return. The runtime stays alive (EXIT_RUNTIME=0); DemoTick drives
	// scenes one frame at a time on browser main where WebGL2 actually
	// works (the OffscreenCanvas + commit_frame path from a pthread is
	// known-broken — emscripten#17816 / #23806).
	InitPolyStats(200);
	ThreadPool::instance().init([]() {
		InitPolyStats(200);
		FPU_LPrecision();
	});
	Generate_RGBFlares();
	ModplayerHandle sh = nullptr;
	if (g_playMusic) {
		sh = Modplayer_Create("REVIVAL.XM");
		if (sh) Modplayer_SetDisplay(sh, false);
		g_RevModuleHandle = sh;
	}
	Timer = 0;
	DemoBoot(sh);
	emscripten_set_main_loop(
		[](){
			if (!DemoTick()) {
				emscripten_cancel_main_loop();
			}
		},
		0,    // fps=0 -> use requestAnimationFrame
		false // don't simulate-infinite-loop; let main() return cleanly.
	);
	// Leak the SDL window: main() is about to return and EXIT_RUNTIME=0
	// keeps the runtime alive for rAF, but C++ destructors still fire on
	// return. The window/renderer/etc. need to outlive main(). The OS
	// reaps everything when the tab closes; emscripten_cancel_main_loop
	// in DemoTick:DONE handles graceful teardown.
	(void)window.release();
	return 0;
#endif

	fprintf(stderr, "[DEMO] spawning CodeEntry thread\n");
	auto demoThread = std::thread(CodeEntry, nullptr);
	fprintf(stderr, "[DEMO] CodeEntry thread spawned\n");

    while (1)
    {
        SDL_Event event;
        SDL_WaitEvent(&event);
		if (event.type == SDL_QUIT) {
			// Window close, OS shutdown, or SDL's default SIGINT/SIGTERM
			// handler (it posts SDL_QUIT for us). Tell the worker to bail
			// before we drop out of the event pump.
			g_shouldQuit.store(true);
			break;
		}
		if (event.type == SDL_WINDOWEVENT &&
		    (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
		     event.window.event == SDL_WINDOWEVENT_RESIZED)) {
			// event.window.data1/data2 are in window points (logical units);
			// the actual backing-store pixel size is what we render in. With
			// HiDPI off they're equal; with HiDPI on (retina) they differ by
			// the OS scale factor.
			int px = 0, py = 0;
			SDL_Renderer *r = MainSurf ? static_cast<SDL_Renderer *>(MainSurf->Renderer) : nullptr;
			if (r) {
				SDL_GetRendererOutputSize(r, &px, &py);
			} else {
				// Wasm path has no SDL renderer (we present via WebGL2 directly).
				// JS already pre-multiplies by devicePixelRatio when calling
				// SDL2_RequestSize, so data1/data2 are in physical pixels.
				px = event.window.data1;
				py = event.window.data2;
			}
			fprintf(stderr, "[RESIZE-EVT] kind=%d points=%dx%d pixels=%dx%d\n",
			        event.window.event, event.window.data1, event.window.data2, px, py);
			if (px > 0 && py > 0) {
				// Latest pending size wins; demo thread polls + applies at frame top.
				g_pendingResize.store(pack_size(px, py));
			}
		}
		if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
			// FDS_DEFS.H ScXxx constants are PS/2 set-1 scancodes inherited
			// from the original DOS build; SDL2 reports USB-HID scancodes,
			// which collide for almost everything. Only ScESC happens to
			// match (both = 41), which is why exit worked but pause/tab/etc.
			// did not. Translate so the legacy scene code keeps working.
			int legacy = -1;
			switch (event.key.keysym.scancode) {
				case SDL_SCANCODE_ESCAPE:    legacy = ScESC; break;
				case SDL_SCANCODE_BACKSPACE: legacy = ScBackSpace; break;
				case SDL_SCANCODE_F1:        legacy = ScF1; break;
				case SDL_SCANCODE_F2:        legacy = ScF2; break;
				case SDL_SCANCODE_F3:        legacy = ScF3; break;
				// F4 was missing here while F3 was present, so TimerProc's
				// review scrub only ran BACKWARD in the native build (the
				// wasm table in MainLoop.cpp had both). ScF4 has exactly one
				// consumer (TimerProc), so adding it resurrects nothing else.
				case SDL_SCANCODE_F4:        legacy = ScF4; break;
				case SDL_SCANCODE_F9:        legacy = ScF9; break;
				case SDL_SCANCODE_F11:       legacy = ScF11; break;
				case SDL_SCANCODE_TAB:       legacy = ScTab; break;
				case SDL_SCANCODE_SPACE:     legacy = ScSpace; break;
				case SDL_SCANCODE_LCTRL:
				case SDL_SCANCODE_RCTRL:     legacy = ScCtrl; break;
				case SDL_SCANCODE_LEFT:      legacy = ScLeft; break;
				case SDL_SCANCODE_RIGHT:     legacy = ScRight; break;
				case SDL_SCANCODE_UP:        legacy = ScUp; break;
				case SDL_SCANCODE_DOWN:      legacy = ScDown; break;
				case SDL_SCANCODE_HOME:      legacy = ScHome; break;
				case SDL_SCANCODE_END:       legacy = ScEnd; break;
				case SDL_SCANCODE_PAGEUP:    legacy = ScPgUp; break;
				case SDL_SCANCODE_PAGEDOWN:  legacy = ScPgDn; break;
				case SDL_SCANCODE_KP_MINUS:
				case SDL_SCANCODE_MINUS:     legacy = ScGrayMinus; break;
				case SDL_SCANCODE_KP_PLUS:
				case SDL_SCANCODE_EQUALS:    legacy = ScGrayPlus; break;
				case SDL_SCANCODE_LEFTBRACKET:  legacy = ScOpenSq; break;
				case SDL_SCANCODE_RIGHTBRACKET: legacy = ScCloseSq; break;
				case SDL_SCANCODE_COMMA:     legacy = ScComma; break;
				case SDL_SCANCODE_PERIOD:    legacy = ScPeriod; break;
				case SDL_SCANCODE_A:         legacy = ScA; break;
				case SDL_SCANCODE_C:         legacy = ScC; break;
				case SDL_SCANCODE_D:         legacy = ScD; break;
				case SDL_SCANCODE_E:         legacy = ScE; break;
				case SDL_SCANCODE_G:         legacy = ScG; break;
				case SDL_SCANCODE_H:         legacy = ScH; break;
				case SDL_SCANCODE_I:         legacy = ScI; break;
				case SDL_SCANCODE_J:         legacy = ScJ; break;
				case SDL_SCANCODE_K:         legacy = ScK; break;
				case SDL_SCANCODE_L:         legacy = ScL; break;
				case SDL_SCANCODE_O:         legacy = ScO; break;
				case SDL_SCANCODE_M:         legacy = ScM; break;
				case SDL_SCANCODE_N:         legacy = ScN; break;
				case SDL_SCANCODE_P:         legacy = ScP; break;
				case SDL_SCANCODE_Q:         legacy = ScQ; break;
				case SDL_SCANCODE_R:         legacy = ScR; break;
				case SDL_SCANCODE_S:         legacy = ScS; break;
				case SDL_SCANCODE_U:         legacy = ScU; break;
				case SDL_SCANCODE_W:         legacy = ScW; break;
				case SDL_SCANCODE_Y:         legacy = ScY; break;
				case SDL_SCANCODE_Z:         legacy = ScZ; break;
				default: break;
			}
			if (legacy >= 0) {
				Keyboard[legacy] = (event.type == SDL_KEYDOWN) ? 1 : 0;
			}
			// New key conventions (see CLAUDE.md / commit message):
			//   ESC       = exit demo immediately (whole program)
			//   Backspace = skip to next scene
			// We still set Keyboard[ScESC] above so legacy poll sites that
			// haven't been migrated keep getting an event, but the actual
			// "exit" trigger is g_shouldQuit so we don't conflate it with
			// "advance scene".
			if (event.type == SDL_KEYDOWN) {
				if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
					g_shouldQuit.store(true);
				} else if (event.key.keysym.scancode == SDL_SCANCODE_BACKSPACE) {
					g_skipScene.store(true);
				} else if (event.key.keysym.scancode == SDL_SCANCODE_J) {
					bool now = !g_HideInnerXpar.load();
					g_HideInnerXpar.store(now);
					std::fprintf(stderr, "[XPAR] inner (%s) %s\n",
						"f in shpere", now ? "HIDDEN" : "shown");
				} else if (event.key.keysym.scancode == SDL_SCANCODE_K) {
					bool now = !g_HideOuterXpar.load();
					g_HideOuterXpar.store(now);
					std::fprintf(stderr, "[XPAR] outer (%s) %s\n",
						"f_sphere", now ? "HIDDEN" : "shown");
				} else if (event.key.keysym.scancode == SDL_SCANCODE_V) {
					// Cycle the shadow-map viewer thumbnail through
					// every registered map. -1 hides; SDL2's V_Flip
					// paints the active one as a thumbnail before push.
					// Shift+V steps backward.
					const Uint16 mod = event.key.keysym.mod;
					const bool back = (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) != 0;
					extern void ShadowMap_ViewCycle();
					extern void ShadowMap_ViewCycleBack();
					if (back) ShadowMap_ViewCycleBack();
					else      ShadowMap_ViewCycle();
				} else if (event.key.keysym.scancode == SDL_SCANCODE_X) {
					// DEBUG-VIZ CYCLE: X steps forward through every viz that
					// can actually show something in this run, Shift+X steps
					// back. FDS/RENDER/VizCycle.cpp prints the menu + the mode
					// name, and paints the name bottom-left so a screenshot is
					// self-identifying.
					//
					// Why X and not V (the obvious letter): V is already the
					// shadow-map thumbnail cycle, ten lines up. X is the only
					// key untranslated in BOTH scancode tables (here and
					// MainLoop.cpp), absent from every Keyboard[] consumer in
					// DEMO/ and FDS/, and outside anyFreeCamKey's fly set — and
					// it sits next to Z/C so the left hand reaches it without
					// leaving WASD while the right hand keeps steering.
					// repeat==0: hold-to-repeat would spin the whole cycle.
					if (event.key.repeat == 0) {
						const Uint16 mod = event.key.keysym.mod;
						const bool back = (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) != 0;
						fds::VizCycle_Step(back ? -1 : +1);
					}
				} else if (event.key.keysym.scancode == SDL_SCANCODE_B) {
					// Cycle the shadow-map debug view's buffer mode:
					// static → dynamic → combined (closest-by-depth).
					// Applies to both thumbnail (V) and fullscreen (M).
					extern void ShadowMap_ViewModeCycle();
					ShadowMap_ViewModeCycle();
				} else if (event.key.keysym.scancode == SDL_SCANCODE_N) {
					// N: cycle force-miplevel through -1 (auto) → 0..7 → -1.
					// Shift+N: toggle the per-pixel mip-viz palette.
					// Both wired to the deferred kernel's per-pixel path
					// (see DeferredLighting.cpp; reads g_forceMipLevel /
					// g_vizMipLevel atomically once per tile).
					const Uint16 mod = event.key.keysym.mod;
					const bool shift = (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) != 0;
					if (shift) {
						const bool now = !g_vizMipLevel.load();
						g_vizMipLevel.store(now);
						std::fprintf(stderr, "[MIP-VIZ] %s "
						    "(mip0=red mip1=orange mip2=yellow mip3=green "
						    "mip4=blue mip5=indigo mip6=violet mip7=white)\n",
						    now ? "ON" : "off");
					} else {
						const int prev = g_forceMipLevel.load();
						const int next = (prev < 0) ? 0
						               : (prev >= 7 ? -1 : prev + 1);
						g_forceMipLevel.store(next);
						if (next < 0) std::fprintf(stderr, "[MIP-FORCE] AUTO (rasterizer chooses)\n");
						else          std::fprintf(stderr, "[MIP-FORCE] forced mip = %d\n", next);
					}
				} else if (event.key.keysym.scancode == SDL_SCANCODE_T) {
					// Toggle per-frame staleness tracker for the dynamic
					// cube buffers. When on, logs every 60 frames any
					// shadow maps whose polyId_dynamic hash hasn't changed
					// since the last check — those are the ones whose
					// dynamic bake isn't actually writing fresh content
					// (the "frozen at t=0" symptom).
					extern void ShadowMap_StalenessToggle();
					ShadowMap_StalenessToggle();
				}
			}
		}
    }

	// Belt-and-suspenders: if we got here via SDL_QUIT (Ctrl-C, window close,
	// OS shutdown) g_shouldQuit is already set by the event handler above. If
	// we ever get here via some other path (future code), make sure the demo
	// worker doesn't block us forever.
	g_shouldQuit.store(true);
	demoThread.join();
	// SDL_DestroyRenderer walks the renderer's child-texture list and
	// frees each. If any of those textures is in a corrupt state (the
	// post-greets OOB pattern points here), we want a "[SDL] -renderer"
	// trace right before the explosion.
	SDL_Renderer *renderer = (SDL_Renderer *)MainSurf->Renderer;
	fprintf(stderr, "[SDL] -renderer ptr=%p\n", (void *)renderer);
	SDL_DestroyRenderer(renderer);
	fprintf(stderr, "[SDL] -renderer done\n");
	// Drop the window before SDL_Quit — once SDL_Quit unloads the video
	// subsystem, SDL_DestroyWindow on a stale handle is undefined.
	window.reset();
	fprintf(stderr, "[SDL] SDL_Quit start\n");
	SDL_Quit();
	fprintf(stderr, "[SDL] SDL_Quit done\n");
	return 0;
}
