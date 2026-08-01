#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"
#include "LightD3D12/LightHLSLLoader.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace lightd3d12;

namespace
{
	constexpr uint32_t kMaxCubeColors = 32;
	constexpr uint32_t kMaxCubeCount = 256;
	constexpr uint32_t kRingFrameCount = 3;

	struct ScenePushConstants
	{
		uint32_t cubeCount = 0;
		uint32_t matrixBaseIndex = 0;
		uint32_t colorFrameIndex = 0;
		float aspectRatio = 1.0f;
		float viewDistance = 12.0f;
	};

	static_assert( sizeof( ScenePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct alignas( 16 ) MatrixRows
	{
		std::array<float, 4> row0 = { 1.0f, 0.0f, 0.0f, 0.0f };
		std::array<float, 4> row1 = { 0.0f, 1.0f, 0.0f, 0.0f };
		std::array<float, 4> row2 = { 0.0f, 0.0f, 1.0f, 0.0f };
		std::array<float, 4> row3 = { 0.0f, 0.0f, 0.0f, 1.0f };
	};

	static_assert( sizeof( MatrixRows ) == 64 );

	struct alignas( 16 ) CubeColorFrame
	{
		std::array<std::array<float, 4>, kMaxCubeColors> colors = {};
		uint32_t colorCount = 1;
		std::array<uint32_t, 3> padding = {};
	};

	static_assert( sizeof( CubeColorFrame ) == ( kMaxCubeColors + 1u ) * 16u );

	struct alignas( 16 ) CubeColorConstants
	{
		std::array<CubeColorFrame, kRingFrameCount> frames = {};
	};

	static_assert( sizeof( CubeColorConstants ) <= 64u * 1024u );

	struct Vec3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	struct DepthTarget
	{
		TextureHandle texture = {};
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct AppState
	{
		DeviceManager* deviceManager = nullptr;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState pipeline;
		DepthTarget depthTarget;
		BufferHandle matrixBuffer = {};
		BufferHandle colorBuffer = {};
		bool running = true;
		bool minimized = false;
		bool pauseAnimation = false;
		int cubeCount = 36;
		int colorCount = 8;
		float spacing = 2.0f;
		float cubeScale = 0.42f;
		float rotationSpeed = 1.0f;
		float simulationTime = 0.0f;
		uint32_t frameIndex = 0;
		uint32_t ringFrameIndex = 0;
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

	Vec3 RotateX( Vec3 value, float angle )
	{
		const float c = std::cos( angle );
		const float s = std::sin( angle );
		return { value.x, value.y * c - value.z * s, value.y * s + value.z * c };
	}

	Vec3 RotateY( Vec3 value, float angle )
	{
		const float c = std::cos( angle );
		const float s = std::sin( angle );
		return { value.x * c + value.z * s, value.y, -value.x * s + value.z * c };
	}

	Vec3 RotateZ( Vec3 value, float angle )
	{
		const float c = std::cos( angle );
		const float s = std::sin( angle );
		return { value.x * c - value.y * s, value.x * s + value.y * c, value.z };
	}

	Vec3 RotateXYZ( Vec3 value, float angleX, float angleY, float angleZ )
	{
		return RotateZ( RotateY( RotateX( value, angleX ), angleY ), angleZ );
	}

	MatrixRows BuildCubeMatrix( uint32_t cubeIndex, uint32_t cubeCount, const AppState& app )
	{
		const int columns = std::max( 1, static_cast<int>( std::ceil( std::sqrt( static_cast<float>( cubeCount ) ) ) ) );
		const int rows = std::max( 1, ( static_cast<int>( cubeCount ) + columns - 1 ) / columns );
		const int x = static_cast<int>( cubeIndex ) % columns;
		const int z = static_cast<int>( cubeIndex ) / columns;
		const float centeredX = ( static_cast<float>( x ) - static_cast<float>( columns - 1 ) * 0.5f ) * app.spacing;
		const float centeredZ = ( static_cast<float>( z ) - static_cast<float>( rows - 1 ) * 0.5f ) * app.spacing;
		const float waveY = std::sin( app.simulationTime * 1.25f + static_cast<float>( cubeIndex ) * 0.41f ) * 0.45f;

		const float baseAngle = app.simulationTime * app.rotationSpeed + static_cast<float>( cubeIndex ) * 0.19f;
		const Vec3 basisX = RotateXYZ( { app.cubeScale, 0.0f, 0.0f }, baseAngle * 0.6f, baseAngle, baseAngle * 0.35f );
		const Vec3 basisY = RotateXYZ( { 0.0f, app.cubeScale, 0.0f }, baseAngle * 0.6f, baseAngle, baseAngle * 0.35f );
		const Vec3 basisZ = RotateXYZ( { 0.0f, 0.0f, app.cubeScale }, baseAngle * 0.6f, baseAngle, baseAngle * 0.35f );

		MatrixRows matrix{};
		matrix.row0 = { basisX.x, basisY.x, basisZ.x, centeredX };
		matrix.row1 = { basisX.y, basisY.y, basisZ.y, waveY };
		matrix.row2 = { basisX.z, basisY.z, basisZ.z, centeredZ };
		matrix.row3 = { 0.0f, 0.0f, 0.0f, 1.0f };
		return matrix;
	}

	RenderPipelineState CreateSrvCbvCubesPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat )
	{
		RenderPipelineDesc desc{};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/SrvCbvCubes.hlsl", "vs_6_6", "VSMain" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( "shaders/SrvCbvCubes.hlsl", "ps_6_6", "PSMain" );
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = depthFormat;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
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
		depthDesc.debugName = "SRV CBV Cubes Depth";
		depthDesc.width = width;
		depthDesc.height = height;
		depthDesc.format = DXGI_FORMAT_D32_FLOAT;
		depthDesc.usage = TextureUsage::DepthStencil;
		depthDesc.useClearValue = true;
		depthDesc.clearValue.Format = depthDesc.format;
		depthDesc.clearValue.DepthStencil.Depth = 1.0f;
		depthDesc.clearValue.DepthStencil.Stencil = 0;
		app.depthTarget.texture = ctx.CreateTexture( depthDesc );
		app.depthTarget.width = width;
		app.depthTarget.height = height;
	}

	void DestroyMatrixBuffer( RenderDevice& ctx, AppState& app )
	{
		if( app.matrixBuffer.Valid() )
		{
			ctx.Destroy( app.matrixBuffer );
			app.matrixBuffer = {};
		}
	}

	void DestroyColorBuffer( RenderDevice& ctx, AppState& app )
	{
		if( app.colorBuffer.Valid() )
		{
			ctx.Destroy( app.colorBuffer );
			app.colorBuffer = {};
		}
	}

	std::vector<MatrixRows> BuildMatrices( const AppState& app, uint32_t cubeCount )
	{
		std::vector<MatrixRows> matrices;
		matrices.reserve( cubeCount );
		for( uint32_t cubeIndex = 0; cubeIndex < cubeCount; ++cubeIndex )
		{
			matrices.push_back( BuildCubeMatrix( cubeIndex, cubeCount, app ) );
		}
		return matrices;
	}

	void UpdateMatrixSrv( AppState& app, uint32_t ringFrameIndex )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();

		const uint32_t cubeCount = static_cast<uint32_t>( std::clamp( app.cubeCount, 1, static_cast<int>( kMaxCubeCount ) ) );
		const std::vector<MatrixRows> matrices = BuildMatrices( app, cubeCount );
		const uint64_t matrixDataSize = static_cast<uint64_t>( matrices.size() * sizeof( MatrixRows ) );
		const uint64_t ringOffset =
			static_cast<uint64_t>( ringFrameIndex ) * kMaxCubeCount * sizeof( MatrixRows );

		if( !app.matrixBuffer.Valid() )
		{
			BufferDesc matrixBufferDesc{};
			matrixBufferDesc.debugName = "SRV CBV Cubes Matrices SRV";
			matrixBufferDesc.size =
				static_cast<uint64_t>( kRingFrameCount ) * kMaxCubeCount * sizeof( MatrixRows );
			matrixBufferDesc.stride = sizeof( MatrixRows );
			app.matrixBuffer = ctx.CreateBuffer( matrixBufferDesc, ShaderResourceSlot::FreeSRV0 );
		}

		ctx.WriteBuffer( app.matrixBuffer, ringOffset, matrices.data(), matrixDataSize );
	}

	CubeColorFrame BuildColorFrame( const AppState& app )
	{
		CubeColorFrame frame{};
		frame.colorCount = static_cast<uint32_t>( std::clamp( app.colorCount, 1, static_cast<int>( kMaxCubeColors ) ) );
		for( uint32_t colorIndex = 0; colorIndex < kMaxCubeColors; ++colorIndex )
		{
			const float hue = Fract( static_cast<float>( colorIndex ) / static_cast<float>( kMaxCubeColors ) + 0.58f );
			const auto rgb = HueToRgb( hue );
			frame.colors[ colorIndex ] = { rgb[ 0 ], rgb[ 1 ], rgb[ 2 ], 1.0f };
		}
		return frame;
	}

	void UpdateColorCbv( AppState& app, uint32_t ringFrameIndex )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const CubeColorFrame frame = BuildColorFrame( app );

		BufferDesc colorBufferDesc{};
		colorBufferDesc.debugName = "SRV CBV Cubes Colors CBV";
		colorBufferDesc.size = sizeof( CubeColorConstants );
		if( !app.colorBuffer.Valid() )
		{
			app.colorBuffer = ctx.CreateBuffer( colorBufferDesc, ConstantBufferSlot::FreeCB0 );
		}

		const uint64_t ringOffset = static_cast<uint64_t>( ringFrameIndex ) * sizeof( CubeColorFrame );
		ctx.WriteBuffer( app.colorBuffer, ringOffset, &frame, sizeof( frame ) );
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
			{
				if( app != nullptr )
				{
					app->running = false;
					app->minimized = true;
				}
				return 0;
			}

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
		windowClass.lpszClassName = L"LightD3D12SrvCbvCubesWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1280;
		constexpr uint32_t kInitialHeight = 800;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 SRV + CBV Cubes",
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

		LightHLSLLoader::SetRootDirectory( std::filesystem::path( __FILE__ ).parent_path() );

		ContextDesc contextDesc{};
		contextDesc.enableDebugLayer = true;
		contextDesc.swapchainBufferCount = 3;

		SwapchainDesc swapchainDesc{};
		swapchainDesc.window = MakeWin32WindowHandle( hwnd );
		swapchainDesc.width = kInitialWidth;
		swapchainDesc.height = kInitialHeight;
		swapchainDesc.vsync = true;

		app.deviceManager = &DeviceManager::Initialize( contextDesc, swapchainDesc );
		app.imguiRenderer = std::make_unique<ImguiRenderer>( *app.deviceManager, swapchainDesc.window );
		app.pipeline = CreateSrvCbvCubesPipeline( *app.deviceManager->GetRenderDevice(), contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT );
		RecreateDepthTarget( app );
		UpdateMatrixSrv( app, app.ringFrameIndex );
		UpdateColorCbv( app, app.ringFrameIndex );

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
			app.ringFrameIndex = app.frameIndex % kRingFrameCount;

			if( app.imguiRenderer )
			{
				app.imguiRenderer->NewFrame();

				ImGui::SetNextWindowPos( ImVec2( 18.0f, 18.0f ), ImGuiCond_FirstUseEver );
				ImGui::SetNextWindowSize( ImVec2( 370.0f, 0.0f ), ImGuiCond_FirstUseEver );
				ImGui::Begin( "SRV + CBV Cubes" );
				ImGui::TextWrapped( "Matrices: StructuredBuffer/SRV. Colors: ConstantBuffer/CBV." );
				ImGui::Separator();
				ImGui::Text( "FPS: %.1f", ImGui::GetIO().Framerate );
				ImGui::Text( "Cubes: %d", app.cubeCount );
				ImGui::Text( "Ring frame: %u / %u", app.ringFrameIndex, kRingFrameCount );
				ImGui::Text( "Matrix SRV slot: %u", ToSlotIndex( ShaderResourceSlot::FreeSRV0 ) );
				ImGui::Text( "Color CBV slot: %u", ToSlotIndex( ConstantBufferSlot::FreeCB0 ) );

				int cubeCount = app.cubeCount;
				if( ImGui::SliderInt( "Cube count", &cubeCount, 1, static_cast<int>( kMaxCubeCount ) ) )
				{
					app.cubeCount = cubeCount;
				}

				int colorCount = app.colorCount;
				if( ImGui::SliderInt( "Active colors", &colorCount, 1, static_cast<int>( kMaxCubeColors ) ) )
				{
					app.colorCount = colorCount;
				}

				if( ImGui::SliderFloat( "Spacing", &app.spacing, 1.2f, 3.2f, "%.2f" ) )
				{
				}
				if( ImGui::SliderFloat( "Cube scale", &app.cubeScale, 0.18f, 0.8f, "%.2f" ) )
				{
				}
				ImGui::SliderFloat( "Rotation speed", &app.rotationSpeed, 0.0f, 3.0f, "%.2f" );
				ImGui::Checkbox( "Pause animation", &app.pauseAnimation );
				ImGui::End();
			}

			UpdateMatrixSrv( app, app.ringFrameIndex );
			UpdateColorCbv( app, app.ringFrameIndex );

			auto& commandBuffer = ctx->AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx->GetCurrentSwapchainTexture();

			ScenePushConstants pushConstants{};
			pushConstants.cubeCount = static_cast<uint32_t>( app.cubeCount );
			pushConstants.matrixBaseIndex = app.ringFrameIndex * kMaxCubeCount;
			pushConstants.colorFrameIndex = app.ringFrameIndex;
			pushConstants.aspectRatio = static_cast<float>( app.deviceManager->GetWidth() ) / static_cast<float>( app.deviceManager->GetHeight() );
			pushConstants.viewDistance = std::max( 14.0f, std::sqrt( static_cast<float>( app.cubeCount ) ) * app.spacing * 1.6f + 8.0f );

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.035f, 0.045f, 0.065f, 1.0f };
			renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
			renderPass.depthStencil.clearDepth = 1.0f;

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;
			framebuffer.depthStencil.texture = app.depthTarget.texture;

			commandBuffer.CmdBeginRendering( renderPass, framebuffer );
			commandBuffer.CmdBindRenderPipeline( app.pipeline );
			commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			commandBuffer.CmdDraw( 36, static_cast<uint32_t>( app.cubeCount ) );
			commandBuffer.CmdEndRendering();

			if( app.imguiRenderer )
			{
				RenderPass imguiRenderPass{};
				imguiRenderPass.color[ 0 ].loadOp = LoadOp::Load;

				Framebuffer imguiFramebuffer{};
				imguiFramebuffer.color[ 0 ].texture = currentTexture;

				commandBuffer.CmdBeginRendering( imguiRenderPass, imguiFramebuffer );
				app.imguiRenderer->Render( commandBuffer );
				commandBuffer.CmdEndRendering();
			}

			ctx->Submit( commandBuffer, currentTexture );
			++app.frameIndex;
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		if( app.deviceManager )
		{
			RenderDevice* ctx = app.deviceManager->GetRenderDevice();
			app.deviceManager->WaitIdle();
			DestroyDepthTarget( *ctx, app.depthTarget );
			DestroyMatrixBuffer( *ctx, app );
			DestroyColorBuffer( *ctx, app );
		}
		app.pipeline = {};
		app.imguiRenderer.reset();
		DeviceManager::ShutdownSingleton();
		app.deviceManager = nullptr;
		if( IsWindow( hwnd ) != FALSE )
		{
			DestroyWindow( hwnd );
		}
		UnregisterClassW( windowClass.lpszClassName, instance );
		return 0;
	}
	catch( const std::exception& error )
	{
		DeviceManager::ShutdownSingleton();
		MessageBoxA( nullptr, error.what(), "LightD3D12 SRV + CBV Cubes failed", MB_ICONERROR | MB_OK );
		return 1;
	}
}
