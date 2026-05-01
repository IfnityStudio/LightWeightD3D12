#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"
#include "LightD3D12/LightHLSLLoader.hpp"

#include <imgui.h>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <wincodec.h>

using namespace DirectX;
using namespace lightd3d12;

namespace
{
	constexpr float kPi = 3.14159265358979323846f;
	constexpr float kFlatModeZoom = 0.78f;

	struct GeoVertex
	{
		XMFLOAT3 position = {};
		XMFLOAT3 normal = {};
		XMFLOAT2 latLonDegrees = {};
		XMFLOAT4 color = {};
	};

	struct GlobePushConstants
	{
		XMFLOAT4X4 worldViewProj = {};
		XMFLOAT4X4 world = {};
		XMFLOAT4 mapParams = {};   // x: flat mode, y: center longitude, z: center latitude, w: flat zoom scale.
		XMFLOAT4 lighting = {};    // xyz: light direction, w: unused.
		uint32_t textureIndex = 0;
		uint32_t useTexture = 0;
		std::array<uint32_t, 2> padding = {};
	};

	static_assert( sizeof( GlobePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct MarkerPushConstants
	{
		XMFLOAT4 rect = {};        // xy: center in NDC, zw: half-size in NDC.
		XMFLOAT4 fillColor = {};
		XMFLOAT4 borderColor = {};
		XMFLOAT4 style = {};       // x: border thickness in local UV space.
	};

	static_assert( sizeof( MarkerPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct MeshGeometry
	{
		BufferHandle vertexBuffer = {};
		BufferHandle indexBuffer = {};
		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
	};

	struct LineGeometry
	{
		BufferHandle vertexBuffer = {};
		uint32_t vertexCount = 0;
		float stepDegrees = 0.0f;
	};

	struct DepthTarget
	{
		TextureHandle texture = {};
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct LoadedImage
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> pixels;
	};

	struct MapEntity
	{
		const char* name = "";
		float latitude = 0.0f;
		float longitude = 0.0f;
		XMFLOAT4 color = {};
	};

	enum class ViewMode : int
	{
		Auto = 0,
		Globe = 1,
		Flat2D = 2,
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState globePipeline;
		RenderPipelineState tilePipeline;
		RenderPipelineState markerPipeline;
		MeshGeometry globe;
		std::array<LineGeometry, 3> tileLevels = {};
		DepthTarget depthTarget;
		TextureHandle mapTexture = {};
		std::vector<MapEntity> entities;
		ViewMode viewMode = ViewMode::Auto;
		float yaw = 0.0f;
		float pitch = 0.18f;
		float zoom = 0.24f;
		float markerSizePixels = 28.0f;
		bool showTiles = true;
		bool showEntities = true;
		bool running = true;
		bool minimized = false;
		float smoothedFrameMs = 16.6f;
		float smoothedFps = 60.0f;
	};

	float ToRadians( float degrees )
	{
		return degrees * kPi / 180.0f;
	}

	float ToDegrees( float radians )
	{
		return radians * 180.0f / kPi;
	}

	float WrapLongitudeDegrees( float longitude )
	{
		while( longitude > 180.0f )
		{
			longitude -= 360.0f;
		}

		while( longitude < -180.0f )
		{
			longitude += 360.0f;
		}

		return longitude;
	}

	float WrappedLongitudeDelta( float longitude, float centerLongitude )
	{
		return WrapLongitudeDegrees( longitude - centerLongitude );
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
			"Failed to open map.png." );

		ComPtr<IWICBitmapFrameDecode> frame;
		ThrowIfFailed( decoder->GetFrame( 0, frame.GetAddressOf() ), "Failed to decode map.png frame." );

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
			"Failed to convert map.png to RGBA8." );

		LoadedImage image{};
		ThrowIfFailed( converter->GetSize( &image.width, &image.height ), "Failed to query map.png size." );

		const uint32_t rowPitch = image.width * 4u;
		image.pixels.resize( static_cast<size_t>( rowPitch ) * image.height );
		ThrowIfFailed(
			converter->CopyPixels( nullptr, rowPitch, static_cast<UINT>( image.pixels.size() ), image.pixels.data() ),
			"Failed to copy map.png pixels." );

		return image;
	}

	TextureHandle CreateMapTexture( RenderDevice& ctx )
	{
		const std::filesystem::path mapPath = std::filesystem::path( __FILE__ ).parent_path() / "map.png";
		if( !std::filesystem::exists( mapPath ) )
		{
			throw std::runtime_error( "GlobeMap requires samples/GlobeMap/map.png." );
		}

		const LoadedImage image = LoadImageRgba8( mapPath );

		TextureDesc textureDesc{};
		textureDesc.debugName = "GlobeMap Earth Map";
		textureDesc.width = image.width;
		textureDesc.height = image.height;
		textureDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.usage = TextureUsage::Sampled;
		textureDesc.data = image.pixels.data();
		textureDesc.rowPitch = image.width * 4u;
		textureDesc.slicePitch = static_cast<uint32_t>( image.pixels.size() );
		return ctx.CreateTexture( textureDesc );
	}

	XMFLOAT3 LatLonToUnitSphere( float latitudeDegrees, float longitudeDegrees, float radius = 1.0f )
	{
		const float latitude = ToRadians( latitudeDegrees );
		const float longitude = ToRadians( longitudeDegrees );
		const float cosLatitude = std::cos( latitude );

		return XMFLOAT3(
			radius * cosLatitude * std::sin( longitude ),
			radius * std::sin( latitude ),
			-radius * cosLatitude * std::cos( longitude ) );
	}

	float EllipseMask( float latitude, float longitude, float centerLatitude, float centerLongitude, float radiusLatitude, float radiusLongitude )
	{
		const float x = WrappedLongitudeDelta( longitude, centerLongitude ) / radiusLongitude;
		const float y = ( latitude - centerLatitude ) / radiusLatitude;
		const float distance = x * x + y * y;
		return std::clamp( 1.0f - distance, 0.0f, 1.0f );
	}

	XMFLOAT4 ComputeProceduralSurfaceColor( float latitude, float longitude )
	{
		float land = 0.0f;
		land = std::max( land, EllipseMask( latitude, longitude, 46.0f, -103.0f, 28.0f, 52.0f ) );
		land = std::max( land, EllipseMask( latitude, longitude, -15.0f, -61.0f, 42.0f, 26.0f ) );
		land = std::max( land, EllipseMask( latitude, longitude, 47.0f, 57.0f, 33.0f, 105.0f ) );
		land = std::max( land, EllipseMask( latitude, longitude, 6.0f, 22.0f, 43.0f, 33.0f ) );
		land = std::max( land, EllipseMask( latitude, longitude, -25.0f, 135.0f, 17.0f, 29.0f ) );
		land = std::max( land, EllipseMask( latitude, longitude, 72.0f, -42.0f, 13.0f, 32.0f ) );

		// A tiny deterministic wobble breaks the perfectly oval look while keeping the sample texture-free.
		const float wobble = 0.12f * std::sin( ToRadians( longitude * 5.0f + latitude * 2.0f ) );
		land = std::clamp( land + wobble, 0.0f, 1.0f );

		if( std::abs( latitude ) > 76.0f )
		{
			return XMFLOAT4( 0.86f, 0.91f, 0.90f, 1.0f );
		}

		const XMFLOAT3 oceanDeep( 0.02f, 0.13f, 0.29f );
		const XMFLOAT3 oceanShallow( 0.03f, 0.28f, 0.45f );
		const XMFLOAT3 landLow( 0.21f, 0.57f, 0.22f );
		const XMFLOAT3 landHigh( 0.65f, 0.55f, 0.34f );
		const float latitudeTint = std::clamp( std::abs( latitude ) / 75.0f, 0.0f, 1.0f );

		const XMFLOAT3 ocean(
			std::lerp( oceanDeep.x, oceanShallow.x, latitudeTint * 0.35f ),
			std::lerp( oceanDeep.y, oceanShallow.y, latitudeTint * 0.35f ),
			std::lerp( oceanDeep.z, oceanShallow.z, latitudeTint * 0.35f ) );

		const XMFLOAT3 terrain(
			std::lerp( landLow.x, landHigh.x, latitudeTint ),
			std::lerp( landLow.y, landHigh.y, latitudeTint ),
			std::lerp( landLow.z, landHigh.z, latitudeTint ) );

		const float finalLand = land > 0.18f ? 1.0f : 0.0f;
		return XMFLOAT4(
			std::lerp( ocean.x, terrain.x, finalLand ),
			std::lerp( ocean.y, terrain.y, finalLand ),
			std::lerp( ocean.z, terrain.z, finalLand ),
			1.0f );
	}

	GeoVertex MakeGeoVertex( float latitude, float longitude, float radius, XMFLOAT4 color )
	{
		const XMFLOAT3 normal = LatLonToUnitSphere( latitude, longitude, 1.0f );
		GeoVertex vertex{};
		vertex.position = LatLonToUnitSphere( latitude, longitude, radius );
		vertex.normal = normal;
		vertex.latLonDegrees = XMFLOAT2( latitude, longitude );
		vertex.color = color;
		return vertex;
	}

	MeshGeometry CreateGlobeGeometry( RenderDevice& ctx )
	{
		constexpr uint32_t kLatitudeSegments = 72;
		constexpr uint32_t kLongitudeSegments = 144;

		std::vector<GeoVertex> vertices;
		std::vector<uint32_t> indices;
		vertices.reserve( ( kLatitudeSegments + 1 ) * ( kLongitudeSegments + 1 ) );
		indices.reserve( kLatitudeSegments * kLongitudeSegments * 6 );

		for( uint32_t latIndex = 0; latIndex <= kLatitudeSegments; ++latIndex )
		{
			const float latitude = -90.0f + 180.0f * static_cast<float>( latIndex ) / static_cast<float>( kLatitudeSegments );
			for( uint32_t lonIndex = 0; lonIndex <= kLongitudeSegments; ++lonIndex )
			{
				const float longitude = -180.0f + 360.0f * static_cast<float>( lonIndex ) / static_cast<float>( kLongitudeSegments );
				vertices.push_back( MakeGeoVertex( latitude, longitude, 1.0f, ComputeProceduralSurfaceColor( latitude, longitude ) ) );
			}
		}

		for( uint32_t latIndex = 0; latIndex < kLatitudeSegments; ++latIndex )
		{
			for( uint32_t lonIndex = 0; lonIndex < kLongitudeSegments; ++lonIndex )
			{
				const uint32_t i0 = latIndex * ( kLongitudeSegments + 1 ) + lonIndex;
				const uint32_t i1 = i0 + 1;
				const uint32_t i2 = i0 + ( kLongitudeSegments + 1 );
				const uint32_t i3 = i2 + 1;

				indices.push_back( i0 );
				indices.push_back( i2 );
				indices.push_back( i1 );
				indices.push_back( i1 );
				indices.push_back( i2 );
				indices.push_back( i3 );
			}
		}

		BufferDesc vertexDesc{};
		vertexDesc.debugName = "GlobeMap Globe Vertices";
		vertexDesc.size = vertices.size() * sizeof( GeoVertex );
		vertexDesc.stride = sizeof( GeoVertex );
		vertexDesc.bufferType = BufferDesc::BufferType::VertexBuffer;
		vertexDesc.data = vertices.data();
		vertexDesc.dataSize = vertexDesc.size;

		BufferDesc indexDesc{};
		indexDesc.debugName = "GlobeMap Globe Indices";
		indexDesc.size = indices.size() * sizeof( uint32_t );
		indexDesc.bufferType = BufferDesc::BufferType::IndexBuffer;
		indexDesc.data = indices.data();
		indexDesc.dataSize = indexDesc.size;

		MeshGeometry geometry{};
		geometry.vertexBuffer = ctx.CreateBuffer( vertexDesc );
		geometry.indexBuffer = ctx.CreateBuffer( indexDesc );
		geometry.vertexCount = static_cast<uint32_t>( vertices.size() );
		geometry.indexCount = static_cast<uint32_t>( indices.size() );
		return geometry;
	}

	void AddTileLineSegment( std::vector<GeoVertex>& vertices, float lat0, float lon0, float lat1, float lon1, XMFLOAT4 color )
	{
		constexpr float kTileRadius = 1.006f;
		vertices.push_back( MakeGeoVertex( lat0, lon0, kTileRadius, color ) );
		vertices.push_back( MakeGeoVertex( lat1, lon1, kTileRadius, color ) );
	}

	LineGeometry CreateTileGrid( RenderDevice& ctx, float stepDegrees )
	{
		std::vector<GeoVertex> vertices;
		const XMFLOAT4 minorColor( 0.27f, 0.96f, 0.43f, 0.88f );
		const XMFLOAT4 majorColor( 0.87f, 1.00f, 0.88f, 0.96f );
		const float segmentStep = std::min( 5.0f, stepDegrees );

		for( float latitude = -80.0f; latitude <= 80.0f; latitude += stepDegrees )
		{
			const bool major = std::fmod( std::abs( latitude ), 30.0f ) < 0.01f;
			for( float longitude = -180.0f; longitude < 180.0f; longitude += segmentStep )
			{
				AddTileLineSegment( vertices, latitude, longitude, latitude, std::min( longitude + segmentStep, 180.0f ), major ? majorColor : minorColor );
			}
		}

		for( float longitude = -180.0f; longitude <= 180.0f; longitude += stepDegrees )
		{
			const bool major = std::fmod( std::abs( longitude ), 30.0f ) < 0.01f;
			for( float latitude = -80.0f; latitude < 80.0f; latitude += segmentStep )
			{
				AddTileLineSegment( vertices, latitude, longitude, std::min( latitude + segmentStep, 80.0f ), longitude, major ? majorColor : minorColor );
			}
		}

		BufferDesc vertexDesc{};
		vertexDesc.debugName = "GlobeMap Tile Grid";
		vertexDesc.size = vertices.size() * sizeof( GeoVertex );
		vertexDesc.stride = sizeof( GeoVertex );
		vertexDesc.bufferType = BufferDesc::BufferType::VertexBuffer;
		vertexDesc.data = vertices.data();
		vertexDesc.dataSize = vertexDesc.size;

		LineGeometry grid{};
		grid.vertexBuffer = ctx.CreateBuffer( vertexDesc );
		grid.vertexCount = static_cast<uint32_t>( vertices.size() );
		grid.stepDegrees = stepDegrees;
		return grid;
	}

	std::vector<MapEntity> CreateDemoEntities()
	{
		return
		{
			{ "Madrid", 40.4168f, -3.7038f, XMFLOAT4( 0.16f, 0.95f, 0.35f, 0.94f ) },
			{ "New York", 40.7128f, -74.0060f, XMFLOAT4( 0.18f, 0.82f, 1.00f, 0.94f ) },
			{ "Tokyo", 35.6762f, 139.6503f, XMFLOAT4( 1.00f, 0.55f, 0.20f, 0.94f ) },
			{ "Cairo", 30.0444f, 31.2357f, XMFLOAT4( 1.00f, 0.92f, 0.30f, 0.94f ) },
			{ "Sao Paulo", -23.5558f, -46.6396f, XMFLOAT4( 0.62f, 1.00f, 0.34f, 0.94f ) },
			{ "Cape Town", -33.9249f, 18.4241f, XMFLOAT4( 0.98f, 0.35f, 0.90f, 0.94f ) },
			{ "Sydney", -33.8688f, 151.2093f, XMFLOAT4( 0.65f, 0.82f, 1.00f, 0.94f ) },
			{ "Anchorage", 61.2181f, -149.9003f, XMFLOAT4( 0.92f, 1.00f, 0.98f, 0.94f ) },
		};
	}

	RenderPipelineState CreateGeoPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, bool lineList )
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
				.alignedByteOffset = static_cast<uint32_t>( offsetof( GeoVertex, position ) ),
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			},
			VertexInputElementDesc
			{
				.semanticName = "NORMAL",
				.semanticIndex = 0,
				.format = DXGI_FORMAT_R32G32B32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = static_cast<uint32_t>( offsetof( GeoVertex, normal ) ),
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			},
			VertexInputElementDesc
			{
				.semanticName = "TEXCOORD",
				.semanticIndex = 0,
				.format = DXGI_FORMAT_R32G32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = static_cast<uint32_t>( offsetof( GeoVertex, latLonDegrees ) ),
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			},
			VertexInputElementDesc
			{
				.semanticName = "COLOR",
				.semanticIndex = 0,
				.format = DXGI_FORMAT_R32G32B32A32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = static_cast<uint32_t>( offsetof( GeoVertex, color ) ),
				.inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.instanceDataStepRate = 0
			}
		};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/GlobeMapGeoVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( lineList ? "shaders/GlobeMapTilePS.hlsl" : "shaders/GlobeMapSurfacePS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = depthFormat;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = lineList ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.depthStencilState.StencilEnable = FALSE;
		if( lineList )
		{
			desc.primitiveType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
			desc.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
		}
		return ctx.CreateRenderPipeline( desc );
	}

	RenderPipelineState CreateMarkerPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat )
	{
		RenderPipelineDesc desc{};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/GlobeMapMarkerVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( "shaders/GlobeMapMarkerPS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = DXGI_FORMAT_UNKNOWN;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;

		auto& targetBlend = desc.blendState.RenderTarget[ 0 ];
		targetBlend.BlendEnable = TRUE;
		targetBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		targetBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		targetBlend.BlendOp = D3D12_BLEND_OP_ADD;
		targetBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
		targetBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		targetBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		targetBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		return ctx.CreateRenderPipeline( desc );
	}

	void DestroyMesh( RenderDevice& ctx, MeshGeometry& mesh )
	{
		if( mesh.vertexBuffer.Valid() )
		{
			ctx.Destroy( mesh.vertexBuffer );
			mesh.vertexBuffer = {};
		}

		if( mesh.indexBuffer.Valid() )
		{
			ctx.Destroy( mesh.indexBuffer );
			mesh.indexBuffer = {};
		}

		mesh.vertexCount = 0;
		mesh.indexCount = 0;
	}

	void DestroyLineGeometry( RenderDevice& ctx, LineGeometry& lines )
	{
		if( lines.vertexBuffer.Valid() )
		{
			ctx.Destroy( lines.vertexBuffer );
			lines.vertexBuffer = {};
		}

		lines.vertexCount = 0;
		lines.stepDegrees = 0.0f;
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
		depthDesc.debugName = "GlobeMap Depth";
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

	bool IsFlatMode( const AppState& app )
	{
		if( app.viewMode == ViewMode::Flat2D )
		{
			return true;
		}

		if( app.viewMode == ViewMode::Globe )
		{
			return false;
		}

		return app.zoom >= kFlatModeZoom;
	}

	float ComputeViewDistance( float zoom )
	{
		return std::lerp( 4.25f, 1.08f, std::clamp( zoom, 0.0f, 1.0f ) );
	}

	float ComputeFlatScale( float zoom )
	{
		const float normalized = std::clamp( ( zoom - kFlatModeZoom ) / ( 1.0f - kFlatModeZoom ), 0.0f, 1.0f );
		return std::lerp( 1.05f, 5.0f, normalized );
	}

	float ComputeCenterLongitude( const AppState& app )
	{
		return WrapLongitudeDegrees( -ToDegrees( app.yaw ) );
	}

	float ComputeCenterLatitude( const AppState& app )
	{
		return std::clamp( -ToDegrees( app.pitch ), -82.0f, 82.0f );
	}

	uint32_t SelectTileLevel( float zoom )
	{
		if( zoom < 0.38f )
		{
			return 0;
		}

		if( zoom < 0.72f )
		{
			return 1;
		}

		return 2;
	}

	bool ProjectGlobeEntityToNdc( const MapEntity& entity, const XMMATRIX& world, const XMMATRIX& worldViewProjection, float viewDistance, XMFLOAT2& ndc )
	{
		const XMFLOAT3 localPositionFloat = LatLonToUnitSphere( entity.latitude, entity.longitude, 1.035f );
		const XMFLOAT3 localNormalFloat = LatLonToUnitSphere( entity.latitude, entity.longitude, 1.0f );
		const XMVECTOR localPosition = XMLoadFloat3( &localPositionFloat );
		const XMVECTOR localNormal = XMLoadFloat3( &localNormalFloat );
		const XMVECTOR worldPosition = XMVector3Transform( localPosition, world );
		const XMVECTOR worldNormal = XMVector3Normalize( XMVector3TransformNormal( localNormal, world ) );
		const XMVECTOR cameraPosition = XMVectorSet( 0.0f, 0.0f, -viewDistance, 1.0f );
		const XMVECTOR toCamera = XMVector3Normalize( XMVectorSubtract( cameraPosition, worldPosition ) );
		const float facingCamera = XMVectorGetX( XMVector3Dot( worldNormal, toCamera ) );

		if( facingCamera <= 0.0f )
		{
			return false;
		}

		const XMVECTOR clip = XMVector4Transform( XMVectorSet( localPositionFloat.x, localPositionFloat.y, localPositionFloat.z, 1.0f ), worldViewProjection );
		const float w = XMVectorGetW( clip );
		if( w <= 0.0001f )
		{
			return false;
		}

		ndc.x = XMVectorGetX( clip ) / w;
		ndc.y = XMVectorGetY( clip ) / w;
		return ndc.x >= -1.15f && ndc.x <= 1.15f && ndc.y >= -1.15f && ndc.y <= 1.15f;
	}

	bool ProjectFlatEntityToNdc( const MapEntity& entity, float centerLatitude, float centerLongitude, float mapScale, XMFLOAT2& ndc )
	{
		ndc.x = WrappedLongitudeDelta( entity.longitude, centerLongitude ) / 180.0f * mapScale;
		ndc.y = ( entity.latitude - centerLatitude ) / 90.0f * mapScale;
		return ndc.x >= -1.08f && ndc.x <= 1.08f && ndc.y >= -1.08f && ndc.y <= 1.08f;
	}

	GlobePushConstants BuildGlobePushConstants( const AppState& app, const XMMATRIX& worldViewProjection, const XMMATRIX& world, bool flatMode )
	{
		GlobePushConstants push{};
		XMStoreFloat4x4( &push.worldViewProj, worldViewProjection );
		XMStoreFloat4x4( &push.world, world );
		push.mapParams = XMFLOAT4(
			flatMode ? 1.0f : 0.0f,
			ComputeCenterLongitude( app ),
			ComputeCenterLatitude( app ),
			ComputeFlatScale( app.zoom ) );
		push.lighting = XMFLOAT4( -0.45f, 0.62f, -0.64f, 0.0f );
		if( app.mapTexture.Valid() && app.deviceManager )
		{
			push.textureIndex = app.deviceManager->GetRenderDevice()->GetBindlessIndex( app.mapTexture );
			push.useTexture = 1u;
		}
		return push;
	}

	void DrawEntityMarkers( ICommandBuffer& commandBuffer, AppState& app, const XMMATRIX& world, const XMMATRIX& worldViewProjection, bool flatMode )
	{
		if( !app.showEntities )
		{
			return;
		}

		const uint32_t width = std::max<uint32_t>( 1u, app.deviceManager->GetWidth() );
		const uint32_t height = std::max<uint32_t>( 1u, app.deviceManager->GetHeight() );
		const float halfWidthNdc = app.markerSizePixels / static_cast<float>( width );
		const float halfHeightNdc = app.markerSizePixels * 0.58f / static_cast<float>( height );
		const float viewDistance = ComputeViewDistance( app.zoom );
		const float centerLatitude = ComputeCenterLatitude( app );
		const float centerLongitude = ComputeCenterLongitude( app );
		const float mapScale = ComputeFlatScale( app.zoom );

		commandBuffer.CmdBindRenderPipeline( app.markerPipeline );
		for( const MapEntity& entity : app.entities )
		{
			XMFLOAT2 ndc{};
			const bool visible = flatMode ?
				ProjectFlatEntityToNdc( entity, centerLatitude, centerLongitude, mapScale, ndc ) :
				ProjectGlobeEntityToNdc( entity, world, worldViewProjection, viewDistance, ndc );

			if( !visible )
			{
				continue;
			}

			MarkerPushConstants marker{};
			marker.rect = XMFLOAT4( ndc.x, ndc.y, halfWidthNdc, halfHeightNdc );
			marker.fillColor = entity.color;
			marker.borderColor = XMFLOAT4( 0.95f, 1.0f, 0.92f, 1.0f );
			marker.style = XMFLOAT4( 0.12f, 0.0f, 0.0f, 0.0f );
			commandBuffer.CmdPushConstants( &marker, sizeof( marker ) );
			commandBuffer.CmdDraw( 6 );
		}
	}

	void DrawEntityLabels( const AppState& app, const XMMATRIX& world, const XMMATRIX& worldViewProjection, bool flatMode )
	{
		if( !app.showEntities )
		{
			return;
		}

		ImDrawList* drawList = ImGui::GetForegroundDrawList();
		const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
		const float viewDistance = ComputeViewDistance( app.zoom );
		const float centerLatitude = ComputeCenterLatitude( app );
		const float centerLongitude = ComputeCenterLongitude( app );
		const float mapScale = ComputeFlatScale( app.zoom );

		for( const MapEntity& entity : app.entities )
		{
			XMFLOAT2 ndc{};
			const bool visible = flatMode ?
				ProjectFlatEntityToNdc( entity, centerLatitude, centerLongitude, mapScale, ndc ) :
				ProjectGlobeEntityToNdc( entity, world, worldViewProjection, viewDistance, ndc );

			if( !visible )
			{
				continue;
			}

			const ImVec2 screenPosition(
				( ndc.x * 0.5f + 0.5f ) * displaySize.x + app.markerSizePixels * 0.62f,
				( -ndc.y * 0.5f + 0.5f ) * displaySize.y - 8.0f );
			drawList->AddText( screenPosition, IM_COL32( 230, 255, 228, 235 ), entity.name );
		}
	}

	void ApplyMouseNavigation( AppState& app )
	{
		ImGuiIO& io = ImGui::GetIO();
		if( io.WantCaptureMouse )
		{
			return;
		}

		if( io.MouseDown[ 0 ] )
		{
			app.yaw += io.MouseDelta.x * 0.0065f;
			app.pitch += io.MouseDelta.y * 0.0065f;
			app.pitch = std::clamp( app.pitch, -1.35f, 1.35f );
		}

		if( std::abs( io.MouseWheel ) > 0.0f )
		{
			app.zoom = std::clamp( app.zoom + io.MouseWheel * 0.075f, 0.0f, 1.0f );
		}
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
		windowClass.lpszClassName = L"LightD3D12GlobeMapWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1280;
		constexpr uint32_t kInitialHeight = 720;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Globe Map",
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
		app.globePipeline = CreateGeoPipeline( ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, false );
		app.tilePipeline = CreateGeoPipeline( ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, true );
		app.markerPipeline = CreateMarkerPipeline( ctx, contextDesc.swapchainFormat );
		app.mapTexture = CreateMapTexture( ctx );
		app.globe = CreateGlobeGeometry( ctx );
		app.tileLevels[ 0 ] = CreateTileGrid( ctx, 30.0f );
		app.tileLevels[ 1 ] = CreateTileGrid( ctx, 15.0f );
		app.tileLevels[ 2 ] = CreateTileGrid( ctx, 5.0f );
		app.entities = CreateDemoEntities();
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
			float deltaSeconds = std::chrono::duration<float>( now - lastFrameTime ).count();
			lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );
			const float frameMs = deltaSeconds * 1000.0f;
			app.smoothedFrameMs = std::lerp( app.smoothedFrameMs, frameMs, 0.08f );
			app.smoothedFps = 1000.0f / std::max( app.smoothedFrameMs, 0.001f );

			app.imguiRenderer->NewFrame();
			ApplyMouseNavigation( app );

			const bool flatMode = IsFlatMode( app );
			const uint32_t selectedTileLevel = SelectTileLevel( app.zoom );
			const uint32_t width = std::max<uint32_t>( 1u, app.deviceManager->GetWidth() );
			const uint32_t height = std::max<uint32_t>( 1u, app.deviceManager->GetHeight() );
			const float aspectRatio = static_cast<float>( width ) / static_cast<float>( height );
			const float viewDistance = ComputeViewDistance( app.zoom );
			const XMMATRIX world = XMMatrixRotationX( app.pitch ) * XMMatrixRotationY( app.yaw );
			const XMMATRIX view = XMMatrixLookAtLH(
				XMVectorSet( 0.0f, 0.0f, -viewDistance, 1.0f ),
				XMVectorZero(),
				XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f ) );
			const XMMATRIX projection = XMMatrixPerspectiveFovLH( XMConvertToRadians( 43.0f ), aspectRatio, 0.05f, 64.0f );
			const XMMATRIX worldViewProjection = world * view * projection;
			const GlobePushConstants globePush = BuildGlobePushConstants( app, worldViewProjection, world, flatMode );

			DrawEntityLabels( app, world, worldViewProjection, flatMode );

			ImGui::SetNextWindowPos( ImVec2( 16.0f, 16.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowSize( ImVec2( 390.0f, 0.0f ), ImGuiCond_FirstUseEver );
			ImGui::Begin( "Globe Map" );
			ImGui::TextWrapped( "map.png is sampled with real latitude/longitude UVs. The tiles and entities are drawn on top, and the same data switches to a flat 2D map when you zoom in." );
			ImGui::Separator();
			ImGui::Text( "Controls: drag left mouse to rotate, wheel to zoom." );
			ImGui::Text( "Mode: %s", flatMode ? "Flat 2D" : "Globe" );
			ImGui::Text( "FPS: %.1f | Frame: %.2f ms", app.smoothedFps, app.smoothedFrameMs );
			ImGui::SliderFloat( "Zoom", &app.zoom, 0.0f, 1.0f, "%.2f" );
			ImGui::SliderFloat( "Marker size", &app.markerSizePixels, 14.0f, 48.0f, "%.0f px" );
			ImGui::Checkbox( "Show tile grid", &app.showTiles );
			ImGui::Checkbox( "Show entities", &app.showEntities );
			const char* modeItems[] = { "Auto", "Force globe", "Force flat 2D" };
			int modeIndex = static_cast<int>( app.viewMode );
			if( ImGui::Combo( "View mode", &modeIndex, modeItems, IM_ARRAYSIZE( modeItems ) ) )
			{
				app.viewMode = static_cast<ViewMode>( modeIndex );
			}
			if( ImGui::Button( "Reset camera" ) )
			{
				app.yaw = 0.0f;
				app.pitch = 0.18f;
				app.zoom = 0.24f;
			}
			ImGui::Separator();
			ImGui::Text( "Center lat/lon: %.2f, %.2f", ComputeCenterLatitude( app ), ComputeCenterLongitude( app ) );
			ImGui::TextUnformatted( "Texture: map.png" );
			ImGui::Text( "Tile step: %.0f degrees", app.tileLevels[ selectedTileLevel ].stepDegrees );
			ImGui::Text( "Entities: %u", static_cast<uint32_t>( app.entities.size() ) );
			ImGui::End();

			auto& commandBuffer = renderDevice->AcquireCommandBuffer();
			const TextureHandle currentTexture = renderDevice->GetCurrentSwapchainTexture();

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.005f, 0.008f, 0.014f, 1.0f };
			renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
			renderPass.depthStencil.depthStoreOp = StoreOp::Store;
			renderPass.depthStencil.clearDepth = 1.0f;

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;
			framebuffer.depthStencil.texture = app.depthTarget.texture;

			commandBuffer.CmdBeginRendering( renderPass, framebuffer );
			commandBuffer.CmdPushDebugGroupLabel( "Globe Map", 0xff35f06b );

			commandBuffer.CmdBindRenderPipeline( app.globePipeline );
			commandBuffer.CmdBindVertexBuffer( app.globe.vertexBuffer, sizeof( GeoVertex ) );
			commandBuffer.CmdBindIndexBuffer( app.globe.indexBuffer, DXGI_FORMAT_R32_UINT );
			commandBuffer.CmdPushConstants( &globePush, sizeof( globePush ) );
			commandBuffer.CmdDrawIndexed( app.globe.indexCount );

			if( app.showTiles )
			{
				const LineGeometry& tileGrid = app.tileLevels[ selectedTileLevel ];
				commandBuffer.CmdBindRenderPipeline( app.tilePipeline );
				commandBuffer.CmdBindVertexBuffer( tileGrid.vertexBuffer, sizeof( GeoVertex ) );
				commandBuffer.CmdPushConstants( &globePush, sizeof( globePush ) );
				commandBuffer.CmdDraw( tileGrid.vertexCount );
			}

			DrawEntityMarkers( commandBuffer, app, world, worldViewProjection, flatMode );
			commandBuffer.CmdPopDebugGroupLabel();
			app.imguiRenderer->Render( commandBuffer );
			commandBuffer.CmdEndRendering();

			renderDevice->Submit( commandBuffer, currentTexture );
		}

		ctx.WaitIdle();
		DestroyMesh( ctx, app.globe );
		for( LineGeometry& tileLevel : app.tileLevels )
		{
			DestroyLineGeometry( ctx, tileLevel );
		}
		if( app.mapTexture.Valid() )
		{
			ctx.Destroy( app.mapTexture );
			app.mapTexture = {};
		}
		DestroyDepthTarget( ctx, app.depthTarget );
		app.markerPipeline = {};
		app.tilePipeline = {};
		app.globePipeline = {};
		app.imguiRenderer.reset();
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
	catch( const std::exception& exception )
	{
		if( shouldUninitializeCom )
		{
			CoUninitialize();
		}
		MessageBoxA( nullptr, exception.what(), "LightD3D12 Globe Map", MB_ICONERROR | MB_OK );
		return 1;
	}
}
