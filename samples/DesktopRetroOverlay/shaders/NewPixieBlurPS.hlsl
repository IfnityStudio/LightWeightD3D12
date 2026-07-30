// NewPixie-inspired separable blur pass. See THIRD_PARTY_NOTICES.md.

cbuffer PushConstants : register(b0)
{
    uint gSourceTextureIndex;
    uint gUnusedTextureIndex;
    float gStepX;
    float gStepY;
};

SamplerState gSampler : register(s0);

float3 SampleClamped(Texture2D<float4> textureToSample, float2 uv)
{
    return textureToSample.SampleLevel(gSampler, saturate(uv), 0).rgb;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[gSourceTextureIndex];
    const float2 stepOffset = float2(gStepX, gStepY);

    // Symmetric five-tap gaussian approximation. Horizontal and vertical
    // invocations turn it into a broad, inexpensive bloom buffer.
    float3 color = SampleClamped(sourceTexture, uv) * 0.40;
    color += (SampleClamped(sourceTexture, uv - stepOffset) +
        SampleClamped(sourceTexture, uv + stepOffset)) * 0.24;
    color += (SampleClamped(sourceTexture, uv - 2.0 * stepOffset) +
        SampleClamped(sourceTexture, uv + 2.0 * stepOffset)) * 0.06;
    return float4(color, 1.0);
}
