cbuffer BatchUniforms : register(b1) {
    float4 rotRow0;
    float4 rotRow1;
    float4 rotRow2;
    float4 objPos;
    float4 baseColor;
    float4 matParams;
};

cbuffer ShadowUniforms : register(b2) {
    float4 row0;
    float4 row1;
    float4 row2;
    float4 lightPos; // w = unused
    float4 projData; // x = dza, y = dzb, z = projScale, w = unused
};

struct VSInput {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float2 uv      : TEXCOORD;
    float4 tangent : TANGENT;
};

float3 rowmul(float4 r0, float4 r1, float4 r2, float3 v) {
    return float3(
        r0.x * v.x + r0.y * v.y + r0.z * v.z,
        r1.x * v.x + r1.y * v.y + r1.z * v.z,
        r2.x * v.x + r2.y * v.y + r2.z * v.z
    );
}

float4 main(VSInput input) : SV_Position {
    float3 wp = rowmul(rotRow0, rotRow1, rotRow2, input.pos) + objPos.xyz;
    float3 rel = wp - lightPos.xyz;
    float3 vp = rowmul(row0, row1, row2, rel);
    
    return float4(vp.x * projData.z, vp.y * projData.z,
                  projData.x * vp.z + projData.y, vp.z);
}
