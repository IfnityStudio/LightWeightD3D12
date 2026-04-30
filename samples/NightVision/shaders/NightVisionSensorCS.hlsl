cbuffer PushConstants : register(b0)
{
    uint gVisibleTextureIndex;
    uint gThermalTextureIndex;
    uint gOutputTextureIndex;
    uint gTextureWidth;
    uint gTextureHeight;
    uint gSensorMode;
    uint gCompareSplit;
    uint gCircularMask;
    float gAmbientLight;
    float gLevel;
    float gGain;
    float gStefanBoltzmannConstant;
    float gEmissivityBlendFactor;
    float gNoiseStrength;
    float gBloomStrength;
    float gBlurStrength;
    float gMaskStrength;
    float gPatternStrength;
    float gTime;
};

SamplerState gSampler : register(s0);

float3 SampleSource(Texture2D<float4> sourceTexture, float2 uv)
{
    return sourceTexture.SampleLevel(gSampler, saturate(uv), 0).rgb;
}

float4 SampleThermal(Texture2D<float4> thermalTexture, float2 uv)
{
    return thermalTexture.SampleLevel(gSampler, saturate(uv), 0);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float Hash12(float2 value)
{
    return frac(sin(dot(value, float2(127.1, 311.7))) * 43758.5453123);
}

float FixedPattern(float2 uv)
{
    const float2 p = float2(uv.x * 1.1547005, uv.y + uv.x * 0.5) * 62.0;
    const float2 cell = abs(frac(p) - 0.5);
    const float edge = 1.0 - saturate(min(cell.x, cell.y) * 8.0);
    return edge;
}

float3 ApplyNightVision(Texture2D<float4> sourceTexture, Texture2D<float4> thermalTexture, float2 uv, float2 pixelSize)
{
    const float2 blurStep = pixelSize * lerp(0.75, 2.5, saturate(gBlurStrength));

    const float3 center = SampleSource(sourceTexture, uv);
    const float3 blurXPos = SampleSource(sourceTexture, uv + float2(blurStep.x, 0.0));
    const float3 blurXNeg = SampleSource(sourceTexture, uv - float2(blurStep.x, 0.0));
    const float3 blurYPos = SampleSource(sourceTexture, uv + float2(0.0, blurStep.y));
    const float3 blurYNeg = SampleSource(sourceTexture, uv - float2(0.0, blurStep.y));
    const float3 blurDiagA = SampleSource(sourceTexture, uv + blurStep);
    const float3 blurDiagB = SampleSource(sourceTexture, uv - blurStep);
    const float3 blurDiagC = SampleSource(sourceTexture, uv + float2(blurStep.x, -blurStep.y));
    const float3 blurDiagD = SampleSource(sourceTexture, uv + float2(-blurStep.x, blurStep.y));

    const float3 blurredColor =
        center * 0.28 +
        (blurXPos + blurXNeg + blurYPos + blurYNeg) * 0.12 +
        (blurDiagA + blurDiagB + blurDiagC + blurDiagD) * 0.06;

    float luminance = Luminance(blurredColor);
    const float thermalTemperature = SampleThermal(thermalTexture, uv).g;
    luminance += thermalTemperature * 0.08;

    const float chlorophyllMask = saturate((blurredColor.g - max(blurredColor.r, blurredColor.b)) * 1.8);
    luminance += chlorophyllMask * 0.28;

    const float gainScale = gGain * lerp(4.4, 1.2, saturate(gAmbientLight));
    float nightLuma = 1.0 - exp(-luminance * gainScale);

    const float contrast = lerp(0.62, 1.02, saturate(gAmbientLight));
    nightLuma = saturate((nightLuma - 0.5) * contrast + 0.5);

    const float highlightCenter = max(Luminance(center) - 1.0, 0.0);
    const float highlightBlur = (
        max(Luminance(blurXPos) - 0.9, 0.0) +
        max(Luminance(blurXNeg) - 0.9, 0.0) +
        max(Luminance(blurYPos) - 0.9, 0.0) +
        max(Luminance(blurYNeg) - 0.9, 0.0) +
        highlightCenter * 2.0) / 6.0;

    const float bloom = highlightBlur * gBloomStrength * 0.34;
    nightLuma += bloom;

    const float noiseValue = Hash12(floor(uv * float2(gTextureWidth, gTextureHeight)) + float2(gTime * 71.0, gTime * 131.0)) * 2.0 - 1.0;
    const float noiseAmount = gNoiseStrength * lerp(1.0, 0.22, saturate(gAmbientLight));
    nightLuma = saturate(nightLuma + noiseValue * noiseAmount * (0.35 + 0.65 * (1.0 - nightLuma)));

    const float pattern = FixedPattern(uv);
    nightLuma *= 1.0 - pattern * gPatternStrength * 0.35;

    float3 phosphor = nightLuma * float3(0.08, 1.00, 0.18);
    phosphor += bloom * float3(0.10, 0.82, 0.15);

    if (gCircularMask != 0u)
    {
        float2 centeredUv = uv * 2.0 - 1.0;
        centeredUv.x *= (float)gTextureWidth / (float)max(gTextureHeight, 1u);
        const float radius = length(centeredUv);
        const float mask = 1.0 - smoothstep(0.74, 0.94 + gMaskStrength * 0.12, radius);
        phosphor *= mask;
        phosphor += phosphor * (1.0 - mask) * 0.04;
    }

    const float scanline = 0.92 + 0.08 * sin((uv.y * gTextureHeight + gTime * 42.0) * 3.14159);
    phosphor *= scanline;
    return saturate(phosphor);
}

float3 ApplyInfrared(Texture2D<float4> visibleTexture, Texture2D<float4> thermalTexture, float2 uv)
{
    const float3 visibleColor = SampleSource(visibleTexture, uv);
    const float3 visibleDisplay = visibleColor / (1.0 + visibleColor);
    const float visibleLuminance = saturate(Luminance(visibleDisplay));
    const float4 thermalSample = SampleThermal(thermalTexture, uv);

    const float emissivityFromColor = (1.0 - visibleLuminance) * 0.15 + 0.84;
    const float finalEmissivity = lerp(thermalSample.r, emissivityFromColor, saturate(gEmissivityBlendFactor));

    const float materialTemperature = saturate(thermalSample.g);
    const float temperatureSquared = materialTemperature * materialTemperature;
    const float radiation = finalEmissivity * gStefanBoltzmannConstant * temperatureSquared * temperatureSquared;
    float mappedRadiation = radiation * gGain + gLevel;

    const float foliageBoost = thermalSample.b * 0.12;
    const float emitterBoost = thermalSample.a * 0.25;
    mappedRadiation += foliageBoost + emitterBoost;

    return saturate(mappedRadiation).xxx;
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= gTextureWidth || dispatchThreadID.y >= gTextureHeight)
    {
        return;
    }

    Texture2D<float4> visibleTexture = ResourceDescriptorHeap[gVisibleTextureIndex];
    Texture2D<float4> thermalTexture = ResourceDescriptorHeap[gThermalTextureIndex];
    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[gOutputTextureIndex];

    const float2 dimensions = float2((float)gTextureWidth, (float)gTextureHeight);
    const float2 pixelSize = 1.0 / dimensions;
    const float2 uv = ((float2)dispatchThreadID.xy + 0.5) * pixelSize;

    const float3 originalColor = SampleSource(visibleTexture, uv);
    const float3 originalDisplay = originalColor / (1.0 + originalColor);
    float3 processedColor = gSensorMode == 0u ?
        ApplyNightVision(visibleTexture, thermalTexture, uv, pixelSize) :
        ApplyInfrared(visibleTexture, thermalTexture, uv);

    if (gCompareSplit != 0u)
    {
        const float splitLine = abs(uv.x - 0.5) < pixelSize.x * 2.0 ? 1.0 : 0.0;
        processedColor = uv.x < 0.5 ? originalDisplay : processedColor;
        processedColor = lerp(processedColor, float3(1.0, 0.95, 0.25), splitLine);
    }

    outputTexture[dispatchThreadID.xy] = float4(processedColor, 1.0);
}
