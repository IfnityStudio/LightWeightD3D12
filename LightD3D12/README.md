# LightD3D12

LightD3D12 is the new isolated D3D12 library layer in this repository.

Design goals:

- No direct dependency on Ifnity.
- No scene, mesh, material, or GLFW coupling in the core library.
- Optional Dear ImGui integration layered on top of the core through `LightD3D12Imgui`.
- A lightweight API split between `DeviceManager` for D3D12 bootstrap and `RenderDevice` for rendering work.
- Bindless-ready root signature with direct descriptor heap indexing.
- Multi-draw ready command signature through `ExecuteIndirect`.
- Separate implementation units for `ImmediateCommands`, `StagingDevice`, resources, and command buffers.
- Texture uploads can build their mip chain on the GPU through an internal compute path instead of expanding mip data on the CPU.

The `samples/HelloTriangle` project demonstrates the intended command flow:

```cpp
lightd3d12::DeviceManager deviceManager(contextDesc, swapchainDesc);
lightd3d12::RenderDevice* ctx = deviceManager.GetRenderDevice();

auto& buffer = ctx->AcquireCommandBuffer();
auto currentTexture = ctx->GetCurrentSwapchainTexture();

buffer.CmdBeginRendering(renderPass, framebuffer);
buffer.CmdBindRenderPipeline(trianglePipeline);
buffer.CmdPushDebugGroupLabel("Render Triangle", 0xff0000ff);
buffer.CmdDraw(3);
buffer.CmdPopDebugGroupLabel();
buffer.CmdEndRendering();

ctx->Submit(buffer, currentTexture);
```

## GPU mip generation

`TextureDesc` now exposes a single `countMipMap` field instead of the old `mipLevels + mipsEnabled` pair:

```cpp
TextureDesc desc{};
desc.width = image.width;
desc.height = image.height;
desc.countMipMap = CalculateMipCount(image.width, image.height);
desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
desc.usage = TextureUsage::Sampled;
desc.data = image.pixels.data();
desc.rowPitch = image.width * 4u;
desc.slicePitch = static_cast<uint32_t>(image.pixels.size());
```

Texture creation still owns resource/view format resolution. `BaseMips` only receives a fully created texture plus an internal UAV range reserved for the destination mip levels.

When `countMipMap` is greater than `1` and initial texture data is provided, LightD3D12 now:

1. Uploads only mip `0` through `StagingDevice`.
2. Allocates one contiguous internal bindless UAV range for the destination mip levels.
3. Dispatches the internal `BaseMips` compute pipeline to downsample each next level on the GPU.
4. Returns the resource to the texture's declared `initialState`.

This removes the previous CPU-side mip chain build, avoids temporary `std::vector<std::vector<uint8_t>>` allocations for texture data, and keeps the mip generation work close to the GPU where the texture will be consumed.

The compute shader used by `BaseMips` lives in [src/shaders/LightD3D12BaseMipsCS.hlsl](./src/shaders/LightD3D12BaseMipsCS.hlsl), so the mip generation logic stays isolated from the C++ orchestration code.

Current scope of the internal compute mip path:

- `Texture2D`
- `depthOrArraySize == 1`
- `DXGI_FORMAT_R8G8B8A8_UNORM`
- `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`

For `sRGB` textures, the compute pass samples through the `sRGB` SRV and writes the generated destination mip back through a `UNORM` UAV with explicit `linear -> sRGB` encoding, so the full mip chain stays consistent when later sampled as `sRGB`.
