#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <memory>
#include <stdexcept>

using namespace lightd3d12;

namespace
{
	struct DeferredTargets
	{
		TextureHandle sceneColor = {};
		TextureHandle position = {};
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState pipelineGeometry;
		RenderPipelineState pipelineFinal;
		DeferredTargets deferredTargets;
		bool showPositionOutput = false;
		bool running = true;
		bool minimized = false;
	};

	LRESULT CALLBACK WindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		auto* app = reinterpret_cast< AppState* >( GetWindowLongPtr( hwnd, GWLP_USERDATA ) );

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

	RenderPipelineState CreateGeometryPipeline( RenderDevice& ctx )
	{
		static constexpr char ourVertexShader[] = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float3 encodedPosition : TEXCOORD0;
    float3 color : COLOR0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    VSOutput output;

    const float2 positions[3] =
    {
        float2( 0.00,  0.55),
        float2( 0.50, -0.45),
        float2(-0.50, -0.45)
    };

    const float3 colors[3] =
    {
        float3(1.0, 0.3, 0.2),
        float3(0.2, 0.9, 0.3),
        float3(0.2, 0.4, 1.0)
    };

    const float2 ndc = positions[vertexID];
    output.position = float4(ndc, 0.0, 1.0);
    output.encodedPosition = float3(ndc * 0.5 + 0.5, 0.5);
    output.color = colors[vertexID];
    return output;
}
)";

		static constexpr char ourPixelShader[] = R"(
struct PSOutput
{
    float4 color : SV_Target0;
    float4 position : SV_Target1;
};

PSOutput main(float4 svPosition : SV_Position, float3 encodedPosition : TEXCOORD0, float3 color : COLOR0)
{
    PSOutput output;
    output.color = float4(color, 1.0);
    output.position = float4(encodedPosition, 1.0);
    return output;
}
)";

		RenderPipelineDesc desc{};
		desc.vertexShader.source = ourVertexShader;
		desc.vertexShader.entryPoint = "main";
		desc.vertexShader.profile = "vs_5_1";
		desc.fragmentShader.source = ourPixelShader;
		desc.fragmentShader.entryPoint = "main";
		desc.fragmentShader.profile = "ps_5_1";
		desc.color[ 0 ].format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.color[ 1 ].format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.depthFormat = DXGI_FORMAT_UNKNOWN;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	RenderPipelineState CreateFinalPipeline( RenderDevice& ctx, DXGI_FORMAT backbufferFormat )
	{
		static constexpr char ourVertexShader[] = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    VSOutput output;

    const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    const float2 uvs[3] =
    {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    output.position = float4(positions[vertexID], 0.0, 1.0);
    output.uv = uvs[vertexID];
    return output;
}
)";

		static constexpr char ourPixelShader[] = R"(
cbuffer PushConstants : register(b0)
{
    uint gTextureIndex;
};

SamplerState gSampler : register(s0);

float4 main(float4 svPosition : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[gTextureIndex];
    return sourceTexture.Sample(gSampler, uv);
}
)";

		RenderPipelineDesc desc{};
		desc.vertexShader.source = ourVertexShader;
		desc.vertexShader.entryPoint = "main";
		desc.vertexShader.profile = "vs_6_6";
		desc.fragmentShader.source = ourPixelShader;
		desc.fragmentShader.entryPoint = "main";
		desc.fragmentShader.profile = "ps_6_6";
		desc.color[ 0 ].format = backbufferFormat;
		desc.depthFormat = DXGI_FORMAT_UNKNOWN;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	void DestroyDeferredTargets( RenderDevice& ctx, DeferredTargets& targets )
	{
		if( targets.sceneColor.Valid() )
		{
			ctx.Destroy( targets.sceneColor );
			targets.sceneColor = {};
		}

		if( targets.position.Valid() )
		{
			ctx.Destroy( targets.position );
			targets.position = {};
		}

		targets.width = 0;
		targets.height = 0;
	}

	void RecreateDeferredTargets( AppState& app )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const uint32_t width = app.deviceManager->GetWidth();
		const uint32_t height = app.deviceManager->GetHeight();

		if( app.deferredTargets.sceneColor.Valid() && app.deferredTargets.position.Valid() &&
			app.deferredTargets.width == width && app.deferredTargets.height == height )
		{
			return;
		}

		DestroyDeferredTargets( ctx, app.deferredTargets );

		TextureDesc colorDesc{};
		colorDesc.debugName = "Deferred Scene Color";
		colorDesc.width = width;
		colorDesc.height = height;
		colorDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		colorDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
		colorDesc.useClearValue = true;
		colorDesc.clearValue.Format = colorDesc.format;
		colorDesc.clearValue.Color[ 0 ] = 0.08f;
		colorDesc.clearValue.Color[ 1 ] = 0.10f;
		colorDesc.clearValue.Color[ 2 ] = 0.14f;
		colorDesc.clearValue.Color[ 3 ] = 1.0f;
		app.deferredTargets.sceneColor = ctx.CreateTexture( colorDesc );

		TextureDesc positionDesc{};
		positionDesc.debugName = "Deferred Position";
		positionDesc.width = width;
		positionDesc.height = height;
		positionDesc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		positionDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
		positionDesc.useClearValue = true;
		positionDesc.clearValue.Format = positionDesc.format;
		positionDesc.clearValue.Color[ 0 ] = 0.0f;
		positionDesc.clearValue.Color[ 1 ] = 0.0f;
		positionDesc.clearValue.Color[ 2 ] = 0.0f;
		positionDesc.clearValue.Color[ 3 ] = 1.0f;
		app.deferredTargets.position = ctx.CreateTexture( positionDesc );

		app.deferredTargets.width = width;
		app.deferredTargets.height = height;
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
		windowClass.lpszClassName = L"LightD3D12HelloDeferredWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t ourInitialWidth = 1400;
		constexpr uint32_t ourInitialHeight = 900;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Deferred",
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			static_cast<int>( ourInitialWidth ),
			static_cast<int>( ourInitialHeight ),
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
		swapchainDesc.width = ourInitialWidth;
		swapchainDesc.height = ourInitialHeight;
		swapchainDesc.vsync = true;

		app.deviceManager = std::make_unique<DeviceManager>( contextDesc, swapchainDesc );
		app.imguiRenderer = std::make_unique<ImguiRenderer>( *app.deviceManager, swapchainDesc.window );
		app.pipelineGeometry = CreateGeometryPipeline( *app.deviceManager->GetRenderDevice() );
		app.pipelineFinal = CreateFinalPipeline( *app.deviceManager->GetRenderDevice(), contextDesc.swapchainFormat );
		RecreateDeferredTargets( app );

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

			RecreateDeferredTargets( app );

			auto& buffer = ctx->AcquireCommandBuffer();
			const TextureHandle backbuffer = ctx->GetCurrentSwapchainTexture();

			RenderPass geometryPass{};
			geometryPass.color[ 0 ].loadOp = LoadOp::Clear;
			geometryPass.color[ 0 ].clearColor = { 0.08f, 0.10f, 0.14f, 1.0f };
			geometryPass.color[ 1 ].loadOp = LoadOp::Clear;
			geometryPass.color[ 1 ].clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };

			Framebuffer geometryFramebuffer{};
			geometryFramebuffer.color[ 0 ].texture = app.deferredTargets.sceneColor;
			geometryFramebuffer.color[ 1 ].texture = app.deferredTargets.position;

			buffer.CmdBeginRendering( geometryPass, geometryFramebuffer );
			buffer.CmdBindRenderPipeline( app.pipelineGeometry );
			buffer.CmdPushDebugGroupLabel( "Geometry Pass", 0xff2d7dd2 );
			buffer.CmdDraw( 3 );
			buffer.CmdPopDebugGroupLabel();
			buffer.CmdEndRendering();

			buffer.CmdTransitionTexture( app.deferredTargets.sceneColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			buffer.CmdTransitionTexture( app.deferredTargets.position, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );

			if( app.imguiRenderer )
			{
				app.imguiRenderer->NewFrame();

				const float frameRate = ImGui::GetIO().Framerate;
				const float frameTimeMs = frameRate > 0.0f ? 1000.0f / frameRate : 0.0f;
				const D3D12_GPU_DESCRIPTOR_HANDLE sceneColorHandle = app.imguiRenderer->GetTextureGpuDescriptor( app.deferredTargets.sceneColor );
				const D3D12_GPU_DESCRIPTOR_HANDLE positionHandle = app.imguiRenderer->GetTextureGpuDescriptor( app.deferredTargets.position );

				ImGui::SetNextWindowPos( ImVec2( 16.0f, 16.0f ), ImGuiCond_Once );
				ImGui::SetNextWindowSize( ImVec2( 430.0f, 720.0f ), ImGuiCond_Once );
				ImGui::Begin( "Deferred Preview" );
				ImGui::Text( "Simple forward-generated MRT" );
				ImGui::Separator();
				ImGui::Text( "Final output: %s", app.showPositionOutput ? "Position" : "Scene Color" );
				if( ImGui::RadioButton( "Show Scene Color", !app.showPositionOutput ) )
				{
					app.showPositionOutput = false;
				}
				if( ImGui::RadioButton( "Show Position", app.showPositionOutput ) )
				{
					app.showPositionOutput = true;
				}
				ImGui::Separator();
				ImGui::Text( "Frame time: %.3f ms", frameTimeMs );
				ImGui::Text( "FPS: %.1f", frameRate );
				ImGui::Text( "Resolution: %u x %u", app.deferredTargets.width, app.deferredTargets.height );
				ImGui::Spacing();
				ImGui::Text( "Scene Color" );
				ImGui::Image( static_cast<ImTextureID>( sceneColorHandle.ptr ), ImVec2( 384.0f, 216.0f ) );
				ImGui::Spacing();
				ImGui::Text( "Position Buffer" );
				ImGui::Image( static_cast<ImTextureID>( positionHandle.ptr ), ImVec2( 384.0f, 216.0f ) );
				ImGui::End();
			}

			RenderPass finalPass{};
			finalPass.color[ 0 ].loadOp = LoadOp::Clear;
			finalPass.color[ 0 ].clearColor = { 0.04f, 0.05f, 0.07f, 1.0f };

			Framebuffer finalFramebuffer{};
			finalFramebuffer.color[ 0 ].texture = backbuffer;
			const uint32_t finalTextureIndex = ctx->GetBindlessIndex(
				app.showPositionOutput ? app.deferredTargets.position : app.deferredTargets.sceneColor );

			buffer.CmdBeginRendering( finalPass, finalFramebuffer );
			buffer.CmdBindRenderPipeline( app.pipelineFinal );
			buffer.CmdPushDebugGroupLabel( "Final Pass", 0xff00aa88 );
			buffer.CmdPushConstants( &finalTextureIndex, sizeof( finalTextureIndex ) );
			buffer.CmdDraw( 3 );
			buffer.CmdPopDebugGroupLabel();
			if( app.imguiRenderer )
			{
				app.imguiRenderer->Render( buffer );
			}
			buffer.CmdEndRendering();

			ctx->Submit( buffer, backbuffer );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		if( app.deviceManager )
		{
			app.deviceManager->WaitIdle();
		}
		app.imguiRenderer.reset();
		app.pipelineFinal = {};
		app.pipelineGeometry = {};
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
		MessageBoxA( nullptr, "LightD3D12 HelloDeferred failed.", "LightD3D12", MB_ICONERROR | MB_OK );
		return 1;
	}
}
