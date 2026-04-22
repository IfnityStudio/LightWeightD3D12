#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

using namespace lightd3d12;

namespace
{
	struct ScenePushConstants
	{
		uint32_t instanceBufferIndex = 0;
		float aspectRatio = 1.0f;
		float time = 0.0f;
		float globalRotation = 0.0f;
		float waveAmplitude = 0.0f;
		float viewDistance = 12.0f;
	};

	static_assert( sizeof( ScenePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct alignas( 16 ) InstanceData
	{
		std::array<float, 4> basePositionScale = { 0.0f, 0.0f, 0.0f, 1.0f };
		std::array<float, 4> color = { 1.0f, 1.0f, 1.0f, 1.0f };
		std::array<float, 4> params = { 0.0f, 1.0f, 0.0f, 0.0f };
	};

	static_assert( sizeof( InstanceData ) == 48 );

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
		RenderPipelineState solidPipeline;
		RenderPipelineState wireframePipeline;
		DepthTarget depthTarget;
		BufferHandle instanceBuffer = {};
		uint32_t instanceCount = 0;
		bool instancesDirty = true;
		bool running = true;
		bool minimized = false;
		bool pauseAnimation = false;
		bool showWireframe = true;
		int gridX = 16;
		int gridY = 10;
		float spacing = 2.0f;
		float cubeScale = 0.52f;
		float waveAmplitude = 0.75f;
		float rotationSpeed = 0.85f;
		float simulationTime = 0.0f;
	};

	float Fract( float value )
	{
		return value - std::floor( value );
	}

	std::array<float, 3> HueToRgb( float hue )
	{
		const float r = std::clamp( std::abs( Fract( hue + 1.0f ) * 6.0f - 3.0f ) - 1.0f, 0.0f, 1.0f );
		const float g = std::clamp( std::abs( Fract( hue + 2.0f / 3.0f ) * 6.0f - 3.0f ) - 1.0f, 0.0f, 1.0f );
		const float b = std::clamp( std::abs( Fract( hue + 1.0f / 3.0f ) * 6.0f - 3.0f ) - 1.0f, 0.0f, 1.0f );
		return { r, g, b };
	}

	RenderPipelineState CreateInstancingPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, bool wireframe )
	{
		static constexpr char ourVertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    uint gInstanceBufferIndex;
    float gAspectRatio;
    float gTime;
    float gGlobalRotation;
    float gWaveAmplitude;
    float gViewDistance;
};

struct InstanceData
{
    float4 basePositionScale;
    float4 color;
    float4 params;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
    float3 normal : NORMAL0;
};

float3 RotateX(float3 value, float angle)
{
    const float c = cos(angle);
    const float s = sin(angle);
    return float3(value.x, value.y * c - value.z * s, value.y * s + value.z * c);
}

float3 RotateY(float3 value, float angle)
{
    const float c = cos(angle);
    const float s = sin(angle);
    return float3(value.x * c + value.z * s, value.y, -value.x * s + value.z * c);
}

float3 RotateZ(float3 value, float angle)
{
    const float c = cos(angle);
    const float s = sin(angle);
    return float3(value.x * c - value.y * s, value.x * s + value.y * c, value.z);
}

float3 RotateXYZ(float3 value, float angleX, float angleY, float angleZ)
{
    return RotateZ(RotateY(RotateX(value, angleX), angleY), angleZ);
}

VSOutput main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    // Expanded cube geometry keeps the sample independent from vertex/index buffers.
    static const float3 positions[36] =
    {
        float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0),
        float3(-1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0),

        float3(-1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0), float3( 1.0, -1.0,  1.0),
        float3(-1.0, -1.0,  1.0), float3(-1.0,  1.0,  1.0), float3( 1.0,  1.0,  1.0),

        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0, -1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0, -1.0), float3(-1.0, -1.0, -1.0),

        float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0),
        float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),

        float3( 1.0, -1.0, -1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0),
        float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0,  1.0), float3( 1.0,  1.0, -1.0),

        float3(-1.0, -1.0,  1.0), float3(-1.0, -1.0, -1.0), float3(-1.0,  1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3(-1.0,  1.0, -1.0), float3(-1.0,  1.0,  1.0)
    };

    static const float3 normals[36] =
    {
        float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0),
        float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0),

        float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0),
        float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0),

        float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0),
        float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0),

        float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0),
        float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0),

        float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0),
        float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0),

        float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0),
        float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0)
    };

    // Fetch per-instance data from a bindless StructuredBuffer via SV_InstanceID.
    StructuredBuffer<InstanceData> instanceBuffer = ResourceDescriptorHeap[gInstanceBufferIndex];
    const InstanceData instance = instanceBuffer[instanceID];

    const float scale = instance.basePositionScale.w;
    const float spin = gTime * (0.75 + instance.params.y) + instance.params.x;
    const float3 localPosition = positions[vertexID] * scale;
    const float3 localNormal = normals[vertexID];

    float3 rotatedPosition = RotateXYZ(localPosition, spin * 0.7, spin, spin * 0.45);
    float3 rotatedNormal = RotateXYZ(localNormal, spin * 0.7, spin, spin * 0.45);

    rotatedPosition = RotateY(rotatedPosition, gGlobalRotation);
    rotatedNormal = RotateY(rotatedNormal, gGlobalRotation);

    float3 worldPosition = rotatedPosition + instance.basePositionScale.xyz;
    worldPosition.y += sin(gTime + instance.params.x) * gWaveAmplitude;
    worldPosition.z += gViewDistance;

    const float perspectiveScale = 2.25 / max(worldPosition.z, 0.001);
    const float2 clipXY = float2(worldPosition.x * perspectiveScale / gAspectRatio, worldPosition.y * perspectiveScale);
    const float clipZ = saturate((worldPosition.z - 1.0) / (gViewDistance + 6.0));

    VSOutput output;
    output.position = float4(clipXY, clipZ, 1.0);
    output.color = instance.color.rgb;
    output.normal = normalize(rotatedNormal);
    return output;
}
)";

		static constexpr char ourSolidPixelShader[] = R"(
float4 main(float4 position : SV_Position, float3 color : COLOR0, float3 normal : NORMAL0) : SV_Target0
{
    const float3 lightDirection = normalize(float3(-0.45, 0.85, -0.35));
    const float lambert = saturate(dot(normalize(normal), lightDirection)) * 0.75 + 0.25;
    return float4(color * lambert, 1.0);
}
)";

		static constexpr char ourWireframePixelShader[] = R"(
float4 main() : SV_Target0
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
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_BACK;
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
		depthDesc.debugName = "HelloInstancing Depth";
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

	void DestroyInstanceBuffer( RenderDevice& ctx, AppState& app )
	{
		if( app.instanceBuffer.Valid() )
		{
			ctx.Destroy( app.instanceBuffer );
			app.instanceBuffer = {};
		}

		app.instanceCount = 0;
	}

	void RebuildInstanceBuffer( AppState& app )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		DestroyInstanceBuffer( ctx, app );

		std::vector<InstanceData> instances;
		instances.reserve( static_cast<size_t>( app.gridX * app.gridY ) );

		static std::mt19937 ourRandomGenerator( std::random_device{}() );
		std::uniform_real_distribution<float> phaseDistribution( 0.0f, 6.2831853f );
		std::uniform_real_distribution<float> spinDistribution( 0.35f, 1.25f );
		std::uniform_real_distribution<float> hueJitterDistribution( -0.08f, 0.08f );

		const float halfWidth = static_cast<float>( app.gridX - 1 ) * 0.5f;
		const float halfHeight = static_cast<float>( app.gridY - 1 ) * 0.5f;

		// Build one immutable record per cube instance. The shader will fetch it via SV_InstanceID.
		for( int y = 0; y < app.gridY; ++y )
		{
			for( int x = 0; x < app.gridX; ++x )
			{
				const float centeredX = ( static_cast<float>( x ) - halfWidth ) * app.spacing;
				const float centeredZ = ( static_cast<float>( y ) - halfHeight ) * app.spacing;
				const float hue = Fract( ( static_cast<float>( x ) / std::max( app.gridX, 1 ) ) * 0.62f +
					( static_cast<float>( y ) / std::max( app.gridY, 1 ) ) * 0.28f +
					hueJitterDistribution( ourRandomGenerator ) );
				const auto rgb = HueToRgb( hue );

				InstanceData instance{};
				instance.basePositionScale = { centeredX, 0.0f, centeredZ, app.cubeScale };
				instance.color = { rgb[ 0 ], rgb[ 1 ], rgb[ 2 ], 1.0f };
				instance.params = { phaseDistribution( ourRandomGenerator ), spinDistribution( ourRandomGenerator ), 0.0f, 0.0f };
				instances.push_back( instance );
			}
		}

		BufferDesc instanceBufferDesc{};
		instanceBufferDesc.debugName = "HelloInstancing Instances";
		instanceBufferDesc.size = static_cast<uint64_t>( instances.size() * sizeof( InstanceData ) );
		instanceBufferDesc.stride = sizeof( InstanceData );
		instanceBufferDesc.createShaderResourceView = true;
		instanceBufferDesc.data = instances.data();
		instanceBufferDesc.dataSize = instanceBufferDesc.size;
		app.instanceBuffer = ctx.CreateBuffer( instanceBufferDesc );
		app.instanceCount = static_cast<uint32_t>( instances.size() );
		app.instancesDirty = false;
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
		windowClass.lpszClassName = L"LightD3D12HelloInstancingWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1440;
		constexpr uint32_t kInitialHeight = 900;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Instancing",
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
		app.solidPipeline = CreateInstancingPipeline( *app.deviceManager->GetRenderDevice(), contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, false );
		app.wireframePipeline = CreateInstancingPipeline( *app.deviceManager->GetRenderDevice(), contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, true );
		RecreateDepthTarget( app );
		RebuildInstanceBuffer( app );

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

			RenderDevice* ctx = app.deviceManager ? app.deviceManager->GetRenderDevice() : nullptr;
			if( !app.running || app.minimized || ctx == nullptr )
			{
				continue;
			}

			const auto now = std::chrono::steady_clock::now();
			float deltaSeconds = std::chrono::duration<float>( now - lastFrameTime ).count();
			lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );
			if( !app.pauseAnimation )
			{
				app.simulationTime += deltaSeconds;
			}

			RecreateDepthTarget( app );

			if( app.instancesDirty )
			{
				RebuildInstanceBuffer( app );
			}

			auto& buffer = ctx->AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx->GetCurrentSwapchainTexture();

			ScenePushConstants pushConstants{};
			pushConstants.instanceBufferIndex = ctx->GetBindlessIndex( app.instanceBuffer );
			pushConstants.aspectRatio = static_cast<float>( app.deviceManager->GetWidth() ) / static_cast<float>( app.deviceManager->GetHeight() );
			pushConstants.time = app.simulationTime;
			pushConstants.globalRotation = app.simulationTime * app.rotationSpeed * 0.35f;
			pushConstants.waveAmplitude = app.waveAmplitude;
			pushConstants.viewDistance = std::max( static_cast<float>( app.gridX ), static_cast<float>( app.gridY ) ) * app.spacing * 0.85f + 9.0f;

			if( app.imguiRenderer )
			{
				app.imguiRenderer->NewFrame();

				ImGui::SetNextWindowPos( ImVec2( 18.0f, 18.0f ), ImGuiCond_FirstUseEver );
				ImGui::SetNextWindowSize( ImVec2( 360.0f, 0.0f ), ImGuiCond_FirstUseEver );
				ImGui::Begin( "Hello Instancing" );
				ImGui::TextWrapped( "One draw call renders the full grid. Per-instance data lives in a bindless StructuredBuffer." );
				ImGui::Separator();
				ImGui::Text( "FPS: %.1f", ImGui::GetIO().Framerate );
				ImGui::Text( "Instances: %u", app.instanceCount );
				ImGui::Text( "Draw calls: %d", app.showWireframe ? 2 : 1 );

				int gridX = app.gridX;
				int gridY = app.gridY;
				if( ImGui::SliderInt( "Grid X", &gridX, 1, 48 ) )
				{
					app.gridX = gridX;
					app.instancesDirty = true;
				}
				if( ImGui::SliderInt( "Grid Y", &gridY, 1, 48 ) )
				{
					app.gridY = gridY;
					app.instancesDirty = true;
				}
				if( ImGui::SliderFloat( "Spacing", &app.spacing, 1.1f, 3.5f, "%.2f" ) )
				{
					app.instancesDirty = true;
				}
				if( ImGui::SliderFloat( "Cube Scale", &app.cubeScale, 0.18f, 0.85f, "%.2f" ) )
				{
					app.instancesDirty = true;
				}

				ImGui::Checkbox( "Pause animation", &app.pauseAnimation );
				ImGui::SameLine();
				ImGui::Checkbox( "Wireframe overlay", &app.showWireframe );
				ImGui::SliderFloat( "Wave amplitude", &app.waveAmplitude, 0.0f, 1.8f, "%.2f" );
				ImGui::SliderFloat( "Rotation speed", &app.rotationSpeed, 0.0f, 2.5f, "%.2f" );
				if( ImGui::Button( "Randomize instance colors" ) )
				{
					app.instancesDirty = true;
				}

				ImGui::End();
			}

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.05f, 0.07f, 0.11f, 1.0f };
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

			buffer.CmdBeginRendering( renderPass, framebuffer );
			buffer.CmdBindRenderPipeline( app.solidPipeline );
			buffer.CmdPushDebugGroupLabel( "Render Instanced Cubes", 0xff50c878 );
			buffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			// One instanced draw covers the whole grid.
			buffer.CmdDraw( 36, app.instanceCount );
			buffer.CmdPopDebugGroupLabel();

			if( app.showWireframe )
			{
				buffer.CmdBindRenderPipeline( app.wireframePipeline );
				buffer.CmdPushDebugGroupLabel( "Render Instanced Wireframe", 0xff202020 );
				buffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
				buffer.CmdDraw( 36, app.instanceCount );
				buffer.CmdPopDebugGroupLabel();
			}

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
			RenderDevice* ctx = app.deviceManager->GetRenderDevice();
			app.deviceManager->WaitIdle();
			if( app.depthTarget.texture.Valid() )
			{
				ctx->Destroy( app.depthTarget.texture );
				app.depthTarget.texture = {};
			}
			if( app.instanceBuffer.Valid() )
			{
				ctx->Destroy( app.instanceBuffer );
				app.instanceBuffer = {};
			}
		}
		app.solidPipeline = {};
		app.wireframePipeline = {};
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
		MessageBoxA( nullptr, "LightD3D12 HelloInstancing failed.", "LightD3D12", MB_ICONERROR | MB_OK );
		return 1;
	}
}
