// NewPixie-inspired persistence pass. See THIRD_PARTY_NOTICES.md.

cbuffer PushConstants : register(b0)
{
    uint gSourceTextureIndex;
    uint gHistoryTextureIndex;
    float gPersistence;
    float gPadding;
};

SamplerState gSampler : register(s0);

float3 SampleClamped(Texture2D<float4> textureToSample, float2 uv)
{
    return textureToSample.SampleLevel(gSampler, saturate(uv), 0).rgb;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[gSourceTextureIndex];
    const float3 freshFrame = SampleClamped(sourceTexture, uv);
    if (gPersistence <= 0.0)
    {
        return float4(freshFrame, 1.0);
    }

    Texture2D<float4> historyTexture = ResourceDescriptorHeap[gHistoryTextureIndex];
    const float3 previousFrame = SampleClamped(historyTexture, uv);

    // A phosphor's bright parts persist longer than the dark parts. max() keeps
    // highlights alive for a short time without a blanket transparent overlay.
    return float4(max(freshFrame * 0.985, previousFrame * gPersistence), 1.0);
}
