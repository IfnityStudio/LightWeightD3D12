cbuffer PushConstants : register(b0)
{
    uint gOutputTextureIndex;
    uint gTextureSize;
    uint2 gPadding;
};

float3 Hash33(float3 p)
{
    return frac(sin(float3(
        dot(p, float3(127.1, 311.7, 74.7)),
        dot(p, float3(269.5, 183.3, 246.1)),
        dot(p, float3(113.5, 271.9, 124.6)))) * 43758.5453123);
}

float3 WrapCell(float3 cell, float period)
{
    return fmod(cell + period, period);
}

float WorleyNoise(float3 uvw, float frequency)
{
    const float3 position = uvw * frequency;
    const float3 cell = floor(position);
    const float3 local = frac(position);
    float minimumDistanceSquared = 1.0e9;

    [unroll]
    for (int z = -1; z <= 1; ++z)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                const float3 neighbor = float3((float)x, (float)y, (float)z);
                const float3 wrappedCell = WrapCell(cell + neighbor, frequency);
                const float3 featurePoint = Hash33(wrappedCell);
                const float3 difference = neighbor + featurePoint - local;
                minimumDistanceSquared = min(minimumDistanceSquared, dot(difference, difference));
            }
        }
    }

    return 1.0 - saturate(sqrt(minimumDistanceSquared));
}

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= gTextureSize || dispatchThreadID.y >= gTextureSize || dispatchThreadID.z >= gTextureSize)
    {
        return;
    }

    RWTexture3D<float4> outputTexture = ResourceDescriptorHeap[gOutputTextureIndex];
    const float3 uvw = (float3(dispatchThreadID) + 0.5) / max((float)gTextureSize, 1.0);

    // The detail texture is pure high-frequency Worley noise used to erode the base cloud.
    const float worley0 = WorleyNoise(uvw, 8.0);
    const float worley1 = WorleyNoise(uvw, 16.0);
    const float worley2 = WorleyNoise(uvw, 32.0);

    outputTexture[dispatchThreadID] = float4(worley0, worley1, worley2, 0.0);
}
