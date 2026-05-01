cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProj;
    row_major float4x4 gWorld;
    float4 gMapParams;
    float4 gLighting;
    uint gTextureIndex;
    uint gUseTexture;
    uint2 gPadding;
};

SamplerState gSampler : register(s0);

float4 main(float4 position : SV_Position, float3 normal : NORMAL0, float4 color : COLOR0, float flatMode : TEXCOORD1, float2 uv : TEXCOORD2) : SV_Target0
{
    Texture2D<float4> mapTexture = ResourceDescriptorHeap[gTextureIndex];
    const float3 mapColor = gUseTexture != 0u ? mapTexture.Sample(gSampler, uv).rgb : color.rgb;

    if (flatMode > 0.5)
    {
        return float4(mapColor * 0.95, color.a);
    }

    const float diffuse = saturate(dot(normalize(normal), normalize(gLighting.xyz)) * 0.55 + 0.52);
    const float rim = pow(saturate(1.0 - abs(normal.z)), 2.0) * 0.18;
    return float4(mapColor * diffuse + rim.xxx, color.a);
}
