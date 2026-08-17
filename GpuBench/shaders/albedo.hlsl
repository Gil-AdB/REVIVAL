// albedo.hlsl — Direct3D 11 HLSL shader for Phase 2 GpuBench (Geometry + Albedo)

cbuffer FrameUniforms : register(b0)
{
    float4 camRow0;
    float4 camRow1;
    float4 camRow2;
    float4 camSrc;
    float  sx, ox, sy, oy;
    float  dza, dzb, pad0, pad1;
};

cbuffer BatchUniforms : register(b1)
{
    float4 rotRow0;
    float4 rotRow1;
    float4 rotRow2;
    float4 objPos;
    float4 baseColor;
};

struct VS_INPUT
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct PS_INPUT
{
    float4 position   : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float3 viewNormal : TEXCOORD1;
};

Texture2D g_albedoTex : register(t0);
SamplerState g_sampler : register(s0);

PS_INPUT vs_albedo(VS_INPUT input)
{
    float3 wp = float3(dot(rotRow0.xyz, input.pos), dot(rotRow1.xyz, input.pos), dot(rotRow2.xyz, input.pos)) + objPos.xyz;
    float3 rel = wp - camSrc.xyz;
    float3 vp = float3(dot(camRow0.xyz, rel), dot(camRow1.xyz, rel), dot(camRow2.xyz, rel));

    PS_INPUT output;
    output.position.x = sx * vp.x + ox * vp.z;
    output.position.y = sy * vp.y + oy * vp.z;
    output.position.z = dza * vp.z + dzb;
    output.position.w = vp.z;
    output.uv = input.uv;

    float3 wn = float3(dot(rotRow0.xyz, input.normal), dot(rotRow1.xyz, input.normal), dot(rotRow2.xyz, input.normal));
    output.viewNormal = float3(dot(camRow0.xyz, wn), dot(camRow1.xyz, wn), dot(camRow2.xyz, wn));

    return output;
}

float4 ps_albedo(PS_INPUT input) : SV_TARGET
{
    float3 c = baseColor.rgb;
    if (baseColor.w > 0.5f) {
        c = g_albedoTex.Sample(g_sampler, input.uv).rgb;
    }
    float3 n = normalize(input.viewNormal);
    float shade = 0.35f + 0.65f * saturate(-n.z);
    return float4(c * shade, 1.0f);
}
