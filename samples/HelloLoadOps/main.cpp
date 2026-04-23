#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <stdexcept>

using namespace lightd3d12;

namespace
{
	struct CubePushConstants
	{
		float rotationX = 0.0f;
		float rotationY = 0.0f;
		float rotationZ = 0.0f;
		float aspectRatio = 1.0f;
		float colorPhase = 0.0f;
		float orbitX = 0.0f;
		float orbitY = 0.0f;
		float cubeScale = 1.0f;
	};

	static_assert( sizeof( CubePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct ViewportTargets
	{
		TextureHandle color = {};
		TextureHandle depth = {};
		uint32_t extent = 0;
		bool forceClearOnNextFrame = false;
	};

	struct LoadOpViewport
	{
		const char* label = "";
		const char* description = "";
		LoadOp loadOp = LoadOp::Load;
		ViewportTargets targets = {};
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState cubePipeline;
		RenderPipelineState cubeWireframePipeline;
		std::array<LoadOpViewport, 3> viewports = {};
		bool running = true;
		bool minimized = false;
		bool pauseAnimation = false;
		bool showWireframe = true;
		bool recreateViewportTargets = false;
		float animationTime = 0.0f;
		float rotationSpeed = 1.0f;
		float orbitRadius = 0.55f;
		float cubeScale = 0.82f;
		float colorPhase = 0.0f;
		uint32_t viewportExtent = 512;
		std::array<float, 4> clearColor = { 0.08f, 0.10f, 0.14f, 1.0f };
	};

	void RandomizeVertexColors( float& colorPhase )
	{
		static std::mt19937 ourRandomGenerator( std::random_device{}() );
		std::uniform_real_distribution<float> distribution( 0.0f, 1.0f );
		colorPhase = distribution( ourRandomGenerator );
	}

	RenderPipelineState CreateCubePipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, bool wireframe )
	{
		static constexpr char ourVertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    float gRotationX;
    float gRotationY;
    float gRotationZ;
    float gAspectRatio;
    float gColorPhase;
    float gOrbitX;
    float gOrbitY;
    float gCubeScale;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

float3 HueToRgb(float hue)
{
    const float3 k = float3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    const float3 p = abs(frac(hue + k) * 6.0 - 3.0);
    return saturate(p - 1.0);
}

VSOutput main(uint vertexID : SV_VertexID)
{
    static const float3 positions[8] =
    {
        float3(-1.0, -1.0, -1.0),
        float3( 1.0, -1.0, -1.0),
        float3( 1.0,  1.0, -1.0),
        float3(-1.0,  1.0, -1.0),
        float3(-1.0, -1.0,  1.0),
        float3( 1.0, -1.0,  1.0),
        float3( 1.0,  1.0,  1.0),
        float3(-1.0,  1.0,  1.0)
    };

    static const uint indices[36] =
    {
        0, 1, 2, 0, 2, 3,
        4, 6, 5, 4, 7, 6,
        4, 5, 1, 4, 1, 0,
        3, 2, 6, 3, 6, 7,
        1, 5, 6, 1, 6, 2,
        4, 0, 3, 4, 3, 7
    };

    VSOutput output;
    const uint cubeVertexIndex = indices[vertexID];
    float3 position = positions[cubeVertexIndex] * gCubeScale;

    const float cosX = cos(gRotationX);
    const float sinX = sin(gRotationX);
    const float cosY = cos(gRotationY);
    const float sinY = sin(gRotationY);
    const float cosZ = cos(gRotationZ);
    const float sinZ = sin(gRotationZ);

    float3 rotatedX = float3(
        position.x,
        position.y * cosX - position.z * sinX,
        position.y * sinX + position.z * cosX);

    float3 rotatedY = float3(
        rotatedX.x * cosY + rotatedX.z * sinY,
        rotatedX.y,
        -rotatedX.x * sinY + rotatedX.z * cosY);

    float3 rotatedZ = float3(
        rotatedY.x * cosZ - rotatedY.y * sinZ,
        rotatedY.x * sinZ + rotatedY.y * cosZ,
        rotatedY.z);

    rotatedZ.x += gOrbitX;
    rotatedZ.y += gOrbitY;
    rotatedZ.z += 4.6;

    const float perspectiveScale = 1.35 / rotatedZ.z;
    const float2 clipXY = float2(rotatedZ.x * perspectiveScale / gAspectRatio, rotatedZ.y * perspectiveScale);
    const float clipZ = saturate((rotatedZ.z - 2.5) / 6.0);

    output.position = float4(clipXY, clipZ, 1.0);
    const float hue = frac(gColorPhase + (float)cubeVertexIndex / 8.0);
    output.color = lerp(float3(1.0, 1.0, 1.0), HueToRgb(hue), 0.92);
    return output;
}
)";

		static constexpr char ourSolidPixelShader[] = R"(
float4 main(float4 position : SV_Position, float3 color : COLOR0) : SV_Target0
{
    return float4(color, 1.0);
}
)";

		static constexpr char ourWireframePixelShader[] = R"(
float4 main(float4 position : SV_Position, float3 color : COLOR0) : SV_Target0
{
    return float4(0.04, 0.05, 0.07, 1.0);
}
)";

		RenderPipelineDesc desc{};
		desc.vertexShader.source = ourVertexShader;
		desc.vertexShader.entryPoint = "main";
		desc.vertexShader.profile = "vs_6_6";
		desc.fragmentShader.source = wireframe ? ourWireframePixelShader : ourSolidPixelShader;
		desc.fragmentShader.entryPoint = "main";
		desc.fragmentShader.profile = "ps_6_6";
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = depthFormat;
		desc.rasterizerState.FillMode = wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = wireframe ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = wireframe ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_LESS;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	void DestroyViewportTargets( RenderDevice& ctx, ViewportTargets& targets )
	{
		if( targets.color.Valid() )
		{
			ctx.Destroy( targets.color );
			targets.color = {};
		}

		if( targets.depth.Valid() )
		{
			ctx.Destroy( targets.depth );
			targets.depth = {};
		}

		targets.extent = 0;
		targets.forceClearOnNextFrame = false;
	}

	void InitializeViewports( AppState& app )
	{
		app.viewports[ 0 ] = {
			.label = "Load",
			.description = "The color attachment loads the previous frame. Because this demo only redraws the cube, old pixels stay visible and the motion leaves trails.",
			.loadOp = LoadOp::Load
		};

		app.viewports[ 1 ] = {
			.label = "Clear",
			.description = "The color attachment is cleared every frame. You only see the current cube pose on top of the chosen clear color.",
			.loadOp = LoadOp::Clear
		};

		app.viewports[ 2 ] = {
			.label = "DontCare",
			.description = "Previous color contents are undefined. Untouched pixels may look black, stale or random depending on the driver and GPU.",
			.loadOp = LoadOp::DontCare
		};
	}

	void RecreateViewportTargets( AppState& app, bool forceRecreate )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const DXGI_FORMAT colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		const DXGI_FORMAT depthFormat = DXGI_FORMAT_D32_FLOAT;

		for( LoadOpViewport& viewport : app.viewports )
		{
			if( !forceRecreate && viewport.targets.color.Valid() && viewport.targets.extent == app.viewportExtent )
			{
				continue;
			}

			DestroyViewportTargets( ctx, viewport.targets );

			TextureDesc colorDesc{};
			colorDesc.debugName = std::string( "LoadOp Sandbox " ) + viewport.label + " Color";
			colorDesc.width = app.viewportExtent;
			colorDesc.height = app.viewportExtent;
			colorDesc.format = colorFormat;
			colorDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
			colorDesc.useClearValue = true;
			colorDesc.clearValue.Format = colorFormat;
			colorDesc.clearValue.Color[ 0 ] = app.clearColor[ 0 ];
			colorDesc.clearValue.Color[ 1 ] = app.clearColor[ 1 ];
			colorDesc.clearValue.Color[ 2 ] = app.clearColor[ 2 ];
			colorDesc.clearValue.Color[ 3 ] = app.clearColor[ 3 ];
			viewport.targets.color = ctx.CreateTexture( colorDesc );

			TextureDesc depthDesc{};
			depthDesc.debugName = std::string( "LoadOp Sandbox " ) + viewport.label + " Depth";
			depthDesc.width = app.viewportExtent;
			depthDesc.height = app.viewportExtent;
			depthDesc.format = depthFormat;
			depthDesc.usage = TextureUsage::DepthStencil;
			depthDesc.useClearValue = true;
			depthDesc.clearValue.Format = depthFormat;
			depthDesc.clearValue.DepthStencil.Depth = 1.0f;
			depthDesc.clearValue.DepthStencil.Stencil = 0;
			viewport.targets.depth = ctx.CreateTexture( depthDesc );

			viewport.targets.extent = app.viewportExtent;
			viewport.targets.forceClearOnNextFrame = viewport.loadOp == LoadOp::Load;
		}
	}

	CubePushConstants BuildSceneConstants( const AppState& app, float aspectRatio )
	{
		CubePushConstants constants{};
		const float time = app.animationTime;
		const float angularSpeed = app.rotationSpeed;
		const float orbitPhase = time * angularSpeed * 0.9f;
		constants.rotationX = time * angularSpeed * 0.75f;
		constants.rotationY = time * angularSpeed * 1.08f;
		constants.rotationZ = time * angularSpeed * 0.52f;
		constants.aspectRatio = aspectRatio;
		constants.colorPhase = app.colorPhase;
		constants.orbitX = std::cos( orbitPhase ) * app.orbitRadius;
		constants.orbitY = std::sin( orbitPhase * 1.13f ) * app.orbitRadius * 0.65f;
		constants.cubeScale = app.cubeScale;
		return constants;
	}

	void RenderViewportScene( AppState& app, ICommandBuffer& buffer, LoadOpViewport& viewport, const CubePushConstants& constants )
	{
		// The key teaching point:
		// - depth is always cleared so the cube itself remains stable and readable
		// - only the color attachment changes its LoadOp behaviour
		RenderPass renderPass{};
		renderPass.color[ 0 ].loadOp = viewport.targets.forceClearOnNextFrame ? LoadOp::Clear : viewport.loadOp;
		renderPass.color[ 0 ].storeOp = StoreOp::Store;
		renderPass.color[ 0 ].clearColor = app.clearColor;
		renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
		renderPass.depthStencil.depthStoreOp = StoreOp::Store;
		renderPass.depthStencil.clearDepth = 1.0f;

		Framebuffer framebuffer{};
		framebuffer.color[ 0 ].texture = viewport.targets.color;
		framebuffer.depthStencil.texture = viewport.targets.depth;

		buffer.CmdBeginRendering( renderPass, framebuffer );
		buffer.CmdBindRenderPipeline( app.cubePipeline );
		buffer.CmdPushDebugGroupLabel( viewport.label, 0xff4cc9f0 );
		buffer.CmdPushConstants( &constants, sizeof( constants ) );
		buffer.CmdDraw( 36 );
		if( app.showWireframe )
		{
			buffer.CmdBindRenderPipeline( app.cubeWireframePipeline );
			buffer.CmdPushConstants( &constants, sizeof( constants ) );
			buffer.CmdDraw( 36 );
		}
		buffer.CmdPopDebugGroupLabel();
		buffer.CmdEndRendering();
		buffer.CmdTransitionTexture( viewport.targets.color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		viewport.targets.forceClearOnNextFrame = false;
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

			case WM_DESTROY:
				PostQuitMessage( 0 );
				return 0;

			case WM_CLOSE:
			{
				if( app != nullptr )
				{
					app->running = false;
					app->minimized = true;
				}
				return 0;
			}

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
		windowClass.lpszClassName = L"LightD3D12HelloLoadOpsWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1540;
		constexpr uint32_t kInitialHeight = 960;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 LoadOp Sandbox",
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
		RandomizeVertexColors( app.colorPhase );
		InitializeViewports( app );
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
		app.cubePipeline = CreateCubePipeline( ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, false );
		app.cubeWireframePipeline = CreateCubePipeline( ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, true );
		RecreateViewportTargets( app, true );

		auto previousTime = std::chrono::steady_clock::now();
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

			if( !app.running )
			{
				break;
			}

			const auto currentTime = std::chrono::steady_clock::now();
			const float deltaTime = std::chrono::duration<float>( currentTime - previousTime ).count();
			previousTime = currentTime;

			if( app.minimized )
			{
				Sleep( 16 );
				continue;
			}

			if( !app.pauseAnimation )
			{
				app.animationTime += deltaTime;
			}

			if( app.recreateViewportTargets )
			{
				RecreateViewportTargets( app, true );
				app.recreateViewportTargets = false;
			}
			else
			{
				RecreateViewportTargets( app, false );
			}

			app.imguiRenderer->NewFrame();

			ImGui::SetNextWindowPos( ImVec2( 18.0f, 18.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowSize( ImVec2( 1480.0f, 860.0f ), ImGuiCond_FirstUseEver );
			ImGui::Begin( "LoadOp Sandbox" );
			ImGui::TextWrapped(
				"Each panel renders the same cube into its own persistent render target. The only difference is the color attachment LoadOp. "
				"This lets you compare how previous pixels are preserved, cleared or discarded." );
			ImGui::Separator();
			ImGui::Checkbox( "Pause animation", &app.pauseAnimation );
			ImGui::SameLine();
			ImGui::Checkbox( "Wireframe overlay", &app.showWireframe );
			ImGui::SliderFloat( "Rotation speed", &app.rotationSpeed, 0.0f, 3.0f, "%.2f" );
			ImGui::SliderFloat( "Orbit radius", &app.orbitRadius, 0.0f, 1.20f, "%.2f" );
			ImGui::SliderFloat( "Cube scale", &app.cubeScale, 0.35f, 1.10f, "%.2f" );
			int viewportExtent = static_cast<int>( app.viewportExtent );
			if( ImGui::SliderInt( "Viewport extent", &viewportExtent, 256, 1024 ) )
			{
				app.viewportExtent = static_cast<uint32_t>( viewportExtent );
				app.recreateViewportTargets = true;
			}
			ImGui::ColorEdit3( "Clear background", app.clearColor.data() );
			if( ImGui::Button( "Randomize vertex colors" ) )
			{
				RandomizeVertexColors( app.colorPhase );
			}
			ImGui::SameLine();
			if( ImGui::Button( "Reset persistent targets" ) )
			{
				app.recreateViewportTargets = true;
			}

			ImGui::Separator();

			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const float availableWidth = ImGui::GetContentRegionAvail().x;
			const float panelWidth = std::max( 220.0f, ( availableWidth - 2.0f * spacing ) / 3.0f );
			const ImVec2 previewSize( panelWidth, panelWidth );

			for( size_t index = 0; index < app.viewports.size(); ++index )
			{
				const LoadOpViewport& viewport = app.viewports[ index ];
				ImGui::BeginGroup();
				ImGui::TextUnformatted( viewport.label );
				ImGui::TextWrapped( viewport.description );
				ImGui::Separator();
				const D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = app.imguiRenderer->GetTextureGpuDescriptor( viewport.targets.color );
				ImGui::Image( static_cast<ImTextureID>( textureHandle.ptr ), previewSize );
				ImGui::Text( "Configured LoadOp: %s", viewport.label );
				ImGui::EndGroup();
				if( index + 1 < app.viewports.size() )
				{
					ImGui::SameLine();
				}
			}

			ImGui::End();

			const CubePushConstants sceneConstants = BuildSceneConstants( app, 1.0f );
			auto& buffer = ctx.AcquireCommandBuffer();
			for( LoadOpViewport& viewport : app.viewports )
			{
				RenderViewportScene( app, buffer, viewport, sceneConstants );
			}

			const TextureHandle backbuffer = ctx.GetCurrentSwapchainTexture();
			RenderPass uiPass{};
			uiPass.color[ 0 ].loadOp = LoadOp::Clear;
			uiPass.color[ 0 ].clearColor = { 0.05f, 0.06f, 0.09f, 1.0f };

			Framebuffer uiFramebuffer{};
			uiFramebuffer.color[ 0 ].texture = backbuffer;

			buffer.CmdBeginRendering( uiPass, uiFramebuffer );
			app.imguiRenderer->Render( buffer );
			buffer.CmdEndRendering();
			ctx.Submit( buffer, backbuffer );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		if( app.deviceManager )
		{
			app.deviceManager->WaitIdle();
			RenderDevice& renderDevice = *app.deviceManager->GetRenderDevice();
			for( LoadOpViewport& viewport : app.viewports )
			{
				DestroyViewportTargets( renderDevice, viewport.targets );
			}
		}

		app.cubePipeline = {};
		app.cubeWireframePipeline = {};
		app.imguiRenderer.reset();
		app.deviceManager.reset();
		if( IsWindow( hwnd ) != FALSE )
		{
			DestroyWindow( hwnd );
		}
		UnregisterClassW( windowClass.lpszClassName, instance );
		return 0;
	}
	catch( const std::exception& )
	{
		MessageBoxA( nullptr, "LightD3D12 HelloLoadOps failed.", "LightD3D12", MB_ICONERROR | MB_OK );
		return 1;
	}
}
