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
	struct BakedCubeVertex
	{
		XMFLOAT3 position = {};
		XMFLOAT3 normal = {};
		XMFLOAT2 uv = {};
		XMFLOAT2 lightUv = {};
	};

	enum LightType : uint32_t
	{
		LightType_Directional = 0,
		LightType_Point = 1,
		LightType_Spot = 2,
	};

	struct BakedCubePushConstants
	{
		XMFLOAT4X4 worldViewProjection = {};
		XMFLOAT4X4 world = {};
		uint32_t albedoTextureIndex = 0;
		uint32_t lightmapTextureIndex = 0;
		uint32_t showLightmapOnly = 0;
		uint32_t showUvDebug = 0;
		uint32_t useBakedLight = 1;
		uint32_t splitCompare = 1;
		float splitPosition = 0.5f;
		float screenWidth = 1.0f;
		XMFLOAT3 keyLightDirection = { -0.5f, 0.7f, -0.5f };
		float keyLightIntensity = 1.0f;
		XMFLOAT3 keyLightColor = { 1.0f, 0.86f, 0.55f };
		float ambientIntensity = 0.08f;
		uint32_t lightType = LightType_Directional;
		float lightRange = 4.0f;
		float spotInnerCos = 0.94f;
		float spotOuterCos = 0.82f;
		XMFLOAT3 lightPosition = { 0.0f, 1.8f, -1.8f };
		float lightPadding = 0.0f;
	};

	static_assert( sizeof( BakedCubePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct BakeLightPushConstants
	{
		XMFLOAT4X4 world = {};
		XMFLOAT3 keyLightDirection = { -0.35f, 0.85f, -0.40f };
		float keyLightIntensity = 0.72f;
		XMFLOAT3 keyLightColor = { 1.00f, 0.82f, 0.45f };
		float ambientIntensity = 0.035f;
		uint32_t lightType = LightType_Directional;
		float lightRange = 4.0f;
		float spotInnerCos = 0.94f;
		float spotOuterCos = 0.82f;
		XMFLOAT3 lightPosition = { 0.0f, 1.8f, -1.8f };
		float lightPadding = 0.0f;
	};

	static_assert( sizeof( BakeLightPushConstants ) / sizeof( uint32_t ) <= 63 );

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
		RenderPipelineState bakePipeline = {};
		RenderPipelineState renderPipeline = {};
		MeshGeometry cube = {};
		DepthTarget depth = {};
		TextureHandle albedoTexture = {};
		TextureHandle lightmapTexture = {};
		std::string albedoPath;
		uint32_t albedoWidth = 0;
		uint32_t albedoHeight = 0;
		uint32_t lightmapWidth = 0;
		uint32_t lightmapHeight = 0;
		bool bakeRequested = false;
		bool hasCookedBake = false;
		bool applyBakedLight = false;
		bool splitCompare = false;
		bool showLightmapOnly = false;
		bool showUvDebug = false;
		float splitPosition = 0.5f;
		float lightYaw = 225.0f;
		float lightPitch = 55.0f;
		float keyLightIntensity = 1.0f;
		float ambientIntensity = 0.08f;
		XMFLOAT3 keyLightColor = { 1.0f, 0.86f, 0.55f };
		int lightType = LightType_Directional;
		XMFLOAT3 lightPosition = { 0.0f, 1.8f, -1.8f };
		float lightRange = 4.0f;
		float spotInnerAngle = 20.0f;
		float spotOuterAngle = 35.0f;
		bool pauseCubeRotation = false;
		bool rotateX = false;
		bool rotateY = true;
		bool rotateZ = false;
		float rotationSpeedX = 25.0f;
		float rotationSpeedY = 42.0f;
		float rotationSpeedZ = 15.0f;
		float rotationX = 0.0f;
		float rotationY = 0.0f;
		float rotationZ = 0.0f;
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

	LoadedImage LoadImageRgba8( const std::filesystem::path& path )
	{
		ComPtr<IWICImagingFactory> factory;
		ThrowIfFailed(
			CoCreateInstance( CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( factory.GetAddressOf() ) ),
			"Failed to create WIC imaging factory." );

		ComPtr<IWICBitmapDecoder> decoder;
		ThrowIfFailed(
			factory->CreateDecoderFromFilename( path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf() ),
			"Failed to open image." );

		ComPtr<IWICBitmapFrameDecode> frame;
		ThrowIfFailed( decoder->GetFrame( 0, frame.GetAddressOf() ), "Failed to decode image." );

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
			"Failed to convert image to RGBA8." );

		LoadedImage image{};
		ThrowIfFailed( converter->GetSize( &image.width, &image.height ), "Failed to query image size." );

		const uint32_t rowPitch = image.width * 4u;
		image.pixels.resize( static_cast<size_t>( rowPitch ) * image.height );
		ThrowIfFailed(
			converter->CopyPixels( nullptr, rowPitch, static_cast<UINT>( image.pixels.size() ), image.pixels.data() ),
			"Failed to copy image pixels." );

		return image;
	}

	TextureHandle CreateTextureFromImage( RenderDevice& ctx, const LoadedImage& image, const char* debugName, bool mipsEnabled )
	{
		uint16_t mipCount = 1;
		if( mipsEnabled )
		{
			uint32_t width = image.width;
			uint32_t height = image.height;
			while( width > 1 || height > 1 )
			{
				width = std::max( 1u, width >> 1u );
				height = std::max( 1u, height >> 1u );
				++mipCount;
			}
		}

		TextureDesc desc{};
		desc.debugName = debugName;
		desc.width = image.width;
		desc.height = image.height;
		desc.countMipMap = mipCount;
		desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.usage = TextureUsage::Sampled;
		desc.data = image.pixels.data();
		desc.rowPitch = image.width * 4u;
		desc.slicePitch = static_cast<uint32_t>( image.pixels.size() );
		return ctx.CreateTexture( desc );
	}

	XMFLOAT2 LightUvForFace( uint32_t tileX, uint32_t tileY, float u, float v )
	{
		constexpr float kColumns = 3.0f;
		constexpr float kRows = 2.0f;
		constexpr float kPaddingTexels = 2.0f;
		constexpr float kFacePixels = 256.0f;
		constexpr float kPad = kPaddingTexels / kFacePixels;

		u = std::lerp( kPad, 1.0f - kPad, u );
		v = std::lerp( kPad, 1.0f - kPad, v );

		return {
			( static_cast<float>( tileX ) + u ) / kColumns,
			( static_cast<float>( tileY ) + v ) / kRows,
		};
	}

	void AddFace(
		std::vector<BakedCubeVertex>& vertices,
		std::vector<uint32_t>& indices,
		const std::array<XMFLOAT3, 4>& positions,
		XMFLOAT3 normal,
		uint32_t tileX,
		uint32_t tileY )
	{
		const uint32_t base = static_cast<uint32_t>( vertices.size() );
		const std::array<XMFLOAT2, 4> uv =
		{
			XMFLOAT2{ 0.0f, 1.0f },
			XMFLOAT2{ 0.0f, 0.0f },
			XMFLOAT2{ 1.0f, 0.0f },
			XMFLOAT2{ 1.0f, 1.0f },
		};

		for( uint32_t i = 0; i != 4; ++i )
		{
			vertices.push_back( BakedCubeVertex{
				.position = positions[ i ],
				.normal = normal,
				.uv = uv[ i ],
				.lightUv = LightUvForFace( tileX, tileY, uv[ i ].x, uv[ i ].y ),
			} );
		}

		indices.insert( indices.end(), { base + 0, base + 1, base + 2, base + 0, base + 2, base + 3 } );
	}

	MeshGeometry CreateBakedCubeGeometry( RenderDevice& ctx )
	{
		std::vector<BakedCubeVertex> vertices;
		std::vector<uint32_t> indices;
		vertices.reserve( 24 );
		indices.reserve( 36 );

		// The lightmap atlas layout generated by LightmapBake is:
		// +X | -X | +Y
		// -Y | +Z | -Z
		AddFace( vertices, indices, { XMFLOAT3{ +1, -1, +1 }, XMFLOAT3{ +1, +1, +1 }, XMFLOAT3{ +1, +1, -1 }, XMFLOAT3{ +1, -1, -1 } }, { +1, 0, 0 }, 0, 0 );
		AddFace( vertices, indices, { XMFLOAT3{ -1, -1, -1 }, XMFLOAT3{ -1, +1, -1 }, XMFLOAT3{ -1, +1, +1 }, XMFLOAT3{ -1, -1, +1 } }, { -1, 0, 0 }, 1, 0 );
		AddFace( vertices, indices, { XMFLOAT3{ -1, +1, +1 }, XMFLOAT3{ -1, +1, -1 }, XMFLOAT3{ +1, +1, -1 }, XMFLOAT3{ +1, +1, +1 } }, { 0, +1, 0 }, 2, 0 );
		AddFace( vertices, indices, { XMFLOAT3{ -1, -1, -1 }, XMFLOAT3{ -1, -1, +1 }, XMFLOAT3{ +1, -1, +1 }, XMFLOAT3{ +1, -1, -1 } }, { 0, -1, 0 }, 0, 1 );
		AddFace( vertices, indices, { XMFLOAT3{ -1, -1, +1 }, XMFLOAT3{ -1, +1, +1 }, XMFLOAT3{ +1, +1, +1 }, XMFLOAT3{ +1, -1, +1 } }, { 0, 0, +1 }, 1, 1 );
		AddFace( vertices, indices, { XMFLOAT3{ +1, -1, -1 }, XMFLOAT3{ +1, +1, -1 }, XMFLOAT3{ -1, +1, -1 }, XMFLOAT3{ -1, -1, -1 } }, { 0, 0, -1 }, 2, 1 );

		BufferDesc vertexDesc{};
		vertexDesc.debugName = "BakedCube Vertex Buffer";
		vertexDesc.size = sizeof( BakedCubeVertex ) * vertices.size();
		vertexDesc.stride = sizeof( BakedCubeVertex );
		vertexDesc.bufferType = BufferDesc::BufferType::VertexBuffer;
		vertexDesc.data = vertices.data();
		vertexDesc.dataSize = vertexDesc.size;

		BufferDesc indexDesc{};
		indexDesc.debugName = "BakedCube Index Buffer";
		indexDesc.size = sizeof( uint32_t ) * indices.size();
		indexDesc.bufferType = BufferDesc::BufferType::IndexBuffer;
		indexDesc.data = indices.data();
		indexDesc.dataSize = indexDesc.size;

		MeshGeometry geometry{};
		geometry.vertexBuffer = ctx.CreateBuffer( vertexDesc );
		geometry.indexBuffer = ctx.CreateBuffer( indexDesc );
		geometry.indexCount = static_cast<uint32_t>( indices.size() );
		return geometry;
	}

	RenderPipelineState CreateBakePipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat )
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
				.semanticName = "NORMAL",
				.semanticIndex = 0,
				.format = DXGI_FORMAT_R32G32B32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = 12,
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			},
			VertexInputElementDesc
			{
				.semanticName = "TEXCOORD",
				.semanticIndex = 0,
				.format = DXGI_FORMAT_R32G32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = 24,
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			},
			VertexInputElementDesc
			{
				.semanticName = "TEXCOORD",
				.semanticIndex = 1,
				.format = DXGI_FORMAT_R32G32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = 32,
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			}
		};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/BakeLightVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( "shaders/BakeLightPS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = DXGI_FORMAT_UNKNOWN;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	RenderPipelineState CreateRenderPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat )
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
				.semanticName = "NORMAL",
				.semanticIndex = 0,
				.format = DXGI_FORMAT_R32G32B32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = 12,
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			},
			VertexInputElementDesc
			{
				.semanticName = "TEXCOORD",
				.semanticIndex = 0,
				.format = DXGI_FORMAT_R32G32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = 24,
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			},
			VertexInputElementDesc
			{
				.semanticName = "TEXCOORD",
				.semanticIndex = 1,
				.format = DXGI_FORMAT_R32G32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = 32,
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			}
		};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/BakedCubeVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( "shaders/BakedCubePS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = depthFormat;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	TextureHandle CreateGpuLightmap( RenderDevice& ctx )
	{
		TextureDesc desc{};
		desc.debugName = "GPU Baked Cube Lightmap";
		desc.width = 768;
		desc.height = 512;
		desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
		desc.useClearValue = true;
		desc.clearValue.Format = desc.format;
		desc.clearValue.Color[ 0 ] = 0.0f;
		desc.clearValue.Color[ 1 ] = 0.0f;
		desc.clearValue.Color[ 2 ] = 0.0f;
		desc.clearValue.Color[ 3 ] = 1.0f;
		return ctx.CreateTexture( desc );
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
		desc.debugName = "BakedCube Depth";
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
				if( app != nullptr )
				{
					app->running = false;
					app->minimized = true;
				}
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
		ThrowIfFailed( CoInitializeEx( nullptr, COINIT_MULTITHREADED ), "Failed to initialize COM." );

		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = L"LightD3D12LightmapBakedCubeWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1280;
		constexpr uint32_t kInitialHeight = 720;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Lightmap Baked Cube",
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
		const std::filesystem::path sampleDir = std::filesystem::path( __FILE__ ).parent_path();
		LightHLSLLoader::SetRootDirectory( sampleDir );

		const std::filesystem::path albedoPath = sampleDir.parent_path() / "data" / "wood_box.png";
		const LoadedImage albedo = LoadImageRgba8( albedoPath );
		app.albedoPath = albedoPath.string();
		app.albedoWidth = albedo.width;
		app.albedoHeight = albedo.height;
		app.lightmapWidth = 768;
		app.lightmapHeight = 512;
		app.albedoTexture = CreateTextureFromImage( ctx, albedo, "BakedCube Wood Albedo", true );
		app.lightmapTexture = CreateGpuLightmap( ctx );
		app.cube = CreateBakedCubeGeometry( ctx );
		app.bakePipeline = CreateBakePipeline( ctx, DXGI_FORMAT_R8G8B8A8_UNORM );
		app.renderPipeline = CreateRenderPipeline( ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT );
		RecreateDepthTarget( app );

		const auto startTime = std::chrono::steady_clock::now();
		auto lastFrameTime = startTime;
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
			float deltaSeconds = std::chrono::duration<float>( now - lastFrameTime ).count();
			lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );
			app.smoothedFrameMs = std::lerp( app.smoothedFrameMs, deltaSeconds * 1000.0f, 0.08f );
			app.smoothedFps = 1000.0f / std::max( app.smoothedFrameMs, 0.001f );
			app.imguiRenderer->NewFrame();
			ImGui::SetNextWindowPos( ImVec2( 18.0f, 18.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowSize( ImVec2( 440.0f, 0.0f ), ImGuiCond_FirstUseEver );
			ImGui::Begin( "Baked Lightmap Cube" );
			ImGui::TextWrapped( "Step 1: adjust Runtime light preview. Step 2: Cook bake light. Step 3: Apply baked light." );
			ImGui::Separator();
			ImGui::Text( "Albedo: %u x %u", app.albedoWidth, app.albedoHeight );
			ImGui::Text( "GPU lightmap RT: %u x %u", app.lightmapWidth, app.lightmapHeight );
			ImGui::TextWrapped( "Albedo file: %s", app.albedoPath.c_str() );
			ImGui::Separator();
			ImGui::TextColored( ImVec4( 0.35f, 0.85f, 1.0f, 1.0f ), app.applyBakedLight ? "Mode: APPLY BAKED LIGHT" : "Mode: RUNTIME LIGHT PREVIEW" );
			if( ImGui::Button( "Cook bake light" ) )
			{
				app.bakeRequested = true;
			}
			ImGui::SameLine();
			ImGui::Text( app.hasCookedBake ? "bake ready" : "no bake cooked yet" );
			ImGui::BeginDisabled( !app.hasCookedBake );
			ImGui::Checkbox( "Apply baked light", &app.applyBakedLight );
			ImGui::EndDisabled();
			const char* lightTypes[] = { "Directional light", "Point light", "Cone / spot light" };
			ImGui::Combo( "Runtime/bake light type", &app.lightType, lightTypes, static_cast<int>( std::size( lightTypes ) ) );
			ImGui::SliderFloat( "Runtime/bake light yaw", &app.lightYaw, 0.0f, 360.0f );
			ImGui::SliderFloat( "Runtime/bake light pitch", &app.lightPitch, -80.0f, 80.0f );
			if( app.lightType == LightType_Point || app.lightType == LightType_Spot )
			{
				ImGui::SliderFloat3( "Runtime/bake light position", &app.lightPosition.x, -3.0f, 3.0f );
				ImGui::SliderFloat( "Runtime/bake light range", &app.lightRange, 0.25f, 8.0f );
			}
			if( app.lightType == LightType_Spot )
			{
				ImGui::SliderFloat( "Cone inner angle", &app.spotInnerAngle, 1.0f, 85.0f );
				ImGui::SliderFloat( "Cone outer angle", &app.spotOuterAngle, app.spotInnerAngle + 1.0f, 89.0f );
			}
			ImGui::ColorEdit3( "Runtime/bake light color", &app.keyLightColor.x );
			ImGui::SliderFloat( "Runtime/bake light intensity", &app.keyLightIntensity, 0.0f, 2.0f );
			ImGui::SliderFloat( "Ambient intensity", &app.ambientIntensity, 0.0f, 0.25f );
			ImGui::Separator();
			ImGui::Checkbox( "Pause cube rotation", &app.pauseCubeRotation );
			ImGui::Checkbox( "Rotate X", &app.rotateX );
			ImGui::SameLine();
			ImGui::Checkbox( "Rotate Y", &app.rotateY );
			ImGui::SameLine();
			ImGui::Checkbox( "Rotate Z", &app.rotateZ );
			ImGui::SliderFloat( "Rotation speed X", &app.rotationSpeedX, -180.0f, 180.0f );
			ImGui::SliderFloat( "Rotation speed Y", &app.rotationSpeedY, -180.0f, 180.0f );
			ImGui::SliderFloat( "Rotation speed Z", &app.rotationSpeedZ, -180.0f, 180.0f );
			ImGui::SliderFloat( "Manual rotation X", &app.rotationX, -180.0f, 180.0f );
			ImGui::SliderFloat( "Manual rotation Y", &app.rotationY, -180.0f, 180.0f );
			ImGui::SliderFloat( "Manual rotation Z", &app.rotationZ, -180.0f, 180.0f );
			ImGui::Separator();
			ImGui::Checkbox( "Split compare: left raw / right baked", &app.splitCompare );
			ImGui::BeginDisabled( !app.splitCompare );
			ImGui::SliderFloat( "Split position", &app.splitPosition, 0.05f, 0.95f );
			ImGui::EndDisabled();
			ImGui::Checkbox( "Show lightmap only", &app.showLightmapOnly );
			ImGui::Checkbox( "Show light UV debug", &app.showUvDebug );
			ImGui::TextWrapped( "Cook bake light flattens the cube into UV2 and writes the current light into the GPU lightmap." );
			ImGui::TextWrapped( "After Apply baked light is enabled, changing the sliders will NOT change the cube until you cook a new bake." );
			ImGui::TextWrapped( app.splitCompare
									 ? "Split mode: left side is runtime light preview, right side is the last cooked bake."
									 : "Runtime preview uses the current sliders. Apply baked light uses the last cooked texture." );
			ImGui::Text( "Frame: %.3f ms", app.smoothedFrameMs );
			ImGui::Text( "FPS: %.1f", app.smoothedFps );
			ImGui::End();

			const uint32_t width = app.deviceManager->GetWidth();
			const uint32_t height = app.deviceManager->GetHeight();
			const float aspect = static_cast<float>( width ) / static_cast<float>( std::max( 1u, height ) );
			if( !app.pauseCubeRotation )
			{
				if( app.rotateX )
				{
					app.rotationX = std::fmod( app.rotationX + app.rotationSpeedX * deltaSeconds + 360.0f, 360.0f );
				}
				if( app.rotateY )
				{
					app.rotationY = std::fmod( app.rotationY + app.rotationSpeedY * deltaSeconds + 360.0f, 360.0f );
				}
				if( app.rotateZ )
				{
					app.rotationZ = std::fmod( app.rotationZ + app.rotationSpeedZ * deltaSeconds + 360.0f, 360.0f );
				}
			}
			const XMMATRIX world = XMMatrixRotationRollPitchYaw(
				XMConvertToRadians( app.rotationX ),
				XMConvertToRadians( app.rotationY ),
				XMConvertToRadians( app.rotationZ ) );
			const XMMATRIX view = XMMatrixLookAtLH( XMVectorSet( 0.0f, 0.4f, -5.2f, 1.0f ), XMVectorZero(), XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f ) );
			const XMMATRIX projection = XMMatrixPerspectiveFovLH( XMConvertToRadians( 50.0f ), aspect, 0.1f, 100.0f );

			BakedCubePushConstants pushConstants{};
			XMStoreFloat4x4( &pushConstants.worldViewProjection, world * view * projection );
			XMStoreFloat4x4( &pushConstants.world, world );
			pushConstants.albedoTextureIndex = renderDevice->GetBindlessIndex( app.albedoTexture );
			pushConstants.lightmapTextureIndex = renderDevice->GetBindlessIndex( app.lightmapTexture );
			pushConstants.showLightmapOnly = app.showLightmapOnly ? 1u : 0u;
			pushConstants.showUvDebug = app.showUvDebug ? 1u : 0u;
			pushConstants.useBakedLight = ( app.applyBakedLight && app.hasCookedBake ) ? 1u : 0u;
			pushConstants.splitCompare = app.splitCompare ? 1u : 0u;
			pushConstants.splitPosition = app.splitPosition;
			pushConstants.screenWidth = static_cast<float>( std::max( 1u, width ) );

			const float yawRadians = XMConvertToRadians( app.lightYaw );
			const float pitchRadians = XMConvertToRadians( app.lightPitch );
			app.spotOuterAngle = std::max( app.spotOuterAngle, app.spotInnerAngle + 1.0f );
			BakeLightPushConstants bakePushConstants{};
			XMStoreFloat4x4( &bakePushConstants.world, world );
			bakePushConstants.keyLightDirection = {
				std::cos( pitchRadians ) * std::cos( yawRadians ),
				std::sin( pitchRadians ),
				std::cos( pitchRadians ) * std::sin( yawRadians ),
			};
			bakePushConstants.keyLightIntensity = app.keyLightIntensity;
			bakePushConstants.keyLightColor = app.keyLightColor;
			bakePushConstants.ambientIntensity = app.ambientIntensity;
			bakePushConstants.lightType = static_cast<uint32_t>( std::clamp( app.lightType, 0, 2 ) );
			bakePushConstants.lightRange = app.lightRange;
			bakePushConstants.spotInnerCos = std::cos( XMConvertToRadians( app.spotInnerAngle ) );
			bakePushConstants.spotOuterCos = std::cos( XMConvertToRadians( app.spotOuterAngle ) );
			bakePushConstants.lightPosition = app.lightPosition;
			pushConstants.keyLightDirection = bakePushConstants.keyLightDirection;
			pushConstants.keyLightIntensity = bakePushConstants.keyLightIntensity;
			pushConstants.keyLightColor = bakePushConstants.keyLightColor;
			pushConstants.ambientIntensity = bakePushConstants.ambientIntensity;
			pushConstants.lightType = bakePushConstants.lightType;
			pushConstants.lightRange = bakePushConstants.lightRange;
			pushConstants.spotInnerCos = bakePushConstants.spotInnerCos;
			pushConstants.spotOuterCos = bakePushConstants.spotOuterCos;
			pushConstants.lightPosition = bakePushConstants.lightPosition;

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.045f, 0.05f, 0.065f, 1.0f };
			renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
			renderPass.depthStencil.depthStoreOp = StoreOp::Store;
			renderPass.depthStencil.clearDepth = 1.0f;

			const TextureHandle currentTexture = renderDevice->GetCurrentSwapchainTexture();

			ICommandBuffer& commandBuffer = renderDevice->AcquireCommandBuffer();
			if( app.bakeRequested )
			{
				RenderPass bakePass{};
				bakePass.color[ 0 ].loadOp = LoadOp::Clear;
				bakePass.color[ 0 ].clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };

				Framebuffer bakeFramebuffer{};
				bakeFramebuffer.color[ 0 ].texture = app.lightmapTexture;

				commandBuffer.CmdBeginRendering( bakePass, bakeFramebuffer );
				commandBuffer.CmdPushDebugGroupLabel( "GPU Bake Lightmap", 0xffffc857 );
				commandBuffer.CmdBindRenderPipeline( app.bakePipeline );
				commandBuffer.CmdBindVertexBuffer( app.cube.vertexBuffer, sizeof( BakedCubeVertex ) );
				commandBuffer.CmdBindIndexBuffer( app.cube.indexBuffer, DXGI_FORMAT_R32_UINT );
				commandBuffer.CmdPushConstants( &bakePushConstants, sizeof( bakePushConstants ) );
				commandBuffer.CmdDrawIndexed( app.cube.indexCount );
				commandBuffer.CmdPopDebugGroupLabel();
				commandBuffer.CmdEndRendering();
				commandBuffer.CmdTransitionTexture( app.lightmapTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
				app.bakeRequested = false;
				app.hasCookedBake = true;
			}

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;
			framebuffer.depthStencil.texture = app.depth.texture;

			commandBuffer.CmdBeginRendering( renderPass, framebuffer );
			commandBuffer.CmdPushDebugGroupLabel( "Baked Lightmap Cube", 0xffd8a531 );
			commandBuffer.CmdBindRenderPipeline( app.renderPipeline );
			commandBuffer.CmdBindVertexBuffer( app.cube.vertexBuffer, sizeof( BakedCubeVertex ) );
			commandBuffer.CmdBindIndexBuffer( app.cube.indexBuffer, DXGI_FORMAT_R32_UINT );
			commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			commandBuffer.CmdDrawIndexed( app.cube.indexCount );
			commandBuffer.CmdPopDebugGroupLabel();
			app.imguiRenderer->Render( commandBuffer );
			commandBuffer.CmdEndRendering();
			renderDevice->Submit( commandBuffer, currentTexture );
		}

		ctx.WaitIdle();
		if( app.albedoTexture.Valid() )
		{
			ctx.Destroy( app.albedoTexture );
		}
		if( app.lightmapTexture.Valid() )
		{
			ctx.Destroy( app.lightmapTexture );
		}
		if( app.cube.vertexBuffer.Valid() )
		{
			ctx.Destroy( app.cube.vertexBuffer );
		}
		if( app.cube.indexBuffer.Valid() )
		{
			ctx.Destroy( app.cube.indexBuffer );
		}
		DestroyDepthTarget( ctx, app.depth );
		app.imguiRenderer.reset();
		app.deviceManager.reset();

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		DestroyWindow( hwnd );
		CoUninitialize();
		return 0;
	}
	catch( const std::exception& exception )
	{
		MessageBoxA( nullptr, exception.what(), "LightD3D12 LightmapBakedCube Error", MB_ICONERROR | MB_OK );
		return -1;
	}
}
