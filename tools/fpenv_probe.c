/* fpenv_probe.c - FP environment + complete estimate tables + mass libm hashes.
   Build+run: clang -O2 -o /tmp/fpenv_probe tools/fpenv_probe.c -lm && /tmp/fpenv_probe > /tmp/fpenv_m5.txt
   Diff vs docs/data/fpenv_m2max.txt. Any differing line names the mechanism. */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <arm_neon.h>
static uint64_t fnv = 1469598103934665603ull;
static void H(uint32_t v){ fnv ^= v; fnv *= 1099511628211ull; }
static uint32_t B(float f){ union{float f;uint32_t u;}c; c.f=f; return c.u; }
int main(void){
    uint64_t fpcr, fpsr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    printf("FPCR=%016llx FPSR(masked FZ bits)=%016llx\n",(unsigned long long)fpcr,(unsigned long long)(fpsr&0x0));
    /* denormal behavior, scalar and vector */
    volatile float tiny = 1.0e-42f, tiny2 = 3.0e-42f;
    printf("denorm scalar: t*0.5=%a t+t2=%a t/3=%a\n", tiny*0.5f, tiny+tiny2, tiny/3.0f);
    float32x4_t vt=vdupq_n_f32(1.0e-42f);
    printf("denorm neon:   t*0.5=%a t+t=%a\n", vgetq_lane_f32(vmulq_n_f32(vt,0.5f),0), vgetq_lane_f32(vaddq_f32(vt,vt),0));
    /* complete FRECPE table: all 512 mantissa entries at exponent 0 */
    fnv=1469598103934665603ull;
    for (int i=0;i<512;i++){ union{uint32_t u;float f;}c; c.u=0x3F800000u|(uint32_t)i<<14; float32x4_t v=vdupq_n_f32(c.f); H(B(vgetq_lane_f32(vrecpeq_f32(v),0))); }
    printf("FRECPE table hash (512 entries, exp 0): %016llx\n",(unsigned long long)fnv);
    /* complete FRSQRTE table: 512 entries across both exponent parities */
    fnv=1469598103934665603ull;
    for (int p=0;p<2;p++) for (int i=0;i<256;i++){ union{uint32_t u;float f;}c; c.u=((p?0x40000000u:0x3F800000u))|(uint32_t)i<<15; float32x4_t v=vdupq_n_f32(c.f); H(B(vgetq_lane_f32(vrsqrteq_f32(v),0))); }
    printf("FRSQRTE table hash (512 entries): %016llx\n",(unsigned long long)fnv);
    /* mass libm sweeps: 1M inputs per function, hashed */
    #define SWEEP(name, expr) { fnv=1469598103934665603ull; for (uint32_t k=0;k<1048576u;k++){ union{uint32_t u;float f;}c; c.u=0x38000000u + k*0x71u; float x=c.f; H(B(expr)); } printf("%-8s sweep hash: %016llx\n", name, (unsigned long long)fnv); }
    SWEEP("tanf",   tanf(x))
    SWEEP("sinf",   sinf(x))
    SWEEP("cosf",   cosf(x))
    SWEEP("expf",   expf(x))
    SWEEP("logf",   logf(fabsf(x)+1e-30f))
    SWEEP("acosf",  acosf(fmodf(x,1.0f)))
    SWEEP("atan2f", atan2f(x,1.7f))
    SWEEP("powf",   powf(fabsf(x)+1e-6f,1.3f))
    SWEEP("fmodf",  fmodf(x,3.7f))
    SWEEP("tanhf",  tanhf(x))
    /* double spot sweep */
    fnv=1469598103934665603ull;
    for (uint32_t k=0;k<262144u;k++){ double x = 1e-3*(double)k + 1e-7; union{double d;uint64_t u;}c; c.d=tan(x)+sin(x)+cos(x)+log(x)+exp(-x); H((uint32_t)(c.u&0xffffffffu)); H((uint32_t)(c.u>>32)); }
    printf("double combo sweep hash: %016llx\n",(unsigned long long)fnv);
    return 0;
}
