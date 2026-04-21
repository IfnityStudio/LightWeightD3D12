#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <chrono>
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
	};

	static_assert( sizeof( CubePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct DepthTarget
	{
		TextureHandle texture = {};
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState cubePipeline;
		RenderPipelineState cubeWireframePipeline;
		DepthTarget depthTarget;
		float colorPhase = 0.0f;
		bool running = true;
		bool minimized = false;
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
    float3 position = positions[cubeVertexIndex];

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

    rotatedZ.z += 5.5;

    const float perspectiveScale = 1.25 / rotatedZ.z;
    const float2 clipXY = float2(rotatedZ.x * perspectiveScale / gAspectRatio, rotatedZ.y * perspectiveScale);
    const float clipZ = saturate((rotatedZ.z - 3.0) / 8.0);

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
    return float4(0.03, 0.04, 0.06, 1.0);
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

	void DestroyDepthTarget( RenderDevice& ctx, DepthTarget& depthTarget )
	{
		if( depthTarget.texture.Valid() )
		{
			ctx.Destroy( depthTarget.texture );
			depthTarget.texture = {};
		}

		depthTarget.width = 0;
		depthTarget.height = 0;
	}

	void RecreateDepthTarget( AppState& app )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const uint32_t width = app.deviceManager->GetWidth();
		const uint32_t height = app.deviceManager->GetHeight();
		if( app.depthTarget.texture.Valid() && app.depthTarget.width == width && app.depthTarget.height == height )
		{
			return;
		}

		DestroyDepthTarget( ctx, app.depthTarget );

		TextureDesc depthDesc{};
		depthDesc.debugName = "HelloCube Depth";
		depthDesc.width = width;
		depthDesc.height = height;
		depthDesc.format = DXGI_FORMAT_D32_FLOAT;
		depthDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		depthDesc.createShaderResourceView = false;
		depthDesc.createDepthStencilView = true;
		depthDesc.useClearValue = true;
		depthDesc.clearValue.Format = depthDesc.format;
		depthDesc.clearValue.DepthStencil.Depth = 1.0f;
		depthDesc.clearValue.DepthStencil.Stencil = 0;
		app.depthTarget.texture = ctx.CreateTexture( depthDesc );
		app.depthTarget.width = width;
		app.depthTarget.height = height;
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
		windowClass.lpszClassName = L"LightD3D12HelloCubeWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1280;
		constexpr uint32_t kInitialHeight = 720;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Cube",
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
		app.cubePipeline = CreateCubePipeline( *app.deviceManager->GetRenderDevice(), contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, false );
		app.cubeWireframePipeline = CreateCubePipeline( *app.deviceManager->GetRenderDevice(), contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, true );
		RecreateDepthTarget( app );

		const auto startTime = std::chrono::steady_clock::now();
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

			RenderDevice* ctx = app.deviceManager ? app.deviceManager->GetRenderDevice() : nullptr;
			if( !app.running || app.minimized || ctx == nullptr )
			{
				continue;
			}

			RecreateDepthTarget( app );

			const float elapsedSeconds = std::chrono::duration<float>( std::chrono::steady_clock::now() - startTime ).count();

			CubePushConstants pushConstants{};
			pushConstants.rotationX = elapsedSeconds * 0.85f;
			pushConstants.rotationY = elapsedSeconds * 1.10f;
			pushConstants.rotationZ = elapsedSeconds * 0.55f;
			pushConstants.aspectRatio = static_cast<float>( app.deviceManager->GetWidth() ) / static_cast<float>( app.deviceManager->GetHeight() );
			pushConstants.colorPhase = app.colorPhase;

			auto& buffer = ctx->AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx->GetCurrentSwapchainTexture();

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.06f, 0.08f, 0.12f, 1.0f };
			renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
			renderPass.depthStencil.depthStoreOp = StoreOp::Store;
			renderPass.depthStencil.clearDepth = 1.0f;

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;
			framebuffer.depthStencil.texture = app.depthTarget.texture;

			RenderPass imguiRenderPass{};
			imguiRenderPass.color[ 0 ].loadOp = LoadOp::Load;

			Framebuffer imguiFramebuffer{};
			imguiFramebuffer.color[ 0 ].texture = currentTexture;

			if( app.imguiRenderer )
			{
				app.imguiRenderer->NewFrame();
				const float frameRate = ImGui::GetIO().Framerate;
				const float frameTimeMs = frameRate > 0.0f ? 1000.0f / frameRate : 0.0f;

				ImGui::SetNextWindowPos( ImVec2( 16.0f, 16.0f ), ImGuiCond_Once );
				ImGui::SetNextWindowSize( ImVec2( 360.0f, 0.0f ), ImGuiCond_Once );
				ImGui::Begin( "Hello Cube" );
				ImGui::Text( "Diagonal rotating cube" );
				ImGui::Separator();
				ImGui::Text( "Frame time: %.3f ms", frameTimeMs );
				ImGui::Text( "FPS: %.1f", frameRate );
				ImGui::Text( "Elapsed: %.2f s", elapsedSeconds );
				ImGui::Text( "Resolution: %u x %u", app.deviceManager->GetWidth(), app.deviceManager->GetHeight() );
				if( ImGui::Button( "Randomize Vertex Colors" ) )
				{
					RandomizeVertexColors( app.colorPhase );
				}
				ImGui::End();
			}

			buffer.CmdBeginRendering( renderPass, framebuffer );
			buffer.CmdBindRenderPipeline( app.cubePipeline );
			buffer.CmdPushDebugGroupLabel( "Render Cube", 0xff4cc9f0 );
			buffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			buffer.CmdDraw( 36 );
			buffer.CmdPopDebugGroupLabel();
			buffer.CmdBindRenderPipeline( app.cubeWireframePipeline );
			buffer.CmdPushDebugGroupLabel( "Render Cube Wireframe", 0xff101010 );
			buffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			buffer.CmdDraw( 36 );
			buffer.CmdPopDebugGroupLabel();
			buffer.CmdEndRendering();

			if( app.imguiRenderer )
			{
				buffer.CmdBeginRendering( imguiRenderPass, imguiFramebuffer );
				app.imguiRenderer->Render( buffer );
				buffer.CmdEndRendering();
			}

			ctx->Submit( buffer, currentTexture );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		if( app.deviceManager )
		{
			app.deviceManager->WaitIdle();
			if( app.depthTarget.texture.Valid() )
			{
				app.deviceManager->GetRenderDevice()->Destroy( app.depthTarget.texture );
				app.depthTarget.texture = {};
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
		MessageBoxA( nullptr, "LightD3D12 HelloCube failed.", "LightD3D12", MB_ICONERROR | MB_OK );
		return 1;
	}
}
