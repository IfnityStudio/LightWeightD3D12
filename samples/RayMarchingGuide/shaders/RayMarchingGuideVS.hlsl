struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    VSOutput output;
    output.position = float4(positions[vertexID], 0.0, 1.0);
    output.uv = float2(positions[vertexID].x * 0.5 + 0.5, 0.5 - positions[vertexID].y * 0.5);
    return output;
}
