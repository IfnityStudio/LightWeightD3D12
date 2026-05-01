cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProjection;
    float4 gUvScaleAndOffset; // xy: UV scale, zw: UV offset.
    uint gTextureIndex;
    uint gUseForcedMip;
    float gForcedMipLevel;
    uint gMipCount;
    uint gShowAutoMipDebug;
    float3 gPadding;
};

SamplerState gTextureSampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[gTextureIndex];

    if (gShowAutoMipDebug != 0)
    {
        const float2 dx = ddx(uv);
        const float2 dy = ddy(uv);
        uint width = 1;
        uint height = 1;
        uint levels = 1;
        sourceTexture.GetDimensions(0, width, height, levels);

        const float2 textureDx = dx * float2(width, height);
        const float2 textureDy = dy * float2(width, height);
        const float rho = max(length(textureDx), length(textureDy));
        const float autoLod = clamp(log2(max(rho, 1.0e-5)), 0.0, max(0.0, (float)levels - 1.0));
        const float normalizedLod = autoLod / max(1.0, (float)levels - 1.0);

        // Blue means detailed mips near 0, green/yellow means middle mips, red means low-resolution mips.
        const float3 color = lerp(float3(0.05, 0.25, 1.0), float3(0.0, 1.0, 0.25), saturate(normalizedLod * 2.0));
        const float3 hotColor = lerp(color, float3(1.0, 0.1, 0.0), saturate(normalizedLod * 2.0 - 1.0));
        return float4(hotColor, 1.0);
    }

    if (gUseForcedMip != 0)
    {
        return sourceTexture.SampleLevel(gTextureSampler, uv, gForcedMipLevel);
    }

    return sourceTexture.Sample(gTextureSampler, uv);
}
