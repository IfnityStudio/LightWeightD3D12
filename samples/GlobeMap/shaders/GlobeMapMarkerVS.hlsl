cbuffer PushConstants : register(b0)
{
    float4 gRect;
    float4 gFillColor;
    float4 gBorderColor;
    float4 gStyle;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    static const float2 corners[6] =
    {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
        float2(0.0, 1.0), float2(1.0, 0.0), float2(1.0, 1.0)
    };

    VSOutput output;
    const float2 uv = corners[vertexID];
    const float2 local = uv * 2.0 - 1.0;
    output.position = float4(gRect.xy + local * gRect.zw, 0.05, 1.0);
    output.uv = uv;
    return output;
}
