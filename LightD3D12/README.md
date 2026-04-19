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
