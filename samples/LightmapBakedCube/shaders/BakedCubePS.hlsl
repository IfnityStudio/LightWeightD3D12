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

SamplerState gTextureSampler : register(s0);

float3 ComputeRuntimeLight(float3 normal)
{
	float3 n = normalize(normal);
	float3 lightDir = normalize(gKeyLightDirection);
	float ndotl = max(dot(n, lightDir), 0.0f);
	return float3(gAmbientIntensity, gAmbientIntensity, gAmbientIntensity + 0.01f) + gKeyLightColor * gKeyLightIntensity * ndotl;
}

float4 main(float4 position : SV_Position, float3 normal : NORMAL0, float2 uv : TEXCOORD0, float2 lightUv : TEXCOORD1) : SV_Target0
{
	if (gShowUvDebug != 0)
	{
		return float4(lightUv, 0.0f, 1.0f);
	}

	Texture2D<float4> albedoTexture = ResourceDescriptorHeap[gAlbedoTextureIndex];
	Texture2D<float4> lightmapTexture = ResourceDescriptorHeap[gLightmapTextureIndex];

	const float3 albedo = albedoTexture.Sample(gTextureSampler, uv).rgb;
	const float3 bakedLight = lightmapTexture.Sample(gTextureSampler, lightUv).rgb;
	const float3 runtimeLight = ComputeRuntimeLight(normal);
	const float3 rawColor = albedo * runtimeLight;
	const float3 bakedColor = albedo * bakedLight;

	if (gShowLightmapOnly != 0)
	{
		return float4(bakedLight, 1.0f);
	}

	if (gSplitCompare != 0)
	{
		const float normalizedX = position.x / max(gScreenWidth, 1.0f);
		if (abs(normalizedX - gSplitPosition) < 0.003f)
		{
			return float4(1.0f, 0.95f, 0.15f, 1.0f);
		}
		return float4(normalizedX < gSplitPosition ? rawColor : bakedColor, 1.0f);
	}

	if (gUseBakedLight == 0)
	{
		return float4(rawColor, 1.0f);
	}

	// This is the whole runtime side of lightmapping:
	// the expensive lighting is stored in light.png, so the shader only multiplies material color by baked light.
	return float4(bakedColor, 1.0f);
}
