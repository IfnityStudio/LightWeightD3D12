cbuffer PushConstants : register(b0)
{
    uint gSourceTextureIndex;
    uint gDestinationTextureIndex;
    uint gSourceMipLevel;
    uint gDestinationWidth;
    uint gDestinationHeight;
    uint gWriteSrgb;
};

SamplerState gLinearClampSampler : register(s0);

float SrgbEncode(float value)
{
    value = saturate(value);
    return value <= 0.0031308 ? value * 12.92 : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= gDestinationWidth || dispatchThreadID.y >= gDestinationHeight)
    {
        return;
    }

    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[gSourceTextureIndex];
    RWTexture2D<float4> destinationTexture = ResourceDescriptorHeap[gDestinationTextureIndex];

    const float2 destinationSize = float2((float)gDestinationWidth, (float)gDestinationHeight);
    const float2 uv = ((float2)dispatchThreadID.xy + 0.5) / destinationSize;

    float4 color = sourceTexture.SampleLevel(gLinearClampSampler, uv, (float)gSourceMipLevel);
    if (gWriteSrgb != 0u)
    {
        color.rgb = float3(
            SrgbEncode(color.r),
            SrgbEncode(color.g),
            SrgbEncode(color.b));
    }

    destinationTexture[dispatchThreadID.xy] = color;
}
