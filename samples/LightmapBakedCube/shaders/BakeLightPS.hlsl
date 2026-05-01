struct PSInput
{
	float4 position : SV_Position;
	float3 normal : NORMAL0;
	float2 lightUv : TEXCOORD0;
};

cbuffer BakeLightConstants : register(b0)
{
	float3 gKeyLightDirection;
	float gKeyLightIntensity;
	float3 gKeyLightColor;
	float gAmbientIntensity;
};

float3 ComputeDirectionalBake(float3 normal)
{
	float3 n = normalize(normal);

	float3 ambient = float3(gAmbientIntensity, gAmbientIntensity, gAmbientIntensity + 0.01f);

	float3 lightDir0 = normalize(gKeyLightDirection);
	float3 lightDir1 = normalize(float3(+0.80f, +0.25f, +0.25f));
	float3 lightDir2 = normalize(float3(-0.25f, -0.35f, +0.90f));

	float3 lightColor0 = gKeyLightColor * gKeyLightIntensity;
	float3 lightColor1 = float3(0.25f, 0.42f, 1.00f) * 0.30f;
	float3 lightColor2 = float3(0.28f, 1.00f, 0.58f) * 0.20f;

	float3 baked = ambient;
	baked += lightColor0 * max(dot(n, lightDir0), 0.0f);
	baked += lightColor1 * max(dot(n, lightDir1), 0.0f);
	baked += lightColor2 * max(dot(n, lightDir2), 0.0f);
	return baked;
}

float4 main(PSInput input) : SV_Target0
{
	// This pixel shader writes light, not material color.
	// The output goes into the GPU lightmap render target.
	return float4(ComputeDirectionalBake(input.normal), 1.0f);
}
