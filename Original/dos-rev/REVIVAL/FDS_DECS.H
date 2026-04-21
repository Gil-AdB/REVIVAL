#ifdef __cplusplus
extern "C" {
#endif

// Math declarations

#ifndef MathIncluded
#define MathIncluded
extern void  Generate_SCTabs();
extern void  Init_Identities();

extern void   Vector_Form(Vector *V,float X,float Y,float Z);
extern Vector Vector_Make(float X,float Y,float Z);
extern void   Vector_Zero(Vector *U);
extern void   Vector_Copy(Vector *U,Vector *V);
extern void   Vector_Scale(Vector *V,float S,Vector *W);
extern void   Vector_SelfScale(Vector *V,float S);
extern float  Vector_Length(Vector *V);
extern float  Vector_SelfDot(Vector *V);
extern void   Vector_Norm(Vector *V);
//extern void  Vector_Add(Vector *U,Vector *V,Vector *W);
extern void   Vector_SelfAdd(Vector *U,Vector *V);
//extern void  Vector_Sub(Vector *U,Vector *V,Vector *W);
extern void   Vector_SelfSub(Vector *U,Vector *V);
extern void   Vector_SAdd(Vector *U,float S,Vector *V);
extern void   Vector_LComb(Vector *U,Vector *V,float A,float B,Vector *W);
extern void   Vector_LERP(Vector *U,Vector *V,float T,Vector *W);
extern float  Dot_Product(Vector *U,Vector *V);
extern void   Cross_Product(Vector *U,Vector *V,Vector *W);
extern float  Vector_CosAngle(Vector *U,Vector *V);
extern float  Vector_Angle(Vector *U,Vector *V);
extern float Distance(Vector *V,Vector *U);
extern void   Matrix_Form(Matrix M,float AA,float AB,float AC,float BA,float BB,float BC,float CA,float CB,float CC);
extern void   Matrix_Copy(Matrix A,Matrix B);
extern void   Matrix_Scale(Matrix M,float S);
extern float  Determinant(Matrix A);
extern void   Matrix_Transpose(Matrix A);
extern void   Matrix_Norm(Matrix A);
extern void   Matrix_InvScale(Matrix A);
extern void   Orthogonalize(Matrix A);
//extern void  MatrixXVector(Matrix M,Vector *U,Vector *V);
extern void   MatrixTXVector(Matrix M,Vector *U,Vector *V);
extern void   MatrixXMatrix(Matrix A,Matrix B,Matrix C);
extern void   MatrixTXMatrix(Matrix A,Matrix B,Matrix C);
extern void   Matrix_Identity(Matrix M);
extern void   Matrix_Rotation(Matrix Mat,float RX,float RY,float RZ);
extern void   Euler_Angles(Matrix Mat,float RX,float RY,float RZ);
extern float  Tri_Surface(Vector *A,Vector *B,Vector *C);
extern float  TetraHedron_Volume(Vector *A,Vector *B,Vector *C,Vector *D);
extern float  Trapped_Volume(Vector *A,Vector *B,Vector *C);
extern void   Triangle2Plane(Vector *A,Vector *B,Vector *C,Plane *P);
extern void   Normalize_Plane(Plane *P);
extern float  PointPlaneDistance(Vector *V,Plane *P);
extern void   DVertex2Line(Vector *V,Vector *W,Line *L);
extern void   Normalize_Line(Line *L);
extern float  PointLineDistance(Vector *V,Line *L);
extern float  LineLineDistance(Line *L,Line *M);
extern void   Vector4_Form(Vector4 *V,float X,float Y,float Z,float T);
extern void   Vector4_Zero(Vector4 *V);
extern void   Vector4_Copy(Vector4 *V,Vector4 *U);
extern void   Vector4_WNorm(Vector4 *V);
extern void   Matrix4_Form(Matrix4 M,float AA,float AB,float AC,float AD,float BA,float BB,float BC,float BD,float CA,float CB,float CC,float CD,float DA,float DB,float DC,float DD);
extern void   Matrix4_Copy(Matrix4 A,Matrix4 B);
extern void   Matrix4_Identity(Matrix4 M);
extern void   Matrix4_TransposeOut(Matrix4 A,Matrix4 B);
extern void   Matrix4_Transpose(Matrix4 A);
extern void   MatrixXVector4(Matrix4 M,Vector4 *U,Vector4 *V);
extern void   MatrixTXVector4(Matrix4 M,Vector4 *U,Vector4 *V);
extern void   MatrixXMatrix4(Matrix4 A,Matrix4 B,Matrix4 C);
extern void   MatrixTXMatrix4(Matrix4 A,Matrix4 B,Matrix4 C);

extern void   Complex_Form(Complex *Z,float X,float Y);
extern void   Complex_Adj(Complex *Z,Complex *W);
extern void   Complex2AngLen(Complex *Z,float *Ang,float *Len);
extern void   AngLen2Complex(Complex *Z,float Ang,float Len);
extern void   Complex_Add(Complex *Z,Complex *W,Complex *R);
extern void   Complex_Sub(Complex *Z,Complex *W,Complex *R);
extern void   Complex_Mul(Complex *Z,Complex *W,Complex *R);
extern void   Complex_Div(Complex *Z,Complex *W,Complex *R);

extern void   Quaternion_Form(Quaternion *Q,float X,float Y,float Z,float W);
extern void   Quaternion_Copy(Quaternion *Q,Quaternion *S);
extern void   Quaternion_Scale( Quaternion *Q,float S,Quaternion *P);
extern float  Quaternion_Length(Quaternion *Q);
extern void   Normalize_Quaternion(Quaternion *Q);
extern void   Quaternion_Unit(Quaternion *Q,Quaternion *Dest);
extern void   Quaternion_Negate(Quaternion *Q);
extern void   Quaternion_Exponent(Quaternion *Q, Quaternion *Dest);
extern void   Quaternion_Logarithm(Quaternion *Q, Quaternion *Dest);
extern void   Quaternion_Inverse(Quaternion *Q, Quaternion *Dest);
extern float  Quaternion_Dot_Product(Quaternion *Q1, Quaternion *Q2);
extern void   AngleAxis2Quaternion(Quaternion *Q,Quaternion *P);
extern void   Quaternion2AngleAxis(Quaternion *Q, Quaternion *A);
extern void   Quaternion_Mul(Quaternion *A,Quaternion *B,Quaternion *C);
extern void   Quaternion_Ln_Dif(Quaternion *P, Quaternion *Q, Quaternion *Dest);
extern void   Convert_Quat2Mat(Quaternion *Q,Matrix M);
extern void   Convert_Mat2Quat(Matrix M, Quaternion *Q);

extern void   Quaternion_SLERP(Quaternion *A,Quaternion *B, Quaternion *Dest, float Time,float Spin);


extern void   Quaternion_SLERP_V2(Quaternion *A,Quaternion *B, Quaternion *Dest, float Time,float Spin);
extern void   SLERP(Quaternion *Q,Quaternion *P,float T,Quaternion *R);
extern void   SQUAD(Quaternion *Q,Quaternion *A,Quaternion *B,Quaternion *P,float T,Quaternion *R);

extern void   Spline_SetKey_4D(Spline *S,Quaternion *Q,float Frame,float SpF[5]);
extern void   Spline_SetKey_3D(Spline *S,Vector *V, float Frame,float SpF[5]);
extern void   Spline_SetKey_1D(Spline *S,float X,float Frame,float SpF[5]);

extern float  BKS_Ease(float T,float A,float B);

extern void   Spline_Init_3D(Spline *S);
extern void   Spline_Init_4D(Spline *S);
extern void   Spline_Calc_1D(Spline *S,float Frame, float *Out);
extern void   Spline_Calc_3D(Spline *S,float Frame, Vector *Out);
extern void   Spline_Calc_4D_Alt(Spline *S,float Frame, Quaternion *Out);
extern void   Spline_Calc_4D(Spline *S,float Frame, Quaternion *Out);

extern void   Spline_Init_Bezier(Spline *S);
extern void   Spline_Subdivide_Bezier(Spline *S,float Frame, Quaternion *Out);
#endif

#ifndef _3DSIncluded
#define _3DSIncluded
extern char Load_3DS(Scene *Sc, char *FileName);
extern void Add_Scene_3DS(char *FileName);
#endif

#ifndef _FLDIncluded
#define _FLDIncluded
extern char LoadFLD(Scene *Sc,char *FileName);
#endif


#ifndef V3DIncluded
#define V3DIncluded
extern void Material2Face(Material *M,Face *F);
extern void Insert_TriMesh2Scene(Scene *Sc,TriMesh *T);
extern void Insert_Omni2Scene(Scene *Sc,Omni *O);
extern char Load_V3D(Scene *Sc,char *FileName);
#endif

#ifndef DPMIIncluded
#define DPMIIncluded
extern void * D_New(DWord Size);
extern void D_Del(void *Target);
extern void * D_Resize(void *Target,DWord Size);
extern void DPMI_Init_Core();
extern void DPMI_Deinit_Core();
extern void DPMI_Init_MB(DWord Amount);
extern void * DPMI_NewModule(Module_MB *Father,DWord Size,char *Name);
extern void * DPMI_New(void *Father,DWord Size,char *Name);
extern void * DPMI_Assisted_New(Module_MB *Core,void *Father,DWord Size,char *Name);
extern void * DPMI_Resize(void *Target,DWord Size);
extern void DPMI_Destroy(Module_MB *Target);
extern void DPMI_RecDestroy(Module_MB *Target);
extern void DPMI_Delete(void *Target);
extern void DPMI_Assisted_Delete(Module_MB *Core,void *Target);
extern Module_MB * DPMI_Get_MMB(void *Target);
extern DWord DPMI_Get_Handle(void *Target);
extern void DPMI_Message(char *ErrMsg);
extern void DPMI_Warning(char *ErrMsg);
extern void DPMI_Error(char *ErrMsg);
extern DWord DPMI_Free_Memory(void);
extern void DPMI_allocRealSeg(int size,int *sel,int *r_seg);
extern void DPMI_freeRealSeg(unsigned sel);
extern int DPMI_int86(int intno, RMREGS *in, RMREGS *out);
extern int DPMI_int86x(int intno, RMREGS *in, RMREGS *out, RMSREGS *sregs);
extern int DPMI_allocSelector(void);
extern void DPMI_setSelectorBase(int sel,DWord linAddr);
extern void DPMI_setSelectorLimit(int sel,DWord limit);
extern void Init_Aligned(long Amount);
extern char * Get_Aligned_Block();
extern void Remove_Aligned_Block(void *Target);
extern void Deinit_Aligned();

extern void *DPMI_Map_P2L(DWord physAddr,DWord limit);
extern void DPMI_Del_P2L(void *Linear);
#endif

#ifndef VESAIncluded
#define VESAIncluded
extern void Initialize_VESA();
extern int VBE_Detect();
extern void VESA_FullInfo(long RX,long RY,char RBPP);
extern VESA_Surface * VESA_Alloc_Surface(long RX,long RY,long RBPP,long FOV,DWord Flags);
extern char VESA_Init_Video(long X,long Y,long BPP);
extern void Set_Screen(long X,long Y,long BPP,char ZBuf,float FOV);
extern void VESA_Surface2Global(VESA_Surface *VS);
extern char Modesetup();
extern void End_Graphics();

extern void Build_YOffs_Table(VESA_Surface *VS);

extern Font * LoadAFT(char *FileName);
extern long OutTextXY(char *Where,long X,long Y, char *S,char C);
extern long Write_String_T32(long X,long Y,char *S,DWord C);
extern void Set_RGB(char Reg,char R,char G,char B);
extern void Set_Palette(VPalette Pal);
extern void Make_Gouraud_8(char R0,char G0,char B0,char R1,char G1,char B1);
extern void Make_Addative();
extern void Make_ColorMap();
extern void Make_Shade(float Dark,float Bright);
extern void Make_Transparence();
extern void View_8Tables();
extern void Build_8Tables();
extern void Deinitialize_VESA();

extern void SetVESAMode(int Mode);
extern void SetVGAMode(unsigned short Mode);
extern void Transparence(VESA_Surface *Source,VESA_Surface *Target);
extern void Modulate(VESA_Surface *Source,VESA_Surface *Target,DWord SrcMask,DWord TrgMask);
#endif

#ifndef ISRIncluded
#define ISRIncluded
extern void  Install_8();
extern void  Restore_8();
extern void  Set_Rate(unsigned int Rate);
extern void  Install_9();
extern void  Restore_9();
extern void  Keyboard_HTest();
extern char  Keypressed();
extern char Read_KBStack();
#endif

#ifndef CameraIncluded
#define CameraIncluded
extern void  CalcPersp(Camera *Cm);
extern float LensToFOV(float lens);
extern void  Shatter_Camera(Vector *Source,Vector *Target,float Roll,Matrix CamMat);
extern void  Kick_Camera(Vector *Source,Vector *Target,float Roll,Matrix CamMat);
extern void  Init_FreeCamera();
extern void  Dynamic_Camera();
#endif

#ifndef MiscIncluded
#define MiscIncluded
//Mouse stuff
extern void InitMouse();
extern void ShowMouse();
extern void HideMouse();
extern void DisplayMouse();
extern void Hierarchy_Sort(Scene *Sc);
extern void Detect_MMX();

extern void Compute_Face_Normals(TriMesh *T);
extern void Compute_Vertex_Normals(TriMesh *T);
extern void Compute_Normal_Products(TriMesh *T);
#endif

#ifndef ImageCodeIncluded
#define ImageCodeIncluded
extern char Identify_Texture(Texture *Tx);
extern char Load_Texture(Texture *Tx);
extern "C++" char *LoadJPEG(char *argv1,unsigned int SeekPos);
extern void View_Texture(Texture *Tx);
extern void BPPConvert_Texture(Texture *Tx,char TB);
extern void FilterLSB_Texture(Texture *T);
extern void Unify_Textures(Scene *Sc);
extern char Load_Image_PCX(Image *Img,char *FN);
extern char Load_Image_GIF(Image *Img,char *FN);
extern char Load_Image_JPEG(Image *Img,char *FN);
#endif

#ifndef ImageProcessingIncluded
#define ImageProcessingIncluded
extern void MipmapXY(Image *Img);
extern void MipmapX(Image *Img);
extern void MipmapY(Image *Img);
extern void Scale_Image(Image *Img,long NX,long NY);
extern void Convert_Texture2Image(Texture *Tx,Image *Img);
extern void Convert_Image2Texture(Image *Img,Texture *Tx);
extern void Gamma_Correction(Image *Img,float Gamma);
extern void Intns_Histogram_Correction(Image *Img);
extern void Intensity_Alpha(Image *Img);
extern void Image_Convulate_3x3(Image *Img,Matrix M);
extern void Image_Laplasian(Image *Img);
extern void Image_LPF(Image *Img);
extern void Image_HPF(Image *Img);
extern void Image_Enhance(Image *Img);
extern void Bump_Image_2D(Image *Prim,Image *BMap,Image *BTbl,long LX,long LY);
extern void Image_Ripple(Image *Prim,float Amp,float Freq,float Ofs);
#endif

#ifndef ImageGeneratorsIncluded
#define ImageGeneratorsIncluded
extern void New_Image(Image *Img,long X,long Y);
extern void Generate_Flare_Image(Image *Img,float FR,float FG,float FB);
extern void Generate_Flare_Image_Test(Image *Img,float FR,float FG,float FB,float blah,float XDec,float YDec);
extern void Generate_RGBFlares();
extern void Grid_Texture_Mapper_320(GridPoint *GP,Image *Img,DWord *DAC);
extern void Grid_Texture_Mapper_MMX(GridPoint *GP,Image *Img,DWord *DAC);
extern Material *Generate_RGBFlare(unsigned char R,unsigned char G,unsigned char B);
extern void Fit_FlareImage(Omni *O);
extern void Generate_Julia_Fractal(Image *Img,Complex *C,Complex *R1,Complex *R2);
extern void Generate_Mandelbrot_Fractal(Image *Img,Complex *R1,Complex *R2);
extern void Generate_Phoenix_Fractal(Image *Img,float P1,float P2,Complex *R1,Complex *R2);
extern void Grid_Texture_Mapper(GridPoint *GP,Image *Img,DWord *DAC);
extern void Init_PlanarGrid(GridPoint **GP);
extern void GridFX_Test();
extern void Generate_Fractal_Noise(Image *Img,float Graininess);
extern void Generate_Plasma(Image *Img, short p, long seed);
extern void Init_Whirlwind(char Spots,float Grav,float Repulse);
extern void Init_WWGrav(float U,float V);
extern void Reset_WWGrid();
extern void Update_WWGrid();
extern void Display_WWGrid(Image *Img);
extern AGridPoint * Init_Antiwobbler();
extern void AW_Staticmap(AGridPoint *AGP);
extern void AW_Distort(AGridPoint *AGP,float Bang);
extern void AW_FromParticles(AGridPoint *AGP,Particle *P);
extern void AntiGrid_Mapper(AGridPoint *AGP,Image *Img,DWord *DAC);
extern void Init_WRaytrace_Table(float FOV);
extern void Affine_Wobbler(GridPoint *Target_GP,float Ang,float Zoom,float XO,float YO);
extern void WRaytrace_Cylinder(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float Radius);
extern void WRaytrace_CurvedCylinder(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float *Radius);
extern void WRaytrace_Plane(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float Height);
extern void WRaytrace_Sphere(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float Radius);

// general utility stuff, and some other neat stuff like that
extern void Phong_Map(Image *Img,float R,float G,float B,float Power);
extern void Hidden_Flare(Image *Src,Vector *L,Image *Trg,long NumRays);
#endif

#ifndef RenderIncluded
#define RenderIncluded
extern void FDS_Title(void);
extern void LoadScenes(char Scenes,char *Name[]);
extern char FDS_Init(unsigned short x,unsigned short y,unsigned char BPP);
extern void SetGraphicsMode(void);
extern void Get_Palette2Prime(Scene *Sc);
extern void FDS_End(void);
extern void Display_Hierarchy_Links(Scene *Sc);
extern void MsgToDisplay(DWord ID,DWord Issue,char *Str);
extern DWord Find_RetraceRate();

extern void Planar_Mapping(TriMesh *T);

extern void Preprocess_Scene(Scene *Sc);
extern void Default_Texture(Scene *Sc);
extern void Init_Default_Material();
extern void Init_Flare_Textures(Scene *Sc);
extern void Materials_Remap_Aligned();
extern void Materials_Load(Scene *Sc);
extern void Assign_Fillers(Scene *Sc);
extern void FList_Allocate(Scene *Sc);
extern void Scene_Computations(Scene *Sc);

extern void Radix_SortingASM(Face **f,Face **s,unsigned int n);
extern void Radix_Sorting();

extern void RunScene(Scene *Sc,float Seconds);

extern void Animate_Objects(Scene *Sc);
extern void Transform_Objects(Scene *Sc);
extern char BFC(Face *F);
extern char BOC(Face *F);
extern void Construct_FaceList(Scene *Sc);
extern void Lighting(Scene *Sc);
extern void Restore_Splines(Scene *Sc);
extern void Render();
#endif

#ifndef FrustrumIncluded
#define FrustrumIncluded
extern void Universal_Frustrum_Elite();
extern void Frust_Set_Flat();
extern void Frust_Set_L();
extern void Frust_Set_Env();
extern void Frust_Set_UV();
extern void Frust_Set_UVL();
#endif

#ifndef FillersIncluded
#define FillersIncluded
//Wire frame RULEZ
extern void Draw_Line_8Bit(Vertex *V1,Vertex *V2);
extern void Draw_Line_32Bit(Vertex *V1,Vertex *V2);
extern void Draw_Line_32Bit_ForKids(Vertex *V1,Vertex *V2);
//Flat mappers (ZOL!)
extern void Flat_Poly_8Bit(Vertex *A,Vertex *B,Vertex *C);
extern void FlatV2_Poly_8Bit(Vertex *A,Vertex *B,Vertex *C);
extern void FlatV3_Poly_8Bit(Vertex *A,Vertex *B,Vertex *C);
extern void Flat_Poly_ManyBits(Vertex *A,Vertex *B,Vertex *C);
extern void Adv_Flat_32(Vertex *A,Vertex *B,Vertex *C);
// Gouraud/Linear mappers
extern void The_Specular_Gouraud(Vertex *A,Vertex *B,Vertex *C);
extern void The_MMX_Gouraud(Vertex *A,Vertex *B,Vertex *C);
extern void The_Specular_TGouraud(Vertex *A,Vertex *B,Vertex *C);
// Bi-Linear Interp. Texture mappers
extern void The_Bilinear(Vertex *A,Vertex *B,Vertex *C);
extern void The_BiHi(Vertex *A,Vertex *B,Vertex *C);
extern void The_BiTrue(Vertex *A,Vertex *B,Vertex *C);
extern void The_BiTTrue(Vertex *A,Vertex *B,Vertex *C);
extern void The_BiPTrue(Vertex *A,Vertex *B,Vertex *C);
extern void The_BiPTTrue(Vertex *A,Vertex *B,Vertex *C);

// Get a g/txtr, kid...3328 bytes.
extern void The_BiGTrue(Vertex *A,Vertex *B,Vertex *C);
extern void The_BiGMMXTrue(Vertex *A,Vertex *B,Vertex *C);

// Sprite scalers
extern void Dark_Sprite(Vertex *A,Vertex *B,Vertex *C);
extern void Spirit_16(Vertex *A,Vertex *B,Vertex *C);
extern void The_Seven_UP(Vertex *A,Vertex *B,Vertex *C);
extern void The_Seven_UP_64(long X,long Y,long SX,long SY,DWord *Bitmap,char *Page);
extern void The_MMX_Scalar(Vertex *A,Vertex *B,Vertex *C);
extern void The_ZScale_Shit(Vertex *A,Vertex *B,Vertex *C);
extern void Particle_Sprite(Vertex *A,Vertex *B,Vertex *C);
extern void The_Seven_UP_HALFSIZE(Vertex *A,Vertex *B,Vertex *C);
#endif

#ifndef SpanBufferIncluded
#define SpanBufferIncluded
extern void PrepareSpans(unsigned short NumOfLines);
extern void DrawTextureTriangle(Vertex *v1,Vertex *v2,Vertex *v3);
extern void BlastSpans(unsigned int Screen);
extern void KillSpans(void);
#endif

#ifndef ParticleSysIncluded
#define ParticleSysIncluded
extern void Install_Particle_Dreams(Scene *Sc);
extern void Particle_Dreams_EXEC();
extern void Particle_Display();
extern void Particle_Deinitialize();
extern void Rast_Explosion_Pcl(Vertex *A,Vertex *B,Vertex *C);
#endif

#ifndef UNARJ_LibraryIncluded
#define UNARJ_LibraryIncluded
extern "C++" char *LoadARJ(char argv1[],char argv2[]); // returns Data stream , Size in 4 first bytes , size not including these 4 bytes
#endif

#ifndef MIDAS_LibraryIncluded
#define MIDAS_LibraryIncluded
extern void __cdecl UpdateInfo(void);
extern void __cdecl TimerCallback(void);
extern void Install_MIDAS();
extern void Initialize_Module(char *XMFile);
extern void Play_Module();
extern void ShutDown();
#endif

#ifdef __cplusplus
}
#endif
