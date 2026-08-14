// Phase 2 of the standalone GPU benchmark (docs/GPU_BENCHMARK_PLAN.md):
// geometry + albedo textures, NO scene lighting.
//
// Compiled at RUNTIME via newLibraryWithSource:. This machine has Command Line
// Tools only, so the offline `metal` compiler is absent — runtime compilation was
// measured working and is what we rely on.
//
// Matrices arrive as three float3 ROWS and are applied as row-dot-vector,
// matching FDS's `Matrix` (float[3][3], row-major) and MatrixXVector exactly.
// Passing rows and dotting explicitly avoids any column-major/row-major
// ambiguity with MSL's float3x3.

#include <metal_stdlib>
using namespace metal;

struct FrameUniforms {
    // View matrix rows (Camera::Mat, produced by FDS's Kick_Camera).
    float3 camRow0;
    float3 camRow1;
    float3 camRow2;
    float3 camSrc;
    // Projection, derived on the CPU so the pixel mapping is IDENTICAL to the
    // engine's:  px = cntrEX + (X/Z)*FOVX ,  py = cntrEY - (Y/Z)*FOVY
    // becomes    clip.x = sx*X + ox*Z ,  clip.y = sy*Y + oy*Z ,  clip.w = Z
    float  sx, ox;
    float  sy, oy;
    // Reversed-Z depth:  clip.z = dza*Z + dzb   (ndc 1 at near, 0 at far)
    float  dza, dzb;
};

struct BatchUniforms {
    float3 rotRow0;
    float3 rotRow1;
    float3 rotRow2;
    float3 objPos;
    float4 baseColor;   // .rgb authored base colour, .w = 1 when a texture is bound
};

struct VertexIn {
    float3 pos    [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv     [[attribute(2)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
    float3 viewNormal;
};

static inline float3 rowmul(float3 r0, float3 r1, float3 r2, float3 v) {
    return float3(dot(r0, v), dot(r1, v), dot(r2, v));
}

vertex VertexOut vs_albedo(VertexIn in [[stage_in]],
                           constant FrameUniforms &u [[buffer(1)]],
                           constant BatchUniforms &b [[buffer(2)]])
{
    // Object -> world. Animate_Objects already folded IScale into the rot rows
    // and resolved the parent hierarchy into objPos, so this is the whole
    // model transform.
    const float3 wp  = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, in.pos) + b.objPos;
    const float3 rel = wp - u.camSrc;
    const float3 vp  = rowmul(u.camRow0, u.camRow1, u.camRow2, rel);

    VertexOut o;
    o.position.x = u.sx * vp.x + u.ox * vp.z;
    o.position.y = u.sy * vp.y + u.oy * vp.z;
    o.position.z = u.dza * vp.z + u.dzb;
    o.position.w = vp.z;
    o.uv = in.uv;

    const float3 wn = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, in.normal);
    o.viewNormal = rowmul(u.camRow0, u.camRow1, u.camRow2, wn);
    return o;
}

fragment float4 fs_albedo(VertexOut in [[stage_in]],
                          constant BatchUniforms &b [[buffer(2)]],
                          texture2d<float> albedo   [[texture(0)]],
                          sampler          samp     [[sampler(0)]])
{
    float3 c = b.baseColor.rgb;
    if (b.baseColor.w > 0.5f) c = albedo.sample(samp, in.uv).rgb;

    // A fixed camera-facing term. This is NOT scene lighting — there are no
    // scene lights in this pass. It is a one-dot-product depth cue so the render
    // is verifiable by eye (a pure albedo blit makes every wall of the room look
    // identical and hides whether the geometry is even correct). Two FMAs; it
    // cannot move the benchmark.
    const float3 n = normalize(in.viewNormal);
    const float shade = 0.35f + 0.65f * saturate(-n.z);
    return float4(c * shade, 1.0f);
}
