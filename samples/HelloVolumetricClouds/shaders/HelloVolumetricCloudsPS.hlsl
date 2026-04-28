cbuffer PushConstants : register(b0)
{
    row_major float4x4 gInvViewProjection;
    float4 gCameraPositionAndTime;
    float4 gPlanetCenterAndRadius;
    float4 gCloudLayer;
    float4 gWindDirectionAndSpeed;
    float4 gNoiseSampling;
    float4 gLighting0;
    float4 gSunDirectionAndLightFactor;
    float4 gColors0;
    float4 gColors1;
    float4 gColors2;
    uint4 gTextureIndices;
};

SamplerState gSampler : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct Ray
{
    float3 origin;
    float3 direction;
};

float RemapUnclamped(float value, float originalMin, float originalMax, float newMin, float newMax)
{
    const float denominator = originalMax - originalMin;
    if (abs(denominator) < 1.0e-5)
    {
        return newMin;
    }

    const float normalized = (value - originalMin) / denominator;
    return newMin + normalized * (newMax - newMin);
}

float RemapToUnit(float value, float originalMin, float originalMax)
{
    return saturate(RemapUnclamped(value, originalMin, originalMax, 0.0, 1.0));
}

float InterleavedGradientNoise(float2 pixelPosition, uint frameIndex)
{
    const float2 jitteredPixel = pixelPosition + (float)frameIndex;
    return frac(52.9829189 * frac(dot(jitteredPixel, float2(0.06711056, 0.00583715))));
}

Ray GenerateViewRay(float2 uv)
{
    const float2 clipXY = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 target = mul(float4(clipXY, 1.0, 1.0), gInvViewProjection);
    target /= max(target.w, 1.0e-5);

    Ray ray;
    ray.origin = gCameraPositionAndTime.xyz;
    ray.direction = normalize(target.xyz - ray.origin);
    return ray;
}

bool IntersectSphere(Ray ray, float3 sphereCenter, float sphereRadius, out float tNear, out float tFar)
{
    const float3 offset = ray.origin - sphereCenter;
    const float a = dot(ray.direction, ray.direction);
    const float b = 2.0 * dot(ray.direction, offset);
    const float c = dot(offset, offset) - sphereRadius * sphereRadius;
    const float discriminant = b * b - 4.0 * a * c;

    if (discriminant < 0.0)
    {
        tNear = 0.0;
        tFar = 0.0;
        return false;
    }

    const float sqrtDiscriminant = sqrt(discriminant);
    const float inverseDenominator = 0.5 / a;
    tNear = (-b - sqrtDiscriminant) * inverseDenominator;
    tFar = (-b + sqrtDiscriminant) * inverseDenominator;

    if (tNear > tFar)
    {
        const float swapValue = tNear;
        tNear = tFar;
        tFar = swapValue;
    }

    return tFar > 0.0;
}

bool BuildCloudSegment(Ray ray, out float segmentStart, out float segmentEnd)
{
    const float innerRadius = gPlanetCenterAndRadius.w + gCloudLayer.x;
    const float outerRadius = gPlanetCenterAndRadius.w + gCloudLayer.y;

    float outerNear = 0.0;
    float outerFar = 0.0;
    if (!IntersectSphere(ray, gPlanetCenterAndRadius.xyz, outerRadius, outerNear, outerFar))
    {
        segmentStart = 0.0;
        segmentEnd = 0.0;
        return false;
    }

    segmentStart = max(outerNear, 0.0);
    segmentEnd = outerFar;

    const float cameraDistance = distance(ray.origin, gPlanetCenterAndRadius.xyz);
    if (cameraDistance < innerRadius)
    {
        float innerNear = 0.0;
        float innerFar = 0.0;
        if (!IntersectSphere(ray, gPlanetCenterAndRadius.xyz, innerRadius, innerNear, innerFar))
        {
            return false;
        }

        segmentStart = max(innerFar, 0.0);
    }
    else if (cameraDistance < outerRadius)
    {
        float innerNear = 0.0;
        float innerFar = 0.0;
        if (IntersectSphere(ray, gPlanetCenterAndRadius.xyz, innerRadius, innerNear, innerFar) && innerNear > 0.0)
        {
            segmentEnd = min(segmentEnd, innerNear);
        }

        segmentStart = 0.0;
    }
    else
    {
        float innerNear = 0.0;
        float innerFar = 0.0;
        if (IntersectSphere(ray, gPlanetCenterAndRadius.xyz, innerRadius, innerNear, innerFar) && innerNear > 0.0)
        {
            segmentEnd = min(segmentEnd, innerNear);
        }
    }

    return segmentEnd > segmentStart;
}

float HeightFractionForPoint(float3 position)
{
    const float innerRadius = gPlanetCenterAndRadius.w + gCloudLayer.x;
    const float cloudThickness = max(gCloudLayer.y - gCloudLayer.x, 1.0e-3);
    const float altitudeInsideLayer = distance(position, gPlanetCenterAndRadius.xyz) - innerRadius;
    return saturate(altitudeInsideLayer / cloudThickness);
}

float DensityHeightGradient(float heightFraction)
{
    // A simple cumulus profile: quick fade-in near the base and stronger erosion near the top.
    const float bottom = RemapToUnit(heightFraction, 0.02, 0.18);
    const float top = RemapToUnit(heightFraction, 1.0, 0.72);
    return bottom * top;
}

float SampleCloudDensity(float3 position, float heightFraction, bool useDetailNoise)
{
    Texture3D<float4> shapeTexture = ResourceDescriptorHeap[gTextureIndices.x];
    Texture3D<float4> detailTexture = ResourceDescriptorHeap[gTextureIndices.y];

    const float3 windOffset = (gWindDirectionAndSpeed.xyz + float3(0.0, 0.05, 0.0)) * gWindDirectionAndSpeed.w * gCameraPositionAndTime.w * 0.0012;
    float3 noisePosition = position + gWindDirectionAndSpeed.xyz * gColors1.w * heightFraction;

    const float3 baseNoiseCoord = frac(noisePosition * (0.00215 * gNoiseSampling.x) + windOffset);
    const float4 lowFrequencyNoises = shapeTexture.SampleLevel(gSampler, baseNoiseCoord, 0.0);
    const float lowFreqFbm = lowFrequencyNoises.y * 0.625 + lowFrequencyNoises.z * 0.25 + lowFrequencyNoises.w * 0.125;

    float baseCloud = RemapToUnit(lowFrequencyNoises.x, 1.0 - lowFreqFbm, 1.0);
    baseCloud *= DensityHeightGradient(heightFraction);

    const float coverageThreshold = 1.0 - gCloudLayer.z;
    float baseCloudWithCoverage = RemapToUnit(baseCloud, coverageThreshold, 1.0);
    baseCloudWithCoverage *= gCloudLayer.z;

    if (baseCloudWithCoverage <= 0.0)
    {
        return 0.0;
    }

    if (!useDetailNoise)
    {
        return baseCloudWithCoverage;
    }

    // Distort the detail sampling a little so the cloud underside does not look static.
    const float2 turbulence = (shapeTexture.SampleLevel(gSampler, frac(baseNoiseCoord.zxy + 0.37), 0.0).yz * 2.0 - 1.0);
    noisePosition.xz += turbulence * (1.0 - heightFraction) * gNoiseSampling.w;

    const float3 detailNoiseCoord = frac(noisePosition * (0.0095 * gNoiseSampling.y) + windOffset * 1.9);
    const float3 highFrequencyNoises = detailTexture.SampleLevel(gSampler, detailNoiseCoord, 0.0).xyz;
    const float highFreqFbm = highFrequencyNoises.x * 0.625 + highFrequencyNoises.y * 0.25 + highFrequencyNoises.z * 0.125;
    const float highFreqModifier = lerp(1.0 - highFreqFbm, highFreqFbm, saturate(heightFraction * 1.3));

    return RemapToUnit(baseCloudWithCoverage, highFreqModifier * gNoiseSampling.z, 1.0);
}

float SampleCloudDensityAlongCone(float3 position, float3 lightDirection)
{
    static const float3 kNoiseKernel[6] =
    {
        float3(-0.6, -0.8, -0.2),
        float3( 1.0, -0.3,  0.0),
        float3(-0.7,  0.0,  0.7),
        float3(-0.2,  0.6, -0.8),
        float3( 0.4,  0.3,  0.9),
        float3(-0.2,  0.6, -0.8)
    };

    float densityAlongCone = 0.0;

    [unroll]
    for (uint sampleIndex = 0u; sampleIndex < 6u; ++sampleIndex)
    {
        position += lightDirection * gLighting0.x;
        const float3 offsetPosition = position + kNoiseKernel[sampleIndex] * gLighting0.x * gLighting0.y * (float)(sampleIndex + 1u);
        const float heightFraction = HeightFractionForPoint(offsetPosition);
        densityAlongCone += SampleCloudDensity(offsetPosition, heightFraction, sampleIndex < 2u);
    }

    position += 28.0 * gLighting0.x * lightDirection;
    densityAlongCone += SampleCloudDensity(position, HeightFractionForPoint(position), false) * 2.6;

    return densityAlongCone;
}

float BeerLambert(float density)
{
    return exp(-density * gLighting0.z);
}

float BeerPowder(float density)
{
    const float d = -density * gLighting0.z;
    return max(exp(d), exp(d * 0.5) * 0.7);
}

float HenyeyGreensteinPhase(float cosAngle, float g)
{
    const float gSquared = g * g;
    const float denominator = pow(max(1.0 + gSquared - 2.0 * g * cosAngle, 1.0e-4), 1.5);
    return ((1.0 - gSquared) / denominator) / (4.0 * 3.14159265);
}

float PowderEffect(float density, float cosAngle)
{
    const float powder = 1.0 - exp(-density * 2.0);
    return lerp(1.0, powder, saturate((-cosAngle * 0.5) + 0.5));
}

float CalculateLightEnergy(float coneDensity, float cosAngle, float powderDensity)
{
    const float beerPowder = 2.0 * BeerPowder(coneDensity) * PowderEffect(powderDensity, cosAngle);
    const float forwardPhase = HenyeyGreensteinPhase(cosAngle, 0.65);
    const float backwardPhase = HenyeyGreensteinPhase(cosAngle, -0.28);
    const float phaseBlend = max(forwardPhase, backwardPhase) * 0.07 + 0.8;
    return beerPowder * phaseBlend;
}

float3 SampleSkyColor(float3 rayDirection)
{
    const float upward = saturate(rayDirection.y * 0.5 + 0.5);
    float3 skyColor = lerp(float3(0.22, 0.26, 0.32), float3(0.54, 0.68, 0.91), pow(upward, 0.58));
    skyColor = lerp(skyColor, float3(0.10, 0.12, 0.15), saturate(-rayDirection.y * 3.0));

    const float sunAlignment = saturate(dot(rayDirection, gSunDirectionAndLightFactor.xyz));
    const float sunGlow = pow(sunAlignment, 1800.0);
    const float sunHalo = pow(sunAlignment, 24.0);
    skyColor += gColors0.rgb * sunGlow * 18.0;
    skyColor += gColors0.rgb * sunHalo * 0.8;
    return skyColor;
}

float4 RayMarchClouds(Ray ray, float segmentStart, float segmentEnd, float jitter)
{
    const uint maximumStepCount = max((uint)gCloudLayer.w, 1u);
    const uint reducedStepCount = max((uint)(gCloudLayer.w * 0.55), 1u);
    const uint actualStepCount = (uint)round(lerp((float)maximumStepCount, (float)reducedStepCount, saturate(ray.direction.y)));
    const float stepCount = max((float)actualStepCount, 1.0);
    const float stepSize = (segmentEnd - segmentStart) / stepCount;

    float3 position = ray.origin + ray.direction * (segmentStart + stepSize * jitter);
    float accumulatedTransmittance = 1.0;
    float3 accumulatedScattering = 0.0;

    [loop]
    for (uint stepIndex = 0u; stepIndex < actualStepCount; ++stepIndex)
    {
        const float heightFraction = HeightFractionForPoint(position);
        const float density = SampleCloudDensity(position, heightFraction, true);
        if (density > 0.001)
        {
            const float opticalDepth = density * stepSize;
            const float stepTransmittance = BeerLambert(opticalDepth);
            const float coneDensity = SampleCloudDensityAlongCone(position, gSunDirectionAndLightFactor.xyz);
            const float lightEnergy = CalculateLightEnergy(coneDensity * gLighting0.x, dot(ray.direction, gSunDirectionAndLightFactor.xyz), opticalDepth);

            const float3 ambientLight = lerp(gColors1.rgb, gColors2.rgb, heightFraction) * gLighting0.w;
            const float3 sunLight = lightEnergy * gColors0.rgb * gSunDirectionAndLightFactor.w;
            accumulatedScattering += (ambientLight + sunLight) * accumulatedTransmittance * opticalDepth;
            accumulatedTransmittance *= stepTransmittance;

            if (accumulatedTransmittance <= 0.01)
            {
                break;
            }
        }

        position += ray.direction * stepSize;
    }

    return float4(accumulatedScattering, 1.0 - accumulatedTransmittance);
}

float4 main(PSInput input) : SV_Target0
{
    const Ray ray = GenerateViewRay(input.uv);
    const float3 skyColor = SampleSkyColor(ray.direction);
    const float horizonBlend = smoothstep(-0.04, 0.05, ray.direction.y);

    float segmentStart = 0.0;
    float segmentEnd = 0.0;
    if (!BuildCloudSegment(ray, segmentStart, segmentEnd))
    {
        const float3 tonemappedSky = 1.0 - exp(-skyColor * gColors0.w);
        return float4(pow(saturate(tonemappedSky), 1.0 / 2.2), horizonBlend);
    }

    const float jitter = InterleavedGradientNoise(input.position.xy, gTextureIndices.z);
    const float4 clouds = RayMarchClouds(ray, segmentStart, segmentEnd, jitter);
    const float3 color = clouds.rgb + (1.0 - clouds.a) * skyColor;
    const float3 tonemapped = 1.0 - exp(-color * gColors0.w);
    return float4(pow(saturate(tonemapped), 1.0 / 2.2), horizonBlend);
}
