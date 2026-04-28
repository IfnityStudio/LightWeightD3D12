cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProj;
    row_major float4x4 gWorld;
    float4 gGroundTint;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    const float4 worldPosition = mul(float4(input.position, 1.0), gWorld);
    output.position = mul(float4(input.position, 1.0), gWorldViewProj);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(mul(float4(input.normal, 0.0), gWorld).xyz);
    output.texCoord = input.texCoord;
    return output;
}
