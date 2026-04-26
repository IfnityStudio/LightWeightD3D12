cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProj;
    row_major float4x4 gWorld;
    float4 gBaseColor;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    float3 baseColor : COLOR0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), gWorldViewProj);
    output.normal = normalize(mul(float4(input.normal, 0.0), gWorld).xyz);
	output.baseColor = input.position.rgb;
    return output;
}
