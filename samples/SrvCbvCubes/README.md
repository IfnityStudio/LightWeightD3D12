# SRV + CBV Cubes

Small sample for comparing two bindless buffer paths:

- `MatrixRows` records are uploaded to a `StructuredBuffer` exposed as an SRV. The slider changes the cube count, so the SRV grows or shrinks.
- `CubeColorConstants` is uploaded to a constant buffer exposed as a CBV. The slider changes how many colors are active, up to `kMaxCubeColors`.
- The shader lives in `shaders/SrvCbvCubes.hlsl`, includes `LightD3D12_Defines.hlsli`, and reads both resources from fixed slots in `ResourceDescriptorHeap`.

This is intentionally simple: cube geometry is generated in the vertex shader with `SV_VertexID`, and instances are selected with `SV_InstanceID`.
