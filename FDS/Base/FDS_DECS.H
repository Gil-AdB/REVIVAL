#ifndef FDS_DECS_H_INCLUDED
#define FDS_DECS_H_INCLUDED
#include "FDS_VARS.H"
#include "Vector.h"
// FrameState.h pulls in CameraContext + FaceListContext + defines the
// convenience inline overload of Transform_Objects that routes through
// fds::g_mainCamera / fds::g_mainFaces. Including it here means every
// FDS_DECS.H consumer sees the simple `Transform_Objects(Sc)` form.
#include "FrameState.h"
#include <atomic>

// Math declarations

#ifndef MathIncluded
#define MathIncluded
void		Generate_SCTabs();
void		Init_Identities();

void		Vector_Form(Vector *V,float X,float Y,float Z);
Vector		Vector_Make(float X,float Y,float Z);
void		Vector_Zero(Vector *U);
void		Vector_Copy(Vector *U,Vector *V);
//void		Vector_Scale(const Vector *V,float S,Vector *W);

// Scales Vector V by S,stores result in W. W can overlap V.
template <typename V1, typename V2>
void Vector_Scale(const V1 *V, float S, V2 *W)
{
	W->x = V->x * S;
	W->y = V->y * S;
	W->z = V->z * S;
}

void		Vector_SelfScale(Vector *V,float S);
float	Vector_Length(Vector *V);
//float	Vector_SelfDot(Vector *V);
// Returns the Vector length of Vector V, squared.
// Equivelent of Dot_Product(V,V)
template <typename V1>
float Vector_SelfDot(V1 *V)
{
	return V->x * V->x + V->y * V->y + V->z * V->z;
}

void		Vector_Norm(Vector *V);
//void  Vector_Add(Vector *U,Vector *V,Vector *W);
//void		Vector_SelfAdd(Vector *U,Vector *V);
template <typename V1, typename V2>
void Vector_SelfAdd(V1 *U, V2 *V)
{
	U->x += V->x;
	U->y += V->y;
	U->z += V->z;
}

//void  Vector_Sub(Vector *U,Vector *V,Vector *W);
void		Vector_SelfSub(Vector *U,Vector *V);
void		Vector_SAdd(Vector *U,float S,Vector *V);
void		Vector_LComb(Vector *U,Vector *V,float A,float B,Vector *W);
void		Vector_LERP(Vector *U,Vector *V,float T,Vector *W);
//float	Dot_Product(Vector *U,Vector *V);
// Returns Dot Product of U and V.
template <typename V1, typename V2>
float Dot_Product(V1 *U, V2 *V)
{
	return U->x * V->x + U->y * V->y + U->z * V->z;
}

void		Cross_Product(Vector *U,Vector *V,Vector *W);
float	Vector_CosAngle(Vector *U,Vector *V);
float	Vector_Angle(Vector *U,Vector *V);
float	Distance(Vector *V,Vector *U);
void		Matrix_Form(Matrix M,float AA,float AB,float AC,float BA,float BB,float BC,float CA,float CB,float CC);
void		Matrix_Copy(Matrix A,Matrix B);
void		Matrix_Scale(Matrix M,float S);
float	Determinant(Matrix A);
void		Matrix_Transpose(Matrix A);
void		Matrix_Norm(Matrix A);
void		Matrix_InvScale(Matrix A);
void		Orthogonalize(Matrix A);
//void  MatrixXVector(Matrix M,Vector *U,Vector *V);
void		MatrixTXVector(Matrix M,Vector *U,Vector *V);
void		MatrixXMatrix(Matrix A,Matrix B,Matrix C);
void		MatrixTXMatrix(Matrix A,Matrix B,Matrix C);
void		Matrix_Identity(Matrix M);
void		Matrix_Rotation(Matrix Mat,float RX,float RY,float RZ);
void		Euler_Angles(Matrix Mat,float RX,float RY,float RZ);
float	Tri_Surface(Vector *A,Vector *B,Vector *C);
float	TetraHedron_Volume(Vector *A,Vector *B,Vector *C,Vector *D);
float	Trapped_Volume(Vector *A,Vector *B,Vector *C);
void		Triangle2Plane(Vector *A,Vector *B,Vector *C,Plane *P);
void		Normalize_Plane(Plane *P);
float	PointPlaneDistance(Vector *V,Plane *P);
void		DVertex2Line(Vector *V,Vector *W,Line *L);
void		Normalize_Line(Line *L);
float	PointLineDistance(Vector *V,Line *L);
float	LineLineDistance(Line *L,Line *M);
void		Vector4_Form(Vector4 *V,float X,float Y,float Z,float T);
void		Vector4_Zero(Vector4 *V);
void		Vector4_Copy(Vector4 *V,Vector4 *U);
void		Vector4_WNorm(Vector4 *V);
void		Matrix4_Form(Matrix4 M,float AA,float AB,float AC,float AD,float BA,float BB,float BC,float BD,float CA,float CB,float CC,float CD,float DA,float DB,float DC,float DD);
void		Matrix4_Copy(Matrix4 A,Matrix4 B);
void		Matrix4_Identity(Matrix4 M);
void		Matrix4_TransposeOut(Matrix4 A,Matrix4 B);
void		Matrix4_Transpose(Matrix4 A);
void		MatrixXVector4(Matrix4 M,Vector4 *U,Vector4 *V);
void		MatrixTXVector4(Matrix4 M,Vector4 *U,Vector4 *V);
void		MatrixXMatrix4(Matrix4 A,Matrix4 B,Matrix4 C);
void		MatrixTXMatrix4(Matrix4 A,Matrix4 B,Matrix4 C);
				
void		Complex_Form(Complex *Z,float X,float Y);
void		Complex_Adj(Complex *Z,Complex *W);
void		Complex2AngLen(Complex *Z,float *Ang,float *Len);
void		AngLen2Complex(Complex *Z,float Ang,float Len);
void		Complex_Add(Complex *Z,Complex *W,Complex *R);
void		Complex_Sub(Complex *Z,Complex *W,Complex *R);
void		Complex_Mul(Complex *Z,Complex *W,Complex *R);
void		Complex_Div(Complex *Z,Complex *W,Complex *R);
				
void		Quaternion_Form(Quaternion *Q,float X,float Y,float Z,float W);
void		Quaternion_Copy(Quaternion *Q,Quaternion *S);
void		Quaternion_Scale( Quaternion *Q,float S,Quaternion *P);
float	Quaternion_Length(Quaternion *Q);
void		Normalize_Quaternion(Quaternion *Q);
void		Quaternion_Unit(Quaternion *Q,Quaternion *Dest);
void		Quaternion_Negate(Quaternion *Q);
void		Quaternion_Exponent(Quaternion *Q, Quaternion *Dest);
void		Quaternion_Logarithm(Quaternion *Q, Quaternion *Dest);
void		Quaternion_Inverse(Quaternion *Q, Quaternion *Dest);
float	Quaternion_Dot_Product(Quaternion *Q1, Quaternion *Q2);
void		AngleAxis2Quaternion(Quaternion *Q,Quaternion *P);
void		Quaternion2AngleAxis(Quaternion *Q, Quaternion *A);
void		Quaternion_Mul(Quaternion *A,Quaternion *B,Quaternion *C);
void		Quaternion_Ln_Dif(Quaternion *P, Quaternion *Q, Quaternion *Dest);
void		Convert_Quat2Mat(Quaternion *Q,Matrix M);
void		Convert_Mat2Quat(Matrix M, Quaternion *Q);
				
void		Quaternion_SLERP(Quaternion *A,Quaternion *B, Quaternion *Dest, float Time,float Spin);
				
				
void		Quaternion_SLERP_V2(Quaternion *A,Quaternion *B, Quaternion *Dest, float Time,float Spin);
void		SLERP(Quaternion *Q,Quaternion *P,float T,Quaternion *R);
void		SQUAD(Quaternion *Q,Quaternion *A,Quaternion *B,Quaternion *P,float T,Quaternion *R);
				
void		Spline_SetKey_4D(Spline *S,Quaternion *Q,float Frame,float SpF[5]);
void		Spline_SetKey_3D(Spline *S,Vector *V, float Frame,float SpF[5]);
void		Spline_SetKey_1D(Spline *S,float X,float Frame,float SpF[5]);
				
float	BKS_Ease(float T,float A,float B);
				
void		Spline_Init_3D(Spline *S);
void		Spline_Init_4D(Spline *S);
void		Spline_Calc_1D(Spline *S,float Frame, float *Out);
void		Spline_Calc_3D(Spline *S,float Frame, Vector *Out);
void		Spline_Calc_4D_Alt(Spline *S,float Frame, Quaternion *Out);
void		Spline_Calc_4D(Spline *S,float Frame, Quaternion *Out);
void		Spline_Scale(Spline *S, float s);
				
void		Spline_Init_Bezier(Spline *S);
void		Spline_Subdivide_Bezier(Spline *S,float Frame, Quaternion *Out);
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _3DSIncluded
#define _3DSIncluded
char Load_3DS(Scene *Sc, char *FileName);
void Add_Scene_3DS(char *FileName);
#endif

#ifndef _FLDIncluded
#define _FLDIncluded
char LoadFLD(Scene *Sc, const char *FileName);
#endif


#ifndef DPMIIncluded
#define DPMIIncluded
void * D_New(dword Size);
void D_Del(void *Target);
void * D_Resize(void *Target,dword Size);
void DPMI_Init_Core();
void DPMI_Deinit_Core();
void DPMI_Init_MB(dword Amount);
void * DPMI_NewModule(Module_MB *Father,dword Size,char *Name);
void * DPMI_New(void *Father,dword Size,char *Name);
void * DPMI_Assisted_New(Module_MB *Core,void *Father,dword Size,char *Name);
void * DPMI_Resize(void *Target,dword Size);
void DPMI_Destroy(Module_MB *Target);
void DPMI_RecDestroy(Module_MB *Target);
void DPMI_Delete(void *Target);
void DPMI_Assisted_Delete(Module_MB *Core,void *Target);
extern Module_MB * DPMI_Get_MMB(void *Target);
extern dword DPMI_Get_Handle(void *Target);
void DPMI_Message(char *ErrMsg);
void DPMI_Warning(char *ErrMsg);
void DPMI_Error(char *ErrMsg);
extern dword DPMI_Free_Memory(void);
void DPMI_allocRealSeg(int size,int *sel,int *r_seg);
void DPMI_freeRealSeg(unsigned sel);
int DPMI_int86(int intno, RMREGS *in, RMREGS *out);
int DPMI_int86x(int intno, RMREGS *in, RMREGS *out, RMSREGS *sregs);
int DPMI_allocSelector(void);
void DPMI_setSelectorBase(int sel,dword linAddr);
void DPMI_setSelectorLimit(int sel,dword limit);
void Init_Aligned(int32_t Amount);
void * Get_Aligned_Block();
void Remove_Aligned_Block(void *Target);
void Deinit_Aligned();

void *DPMI_Map_P2L(dword physAddr,dword limit);
void DPMI_Del_P2L(void *Linear);
#endif

#ifndef VESAIncluded
#define VESAIncluded
void Initialize_VESA();
int VBE_Detect();
void VESA_FullInfo(int32_t RX,int32_t RY,char RBPP);
extern VESA_Surface * VESA_Alloc_Surface(int32_t RX,int32_t RY,int32_t RBPP,int32_t FOV,dword Flags);
char VESA_Init_Video(int32_t X,int32_t Y,int32_t BPP);
void Set_Screen(int32_t X,int32_t Y,int32_t BPP,char ZBuf,float FOV);
void VESA_Surface2Global(VESA_Surface *VS);
char Modesetup();
void End_Graphics();

//void Build_YOffs_Table(VESA_Surface *VS);

Font * LoadAFT(const char *FileName);
// `scale` < 0 -> use g_fontScale (HiDPI auto). Pass an explicit value
// (e.g. 1) when writing into a fixed-size off-screen buffer so that the
// glyph dimensions don't scale with screen DPI.
int32_t OutTextXY(byte *Where,int32_t X,int32_t Y, const char *S, uint8_t C, int xres = XRes, int yres = YRes, int scale = -1);
void OutTextXYExtents(int32_t X, int32_t Y, const char* S, int xres, int yres, int32_t& out_height, int32_t& out_width);
int32_t Write_String_T32(int32_t X,int32_t Y,char *S,dword C);
void Set_RGB(char Reg,char R,char G,char B);
void Set_Palette(VPalette Pal);
void Make_Gouraud_8(char R0,char G0,char B0,char R1,char G1,char B1);
void Make_Addative();
void Make_ColorMap();
void Make_Shade(float Dark,float Bright);
void Make_Transparence();
void View_8Tables();
void Build_8Tables();
void Deinitialize_VESA();

void SetVESAMode(int Mode);
void SetVGAMode(unsigned short Mode);
void Transparence(VESA_Surface *Source,VESA_Surface *Target);
void Modulate(VESA_Surface *Source,VESA_Surface *Target,dword SrcMask,dword TrgMask, dword PageSize);

dword AttemptGraphicsSet(int32_t X,int32_t Y,int32_t BPP,dword ForceBanked);

void VESA_InitExternal(int32_t x, int32_t y, int32_t bpp);
void VESA_VPageExternal(VESA_Surface *ExtrnVS);
#endif

#ifndef ISRIncluded
#define ISRIncluded
void  Install_8();
void  Restore_8();
void  Set_Rate(unsigned int Rate);
void  Install_9();
void  Restore_9();
void  Keyboard_HTest();
char  Keypressed();
char Read_KBStack();
#endif

#ifndef CameraIncluded
#define CameraIncluded
void  CalcPersp(Camera *Cm);
float LensToFOV(float lens);
void  Shatter_Camera(Vector *Source,Vector *Target,float Roll,Matrix CamMat);
void  Kick_Camera(Vector *Source,Vector *Target,float Roll,Matrix CamMat);
void  Init_FreeCamera();
void  Calibrate_FreeCamera_ForScene(float sceneFZP, Camera *sceneCamera);
void  Dynamic_Camera();
#endif

#ifndef MiscIncluded
#define MiscIncluded
void Hierarchy_Sort(Scene *Sc);

void *getAlignedBlock(uintptr_t size, uintptr_t alignment = 64);
void freeAlignedBlock(void *ptr);

#endif

#ifndef ImageCodeIncluded
#define ImageCodeIncluded
char Identify_Texture(Texture *Tx);
char Load_Texture(Texture *Tx);
void View_Texture(Texture *Tx);
void BPPConvert_Texture(Texture *Tx, byte TB);
void FilterLSB_Texture(Texture *T);
void Unify_Textures(Scene *Sc);

// Per-scene matID -> Material* lookup table. Built by Unify_Textures
// alongside the M->ID = matID++ assignment; consumed by the deferred
// lighting pass which reads matID out of the G-buffer's mat32 packing
// and needs the Material* to know which texture / lighting params to
// apply. Returned data pointer is borrowed and stays alive until the
// next Unify_Textures(Sc) call (i.e. for the lifetime of the scene).
struct MatTable {
	Material *const *data;
	dword            count;
};
MatTable Scene_GetMatTable(Scene *Sc);
// Re-walk MatLib for `Sc`, reassign sequential matIDs, rebuild the matTable.
// Idempotent. Use after scene-init adds Materials to MatLib post-Unify_Textures.
void Scene_RebuildMatTable(Scene *Sc);
char Load_Image_PCX(Image *Img,	const char *FN);
char Load_Image_GIF(Image *Img, const char *FN);
char Load_Image_JPEG(Image *Img, const char *FN);
void TAS_Set(dword TASType,int32_t X,int32_t Y);
void Generate_Mipmaps(Texture *Tx, mword blockSizeX, mword blockSizeY, mword enableMip);
#endif

#ifndef ImageProcessingIncluded
#define ImageProcessingIncluded
// Kombat's stuph go here
void T16Conv(Word **Data,int32_t OldX,int32_t OldY,int32_t X,int32_t Y);
// end
void MipmapXY(Image *Img);
void MipmapX(Image *Img);
void MipmapY(Image *Img);
void Scale_Image(Image *Img,int32_t NX,int32_t NY);
void Convert_Texture2Image(Texture *Tx,Image *Img);
void Convert_Image2Texture(Image *Img,Texture *Tx);
void Gamma_Correction(Image *Img,float Gamma);
void Intns_Histogram_Correction(Image *Img);
void Intensity_Alpha(Image *Img);
void Image_Convulate_3x3(Image *Img,Matrix M);
void Image_Laplasian(Image *Img);
void Image_LPF(Image *Img);
void Image_HPF(Image *Img);
void Image_Enhance(Image *Img);
void Bump_Image_2D(Image *Prim,Image *BMap,Image *BTbl,int32_t LX,int32_t LY);
void Image_Ripple(Image *Prim,float Amp,float Freq,float Ofs);
#endif

#ifndef ImageGeneratorsIncluded
#define ImageGeneratorsIncluded
void New_Image(Image *Img,int32_t X,int32_t Y);
void Generate_Flare_Image(Image *Img,float FR,float FG,float FB);
void Generate_Flare_Image_Test(Image *Img,float FR,float FG,float FB,float blah,float XDec,float YDec);
void Generate_RGBFlares();
Material *Generate_RGBFlare(unsigned char R,unsigned char G,unsigned char B);

void Setup_Grid_Texture_Mapper_MMX(dword xres, dword yres);
void Grid_Texture_Mapper(GridPoint *GP,Image *Img,dword *DAC);
void Generate_Fractal_Noise(Image *Img,float Graininess);
void Generate_Plasma(Image *Img, short p, int32_t seed);
AGridPoint * Init_Antiwobbler();
void AW_Staticmap(AGridPoint *AGP);
#endif

#ifndef RenderIncluded
#define RenderIncluded
void FDS_Title(void);
char FDS_Init(unsigned short x,unsigned short y,unsigned char BPP);
void DrawSpline(Spline Pos, Vector CamISource, int EndFrame);
void FDS_End(void);
void Init_Randomseed();


void Preprocess_Scene(Scene *Sc, const char *tag = nullptr);
void Materials_Remap_Aligned();
void Materials_Load(Scene *Sc);
void Assign_Fillers(Scene *Sc);
void FList_Allocate(Scene *Sc);
void Scene_Computations(Scene *Sc);

void Radix_Sort(fds::FListEntry *f, fds::FListEntry *s, unsigned int n);

void SetCurrentScene(Scene *Sc);

// Animate the scene's meshes + omnis from their splines at CurFrame.
// `cam`: if non-null, also re-animate that camera's pose + perspective
// and update the global FOVX/FOVY for the active render loop. If null,
// no camera work happens — safe to call from off-render-loop threads
// (Initialize_X bake prep) without touching the global `View`.
// Old `bool SkipCameraAnimation` API removed; render loops pass `View`,
// init-time bake prep passes `nullptr`.
void Animate_Objects(Scene *Sc, Camera *cam);
// xresOverride / yresOverride: alternate screen rect for visibility-flag
// computation (>= 0 to override, -1 to use XRes/YRes globals). The
// shadow-map orchestrator passes its shadow-map dimensions so faces
// near the cone edge aren't dropped by the F->VisibilityFlagsAll
// face-filter, which would otherwise compare PX/PY against the main
// framebuffer XRes/YRes instead of the shadow rect.
// `cam` and `faces` carry the per-pass projection + face-list state.
// Convenience overload (defined in FrameState.h to avoid a header
// dependency cycle) routes through fds::g_mainCamera / fds::g_mainFaces
// — same memory as the legacy CAll / FList / FOVX aliases, so existing
// one-arg call sites need no further changes. The parallel shadow
// orchestrator will pass per-light instances explicitly.
// Primary form: explicit per-pass camera + face-list contexts. Default
// args at the end let callers say `Transform_Objects(Sc, cam, faces)`
// for the common main-pass case (xres/yres overrides only matter when
// rendering into a different surface than the engine framebuffer —
// e.g. shadow maps).
namespace fds { struct VertexScratch; }
void Transform_Objects(Scene *Sc,
                        fds::CameraContext &cam,
                        fds::FaceListContext &faces,
                        int xresOverride = -1, int yresOverride = -1,
                        fds::VertexScratch *scratch = nullptr);
char BFC(Face *F);
char BOC(Face *F);
void Construct_FaceList(Scene *Sc);
void StaticLighting(Scene *Sc);
void Lighting(Scene *Sc);
void Restore_Splines(Scene *Sc);
enum class RenderPath {
	Default,        // honor FDS_DEFERRED env var
	ForceForward,   // forward fillers regardless (sky cube, cube-map bake)
	ForceDeferred,  // deferred path regardless of env (snapshot harnesses)
};
// skipVolumetric=true bypasses the per-frame volumetric passes (cones,
// halos, unified). Used by scenes that issue a subordinate Render()
// call whose output is consumed by post-processing (e.g. City's pass-1
// reflection bake into dispMap-distorted backbuffer): the volumetric
// work in that pass is largely wasted because the result is distorted/
// composited beyond recognition.
void Render(RenderPath path = RenderPath::Default, bool skipVolumetric = false);
// Single-threaded forward render of a screen region (no threadpool) into the
// given context's target/face-list/camera. For tiny offscreen targets where
// dispatch overhead dwarfs the raster; also the per-pass entry that lets the
// shard/shadow/RTT bakes run on their own context concurrently.
namespace fds { struct RenderContext; }
void RenderForwardRegionInline(const fds::RenderContext& ctx,
                               float x1, float y1, float x2, float y2);
// Deferred sibling: Mekalele-fill ctx's face list into ctx.target's G-buffer
// (which MUST be set). Single-threaded, inline. Used by the parallel deferred
// shard bake (each worker fills its own G-buffer then shades it).
void MekaleleFillRegionInline(const fds::RenderContext& ctx,
                              float x1, float y1, float x2, float y2);
void SetDeferredWaterMatID(int id);
// Effective procedural-water state for the CURRENT scene's water material:
// the material's tri-state WaterProcMode (sidecar 'waterProcedural', -1 off /
// 0 auto / 1 on) wins; auto falls back to the global --water_procedural flag.
// The per-surface twin of FeatureFlags::water_procedural() — use this in any
// water consumer outside the deferred kernel (which resolves via its ctx).
bool WaterProceduralEffective();
bool deferredEnabled();
// Shadow-map depth pre-pass. Walks Omni_CastsShadow lights, builds a
// temp camera per light, re-transforms the scene into the light's POV,
// depth-rasters into ShadowMap::depth. Globals saved & restored; safe
// to call before the main Render(). Gated on env FDS_SHADOWS=1.
//
// staticOnly=false (default): per-frame pass — renders dynamic shadow
// maps (Omni_CastsShadow without Omni_StaticShadow). Static lights
// are skipped (already baked).
// staticOnly=true: one-shot bake of Omni_StaticShadow lights, called
// from ShadowMaps_BakeStatic during scene init.
//
// See FILLERS/ShadowMap.h.
// Bake mode selector. See Shadows.cpp for behavioral details.
enum class ShadowBakeMode {
	StaticOnce,           // ShadowMaps_BakeStatic — static omnis + static meshes, write to packSD
	DynamicOmnisPerFrame, // dynamic omnis + all meshes, write to packSD (legacy per-frame)
	DynamicMeshesPerFrame // static omnis + dynamic meshes only, write to packDyn
};
// forceEnable: honoured only by the StaticOnce bake. Bypasses the global
// FeatureFlags::shadows() gate so a scene that turns shadows on at RUN time
// (e.g. greets, whose GreetsApplyRunDefaults sets --shadows after the init
// bake window) can still populate its static occluder maps at INIT — the
// forced static-shadow lightmap bake reads them. Default false: per-frame
// modes and other scenes keep the flag gate.
void Render_DeferredShadowMaps(class Scene *Sc,
                               ShadowBakeMode mode = ShadowBakeMode::DynamicOmnisPerFrame,
                               bool forceEnable = false);

// Stage A overlap (docs/THREADING_OVERLAP_PLAN.md). The greets per-frame
// dynamic shadow bake. With --shadow-gbuffer-overlap, runs the bake(s) on a
// dedicated orchestrator thread so it overlaps the subsequent gbuffer fill;
// renderFrame calls ShadowBake_JoinPending() before deferred lighting (which
// samples the maps). Without the flag, runs the bakes synchronously inline
// (current behavior) and JoinPending is a no-op. DispatchGreets runs both the
// DynamicOmnis bake and, when --shadow-dynamic is set, the DynamicMeshes bake.
void ShadowBake_DispatchGreets(class Scene *Sc);
void ShadowBake_JoinPending();

// Reflection fog gradient: a scene that composites a planar mirrored pass
// under its water hands the (wobble-displaced) pass-1 Z here, between its
// passes; the next froxel fog composite fogs those z==0 reflection pixels
// at the mirrored path length instead of uniformly at the grid far plane.
// Consumed once, then reset (no dangling pointer across scene changes).
void FastFog_SetReflectionZ(const uint16_t *z, float waterY);

// Shadow technique. Toggleable at runtime.
enum class ShadowMode {
	Depth,    // z-compare with bias and PCF
	PolyId,   // material-ID equality (per-material shadow grouping)
};
extern std::atomic<ShadowMode> g_shadowMode;
#endif

#ifndef FrustrumIncluded
#define FrustrumIncluded

// Third generation Frustrum clipper
//void InitFrustrumClipper();
//void InitClipperViewport(Scene *Sc);
//void FrustrumClipper();


#endif

void GridRendererT(GridPointT *GP, Image *Img, dword *page, dword XRes, dword YRes);
void GridRendererTG(GridPointTG *GP, Image *Img, dword *page, dword XRes, dword YRes);


#ifndef FillersIncluded
#define FillersIncluded

// Sprite scalers
void The_Seven_UP(Vertex **V, dword numVerts);
void The_Seven_UP_64(int32_t X,int32_t Y,int32_t SX,int32_t SY,dword *Bitmap,byte *Page);
void The_MMX_Scalar(Face* F, Vertex **V, dword numVerts, dword miplevel,
                    const fds::RenderTarget& rt,
                    const fds::CameraContext& cam);

// Legacy "IX" software fillers deleted 2026-05-15 after a snapshot
// trace across every demo scene confirmed none were ever called.
// PREPROC's stdFillers[] now routes to TheOtherBarry variants for every
// rasterFlags slot. The poly-stats helpers below were defined in the
// same translation unit and gated on the un-defined MEASURE_POLYSTATS;
// kept as inline no-ops so the (also-disabled) callers in REV / Snapshot
// keep compiling.
inline void InitPolyStats(int32_t /*cells*/) {}
inline void SavePolyStats(const char * /*fileName*/) {}


// Tile based rendering
void TBR_Init(Scene *Sc, mword size);
void TBR_EnsureInit(Scene *Sc, mword entriesPerStrip);
bool TBR_MatchesTarget(const Scene *Sc);
void TBR_Sprite(Face* F, Vertex **V, dword numVerts, dword miplevel,
                const fds::RenderTarget& rt,
                const fds::CameraContext& cam);
struct DeferredLightingCtx;
void TBR_Render(Scene *Sc, const DeferredLightingCtx *dctx);
void InsertTransparentToTBR(Face *F);

// Per-strip transparent rendering. Called from TBR_Render's per-strip
// walk for each clump of consecutive same-(mesh, frontFacing) transparent
// faces. Clears the strip's slice of the xpar G-buffer (back if !front,
// front layer if front), rasterizes the clump's faces with clipper rect
// limited to the strip, then composites that layer onto VPage strip rows
// via the existing transparent lighting kernel.
void RenderXparClumpInStrip(const DeferredLightingCtx &dctx,
                            class Face** faces, int count, bool front,
                             int strip_y, int strip_h, bool bandSnapshot = true);

// Declare every strip's xpar layer slice dirty over its full width. Both TBR
// schedulers call this before dispatch: the per-strip column tracker that
// bounds the clump clears (--xpar_strip_extent) only knows what IT dirtied,
// so anything left by the previous frame, by the legacy peel, or by a
// resolution change has to be conservatively re-declared.
void XparStripSlices_MarkAllDirty();
// --xpar_extent_census: print + reset the per-call composite census.
void XparExtentCensus_Report();

// --mirror_rtt_trace: print the whole run's greets mirror RTT bake trace in
// one burst (recorded in memory so the per-frame path stays timing-neutral).
// Called by the --repro harness just before it dumps its frame.
namespace fds { void MirrorRttTrace_Report(); }

// Re-establish every ENGINE-scoped transparent depth-peel global (peel floor
// -> all-0xFFFF, the two deep-layer slices + their Z, the per-strip
// dirty-column records, this thread's reverse-peel flag). Called from
// SceneDriver::setupFaceLists, i.e. once per scene entry: the peel state is
// engine-scoped and a deep-peel scene otherwise leaks it into the next one.
void XparPeel_ResetAll();

bool deferredUnifiedTbrEnabled();
int  xparPeelPassesEffective();

#endif

#ifndef SpanBufferIncluded
#define SpanBufferIncluded
void PrepareSpans(unsigned short NumOfLines);
void DrawTextureTriangle(Vertex *v1,Vertex *v2,Vertex *v3);
void BlastSpans(unsigned int Screen);
void KillSpans(void);
#endif

#ifndef ParticleSysIncluded
#define ParticleSysIncluded
void Install_Particle_Dreams(Scene *Sc);
void Particle_Dreams_EXEC();
void Particle_Display();
void Particle_Deinitialize();
void Rast_Explosion_Pcl(Vertex **V, dword numVerts);
#endif

#ifndef UNARJ_LibraryIncluded
#define UNARJ_LibraryIncluded
extern "C++" char *LoadARJ(char argv1[],char argv2[]); // returns Data stream , Size in 4 first bytes , size not including these 4 bytes
#endif

#ifndef MIDAS_LibraryIncluded
#define MIDAS_LibraryIncluded
void __cdecl UpdateInfo(void);
void __cdecl TimerCallback(void);
void Install_MIDAS();
void Initialize_Module(char *XMFile);
void Play_Module();
void ShutDown();
#endif

#ifndef Radiosity_Included
void Radiosity(Scene *Sc);
extern dword New_SCache(dword Size);
#define Radiosity_Included
#endif


#ifdef __cplusplus
}
#endif


// C++ linkage

// v0.8: FMod module player interface; no library-specific structures are interfaced
//#ifndef FMod_Included
//#define FMod_Included
//dword FModInit();
//dword FModLoadModule(const char *ModuleFilename);
////dword FModLoadModule(void *moduleBuffer, dword moduleLength);
//void FModPlayModule(dword handle);
//void FModFreeModule(dword handle);
//void FModClose();
//
//// FMod: Advanced features
//void FModGetModuleInfo(MusicModuleInfo &mmi);
//void FModSetModuleInfo(MusicModuleInfo &mmi);
//#endif

// Skycube generation routine
Scene * CreateSkyCube(dword skyType);
void RenderSkyCube(Scene *Sc, Camera *Cm, bool SkipCameraAnimation, bool ForceForward = false);

void Sachletz(dword* data, dword xres, dword yres);

template <typename T>
T* getAlignedArray(size_t N, size_t alignment) {
	T* block = (T*)getAlignedBlock(sizeof(T) * N, alignment);
	for (auto i = 0u; i < N; ++i) {
		new(&block[i]) T();
	}
	return block;
}

template <typename T>
T* getAlignedType(uintptr_t alignment) {
	T* block = (T*)getAlignedBlock(sizeof(T), alignment);
	return new(block) T();
}


#endif //FDS_DECS_H_INCLUDED