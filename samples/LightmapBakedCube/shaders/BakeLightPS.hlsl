struct PSInput
{
	float4 position : SV_Position;
	float3 normal : NORMAL0;
	float2 lightUv : TEXCOORD0;
	float3 worldPosition : TEXCOORD1;
};

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

float GetDistanceAttenuation(float distanceToLight)
{
	float range = max(gLightRange, 0.001f);
	float attenuation = saturate(1.0f - distanceToLight / range);
	return attenuation * attenuation;
}

float3 ComputeBakedLight(float3 normal, float3 worldPosition)
{
	float3 n = normalize(normal);

	float3 ambient = float3(gAmbientIntensity, gAmbientIntensity, gAmbientIntensity + 0.01f);
	float3 lightColor = gKeyLightColor * gKeyLightIntensity;

	if (gLightType == 1)
	{
		float3 toLight = gLightPosition - worldPosition;
		float distanceToLight = length(toLight);
		float3 lightDir = toLight / max(distanceToLight, 0.001f);
		return ambient + lightColor * max(dot(n, lightDir), 0.0f) * GetDistanceAttenuation(distanceToLight);
	}

	if (gLightType == 2)
	{
		float3 toLight = gLightPosition - worldPosition;
		float distanceToLight = length(toLight);
		float3 lightDir = toLight / max(distanceToLight, 0.001f);
		float3 fromLightToPixel = -lightDir;
		float3 coneDirection = normalize(gKeyLightDirection);
		float cone = dot(fromLightToPixel, coneDirection);
		float coneAttenuation = smoothstep(gSpotOuterCos, gSpotInnerCos, cone);
		return ambient + lightColor * max(dot(n, lightDir), 0.0f) * GetDistanceAttenuation(distanceToLight) * coneAttenuation;
	}

	float3 lightDir = normalize(gKeyLightDirection);
	return ambient + lightColor * max(dot(n, lightDir), 0.0f);
}

float4 main(PSInput input) : SV_Target0
{
	// This pixel shader writes light, not material color.
	// The output goes into the GPU lightmap render target.
	return float4(ComputeBakedLight(input.normal, input.worldPosition), 1.0f);
}
