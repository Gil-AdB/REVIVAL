//Flood scene Variables
typedef struct
{
  unsigned char Red,Green,Blue;
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
} FldMat;

typedef struct
{
  Vector        Position;
  Vector        Rotation;
  Vector        Scale;

  float         LinearValue;
  float         FrameNumber;
  float         Tension;
  float         Continuity;
  float         Bias;
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
  unsigned long Keys;
  unsigned long Channels;
  unsigned long EndBehavior;
} FldEnv;

typedef struct FldObj
{
  char          *Name;
  unsigned long  Flags;
  unsigned long  NumOfMat;
  FldMat        *Material;
  unsigned long  NumOfVerts;
  Vector        *Verts;
  unsigned long  NumOfFaces;
  FldFace       *Faces;
  unsigned long  Keys;
  FldKeyFrame   *KF;
  unsigned long  Parent;
  Vector         Pivot;
  FldEnv        *PolygonSize;
  FldObj        *Next;
} FldObject;

typedef struct FldLgt
{
  char          *Name;
  unsigned long  Flags;
  unsigned long  Keys;
  FldKeyFrame   *KF;
  FldEnv        *Color;
  FldEnv        *Intensity;
  unsigned long  Parent;
  unsigned long  TargetObject;
  FldEnv        *Falloff;
  FldEnv        *ConeAngle;
  FldLgt        *Next;
} FldLight;

typedef struct
{
  unsigned long  Flags;
  unsigned long  Keys;
  FldKeyFrame   *KF;
  unsigned long  Parent;
  unsigned long  TargetObject;
  FldEnv        *ZoomFactor;
} FldCamera;

typedef struct
{
  FldObject    *Object;
  FldLight     *Light;
  FldCamera    *Camera;
  unsigned long FirstFrame;
  unsigned long LastFrame;
  unsigned long FrameStep;
  float         FramesPerSecond;
  FldEnv       *AmbientColor;
	FldEnv       *AmbientIntensity;
} FldScene;

extern Material *CurMat,*FirstMat;

extern FldScene *ReadFLD(char *FileName);
extern Scene *ConvertFLD(FldScene *FLD);
extern void AddMaterial(FldMat *OrgMat,Scene *SceneGroup);
extern void Get_Mapping(Face *F,FldMat *Mat);
