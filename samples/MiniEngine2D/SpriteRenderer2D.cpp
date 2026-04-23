#include "SpriteRenderer2D.hpp"

#include "Profiler.hpp"

#include <wincodec.h>

#include <stdexcept>
#include <vector>

namespace mini2d
{
	using namespace lightd3d12;

	namespace
	{
		struct LoadedImage
		{
			uint32_t width = 0;
			uint32_t height = 0;
			std::vector<uint8_t> pixels = {};
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
			float tint[ 4 ] = { 1.0f, 1.0f, 1.0f, 1.0f };
		};

		static_assert( sizeof( SpritePushConstants ) % sizeof( uint32_t ) == 0 );

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

		TextureHandle CreateWhiteTexture( RenderDevice& renderDevice )
		{
			static constexpr uint8_t ourWhitePixel[ 4 ] = { 255, 255, 255, 255 };

			TextureDesc textureDesc{};
			textureDesc.debugName = "MiniEngine White Texture";
			textureDesc.width = 1;
			textureDesc.height = 1;
			textureDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
			textureDesc.usage = TextureUsage::Sampled;
			textureDesc.data = ourWhitePixel;
			textureDesc.rowPitch = 4;
			textureDesc.slicePitch = 4;
			return renderDevice.CreateTexture( textureDesc );
		}

		RenderPipelineState CreateSpritePipeline( RenderDevice& renderDevice, DXGI_FORMAT format )
		{
			static constexpr char ourVertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    float4 gSpriteRect;
    float2 gScreenSize;
    uint gTextureIndex;
    uint gPadding;
    float4 gTint;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 tint : COLOR0;
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
    output.tint = gTint;
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
    float4 gTint;
};

SamplerState gSampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0, float4 tint : COLOR0) : SV_Target0
{
    Texture2D<float4> spriteTexture = ResourceDescriptorHeap[gTextureIndex];
    const float4 color = spriteTexture.Sample(gSampler, uv) * tint;
    clip(color.a - 0.01);
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
			return renderDevice.CreateRenderPipeline( desc );
		}
	}

	void SpriteRenderer2D::Initialize( RenderDevice& renderDevice, DXGI_FORMAT backbufferFormat, const std::filesystem::path& enemyTexturePath )
	{
		pipeline_ = CreateSpritePipeline( renderDevice, backbufferFormat );
		whiteTexture_ = CreateWhiteTexture( renderDevice );

		const LoadedImage image = LoadImageRgba8( enemyTexturePath );
		TextureDesc textureDesc{};
		textureDesc.debugName = "MiniEngine Enemy Texture";
		textureDesc.width = image.width;
		textureDesc.height = image.height;
		textureDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.usage = TextureUsage::Sampled;
		textureDesc.data = image.pixels.data();
		textureDesc.rowPitch = image.width * 4u;
		textureDesc.slicePitch = static_cast<uint32_t>( image.pixels.size() );
		enemyTexture_ = renderDevice.CreateTexture( textureDesc );
	}

	void SpriteRenderer2D::Shutdown( RenderDevice& renderDevice )
	{
		if( enemyTexture_.Valid() )
		{
			renderDevice.Destroy( enemyTexture_ );
			enemyTexture_ = {};
		}

		if( whiteTexture_.Valid() )
		{
			renderDevice.Destroy( whiteTexture_ );
			whiteTexture_ = {};
		}

		pipeline_ = {};
	}

	void SpriteRenderer2D::Render( RenderDevice& renderDevice, ICommandBuffer& commandBuffer, const RenderFrame& frame, uint32_t width, uint32_t height )
	{
		MINI_PROFILE_FUNCTION();
		commandBuffer.CmdBindRenderPipeline( pipeline_ );
		commandBuffer.CmdPushDebugGroupLabel( "Render Scene Proxies", 0xff4cc9f0 );

		for( const SpriteSceneProxy& proxy : frame.sprites )
		{
			const TextureHandle texture = ResolveTextureHandle( proxy.spriteId );
			SpritePushConstants constants{};
			constants.x = proxy.position.x;
			constants.y = proxy.position.y;
			constants.width = proxy.size.x;
			constants.height = proxy.size.y;
			constants.screenWidth = static_cast<float>( width );
			constants.screenHeight = static_cast<float>( height );
			constants.textureIndex = renderDevice.GetBindlessIndex( texture );
			constants.tint[ 0 ] = proxy.tint.r;
			constants.tint[ 1 ] = proxy.tint.g;
			constants.tint[ 2 ] = proxy.tint.b;
			constants.tint[ 3 ] = proxy.tint.a;
			commandBuffer.CmdPushConstants( &constants, sizeof( constants ) );
			commandBuffer.CmdDraw( 6 );
		}

		commandBuffer.CmdPopDebugGroupLabel();
	}

	TextureHandle SpriteRenderer2D::ResolveTextureHandle( SpriteId spriteId ) const
	{
		switch( spriteId )
		{
			case SpriteId::Enemy:
				return enemyTexture_;
			case SpriteId::WhiteSquare:
			default:
				return whiteTexture_;
		}
	}
}
