#ifndef _LWREAD_H
#define _LWREAD_H
/*
	Flood Demo System - LightWave reader - Header file
	--------------------------------------------------
*/
#include <cstdint>
#pragma pack (push, 1)				// No data alligning


enum LightWave_Constants
{
// Material flags
	Surf_Luminous   		 	  = 1,
	Surf_Outline    		 	  = 2,
	Surf_Smoothing  		 	  = 4,
	Surf_ColorHighlights 	  = 8,
	Surf_ColorFilter		 	  = 16,
	Surf_OpaqueEdge			 	  = 32,
	Surf_TransparentEdge 	  = 64,
	Surf_SharpTerminator 	  = 128,
	Surf_DoubleSided		 	  = 256,
	Surf_Additive				 	  = 512,
	Surf_ShadowAlpha			  = 1024,
	// Revival per-surface extension marker (custom LWO SURF sub-chunk "RVSF").
	// 0x0800..0x8000 are free in the 16-bit LWOB surface FLAG word (standard
	// LWOB uses only 1..1024). When set on FldMat::Flags, SaveMaterial writes a
	// conditional payload (u16 RevExtMask + only the authored fields) after the
	// material record — see FLDSAVE.CPP / docs/SIDECAR_MIGRATION_PLAN.md §1a.
	// FLDs whose materials don't carry the bit stay byte-identical to the
	// pre-extension format. 0x1000/0x2000/0x4000 reserved for future
	// boolean surface markers (0x4000 earmarked Rev_SingleShadowId).
	Surf_RevExt					= 0x8000,
	// Revival per-surface PBR map-role extension marker — set on the FLAG word
	// by tools/lwsread (LWOREAD parsing the custom LWO SURF "RVSM" sub-chunk).
	// Gates a SECOND conditional per-material payload (u16 RevMapMask +
	// NUL-terminated role paths + normalFlip byte) AFTER the RVSF numeric
	// payload — see FLDSAVE.CPP / FLD_READ.CPP. A SIBLING of RVSF (not folded
	// in): variable-length strings stay apart from the fixed-width numeric
	// parse, and the u16 RevExtMask lacks free bits for the 7 map fields.
	// 0x0800 is free in the LWOB FLAG word. See SIDECAR_MIGRATION_PLAN §1e.
	Surf_RevMaps				= 0x0800,
// Texture flags
	Texture_XAxis					  = 1,
	Texture_YAxis           = 2,
	Texture_ZAxis           = 4,
	Texture_WorldCoords     = 8,
	Texture_NegativeImage   = 16,
	Texture_PixelBlending   = 32,
	Texture_Antialiasing    = 64,
// Glossiness values
	Gloss_Low							  = 16,
	Gloss_Medium					  = 64,
	Gloss_High						  = 256,
	Gloss_Max							  = 1024,
// Reflection modes
	RefMode_BackdropOnly	  = 0,
	RefMode_RaytraceObjects = 1,
	RefMode_WrapImage				= 2,
	RefMode_WrapImgRayTrace	= 3,
// Object flags
	Object_Parent					= 1,
	Object_PivotPoint				= 2,
	Object_PolygonSize				= 4,
	Object_AlignToPath				= 4096,
// End behaviors
	EndBehavior_Reset				=	256,
	EndBehavior_Stop				=	512,
	EndBehavior_Repeat				=	1024,
// Light flags
	Light_Parent					= 1,
	Light_TargetObject				= 2,
	Light_FallOff					= 4,
	Light_ConeAngle					= 8,
	Light_Range						= 16,
	Light_Distant   				= 32,
	Light_Point						=	64,
	Light_Spot						= 128,
	Light_LensFlare					= 256,
	// Authored volumetric beam (LWS "VolumetricLight 1"). FLD extension bit:
	// SaveLight writes a conditional per-light gain float when set — FLDs
	// without the bit are byte-identical to the pre-extension format.
	// NOT 512: light Flags double as the EndBehavior store (ReadEndBehavior
	// ORs 256/512/1024 into CurLight->Flags — "EndBehavior 1" = 512 on
	// nearly every authored light), so 2048 is the first free bit.
	Light_VolumetricCone			= 2048,
	// Authored per-light flare-sprite size multiplier (LWS "FdsFlareScale <f>").
	// Decouples the lens-flare SPRITE size from the light's ISize
	// (F.FlareSize = ISize × FlareScale) so a bright engine glow keeps its
	// illumination while its flare shrinks below the ship silhouette. FLD
	// extension bit: SaveLight writes a conditional gain float when set — FLDs
	// without the bit are byte-identical to the pre-extension format. 4096 is
	// free on the LIGHT flag word (Object_AlignToPath = 4096 is the OBJECT
	// word; no authored LWS sets PathAlignment inside a light section —
	// verified object-section across city/greets/fountain/chase — so a light
	// never carries 4096), reused here in light context.
	Light_FlareScale				= 4096,
	// Authored SCENE-wide env-reflection defaults (LWS "FdsSceneEnvRefl -1|1"
	// / "FdsSceneEnvBakeRes N"). The FLD scene header has no flags word, so
	// the extension bit rides the AmbientIntensity envelope's EndBehavior
	// (legacy writers only ever store 0/256/512/1024 there — 2048 is free,
	// same idiom as Light_VolumetricCone). Payload = two conditional int32s
	// (EnvReflSceneMode, EnvBakeResScene) appended after the "OML" names
	// triplet; the loader STRIPS the bit after reading so EndBehavior
	// consumers never see it. FLDs without authored values are byte-
	// identical to the pre-extension format.
	Scene_EnvDefaults				= 2048,
	// Authored backdrop GRADIENT colors (LWS "ZenithColor"/"SkyColor"/
	// "GroundColor"/"NadirColor" — standard LightWave backdrop keys the 1998
	// pipeline dropped). Same scene-header extension idiom as Scene_EnvDefaults:
	// the bit rides the AmbientIntensity envelope's EndBehavior (legacy values
	// top out at 1024; 2048 taken by env-defaults; 4096 free). Payload = four
	// FldColor triples (12 bytes) after the env-defaults payload; the loader
	// STRIPS the bit after reading. FLDs without the keys are byte-identical.
	Scene_SkyColors					= 4096,
	// Camera flags
	Camera_Parent						= 1,
	Camera_TargetObject			= 2
};

typedef struct
{
	float X,Y,Z;
} Vector;

typedef struct
{
	unsigned char Red,Green,Blue;
} FldColor;

typedef struct
{
	unsigned short  FaceVerts;
	unsigned short *Vertex;
	signed   short  Surface;
} FldFace;

typedef struct
{
	char 					*Name;
	FldColor			 Color;
	unsigned short Flags;
	float          Luminosity;
	float          Diffuse;
	float          Specular;
	float          Reflection;
	float          Transparency;
	unsigned short Glossiness;
	unsigned short ReflectionMode;
	char					*ReflectionImage;
	float					 ReflectionSeamAngle;
	float					 RefractiveIndex;
	float					 EdgeTransparency;
	float					 MaxSmoothingAngle;
	char					*ColorTexture;
	char					*DiffuseTexture;
	char					*SpecularTexture;
	char					*ReflectionTexture;
	char					*TransparencyTexture;
	char					*BumpTexture;
	char					*TextureImage;
	unsigned short TextureFlags;
	Vector 				 TextureSize;
	Vector 				 TextureCenter;
	Vector 				 TextureFallOff;
	Vector 				 TextureVelocity;
	char					*TextureAlpha;
	unsigned short NoiseFrequencies;
	unsigned short TextureWrapX;
	unsigned short TextureWrapY;
	float					 AAStrength;
	float					 Opacity;
	float					 TFP0;
	float					 TFP1;
	// Revival per-surface extension (custom LWO SURF sub-chunk "RVSF" ->
	// Surf_RevExt(0x8000)-gated FLD material payload). RevExtMask bit i = field
	// i authored (only authored fields are serialized). Bit order MUST match the
	// LWO reader (LWOREAD ReadSurfaceRevExt), the FLD writer (FLDSAVE
	// SaveMaterial) and the engine FLD reader (FDS/FLD/FLD_READ.CPP ReadMaterial).
	// See docs/SIDECAR_MIGRATION_PLAN.md §1a.
	uint16_t       RevExtMask;   // 0 = no extension
	float          AoStrength;      // bit 0x001
	float          ParallaxScale;   // bit 0x002
	float          TintR;           // bit 0x004
	float          TintG;           // bit 0x008
	float          TintB;           // bit 0x010
	float          RefractIor;      // bit 0x020
	uint8_t        Refractive;      // bit 0x040 (-> Mat_Refractive)
	int8_t         EnvReflMode;     // bit 0x080 (-1/0/1)
	int32_t        EnvBakeRes;      // bit 0x100 (pow2 64..1024)
	int8_t         WaterProcMode;   // bit 0x200 (-1/0/1)
	uint8_t        EnvDynamic;      // bit 0x400 (0/1) — ENVDYN Workstream A1
	float          SpecMul;         // bit 0x800 — specular response multiplier
	// (bits 0x1000..0x8000 free for future response multipliers; maps -> RVSM)
	// Revival per-surface PBR map-SET extension (custom LWO SURF sub-chunk
	// "RVSM" -> Surf_RevMaps(0x0800)-gated FLD payload). Directory-per-set:
	// MapSet names a texture set; the engine resolves TEXTURES/PBR/<set>/
	// <role>.png at FLD time via MaterialImport_ApplyMapFile (§1e). RevMapMask
	// bit i = field i authored. Bit order MUST match tools/lwopatch RVSM_FIELDS,
	// FLDSAVE SaveMaterial and the engine reader (FDS/FLD/FLD_READ.CPP).
	// NormalFlip pairs with the normal map (green-channel convention).
	uint16_t       RevMapMask;      // 0 = no map extension
	char          *MapSet;          // bit 0x01 (texture set name)
	uint8_t        NormalFlip;      // bit 0x40 (0/1)
} FldMat;

typedef struct
{
	Vector				Position;
	Vector				Rotation;
	Vector				Scale;

	float					LinearValue;
	float					FrameNumber;
	float					Tension;
	float					Continuity;
	float					Bias;
}	FldKeyFrame;

typedef struct
{
	float				 *Channel;
	float					FrameNumber;
	float					LinearValue;
	float					Tension;
	float					Continuity;
	float					Bias;
}	FldEnvKey;

typedef struct
{
	FldEnvKey		 *Key;
	uint32_t      Keys;
	uint32_t      Channels;
	uint32_t     	EndBehavior;
}	FldEnv;

typedef struct FldObj
{
	char				  *Name;
	uint32_t       Flags;
	uint32_t       NumOfMat;
	FldMat    	  *Material;
	uint32_t       NumOfVerts;
	Vector        *Vertex;
	uint32_t       NumOfFaces;
	FldFace       *Face;
	uint32_t     	 Keys;
	FldKeyFrame		*KF;
	uint32_t     	 Parent;
	Vector				 PivotPoint;
	FldEnv				*PolygonSize;
	FldObj   			*Next;
} FldObject;

typedef struct FldLgt
{
	char					*Name;
	uint32_t     	 Flags;
	uint32_t     	 Keys;
	FldKeyFrame		*KF;
	FldColor				Color;
	FldEnv				*Intensity;
	uint32_t     	 Parent;
	uint32_t       TargetObject;
	FldEnv				*Falloff;
	FldEnv				*ConeAngle;
	FldEnv				*Range;
	// Per-light volumetric beam gain (LWS "VolumetricLightIntensity").
	// Only serialized when Flags carries Light_VolumetricCone (2048);
	// 0 = unset → engine treats as 1.0.
	float					 VolBeamGain;
	// Per-light flare-sprite size multiplier (LWS "FdsFlareScale").
	// Only serialized when Flags carries Light_FlareScale (4096);
	// 0 = unset → engine treats as 1.0 (Omni::FlareScale convention).
	float					 FlareScale;
	FldLgt				*Next;
} FldLight;

typedef struct
{
	uint32_t       Flags;
	uint32_t     	 Keys;
	FldKeyFrame		*KF;
	uint32_t       Parent;
	uint32_t       TargetObject;
	FldEnv				*ZoomFactor;
} FldCamera;

typedef struct
{
	FldObject		 *Object;
	FldLight		 *Light;
	FldCamera 	 *Camera;
	uint32_t      FirstFrame;
	uint32_t      LastFrame;
	uint32_t      FrameStep;
	float					FramesPerSecond;
	FldEnv		   *AmbientColor;
	FldEnv       *AmbientIntensity;
	// Authored scene-wide env-reflection defaults (LWS "FdsSceneEnvRefl" /
	// "FdsSceneEnvBakeRes"; 0 = unset). Serialized only when either is
	// nonzero, as a Scene_EnvDefaults-gated conditional payload — see the
	// enum note above.
	int32_t       EnvReflSceneMode;   // -1 none / 0 auto / 1 all
	int32_t       EnvBakeResScene;    // probe face res (pow2 64..1024), 0 = unset
	// Authored backdrop gradient (LWS ZenithColor/SkyColor/GroundColor/
	// NadirColor). SkyColorsSet != 0 when any of the four was parsed →
	// serialized as a Scene_SkyColors-gated conditional payload (4 FldColor).
	int32_t       SkyColorsSet;
	FldColor      ZenithColor;        // straight up
	FldColor      SkyColor;           // upper sky
	FldColor      GroundColor;        // lower / toward horizon-ground
	FldColor      NadirColor;         // straight down
} FldScene;

#pragma pack (pop)
#endif