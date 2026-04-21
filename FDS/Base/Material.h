#ifndef REVIVAL_MATERIAL_H
#define REVIVAL_MATERIAL_H

#include "Color.h"
#include "Texture.h"
#include "Vector.h"

#pragma pack(push, 1)

struct Scene;

struct Material
{
    // 16 byte alignment variables
    Color                 BaseCol; //Base Color for the material

    Scene               * RelScene              = nullptr; //Related Scene  // This should be nuked from orbit. Just keep a scene id instead of a pointer
    Texture             * Txtr                  = nullptr;
    DWord                 Flags                 = 0;
    DWord                 TFlags                = 0; //Texture Flags

    dword				  ID                    = 0;

    float                 Luminosity            = 0.0f;
    float                 Diffuse               = 0.0f; //Diffuse reflection
    float                 Specular              = 0.0f; //Specular reflection
    float                 Reflection            = 0.0f; //Rebounded light Reflection
    float                 Transparency          = 0.0f; //Transparency ratio
    unsigned short        Glossiness            = 0; //unknown parameter
    unsigned short        ReflectionMode        = 0; //unknown parameter
    char                * ReflectionImage       = nullptr; //Reflection detail
    float                 ReflectionSeamAngle   = 0.0f; //unknown
    float                 RefractiveIndex       = 0.0f; //Optic Factor
    float                 EdgeTransparency      = 0.0f; //unknown factor
    float                 MaxSmoothingAngle     = 0.0f; //?
    char                * ColorTexture          = nullptr; //Color Map Textutre Filename
    char                * DiffuseTexture        = nullptr; //Diffuse Map Texture Filename
    char                * SpecularTexture       = nullptr; //Specular Map Texture Filename
    char                * ReflectionTexture     = nullptr; //Reflection Map Texture Filename
    char                * TransparencyTexture   = nullptr; //Transparency Map Texture Filename
    char                * BumpTexture           = nullptr; //Bump Map Texture Filename
    char                * TextureImage          = nullptr; //Texture Map Filename
    unsigned short        TextureFlags          = 0; //Should be replaced by dword TFlags
    Vector                TextureSize; //Texture Proj. Size in spatial coordinates
    Vector                TextureCenter; //Texture Origin in space
    Vector                TextureFallOff; //??
    Vector                TextureVelocity; //U/V animation speed
    char                * TextureAlpha          = nullptr; //Texture Map for Alpha Channel
    unsigned short        NoiseFrequencies      = 0; //??
    unsigned short        TextureWrapX          = 0; //Wrap flag for U
    unsigned short        TextureWrapY          = 0; //Wrap flag for V
    float                 AAStrength            = 0.0f; //Antialiasing Strength
    float                 Opacity               = 0.0f; //?? gotta learn some physics
    Texture				* EnvTexture            = nullptr;
    float				  TFP0                  = 0.0f;
    float				  TFP1                  = 0.0f;
    bool                  ZBufferWrite          = true;
    bool                  ZBufferTest           = true;
    Material            * Next                  = nullptr;
    Material            * Prev                  = nullptr;
    char                * Name                  = nullptr;
};

#pragma pack(pop)

#endif //REVIVAL_MATERIAL_H
