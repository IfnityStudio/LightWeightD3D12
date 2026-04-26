#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightHLSLLoader.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>

using namespace lightd3d12;

namespace
{
	struct ScenePushConstants
	{
		float aspectRatio = 1.0f;
		float time = 0.0f;
		float cubeScale = 1.0f;
		float planeScale = 7.0f;
		uint32_t drawMode = 0;
		float padding_[ 3 ] = {};
	};

	static_assert( sizeof( ScenePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct NightVisionPushConstants
	{
		uint32_t visibleTextureIndex = 0;
		uint32_t thermalTextureIndex = 0;
		uint32_t outputTextureIndex = 0;
		uint32_t textureWidth = 1;
		uint32_t textureHeight = 1;
		uint32_t sensorMode = 0;
		uint32_t compareSplit = 0;
		uint32_t circularMask = 1;
		float ambientLight = 0.28f;
		float level = 0.0f;
		float gain = 1.2f;
		float stefanBoltzmannConstant = 1.35f;
		float emissivityBlendFactor = 0.45f;
		float noiseStrength = 0.14f;
		float bloomStrength = 0.65f;
		float blurStrength = 0.55f;
		float maskStrength = 0.95f;
		float patternStrength = 0.18f;
		float time = 0.0f;
	};

	static_assert( sizeof( NightVisionPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct PresentPushConstants
	{
		uint32_t sourceTextureIndex = 0;
	};

	static_assert( sizeof( PresentPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct OffscreenTargets
	{
		TextureHandle visibleColor = {};
		TextureHandle thermalData = {};
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
		ComputePipelineState nightVisionPipeline;
		RenderPipelineState presentPipeline;
		OffscreenTargets targets;
		bool running = true;
		bool minimized = false;
		bool pauseAnimation = false;
		bool compareSplit = false;
		bool circularMask = true;
		int sensorModeIndex = 0;
		float animationTime = 0.0f;
		float rotationSpeed = 1.0f;
		float renderScale = 0.62f;
		float cubeScale = 0.85f;
		float planeScale = 7.5f;
		float ambientLight = 0.28f;
		float level = 0.0f;
		float gain = 1.2f;
		float stefanBoltzmannConstant = 1.35f;
		float emissivityBlendFactor = 0.45f;
		float noiseStrength = 0.14f;
		float bloomStrength = 0.65f;
		float blurStrength = 0.55f;
		float maskStrength = 0.95f;
		float patternStrength = 0.18f;
		float smoothedFrameMs = 16.6f;
		float smoothedFps = 60.0f;
	};

	constexpr uint32_t kCubeVertexCount = 36;
	constexpr uint32_t kPlaneVertexCount = 6;
	constexpr uint32_t kCubeInstanceCount = 9;
	constexpr std::array<const char*, 2> kSensorModeNames = { "Night Vision", "Infrared" };

	RenderPipelineState CreateScenePipeline( RenderDevice& ctx, DXGI_FORMAT visibleColorFormat, DXGI_FORMAT thermalDataFormat, DXGI_FORMAT depthFormat )
	{
		RenderPipelineDesc desc{};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/HelloNightVisionSceneVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( "shaders/HelloNightVisionScenePS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = visibleColorFormat;
		desc.color[ 1 ].format = thermalDataFormat;
		desc.depthFormat = depthFormat;
		desc.rasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	ComputePipelineState CreateNightVisionPipeline( RenderDevice& ctx )
	{
		ComputePipelineDesc desc{};
		desc.computeShader = LightHLSLLoader::LoadStage( "shaders/HelloNightVisionSensorCS.hlsl", "cs_6_6" );
		return ctx.CreateComputePipeline( desc );
	}

	RenderPipelineState CreatePresentPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat )
	{
		RenderPipelineDesc desc{};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/HelloNightVisionPresentVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( "shaders/HelloNightVisionPresentPS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = DXGI_FORMAT_UNKNOWN;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	void DestroyTargets( RenderDevice& ctx, OffscreenTargets& targets )
	{
		if( targets.visibleColor.Valid() )
		{
			ctx.Destroy( targets.visibleColor );
			targets.visibleColor = {};
		}

		if( targets.thermalData.Valid() )
		{
			ctx.Destroy( targets.thermalData );
			targets.thermalData = {};
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
		const uint32_t targetWidth = std::max<uint32_t>( 96u, static_cast<uint32_t>( std::lround( app.deviceManager->GetWidth() * app.renderScale ) ) );
		const uint32_t targetHeight = std::max<uint32_t>( 96u, static_cast<uint32_t>( std::lround( app.deviceManager->GetHeight() * app.renderScale ) ) );

		if( app.targets.visibleColor.Valid() && app.targets.thermalData.Valid() && app.targets.sceneDepth.Valid() && app.targets.postOutput.Valid() &&
			app.targets.width == targetWidth && app.targets.height == targetHeight )
		{
			return;
		}

		ctx.WaitIdle();
		DestroyTargets( ctx, app.targets );

		TextureDesc colorDesc{};
		colorDesc.debugName = "HelloNightVision Visible Color";
		colorDesc.width = targetWidth;
		colorDesc.height = targetHeight;
		colorDesc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		colorDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
		colorDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		colorDesc.useClearValue = true;
		colorDesc.clearValue.Format = colorDesc.format;
		colorDesc.clearValue.Color[ 0 ] = 0.015f;
		colorDesc.clearValue.Color[ 1 ] = 0.02f;
		colorDesc.clearValue.Color[ 2 ] = 0.035f;
		colorDesc.clearValue.Color[ 3 ] = 1.0f;
		app.targets.visibleColor = ctx.CreateTexture( colorDesc );

		TextureDesc thermalDesc{};
		thermalDesc.debugName = "HelloNightVision Thermal Data";
		thermalDesc.width = targetWidth;
		thermalDesc.height = targetHeight;
		thermalDesc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		thermalDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
		thermalDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		thermalDesc.useClearValue = true;
		thermalDesc.clearValue.Format = thermalDesc.format;
		thermalDesc.clearValue.Color[ 0 ] = 0.92f;
		thermalDesc.clearValue.Color[ 1 ] = 0.22f;
		thermalDesc.clearValue.Color[ 2 ] = 0.0f;
		thermalDesc.clearValue.Color[ 3 ] = 0.0f;
		app.targets.thermalData = ctx.CreateTexture( thermalDesc );

		TextureDesc depthDesc{};
		depthDesc.debugName = "HelloNightVision Scene Depth";
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
		outputDesc.debugName = "HelloNightVision Output";
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
		windowClass.lpszClassName = L"LightD3D12HelloNightVisionWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1440;
		constexpr uint32_t kInitialHeight = 860;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Night Vision",
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
		LightHLSLLoader::SetRootDirectory( std::filesystem::path( __FILE__ ).parent_path() );
		app.scenePipeline = CreateScenePipeline( ctx, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT );
		app.nightVisionPipeline = CreateNightVisionPipeline( ctx );
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

			const float frameMs = deltaSeconds * 1000.0f;
			app.smoothedFrameMs = std::lerp( app.smoothedFrameMs, frameMs, 0.09f );
			app.smoothedFps = 1000.0f / std::max( app.smoothedFrameMs, 0.001f );

			app.imguiRenderer->NewFrame();
			ImGui::SetNextWindowPos( ImVec2( 20.0f, 20.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowSize( ImVec2( 430.0f, 0.0f ), ImGuiCond_FirstUseEver );
			ImGui::Begin( "Hello Night Vision" );
			ImGui::TextWrapped( "Scene pass -> visible + thermal offscreen targets -> sensor compute shader -> fullscreen present." );
			ImGui::Separator();
			ImGui::Text( "FPS: %.1f", app.smoothedFps );
			ImGui::Text( "Frame time: %.2f ms", app.smoothedFrameMs );
			ImGui::Combo( "Sensor mode", &app.sensorModeIndex, kSensorModeNames.data(), static_cast<int>( kSensorModeNames.size() ) );
			ImGui::Checkbox( "Compare original / processed", &app.compareSplit );
			ImGui::Checkbox( "Pause animation", &app.pauseAnimation );
			ImGui::Checkbox( "Circular tube mask", &app.circularMask );
			ImGui::SliderFloat( "Render scale", &app.renderScale, 0.35f, 1.0f, "%.2f" );
			ImGui::SliderFloat( "Rotation speed", &app.rotationSpeed, 0.0f, 2.5f, "%.2f" );
			ImGui::SliderFloat( "Cube scale", &app.cubeScale, 0.45f, 1.35f, "%.2f" );
			ImGui::SliderFloat( "Ground scale", &app.planeScale, 4.0f, 12.0f, "%.2f" );
			ImGui::Separator();
			ImGui::SliderFloat( "Ambient light", &app.ambientLight, 0.0f, 1.0f, "%.2f" );
			ImGui::SliderFloat( "Auto gain", &app.gain, 0.6f, 2.6f, "%.2f" );
			if( app.sensorModeIndex == 0 )
			{
				ImGui::SliderFloat( "Noise strength", &app.noiseStrength, 0.0f, 0.45f, "%.2f" );
				ImGui::SliderFloat( "Bloom strength", &app.bloomStrength, 0.0f, 1.6f, "%.2f" );
				ImGui::SliderFloat( "Blur strength", &app.blurStrength, 0.0f, 1.0f, "%.2f" );
				ImGui::SliderFloat( "Mask strength", &app.maskStrength, 0.0f, 1.5f, "%.2f" );
				ImGui::SliderFloat( "Sensor pattern", &app.patternStrength, 0.0f, 0.55f, "%.2f" );
			}
			else
			{
				ImGui::SliderFloat( "Level", &app.level, -2.0f, 2.0f, "%.2f" );
				ImGui::SliderFloat( "Stefan-Boltzmann", &app.stefanBoltzmannConstant, 0.10f, 3.00f, "%.3f" );
				ImGui::SliderFloat( "Emissivity blend", &app.emissivityBlendFactor, 0.0f, 1.0f, "%.2f" );
			}
			ImGui::Separator();
			ImGui::Text( "Internal target: %u x %u", app.targets.width, app.targets.height );
			ImGui::TextWrapped(
				app.sensorModeIndex == 0 ?
				"Night Vision mode based on section 3.3: phosphor green mapping, gain-driven noise, chlorophyll boost, soft bloom, and an optional circular goggle mask." :
				"Infrared mode guided by Listing 3.1: visible luminance drives emissivity blending, thermal material data feeds the Stefan-Boltzmann approximation, and the final signal is mapped as white-hot." );
			ImGui::End();

			auto& commandBuffer = ctx.AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx.GetCurrentSwapchainTexture();

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloNightVision::ScenePass", 0xff4cc9f0u );

				RenderPass scenePass{};
				scenePass.color[ 0 ].loadOp = LoadOp::Clear;
				scenePass.color[ 0 ].clearColor = { 0.015f, 0.02f, 0.035f, 1.0f };
				scenePass.color[ 1 ].loadOp = LoadOp::Clear;
				scenePass.color[ 1 ].clearColor = { 0.92f, 0.22f, 0.0f, 0.0f };
				scenePass.depthStencil.depthLoadOp = LoadOp::Clear;
				scenePass.depthStencil.depthStoreOp = StoreOp::Store;
				scenePass.depthStencil.clearDepth = 1.0f;

				Framebuffer sceneFramebuffer{};
				sceneFramebuffer.color[ 0 ].texture = app.targets.visibleColor;
				sceneFramebuffer.color[ 1 ].texture = app.targets.thermalData;
				sceneFramebuffer.depthStencil.texture = app.targets.sceneDepth;

				ScenePushConstants scenePushConstants{};
				scenePushConstants.aspectRatio = static_cast<float>( app.targets.width ) / static_cast<float>( app.targets.height );
				scenePushConstants.time = app.animationTime;
				scenePushConstants.cubeScale = app.cubeScale;
				scenePushConstants.planeScale = app.planeScale;

				commandBuffer.CmdBeginRendering( scenePass, sceneFramebuffer );
				commandBuffer.CmdBindRenderPipeline( app.scenePipeline );

				scenePushConstants.drawMode = 1;
				commandBuffer.CmdPushConstants( &scenePushConstants, sizeof( scenePushConstants ) );
				commandBuffer.CmdDraw( kPlaneVertexCount );

				scenePushConstants.drawMode = 0;
				commandBuffer.CmdPushConstants( &scenePushConstants, sizeof( scenePushConstants ) );
				commandBuffer.CmdDraw( kCubeVertexCount, kCubeInstanceCount );

				commandBuffer.CmdEndRendering();
				commandBuffer.CmdTransitionTexture( app.targets.visibleColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
				commandBuffer.CmdTransitionTexture( app.targets.thermalData, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			}

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloNightVision::ComputePass", 0xff90be6du );

				NightVisionPushConstants postPushConstants{};
				postPushConstants.visibleTextureIndex = ctx.GetBindlessIndex( app.targets.visibleColor );
				postPushConstants.thermalTextureIndex = ctx.GetBindlessIndex( app.targets.thermalData );
				postPushConstants.outputTextureIndex = ctx.GetUnorderedAccessIndex( app.targets.postOutput );
				postPushConstants.textureWidth = app.targets.width;
				postPushConstants.textureHeight = app.targets.height;
				postPushConstants.sensorMode = static_cast<uint32_t>( std::clamp( app.sensorModeIndex, 0, static_cast<int>( kSensorModeNames.size() - 1 ) ) );
				postPushConstants.compareSplit = app.compareSplit ? 1u : 0u;
				postPushConstants.circularMask = app.circularMask ? 1u : 0u;
				postPushConstants.ambientLight = app.ambientLight;
				postPushConstants.level = app.level;
				postPushConstants.gain = app.gain;
				postPushConstants.stefanBoltzmannConstant = app.stefanBoltzmannConstant;
				postPushConstants.emissivityBlendFactor = app.emissivityBlendFactor;
				postPushConstants.noiseStrength = app.noiseStrength;
				postPushConstants.bloomStrength = app.bloomStrength;
				postPushConstants.blurStrength = app.blurStrength;
				postPushConstants.maskStrength = app.maskStrength;
				postPushConstants.patternStrength = app.patternStrength;
				postPushConstants.time = app.animationTime;

				commandBuffer.CmdTransitionTexture( app.targets.postOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
				commandBuffer.CmdBindComputePipeline( app.nightVisionPipeline );
				commandBuffer.CmdPushConstants( &postPushConstants, sizeof( postPushConstants ) );
				commandBuffer.CmdDispatch( ( app.targets.width + 15u ) / 16u, ( app.targets.height + 15u ) / 16u, 1u );
				commandBuffer.CmdTransitionTexture( app.targets.postOutput, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			}

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloNightVision::PresentPass", 0xfff9c74fu );

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
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloNightVision::ImguiPass", 0xfff9844au );

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
		app.nightVisionPipeline = {};
		app.presentPipeline = {};
		app.imguiRenderer.reset();
		app.deviceManager.reset();
		DestroyWindow( hwnd );
	}
	catch( const std::exception& exception )
	{
		MessageBoxA( nullptr, exception.what(), "LightD3D12 Hello Night Vision", MB_ICONERROR | MB_OK );
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
