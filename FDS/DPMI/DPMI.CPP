#include <cstdint>
#include <cstdlib>

#include "Base/FDS_DECS.H"

dword DPMI_Free_Memory(void)
{
    return 100000000;
}

static char *AlignedBuffer;
static char *AlignedPtr;
static int32_t AlignedBlocks;

void Init_Aligned(int32_t Amount)
{
    AlignedBuffer = (char *)malloc(65536 * (Amount + 1));
    AlignedBlocks = Amount;
    AlignedPtr = (char *)(((uintptr_t)AlignedBuffer + 65535) & (~65535));
}

void *Get_Aligned_Block()
{
    char *r = AlignedPtr;
    AlignedPtr += 65536;
    return r;
}
