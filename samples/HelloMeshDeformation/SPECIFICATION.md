# HelloMeshDeformation Specification

This sample follows the mesh-deformation article from ShaderX3 and reuses the rubber duck asset from the classic Assimp sample.

## What Is Already Implemented

- `Twist` around the `Z` axis in the vertex shader.
- `Tape` along the `Z` axis by scaling `X` and `Y`.
- `Tape + Twist` as a chained deformation.
- `Hermite` or linear mapping for the tape factor.
- A wireframe overlay so the deformation is easy to inspect.

## Current Shader Layout

- [main.cpp](/C:/LightWeightD3D12/samples/HelloMeshDeformation/main.cpp)
- [HelloMeshDeformationVS.hlsl](/C:/LightWeightD3D12/samples/HelloMeshDeformation/shaders/HelloMeshDeformationVS.hlsl)
- [HelloMeshDeformationSolidPS.hlsl](/C:/LightWeightD3D12/samples/HelloMeshDeformation/shaders/HelloMeshDeformationSolidPS.hlsl)

## Spherification

The `Spherify` mode is fully implemented in the vertex shader.

### Target behavior

- Start from the normalized duck vertex position.
- Define a sphere center `c`.
- Define a sphere radius `r`.
- Compute a point on the sphere by casting a ray from `c` through the current vertex.
- Blend from the original vertex toward the sphere hit point with factor `f`.

### Chosen center for this sample

- Use `float3(0, 0, 0)` as the first sphere center.
- This is the correct simple choice here because the duck is already recentered around the origin on the CPU before the vertex buffer is created.
- You can verify that in [main.cpp](/C:/LightWeightD3D12/samples/HelloMeshDeformation/main.cpp), where the mesh center is subtracted from every imported vertex during normalization.
- If later you want artistic control, the next step is to expose the center as a push-constant or ImGui-controlled parameter.

### Position mapping

- Let `c` be the sphere center.
- Build the ray direction from `v - c`.
- Normalize that direction.
- The sphere target point is `c + dir * radius`.
- Blend from the original vertex toward that sphere point with the spherify factor.

### Normal handling

- Let `S` be the original normal.
- Let `E` be the target sphere normal at the hit point.
- Compute the rotation axis with `N = cross(S, E)`.
- Compute the rotation angle from the dot product between `S` and `E`.
- Rotate `S` around `N` by the blended angle.
- If `S` and `E` are opposite, choose a stable perpendicular fallback axis and rotate by `PI * factor`.

### Useful places to edit

- CPU parameters:
  [main.cpp](/C:/LightWeightD3D12/samples/HelloMeshDeformation/main.cpp)
- Vertex deformation entry point:
  [HelloMeshDeformationVS.hlsl](/C:/LightWeightD3D12/samples/HelloMeshDeformation/shaders/HelloMeshDeformationVS.hlsl)

## Notes

- The duck is normalized on the CPU so the deformation math stays in a compact range.
- The tape normal update is only an approximation.
- If you want exact taper lighting later, derive the deformation Jacobian and transform normals from it.
- If later you want artistic control, the next natural step is exposing the sphere center from CPU instead of fixing it at the origin.
