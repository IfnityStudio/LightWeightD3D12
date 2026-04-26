cbuffer PushConstants : register(b0)
{
    uint gSourceTextureIndex;
};

SamplerState gSampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[gSourceTextureIndex];
    return float4(sourceTexture.SampleLevel(gSampler, uv, 0).rgb, 1.0);
}
