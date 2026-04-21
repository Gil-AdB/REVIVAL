#include <atomic>
#include <cstdint>

#include "Base/FDS_DECS.H"

std::atomic<int32_t> Timer;
volatile char Keyboard[128];
int32_t Timer_Freq = 18;

char Keypressed()
{
    for (int32_t i = 128; i--;)
        if (Keyboard[i]) return 1;
    return 0;
}
