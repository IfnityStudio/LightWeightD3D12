cbuffer PushConstants : register(b0)
{
	row_major float4x4 gWorldViewProjection;
	row_major float4x4 gWorld;
	uint gAlbedoTextureIndex;
	uint gLightmapTextureIndex;
	uint gShowLightmapOnly;
	uint gShowUvDebug;
	uint gUseBakedLight;
	uint gSplitCompare;
	float gSplitPosition;
	float gScreenWidth;
	float3 gKeyLightDirection;
	float gKeyLightIntensity;
	float3 gKeyLightColor;
	float gAmbientIntensity;
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
	float2 uv : TEXCOORD0;
	float2 lightUv : TEXCOORD1;
};

VSOutput main(VSInput input)
{
	VSOutput output;
	output.position = mul(float4(input.position, 1.0f), gWorldViewProjection);
	output.normal = normalize(mul(float4(input.normal, 0.0f), gWorld).xyz);
	output.uv = input.uv;
	output.lightUv = input.lightUv;
	return output;
}
