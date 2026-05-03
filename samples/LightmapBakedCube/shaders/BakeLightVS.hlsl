cbuffer BakeLightConstants : register(b0)
{
	row_major float4x4 gWorld;
	float3 gKeyLightDirection;
	float gKeyLightIntensity;
	float3 gKeyLightColor;
	float gAmbientIntensity;
	uint gLightType;
	float gLightRange;
	float gSpotInnerCos;
	float gSpotOuterCos;
	float3 gLightPosition;
	float gLightPadding;
};

struct VSInput
{
	float3 position : POSITION0;
	float3 normal : NORMAL0;
	float2 uv : TEXCOORD0;
	float2 lightUv : TEXCOORD1;
};

struct VSOutput
{
	float4 position : SV_Position;
	float3 normal : NORMAL0;
	float2 lightUv : TEXCOORD0;
	float3 worldPosition : TEXCOORD1;
};

VSOutput main(VSInput input)
{
	VSOutput output;

	// This is the "bake projection":
	// we are not using a camera. We put every vertex directly where it belongs in the lightmap.
	output.position = float4(input.lightUv.x * 2.0f - 1.0f, 1.0f - input.lightUv.y * 2.0f, 0.0f, 1.0f);
	output.normal = normalize(mul(float4(input.normal, 0.0f), gWorld).xyz);
	output.lightUv = input.lightUv;
	output.worldPosition = mul(float4(input.position, 1.0f), gWorld).xyz;
	return output;
}
