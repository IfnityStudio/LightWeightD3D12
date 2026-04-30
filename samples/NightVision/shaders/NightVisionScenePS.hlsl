cbuffer PushConstants : register(b0)
{
    float gAspectRatio;
    float gTime;
    float gCubeScale;
    float gPlaneScale;
    uint gDrawMode;
    float3 gPadding;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 albedo : TEXCOORD2;
    float2 materialParams : TEXCOORD3;
};

struct PSOutput
{
    float4 visibleColor : SV_Target0;
    float4 thermalData : SV_Target1;
};

PSOutput main(PSInput input)
{
    const float3 lightDirection = normalize(float3(-0.55, 0.82, -0.25));
    const float lambert = saturate(dot(normalize(input.normal), lightDirection)) * 0.72 + 0.28;
    float3 color = input.albedo * lambert;

    if (input.materialParams.x > 0.0)
    {
        const float foliageRipple = 0.5 + 0.5 * sin(input.worldPosition.x * 2.2 + input.worldPosition.z * 0.9 + gTime * 0.35);
        color *= lerp(0.90, 1.25, foliageRipple * input.materialParams.x);
    }

    float3 emissive = 0.0;
    if (input.materialParams.y > 0.0)
    {
        const float topGlow = smoothstep(0.55, 0.98, input.normal.y);
        const float beacon = smoothstep(0.55, 0.98, 0.5 + 0.5 * sin(input.worldPosition.x * 6.0 + gTime * 3.2));
        emissive += input.materialParams.y * topGlow * beacon * float3(3.2, 2.7, 1.8);

        const float windowBands = smoothstep(0.70, 0.96, abs(input.normal.z));
        const float windowRows = smoothstep(0.45, 0.92, 0.5 + 0.5 * sin(input.worldPosition.y * 8.0));
        const float windowColumns = smoothstep(0.30, 0.88, 0.5 + 0.5 * cos(input.worldPosition.x * 7.0 + input.worldPosition.z * 0.6));
        emissive += input.materialParams.y * windowBands * windowRows * windowColumns * float3(1.2, 1.0, 0.65);
    }

    const float3 litVisible = color + emissive;
    const float visibleLuminance = dot(input.albedo, float3(0.2126, 0.7152, 0.0722));
    const float materialEmissivity = saturate(lerp(0.90, 0.98, input.materialParams.x) - visibleLuminance * 0.08);
    const float materialTemperature = saturate(0.24 + input.materialParams.y * 0.55 + length(emissive) * 0.08 + input.materialParams.x * 0.12);

    PSOutput output;
    output.visibleColor = float4(litVisible, 1.0);
    output.thermalData = float4(materialEmissivity, materialTemperature, input.materialParams.x, input.materialParams.y);
    return output;
}
