/* libm_probe.c - print hex-exact libm results for engine-relevant inputs.
   Build+run:  clang -O2 -o /tmp/libm_probe tools/libm_probe.c -lm && /tmp/libm_probe > /tmp/libm_probe.txt
   Compare the file between machines: any differing line = the system libm
   (dyld shared-cache CPU variant) computes differently on the two CPUs.
   sqrtf/fmaf are hardware instructions (control rows - must match). */
#include <stdio.h>
#include <math.h>
int main(void){
    float fovs[] = {58.1092072f/2*(float)M_PI/180.0f, 75.0f/2*(float)M_PI/180.0f, 0.507f, 1.0134f};
    float angs[] = {0.001f, 0.1963495f, 0.3926991f, 0.7853982f, 1.5707963f, 2.3561945f, 3.1415927f, 4.7123890f, 6.2831853f};
    float vals[] = {0.0025f, 0.070f, 0.164f, 0.3405f, 0.5471f, 1.0f, 2.7182818f, 10.9697f, 894.354f};
    unsigned i;
    printf("== tanf (projection setup)\n");
    for (i=0;i<sizeof(fovs)/sizeof(*fovs);i++) printf("tanf(%a) = %a\n", fovs[i], tanf(fovs[i]));
    printf("== sinf/cosf (camera, GTAO slices)\n");
    for (i=0;i<sizeof(angs)/sizeof(*angs);i++) printf("sincos(%a) = %a %a\n", angs[i], sinf(angs[i]), cosf(angs[i]));
    printf("== atan2f/acosf/expf/logf/powf\n");
    for (i=0;i<sizeof(vals)/sizeof(*vals);i++)
        printf("v=%a atan2f=%a acosf=%a expf=%a logf=%a powf=%a\n", vals[i],
               atan2f(vals[i],1.7f), acosf(vals[i]<1.f?vals[i]:0.5f), expf(vals[i]), logf(vals[i]), powf(vals[i],2.2f));
    printf("== hardware controls (must match)\n");
    for (i=0;i<sizeof(vals)/sizeof(*vals);i++)
        printf("v=%a sqrtf=%a fmaf=%a\n", vals[i], sqrtf(vals[i]), fmaf(vals[i],1.0000001f,0.0000001f));
    /* double-precision spot checks: the transform path mixes double in places */
    printf("== double tan/sin/cos\n");
    printf("tan(%la)=%la sin=%la cos=%la\n", 0.507, tan(0.507), sin(0.507), cos(0.507));
    return 0;
}
