#ifndef REVIVAL_TEXTURE_H
#define REVIVAL_TEXTURE_H

#pragma pack(push, 1)

#include "Palette.h"

// Scene variables and structures
// [16 Bytes]
// Total: [16 Bytes+64K per Charperpixel,+1Kb if Palettized]
struct Texture
{
    Palette *Pal            = nullptr;   // Texture's Local Palette. Relevant only for 8BPP Textures
    byte *Data              = nullptr;     // Texture's Storage space.
    DWord BPP               = 0;      // Texture's BPP. Adjusts format in memory.
    // 0 used when Texture is invalid.
    // DWord size for Aligning, Should be treated as Byte.
    char* FileName          = 0; // Texture's Originating File-name.

    // OptClass - Will be used by Assign_Fillers to determine Raster device
    DWord OptClass          = 0; // 0 = 256x256 ; 1 = square 2^n ; 2 = arbitrary size.
    int32_t SizeX  = 0, SizeY  = 0;
	int32_t LSizeX = 0, LSizeY = 0; // log2(SizeX), log2(SizeY).

    // Mipmapping/blocktiling support.
	int32_t blockSizeX = 0, blockSizeY = 0; // zero to disable, 1<<blockSize measures length in pixels of each block
    byte* Mipmap[16]        = {nullptr}; // pointers to up to 16 levels of mipmaps
    dword numMipmaps        = 0; // number of mipmap levels.

    dword ID                = 0;
    dword Flags             = 0;
};

#pragma pack(pop)

#endif //REVIVAL_TEXTURE_H
