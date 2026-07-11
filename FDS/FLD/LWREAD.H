/*
	Flood Demo System - LightWave reader - Header file
	--------------------------------------------------
*/


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
	Object_Parent						= 1,
	Object_PivotPoint				= 2,
	Object_PolygonSize			= 4,
	Object_AlignToPath			=	4096,
// End behaviors
	EndBehavior_Reset				=	256,
	EndBehavior_Stop				=	512,
	EndBehavior_Repeat			=	1024,
// Light flags
	Light_Parent						= 1,
	Light_TargetObject			= 2,
	Light_FallOff						= 4,
	Light_ConeAngle					= 8,
	Light_Range							= 16,
	Light_Distant   				= 32,
	Light_Point							=	64,
	Light_Spot							= 128,
	Light_LensFlare					= 256,
	// Authored volumetric beam (LWS "VolumetricLight 1", tools/lwsread).
	// Gates a conditional per-light gain float in the FLD light record —
	// see FLD_READ.CPP ReadLight. FLDs without the bit are byte-identical
	// to the pre-extension format. NOT 512: light Flags double as the
	// EndBehavior store (256/512/1024 — "EndBehavior 1" = 512 on nearly
	// every authored light), so 2048 is the first free bit.
	Light_VolumetricCone		= 2048,
// Camera flags
	Camera_Parent						= 1,
	Camera_TargetObject			= 2
};