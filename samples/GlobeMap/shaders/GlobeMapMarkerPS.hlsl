cbuffer PushConstants : register(b0)
{
    float4 gRect;
    float4 gFillColor;
    float4 gBorderColor;
    float4 gStyle;
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    const float2 edge = min(uv, 1.0 - uv);
    const float border = min(edge.x, edge.y) < gStyle.x ? 1.0 : 0.0;
    return lerp(gFillColor, gBorderColor, border);
}
