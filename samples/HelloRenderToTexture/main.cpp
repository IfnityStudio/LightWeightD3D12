#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

#include <wincodec.h>
#include <shellapi.h>

using namespace lightd3d12;

namespace
{
	struct LoadedImage
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> pixels;
	};

	struct ScenePushConstants
	{
		float rotationX = 0.0f;
		float rotationY = 0.0f;
		float rotationZ = 0.0f;
		float aspectRatio = 1.0f;
		float colorPhase = 0.0f;
		float orbitX = 0.0f;
		float orbitY = 0.0f;
		float cubeScale = 1.0f;
		uint32_t textureIndex = 0;
		uint32_t padding_[ 3 ] = {};
	};

	static_assert( sizeof( ScenePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct PresentPushConstants
	{
		uint32_t sourceTextureIndex = 0;
	};

	static_assert( sizeof( PresentPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct OffscreenTargets
	{
		TextureHandle color = {};
		TextureHandle depth = {};
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState scenePipeline;
		RenderPipelineState sceneWireframePipeline;
		RenderPipelineState presentPipeline;
		OffscreenTargets offscreenTargets;
		TextureHandle cubeTexture = {};
		float colorPhase = 0.0f;
		bool running = true;
		bool minimized = false;
		bool pauseAnimation = false;
		bool showWireframe = true;
		bool showPreview = true;
		float renderScale = 0.70f;
		float rotationSpeed = 1.0f;
		float cubeScale = 0.82f;
		float animationTime = 0.0f;
	};

	void RandomizeVertexColors( float& colorPhase )
	{
		static std::mt19937 ourRandomGenerator( std::random_device{}() );
		std::uniform_real_distribution<float> distribution( 0.0f, 1.0f );
		colorPhase = distribution( ourRandomGenerator );
	}

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

	RenderPipelineState CreateScenePipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, bool wireframe )
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
    uint gTextureIndex;
    uint3 gPadding;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
    float2 uv : TEXCOORD0;
};

float3 HueToRgb(float hue)
{
    const float3 k = float3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    const float3 p = abs(frac(hue + k) * 6.0 - 3.0);
    return saturate(p - 1.0);
}

VSOutput main(uint vertexID : SV_VertexID)
{
    struct Vertex
    {
        float3 position;
        float2 uv;
    };

    // Each face carries its own UV layout so the texture is continuous inside the quad.
    static const Vertex vertices[36] =
    {
        { float3(-1.0, -1.0, -1.0), float2(0.0, 1.0) }, { float3( 1.0, -1.0, -1.0), float2(1.0, 1.0) }, { float3( 1.0,  1.0, -1.0), float2(1.0, 0.0) },
        { float3(-1.0, -1.0, -1.0), float2(0.0, 1.0) }, { float3( 1.0,  1.0, -1.0), float2(1.0, 0.0) }, { float3(-1.0,  1.0, -1.0), float2(0.0, 0.0) },

        { float3(-1.0, -1.0,  1.0), float2(0.0, 1.0) }, { float3( 1.0, -1.0,  1.0), float2(1.0, 1.0) }, { float3( 1.0,  1.0,  1.0), float2(1.0, 0.0) },
        { float3(-1.0, -1.0,  1.0), float2(0.0, 1.0) }, { float3( 1.0,  1.0,  1.0), float2(1.0, 0.0) }, { float3(-1.0,  1.0,  1.0), float2(0.0, 0.0) },

        { float3(-1.0, -1.0,  1.0), float2(0.0, 1.0) }, { float3(-1.0, -1.0, -1.0), float2(1.0, 1.0) }, { float3(-1.0,  1.0, -1.0), float2(1.0, 0.0) },
        { float3(-1.0, -1.0,  1.0), float2(0.0, 1.0) }, { float3(-1.0,  1.0, -1.0), float2(1.0, 0.0) }, { float3(-1.0,  1.0,  1.0), float2(0.0, 0.0) },

        { float3( 1.0, -1.0, -1.0), float2(0.0, 1.0) }, { float3( 1.0, -1.0,  1.0), float2(1.0, 1.0) }, { float3( 1.0,  1.0,  1.0), float2(1.0, 0.0) },
        { float3( 1.0, -1.0, -1.0), float2(0.0, 1.0) }, { float3( 1.0,  1.0,  1.0), float2(1.0, 0.0) }, { float3( 1.0,  1.0, -1.0), float2(0.0, 0.0) },

        { float3(-1.0,  1.0, -1.0), float2(0.0, 1.0) }, { float3( 1.0,  1.0, -1.0), float2(1.0, 1.0) }, { float3( 1.0,  1.0,  1.0), float2(1.0, 0.0) },
        { float3(-1.0,  1.0, -1.0), float2(0.0, 1.0) }, { float3( 1.0,  1.0,  1.0), float2(1.0, 0.0) }, { float3(-1.0,  1.0,  1.0), float2(0.0, 0.0) },

        { float3(-1.0, -1.0,  1.0), float2(0.0, 1.0) }, { float3( 1.0, -1.0,  1.0), float2(1.0, 1.0) }, { float3( 1.0, -1.0, -1.0), float2(1.0, 0.0) },
        { float3(-1.0, -1.0,  1.0), float2(0.0, 1.0) }, { float3( 1.0, -1.0, -1.0), float2(1.0, 0.0) }, { float3(-1.0, -1.0, -1.0), float2(0.0, 0.0) }
    };

    VSOutput output;
    const Vertex vertex = vertices[vertexID];
    float3 position = vertex.position * gCubeScale;

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
    rotatedZ.z += 4.7;

    const float perspectiveScale = 1.35 / rotatedZ.z;
    const float2 clipXY = float2(rotatedZ.x * perspectiveScale / gAspectRatio, rotatedZ.y * perspectiveScale);
    const float clipZ = saturate((rotatedZ.z - 2.5) / 6.0);

    output.position = float4(clipXY, clipZ, 1.0);
    const float hue = frac(gColorPhase + (float)vertexID / 36.0);
    output.color = lerp(float3(1.0, 1.0, 1.0), HueToRgb(hue), 0.92);
    output.uv = vertex.uv;
    return output;
}
)";

		static constexpr char ourSolidPixelShader[] = R"(
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
    uint gTextureIndex;
    uint3 gPadding;
};

SamplerState gSampler : register(s0);

float4 main(float4 position : SV_Position, float3 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> cubeTexture = ResourceDescriptorHeap[gTextureIndex];
    const float4 textureSample = cubeTexture.Sample(gSampler, uv);
    // Sprite textures usually carry transparency, so use alpha to avoid projecting the transparent background as black.
    const float3 finalColor = lerp(color, textureSample.rgb, textureSample.a);
    return float4(finalColor, 1.0);
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
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = wireframe ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = wireframe ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_LESS;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	RenderPipelineState CreatePresentPipeline( RenderDevice& ctx, DXGI_FORMAT backbufferFormat )
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
    uint gSourceTextureIndex;
};

SamplerState gSampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[gSourceTextureIndex];
    // Pass 2 simply samples the offscreen render target and presents it to the backbuffer.
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

	TextureHandle CreateCubeTexture( RenderDevice& ctx )
	{
		const std::filesystem::path texturePath =
			std::filesystem::path( __FILE__ ).parent_path().parent_path() / "HelloSprites" / "enemy.png";
		const LoadedImage image = LoadImageRgba8( texturePath );

		TextureDesc textureDesc{};
		textureDesc.debugName = "HelloRenderToTexture Cube Texture";
		textureDesc.width = image.width;
		textureDesc.height = image.height;
		textureDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.usage = TextureUsage::Sampled;
		textureDesc.data = image.pixels.data();
		textureDesc.rowPitch = image.width * 4u;
		textureDesc.slicePitch = textureDesc.rowPitch * image.height;
		return ctx.CreateTexture( textureDesc );
	}

	void DestroyOffscreenTargets( RenderDevice& ctx, OffscreenTargets& targets )
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

		targets.width = 0;
		targets.height = 0;
	}

	void RecreateOffscreenTargets( AppState& app )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const uint32_t targetWidth = std::max<uint32_t>( 64u, static_cast<uint32_t>( std::lround( static_cast<double>( app.deviceManager->GetWidth() ) * app.renderScale ) ) );
		const uint32_t targetHeight = std::max<uint32_t>( 64u, static_cast<uint32_t>( std::lround( static_cast<double>( app.deviceManager->GetHeight() ) * app.renderScale ) ) );

		if( app.offscreenTargets.color.Valid() && app.offscreenTargets.depth.Valid() &&
			app.offscreenTargets.width == targetWidth && app.offscreenTargets.height == targetHeight )
		{
			return;
		}

		DestroyOffscreenTargets( ctx, app.offscreenTargets );

		TextureDesc colorDesc{};
		colorDesc.debugName = "HelloRenderToTexture Offscreen Color";
		colorDesc.width = targetWidth;
		colorDesc.height = targetHeight;
		colorDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		colorDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
		colorDesc.useClearValue = true;
		colorDesc.clearValue.Format = colorDesc.format;
		colorDesc.clearValue.Color[ 0 ] = 0.10f;
		colorDesc.clearValue.Color[ 1 ] = 0.12f;
		colorDesc.clearValue.Color[ 2 ] = 0.17f;
		colorDesc.clearValue.Color[ 3 ] = 1.0f;
		app.offscreenTargets.color = ctx.CreateTexture( colorDesc );

		TextureDesc depthDesc{};
		depthDesc.debugName = "HelloRenderToTexture Offscreen Depth";
		depthDesc.width = targetWidth;
		depthDesc.height = targetHeight;
		depthDesc.format = DXGI_FORMAT_D32_FLOAT;
		depthDesc.usage = TextureUsage::DepthStencil;
		depthDesc.useClearValue = true;
		depthDesc.clearValue.Format = depthDesc.format;
		depthDesc.clearValue.DepthStencil.Depth = 1.0f;
		depthDesc.clearValue.DepthStencil.Stencil = 0;
		app.offscreenTargets.depth = ctx.CreateTexture( depthDesc );

		app.offscreenTargets.width = targetWidth;
		app.offscreenTargets.height = targetHeight;
	}

	void RecordScenePass( ICommandBuffer& buffer, AppState& app, const ScenePushConstants& scenePushConstants )
	{
		LIGHTD3D12_CMD_SCOPE_NAMED( buffer, "HelloRenderToTexture::RecordScenePass", 0xff4cc9f0u );

		RenderPass offscreenRenderPass{};
		offscreenRenderPass.color[ 0 ].loadOp = LoadOp::Clear;
		offscreenRenderPass.color[ 0 ].clearColor = { 0.10f, 0.12f, 0.17f, 1.0f };
		offscreenRenderPass.depthStencil.depthLoadOp = LoadOp::Clear;
		offscreenRenderPass.depthStencil.depthStoreOp = StoreOp::Store;
		offscreenRenderPass.depthStencil.clearDepth = 1.0f;

		Framebuffer offscreenFramebuffer{};
		offscreenFramebuffer.color[ 0 ].texture = app.offscreenTargets.color;
		offscreenFramebuffer.depthStencil.texture = app.offscreenTargets.depth;

		// Pass 1: render the scene into an offscreen render target.
		buffer.CmdBeginRendering( offscreenRenderPass, offscreenFramebuffer );
		buffer.CmdBindRenderPipeline( app.scenePipeline );
		buffer.CmdPushDebugGroupLabel( "Render Scene To Texture", 0xff5ec4ff );
		buffer.CmdPushConstants( &scenePushConstants, sizeof( scenePushConstants ) );
		buffer.CmdDraw( 36 );
		buffer.CmdPopDebugGroupLabel();

		if( app.showWireframe )
		{
			buffer.CmdBindRenderPipeline( app.sceneWireframePipeline );
			buffer.CmdPushDebugGroupLabel( "Render Scene Wireframe", 0xff151515 );
			buffer.CmdPushConstants( &scenePushConstants, sizeof( scenePushConstants ) );
			buffer.CmdDraw( 36 );
			buffer.CmdPopDebugGroupLabel();
		}

		buffer.CmdEndRendering();
		buffer.CmdTransitionTexture( app.offscreenTargets.color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	}

	void RecordPresentPass( ICommandBuffer& buffer, const RenderPipelineState& presentPipeline, TextureHandle currentTexture, const PresentPushConstants& presentPushConstants )
	{
		LIGHTD3D12_CMD_SCOPE_NAMED( buffer, "HelloRenderToTexture::RecordPresentPass", 0xff4cc9f0u );

		RenderPass presentRenderPass{};
		presentRenderPass.color[ 0 ].loadOp = LoadOp::Clear;
		presentRenderPass.color[ 0 ].clearColor = { 0.03f, 0.04f, 0.05f, 1.0f };

		Framebuffer presentFramebuffer{};
		presentFramebuffer.color[ 0 ].texture = currentTexture;

		// Pass 2: sample the offscreen texture and copy it to the backbuffer with a fullscreen triangle.
		buffer.CmdBeginRendering( presentRenderPass, presentFramebuffer );
		buffer.CmdBindRenderPipeline( presentPipeline );
		buffer.CmdPushDebugGroupLabel( "Present Offscreen Texture", 0xff8a5cff );
		buffer.CmdPushConstants( &presentPushConstants, sizeof( presentPushConstants ) );
		buffer.CmdDraw( 3 );
		buffer.CmdPopDebugGroupLabel();
		buffer.CmdEndRendering();
	}

	void RecordImguiPass( ICommandBuffer& buffer, TextureHandle currentTexture, ImguiRenderer* imguiRenderer )
	{
		if( imguiRenderer == nullptr )
		{
			return;
		}

		LIGHTD3D12_CMD_SCOPE_NAMED( buffer, "HelloRenderToTexture::RecordImguiPass", 0xff4cc9f0u );

		RenderPass imguiRenderPass{};
		imguiRenderPass.color[ 0 ].loadOp = LoadOp::Load;

		Framebuffer imguiFramebuffer{};
		imguiFramebuffer.color[ 0 ].texture = currentTexture;

		buffer.CmdBeginRendering( imguiRenderPass, imguiFramebuffer );
		imguiRenderer->Render( buffer );
		buffer.CmdEndRendering();
	}

	bool CommandLineHasFlag( const wchar_t* commandLine, const wchar_t* flag )
	{
		if( commandLine == nullptr || flag == nullptr || flag[ 0 ] == L'\0' )
		{
			return false;
		}

		int argumentCount = 0;
		LPWSTR* arguments = CommandLineToArgvW( commandLine, &argumentCount );
		if( arguments == nullptr )
		{
			return false;
		}

		bool found = false;
		for( int index = 0; index < argumentCount; ++index )
		{
			if( _wcsicmp( arguments[ index ], flag ) == 0 )
			{
				found = true;
				break;
			}
		}

		LocalFree( arguments );
		return found;
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
		windowClass.lpszClassName = L"LightD3D12HelloRenderToTextureWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1280;
		constexpr uint32_t kInitialHeight = 720;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Render To Texture",
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
		// Use --pix when launching the sample if you want it to be attachable for PIX GPU capture.
		contextDesc.enablePixGpuCapture = CommandLineHasFlag( GetCommandLineW(), L"--pix" );
		contextDesc.swapchainBufferCount = 3;

		SwapchainDesc swapchainDesc{};
		swapchainDesc.window = MakeWin32WindowHandle( hwnd );
		swapchainDesc.width = kInitialWidth;
		swapchainDesc.height = kInitialHeight;
		swapchainDesc.vsync = true;

		app.deviceManager = std::make_unique<DeviceManager>( contextDesc, swapchainDesc );
		app.imguiRenderer = std::make_unique<ImguiRenderer>( *app.deviceManager, swapchainDesc.window );
		app.scenePipeline = CreateScenePipeline( *app.deviceManager->GetRenderDevice(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT, false );
		app.sceneWireframePipeline = CreateScenePipeline( *app.deviceManager->GetRenderDevice(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT, true );
		app.presentPipeline = CreatePresentPipeline( *app.deviceManager->GetRenderDevice(), contextDesc.swapchainFormat );
		app.cubeTexture = CreateCubeTexture( *app.deviceManager->GetRenderDevice() );
		RecreateOffscreenTargets( app );

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

			RecreateOffscreenTargets( app );

			const auto now = std::chrono::steady_clock::now();
			float deltaSeconds = std::chrono::duration<float>( now - lastFrameTime ).count();
			lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );
			if( !app.pauseAnimation )
			{
				app.animationTime += deltaSeconds;
			}

			const float animatedTime = app.animationTime * app.rotationSpeed;

			ScenePushConstants scenePushConstants{};
			scenePushConstants.rotationX = animatedTime * 0.85f;
			scenePushConstants.rotationY = animatedTime * 1.10f;
			scenePushConstants.rotationZ = animatedTime * 0.55f;
			scenePushConstants.aspectRatio = static_cast<float>( app.offscreenTargets.width ) / static_cast<float>( app.offscreenTargets.height );
			scenePushConstants.colorPhase = app.colorPhase;
			scenePushConstants.orbitX = std::sin( animatedTime * 0.9f ) * 0.45f;
			scenePushConstants.orbitY = std::cos( animatedTime * 0.7f ) * 0.25f;
			scenePushConstants.cubeScale = app.cubeScale;
			scenePushConstants.textureIndex = ctx->GetBindlessIndex( app.cubeTexture );

			PresentPushConstants presentPushConstants{};
			presentPushConstants.sourceTextureIndex = ctx->GetBindlessIndex( app.offscreenTargets.color );

			if( app.imguiRenderer )
			{
				app.imguiRenderer->NewFrame();
				const float frameRate = ImGui::GetIO().Framerate;
				const float frameTimeMs = frameRate > 0.0f ? 1000.0f / frameRate : 0.0f;

				ImGui::SetNextWindowPos( ImVec2( 16.0f, 16.0f ), ImGuiCond_FirstUseEver );
				ImGui::SetNextWindowSize( ImVec2( 420.0f, 0.0f ), ImGuiCond_FirstUseEver );
				ImGui::Begin( "Hello Render To Texture" );
				ImGui::TextWrapped( "Pass 1 renders the cube into an offscreen texture. Pass 2 samples that texture and displays it on the swapchain." );
				ImGui::Separator();
				ImGui::Text( "Frame time: %.3f ms", frameTimeMs );
				ImGui::Text( "FPS: %.1f", frameRate );
				ImGui::Text( "Window: %u x %u", app.deviceManager->GetWidth(), app.deviceManager->GetHeight() );
				ImGui::Text( "Offscreen target: %u x %u", app.offscreenTargets.width, app.offscreenTargets.height );
				ImGui::TextUnformatted( "Cube texture: enemy.png" );
				ImGui::Checkbox( "Pause animation", &app.pauseAnimation );
				ImGui::SameLine();
				ImGui::Checkbox( "Wireframe overlay", &app.showWireframe );
				ImGui::Checkbox( "Show texture preview", &app.showPreview );
				ImGui::SliderFloat( "Render scale", &app.renderScale, 0.35f, 1.00f, "%.2f" );
				ImGui::SliderFloat( "Rotation speed", &app.rotationSpeed, 0.0f, 2.5f, "%.2f" );
				ImGui::SliderFloat( "Cube scale", &app.cubeScale, 0.45f, 1.10f, "%.2f" );
				if( ImGui::Button( "Randomize vertex colors" ) )
				{
					RandomizeVertexColors( app.colorPhase );
				}

				if( app.showPreview )
				{
					ImGui::Separator();
					ImGui::TextUnformatted( "Offscreen texture preview" );
					const D3D12_GPU_DESCRIPTOR_HANDLE previewHandle = app.imguiRenderer->GetTextureGpuDescriptor( app.offscreenTargets.color );
					const float previewWidth = std::min( ImGui::GetContentRegionAvail().x, 320.0f );
					const float previewHeight = previewWidth * static_cast<float>( app.offscreenTargets.height ) / static_cast<float>( app.offscreenTargets.width );
					ImGui::Image( static_cast<ImTextureID>( previewHandle.ptr ), ImVec2( previewWidth, previewHeight ) );
				}

				ImGui::End();
			}

			auto& buffer = ctx->AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx->GetCurrentSwapchainTexture();

			RecordScenePass( buffer, app, scenePushConstants );
			RecordPresentPass( buffer, app.presentPipeline, currentTexture, presentPushConstants );
			RecordImguiPass( buffer, currentTexture, app.imguiRenderer.get() );

			ctx->Submit( buffer, currentTexture );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		if( app.deviceManager )
		{
			app.deviceManager->WaitIdle();
			RenderDevice* ctx = app.deviceManager->GetRenderDevice();
			DestroyOffscreenTargets( *ctx, app.offscreenTargets );
			if( app.cubeTexture.Valid() )
			{
				ctx->Destroy( app.cubeTexture );
				app.cubeTexture = {};
			}
		}
		app.scenePipeline = {};
		app.sceneWireframePipeline = {};
		app.presentPipeline = {};
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
		MessageBoxA( nullptr, "LightD3D12 HelloRenderToTexture failed.", "LightD3D12", MB_ICONERROR | MB_OK );
		return 1;
	}
}
