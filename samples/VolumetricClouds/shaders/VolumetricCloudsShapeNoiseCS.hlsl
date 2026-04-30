cbuffer PushConstants : register(b0)
{
    uint gOutputTextureIndex;
    uint gTextureSize;
    uint2 gPadding;
};

float Remap(float value, float originalMin, float originalMax, float newMin, float newMax)
{
    const float denominator = max(abs(originalMax - originalMin), 1.0e-5);
    const float normalized = saturate((value - originalMin) / denominator);
    return lerp(newMin, newMax, normalized);
}

float Hash11(float n)
{
    return frac(sin(n) * 43758.5453123);
}

float Hash31(float3 p)
{
    return frac(sin(dot(p, float3(127.1, 311.7, 74.7))) * 43758.5453123);
}

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

float ValueNoise(float3 uvw, float frequency)
{
    const float3 position = uvw * frequency;
    const float3 cell = floor(position);
    const float3 fraction = frac(position);
    const float3 smoothFraction = fraction * fraction * (3.0 - 2.0 * fraction);

    const float n000 = Hash31(WrapCell(cell + float3(0.0, 0.0, 0.0), frequency));
    const float n100 = Hash31(WrapCell(cell + float3(1.0, 0.0, 0.0), frequency));
    const float n010 = Hash31(WrapCell(cell + float3(0.0, 1.0, 0.0), frequency));
    const float n110 = Hash31(WrapCell(cell + float3(1.0, 1.0, 0.0), frequency));
    const float n001 = Hash31(WrapCell(cell + float3(0.0, 0.0, 1.0), frequency));
    const float n101 = Hash31(WrapCell(cell + float3(1.0, 0.0, 1.0), frequency));
    const float n011 = Hash31(WrapCell(cell + float3(0.0, 1.0, 1.0), frequency));
    const float n111 = Hash31(WrapCell(cell + float3(1.0, 1.0, 1.0), frequency));

    const float nx00 = lerp(n000, n100, smoothFraction.x);
    const float nx10 = lerp(n010, n110, smoothFraction.x);
    const float nx01 = lerp(n001, n101, smoothFraction.x);
    const float nx11 = lerp(n011, n111, smoothFraction.x);
    const float nxy0 = lerp(nx00, nx10, smoothFraction.y);
    const float nxy1 = lerp(nx01, nx11, smoothFraction.y);
    return lerp(nxy0, nxy1, smoothFraction.z);
}

float BillowyValueFbm(float3 uvw)
{
    float amplitude = 0.5;
    float frequency = 4.0;
    float sum = 0.0;
    float total = 0.0;

    [unroll]
    for (uint octave = 0; octave < 4u; ++octave)
    {
        const float noiseValue = abs(ValueNoise(uvw, frequency) * 2.0 - 1.0);
        sum += noiseValue * amplitude;
        total += amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }

    return sum / max(total, 1.0e-5);
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

    // The OpenGL sample builds a Perlin-Worley volume.
    // This sample uses billowy value-noise FBM plus tiled Worley octaves to keep the code compact.
    const float billowyBase = lerp(1.0, BillowyValueFbm(uvw), 0.5);
    const float worley0 = WorleyNoise(uvw, 4.0);
    const float worley1 = WorleyNoise(uvw, 8.0);
    const float worley2 = WorleyNoise(uvw, 16.0);
    const float perlinWorley = Remap(billowyBase, 0.0, 1.0, worley0, 1.0);

    outputTexture[dispatchThreadID] = float4(perlinWorley, worley0, worley1, worley2);
}
