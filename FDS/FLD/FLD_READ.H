//Flood scene Variables
typedef struct
{
  unsigned char Red,Green,Blue/*,Alpha*/;
} FldColor;

typedef struct
{
  unsigned short  FaceVerts;
  unsigned short *Verts;
  signed   short  Surface;
} FldFace;

typedef struct
{
  char          *Name;
  FldColor       Color;
  unsigned short Flags;
  float          Luminosity;
  float          Diffuse;
  float          Specular;
  float          Reflection;
  float          Transparency;
  unsigned short Glossiness;
  unsigned short ReflectionMode;
  char          *ReflectionImage;
  float          ReflectionSeamAngle;
  float          RefractiveIndex;
  float          EdgeTransparency;
  float          MaxSmoothingAngle;
  char          *ColorTexture;
  char          *DiffuseTexture;
  char          *SpecularTexture;
  char          *ReflectionTexture;
  char          *TransparencyTexture;
  char          *BumpTexture;
  char          *TextureImage;
  unsigned short TextureFlags;
  Vector         TextureSize;
  Vector         TextureCenter;
  Vector         TextureFallOff;
  Vector         TextureVelocity;
  char          *TextureAlpha;
  unsigned short NoiseFrequencies;
  unsigned short TextureWrapX;
  unsigned short TextureWrapY;
  float          AAStrength;
	float          Opacity;
	float					 TFP0;
	float					 TFP1;
	// Revival per-surface extension — Surf_RevExt(0x8000)-gated conditional
	// payload after the material record (u16 RevExtMask + only the authored
	// fields, ascending mask-bit order). Read in FLD_READ.CPP ReadMaterial,
	// mapped to Material in FLD_MAT.CPP AddMaterial. Bit order MUST match
	// tools/lwsread (FLDSAVE.CPP / LWOREAD.CPP). memset(0) = no extension.
	// See docs/SIDECAR_MIGRATION_PLAN.md §1a.
	uint16_t       RevExtMask;      // 0 = none
	float          AoStrength;      // bit 0x001
	float          ParallaxScale;   // bit 0x002
	float          TintR;           // bit 0x004
	float          TintG;           // bit 0x008
	float          TintB;           // bit 0x010
	float          RefractIor;      // bit 0x020
	uint8_t        Refractive;      // bit 0x040 -> Mat_Refractive
	int8_t         EnvReflMode;     // bit 0x080
	int32_t        EnvBakeRes;      // bit 0x100
	int8_t         WaterProcMode;   // bit 0x200
} FldMat;

typedef struct _FldKeyFrame_
{
  Vector        Position;
  Vector        Rotation;
  Vector        Scale;

  float         LinearValue;
  float         FrameNumber;
  float         Tension;
  float         Continuity;
  float         Bias;

  void Read(FILE * f) {
	  Position.Read(f);
	  Rotation.Read(f);
	  Scale.Read(f);
	  fread(&LinearValue, 1, sizeof(float), f);
	  fread(&FrameNumber, 1, sizeof(float), f);
	  fread(&Tension, 1, sizeof(float), f);
	  fread(&Continuity, 1, sizeof(float), f);
	  fread(&Bias, 1, sizeof(float), f);
  }

} FldKeyFrame;

typedef struct
{
  float        *Channel;
  float         FrameNumber;
  float         LinearValue;
  float         Tension;
  float         Continuity;
  float         Bias;
} FldEnvKey;

typedef struct
{
  FldEnvKey    *Key;
  uint32_t Keys;
  uint32_t Channels;
  uint32_t EndBehavior;
} FldEnv;

typedef struct FldObj
{
  char          *Name;
  uint32_t		 Flags;
  uint32_t		 NumOfMat;
  FldMat        *Material;
  uint32_t		 NumOfVerts;
  Vector        *Verts;
  uint32_t		 NumOfFaces;
  FldFace       *Faces;
  uint32_t		 Keys;
  FldKeyFrame   *KF;
  uint32_t		 Parent;
  Vector         Pivot;
  FldEnv        *PolygonSize;
  FldObj        *Next;
} FldObject;

typedef struct FldLgt
{
  char          *Name;
  uint32_t  Flags;
  uint32_t  Keys;
  FldKeyFrame   *KF;
  FldColor       Color;
  FldEnv        *Intensity;
  uint32_t  Parent;
	uint32_t  TargetObject;
	FldEnv        *Falloff;
	FldEnv        *ConeAngle;
	FldEnv        *Range;
	// Per-light volumetric beam gain — conditional field, only present in
	// the FLD when Flags carries Light_VolumetricCone (2048). 0 = unset →
	// converter/engine treat as 1.0 (FlareScale/HaloIntensity convention).
	float          VolBeamGain;
	// Per-light flare-sprite size multiplier — conditional field, only present
	// in the FLD when Flags carries Light_FlareScale (4096). 0 = unset →
	// engine treats as 1.0 (mapped to Omni::FlareScale in FLD_CONV.CPP).
	float          FlareScale;
	FldLgt        *Next;
} FldLight;

typedef struct
{
  uint32_t  Flags;
  uint32_t  Keys;
  FldKeyFrame   *KF;
  uint32_t  Parent;
  uint32_t  TargetObject;
  FldEnv        *zoomFactor;
} FldCamera;

typedef struct
{
  FldObject    *Object;
  FldLight     *Light;
  FldCamera    *Camera;
  uint32_t FirstFrame;
  uint32_t LastFrame;
  uint32_t FrameStep;
  float         FramesPerSecond;
  FldEnv       *AmbientColor;
	FldEnv       *AmbientIntensity;
  // Authored scene-wide env-reflection defaults (Scene_EnvDefaults-gated
  // conditional header payload; 0 = unset). See LWREAD.H + FLD_READ.CPP.
  int32_t       EnvReflSceneMode;   // -1 none / 0 auto / 1 all
  int32_t       EnvBakeResScene;    // probe face res (pow2 64..1024), 0 unset
  // Authored backdrop gradient (Scene_SkyColors-gated header payload; LWS
  // ZenithColor/SkyColor/GroundColor/NadirColor). SkyColorsSet != 0 when the
  // FLD carried the four colors; 0 = unset (legacy black void).
  int32_t       SkyColorsSet;
  FldColor      ZenithColor;        // straight up
  FldColor      SkyColor;           // upper sky
  FldColor      GroundColor;        // lower / toward horizon-ground
  FldColor      NadirColor;         // straight down
} FldScene;

extern FldScene *ReadFLD(const char *FileName);
extern Scene *ConvertFLD(FldScene *FLD);
// Returns the canonical (deduplicated) Material* — multiple FldMats with
// identical runtime-relevant fields collapse onto a single Material entry.
// May return an existing pointer rather than a freshly-allocated one.
extern Material *AddMaterial(FldMat *OrgMat, Scene *SceneGroup);
extern void Get_Mapping(Face *F,FldMat *Mat);
extern void DumpMaterialDedupStats();
