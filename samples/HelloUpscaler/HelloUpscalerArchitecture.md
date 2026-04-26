# HelloUpscaler Architecture

This document explains how `HelloUpscaler` works and how it relates to the AMD FSDK.

## Big Picture

`HelloUpscaler` is the coordinator.

It does not implement the upscaling algorithm itself.

Its job is to:

1. Render the scene at an internal resolution.
2. Generate the inputs that FSR needs.
3. Create and maintain the FSR context.
4. Dispatch the AMD upscaler.
5. Present the final image on screen.

The AMD FSDK is the part that actually performs the temporal upscaling.

## Main Layers

### 1. Sample Layer

The sample lives in:

- [main.cpp](/C:/LightWeightD3D12/samples/HelloUpscaler/main.cpp)

This layer owns the frame flow and the UI.

Important responsibilities:

- choose internal render resolution
- create offscreen textures
- gather camera and frame data
- control FSR on/off
- feed inputs to the FSDK
- choose what gets presented

### 2. LightD3D12 Layer

This is the engine support layer used by the sample.

It provides:

- `DeviceManager`
- `RenderDevice`
- `ICommandBuffer`
- `ImguiRenderer`

This layer is responsible for:

- texture creation and destruction
- pipeline creation
- command recording
- swapchain handling
- exposing native DX12 handles when needed

### 3. AMD FSDK Layer

This is the actual upscaler SDK.

The sample uses it through:

- `ffx::Query`
- `ffx::CreateContext`
- `ffx::DispatchDescUpscale`
- `ffxApiGetResourceDX12(...)`

This layer is responsible for:

- deciding internal resolution from quality mode
- providing jitter information
- creating the temporal upscaler context
- running the upscale dispatch

## Important Sample Data

### AppState

`AppState` is the top-level runtime state of the sample.

It stores:

- `deviceManager`
- `imguiRenderer`
- render pipelines
- target textures
- FSR context state
- UI state
- frame timing
- jitter state

This is the structure that ties the whole sample together.

### OffscreenTargets

`OffscreenTargets` stores the textures used during the frame:

- `sceneColor`
- `sceneDepth`
- `motionVectors`
- `upscaledOutput`

It also stores:

- internal render width and height
- display width and height

### FsrState

`FsrState` stores the AMD FSR runtime state:

- `ffx::Context`
- context flags
- max render size
- max upscale size
- jitter phase count
- estimated GPU memory usage

## GPU Resources

These are the important textures in the sample.

### `sceneColor`

Purpose:

- stores the scene rendered at internal resolution

Usage:

- written during the scene pass
- later read by the FSR dispatch
- can be presented directly if FSR is disabled

Typical view types:

- `RTV`
- `SRV`

### `motionVectors`

Purpose:

- stores per-pixel motion from previous frame to current frame

Usage:

- written during the scene pass
- later read by the FSR dispatch

Typical view types:

- `RTV`
- `SRV`

### `sceneDepth`

Purpose:

- stores the scene depth buffer

Usage:

- written during the scene pass
- later read by the FSR dispatch

Typical view types:

- `DSV`
- `SRV`

### `upscaledOutput`

Purpose:

- stores the final FSR result at display resolution

Usage:

- written by the FSR dispatch
- read by the final present pass

Typical view types:

- `UAV`
- `SRV`

### `swapchain backbuffer`

Purpose:

- final image shown on screen

Usage:

- written in the final present pass
- also used by `ImGui` for the overlay

Typical view type:

- `RTV`

## Relationship With the FSDK

The relationship is simple:

1. The sample creates the textures and frame data.
2. `LightD3D12` exposes the native DX12 handles for those textures.
3. The sample wraps those native resources with `ffxApiGetResourceDX12(...)`.
4. The sample fills `ffx::DispatchDescUpscale`.
5. AMD FSDK performs the upscale and writes `upscaledOutput`.

So the sample is the setup layer, and the FSDK is the algorithm layer.

## Frame Flow

This is the frame from start to finish.

### Step 1. Begin frame

The sample:

- processes window messages
- updates `ImGui`
- reads current display size

### Step 2. Decide internal resolution

If FSR is enabled:

- the sample queries the FSDK for the render resolution that matches the selected quality mode

If FSR is disabled:

- the internal resolution matches the display resolution

### Step 3. Recreate targets if needed

If width or height changed, the sample recreates:

- `sceneColor`
- `sceneDepth`
- `motionVectors`
- `upscaledOutput`

### Step 4. Ensure the FSR context

If FSR is enabled, the sample checks whether the current FSR context still matches:

- render size
- upscale size
- flags

If not, it destroys and recreates the context.

### Step 5. Update jitter

The sample asks the FSDK for:

- jitter phase count
- current jitter offset

Then it applies that jitter to the projection matrix.

This is required for temporal reconstruction.

### Step 6. Scene pass

The scene is rasterized at internal resolution.

Outputs:

- `sceneColor`
- `motionVectors`
- `sceneDepth`

This is the pass that creates the inputs consumed by the upscaler.

### Step 7. FSR dispatch

If FSR is enabled, the sample:

- transitions resources into the required states
- builds `ffx::DispatchDescUpscale`
- passes color, motion vectors, depth, timing, camera info, and reset state
- dispatches AMD FSR

Input resources:

- `sceneColor`
- `motionVectors`
- `sceneDepth`

Output resource:

- `upscaledOutput`

### Step 8. Present pass

If FSR is disabled:

- the present pass samples `sceneColor`

If FSR is enabled:

- the present pass samples `upscaledOutput`

The result is written into the swapchain backbuffer.

### Step 9. ImGui pass

`ImguiRenderer` draws the overlay on top of the backbuffer.

This includes:

- FPS
- frame time
- FSR controls
- view mode
- debug view

## Why You May See Many RTVs in Debug

When debugging in PIX or RenderDoc, it is normal to see more RTVs than expected.

That does not mean the sample has many logical outputs.

What usually happens is:

- `sceneColor` has an `RTV`
- `motionVectors` has an `RTV`
- the swapchain has multiple backbuffers
- each backbuffer has its own `RTV`
- the debugger shows views and resources for the whole frame

So seeing around 5 or 6 RTVs is normal.

It does not mean FSR itself is generating 6 outputs.

## What Changes When Debug View Is Enabled

When `Show upscaler debug view` is enabled:

- the sample sets a debug flag in the FSR dispatch
- the FSDK changes what it writes into `upscaledOutput`

What does not change:

- the scene pass still writes `sceneColor`
- the scene pass still writes `motionVectors`
- the scene pass still writes `sceneDepth`

So debug view changes the content of the FSR output, not the high-level structure of the sample.

## Mental Model

A good way to think about the example is this:

- `HelloUpscaler` prepares the problem
- `LightD3D12` provides the rendering infrastructure
- `AMD FSDK` solves the upscale step
- the present pass shows the result

In short:

`Scene Pass -> FSR Dispatch -> Present Pass -> ImGui Overlay`
