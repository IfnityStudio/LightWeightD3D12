#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"
#include "LightD3D12/LightHLSLLoader.hpp"

#include <imgui.h>

#include <DirectXMath.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace DirectX;
using namespace lightd3d12;

namespace
{
	struct PlaneVertex
	{
		XMFLOAT3 position = {};
		XMFLOAT2 uv = {};
	};

	struct PlanePushConstants
	{
		XMFLOAT4X4 worldViewProjection = {};
		XMFLOAT4 uvScaleAndOffset = { 1.0f, 1.0f, 0.0f, 0.0f };
		uint32_t textureIndex = 0;
		uint32_t useForcedMip = 0;
		float forcedMipLevel = 0.0f;
		uint32_t mipCount = 1;
		uint32_t showAutoMipDebug = 0;
		XMFLOAT3 padding = {};
	};

	static_assert( sizeof( PlanePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct LoadedImage
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> pixels;
	};

	struct MeshGeometry
	{
		BufferHandle vertexBuffer = {};
		BufferHandle indexBuffer = {};
		uint32_t indexCount = 0;
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState planePipeline = {};
		MeshGeometry plane = {};
		TextureHandle texture = {};
		std::string textureSource;
		bool usingFallbackTexture = false;
		uint32_t textureWidth = 0;
		uint32_t textureHeight = 0;
		uint32_t mipCount = 1;
		float textureAspect = 1.0f;
		float cameraDistance = 2.4f;
		bool forceMip = false;
		bool showAutoMipDebug = false;
		float forcedMipLevel = 0.0f;
		float smoothedFrameMs = 16.6f;
		float smoothedFps = 60.0f;
		bool running = true;
		bool minimized = false;
	};

	void ThrowIfFailed( HRESULT result, const char* message )
	{
		if( FAILED( result ) )
		{
			throw std::runtime_error( message );
		}
	}

	uint32_t CalculateMipCount( uint32_t width, uint32_t height )
	{
		uint32_t levels = 1;
		while( width > 1 || height > 1 )
		{
			width = std::max( 1u, width >> 1u );
			height = std::max( 1u, height >> 1u );
			++levels;
		}

		return levels;
	}

	float FallbackNoise( float x, float y )
	{
		const float waves = std::sin( x * 9.0f + std::sin( y * 5.0f ) * 1.5f );
		const float veins = std::sin( ( x + y ) * 23.0f + waves * 3.0f );
		return std::clamp( 0.5f + waves * 0.35f + veins * 0.15f, 0.0f, 1.0f );
	}

	LoadedImage CreateFallbackTexture( uint32_t width, uint32_t height )
	{
		LoadedImage image{};
		image.width = width;
		image.height = height;
		image.pixels.resize( static_cast<size_t>( width ) * height * 4u );

		for( uint32_t y = 0; y < height; ++y )
		{
			for( uint32_t x = 0; x < width; ++x )
			{
				const float u = static_cast<float>( x ) / static_cast<float>( std::max( 1u, width - 1u ) );
				const float v = static_cast<float>( y ) / static_cast<float>( std::max( 1u, height - 1u ) );
				const float n = FallbackNoise( u * 2.0f, v * 3.2f );
				const float checker = ( ( x / 64u + y / 64u ) & 1u ) ? 1.0f : 0.0f;

				const uint8_t r = static_cast<uint8_t>( std::clamp( 72.0f + n * 54.0f + checker * 14.0f, 0.0f, 255.0f ) );
				const uint8_t g = static_cast<uint8_t>( std::clamp( 150.0f + n * 52.0f + checker * 18.0f, 0.0f, 255.0f ) );
				const uint8_t b = static_cast<uint8_t>( std::clamp( 195.0f + n * 45.0f + checker * 12.0f, 0.0f, 255.0f ) );

				uint8_t* pixel = image.pixels.data() + ( static_cast<size_t>( y ) * width + x ) * 4u;
				pixel[ 0 ] = r;
				pixel[ 1 ] = g;
				pixel[ 2 ] = b;
				pixel[ 3 ] = 255;
			}
		}

		return image;
	}

	LoadedImage LoadImageRgba8( const std::filesystem::path& path )
	{
		ComPtr<IWICImagingFactory> factory;
		ThrowIfFailed(
			CoCreateInstance( CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( factory.GetAddressOf() ) ),
			"Failed to create WIC imaging factory." );

		ComPtr<IWICBitmapDecoder> decoder;
		ThrowIfFailed(
			factory->CreateDecoderFromFilename( path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf() ),
			"Failed to open texture image." );

		ComPtr<IWICBitmapFrameDecode> frame;
		ThrowIfFailed( decoder->GetFrame( 0, frame.GetAddressOf() ), "Failed to decode texture image." );

		ComPtr<IWICFormatConverter> converter;
		ThrowIfFailed( factory->CreateFormatConverter( converter.GetAddressOf() ), "Failed to create WIC format converter." );
		ThrowIfFailed(
			converter->Initialize(
				frame.Get(),
				GUID_WICPixelFormat32bppRGBA,
				WICBitmapDitherTypeNone,
				nullptr,
				0.0,
				WICBitmapPaletteTypeCustom ),
			"Failed to convert texture image to RGBA8." );

		LoadedImage image{};
		ThrowIfFailed( converter->GetSize( &image.width, &image.height ), "Failed to query texture image size." );

		const uint32_t rowPitch = image.width * 4u;
		image.pixels.resize( static_cast<size_t>( rowPitch ) * image.height );
		ThrowIfFailed(
			converter->CopyPixels( nullptr, rowPitch, static_cast<UINT>( image.pixels.size() ), image.pixels.data() ),
			"Failed to copy texture image pixels." );

		return image;
	}

	TextureHandle CreatePlaneTexture( RenderDevice& ctx, AppState& app )
	{
		const std::filesystem::path texturePath = std::filesystem::path( __FILE__ ).parent_path() / "texture.png";
		const bool hasTextureFile = std::filesystem::exists( texturePath );
		const LoadedImage image = hasTextureFile ? LoadImageRgba8( texturePath ) : CreateFallbackTexture( 1024, 1024 );

		app.textureSource = hasTextureFile ? texturePath.string() : "procedural fallback (drop samples/TexturePlane/texture.png to use your image)";
		app.usingFallbackTexture = !hasTextureFile;
		app.textureWidth = image.width;
		app.textureHeight = image.height;
		app.textureAspect = static_cast<float>( image.width ) / static_cast<float>( std::max( 1u, image.height ) );
		app.mipCount = CalculateMipCount( image.width, image.height );

		TextureDesc textureDesc{};
		textureDesc.debugName = "TexturePlane Marble Texture";
		textureDesc.width = image.width;
		textureDesc.height = image.height;
		textureDesc.countMipMap = static_cast<uint16_t>( app.mipCount );
		textureDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.usage = TextureUsage::Sampled;
		textureDesc.data = image.pixels.data();
		textureDesc.rowPitch = image.width * 4u;
		textureDesc.slicePitch = static_cast<uint32_t>( image.pixels.size() );
		return ctx.CreateTexture( textureDesc );
	}

	MeshGeometry CreatePlaneGeometry( RenderDevice& ctx )
	{
		const std::array<PlaneVertex, 4> vertices =
		{
			PlaneVertex{ { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
			PlaneVertex{ { -1.0f,  1.0f, 0.0f }, { 0.0f, 0.0f } },
			PlaneVertex{ {  1.0f,  1.0f, 0.0f }, { 1.0f, 0.0f } },
			PlaneVertex{ {  1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } },
		};

		const std::array<uint32_t, 6> indices = { 0, 1, 2, 0, 2, 3 };

		BufferDesc vertexBufferDesc{};
		vertexBufferDesc.debugName = "TexturePlane Vertex Buffer";
		vertexBufferDesc.size = sizeof( PlaneVertex ) * vertices.size();
		vertexBufferDesc.stride = sizeof( PlaneVertex );
		vertexBufferDesc.bufferType = BufferDesc::BufferType::VertexBuffer;
		vertexBufferDesc.data = vertices.data();
		vertexBufferDesc.dataSize = vertexBufferDesc.size;

		BufferDesc indexBufferDesc{};
		indexBufferDesc.debugName = "TexturePlane Index Buffer";
		indexBufferDesc.size = sizeof( uint32_t ) * indices.size();
		indexBufferDesc.bufferType = BufferDesc::BufferType::IndexBuffer;
		indexBufferDesc.data = indices.data();
		indexBufferDesc.dataSize = indexBufferDesc.size;

		MeshGeometry geometry{};
		geometry.vertexBuffer = ctx.CreateBuffer( vertexBufferDesc );
		geometry.indexBuffer = ctx.CreateBuffer( indexBufferDesc );
		geometry.indexCount = static_cast<uint32_t>( indices.size() );
		return geometry;
	}

	RenderPipelineState CreatePlanePipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat )
	{
		RenderPipelineDesc desc{};
		desc.inputElements =
		{
			VertexInputElementDesc
			{
				.semanticName = "POSITION",
				.semanticIndex = 0,
				.format = DXGI_FORMAT_R32G32B32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = 0,
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			},
			VertexInputElementDesc
			{
				.semanticName = "TEXCOORD",
				.semanticIndex = 0,
				.format = DXGI_FORMAT_R32G32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = 12,
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			}
		};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/TexturePlaneVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( "shaders/TexturePlanePS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = DXGI_FORMAT_UNKNOWN;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
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
		ThrowIfFailed( CoInitializeEx( nullptr, COINIT_MULTITHREADED ), "Failed to initialize COM." );

		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = L"LightD3D12TexturePlaneWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1280;
		constexpr uint32_t kInitialHeight = 720;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Texture Plane + Compute Mips",
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
		app.planePipeline = CreatePlanePipeline( ctx, contextDesc.swapchainFormat );
		app.plane = CreatePlaneGeometry( ctx );
		app.texture = CreatePlaneTexture( ctx, app );

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

			const auto now = std::chrono::steady_clock::now();
			float deltaSeconds = std::chrono::duration<float>( now - lastFrameTime ).count();
			lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );

			const float frameMs = deltaSeconds * 1000.0f;
			app.smoothedFrameMs = std::lerp( app.smoothedFrameMs, frameMs, 0.08f );
			app.smoothedFps = 1000.0f / std::max( app.smoothedFrameMs, 0.001f );

			app.imguiRenderer->NewFrame();
			ImGui::SetNextWindowPos( ImVec2( 18.0f, 18.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowSize( ImVec2( 430.0f, 0.0f ), ImGuiCond_FirstUseEver );
			ImGui::Begin( "Texture Plane" );
			ImGui::TextWrapped( "A simple textured plane inspired by the Vulkan texture sample. LightD3D12 uploads mip 0 and builds the remaining mip chain in a compute shader when countMipMap is greater than 1." );
			ImGui::Separator();
			ImGui::Text( "Texture: %u x %u", app.textureWidth, app.textureHeight );
			ImGui::Text( "Mip levels: %u", app.mipCount );
			ImGui::TextWrapped( "Source: %s", app.textureSource.c_str() );
			if( app.usingFallbackTexture )
			{
				ImGui::TextColored( ImVec4( 1.0f, 0.72f, 0.24f, 1.0f ), "texture.png was not found; this is NOT your high-resolution image." );
			}
			ImGui::Separator();
			ImGui::TextWrapped( "The plane is intentionally static: no texture scrolling and no rotation, so mip changes are easier to compare." );
			ImGui::SliderFloat( "Zoom distance", &app.cameraDistance, 1.15f, 100.0f );
			ImGui::Checkbox( "Force mip level", &app.forceMip );
			ImGui::BeginDisabled( !app.forceMip );
			ImGui::SliderFloat( "Manual mip level", &app.forcedMipLevel, 0.0f, static_cast<float>( std::max( 1u, app.mipCount ) - 1u ) );
			ImGui::EndDisabled();
			ImGui::TextWrapped( "Manual mip level only affects SampleLevel when Force mip level is enabled. With Force disabled, Sample chooses a per-pixel mip automatically." );
			ImGui::Checkbox( "Show auto mip debug", &app.showAutoMipDebug );
			ImGui::TextWrapped( "Debug colors: blue = detailed mip 0, green/yellow = middle mips, red = low-resolution mips." );
			ImGui::Separator();
			ImGui::Text( "Frame: %.3f ms", app.smoothedFrameMs );
			ImGui::Text( "FPS: %.1f", app.smoothedFps );
			ImGui::End();

			const uint32_t width = app.deviceManager->GetWidth();
			const uint32_t height = app.deviceManager->GetHeight();
			const float aspect = static_cast<float>( width ) / static_cast<float>( std::max( 1u, height ) );

			const XMMATRIX world = XMMatrixScaling( app.textureAspect, 1.0f, 1.0f );
			const XMMATRIX view = XMMatrixLookAtLH( XMVectorSet( 0.0f, 0.0f, -app.cameraDistance, 1.0f ), XMVectorZero(), XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f ) );
			const XMMATRIX projection = XMMatrixPerspectiveFovLH( XMConvertToRadians( 50.0f ), aspect, 0.1f, 100.0f );

			PlanePushConstants pushConstants{};
			XMStoreFloat4x4( &pushConstants.worldViewProjection, world * view * projection );
			pushConstants.uvScaleAndOffset = { 1.0f, 1.0f, 0.0f, 0.0f };
			pushConstants.textureIndex = renderDevice->GetBindlessIndex( app.texture );
			pushConstants.useForcedMip = app.forceMip ? 1u : 0u;
			pushConstants.forcedMipLevel = std::clamp( app.forcedMipLevel, 0.0f, static_cast<float>( std::max( 1u, app.mipCount ) - 1u ) );
			pushConstants.mipCount = app.mipCount;
			pushConstants.showAutoMipDebug = app.showAutoMipDebug ? 1u : 0u;

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.05f, 0.05f, 0.07f, 1.0f };

			const TextureHandle currentTexture = renderDevice->GetCurrentSwapchainTexture();
			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;

			ICommandBuffer& commandBuffer = renderDevice->AcquireCommandBuffer();
			commandBuffer.CmdBeginRendering( renderPass, framebuffer );
			commandBuffer.CmdPushDebugGroupLabel( "Texture Plane", 0xff2aa9ff );
			commandBuffer.CmdBindRenderPipeline( app.planePipeline );
			commandBuffer.CmdBindVertexBuffer( app.plane.vertexBuffer, sizeof( PlaneVertex ) );
			commandBuffer.CmdBindIndexBuffer( app.plane.indexBuffer, DXGI_FORMAT_R32_UINT );
			commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			commandBuffer.CmdDrawIndexed( app.plane.indexCount );
			commandBuffer.CmdPopDebugGroupLabel();
			app.imguiRenderer->Render( commandBuffer );
			commandBuffer.CmdEndRendering();
			renderDevice->Submit( commandBuffer, currentTexture );
		}

		ctx.WaitIdle();
		if( app.texture.Valid() )
		{
			ctx.Destroy( app.texture );
		}
		if( app.plane.vertexBuffer.Valid() )
		{
			ctx.Destroy( app.plane.vertexBuffer );
		}
		if( app.plane.indexBuffer.Valid() )
		{
			ctx.Destroy( app.plane.indexBuffer );
		}
		app.imguiRenderer.reset();
		app.deviceManager.reset();

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		DestroyWindow( hwnd );
		CoUninitialize();
		return 0;
	}
	catch( const std::exception& exception )
	{
		MessageBoxA( nullptr, exception.what(), "LightD3D12 TexturePlane Error", MB_ICONERROR | MB_OK );
		return -1;
	}
}
