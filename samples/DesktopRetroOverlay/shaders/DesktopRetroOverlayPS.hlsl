// CRTV treatment adapted from the effect vocabulary of Simple CRT Shader
// by yunoda-3DCG (MIT, Copyright 2022 yunoda). See THIRD_PARTY_NOTICES.md.

cbuffer PushConstants : register(b0)
{
    uint gSourceTextureIndex;
    uint gMode;
    float gIntensity;
    float gCurvature;
    float gScanlineStrength;
    float gChromaticAberrationPixels;
    float gNoiseStrength;
    float gTime;
    float gInverseWidth;
    float gInverseHeight;
    uint gBlurTextureIndex;
    uint gReserved;
};

SamplerState gSampler : register(s0);

float Hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float3 ApplyPalette(float3 color, uint mode)
{
    const float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    if (mode == 1)
    {
        return luminance * float3(1.00, 0.56, 0.16);
    }
    if (mode == 2)
    {
        return luminance * float3(0.20, 1.00, 0.40);
    }

    // CRT keeps most of the source colour while adding a little saturation.
    return lerp(luminance.xxx, color, 1.18);
}

float3 SampleClamped(Texture2D<float4> sourceTexture, float2 uv)
{
    // The application's default bindless sampler may wrap. Clamping every
    // displaced RGB sample prevents the opposite edge of the desktop leaking
    // in as a coloured ghost near the CRT border.
    const float2 halfTexel = float2(gInverseWidth, gInverseHeight) * 0.5;
    return sourceTexture.SampleLevel(gSampler, clamp(uv, halfTexel, 1.0 - halfTexel), 0).rgb;
}

float3 BuildRgbPhosphorMask(float pixelX, float strength)
{
    // Three narrow phosphor stripes per RGB triad. Keeping the dark part of
    // each stripe above black preserves desktop readability while making the
    // individual red, green and blue subpixels visible at native scale.
    const float triad = frac(pixelX / 3.0);
    const float3 centers = float3(1.0 / 6.0, 0.5, 5.0 / 6.0);
    const float3 distanceToStripe = abs(frac(triad.xxx - centers + 0.5) - 0.5);
    const float3 stripe = 1.0 - smoothstep(0.075, 0.265, distanceToStripe);
    return lerp(1.0.xxx, 0.66.xxx + 0.34 * stripe, strength);
}

float3 FilmToneMap(float3 color)
{
    const float3 x = max(0.0.xxx, color - 0.004.xxx);
    return (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
}

float3 SamplePs2CleanSignal(Texture2D<float4> sourceTexture, float2 uv, float chromaPixels)
{
    // The original PS2 Clean preset: a stable, low-bandwidth 480-line image
    // with modest component colour spread and no temporal persistence.
    // A slightly coarser raster than the nominal 640×480 gives modern games
    // the larger, softer PS2-era pixels expected on a consumer CRT.
    const float2 signalSize = float2(512.0, 384.0);
    const float2 signalTexel = 1.0 / signalSize;
    const float2 signalUv = (floor(uv * signalSize) + 0.5) * signalTexel;
    const float3 center = SampleClamped(sourceTexture, signalUv);
    const float3 left = SampleClamped(sourceTexture, signalUv - float2(signalTexel.x, 0.0));
    const float3 right = SampleClamped(sourceTexture, signalUv + float2(signalTexel.x, 0.0));
    float3 signal = center * 0.56 + (left + right) * 0.22;
    const float chromaOffset = chromaPixels * signalTexel.x;
    signal.r = lerp(signal.r, SampleClamped(sourceTexture, signalUv + float2(chromaOffset, 0.0)).r, 0.58);
    signal.b = lerp(signal.b, SampleClamped(sourceTexture, signalUv - float2(chromaOffset, 0.0)).b, 0.58);
    return signal;
}

float3 ApplyNewPixieCrt(
    Texture2D<float4> accumulatedTexture,
    Texture2D<float4> blurredTexture,
    float2 uv,
    float strength)
{
    // Separate converging RGB beams give the image the low-bandwidth colour
    // character that makes PS2-era games look at home on a consumer CRT.
    float3 color;
    color.r = SampleClamped(accumulatedTexture, uv + float2(0.00095, 0.00060)).r;
    color.g = SampleClamped(accumulatedTexture, uv + float2(0.00000, -0.00075)).g;
    color.b = SampleClamped(accumulatedTexture, uv + float2(-0.00135, 0.00000)).b;

    const float luma = dot(color, float3(0.299, 0.587, 0.114));
    const float3 glow = SampleClamped(blurredTexture, uv + float2(-0.012, -0.006));
    const float ghostWeight = 0.06 * strength;
    color += glow * float3(0.11, 0.055, 0.08) * ghostWeight * (0.25 + 0.75 * luma);

    // The non-linear beam response gives bright objects a gentle bloom before
    // the final filmic roll-off, without needing a costly full-resolution HDR pass.
    color *= float3(0.95, 1.05, 0.95);
    color = color * 1.10 + 0.20 * color * color + 0.20 * color * color * color * color * color;
    return FilmToneMap(color);
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[gSourceTextureIndex];

    const float effectStrength = saturate(gIntensity);
    const bool ps2CleanMode = gMode == 3;
    const bool newPixieMode = gMode == 4;
    const bool consoleCrtMode = ps2CleanMode || newPixieMode;
    // Never offset the sampled desktop per frame: a tracking disturbance can
    // look authentic on a video, but makes live text appear to move.
    const float2 crtUv = uv;

    const float2 centeredUv = crtUv * 2.0 - 1.0;
    const float radiusSquared = dot(centeredUv, centeredUv);
    // The PS2 preset bends already through the middle of the face, giving it
    // the convex silhouette of a consumer CRT instead of only pulling corners.
    const float curveProfile = consoleCrtMode
        ? lerp(0.35, 1.0, saturate(radiusSquared * 0.5))
        : radiusSquared;
    const float2 curvedUv = (centeredUv * (1.0 + gCurvature * effectStrength * curveProfile)) * 0.5 + 0.5;
    if (any(curvedUv < 0.0) || any(curvedUv > 1.0))
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float3 processed;
    if (newPixieMode)
    {
        Texture2D<float4> blurTexture = ResourceDescriptorHeap[gBlurTextureIndex];
        processed = ApplyNewPixieCrt(sourceTexture, blurTexture, curvedUv, effectStrength);
    }
    else if (ps2CleanMode)
    {
        processed = SamplePs2CleanSignal(sourceTexture, curvedUv, gChromaticAberrationPixels * effectStrength);
    }
    else
    {
        // Keep the RGB convergence deliberately subtle. The previous 0.8px
        // shift was visible as a second, coloured image around text edges.
        const float2 chromaOffset = float2(gChromaticAberrationPixels * effectStrength * gInverseWidth, 0.0);
        processed.r = SampleClamped(sourceTexture, curvedUv + chromaOffset).r;
        processed.g = SampleClamped(sourceTexture, curvedUv).g;
        processed.b = SampleClamped(sourceTexture, curvedUv - chromaOffset).b;
    }

    processed = ApplyPalette(processed, gMode);
    float scanline;
    if (newPixieMode)
    {
        // The beam roll moves the light pattern, not the sampled image, so it
        // has the NewPixie character without moving desktop windows sideways.
        const float beamPhase = curvedUv.y / gInverseHeight * 1.5 - gTime * 7.0;
        const float beam = saturate(0.50 + 0.24 * sin(beamPhase));
        scanline = lerp(1.0, beam, gScanlineStrength * effectStrength);
    }
    else if (ps2CleanMode)
    {
        const float beam = 0.42 + 0.58 * pow(abs(sin(frac(curvedUv.y * 480.0) * 3.14159265)), 0.72);
        scanline = lerp(1.0, beam, gScanlineStrength * effectStrength);
    }
    else
    {
        const float scanlineTime = gMode == 0 ? gTime * 0.18 : 0.0;
        scanline = 1.0 - gScanlineStrength * effectStrength *
            (0.5 + 0.5 * sin(curvedUv.y / gInverseHeight * 3.14159265 + scanlineTime));
    }
    float3 phosphorMask = 1.0.xxx;
    if (gMode == 0 || consoleCrtMode)
    {
        phosphorMask = BuildRgbPhosphorMask(curvedUv.x / gInverseWidth, effectStrength);
    }
    const float vignette = 1.0 - (consoleCrtMode ? 0.30 : 0.24) * effectStrength * saturate(radiusSquared * radiusSquared);
    const float grain = (Hash21(position.xy + gTime * 61.0) - 0.5) * gNoiseStrength * effectStrength;
    // Keep the overall luminance stable. A whole-screen CRT flicker reads as
    // an unwanted brightness pulse on a live desktop rather than as texture.
    processed = saturate(processed * scanline * phosphorMask * vignette + grain);

    // Do not blend a geometrically transformed image over an untransformed
    // desktop image: that is temporal-looking double edging, not CRT glow.
    return float4(processed, 1.0);
}
