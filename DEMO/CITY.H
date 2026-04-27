// definitions, constants, etc
//#define CITY_RAIN

#include <cstdint>

// Total Timer ticks the City scene plays for; tick() returns false once
// Timer reaches this. Exposed so the snapshot harness can pick valid
// Timer values without having to recompute the formula.
int32_t getCityCTPartTime();
