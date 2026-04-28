#include "LightD3D12/LightAssimpImporter.hpp"
#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"
#include "LightD3D12/LightHLSLLoader.hpp"

#include <imgui.h>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lightd3d12;
using namespace DirectX;

namespace
{
	constexpr uint32_t kShapeNoiseSize = 64;
	constexpr uint32_t kDetailNoiseSize = 32;
	constexpr float kCameraNear = 0.1f;
	constexpr float kCameraFar = 4000.0f;
	constexpr float kCameraFovRadians = 58.0f * ( 3.1415926535f / 180.0f );

	struct MeshVertex
	{
		std::array<float, 3> position = {};
		std::array<float, 3> normal = { 0.0f, 1.0f, 0.0f };
		std::array<float, 2> texCoord = {};
	};

	static_assert( sizeof( MeshVertex ) == sizeof( float ) * 8 );

	struct LoadedModel
	{
		BufferHandle vertexBuffer = {};
		BufferHandle indexBuffer = {};
		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
	};

	struct DepthTarget
	{
		TextureHandle texture = {};
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct NoisePushConstants
	{
		uint32_t outputTextureIndex = 0;
		uint32_t textureSize = 1;
		uint32_t padding_[ 2 ] = {};
	};

	static_assert( sizeof( NoisePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct GroundPushConstants
	{
		XMFLOAT4X4 worldViewProj = {};
		XMFLOAT4X4 world = {};
		std::array<float, 4> groundTint = { 0.33f, 0.35f, 0.29f, 1.0f };
	};

	static_assert( sizeof( GroundPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct CloudPushConstants
	{
		XMFLOAT4X4 invViewProjection = {};
		std::array<float, 4> cameraPositionAndTime = {};
		std::array<float, 4> planetCenterAndRadius = {};
		std::array<float, 4> cloudLayer = {};
		std::array<float, 4> windDirectionAndSpeed = {};
		std::array<float, 4> noiseSampling = {};
		std::array<float, 4> lighting0 = {};
		std::array<float, 4> sunDirectionAndLightFactor = {};
		std::array<float, 4> colors0 = {};
		std::array<float, 4> colors1 = {};
		std::array<float, 4> colors2 = {};
		std::array<uint32_t, 4> textureIndices = {};
	};

	static_assert( sizeof( CloudPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct NoiseResources
	{
		TextureHandle shape = {};
		TextureHandle detail = {};
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		ComputePipelineState shapeNoisePipeline;
		ComputePipelineState detailNoisePipeline;
		RenderPipelineState groundPipeline;
		RenderPipelineState cloudPipeline;
		LoadedModel groundPlane;
		DepthTarget depthTarget;
		NoiseResources noise;
		bool running = true;
		bool minimized = false;
		bool pauseAnimation = false;
		bool noiseDirty = true;
		float animationTime = 0.0f;
		float orbitAngle = 0.0f;
		float smoothedFrameMs = 16.6f;
		float smoothedFps = 60.0f;
		float planetRadius = 800.0f;
		float cloudMinHeight = 140.0f;
		float cloudMaxHeight = 260.0f;
		float cloudCoverage = 0.58f;
		float shapeNoiseScale = 1.0f;
		float detailNoiseScale = 1.0f;
		float detailNoiseModifier = 0.34f;
		float turbulenceAmount = 12.0f;
		float windHeadingDegrees = 30.0f;
		float windSpeed = 12.0f;
		float windShearOffset = 38.0f;
		float sunElevationDegrees = 18.0f;
		float sunLightFactor = 1.8f;
		float ambientLightFactor = 0.42f;
		float precipitation = 1.15f;
		float lightStepLength = 16.0f;
		float lightConeRadius = 0.55f;
		float exposure = 1.35f;
		int maxSteps = 96;
	};

	std::array<float, 3> NormalizeNormal( const std::array<float, 3>& normal )
	{
		const float lengthSquared = normal[ 0 ] * normal[ 0 ] + normal[ 1 ] * normal[ 1 ] + normal[ 2 ] * normal[ 2 ];
		if( lengthSquared <= 1.0e-8f )
		{
			return { 0.0f, 1.0f, 0.0f };
		}

		const float inverseLength = 1.0f / std::sqrt( lengthSquared );
		return { normal[ 0 ] * inverseLength, normal[ 1 ] * inverseLength, normal[ 2 ] * inverseLength };
	}

	LoadedModel CreateGroundPlaneModel( RenderDevice& ctx )
	{
		if( !LightAssimpImporter::IsAvailable() )
		{
			throw std::runtime_error( "Assimp is not enabled in this build." );
		}

		const std::filesystem::path sampleDirectory = std::filesystem::path( __FILE__ ).parent_path();
		const std::filesystem::path assetPath = sampleDirectory.parent_path().parent_path() / "Data" / "volumetric_clouds" / "plane.obj";
		if( !std::filesystem::exists( assetPath ) )
		{
			throw std::runtime_error( "Failed to find Data/volumetric_clouds/plane.obj." );
		}

		const ImportedScene importedScene = LightAssimpImporter::ImportScene( assetPath );
		if( importedScene.meshes.empty() )
		{
			throw std::runtime_error( "The imported plane scene does not contain meshes." );
		}

		std::vector<MeshVertex> vertices;
		std::vector<uint32_t> indices;
		for( const ImportedMesh& importedMesh : importedScene.meshes )
		{
			if( importedMesh.vertices.empty() || importedMesh.indices.empty() )
			{
				continue;
			}

			const uint32_t baseVertex = static_cast<uint32_t>( vertices.size() );
			for( const ImportedVertex& importedVertex : importedMesh.vertices )
			{
				MeshVertex vertex{};
				vertex.position = importedVertex.position;
				vertex.normal = NormalizeNormal( importedVertex.normal );
				vertex.texCoord = importedVertex.texCoord;
				vertices.push_back( vertex );
			}

			for( const uint32_t index : importedMesh.indices )
			{
				indices.push_back( baseVertex + index );
			}
		}

		if( vertices.empty() || indices.empty() )
		{
			throw std::runtime_error( "The imported plane scene did not yield drawable geometry." );
		}

		BufferDesc vertexBufferDesc{};
		vertexBufferDesc.debugName = "HelloVolumetricClouds Ground Vertex Buffer";
		vertexBufferDesc.size = static_cast<uint64_t>( vertices.size() * sizeof( MeshVertex ) );
		vertexBufferDesc.stride = sizeof( MeshVertex );
		vertexBufferDesc.bufferType = BufferDesc::BufferType::VertexBuffer;
		vertexBufferDesc.data = vertices.data();
		vertexBufferDesc.dataSize = vertexBufferDesc.size;

		BufferDesc indexBufferDesc{};
		indexBufferDesc.debugName = "HelloVolumetricClouds Ground Index Buffer";
		indexBufferDesc.size = static_cast<uint64_t>( indices.size() * sizeof( uint32_t ) );
		indexBufferDesc.bufferType = BufferDesc::BufferType::IndexBuffer;
		indexBufferDesc.data = indices.data();
		indexBufferDesc.dataSize = indexBufferDesc.size;

		LoadedModel model{};
		model.vertexBuffer = ctx.CreateBuffer( vertexBufferDesc );
		model.indexBuffer = ctx.CreateBuffer( indexBufferDesc );
		model.vertexCount = static_cast<uint32_t>( vertices.size() );
		model.indexCount = static_cast<uint32_t>( indices.size() );
		return model;
	}

	void DestroyModel( RenderDevice& ctx, LoadedModel& model )
	{
		if( model.vertexBuffer.Valid() )
		{
			ctx.Destroy( model.vertexBuffer );
			model.vertexBuffer = {};
		}

		if( model.indexBuffer.Valid() )
		{
			ctx.Destroy( model.indexBuffer );
			model.indexBuffer = {};
		}

		model.vertexCount = 0;
		model.indexCount = 0;
	}

	TextureHandle CreateNoiseTexture( RenderDevice& ctx, const char* debugName, uint32_t size )
	{
		TextureDesc desc{};
		desc.debugName = debugName;
		desc.width = size;
		desc.height = size;
		desc.depthOrArraySize = static_cast<uint16_t>( size );
		desc.dimension = TextureDimension::Texture3D;
		desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.usage = TextureUsage::Sampled | TextureUsage::UnorderedAccess;
		desc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		return ctx.CreateTexture( desc );
	}

	void DestroyNoiseResources( RenderDevice& ctx, NoiseResources& noise )
	{
		if( noise.shape.Valid() )
		{
			ctx.Destroy( noise.shape );
			noise.shape = {};
		}

		if( noise.detail.Valid() )
		{
			ctx.Destroy( noise.detail );
			noise.detail = {};
		}
	}

	void DestroyDepthTarget( RenderDevice& ctx, DepthTarget& target )
	{
		if( target.texture.Valid() )
		{
			ctx.Destroy( target.texture );
			target.texture = {};
		}

		target.width = 0;
		target.height = 0;
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

		ctx.WaitIdle();
		DestroyDepthTarget( ctx, app.depthTarget );

		TextureDesc depthDesc{};
		depthDesc.debugName = "HelloVolumetricClouds Scene Depth";
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

	ComputePipelineState CreateShapeNoisePipeline( RenderDevice& ctx )
	{
		ComputePipelineDesc desc{};
		desc.computeShader = LightHLSLLoader::LoadStage( "shaders/HelloVolumetricCloudsShapeNoiseCS.hlsl", "cs_6_6" );
		return ctx.CreateComputePipeline( desc );
	}

	ComputePipelineState CreateDetailNoisePipeline( RenderDevice& ctx )
	{
		ComputePipelineDesc desc{};
		desc.computeShader = LightHLSLLoader::LoadStage( "shaders/HelloVolumetricCloudsDetailNoiseCS.hlsl", "cs_6_6" );
		return ctx.CreateComputePipeline( desc );
	}

	RenderPipelineState CreateGroundPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat )
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
				.alignedByteOffset = 0
			},
			VertexInputElementDesc
			{
				.semanticName = "NORMAL",
				.semanticIndex = 0,
				.format = DXGI_FORMAT_R32G32B32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = 12
			},
			VertexInputElementDesc
			{
				.semanticName = "TEXCOORD",
				.semanticIndex = 0,
				.format = DXGI_FORMAT_R32G32_FLOAT,
				.inputSlot = 0,
				.alignedByteOffset = 24
			}
		};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/HelloVolumetricCloudsGroundVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( "shaders/HelloVolumetricCloudsGroundPS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = depthFormat;
		desc.rasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	RenderPipelineState CreateCloudPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat )
	{
		RenderPipelineDesc desc{};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/HelloVolumetricCloudsVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( "shaders/HelloVolumetricCloudsPS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = DXGI_FORMAT_UNKNOWN;
		desc.rasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		desc.blendState.AlphaToCoverageEnable = FALSE;
		desc.blendState.IndependentBlendEnable = FALSE;
		auto& renderTargetBlend = desc.blendState.RenderTarget[ 0 ];
		renderTargetBlend.BlendEnable = TRUE;
		renderTargetBlend.LogicOpEnable = FALSE;
		renderTargetBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		renderTargetBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		renderTargetBlend.BlendOp = D3D12_BLEND_OP_ADD;
		renderTargetBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
		renderTargetBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		renderTargetBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		renderTargetBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
		renderTargetBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		return ctx.CreateRenderPipeline( desc );
	}

	XMVECTOR BuildCameraPosition( float orbitAngle )
	{
		const float x = std::sin( orbitAngle ) * 65.0f;
		const float y = 52.0f + std::sin( orbitAngle * 0.73f ) * 7.0f;
		const float z = -235.0f + std::cos( orbitAngle * 0.9f ) * 28.0f;
		return XMVectorSet( x, y, z, 1.0f );
	}

	XMVECTOR BuildCameraTarget()
	{
		return XMVectorSet( 0.0f, 94.0f, 180.0f, 1.0f );
	}

	XMMATRIX BuildViewProjection( uint32_t width, uint32_t height, XMVECTOR cameraPosition )
	{
		const XMMATRIX view = XMMatrixLookAtLH( cameraPosition, BuildCameraTarget(), XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f ) );
		const float aspectRatio = static_cast<float>( width ) / static_cast<float>( std::max<uint32_t>( height, 1u ) );
		const XMMATRIX projection = XMMatrixPerspectiveFovLH( kCameraFovRadians, aspectRatio, kCameraNear, kCameraFar );
		return XMMatrixMultiply( view, projection );
	}

	XMMATRIX BuildGroundWorldMatrix()
	{
		return XMMatrixIdentity();
	}

	XMFLOAT3 BuildWindDirection( float headingDegrees )
	{
		const float headingRadians = XMConvertToRadians( headingDegrees );
		XMFLOAT3 direction{};
		direction.x = std::cos( headingRadians );
		direction.y = 0.05f;
		direction.z = std::sin( headingRadians );

		const float length = std::sqrt( direction.x * direction.x + direction.y * direction.y + direction.z * direction.z );
		if( length > 1.0e-5f )
		{
			direction.x /= length;
			direction.y /= length;
			direction.z /= length;
		}
		return direction;
	}

	XMFLOAT3 BuildSunDirection( float elevationDegrees )
	{
		const float elevationRadians = XMConvertToRadians( elevationDegrees );
		XMFLOAT3 direction{};
		direction.x = 0.28f * std::cos( elevationRadians );
		direction.y = std::sin( elevationRadians );
		direction.z = 0.96f * std::cos( elevationRadians );

		const float length = std::sqrt( direction.x * direction.x + direction.y * direction.y + direction.z * direction.z );
		if( length > 1.0e-5f )
		{
			direction.x /= length;
			direction.y /= length;
			direction.z /= length;
		}
		return direction;
	}

	void GenerateNoiseTextures( ICommandBuffer& commandBuffer, AppState& app )
	{
		if( !app.noiseDirty )
		{
			return;
		}

		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();

		{
			LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloVolumetricClouds::ShapeNoise", 0xff90be6du );
			NoisePushConstants pushConstants{};
			pushConstants.outputTextureIndex = ctx.GetUnorderedAccessIndex( app.noise.shape );
			pushConstants.textureSize = kShapeNoiseSize;

			commandBuffer.CmdTransitionTexture( app.noise.shape, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
			commandBuffer.CmdBindComputePipeline( app.shapeNoisePipeline );
			commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			commandBuffer.CmdDispatch( ( kShapeNoiseSize + 3u ) / 4u, ( kShapeNoiseSize + 3u ) / 4u, ( kShapeNoiseSize + 3u ) / 4u );
			commandBuffer.CmdTransitionTexture( app.noise.shape, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		}

		{
			LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloVolumetricClouds::DetailNoise", 0xfff9c74fu );
			NoisePushConstants pushConstants{};
			pushConstants.outputTextureIndex = ctx.GetUnorderedAccessIndex( app.noise.detail );
			pushConstants.textureSize = kDetailNoiseSize;

			commandBuffer.CmdTransitionTexture( app.noise.detail, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
			commandBuffer.CmdBindComputePipeline( app.detailNoisePipeline );
			commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			commandBuffer.CmdDispatch( ( kDetailNoiseSize + 3u ) / 4u, ( kDetailNoiseSize + 3u ) / 4u, ( kDetailNoiseSize + 3u ) / 4u );
			commandBuffer.CmdTransitionTexture( app.noise.detail, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		}

		app.noiseDirty = false;
	}

	GroundPushConstants BuildGroundPushConstants( const AppState& app )
	{
		GroundPushConstants pushConstants{};
		const XMVECTOR cameraPosition = BuildCameraPosition( app.orbitAngle );
		const XMMATRIX world = BuildGroundWorldMatrix();
		const XMMATRIX worldViewProjection = XMMatrixMultiply( world, BuildViewProjection( app.deviceManager->GetWidth(), app.deviceManager->GetHeight(), cameraPosition ) );
		XMStoreFloat4x4( &pushConstants.world, world );
		XMStoreFloat4x4( &pushConstants.worldViewProj, worldViewProjection );
		pushConstants.groundTint = { 0.34f, 0.35f, 0.30f, 1.0f };
		return pushConstants;
	}

	CloudPushConstants BuildCloudPushConstants( const AppState& app )
	{
		CloudPushConstants pushConstants{};
		const XMVECTOR cameraPosition = BuildCameraPosition( app.orbitAngle );
		const XMMATRIX viewProjection = BuildViewProjection( app.deviceManager->GetWidth(), app.deviceManager->GetHeight(), cameraPosition );
		const XMMATRIX inverseViewProjection = XMMatrixInverse( nullptr, viewProjection );
		XMStoreFloat4x4( &pushConstants.invViewProjection, inverseViewProjection );

		XMFLOAT3 cameraPositionFloat3{};
		XMStoreFloat3( &cameraPositionFloat3, cameraPosition );
		pushConstants.cameraPositionAndTime = { cameraPositionFloat3.x, cameraPositionFloat3.y, cameraPositionFloat3.z, app.animationTime };
		pushConstants.planetCenterAndRadius = { 0.0f, -app.planetRadius, 0.0f, app.planetRadius };
		pushConstants.cloudLayer = { app.cloudMinHeight, app.cloudMaxHeight, app.cloudCoverage, static_cast<float>( app.maxSteps ) };

		const XMFLOAT3 windDirection = BuildWindDirection( app.windHeadingDegrees );
		pushConstants.windDirectionAndSpeed = { windDirection.x, windDirection.y, windDirection.z, app.windSpeed };
		pushConstants.noiseSampling = { app.shapeNoiseScale, app.detailNoiseScale, app.detailNoiseModifier, app.turbulenceAmount };
		pushConstants.lighting0 = { app.lightStepLength, app.lightConeRadius, app.precipitation, app.ambientLightFactor };

		const XMFLOAT3 sunDirection = BuildSunDirection( app.sunElevationDegrees );
		pushConstants.sunDirectionAndLightFactor = { sunDirection.x, sunDirection.y, sunDirection.z, app.sunLightFactor };
		pushConstants.colors0 = { 1.0f, 0.92f, 0.84f, app.exposure };
		pushConstants.colors1 = { 0.35f, 0.39f, 0.46f, app.windShearOffset };
		pushConstants.colors2 = { 0.93f, 0.95f, 0.99f, 0.0f };
		pushConstants.textureIndices[ 0 ] = app.deviceManager->GetRenderDevice()->GetBindlessIndex( app.noise.shape );
		pushConstants.textureIndices[ 1 ] = app.deviceManager->GetRenderDevice()->GetBindlessIndex( app.noise.detail );
		pushConstants.textureIndices[ 2 ] = static_cast<uint32_t>( app.animationTime * 60.0f );
		pushConstants.textureIndices[ 3 ] = 0u;
		return pushConstants;
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
		const std::filesystem::path sampleDirectory = std::filesystem::path( __FILE__ ).parent_path();
		LightHLSLLoader::SetRootDirectory( sampleDirectory );

		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = L"LightD3D12HelloVolumetricCloudsWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1440;
		constexpr uint32_t kInitialHeight = 900;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Volumetric Clouds",
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
		app.shapeNoisePipeline = CreateShapeNoisePipeline( ctx );
		app.detailNoisePipeline = CreateDetailNoisePipeline( ctx );
		app.groundPipeline = CreateGroundPipeline( ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT );
		app.cloudPipeline = CreateCloudPipeline( ctx, contextDesc.swapchainFormat );
		app.noise.shape = CreateNoiseTexture( ctx, "HelloVolumetricClouds Shape Noise", kShapeNoiseSize );
		app.noise.detail = CreateNoiseTexture( ctx, "HelloVolumetricClouds Detail Noise", kDetailNoiseSize );
		app.groundPlane = CreateGroundPlaneModel( ctx );
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

			if( !app.running || app.minimized )
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

			if( !app.pauseAnimation )
			{
				app.animationTime += deltaSeconds;
				app.orbitAngle += deltaSeconds * 0.16f;
			}

			app.imguiRenderer->NewFrame();
			ImGui::SetNextWindowPos( ImVec2( 20.0f, 20.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowSize( ImVec2( 390.0f, 0.0f ), ImGuiCond_FirstUseEver );
			ImGui::Begin( "Volumetric Clouds" );
			ImGui::TextWrapped( "Ground plane imported from the original repo OBJ, then a DX12 fullscreen raymarch adds clouds on top using GPU-generated 3D noise." );
			ImGui::Separator();
			ImGui::Checkbox( "Pause animation", &app.pauseAnimation );
			ImGui::SliderFloat( "Sun elevation", &app.sunElevationDegrees, -20.0f, 65.0f, "%.1f deg" );
			ImGui::SliderFloat( "Cloud coverage", &app.cloudCoverage, 0.20f, 0.92f );
			ImGui::SliderFloat( "Wind heading", &app.windHeadingDegrees, -180.0f, 180.0f, "%.0f deg" );
			ImGui::SliderFloat( "Wind speed", &app.windSpeed, 0.0f, 30.0f, "%.1f" );
			ImGui::SliderInt( "Max steps", &app.maxSteps, 32, 160 );
			ImGui::SliderFloat( "Exposure", &app.exposure, 0.7f, 2.5f );
			ImGui::Text( "Ground mesh: %u vertices / %u indices", app.groundPlane.vertexCount, app.groundPlane.indexCount );
			ImGui::Text( "Shape noise: %u^3", kShapeNoiseSize );
			ImGui::Text( "Detail noise: %u^3", kDetailNoiseSize );
			ImGui::Text( "FPS: %.1f (%.2f ms)", app.smoothedFps, app.smoothedFrameMs );
			ImGui::End();

			auto& commandBuffer = ctx.AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx.GetCurrentSwapchainTexture();

			GenerateNoiseTextures( commandBuffer, app );

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloVolumetricClouds::GroundPass", 0xff4cc9f0u );

				RenderPass renderPass{};
				renderPass.color[ 0 ].loadOp = LoadOp::Clear;
				renderPass.color[ 0 ].clearColor = { 0.05f, 0.08f, 0.11f, 1.0f };
				renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
				renderPass.depthStencil.depthStoreOp = StoreOp::Store;
				renderPass.depthStencil.clearDepth = 1.0f;

				Framebuffer framebuffer{};
				framebuffer.color[ 0 ].texture = currentTexture;
				framebuffer.depthStencil.texture = app.depthTarget.texture;

				const GroundPushConstants pushConstants = BuildGroundPushConstants( app );

				commandBuffer.CmdBeginRendering( renderPass, framebuffer );
				commandBuffer.CmdBindRenderPipeline( app.groundPipeline );
				commandBuffer.CmdBindVertexBuffer( app.groundPlane.vertexBuffer, sizeof( MeshVertex ) );
				commandBuffer.CmdBindIndexBuffer( app.groundPlane.indexBuffer, DXGI_FORMAT_R32_UINT );
				commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
				commandBuffer.CmdDrawIndexed( app.groundPlane.indexCount );
				commandBuffer.CmdEndRendering();
			}

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloVolumetricClouds::CloudPass", 0xff90be6du );

				RenderPass renderPass{};
				renderPass.color[ 0 ].loadOp = LoadOp::Load;

				Framebuffer framebuffer{};
				framebuffer.color[ 0 ].texture = currentTexture;

				const CloudPushConstants pushConstants = BuildCloudPushConstants( app );

				commandBuffer.CmdBeginRendering( renderPass, framebuffer );
				commandBuffer.CmdBindRenderPipeline( app.cloudPipeline );
				commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
				commandBuffer.CmdDraw( 3 );
				commandBuffer.CmdEndRendering();
			}

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloVolumetricClouds::ImguiPass", 0xfff9844au );

				RenderPass renderPass{};
				renderPass.color[ 0 ].loadOp = LoadOp::Load;

				Framebuffer framebuffer{};
				framebuffer.color[ 0 ].texture = currentTexture;

				commandBuffer.CmdBeginRendering( renderPass, framebuffer );
				app.imguiRenderer->Render( commandBuffer );
				commandBuffer.CmdEndRendering();
			}

			ctx.Submit( commandBuffer, currentTexture );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		ctx.WaitIdle();
		DestroyNoiseResources( ctx, app.noise );
		DestroyDepthTarget( ctx, app.depthTarget );
		DestroyModel( ctx, app.groundPlane );
		app.groundPipeline = {};
		app.cloudPipeline = {};
		app.shapeNoisePipeline = {};
		app.detailNoisePipeline = {};
		app.imguiRenderer.reset();
		app.deviceManager.reset();
		DestroyWindow( hwnd );
	}
	catch( const std::exception& exception )
	{
		MessageBoxA( nullptr, exception.what(), "LightD3D12 Hello Volumetric Clouds", MB_ICONERROR | MB_OK );
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
