cbuffer PushConstants : register(b0)
{
    float3 gCameraOrigin;
    float gTime;

    float3 gCameraForward;
    float gAspectRatio;

    float3 gCameraRight;
    float gTanHalfFov;

    float3 gCameraUp;
    float gMaxDistance;

    uint gLessonIndex;
    uint gActiveStep;
    uint gDebugView;
    uint gFlags;

    float3 gLightDirection;
    float gSmoothUnionK;

    float gRepeatSpacing;
    float gFogDensity;
    float gGroundY;
    float gInvViewportHeight;

    float gInspectUvX;
    float gInspectUvY;
    uint gInspectFlags;
    uint gInspectPadding;
};

static const uint VIEW_AUTO = 0u;
static const uint VIEW_UV = 1u;
static const uint VIEW_RAY_DIRECTION = 2u;
static const uint VIEW_SKY = 3u;
static const uint VIEW_HIT_MASK = 4u;
static const uint VIEW_MARCH_STEPS = 5u;
static const uint VIEW_DEPTH = 6u;
static const uint VIEW_NORMALS = 7u;
static const uint VIEW_DIFFUSE = 8u;
static const uint VIEW_SHADOW = 9u;
static const uint VIEW_AO = 10u;
static const uint VIEW_FINAL = 11u;

struct SurfaceInfo
{
    float distance;
    float3 albedo;
    float emissive;
    float material;
};

struct MarchResult
{
    uint hit;
    uint steps;
    float travel;
    float3 position;
    SurfaceInfo surface;
};

struct RayContext
{
    float2 ndc;
    float3 origin;
    float3 direction;
    float3 sky;
    uint viewMode;
};

float2 Rotate2D(float2 p, float angle)
{
    const float cosine = cos(angle);
    const float sine = sin(angle);
    return float2(
        p.x * cosine - p.y * sine,
        p.x * sine + p.y * cosine);
}

float3 TwistY(float3 p, float amount)
{
    p.xz = Rotate2D(p.xz, amount * p.y);
    return p;
}

float2 Repeat2D(float2 p, float spacing)
{
    return fmod(p + 0.5 * spacing, spacing) - 0.5 * spacing;
}

float3 HueToRgb(float hue)
{
    const float3 k = float3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    const float3 q = abs(frac(hue + k) * 6.0 - 3.0);
    return saturate(q - 1.0);
}

float3 Heatmap(float t)
{
    return HueToRgb(0.72 - 0.72 * saturate(t));
}

float3 SkyColor(float3 rd)
{
    const float skyAmount = saturate(0.5 + 0.5 * rd.y);
    const float3 horizon = float3(0.48, 0.57, 0.72);
    const float3 zenith = float3(0.08, 0.16, 0.30);
    const float3 ground = float3(0.09, 0.08, 0.07);
    return rd.y >= 0.0
        ? lerp(horizon, zenith, pow(skyAmount, 1.5))
        : lerp(ground, horizon * 0.65, saturate(rd.y + 1.0));
}

SurfaceInfo MakeSurface(float distance, float3 albedo, float emissive, float material)
{
    SurfaceInfo surface;
    surface.distance = distance;
    surface.albedo = albedo;
    surface.emissive = emissive;
    surface.material = material;
    return surface;
}

SurfaceInfo SurfaceUnion(SurfaceInfo a, SurfaceInfo b)
{
    if (a.distance < b.distance)
    {
        return a;
    }

    return b;
}

SurfaceInfo SurfaceSmoothUnion(SurfaceInfo a, SurfaceInfo b, float k)
{
    if (k <= 0.0001)
    {
        return SurfaceUnion(a, b);
    }

    const float h = saturate(0.5 + 0.5 * (b.distance - a.distance) / k);
    SurfaceInfo result;
    result.distance = lerp(b.distance, a.distance, h) - k * h * (1.0 - h);
    result.albedo = lerp(b.albedo, a.albedo, h);
    result.emissive = lerp(b.emissive, a.emissive, h);
    result.material = lerp(b.material, a.material, h);
    return result;
}

float SdSphere(float3 p, float radius)
{
    return length(p) - radius;
}

float SdBox(float3 p, float3 halfSize)
{
    const float3 q = abs(p) - halfSize;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

float SdPlane(float3 p, float3 normal, float height)
{
    return dot(p, normal) + height;
}

float SdTorus(float3 p, float2 radii)
{
    const float2 q = float2(length(p.xz) - radii.x, p.y);
    return length(q) - radii.y;
}

SurfaceInfo EvaluateLessonOneScene(float3 p)
{
    return MakeSurface(
        SdSphere(p - float3(0.0, 1.0, 0.0), 1.0),
        float3(0.75, 0.82, 0.95),
        0.0,
        1.0);
}

SurfaceInfo EvaluateLessonTwoScene(float3 p, float animation)
{
    const float3 sphereCenter = float3(0.0, 1.0 + 0.08 * sin(animation), 0.0);
    return MakeSurface(
        SdSphere(p - sphereCenter, 1.0),
        float3(0.92, 0.94, 0.98),
        0.0,
        1.0);
}

SurfaceInfo EvaluateLessonThreeScene(float3 p, float animation)
{
    const float groundPattern = 0.52 + 0.08 * sin(p.x * 2.5) * sin(p.z * 2.5);
    SurfaceInfo scene = MakeSurface(
        SdPlane(p - float3(0.0, gGroundY, 0.0), float3(0.0, 1.0, 0.0), 0.0),
        float3(groundPattern, groundPattern * 0.98, groundPattern * 0.94),
        0.0,
        0.0);

    const float3 heroCenter = float3(0.0, 1.0 + 0.08 * sin(animation), 0.0);
    const SurfaceInfo hero = MakeSurface(
        SdSphere(p - heroCenter, 1.0),
        float3(0.78, 0.84, 0.98),
        0.0,
        1.0);
    return SurfaceUnion(scene, hero);
}

SurfaceInfo EvaluateLessonFourScene(float3 p, float animation)
{
    const float groundPattern = 0.52 + 0.08 * sin(p.x * 2.5) * sin(p.z * 2.5);
    SurfaceInfo scene = MakeSurface(
        SdPlane(p - float3(0.0, gGroundY, 0.0), float3(0.0, 1.0, 0.0), 0.0),
        float3(groundPattern, groundPattern * 0.98, groundPattern * 0.94),
        0.0,
        0.0);

    SurfaceInfo sphere = MakeSurface(
        SdSphere(p - float3(-0.85, 1.00, 0.0), 0.95),
        float3(0.93, 0.62, 0.36),
        0.0,
        1.0);

    SurfaceInfo box = MakeSurface(
        SdBox(p - float3(0.78, 0.92, 0.0), float3(0.78, 0.78, 0.78)),
        float3(0.36, 0.72, 0.96),
        0.0,
        2.0);

    SurfaceInfo hero = SurfaceUnion(sphere, box);
    if (gActiveStep >= 2u)
    {
        hero = SurfaceSmoothUnion(sphere, box, gSmoothUnionK);
    }
    scene = SurfaceUnion(scene, hero);

    if (gActiveStep >= 3u)
    {
        float3 repeated = p - float3(0.0, 0.65, 4.4);
        repeated.xz = Repeat2D(repeated.xz, gRepeatSpacing);
        const SurfaceInfo repeatedSpheres = MakeSurface(
            SdSphere(repeated, 0.33),
            float3(0.78, 0.78, 0.86),
            0.0,
            3.0);
        scene = SurfaceUnion(scene, repeatedSpheres);
    }

    if (gActiveStep >= 4u)
    {
        float3 tower = p - float3(0.0, 1.28, 2.2);
        const float twistAmount = 0.35 + 0.12 * sin(animation * 0.85);
        tower = TwistY(tower, twistAmount);
        const SurfaceInfo twistedTower = MakeSurface(
            SdBox(tower, float3(0.32, 1.25, 0.32)),
            float3(0.46, 0.94, 0.76),
            0.0,
            4.0);
        scene = SurfaceUnion(scene, twistedTower);
    }

    return scene;
}

SurfaceInfo EvaluateLessonFiveScene(float3 p, float animation)
{
    SurfaceInfo scene = EvaluateLessonFourScene(p, animation);

    if (gLessonIndex >= 4u && gActiveStep >= 4u)
    {
        float3 torus = p - float3(0.0, 1.35 + 0.08 * sin(animation * 1.25), 0.0);
        torus.xz = Rotate2D(torus.xz, animation * 0.35);
        const SurfaceInfo emissiveRing = MakeSurface(
            SdTorus(torus, float2(1.55, 0.10)),
            float3(1.0, 0.72, 0.26),
            2.1,
            5.0);
        scene = SurfaceUnion(scene, emissiveRing);
    }

    return scene;
}

SurfaceInfo EvaluateScene(float3 p)
{
    const bool animateScene = (gFlags & 1u) != 0u;
    const float animation = animateScene ? gTime : 0.0;

    if (gLessonIndex == 0u)
    {
        return EvaluateLessonOneScene(p);
    }

    if (gLessonIndex == 1u)
    {
        return EvaluateLessonTwoScene(p, animation);
    }

    if (gLessonIndex == 2u)
    {
        return EvaluateLessonThreeScene(p, animation);
    }

    if (gLessonIndex == 3u)
    {
        return EvaluateLessonFourScene(p, animation);
    }

    return EvaluateLessonFiveScene(p, animation);
}

float ComputeHitEpsilon(float travel)
{
    const float pixelSpanAtTravel = 2.0 * gTanHalfFov * max(travel, 1.0) * gInvViewportHeight;
    return max(0.0006, pixelSpanAtTravel * 0.25);
}

MarchResult RayMarch(float3 ro, float3 rd)
{
    MarchResult result;
    result.hit = 0u;
    result.steps = 0u;
    result.travel = 0.0;
    result.position = ro;
    result.surface = MakeSurface(0.0, float3(0.0, 0.0, 0.0), 0.0, 0.0);

    const uint maxSteps = gLessonIndex >= 4u ? 128u : 96u;

    [loop]
    for (uint step = 0u; step < maxSteps; ++step)
    {
        const float3 p = ro + rd * result.travel;
        const SurfaceInfo surface = EvaluateScene(p);
        result.position = p;
        result.surface = surface;
        result.steps = step + 1u;

        const float epsilon = ComputeHitEpsilon(result.travel);
        if (surface.distance <= epsilon)
        {
            result.hit = 1u;
            break;
        }

        result.travel += surface.distance;
        if (result.travel > gMaxDistance)
        {
            break;
        }
    }

    return result;
}

float SceneDistance(float3 p)
{
    return EvaluateScene(p).distance;
}

float3 EstimateNormal(float3 p)
{
    const float2 h = float2(0.0016, 0.0);
    const float nx = SceneDistance(p + float3(h.x, h.y, h.y)) - SceneDistance(p - float3(h.x, h.y, h.y));
    const float ny = SceneDistance(p + float3(h.y, h.x, h.y)) - SceneDistance(p - float3(h.y, h.x, h.y));
    const float nz = SceneDistance(p + float3(h.y, h.y, h.x)) - SceneDistance(p - float3(h.y, h.y, h.x));
    return normalize(float3(nx, ny, nz));
}

float SoftShadow(float3 ro, float3 rd, float maxDistance)
{
    float shadow = 1.0;
    float t = 0.03;

    [loop]
    for (uint index = 0u; index < 40u; ++index)
    {
        const float h = SceneDistance(ro + rd * t);
        if (h < 0.0006)
        {
            return 0.0;
        }

        shadow = min(shadow, 14.0 * h / max(t, 0.001));
        t += clamp(h, 0.02, 0.28);
        if (t > maxDistance)
        {
            break;
        }
    }

    return saturate(shadow);
}

float AmbientOcclusion(float3 p, float3 normal)
{
    float occlusion = 0.0;
    float weight = 1.0;

    [unroll]
    for (uint index = 0u; index < 5u; ++index)
    {
        const float distance = 0.08 + 0.12 * index;
        const float sampleDistance = SceneDistance(p + normal * distance);
        occlusion += max(distance - sampleDistance, 0.0) * weight;
        weight *= 0.68;
    }

    return saturate(1.0 - 1.6 * occlusion);
}

uint ResolveViewMode()
{
    if (gDebugView != VIEW_AUTO)
    {
        return gDebugView;
    }

    if (gLessonIndex == 0u)
    {
        return gActiveStep == 1u ? VIEW_UV : (gActiveStep == 2u ? VIEW_RAY_DIRECTION : VIEW_SKY);
    }

    if (gLessonIndex == 1u)
    {
        return gActiveStep == 1u ? VIEW_HIT_MASK : (gActiveStep == 2u ? VIEW_MARCH_STEPS : VIEW_DEPTH);
    }

    if (gLessonIndex == 2u)
    {
        return gActiveStep == 1u ? VIEW_NORMALS : (gActiveStep == 2u ? VIEW_DIFFUSE : VIEW_FINAL);
    }

    if (gLessonIndex == 4u)
    {
        return gActiveStep == 1u ? VIEW_SHADOW : (gActiveStep == 2u ? VIEW_AO : VIEW_FINAL);
    }

    return VIEW_FINAL;
}

RayContext BuildRayContext(float2 uv)
{
    RayContext ray;
    ray.ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    const float2 rayOffset = float2(
        ray.ndc.x * gAspectRatio * gTanHalfFov,
        ray.ndc.y * gTanHalfFov);

    ray.origin = gCameraOrigin;
    ray.direction = normalize(gCameraForward + gCameraRight * rayOffset.x + gCameraUp * rayOffset.y);
    ray.sky = SkyColor(ray.direction);
    ray.viewMode = ResolveViewMode();
    return ray;
}

float MarkerRing(float2 uv, float2 center)
{
    const float2 delta = (uv - center) * float2(gAspectRatio, 1.0);
    const float dist = length(delta);
    const float outer = 1.0 - smoothstep(0.028, 0.034, dist);
    const float inner = 1.0 - smoothstep(0.014, 0.020, dist);
    return saturate(outer - inner);
}

float BoxMask(float2 uv, float2 minCorner, float2 maxCorner)
{
    const float insideX = step(minCorner.x, uv.x) * step(uv.x, maxCorner.x);
    const float insideY = step(minCorner.y, uv.y) * step(uv.y, maxCorner.y);
    return insideX * insideY;
}

float3 LessonOneProbeColor(RayContext probeRay)
{
    if (probeRay.viewMode == VIEW_UV)
    {
        return float3(gInspectUvX, gInspectUvY, 0.18 + 0.65 * gInspectUvX * gInspectUvY);
    }

    if (probeRay.viewMode == VIEW_RAY_DIRECTION)
    {
        return 0.5 + 0.5 * probeRay.direction;
    }

    return probeRay.sky;
}

float3 ApplyLessonOneProbeOverlay(float2 uv, RayContext probeRay, float3 baseColor)
{
    const float marker = MarkerRing(uv, float2(gInspectUvX, gInspectUvY));
    baseColor = lerp(baseColor, float3(1.0, 0.92, 0.20), marker);

    const float2 panelMin = float2(0.72, 0.05);
    const float2 panelMax = float2(0.96, 0.25);
    const float panel = BoxMask(uv, panelMin, panelMax);
    if (panel > 0.0)
    {
        const float2 panelUv = (uv - panelMin) / (panelMax - panelMin);
        const float borderX = step(panelUv.x, 0.04) + step(0.96, panelUv.x);
        const float borderY = step(panelUv.y, 0.06) + step(0.94, panelUv.y);
        const float border = saturate(borderX + borderY);
        const float3 probeColor = LessonOneProbeColor(probeRay);
        baseColor = lerp(probeColor, float3(0.08, 0.09, 0.11), border);
    }

    return baseColor;
}

float4 RenderLessonOne(float2 uv, RayContext ray)
{
    if (gLessonIndex != 0u)
    {
        return float4(-1.0, -1.0, -1.0, -1.0);
    }

    const RayContext probeRay = BuildRayContext(float2(gInspectUvX, gInspectUvY));
    float3 baseColor = float3(-1.0, -1.0, -1.0);

    if (gLessonIndex == 0u && gActiveStep == 1u && ray.viewMode == VIEW_UV)
    {
        baseColor = float3(uv, 0.18 + 0.65 * uv.x * uv.y);
    }
    else if (ray.viewMode == VIEW_RAY_DIRECTION)
    {
        baseColor = 0.5 + 0.5 * ray.direction;
    }
    else if (ray.viewMode == VIEW_SKY)
    {
        baseColor = ray.sky;
    }

    if (baseColor.x < 0.0)
    {
        return float4(-1.0, -1.0, -1.0, -1.0);
    }

    if ((gInspectFlags & 1u) != 0u)
    {
        baseColor = ApplyLessonOneProbeOverlay(uv, probeRay, baseColor);
    }

    return float4(baseColor, 1.0);
}

float4 RenderMiss(RayContext ray, MarchResult hit)
{
    if (ray.viewMode == VIEW_HIT_MASK)
    {
        return float4(0.04, 0.05, 0.07, 1.0);
    }

    if (ray.viewMode == VIEW_MARCH_STEPS)
    {
        return float4(Heatmap(hit.steps / 96.0) * 0.25, 1.0);
    }

    if (ray.viewMode == VIEW_DEPTH)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }

    return float4(ray.sky, 1.0);
}

float4 RenderHitDebug(RayContext ray, MarchResult hit, float3 normal, float shadow, float ao, float diffuse)
{
    if (ray.viewMode == VIEW_HIT_MASK)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }

    if (ray.viewMode == VIEW_MARCH_STEPS)
    {
        return float4(Heatmap(hit.steps / 96.0), 1.0);
    }

    if (ray.viewMode == VIEW_DEPTH)
    {
        const float depth = saturate(hit.travel / gMaxDistance);
        return float4(depth.xxx, 1.0);
    }

    if (ray.viewMode == VIEW_NORMALS)
    {
        return float4(0.5 + 0.5 * normal, 1.0);
    }

    if (ray.viewMode == VIEW_DIFFUSE)
    {
        return float4((0.08 + diffuse * shadow).xxx, 1.0);
    }

    if (ray.viewMode == VIEW_SHADOW)
    {
        return float4(shadow.xxx, 1.0);
    }

    if (ray.viewMode == VIEW_AO)
    {
        return float4(ao.xxx, 1.0);
    }

    return float4(-1.0, -1.0, -1.0, -1.0);
}

float3 ShadeSurface(RayContext ray, MarchResult hit, float3 normal, float shadow, float ao)
{
    const float3 lightDir = normalize(-gLightDirection);
    const float diffuse = saturate(dot(normal, lightDir));
    const float ambient = 0.20 + 0.45 * saturate(normal.y * 0.5 + 0.5);
    const float rim = pow(saturate(1.0 - dot(normal, -ray.direction)), 3.5);
    const float3 halfVector = normalize(lightDir - ray.direction);
    const float specular = pow(saturate(dot(normal, halfVector)), 48.0) * shadow;

    float3 color = hit.surface.albedo * (ambient * ao + diffuse * shadow * 1.12);
    color += float3(specular, specular, specular) * 0.14;
    color += hit.surface.albedo * rim * 0.08;
    color += hit.surface.emissive * hit.surface.albedo;

    if (gLessonIndex >= 4u && gActiveStep >= 3u)
    {
        const float fog = 1.0 - exp(-gFogDensity * hit.travel * hit.travel);
        color = lerp(color, ray.sky, saturate(fog));
    }

    return pow(saturate(color), 1.0 / 2.2);
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    const RayContext ray = BuildRayContext(uv);
    const float4 lessonOneColor = RenderLessonOne(uv, ray);
    if (lessonOneColor.x >= 0.0)
    {
        return lessonOneColor;
    }

    const MarchResult hit = RayMarch(ray.origin, ray.direction);
    if (hit.hit == 0u)
    {
        return RenderMiss(ray, hit);
    }

    const float3 normal = EstimateNormal(hit.position);
    const float3 lightDir = normalize(-gLightDirection);
    float shadow = 1.0;
    float ao = 1.0;

    if (gLessonIndex >= 4u && gActiveStep >= 1u)
    {
        shadow = SoftShadow(hit.position + normal * 0.01, lightDir, 10.0);
    }

    if (gLessonIndex >= 4u && gActiveStep >= 2u)
    {
        ao = AmbientOcclusion(hit.position, normal);
    }

    const float diffuse = saturate(dot(normal, lightDir));
    const float4 debugColor = RenderHitDebug(ray, hit, normal, shadow, ao, diffuse);
    if (debugColor.x >= 0.0)
    {
        return debugColor;
    }

    return float4(ShadeSurface(ray, hit, normal, shadow, ao), 1.0);
}
