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
	// Revival per-surface extension marker — set on the material FLAG word by
	// tools/lwsread (LWOREAD parsing the custom LWO SURF "RVSF" sub-chunk). Gates
	// a conditional per-material payload after the material record in the FLD (see
	// FLD_READ.CPP ReadMaterial). FLDs without the bit are byte-identical legacy.
	// 0x0800..0x8000 free in the 16-bit LWOB FLAG word; 0x4000 earmarked
	// Rev_SingleShadowId. See docs/SIDECAR_MIGRATION_PLAN.md §1a.
	Surf_RevExt					= 0x8000,
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
	// Authored per-light flare-sprite size multiplier (LWS "FdsFlareScale <f>",
	// tools/lwsread). Gates a conditional per-light gain float in the FLD light
	// record — see FLD_READ.CPP ReadLight, mapped to Omni::FlareScale in
	// FLD_CONV.CPP. FLDs without the bit are byte-identical to the pre-
	// extension format. 4096 is free on the LIGHT flag word (Object_AlignToPath
	// = 4096 is the OBJECT word; no authored LWS sets PathAlignment inside a
	// light section — object-section across every scene — so a light never
	// carries 4096), reused here in light context.
	Light_FlareScale			= 4096,
	// Authored SCENE-wide env-reflection defaults (LWS "FdsSceneEnvRefl" /
	// "FdsSceneEnvBakeRes", tools/lwsread). The FLD scene header has no
	// flags word, so this extension bit rides the AmbientIntensity
	// envelope's EndBehavior (legacy values top out at 1024 — 2048 free,
	// the Light_VolumetricCone idiom). Gates two conditional int32s after
	// the header's "OML" names triplet (EnvReflSceneMode, EnvBakeResScene);
	// ReadGeneralSceneData STRIPS the bit after reading so EndBehavior
	// consumers never see it. Bit-free FLDs are byte-identical legacy.
	Scene_EnvDefaults				= 2048,
	// Authored backdrop GRADIENT colors (LWS ZenithColor/SkyColor/GroundColor/
	// NadirColor, tools/lwsread). Same scene-header extension idiom as
	// Scene_EnvDefaults — the bit rides the AmbientIntensity envelope's
	// EndBehavior (4096 free; 2048 = env-defaults). Gates a 4-FldColor payload
	// after the env-defaults int32s; ReadGeneralSceneData STRIPS the bit after
	// reading. Bit-free FLDs byte-identical. Painted by default-off sky_gradient.
	Scene_SkyColors					= 4096,
// Camera flags
	Camera_Parent						= 1,
	Camera_TargetObject			= 2
};