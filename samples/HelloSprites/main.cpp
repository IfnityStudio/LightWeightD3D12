#include "LightD3D12/LightD3D12.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <wincodec.h>

using namespace lightd3d12;

namespace
{
	struct LoadedImage
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> pixels;
	};

	struct SpriteDesc
	{
		float x = 0.0f;
		float y = 0.0f;
		float size = 0.0f;
	};

	struct SpritePushConstants
	{
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;
		float screenWidth = 0.0f;
		float screenHeight = 0.0f;
		uint32_t textureIndex = 0;
		uint32_t padding_ = 0;
	};

	static_assert( sizeof( SpritePushConstants ) % sizeof( uint32_t ) == 0 );

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		RenderPipelineState spritePipeline;
		TextureHandle enemyTexture = {};
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
			"Failed to open image file." );

		ComPtr<IWICBitmapFrameDecode> frame;
		ThrowIfFailed( decoder->GetFrame( 0, frame.GetAddressOf() ), "Failed to decode image frame." );

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

	RenderPipelineState CreateSpritePipeline( RenderDevice& ctx, DXGI_FORMAT format )
	{
		static constexpr char ourVertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    float4 gSpriteRect;
    float2 gScreenSize;
    uint gTextureIndex;
    uint gPadding;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    VSOutput output;

    const float2 quadPositions[6] =
    {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(1.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(0.0, 1.0)
    };

    const float2 quadUvs[6] =
    {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(1.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(0.0, 1.0)
    };

    const float2 pixelPosition = gSpriteRect.xy + quadPositions[vertexID] * gSpriteRect.zw;
    const float2 clipPosition = float2(
        (pixelPosition.x / gScreenSize.x) * 2.0 - 1.0,
        1.0 - (pixelPosition.y / gScreenSize.y) * 2.0);

    output.position = float4(clipPosition, 0.0, 1.0);
    output.uv = quadUvs[vertexID];
    return output;
}
)";

		static constexpr char ourPixelShader[] = R"(
cbuffer PushConstants : register(b0)
{
    float4 gSpriteRect;
    float2 gScreenSize;
    uint gTextureIndex;
    uint gPadding;
};

SamplerState gSampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> spriteTexture = ResourceDescriptorHeap[gTextureIndex];
    const float4 color = spriteTexture.Sample(gSampler, uv);
    clip(color.a - 0.05); //discard pixel lower alpha.
    return color;
}
)";

		RenderPipelineDesc desc{};
		desc.vertexShader.source = ourVertexShader;
		desc.vertexShader.entryPoint = "main";
		desc.vertexShader.profile = "vs_6_6";
		desc.fragmentShader.source = ourPixelShader;
		desc.fragmentShader.entryPoint = "main";
		desc.fragmentShader.profile = "ps_6_6";
		desc.color[ 0 ].format = format;
		desc.depthFormat = DXGI_FORMAT_UNKNOWN;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.blendState.RenderTarget[ 0 ].BlendEnable = TRUE;
		desc.blendState.RenderTarget[ 0 ].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		desc.blendState.RenderTarget[ 0 ].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		desc.blendState.RenderTarget[ 0 ].BlendOp = D3D12_BLEND_OP_ADD;
		desc.blendState.RenderTarget[ 0 ].SrcBlendAlpha = D3D12_BLEND_ONE;
		desc.blendState.RenderTarget[ 0 ].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		desc.blendState.RenderTarget[ 0 ].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		return ctx.CreateRenderPipeline( desc );
	}

	LRESULT CALLBACK WindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		auto* app = reinterpret_cast<AppState*>( GetWindowLongPtr( hwnd, GWLP_USERDATA ) );

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
	HRESULT comResult = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
	const bool shouldUninitializeCom = comResult == S_OK || comResult == S_FALSE;
	if( FAILED( comResult ) && comResult != RPC_E_CHANGED_MODE )
	{
		return 1;
	}

	try
	{
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = L"LightD3D12HelloSpritesWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t ourInitialWidth = 1280;
		constexpr uint32_t ourInitialHeight = 720;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Sprites",
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
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		app.spritePipeline = CreateSpritePipeline( ctx, contextDesc.swapchainFormat );

		const std::filesystem::path enemyPath = std::filesystem::path( __FILE__ ).parent_path() / "enemy.png";
		const LoadedImage enemyImage = LoadImageRgba8( enemyPath );

		TextureDesc textureDesc{};
		textureDesc.debugName = "Enemy Sprite";
		textureDesc.width = enemyImage.width;
		textureDesc.height = enemyImage.height;
		textureDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.createShaderResourceView = true;
		textureDesc.data = enemyImage.pixels.data();
		textureDesc.rowPitch = enemyImage.width * 4u;
		textureDesc.slicePitch = static_cast<uint32_t>( enemyImage.pixels.size() );
		app.enemyTexture = ctx.CreateTexture( textureDesc );

		const std::array<SpriteDesc, 6> sprites = {
			SpriteDesc{ 48.0f, 48.0f, 128.0f },
			SpriteDesc{ 220.0f, 70.0f, 64.0f },
			SpriteDesc{ 320.0f, 80.0f, 10.0f },
			SpriteDesc{ 390.0f, 140.0f, 24.0f },
			SpriteDesc{ 480.0f, 180.0f, 48.0f },
			SpriteDesc{ 620.0f, 110.0f, 160.0f },
		};

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

			auto& buffer = renderDevice->AcquireCommandBuffer();
			const TextureHandle backbuffer = renderDevice->GetCurrentSwapchainTexture();

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.08f, 0.10f, 0.14f, 1.0f };

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = backbuffer;

			buffer.CmdBeginRendering( renderPass, framebuffer );
			buffer.CmdBindRenderPipeline( app.spritePipeline );
			buffer.CmdPushDebugGroupLabel( "Render Sprites", 0xff00b8ff );

			for( const SpriteDesc& sprite : sprites )
			{
				SpritePushConstants constants{};
				constants.x = sprite.x;
				constants.y = sprite.y;
				constants.width = sprite.size;
				constants.height = sprite.size;
				constants.screenWidth = static_cast<float>( app.deviceManager->GetWidth() );
				constants.screenHeight = static_cast<float>( app.deviceManager->GetHeight() );
				constants.textureIndex = renderDevice->GetBindlessIndex( app.enemyTexture );
				buffer.CmdPushConstants( &constants, sizeof( constants ) );
				buffer.CmdDraw( 6 );
			}

			buffer.CmdPopDebugGroupLabel();
			buffer.CmdEndRendering();
			renderDevice->Submit( buffer, backbuffer );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		if( app.deviceManager )
		{
			app.deviceManager->WaitIdle();
			if( app.enemyTexture.Valid() )
			{
				app.deviceManager->GetRenderDevice()->Destroy( app.enemyTexture );
				app.enemyTexture = {};
			}
		}
		app.spritePipeline = {};
		app.deviceManager.reset();

		if( IsWindow( hwnd ) != FALSE )
		{
			DestroyWindow( hwnd );
		}

		UnregisterClassW( windowClass.lpszClassName, instance );
		if( shouldUninitializeCom )
		{
			CoUninitialize();
		}
		return 0;
	}
	catch( const std::exception& )
	{
		if( shouldUninitializeCom )
		{
			CoUninitialize();
		}
		MessageBoxA( nullptr, "LightD3D12 HelloSprites failed.", "LightD3D12", MB_ICONERROR | MB_OK );
		return 1;
	}
}
