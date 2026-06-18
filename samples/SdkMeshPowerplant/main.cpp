#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <directxtk12/CommonStates.h>
#include <directxtk12/Effects.h>
#include <directxtk12/GraphicsMemory.h>
#include <directxtk12/Model.h>
#include <directxtk12/RenderTargetState.h>
#include <directxtk12/ResourceUploadBatch.h>

#include <DirectXCollision.h>
#include <DirectXMath.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>

using namespace DirectX;
using namespace lightd3d12;

namespace
{
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
		std::unique_ptr<GraphicsMemory> graphicsMemory;
		std::unique_ptr<CommonStates> commonStates;
		std::unique_ptr<Model> model;
		std::unique_ptr<EffectTextureFactory> textureFactory;
		Model::EffectCollection effects;
		DepthTarget depth;
		std::filesystem::path modelPath;
		std::filesystem::path mediaPath;
		XMFLOAT3 cameraPosition = {};
		float cameraYaw = 0.0f;
		float cameraPitch = -8.0f;
		float moveSpeed = 1.0f;
		float modelRadius = 1.0f;
		bool keys[ 256 ] = {};
		bool rightMouseDown = false;
		POINT lastMouse = {};
		bool running = true;
		bool minimized = false;
	};

	std::filesystem::path GetExecutableDirectory()
	{
		std::wstring buffer( MAX_PATH, L'\0' );
		for( ;; )
		{
			const DWORD length = GetModuleFileNameW( nullptr, buffer.data(), static_cast<DWORD>( buffer.size() ) );
			if( length == 0 )
			{
				throw std::runtime_error( "GetModuleFileNameW failed." );
			}
			if( length < buffer.size() )
			{
				buffer.resize( length );
				return std::filesystem::path( buffer ).parent_path();
			}
			buffer.resize( buffer.size() * 2 );
		}
	}

	std::filesystem::path FindRepoRoot()
	{
		std::filesystem::path dir = GetExecutableDirectory();
		for( int i = 0; i != 8; ++i )
		{
			if( std::filesystem::exists( dir / "Media" / "powerplant" / "powerplant.sdkmesh" ) )
			{
				return dir;
			}
			dir = dir.parent_path();
		}
		return std::filesystem::current_path();
	}

	BoundingBox ComputeModelBounds( const Model& model )
	{
		BoundingBox bounds{};
		bool hasBounds = false;
		for( const auto& mesh : model.meshes )
		{
			if( !mesh )
			{
				continue;
			}
			if( hasBounds )
			{
				BoundingBox::CreateMerged( bounds, bounds, mesh->boundingBox );
			}
			else
			{
				bounds = mesh->boundingBox;
				hasBounds = true;
			}
		}
		return bounds;
	}

	void ConfigureEffects( AppState& app )
	{
		for( const auto& effect : app.effects )
		{
			if( auto* lights = dynamic_cast<IEffectLights*>( effect.get() ) )
			{
				lights->EnableDefaultLighting();
				lights->SetAmbientLightColor( XMVectorSet( 0.28f, 0.28f, 0.30f, 1.0f ) );
				lights->SetLightDirection( 0, XMVector3Normalize( XMVectorSet( -0.45f, -0.75f, 0.20f, 0.0f ) ) );
				lights->SetLightDiffuseColor( 0, XMVectorSet( 0.95f, 0.92f, 0.86f, 1.0f ) );
			}
		}
	}

	void LoadPowerplantModel( AppState& app, RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat )
	{
		ID3D12Device* device = ctx.GetNativeDevice();
		ID3D12CommandQueue* queue = ctx.GetNativeCommandQueue();
		if( device == nullptr || queue == nullptr )
		{
			throw std::runtime_error( "DirectXTK requires native D3D12 device and command queue." );
		}

		app.graphicsMemory = std::make_unique<GraphicsMemory>( device );
		app.commonStates = std::make_unique<CommonStates>( device );
		app.model = Model::CreateFromSDKMESH( device, app.modelPath.c_str(), ModelLoader_AllowLargeModels );
		for( auto& material : app.model->materials )
		{
			material.normalTextureIndex = -1;
		}
		for( auto& textureName : app.model->textureNames )
		{
			if( !std::filesystem::exists( app.mediaPath / textureName ) )
			{
				textureName = L"test_ground_b.dds";
			}
		}

		ResourceUploadBatch upload( device );
		upload.Begin();
		app.textureFactory = app.model->LoadTextures( device, upload, app.mediaPath.c_str() );
		app.model->LoadStaticBuffers( device, upload );

		const RenderTargetState rtState( colorFormat, depthFormat );
		const EffectPipelineStateDescription opaquePipeline(
			nullptr,
			CommonStates::Opaque,
			CommonStates::DepthDefault,
			CommonStates::CullNone,
			rtState );
		const EffectPipelineStateDescription alphaPipeline(
			nullptr,
			CommonStates::NonPremultiplied,
			CommonStates::DepthRead,
			CommonStates::CullNone,
			rtState );

		if( app.textureFactory )
		{
			EffectFactory effectFactory( app.textureFactory->Heap(), app.commonStates->Heap() );
			effectFactory.EnableLighting( true );
			effectFactory.EnablePerPixelLighting( true );
			effectFactory.EnableNormalMapEffect( false );
			app.effects = app.model->CreateEffects( effectFactory, opaquePipeline, alphaPipeline );
		}
		else
		{
			EffectFactory effectFactory( device );
			effectFactory.EnableLighting( true );
			effectFactory.EnablePerPixelLighting( true );
			app.effects = app.model->CreateEffects( effectFactory, opaquePipeline, alphaPipeline );
		}

		ConfigureEffects( app );
		upload.End( queue ).wait();

		const BoundingBox bounds = ComputeModelBounds( *app.model );
		const float radius = std::max( 1.0f, std::sqrt( bounds.Extents.x * bounds.Extents.x + bounds.Extents.y * bounds.Extents.y + bounds.Extents.z * bounds.Extents.z ) );
		app.modelRadius = radius;
		app.moveSpeed = radius * 0.75f;
		app.cameraPosition = { bounds.Center.x, bounds.Center.y + radius * 0.35f, bounds.Center.z - radius * 1.65f };
	}

	void DestroyDepthTarget( RenderDevice& ctx, DepthTarget& depth )
	{
		if( depth.texture.Valid() )
		{
			ctx.Destroy( depth.texture );
			depth.texture = {};
		}
		depth.width = 0;
		depth.height = 0;
	}

	void RecreateDepthTarget( AppState& app )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const uint32_t width = app.deviceManager->GetWidth();
		const uint32_t height = app.deviceManager->GetHeight();
		if( app.depth.texture.Valid() && app.depth.width == width && app.depth.height == height )
		{
			return;
		}

		DestroyDepthTarget( ctx, app.depth );

		TextureDesc desc{};
		desc.debugName = "SdkMeshPowerplant Depth";
		desc.width = width;
		desc.height = height;
		desc.format = DXGI_FORMAT_D32_FLOAT;
		desc.usage = TextureUsage::DepthStencil;
		desc.useClearValue = true;
		desc.clearValue.Format = desc.format;
		desc.clearValue.DepthStencil.Depth = 1.0f;
		desc.clearValue.DepthStencil.Stencil = 0;
		app.depth.texture = ctx.CreateTexture( desc );
		app.depth.width = width;
		app.depth.height = height;
	}

	void UpdateCamera( AppState& app, float deltaSeconds )
	{
		const float yaw = XMConvertToRadians( app.cameraYaw );
		const float pitch = XMConvertToRadians( app.cameraPitch );
		const XMMATRIX rotation = XMMatrixRotationRollPitchYaw( pitch, yaw, 0.0f );
		const XMVECTOR forward = XMVector3Normalize( XMVector3TransformNormal( XMVectorSet( 0.0f, 0.0f, 1.0f, 0.0f ), rotation ) );
		const XMVECTOR right = XMVector3Normalize( XMVector3TransformNormal( XMVectorSet( 1.0f, 0.0f, 0.0f, 0.0f ), rotation ) );
		const XMVECTOR up = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );

		XMVECTOR position = XMLoadFloat3( &app.cameraPosition );
		const float speed = app.moveSpeed * deltaSeconds * ( app.keys[ VK_SHIFT ] ? 4.0f : 1.0f );
		if( app.keys[ 'W' ] )
			position += forward * speed;
		if( app.keys[ 'S' ] )
			position -= forward * speed;
		if( app.keys[ 'D' ] )
			position += right * speed;
		if( app.keys[ 'A' ] )
			position -= right * speed;
		if( app.keys[ 'E' ] || app.keys[ VK_SPACE ] )
			position += up * speed;
		if( app.keys[ 'Q' ] || app.keys[ VK_CONTROL ] )
			position -= up * speed;
		XMStoreFloat3( &app.cameraPosition, position );
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
			case WM_KEYDOWN:
				if( app && wParam < 256 )
				{
					app->keys[ wParam ] = true;
					if( wParam == VK_ESCAPE )
					{
						app->running = false;
					}
				}
				return 0;

			case WM_KEYUP:
				if( app && wParam < 256 )
				{
					app->keys[ wParam ] = false;
				}
				return 0;

			case WM_RBUTTONDOWN:
				if( app )
				{
					app->rightMouseDown = true;
					app->lastMouse = { GET_X_LPARAM( lParam ), GET_Y_LPARAM( lParam ) };
					SetCapture( hwnd );
				}
				return 0;

			case WM_RBUTTONUP:
				if( app )
				{
					app->rightMouseDown = false;
					ReleaseCapture();
				}
				return 0;

			case WM_MOUSEMOVE:
				if( app && app->rightMouseDown )
				{
					const POINT mouse = { GET_X_LPARAM( lParam ), GET_Y_LPARAM( lParam ) };
					app->cameraYaw += static_cast<float>( mouse.x - app->lastMouse.x ) * 0.18f;
					app->cameraPitch = std::clamp( app->cameraPitch + static_cast<float>( mouse.y - app->lastMouse.y ) * 0.18f, -85.0f, 85.0f );
					app->lastMouse = mouse;
				}
				return 0;

			case WM_SIZE:
				if( app && app->deviceManager )
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

			case WM_CLOSE:
				if( app )
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
		windowClass.lpszClassName = L"LightD3D12SdkMeshPowerplantWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t initialWidth = 1280;
		constexpr uint32_t initialHeight = 720;
		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 DirectXTK12 SDKMESH Powerplant",
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			static_cast<int>( initialWidth ),
			static_cast<int>( initialHeight ),
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
		const std::filesystem::path repoRoot = FindRepoRoot();
		app.mediaPath = repoRoot / "Media" / "powerplant";
		app.modelPath = app.mediaPath / "powerplant.sdkmesh";
		SetWindowLongPtr( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( &app ) );

		ContextDesc contextDesc{};
		contextDesc.enableDebugLayer = true;
		contextDesc.swapchainBufferCount = 3;

		SwapchainDesc swapchainDesc{};
		swapchainDesc.window = MakeWin32WindowHandle( hwnd );
		swapchainDesc.width = initialWidth;
		swapchainDesc.height = initialHeight;
		swapchainDesc.vsync = true;

		app.deviceManager = &DeviceManager::Initialize( contextDesc, swapchainDesc );
		app.imguiRenderer = std::make_unique<ImguiRenderer>( *app.deviceManager, swapchainDesc.window );
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		LoadPowerplantModel( app, ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT );
		RecreateDepthTarget( app );

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

			RenderDevice* renderDevice = app.deviceManager ? app.deviceManager->GetRenderDevice() : nullptr;
			if( !app.running || app.minimized || renderDevice == nullptr )
			{
				continue;
			}

			RecreateDepthTarget( app );
			const auto now = std::chrono::steady_clock::now();
			const float deltaSeconds = std::clamp( std::chrono::duration<float>( now - lastFrameTime ).count(), 0.0f, 0.05f );
			lastFrameTime = now;
			UpdateCamera( app, deltaSeconds );

			const uint32_t width = app.deviceManager->GetWidth();
			const uint32_t height = app.deviceManager->GetHeight();
			const float aspect = static_cast<float>( width ) / static_cast<float>( std::max( 1u, height ) );
			const float yaw = XMConvertToRadians( app.cameraYaw );
			const float pitch = XMConvertToRadians( app.cameraPitch );
			const XMMATRIX cameraRotation = XMMatrixRotationRollPitchYaw( pitch, yaw, 0.0f );
			const XMVECTOR eye = XMLoadFloat3( &app.cameraPosition );
			const XMVECTOR forward = XMVector3Normalize( XMVector3TransformNormal( XMVectorSet( 0.0f, 0.0f, 1.0f, 0.0f ), cameraRotation ) );
			const XMMATRIX view = XMMatrixLookAtLH( eye, eye + forward, XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f ) );
			const XMMATRIX projection = XMMatrixPerspectiveFovLH( XMConvertToRadians( 60.0f ), aspect, 1.0f, app.modelRadius * 12.0f );
			const XMMATRIX world = XMMatrixIdentity();

			app.imguiRenderer->NewFrame();
			ImGui::SetNextWindowPos( ImVec2( 16.0f, 16.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowSize( ImVec2( 430.0f, 0.0f ), ImGuiCond_FirstUseEver );
			ImGui::Begin( "DirectXTK12 SDKMESH" );
			ImGui::TextWrapped( "Rendering Media/powerplant/powerplant.sdkmesh through DirectXTK12 Model directly." );
			ImGui::Separator();
			ImGui::Text( "WASD move, Q/E up-down, Shift faster" );
			ImGui::Text( "Hold right mouse button to look around" );
			ImGui::Text( "Model meshes: %u", static_cast<uint32_t>( app.model ? app.model->meshes.size() : 0u ) );
			ImGui::Text( "Materials: %u", static_cast<uint32_t>( app.model ? app.model->materials.size() : 0u ) );
			ImGui::Text( "Textures: %u", static_cast<uint32_t>( app.model ? app.model->textureNames.size() : 0u ) );
			ImGui::SliderFloat( "Move speed", &app.moveSpeed, app.modelRadius * 0.05f, app.modelRadius * 4.0f );
			ImGui::Text( "Camera: %.1f %.1f %.1f", app.cameraPosition.x, app.cameraPosition.y, app.cameraPosition.z );
			ImGui::End();

			const TextureHandle currentTexture = renderDevice->GetCurrentSwapchainTexture();
			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.045f, 0.052f, 0.065f, 1.0f };
			renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
			renderPass.depthStencil.depthStoreOp = StoreOp::Store;
			renderPass.depthStencil.clearDepth = 1.0f;

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;
			framebuffer.depthStencil.texture = app.depth.texture;

			ICommandBuffer& commandBuffer = renderDevice->AcquireCommandBuffer();
			commandBuffer.CmdBeginRendering( renderPass, framebuffer );
			commandBuffer.CmdPushDebugGroupLabel( "DirectXTK12 Powerplant", 0xff64d2ff );

			ID3D12GraphicsCommandList* nativeCommandList = commandBuffer.GetNativeGraphicsCommandList();
			if( app.textureFactory )
			{
				ID3D12DescriptorHeap* heaps[] = { app.textureFactory->Heap(), app.commonStates->Heap() };
				nativeCommandList->SetDescriptorHeaps( 2, heaps );
			}
			Model::UpdateEffectMatrices( app.effects, world, view, projection );
			app.model->Draw( nativeCommandList, app.effects.begin() );

			commandBuffer.CmdPopDebugGroupLabel();
			app.imguiRenderer->Render( commandBuffer );
			commandBuffer.CmdEndRendering();
			renderDevice->Submit( commandBuffer, currentTexture );
			app.graphicsMemory->Commit( renderDevice->GetNativeCommandQueue() );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		ctx.WaitIdle();
		DestroyDepthTarget( ctx, app.depth );
		app.effects.clear();
		app.textureFactory.reset();
		app.model.reset();
		app.commonStates.reset();
		app.graphicsMemory.reset();
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
	catch( const std::exception& exception )
	{
		DeviceManager::ShutdownSingleton();
		MessageBoxA( nullptr, exception.what(), "LightD3D12 SDKMESH Powerplant Error", MB_ICONERROR | MB_OK );
		return 1;
	}
}




