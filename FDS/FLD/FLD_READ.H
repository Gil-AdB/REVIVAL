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
} FldScene;

extern Material *CurMat,*FirstMat;

extern FldScene *ReadFLD(const char *FileName);
extern Scene *ConvertFLD(FldScene *FLD);
extern void AddMaterial(FldMat *OrgMat,Scene *SceneGroup);
extern void Get_Mapping(Face *F,FldMat *Mat);
