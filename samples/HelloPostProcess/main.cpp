#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>

using namespace lightd3d12;

namespace
{
	struct ScenePushConstants
	{
		float rotationX = 0.0f;
		float rotationY = 0.0f;
		float rotationZ = 0.0f;
		float aspectRatio = 1.0f;
		float cubeScale = 1.0f;
		float time = 0.0f;
		float padding_[ 2 ] = {};
	};

	static_assert( sizeof( ScenePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct PostProcessPushConstants
	{
		uint32_t sourceTextureIndex = 0;
		uint32_t outputTextureIndex = 0;
		uint32_t textureWidth = 1;
		uint32_t textureHeight = 1;
		uint32_t effect = 0;
		uint32_t compareSplit = 0;
		float intensity = 1.0f;
		float time = 0.0f;
		float pixelSize = 10.0f;
		float chromaOffset = 2.0f;
		float vignetteStrength = 0.75f;
		float padding = 0.0f;
	};

	static_assert( sizeof( PostProcessPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct PresentPushConstants
	{
		uint32_t sourceTextureIndex = 0;
	};

	static_assert( sizeof( PresentPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct OffscreenTargets
	{
		TextureHandle sceneColor = {};
		TextureHandle sceneDepth = {};
		TextureHandle postOutput = {};
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState scenePipeline;
		ComputePipelineState postProcessPipeline;
		RenderPipelineState presentPipeline;
		OffscreenTargets targets;
		bool running = true;
		bool minimized = false;
		bool pauseAnimation = false;
		bool compareSplit = false;
		float animationTime = 0.0f;
		float rotationSpeed = 1.0f;
		float renderScale = 0.75f;
		float cubeScale = 0.92f;
		float intensity = 1.0f;
		float pixelSize = 10.0f;
		float chromaOffset = 2.0f;
		float vignetteStrength = 0.75f;
		int effectIndex = 0;
	};

	constexpr std::array<const char*, 7> ourEffectNames = {
		"None",
		"Grayscale",
		"Invert",
		"Vignette",
		"Pixelate",
		"Chromatic Aberration",
		"Scanlines",
	};

	RenderPipelineState CreateScenePipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat )
	{
		static constexpr char ourVertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    float gRotationX;
    float gRotationY;
    float gRotationZ;
    float gAspectRatio;
    float gCubeScale;
    float gTime;
    float2 gPadding;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
    float3 normal : TEXCOORD0;
};

float3 HueToRgb(float hue)
{
    const float3 k = float3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    const float3 p = abs(frac(hue + k) * 6.0 - 3.0);
    return saturate(p - 1.0);
}

float3 Rotate(float3 p)
{
    const float cosX = cos(gRotationX);
    const float sinX = sin(gRotationX);
    const float cosY = cos(gRotationY);
    const float sinY = sin(gRotationY);
    const float cosZ = cos(gRotationZ);
    const float sinZ = sin(gRotationZ);

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

VSOutput main(uint vertexID : SV_VertexID)
{
    float3 localPosition;
    float3 localNormal;
    GetCubeVertex(vertexID, localPosition, localNormal);

    float3 worldPosition = Rotate(localPosition * gCubeScale);
    worldPosition.xy += float2(sin(gTime * 0.9) * 0.25, cos(gTime * 0.7) * 0.12);
    worldPosition.z += 4.7;

    const float perspectiveScale = 1.45 / worldPosition.z;
    const float2 clipXY = float2(worldPosition.x * perspectiveScale / gAspectRatio, worldPosition.y * perspectiveScale);
    const float clipZ = saturate((worldPosition.z - 2.0) / 7.0);

    VSOutput output;
    output.position = float4(clipXY, clipZ, 1.0);
    output.normal = normalize(Rotate(localNormal));
    output.color = lerp(float3(1.0, 1.0, 1.0), HueToRgb(frac((float)vertexID / 36.0 + gTime * 0.05)), 0.9);
    return output;
}
)";

		static constexpr char ourPixelShader[] = R"(
float4 main(float4 position : SV_Position, float3 color : COLOR0, float3 normal : TEXCOORD0) : SV_Target0
{
    const float3 lightDirection = normalize(float3(-0.45, 0.80, -0.35));
    const float lambert = saturate(dot(normalize(normal), lightDirection)) * 0.78 + 0.22;
    return float4(color * lambert, 1.0);
}
)";

		RenderPipelineDesc desc{};
		desc.vertexShader.source = ourVertexShader;
		desc.vertexShader.entryPoint = "main";
		desc.vertexShader.profile = "vs_6_6";
		desc.fragmentShader.source = ourPixelShader;
		desc.fragmentShader.entryPoint = "main";
		desc.fragmentShader.profile = "ps_6_6";
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = depthFormat;
		desc.rasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	ComputePipelineState CreatePostProcessPipeline( RenderDevice& ctx )
	{
		static constexpr char ourComputeShader[] = R"(
cbuffer PushConstants : register(b0)
{
    uint gSourceTextureIndex;
    uint gOutputTextureIndex;
    uint gTextureWidth;
    uint gTextureHeight;
    uint gEffect;
    uint gCompareSplit;
    float gIntensity;
    float gTime;
    float gPixelSize;
    float gChromaOffset;
    float gVignetteStrength;
    float gPadding;
};

SamplerState gSampler : register(s0);

float3 ApplyPostProcess(Texture2D<float4> sourceTexture, float2 uv, float2 pixelSize)
{
    float3 color = sourceTexture.SampleLevel(gSampler, uv, 0).rgb;

    if (gEffect == 1u)
    {
        const float gray = dot(color, float3(0.299, 0.587, 0.114));
        color = lerp(color, gray.xxx, gIntensity);
    }
    else if (gEffect == 2u)
    {
        color = lerp(color, 1.0 - color, gIntensity);
    }
    else if (gEffect == 3u)
    {
        const float2 centeredUv = uv * 2.0 - 1.0;
        const float vignette = saturate(1.0 - dot(centeredUv, centeredUv) * gVignetteStrength);
        color *= lerp(1.0, vignette, gIntensity);
    }
    else if (gEffect == 4u)
    {
        const float blockSize = max(gPixelSize, 1.0);
        const float2 quantizedPixel = floor(uv / (pixelSize * blockSize)) * pixelSize * blockSize;
        color = sourceTexture.SampleLevel(gSampler, quantizedPixel + pixelSize * blockSize * 0.5, 0).rgb;
    }
    else if (gEffect == 5u)
    {
        const float2 offset = float2(gChromaOffset, 0.0) * pixelSize * gIntensity;
        color.r = sourceTexture.SampleLevel(gSampler, uv + offset, 0).r;
        color.g = sourceTexture.SampleLevel(gSampler, uv, 0).g;
        color.b = sourceTexture.SampleLevel(gSampler, uv - offset, 0).b;
    }
    else if (gEffect == 6u)
    {
        const float scanline = 0.72 + 0.28 * sin((uv.y * gTextureHeight + gTime * 80.0) * 3.14159);
        color *= lerp(1.0, scanline, gIntensity);
    }

    return color;
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= gTextureWidth || dispatchThreadID.y >= gTextureHeight)
    {
        return;
    }

    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[gSourceTextureIndex];
    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[gOutputTextureIndex];

    const float2 dimensions = float2((float)gTextureWidth, (float)gTextureHeight);
    const float2 pixelSize = 1.0 / dimensions;
    const float2 uv = ((float2)dispatchThreadID.xy + 0.5) * pixelSize;
    const float3 originalColor = sourceTexture.SampleLevel(gSampler, uv, 0).rgb;
    float3 processedColor = ApplyPostProcess(sourceTexture, uv, pixelSize);

    if (gCompareSplit != 0u)
    {
        const float splitLine = abs(uv.x - 0.5) < pixelSize.x * 2.0 ? 1.0 : 0.0;
        processedColor = uv.x < 0.5 ? originalColor : processedColor;
        processedColor = lerp(processedColor, float3(1.0, 0.95, 0.25), splitLine);
    }

    outputTexture[dispatchThreadID.xy] = float4(processedColor, 1.0);
}
)";

		ComputePipelineDesc desc{};
		desc.computeShader.source = ourComputeShader;
		desc.computeShader.entryPoint = "main";
		desc.computeShader.profile = "cs_6_6";
		return ctx.CreateComputePipeline( desc );
	}

	RenderPipelineState CreatePresentPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat )
	{
		static constexpr char ourVertexShader[] = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    VSOutput output;
    output.position = float4(positions[vertexID], 0.0, 1.0);
    output.uv = float2(positions[vertexID].x * 0.5 + 0.5, 0.5 - positions[vertexID].y * 0.5);
    return output;
}
)";

		static constexpr char ourPixelShader[] = R"(
cbuffer PushConstants : register(b0)
{
    uint gSourceTextureIndex;
};

SamplerState gSampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[gSourceTextureIndex];
    return float4(sourceTexture.SampleLevel(gSampler, uv, 0).rgb, 1.0);
}
)";

		RenderPipelineDesc desc{};
		desc.vertexShader.source = ourVertexShader;
		desc.vertexShader.entryPoint = "main";
		desc.vertexShader.profile = "vs_6_6";
		desc.fragmentShader.source = ourPixelShader;
		desc.fragmentShader.entryPoint = "main";
		desc.fragmentShader.profile = "ps_6_6";
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = DXGI_FORMAT_UNKNOWN;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	void DestroyTargets( RenderDevice& ctx, OffscreenTargets& targets )
	{
		if( targets.sceneColor.Valid() )
		{
			ctx.Destroy( targets.sceneColor );
			targets.sceneColor = {};
		}

		if( targets.sceneDepth.Valid() )
		{
			ctx.Destroy( targets.sceneDepth );
			targets.sceneDepth = {};
		}

		if( targets.postOutput.Valid() )
		{
			ctx.Destroy( targets.postOutput );
			targets.postOutput = {};
		}

		targets.width = 0;
		targets.height = 0;
	}

	void RecreateTargets( AppState& app )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const uint32_t targetWidth = std::max<uint32_t>( 64u, static_cast<uint32_t>( std::lround( app.deviceManager->GetWidth() * app.renderScale ) ) );
		const uint32_t targetHeight = std::max<uint32_t>( 64u, static_cast<uint32_t>( std::lround( app.deviceManager->GetHeight() * app.renderScale ) ) );

		if( app.targets.sceneColor.Valid() && app.targets.sceneDepth.Valid() && app.targets.postOutput.Valid() &&
			app.targets.width == targetWidth && app.targets.height == targetHeight )
		{
			return;
		}

		ctx.WaitIdle();
		DestroyTargets( ctx, app.targets );

		TextureDesc colorDesc{};
		colorDesc.debugName = "HelloPostProcess Scene Color";
		colorDesc.width = targetWidth;
		colorDesc.height = targetHeight;
		colorDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		colorDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
		colorDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		colorDesc.useClearValue = true;
		colorDesc.clearValue.Format = colorDesc.format;
		colorDesc.clearValue.Color[ 0 ] = 0.05f;
		colorDesc.clearValue.Color[ 1 ] = 0.08f;
		colorDesc.clearValue.Color[ 2 ] = 0.12f;
		colorDesc.clearValue.Color[ 3 ] = 1.0f;
		app.targets.sceneColor = ctx.CreateTexture( colorDesc );

		TextureDesc depthDesc{};
		depthDesc.debugName = "HelloPostProcess Scene Depth";
		depthDesc.width = targetWidth;
		depthDesc.height = targetHeight;
		depthDesc.format = DXGI_FORMAT_D32_FLOAT;
		depthDesc.usage = TextureUsage::DepthStencil;
		depthDesc.useClearValue = true;
		depthDesc.clearValue.Format = depthDesc.format;
		depthDesc.clearValue.DepthStencil.Depth = 1.0f;
		depthDesc.clearValue.DepthStencil.Stencil = 0;
		app.targets.sceneDepth = ctx.CreateTexture( depthDesc );

		TextureDesc outputDesc{};
		outputDesc.debugName = "HelloPostProcess Compute Output";
		outputDesc.width = targetWidth;
		outputDesc.height = targetHeight;
		outputDesc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		outputDesc.usage = TextureUsage::Sampled | TextureUsage::UnorderedAccess;
		outputDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		app.targets.postOutput = ctx.CreateTexture( outputDesc );

		app.targets.width = targetWidth;
		app.targets.height = targetHeight;
	}

	LRESULT CALLBACK WindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		auto* app = reinterpret_cast<AppState*>( GetWindowLongPtr( hwnd, GWLP_USERDATA ) );

		if( app != nullptr && app->imguiRenderer && app->imguiRenderer->ProcessMessage( hwnd, message, wParam, lParam ) )
		{
			return 1;
		}

		switch( message )
		{
			case WM_SIZE:
			{
				if( app != nullptr && app->deviceManager )
				{
					const uint32_t width = LOWORD( lParam );
					const uint32_t height = HIWORD( lParam );
					app->minimized = width == 0 || height == 0;
					if( !app->minimized )
					{
						app->deviceManager->Resize( width, height );
					}
				}
				return 0;
			}

			case WM_CLOSE:
				if( app != nullptr )
				{
					app->running = false;
					app->minimized = true;
				}
				return 0;

			case WM_DESTROY:
				PostQuitMessage( 0 );
				return 0;

			default:
				return DefWindowProc( hwnd, message, wParam, lParam );
		}
	}
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE, PWSTR, int showCommand )
{
	try
	{
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = L"LightD3D12HelloPostProcessWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1280;
		constexpr uint32_t kInitialHeight = 820;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Post Process",
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			static_cast<int>( kInitialWidth ),
			static_cast<int>( kInitialHeight ),
			nullptr,
			nullptr,
			instance,
			nullptr );

		if( hwnd == nullptr )
		{
			throw std::runtime_error( "Failed to create Win32 window." );
		}

		ShowWindow( hwnd, showCommand );
		UpdateWindow( hwnd );

		AppState app{};
		SetWindowLongPtr( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( &app ) );

		ContextDesc contextDesc{};
		contextDesc.enableDebugLayer = true;
		contextDesc.swapchainBufferCount = 3;

		SwapchainDesc swapchainDesc{};
		swapchainDesc.window = MakeWin32WindowHandle( hwnd );
		swapchainDesc.width = kInitialWidth;
		swapchainDesc.height = kInitialHeight;
		swapchainDesc.vsync = true;

		app.deviceManager = std::make_unique<DeviceManager>( contextDesc, swapchainDesc );
		app.imguiRenderer = std::make_unique<ImguiRenderer>( *app.deviceManager, swapchainDesc.window );

		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		app.scenePipeline = CreateScenePipeline( ctx, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT );
		app.postProcessPipeline = CreatePostProcessPipeline( ctx );
		app.presentPipeline = CreatePresentPipeline( ctx, contextDesc.swapchainFormat );
		RecreateTargets( app );

		auto lastFrameTime = std::chrono::steady_clock::now();
		MSG message{};
		while( app.running )
		{
			while( PeekMessage( &message, nullptr, 0, 0, PM_REMOVE ) )
			{
				if( message.message == WM_QUIT )
				{
					app.running = false;
					break;
				}

				TranslateMessage( &message );
				DispatchMessage( &message );
			}

			if( !app.running || app.minimized )
			{
				continue;
			}

			RecreateTargets( app );

			const auto now = std::chrono::steady_clock::now();
			float deltaSeconds = std::chrono::duration<float>( now - lastFrameTime ).count();
			lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );
			if( !app.pauseAnimation )
			{
				app.animationTime += deltaSeconds * app.rotationSpeed;
			}

			app.imguiRenderer->NewFrame();
			ImGui::SetNextWindowPos( ImVec2( 20.0f, 20.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowSize( ImVec2( 390.0f, 0.0f ), ImGuiCond_FirstUseEver );
			ImGui::Begin( "Hello PostProcess" );
			ImGui::TextWrapped( "Raster scene -> offscreen texture -> compute postprocess -> fullscreen present." );
			ImGui::Separator();
			ImGui::Combo( "Effect", &app.effectIndex, ourEffectNames.data(), static_cast<int>( ourEffectNames.size() ) );
			ImGui::Checkbox( "Compare original / processed", &app.compareSplit );
			ImGui::Checkbox( "Pause animation", &app.pauseAnimation );
			ImGui::SliderFloat( "Intensity", &app.intensity, 0.0f, 1.0f, "%.2f" );
			ImGui::SliderFloat( "Render scale", &app.renderScale, 0.35f, 1.0f, "%.2f" );
			ImGui::SliderFloat( "Cube scale", &app.cubeScale, 0.45f, 1.25f, "%.2f" );
			ImGui::SliderFloat( "Rotation speed", &app.rotationSpeed, 0.0f, 2.5f, "%.2f" );
			if( app.effectIndex == 4 )
			{
				ImGui::SliderFloat( "Pixel size", &app.pixelSize, 2.0f, 48.0f, "%.0f" );
			}
			if( app.effectIndex == 5 )
			{
				ImGui::SliderFloat( "Chroma offset", &app.chromaOffset, 0.0f, 8.0f, "%.1f" );
			}
			if( app.effectIndex == 3 )
			{
				ImGui::SliderFloat( "Vignette strength", &app.vignetteStrength, 0.0f, 1.6f, "%.2f" );
			}
			ImGui::Text( "Internal target: %u x %u", app.targets.width, app.targets.height );
			ImGui::End();

			auto& commandBuffer = ctx.AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx.GetCurrentSwapchainTexture();

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloPostProcess::ScenePass", 0xff4cc9f0u );

				RenderPass scenePass{};
				scenePass.color[ 0 ].loadOp = LoadOp::Clear;
				scenePass.color[ 0 ].clearColor = { 0.05f, 0.08f, 0.12f, 1.0f };
				scenePass.depthStencil.depthLoadOp = LoadOp::Clear;
				scenePass.depthStencil.depthStoreOp = StoreOp::Store;
				scenePass.depthStencil.clearDepth = 1.0f;

				Framebuffer sceneFramebuffer{};
				sceneFramebuffer.color[ 0 ].texture = app.targets.sceneColor;
				sceneFramebuffer.depthStencil.texture = app.targets.sceneDepth;

				ScenePushConstants scenePushConstants{};
				scenePushConstants.rotationX = app.animationTime * 0.71f;
				scenePushConstants.rotationY = app.animationTime;
				scenePushConstants.rotationZ = app.animationTime * 0.37f;
				scenePushConstants.aspectRatio = static_cast<float>( app.targets.width ) / static_cast<float>( app.targets.height );
				scenePushConstants.cubeScale = app.cubeScale;
				scenePushConstants.time = app.animationTime;

				commandBuffer.CmdBeginRendering( scenePass, sceneFramebuffer );
				commandBuffer.CmdBindRenderPipeline( app.scenePipeline );
				commandBuffer.CmdPushConstants( &scenePushConstants, sizeof( scenePushConstants ) );
				commandBuffer.CmdDraw( 36 );
				commandBuffer.CmdEndRendering();
				commandBuffer.CmdTransitionTexture( app.targets.sceneColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			}

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloPostProcess::ComputePass", 0xff90be6du );

				PostProcessPushConstants postPushConstants{};
				postPushConstants.sourceTextureIndex = ctx.GetBindlessIndex( app.targets.sceneColor );
				postPushConstants.outputTextureIndex = ctx.GetUnorderedAccessIndex( app.targets.postOutput );
				postPushConstants.textureWidth = app.targets.width;
				postPushConstants.textureHeight = app.targets.height;
				postPushConstants.effect = static_cast<uint32_t>( app.effectIndex );
				postPushConstants.compareSplit = app.compareSplit ? 1u : 0u;
				postPushConstants.intensity = app.intensity;
				postPushConstants.time = app.animationTime;
				postPushConstants.pixelSize = app.pixelSize;
				postPushConstants.chromaOffset = app.chromaOffset;
				postPushConstants.vignetteStrength = app.vignetteStrength;

				commandBuffer.CmdTransitionTexture( app.targets.postOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
				commandBuffer.CmdBindComputePipeline( app.postProcessPipeline );
				commandBuffer.CmdPushConstants( &postPushConstants, sizeof( postPushConstants ) );
				commandBuffer.CmdDispatch( ( app.targets.width + 15u ) / 16u, ( app.targets.height + 15u ) / 16u, 1u );
				commandBuffer.CmdTransitionTexture( app.targets.postOutput, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			}

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloPostProcess::PresentPass", 0xfff9c74fu );

				RenderPass presentPass{};
				presentPass.color[ 0 ].loadOp = LoadOp::Clear;
				presentPass.color[ 0 ].clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };

				Framebuffer presentFramebuffer{};
				presentFramebuffer.color[ 0 ].texture = currentTexture;

				PresentPushConstants presentPushConstants{};
				presentPushConstants.sourceTextureIndex = ctx.GetBindlessIndex( app.targets.postOutput );

				commandBuffer.CmdBeginRendering( presentPass, presentFramebuffer );
				commandBuffer.CmdBindRenderPipeline( app.presentPipeline );
				commandBuffer.CmdPushConstants( &presentPushConstants, sizeof( presentPushConstants ) );
				commandBuffer.CmdDraw( 3 );
				commandBuffer.CmdEndRendering();
			}

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloPostProcess::ImguiPass", 0xfff9844au );

				RenderPass imguiPass{};
				imguiPass.color[ 0 ].loadOp = LoadOp::Load;

				Framebuffer imguiFramebuffer{};
				imguiFramebuffer.color[ 0 ].texture = currentTexture;

				commandBuffer.CmdBeginRendering( imguiPass, imguiFramebuffer );
				app.imguiRenderer->Render( commandBuffer );
				commandBuffer.CmdEndRendering();
			}

			ctx.Submit( commandBuffer, currentTexture );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		ctx.WaitIdle();
		DestroyTargets( ctx, app.targets );
		app.scenePipeline = {};
		app.postProcessPipeline = {};
		app.presentPipeline = {};
		app.imguiRenderer.reset();
		app.deviceManager.reset();
		DestroyWindow( hwnd );
	}
	catch( const std::exception& exception )
	{
		MessageBoxA( nullptr, exception.what(), "LightD3D12 Hello PostProcess", MB_ICONERROR | MB_OK );
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
