cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProjection;
    float4 gUvScaleAndOffset; // xy: UV scale, zw: UV offset.
    uint gTextureIndex;
    uint gUseForcedMip;
    float gForcedMipLevel;
    uint gMipCount;
};

struct VSInput
{
    float3 position : POSITION0;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), gWorldViewProjection);
    output.uv = input.uv * gUvScaleAndOffset.xy + gUvScaleAndOffset.zw;
    return output;
}
