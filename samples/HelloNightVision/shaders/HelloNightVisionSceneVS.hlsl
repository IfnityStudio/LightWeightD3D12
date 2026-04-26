cbuffer PushConstants : register(b0)
{
    float gAspectRatio;
    float gTime;
    float gCubeScale;
    float gPlaneScale;
    uint gDrawMode;
    float3 gPadding;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 albedo : TEXCOORD2;
    float2 materialParams : TEXCOORD3;
};

float3 HueToRgb(float hue)
{
    const float3 k = float3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    const float3 p = abs(frac(hue + k) * 6.0 - 3.0);
    return saturate(p - 1.0);
}

float3 RotateXYZ(float3 p, float3 angles)
{
    const float cosX = cos(angles.x);
    const float sinX = sin(angles.x);
    const float cosY = cos(angles.y);
    const float sinY = sin(angles.y);
    const float cosZ = cos(angles.z);
    const float sinZ = sin(angles.z);

    p = float3(p.x, p.y * cosX - p.z * sinX, p.y * sinX + p.z * cosX);
    p = float3(p.x * cosY + p.z * sinY, p.y, -p.x * sinY + p.z * cosY);
    p = float3(p.x * cosZ - p.y * sinZ, p.x * sinZ + p.y * cosZ, p.z);
    return p;
}

void GetCubeVertex(uint vertexID, out float3 position, out float3 normal)
{
    static const float3 positions[36] =
    {
        float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0),
        float3(-1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),
        float3(-1.0, -1.0,  1.0), float3(-1.0, -1.0, -1.0), float3(-1.0,  1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3(-1.0,  1.0, -1.0), float3(-1.0,  1.0,  1.0),
        float3( 1.0, -1.0, -1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0),
        float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0,  1.0), float3( 1.0,  1.0, -1.0),
        float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0),
        float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0, -1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0, -1.0), float3(-1.0, -1.0, -1.0)
    };

    static const float3 normals[36] =
    {
        float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0),
        float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0),
        float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0),
        float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0),
        float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0),
        float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0),
        float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0),
        float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0),
        float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0),
        float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0),
        float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0),
        float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0)
    };

    position = positions[vertexID];
    normal = normals[vertexID];
}

void GetPlaneVertex(uint vertexID, out float2 position, out float2 uv)
{
    static const float2 positions[6] =
    {
        float2(-1.0, 0.0), float2( 1.0, 0.0), float2( 1.0, 1.0),
        float2(-1.0, 0.0), float2( 1.0, 1.0), float2(-1.0, 1.0)
    };

    position = positions[vertexID];
    uv = float2(position.x * 0.5 + 0.5, position.y);
}

float4 ProjectToClip(float3 worldPosition)
{
    const float safeZ = max(worldPosition.z, 0.35);
    const float perspectiveScale = 1.55 / safeZ;
    const float2 clipXY = float2(worldPosition.x * perspectiveScale / gAspectRatio, worldPosition.y * perspectiveScale);
    const float clipZ = saturate((safeZ - 1.0) / 18.0);
    return float4(clipXY, clipZ, 1.0);
}

VSOutput main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VSOutput output;

    if (gDrawMode == 1u)
    {
        float2 planePos;
        float2 planeUv;
        GetPlaneVertex(vertexID, planePos, planeUv);

        const float worldX = (planeUv.x * 2.0 - 1.0) * gPlaneScale;
        const float worldZ = lerp(3.0, 16.0, planeUv.y);
        const float worldY = -1.65 + sin(worldX * 0.18 + worldZ * 0.11) * 0.08;
        const float3 worldPosition = float3(worldX, worldY, worldZ);

        const float grassPattern = 0.5 + 0.5 * sin(worldX * 0.9) * cos(worldZ * 0.55);
        const float3 grassColor = lerp(float3(0.05, 0.10, 0.04), float3(0.12, 0.26, 0.08), grassPattern);

        output.position = ProjectToClip(worldPosition);
        output.worldPosition = worldPosition;
        output.normal = normalize(float3(-0.03 * cos(worldX * 0.18), 1.0, -0.02 * sin(worldZ * 0.11)));
        output.albedo = grassColor;
        output.materialParams = float2(1.0, 0.0);
        return output;
    }

    float3 localPosition;
    float3 localNormal;
    GetCubeVertex(vertexID, localPosition, localNormal);

    const uint row = instanceID / 3u;
    const uint column = instanceID % 3u;
    const float2 grid = float2((float)column - 1.0, (float)row - 1.0);
    const float3 center = float3(
        grid.x * 2.7,
        -0.10 + sin(gTime * 0.95 + (float)instanceID * 0.65) * 0.28,
        5.8 + grid.y * 2.15 + cos(gTime * 0.45 + (float)instanceID * 0.37) * 0.25);

    const float3 angles = float3(
        gTime * (0.52 + (float)instanceID * 0.07),
        gTime * (0.81 + (float)instanceID * 0.11),
        gTime * (0.33 + (float)instanceID * 0.05));

    const float3 rotatedPosition = RotateXYZ(localPosition * gCubeScale, angles);
    const float3 worldPosition = rotatedPosition + center;
    const float3 worldNormal = normalize(RotateXYZ(localNormal, angles));

    const float hue = frac((float)instanceID * 0.17 + 0.08);
    float3 baseColor = lerp(float3(0.20, 0.22, 0.27), HueToRgb(hue), 0.85);

    const float chlorophyllMask = (instanceID % 4u == 0u) ? 1.0 : 0.0;
    baseColor = lerp(baseColor, float3(0.14, 0.40, 0.12), chlorophyllMask * 0.85);

    const float emissiveMask = (instanceID % 3u == 0u) ? 1.0 : 0.45;

    output.position = ProjectToClip(worldPosition);
    output.worldPosition = worldPosition;
    output.normal = worldNormal;
    output.albedo = baseColor;
    output.materialParams = float2(chlorophyllMask, emissiveMask);
    return output;
}
