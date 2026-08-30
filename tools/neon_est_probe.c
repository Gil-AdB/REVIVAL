/* neon_est_probe.c - hex-exact census of the ARM reciprocal/rsqrt ESTIMATE
   instructions (FRECPE/FRSQRTE), whose precision is implementation-defined
   and may legitimately differ between CPU generations. The engine's hot
   rasterizer uses approx_recipr (vectorclass -> simde -> vrecpeq_f32) for
   perspective division, so a table change between M2 and M5 would move
   every perspective-divided pixel by an LSB and can flip knife-edge tests.
   Build+run: clang -O2 -o /tmp/neon_est_probe tools/neon_est_probe.c && /tmp/neon_est_probe > /tmp/neon_est_m5.txt
   Diff vs docs/data/neon_est_m2max.txt */
#include <stdio.h>
#include <arm_neon.h>
int main(void){
    float bases[] = {0.0025f, 0.070f, 0.5f, 1.0f, 1.5f, 2.7182818f, 10.9697f, 894.354f, 65536.0f};
    unsigned i, j;
    printf("== FRECPE (vrecpeq_f32) raw estimates\n");
    for (i=0;i<sizeof(bases)/sizeof(*bases);i++){
        for (j=0;j<8;j++){
            float x = bases[i] * (1.0f + j*0.1234567f);
            float32x4_t v = vdupq_n_f32(x);
            float e = vgetq_lane_f32(vrecpeq_f32(v),0);
            float r = vgetq_lane_f32(vmulq_f32(vrecpeq_f32(v), vrecpsq_f32(v, vrecpeq_f32(v))),0);
            printf("x=%a est=%a nr1=%a\n", x, e, r);
        }
    }
    printf("== FRSQRTE (vrsqrteq_f32) raw estimates\n");
    for (i=0;i<sizeof(bases)/sizeof(*bases);i++){
        for (j=0;j<8;j++){
            float x = bases[i] * (1.0f + j*0.1234567f);
            float32x4_t v = vdupq_n_f32(x);
            float e = vgetq_lane_f32(vrsqrteq_f32(v),0);
            float32x4_t est = vrsqrteq_f32(v);
            float32x4_t nr = vmulq_f32(est, vrsqrtsq_f32(vmulq_f32(v,est), est));
            printf("x=%a est=%a nr1=%a\n", x, e, vgetq_lane_f32(nr,0));
        }
    }
    return 0;
}
