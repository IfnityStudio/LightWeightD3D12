cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProj;
    row_major float4x4 gWorld;
    float4 gMapParams;
    float4 gLighting;
    uint gTextureIndex;
    uint gUseTexture;
    uint2 gPadding;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 latLonDegrees : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    float4 color : COLOR0;
    float flatMode : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

float WrapDeltaDegrees(float delta)
{
    return delta - round(delta / 360.0) * 360.0;
}

VSOutput main(VSInput input)
{
    VSOutput output;
    const bool flatMode = gMapParams.x > 0.5;
    output.color = input.color;
    output.flatMode = gMapParams.x;
    output.uv = float2((input.latLonDegrees.y + 180.0) / 360.0, (90.0 - input.latLonDegrees.x) / 180.0);

    if (flatMode)
    {
        const float centerLongitude = gMapParams.y;
        const float centerLatitude = gMapParams.z;
        const float mapScale = gMapParams.w;
        const float longitudeDelta = WrapDeltaDegrees(input.latLonDegrees.y - centerLongitude);
        const float latitudeDelta = input.latLonDegrees.x - centerLatitude;
        const float2 ndc = float2(longitudeDelta / 180.0, latitudeDelta / 90.0) * mapScale;

        output.position = float4(ndc, 0.45, 1.0);
        output.normal = float3(0.0, 0.0, -1.0);
    }
    else
    {
        output.position = mul(float4(input.position, 1.0), gWorldViewProj);
        output.normal = normalize(mul(float4(input.normal, 0.0), gWorld).xyz);
    }

    return output;
}
