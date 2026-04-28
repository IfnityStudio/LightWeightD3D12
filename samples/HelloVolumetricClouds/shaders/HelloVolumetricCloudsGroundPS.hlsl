cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProj;
    row_major float4x4 gWorld;
    float4 gGroundTint;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
};

float4 main(PSInput input) : SV_Target0
{
    const float2 tiledUv = input.texCoord * 0.08;
    const float checker = frac(floor(tiledUv.x) + floor(tiledUv.y)) < 0.5 ? 0.0 : 1.0;
    const float3 baseColor = lerp(gGroundTint.rgb * 0.82, gGroundTint.rgb * 1.12, checker);

    const float3 lightDirection = normalize(float3(-0.35, 0.78, 0.42));
    const float lambert = saturate(dot(normalize(input.worldNormal), lightDirection)) * 0.65 + 0.35;

    // Fade the ground toward the horizon so the cloud layer has a gentler transition.
    const float distanceFade = saturate(1.0 - input.position.z * 0.55);
    const float3 color = lerp(baseColor * 0.58, baseColor, distanceFade) * lambert;
    return float4(color, 1.0);
}
