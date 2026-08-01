#include "LightD3D12_Defines.hlsli"

#define SAMPLE_MATRIX_SRV_SLOT LIGHTD3D12_SRV_SLOT_FREESRV0
#define SAMPLE_COLOR_CBV_SLOT LIGHTD3D12_CBV_SLOT_FREECB0

cbuffer PushConstants : register(b0)
{
    uint gCubeCount;
    float gAspectRatio;
    float gViewDistance;
};

static const uint kMaxCubeColors = 32;

struct MatrixRows
{
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
};

struct CubeColorConstants
{
    float4 colors[kMaxCubeColors];
    uint colorCount;
    uint3 padding;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    nointerpolation uint colorIndex : COLOR_INDEX;
};

float4 TransformPoint(MatrixRows matrix, float3 localPosition)
{
    const float4 value = float4(localPosition, 1.0);
    return float4(
        dot(matrix.row0, value),
        dot(matrix.row1, value),
        dot(matrix.row2, value),
        dot(matrix.row3, value));
}
float3 TransformNormal(MatrixRows matrix, float3 localNormal)
{
    return normalize(float3(
        dot(matrix.row0.xyz, localNormal),
        dot(matrix.row1.xyz, localNormal),
        dot(matrix.row2.xyz, localNormal)));
}

VSOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    static const float3 positions[36] =
    {
        float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0),
        float3(-1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0),

        float3(-1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0), float3( 1.0, -1.0,  1.0),
        float3(-1.0, -1.0,  1.0), float3(-1.0,  1.0,  1.0), float3( 1.0,  1.0,  1.0),

        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0, -1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0, -1.0), float3(-1.0, -1.0, -1.0),

        float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0),
        float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),

        float3( 1.0, -1.0, -1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0),
        float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0,  1.0), float3( 1.0,  1.0, -1.0),

        float3(-1.0, -1.0,  1.0), float3(-1.0, -1.0, -1.0), float3(-1.0,  1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3(-1.0,  1.0, -1.0), float3(-1.0,  1.0,  1.0)
    };

    static const float3 normals[36] =
    {
        float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0),
        float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0),

        float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0),
        float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0),

        float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0),
        float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0),

        float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0),
        float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0),

        float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0),
        float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0),

        float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0),
        float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0)
    };

    StructuredBuffer<MatrixRows> matrices = ResourceDescriptorHeap[SAMPLE_MATRIX_SRV_SLOT];
    const uint safeInstanceID = min(instanceID, max(gCubeCount, 1u) - 1u);
    const MatrixRows matrix = matrices[safeInstanceID];

    float3 worldPosition = TransformPoint(matrix, positions[vertexID]).xyz;
    worldPosition.z += gViewDistance;

    const float perspectiveScale = 2.25 / max(worldPosition.z, 0.001);
    const float2 clipXY = float2(worldPosition.x * perspectiveScale / gAspectRatio, worldPosition.y * perspectiveScale);
    const float clipZ = saturate((worldPosition.z - 1.0) / (gViewDistance + 18.0));

    VSOutput output;
    output.position = float4(clipXY, clipZ, 1.0);
    output.normal = TransformNormal(matrix, normals[vertexID]);
    output.colorIndex = safeInstanceID;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    ConstantBuffer<CubeColorConstants> colorConstants = ResourceDescriptorHeap[SAMPLE_COLOR_CBV_SLOT];
    const uint colorCount = max(colorConstants.colorCount, 1u);
    const float3 baseColor = colorConstants.colors[input.colorIndex % colorCount].rgb;

    const float3 lightDirection = normalize(float3(-0.35, 0.8, -0.45));
    const float lambert = saturate(dot(normalize(input.normal), lightDirection)) * 0.72 + 0.28;
    return float4(baseColor * lambert, 1.0);
}
