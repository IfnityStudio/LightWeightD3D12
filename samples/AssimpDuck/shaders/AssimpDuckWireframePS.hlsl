cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProj;
    row_major float4x4 gWorld;
    float4 gBaseColor;
};

float4 main() : SV_Target0
{
    return float4(gBaseColor.rgb, 1.0);
}
