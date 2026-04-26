#include "LightD3D12/LightAssimpImporter.hpp"
#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"
#include "LightD3D12/LightHLSLLoader.hpp"

#include <imgui.h>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <chrono>
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
	struct ScenePushConstants
	{
		XMFLOAT4X4 worldViewProj = {};
		XMFLOAT4X4 world = {};
		std::array<float, 4> baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	static_assert( sizeof( ScenePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct MeshVertex
	{
		std::array<float, 3> position = {};
		std::array<float, 3> normal = { 0.0f, 1.0f, 0.0f };
	};

	static_assert( sizeof( MeshVertex ) == sizeof( float ) * 6 );

	struct MeshRange
	{
		std::string name;
		uint32_t firstIndex = 0;
		uint32_t indexCount = 0;
	};

	struct LoadedModel
	{
		BufferHandle vertexBuffer = {};
		BufferHandle indexBuffer = {};
		std::vector<MeshRange> meshRanges;
		std::string sourceFormatHint;
		uint32_t vertexCount = 0;
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
		RenderPipelineState solidPipeline;
		RenderPipelineState wireframePipeline;
		LoadedModel model;
		DepthTarget depthTarget;
		bool running = true;
		bool minimized = false;
		bool pauseRotation = false;
		bool showWireframe = true;
		float animationTime = 0.0f;
		float rotationSpeed = 0.8f;
		float modelScale = 1.0f;
		float viewDistance = 2.6f;
		float verticalOffset = -0.15f;
		float smoothedFrameMs = 16.6f;
		float smoothedFps = 60.0f;
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

	LoadedModel CreateDuckModel( RenderDevice& ctx )
	{
		if( !LightAssimpImporter::IsAvailable() )
		{
			throw std::runtime_error( "Assimp is not enabled in this build." );
		}

		const std::filesystem::path sampleDirectory = std::filesystem::path( __FILE__ ).parent_path();
		const std::filesystem::path assetPath = sampleDirectory.parent_path().parent_path() / "data" / "rubber_duck" / "scene.gltf";
		if( !std::filesystem::exists( assetPath ) )
		{
			throw std::runtime_error( "Failed to find data/rubber_duck/scene.gltf." );
		}

		const ImportedScene importedScene = LightAssimpImporter::ImportScene( assetPath );
		if( importedScene.meshes.empty() )
		{
			throw std::runtime_error( "The imported duck scene does not contain meshes." );
		}

		std::vector<MeshVertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<MeshRange> meshRanges;

		for( size_t meshIndex = 0; meshIndex < importedScene.meshes.size(); ++meshIndex )
		{
			const ImportedMesh& importedMesh = importedScene.meshes[ meshIndex ];
			if( importedMesh.vertices.empty() || importedMesh.indices.empty() )
			{
				continue;
			}

			const uint32_t baseVertex = static_cast<uint32_t>( vertices.size() );
			MeshRange range{};
			range.name = importedMesh.name.empty() ? "Mesh " + std::to_string( meshIndex ) : importedMesh.name;
			range.firstIndex = static_cast<uint32_t>( indices.size() );

			for( const ImportedVertex& importedVertex : importedMesh.vertices )
			{
				MeshVertex vertex{};
				vertex.position = importedVertex.position;
				vertex.normal = NormalizeNormal( importedVertex.normal );
				vertices.push_back( vertex );
			}

			for( const uint32_t index : importedMesh.indices )
			{
				indices.push_back( baseVertex + index );
			}

			range.indexCount = static_cast<uint32_t>( indices.size() ) - range.firstIndex;
			meshRanges.push_back( std::move( range ) );
		}

		if( vertices.empty() || indices.empty() )
		{
			throw std::runtime_error( "The imported duck scene did not yield drawable geometry." );
		}

		std::array<float, 3> minBounds =
		{
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max()
		};
		std::array<float, 3> maxBounds =
		{
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max()
		};

		for( const MeshVertex& vertex : vertices )
		{
			for( size_t axis = 0; axis < 3; ++axis )
			{
				minBounds[ axis ] = std::min( minBounds[ axis ], vertex.position[ axis ] );
				maxBounds[ axis ] = std::max( maxBounds[ axis ], vertex.position[ axis ] );
			}
		}

		const std::array<float, 3> center =
		{
			0.5f * ( minBounds[ 0 ] + maxBounds[ 0 ] ),
			0.5f * ( minBounds[ 1 ] + maxBounds[ 1 ] ),
			0.5f * ( minBounds[ 2 ] + maxBounds[ 2 ] ),
		};
		const float extentX = maxBounds[ 0 ] - minBounds[ 0 ];
		const float extentY = maxBounds[ 1 ] - minBounds[ 1 ];
		const float extentZ = maxBounds[ 2 ] - minBounds[ 2 ];
		const float maxExtent = std::max( extentX, std::max( extentY, extentZ ) );
		const float normalizeScale = maxExtent > 1.0e-5f ? 1.8f / maxExtent : 1.0f;

		for( MeshVertex& vertex : vertices )
		{
			for( size_t axis = 0; axis < 3; ++axis )
			{
				vertex.position[ axis ] = ( vertex.position[ axis ] - center[ axis ] ) * normalizeScale;
			}
		}

		BufferDesc vertexBufferDesc{};
		vertexBufferDesc.debugName = "HelloAssimpDuck Vertex Buffer";
		vertexBufferDesc.size = static_cast<uint64_t>( vertices.size() * sizeof( MeshVertex ) );
		vertexBufferDesc.stride = sizeof( MeshVertex );
		vertexBufferDesc.bufferType = BufferDesc::BufferType::VertexBuffer;
		vertexBufferDesc.data = vertices.data();
		vertexBufferDesc.dataSize = vertexBufferDesc.size;

		BufferDesc indexBufferDesc{};
		indexBufferDesc.debugName = "HelloAssimpDuck Index Buffer";
		indexBufferDesc.size = static_cast<uint64_t>( indices.size() * sizeof( uint32_t ) );
		indexBufferDesc.bufferType = BufferDesc::BufferType::IndexBuffer;
		indexBufferDesc.data = indices.data();
		indexBufferDesc.dataSize = indexBufferDesc.size;

		LoadedModel model{};
		model.vertexBuffer = ctx.CreateBuffer( vertexBufferDesc );
		model.indexBuffer = ctx.CreateBuffer( indexBufferDesc );
		model.meshRanges = std::move( meshRanges );
		model.sourceFormatHint = importedScene.sourceFormatHint;
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

		model.meshRanges.clear();
		model.sourceFormatHint.clear();
		model.vertexCount = 0;
		model.indexCount = 0;
	}

	RenderPipelineState CreateDuckPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, bool wireframe )
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
			}
		};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/HelloAssimpDuckVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( wireframe ? "shaders/HelloAssimpDuckWireframePS.hlsl" : "shaders/HelloAssimpDuckSolidPS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = depthFormat;
		desc.rasterizerState.FillMode = wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = wireframe ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = wireframe ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_LESS;
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
		depthDesc.debugName = "HelloAssimpDuck Depth";
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
		windowClass.lpszClassName = L"LightD3D12HelloAssimpDuckWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1280;
		constexpr uint32_t kInitialHeight = 720;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Assimp Duck",
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
		app.solidPipeline = CreateDuckPipeline( ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, false );
		app.wireframePipeline = CreateDuckPipeline( ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, true );
		app.model = CreateDuckModel( ctx );
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
			if( !app.pauseRotation )
			{
				app.animationTime += deltaSeconds;
			}

			const float frameMs = deltaSeconds * 1000.0f;
			app.smoothedFrameMs = std::lerp( app.smoothedFrameMs, frameMs, 0.08f );
			app.smoothedFps = 1000.0f / std::max( app.smoothedFrameMs, 0.001f );

			app.imguiRenderer->NewFrame();
			ImGui::SetNextWindowPos( ImVec2( 18.0f, 18.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowSize( ImVec2( 420.0f, 0.0f ), ImGuiCond_FirstUseEver );
			ImGui::Begin( "Hello Assimp Duck" );
			ImGui::TextWrapped( "Classic input-assembler example: Assimp imports the mesh on CPU, LightD3D12 uploads a vertex buffer and an index buffer, and the sample renders with CmdDrawIndexed()." );
			ImGui::Separator();
			ImGui::Text( "Assimp: %s", LightAssimpImporter::IsAvailable() ? "enabled" : "disabled" );
			ImGui::Text( "Source format: %s", app.model.sourceFormatHint.empty() ? "unknown" : app.model.sourceFormatHint.c_str() );
			ImGui::Text( "Meshes: %u", static_cast<uint32_t>( app.model.meshRanges.size() ) );
			ImGui::Text( "Vertices: %u", app.model.vertexCount );
			ImGui::Text( "Indices: %u", app.model.indexCount );
			ImGui::Text( "FPS: %.1f", app.smoothedFps );
			ImGui::Text( "Frame time: %.2f ms", app.smoothedFrameMs );
			ImGui::Checkbox( "Pause rotation", &app.pauseRotation );
			ImGui::Checkbox( "Show wireframe overlay", &app.showWireframe );
			ImGui::SliderFloat( "Rotation speed", &app.rotationSpeed, 0.0f, 2.5f, "%.2f" );
			ImGui::SliderFloat( "Model scale", &app.modelScale, 0.4f, 2.5f, "%.2f" );
			ImGui::SliderFloat( "View distance", &app.viewDistance, 1.2f, 6.0f, "%.2f" );
			ImGui::SliderFloat( "Vertical offset", &app.verticalOffset, -1.2f, 1.2f, "%.2f" );
			if( ImGui::CollapsingHeader( "Mesh list", ImGuiTreeNodeFlags_DefaultOpen ) )
			{
				for( const MeshRange& meshRange : app.model.meshRanges )
				{
					ImGui::BulletText( "%s (%u indices)", meshRange.name.c_str(), meshRange.indexCount );
				}
			}
			ImGui::End();

			auto& commandBuffer = renderDevice->AcquireCommandBuffer();
			const TextureHandle currentTexture = renderDevice->GetCurrentSwapchainTexture();

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.95f, 0.96f, 0.98f, 1.0f };
			renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
			renderPass.depthStencil.clearDepth = 1.0f;

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;
			framebuffer.depthStencil.texture = app.depthTarget.texture;

			commandBuffer.CmdBeginRendering( renderPass, framebuffer );

			const float aspectRatio = static_cast<float>( app.deviceManager->GetWidth() ) / static_cast<float>( std::max<uint32_t>( 1u, app.deviceManager->GetHeight() ) );
			const XMMATRIX modelRotationX = XMMatrixRotationX( XMConvertToRadians( -90.0f ) );
			const XMMATRIX modelRotationY = XMMatrixRotationY( app.animationTime * app.rotationSpeed );
			const XMMATRIX modelScale = XMMatrixScaling( app.modelScale, app.modelScale, app.modelScale );
			const XMMATRIX modelTranslation = XMMatrixTranslation( 0.0f, app.verticalOffset, 0.0f );
			const XMMATRIX world = modelScale * modelRotationX * modelRotationY * modelTranslation;
			const XMMATRIX view = XMMatrixLookAtLH(
				XMVectorSet( 0.0f, 0.35f, -app.viewDistance, 1.0f ),
				XMVectorSet( 0.0f, 0.05f, 0.0f, 1.0f ),
				XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f ) );
			const XMMATRIX projection = XMMatrixPerspectiveFovLH( XMConvertToRadians( 45.0f ), aspectRatio, 0.1f, 100.0f );

			ScenePushConstants solidPush{};
			XMStoreFloat4x4( &solidPush.world, world );
			XMStoreFloat4x4( &solidPush.worldViewProj, world * view * projection );
			solidPush.baseColor = { 0.98f, 0.84f, 0.22f, 1.0f };

			commandBuffer.CmdBindVertexBuffer( app.model.vertexBuffer, sizeof( MeshVertex ) );
			commandBuffer.CmdBindIndexBuffer( app.model.indexBuffer, DXGI_FORMAT_R32_UINT );
			commandBuffer.CmdBindRenderPipeline( app.solidPipeline );
			commandBuffer.CmdPushConstants( &solidPush, sizeof( solidPush ) );
			for( const MeshRange& meshRange : app.model.meshRanges )
			{
				commandBuffer.CmdDrawIndexed( meshRange.indexCount, 1, meshRange.firstIndex );
			}

			if( app.showWireframe )
			{
				ScenePushConstants wireframePush = solidPush;
				wireframePush.baseColor = { 0.04f, 0.05f, 0.07f, 1.0f };
				commandBuffer.CmdBindRenderPipeline( app.wireframePipeline );
				commandBuffer.CmdPushConstants( &wireframePush, sizeof( wireframePush ) );
				for( const MeshRange& meshRange : app.model.meshRanges )
				{
					commandBuffer.CmdDrawIndexed( meshRange.indexCount, 1, meshRange.firstIndex );
				}
			}

			app.imguiRenderer->Render( commandBuffer );
			commandBuffer.CmdEndRendering();
			renderDevice->Submit( commandBuffer, currentTexture );
		}

		ctx.WaitIdle();
		DestroyModel( ctx, app.model );
		DestroyDepthTarget( ctx, app.depthTarget );
		app.imguiRenderer.reset();
		app.deviceManager.reset();

		DestroyWindow( hwnd );
		UnregisterClassW( windowClass.lpszClassName, instance );
		return 0;
	}
	catch( const std::exception& exception )
	{
		MessageBoxA( nullptr, exception.what(), "LightD3D12 Hello Assimp Duck", MB_OK | MB_ICONERROR );
		return 1;
	}
}
