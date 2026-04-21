#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"

#include <map>

using namespace std;
map<uintptr_t, uintptr_t> g_AlignedBlockMap;

void *getAlignedBlock(uintptr_t size, uintptr_t alignment)
{
	uintptr_t addr = (uintptr_t)malloc(size+alignment);
	uintptr_t aligned = (addr + alignment-1)&(~(alignment-1));
	
	//g_AlignedBlockMap.insert(pair<mword, mword> ();
	g_AlignedBlockMap[aligned] = addr;
	return (void *)aligned;
}

void freeAlignedBlock(void *ptr)
{
	uintptr_t aligned = (uintptr_t)ptr;
	bool exists = g_AlignedBlockMap.find(aligned) != g_AlignedBlockMap.end();
	uintptr_t addr;
	if (exists)
	{
		addr = g_AlignedBlockMap[aligned];
	} else {
		addr = aligned;
	}

	if (!addr) return;
	free((void *)addr);
}

