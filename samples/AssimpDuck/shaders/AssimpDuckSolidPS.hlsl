float4 main(float4 position : SV_Position, float3 normal : NORMAL0, float3 baseColor : COLOR0) : SV_Target0
{
    const float3 lightDirection = normalize(float3(-0.45, 0.85, -0.35));
    const float3 viewDirection = normalize(float3(0.15, 0.25, -1.0));
    const float diffuse = saturate(dot(normalize(normal), lightDirection));
    const float rim = pow(1.0 - saturate(dot(normalize(normal), -viewDirection)), 2.5);
    const float3 litColor = baseColor * (0.22 + diffuse * 0.78) + rim * float3(0.08, 0.09, 0.10);
    return float4(saturate(litColor), 1.0);
}
