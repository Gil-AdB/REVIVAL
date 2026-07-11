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
} FldScene;

#pragma pack (pop)
#endif