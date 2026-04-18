# LightD3D12

LightD3D12 is the new isolated D3D12 library layer in this repository.

Design goals:

- No direct dependency on Ifnity.
- No scene, mesh, material, ImGui, or GLFW coupling.
- A lightweight API split between `DeviceManager` for D3D12 bootstrap and `RenderDevice` for rendering work.
- Bindless-ready root signature with direct descriptor heap indexing.
- Multi-draw ready command signature through `ExecuteIndirect`.
- Separate implementation units for `ImmediateCommands`, `StagingDevice`, resources, and command buffers.

The `samples/HelloTriangle` project demonstrates the intended command flow:

```cpp
lightd3d12::DeviceManager deviceManager(contextDesc, swapchainDesc);
lightd3d12::RenderDevice* ctx = deviceManager.getRenderDevice();

auto& buffer = ctx->acquireCommandBuffer();
auto currentTexture = ctx->getCurrentSwapchainTexture();

buffer.cmdBeginRendering(renderPass, framebuffer);
buffer.cmdBindRenderPipeline(trianglePipeline);
buffer.cmdPushDebugGroupLabel("Render Triangle", 0xff0000ff);
buffer.cmdDraw(3);
buffer.cmdPopDebugGroupLabel();
buffer.cmdEndRendering();

ctx->submit(buffer, currentTexture);
```
