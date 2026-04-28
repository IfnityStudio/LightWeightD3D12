cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProj;
    row_major float4x4 gWorld;
    float4 gBaseColor;
    float4 gDeformation;
    float4 gTaper;
    float4 gTwist;
    float4 gSphere;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    float3 baseColor : COLOR0;
};

static const float kDeformationNone = 0.0;
static const float kDeformationTwist = 1.0;
static const float kDeformationTape = 2.0;
static const float kDeformationTapeTwist = 3.0;
static const float kDeformationSpherifyTodo = 4.0;
static const float kPi = 3.141592654;

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    if (lengthSquared <= 1.0e-8)
    {
        return fallback;
    }

    return value * rsqrt(lengthSquared);
}

float HermiteInterpolate(float y1, float y2, float k1, float k2, float x)
{
    const float a = 2.0 * y1 - 2.0 * y2 + k1 + k2;
    const float b = -3.0 * y1 + 3.0 * y2 - 2.0 * k1 - k2;
    const float c = k1;
    const float d = y1;
    return ((a * x + b) * x + c) * x + d;
}

float TapeFactorZ(float zAxisPosition)
{
    const float taperRange = max(gTaper.y, 1.0e-4);
    const float t = abs((gTaper.z - zAxisPosition) / taperRange);
    if (t >= 1.0)
    {
        return 1.0;
    }

    if (gDeformation.z > 0.5)
    {
        return HermiteInterpolate(gTaper.x, 1.0, 0.0, 0.0, t);
    }

    return lerp(gTaper.x, 1.0, t);
}

float2 Rotate2D(float2 value, float angle)
{
    const float sineTerm = sin(angle);
    const float cosineTerm = cos(angle);
    return float2(
        value.x * cosineTerm - value.y * sineTerm,
        value.x * sineTerm + value.y * cosineTerm);
}

float3 RotateAroundAxis(float3 value, float3 axis, float angle)
{
    const float sineTerm = sin(angle);
    const float cosineTerm = cos(angle);
    return value * cosineTerm + cross(axis, value) * sineTerm + axis * dot(axis, value) * (1.0 - cosineTerm);
}

float3 ChoosePerpendicularAxis(float3 normal)
{
    const float3 candidate = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    return SafeNormalize(cross(normal, candidate), float3(1.0, 0.0, 0.0));
}

float TwistAngleZ(float zAxisPosition)
{
    return (zAxisPosition - gTwist.x) * gTwist.y;
}

float3 GetSphereCenter()
{
    // The mesh is recentered around the origin on the CPU before upload,
    // so the origin is the cleanest starting sphere center in normalized space.
    return float3(0.0, 0.0, 0.0);
}

void ApplyTapeZ(inout float3 position, inout float3 normal)
{
    const float factor = TapeFactorZ(position.z);
    position.xz *= factor;

    // TODO: If you want exact taper lighting later, derive the full Jacobian here.
    normal = normalize(float3(normal.x / max(factor, 1.0e-4), normal.y / max(factor, 1.0e-4), normal.z));
}

void ApplyTwistZ(inout float3 position, inout float3 normal)
{
    const float angle = TwistAngleZ(position.z);
    position.xy = Rotate2D(position.xy, angle);
    normal.xy = Rotate2D(normal.xy, angle);
    normal = normalize(normal);
}

void ApplySpherifyTodo(inout float3 position, inout float3 normal)
{
    const float3 sphereCenter = GetSphereCenter();
    const float spherifyFactor = saturate(gSphere.x);
    const float sphereRadius = max(gSphere.y, 1.0e-4);

    const float3 localPosition = position - sphereCenter;
    const float3 sphereDirection = SafeNormalize(localPosition, SafeNormalize(normal, float3(0.0, 0.0, 1.0)));
    const float3 spherePoint = sphereCenter + sphereDirection * sphereRadius;
    position = lerp(position, spherePoint, spherifyFactor);

    const float3 startNormal = SafeNormalize(normal, sphereDirection);
    const float3 endNormal = sphereDirection;
    const float cosineTerm = clamp(dot(startNormal, endNormal), -1.0, 1.0);
    const float3 rawAxis = cross(startNormal, endNormal);
    const float axisLengthSquared = dot(rawAxis, rawAxis);

    if (axisLengthSquared <= 1.0e-8)
    {
        if (cosineTerm < 0.0)
        {
            const float3 fallbackAxis = ChoosePerpendicularAxis(startNormal);
            normal = RotateAroundAxis(startNormal, fallbackAxis, kPi * spherifyFactor);
        }
        else
        {
            normal = startNormal;
        }
    }
    else
    {
        const float3 rotationAxis = rawAxis * rsqrt(axisLengthSquared);
        const float rotationAngle = acos(cosineTerm) * spherifyFactor;
        normal = RotateAroundAxis(startNormal, rotationAxis, rotationAngle);
    }

    normal = SafeNormalize(normal, endNormal);
}

VSOutput main(VSInput input)
{
    VSOutput output;

    //LOCAL SPACE DEFORMATION
    const float3 originalPosition = input.position;
    const float3 originalNormal = normalize(input.normal);
    float3 deformedPosition = originalPosition;
    float3 deformedNormal = originalNormal;

    if (gDeformation.x == kDeformationTwist)
    {
        ApplyTwistZ(deformedPosition, deformedNormal);
    }
    else if (gDeformation.x == kDeformationTape)
    {
        ApplyTapeZ(deformedPosition, deformedNormal);
    }
    else if (gDeformation.x == kDeformationTapeTwist)
    {
        ApplyTapeZ(deformedPosition, deformedNormal);
        ApplyTwistZ(deformedPosition, deformedNormal);
    }
    else if (gDeformation.x == kDeformationSpherifyTodo)
    {
        ApplySpherifyTodo(deformedPosition, deformedNormal);
    }

    const float blendFactor = saturate(gDeformation.y);
    const float3 finalPosition = lerp(originalPosition, deformedPosition, blendFactor);
    const float3 finalNormal = normalize(lerp(originalNormal, deformedNormal, blendFactor));
    
    

    output.position = mul(float4(finalPosition, 1.0), gWorldViewProj);
    output.normal = normalize(mul(float4(finalNormal, 0.0), gWorld).xyz);
    output.baseColor = gBaseColor.rgb;
    return output;
}
