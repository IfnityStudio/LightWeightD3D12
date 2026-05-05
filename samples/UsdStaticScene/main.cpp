#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <DirectXMath.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/vt/array.h>
#include <pxr/pxr.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/sphere.h>


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

PXR_NAMESPACE_USING_DIRECTIVE

using namespace DirectX;
using namespace lightd3d12;

namespace
{
	struct SceneVertex
	{
		XMFLOAT3 position = {};
		XMFLOAT3 normal = {};
		XMFLOAT3 color = {};
	};

	struct ScenePushConstants
	{
		XMFLOAT4X4 worldViewProjection = {};
		XMFLOAT3 lightDirection = { -0.45f, 0.85f, -0.25f };
		float ambient = 0.16f;
	};

	static_assert( sizeof( ScenePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct MeshGeometry
	{
		BufferHandle vertexBuffer = {};
		BufferHandle indexBuffer = {};
		uint32_t indexCount = 0;
		uint32_t vertexCount = 0;
	};

	struct DepthTarget
	{
		TextureHandle texture = {};
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct ScenePrimInfo
	{
		std::string path;
		std::string type;
		uint32_t depth = 0;
		bool drawable = false;
	};

	struct LoadedUsdScene
	{
		std::vector<SceneVertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<ScenePrimInfo> prims;
		uint32_t cubeCount = 0;
		uint32_t sphereCount = 0;
		uint32_t meshCount = 0;
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState pipeline = {};
		RenderPipelineState wireframePipeline = {};
		MeshGeometry geometry = {};
		DepthTarget depth = {};
		std::filesystem::path scenePath;
		std::vector<ScenePrimInfo> prims;
		uint32_t cubeCount = 0;
		uint32_t sphereCount = 0;
		uint32_t meshCount = 0;
		float cameraYaw = 35.0f;
		float cameraPitch = 18.0f;
		float cameraDistance = 6.2f;
		bool showWireframe = false;
		bool showHierarchy = true;
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

	std::filesystem::path GetExecutableDirectory()
	{
		std::wstring buffer;
		buffer.resize( MAX_PATH );

		for( ;; )
		{
			const DWORD length = GetModuleFileNameW( nullptr, buffer.data(), static_cast<DWORD>( buffer.size() ) );
			if( length == 0 )
			{
				throw std::runtime_error( "Failed to locate executable directory." );
			}

			if( length < buffer.size() )
			{
				buffer.resize( length );
				return std::filesystem::path( buffer ).parent_path();
			}

			buffer.resize( buffer.size() * 2 );
		}
	}

	void RegisterOpenUsdPlugins()
	{
		const std::filesystem::path pluginRoot = GetExecutableDirectory() / "usd";
		if( !std::filesystem::exists( pluginRoot ) )
		{
			throw std::runtime_error( "OpenUSD plugin folder was not found next to the executable." );
		}

		std::vector<std::string> plugInfoFiles;
		for( const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator( pluginRoot ) )
		{
			if( entry.is_regular_file() && entry.path().filename() == "plugInfo.json" )
			{
				plugInfoFiles.push_back( entry.path().string() );
			}
		}

		if( plugInfoFiles.empty() )
		{
			throw std::runtime_error( "OpenUSD plugInfo.json files were not found next to the executable." );
		}

		PlugRegistry::GetInstance().RegisterPlugins( plugInfoFiles );
	}

	uint32_t GetUsdPathDepth( const std::string& path )
	{
		const auto separators = static_cast<uint32_t>( std::count( path.begin(), path.end(), '/' ) );
		return separators > 0 ? separators - 1u : 0u;
	}

	ScenePrimInfo GetPrimInfo( const UsdPrim& prim )
	{
		const std::string type = prim.GetTypeName().GetString();
		return ScenePrimInfo{
			.path = prim.GetPath().GetString(),
			.type = type.empty() ? "typeless" : type,
			.depth = GetUsdPathDepth( prim.GetPath().GetString() ),
			.drawable = prim.IsA<UsdGeomCube>() || prim.IsA<UsdGeomSphere>() || prim.IsA<UsdGeomMesh>(),
		};
	}

	XMFLOAT3 ToFloat3( const GfVec3d& value )
	{
		return { static_cast<float>( value[ 0 ] ), static_cast<float>( value[ 1 ] ), static_cast<float>( value[ 2 ] ) };
	}

	XMFLOAT3 ToFloat3( const GfVec3f& value )
	{
		return { value[ 0 ], value[ 1 ], value[ 2 ] };
	}

	GfVec3d TransformPoint( const GfMatrix4d& transform, const GfVec3d& point )
	{
		return transform.Transform( point );
	}

	GfVec3d TransformNormal( const GfMatrix4d& transform, const GfVec3d& normal )
	{
		GfMatrix4d normalMatrix = transform.GetInverse().GetTranspose();
		GfVec3d result = normalMatrix.TransformDir( normal );
		result.Normalize();
		return result;
	}

	GfMatrix4d GetLocalToWorld( const UsdPrim& prim )
	{
		UsdGeomXformable xformable( prim );
		if( xformable )
		{
			return xformable.ComputeLocalToWorldTransform( UsdTimeCode::Default() );
		}
		return GfMatrix4d( 1.0 );
	}

	XMFLOAT3 GetDisplayColor( const UsdPrim& prim )
	{
		UsdGeomGprim gprim( prim );
		VtArray<GfVec3f> colors;
		if( gprim && gprim.GetDisplayColorAttr().Get( &colors ) && !colors.empty() )
		{
			return ToFloat3( colors.front() );
		}
		return { 0.82f, 0.82f, 0.82f };
	}

	uint32_t AddVertex( LoadedUsdScene& scene, const GfVec3d& position, const GfVec3d& normal, const XMFLOAT3& color )
	{
		scene.vertices.push_back( SceneVertex{ ToFloat3( position ), ToFloat3( normal ), color } );
		return static_cast<uint32_t>( scene.vertices.size() - 1 );
	}

	void AddTriangle(
		LoadedUsdScene& scene,
		const GfVec3d& p0,
		const GfVec3d& p1,
		const GfVec3d& p2,
		const GfVec3d& n0,
		const GfVec3d& n1,
		const GfVec3d& n2,
		const XMFLOAT3& color )
	{
		scene.indices.push_back( AddVertex( scene, p0, n0, color ) );
		scene.indices.push_back( AddVertex( scene, p1, n1, color ) );
		scene.indices.push_back( AddVertex( scene, p2, n2, color ) );
	}

	void AddQuad(
		LoadedUsdScene& scene,
		const std::array<GfVec3d, 4>& positions,
		const GfVec3d& normal,
		const XMFLOAT3& color,
		const GfMatrix4d& transform )
	{
		const GfVec3d n = TransformNormal( transform, normal );
		std::array<GfVec3d, 4> p = {};
		for( uint32_t i = 0; i != 4; ++i )
		{
			p[ i ] = TransformPoint( transform, positions[ i ] );
		}

		AddTriangle( scene, p[ 0 ], p[ 1 ], p[ 2 ], n, n, n, color );
		AddTriangle( scene, p[ 0 ], p[ 2 ], p[ 3 ], n, n, n, color );
	}

	void AppendUsdCube( LoadedUsdScene& scene, const UsdGeomCube& cube )
	{
		double size = 2.0;
		cube.GetSizeAttr().Get( &size );
		const double half = size * 0.5;
		const GfMatrix4d transform = GetLocalToWorld( cube.GetPrim() );
		const XMFLOAT3 color = GetDisplayColor( cube.GetPrim() );

		AddQuad( scene, { GfVec3d{ +half, -half, +half }, GfVec3d{ +half, +half, +half }, GfVec3d{ +half, +half, -half }, GfVec3d{ +half, -half, -half } }, { +1, 0, 0 }, color, transform );
		AddQuad( scene, { GfVec3d{ -half, -half, -half }, GfVec3d{ -half, +half, -half }, GfVec3d{ -half, +half, +half }, GfVec3d{ -half, -half, +half } }, { -1, 0, 0 }, color, transform );
		AddQuad( scene, { GfVec3d{ -half, +half, +half }, GfVec3d{ -half, +half, -half }, GfVec3d{ +half, +half, -half }, GfVec3d{ +half, +half, +half } }, { 0, +1, 0 }, color, transform );
		AddQuad( scene, { GfVec3d{ -half, -half, -half }, GfVec3d{ -half, -half, +half }, GfVec3d{ +half, -half, +half }, GfVec3d{ +half, -half, -half } }, { 0, -1, 0 }, color, transform );
		AddQuad( scene, { GfVec3d{ -half, -half, +half }, GfVec3d{ -half, +half, +half }, GfVec3d{ +half, +half, +half }, GfVec3d{ +half, -half, +half } }, { 0, 0, +1 }, color, transform );
		AddQuad( scene, { GfVec3d{ +half, -half, -half }, GfVec3d{ +half, +half, -half }, GfVec3d{ -half, +half, -half }, GfVec3d{ -half, -half, -half } }, { 0, 0, -1 }, color, transform );
		++scene.cubeCount;
	}

	void AppendUsdSphere( LoadedUsdScene& scene, const UsdGeomSphere& sphere )
	{
		double radius = 1.0;
		sphere.GetRadiusAttr().Get( &radius );
		const GfMatrix4d transform = GetLocalToWorld( sphere.GetPrim() );
		const XMFLOAT3 color = GetDisplayColor( sphere.GetPrim() );
		constexpr uint32_t slices = 32;
		constexpr uint32_t stacks = 16;

		auto pointAt = [ radius ]( uint32_t stack, uint32_t slice ) -> GfVec3d
		{
			const double v = static_cast<double>( stack ) / static_cast<double>( stacks );
			const double phi = v * 3.14159265358979323846;
			const double y = std::cos( phi );
			const double r = std::sin( phi );
			const double u = static_cast<double>( slice ) / static_cast<double>( slices );
			const double theta = u * 6.28318530717958647692;
			return GfVec3d( std::cos( theta ) * r * radius, y * radius, std::sin( theta ) * r * radius );
		};

		for( uint32_t stack = 0; stack != stacks; ++stack )
		{
			for( uint32_t slice = 0; slice != slices; ++slice )
			{
				const GfVec3d local00 = pointAt( stack, slice );
				const GfVec3d local01 = pointAt( stack, slice + 1 );
				const GfVec3d local10 = pointAt( stack + 1, slice );
				const GfVec3d local11 = pointAt( stack + 1, slice + 1 );
				const GfVec3d p00 = TransformPoint( transform, local00 );
				const GfVec3d p01 = TransformPoint( transform, local01 );
				const GfVec3d p10 = TransformPoint( transform, local10 );
				const GfVec3d p11 = TransformPoint( transform, local11 );
				const GfVec3d n00 = TransformNormal( transform, local00.GetNormalized() );
				const GfVec3d n01 = TransformNormal( transform, local01.GetNormalized() );
				const GfVec3d n10 = TransformNormal( transform, local10.GetNormalized() );
				const GfVec3d n11 = TransformNormal( transform, local11.GetNormalized() );

				AddTriangle( scene, p00, p10, p11, n00, n10, n11, color );
				AddTriangle( scene, p00, p11, p01, n00, n11, n01, color );
			}
		}
		++scene.sphereCount;
	}

	void AppendUsdMesh( LoadedUsdScene& scene, const UsdGeomMesh& mesh )
	{
		VtArray<GfVec3f> points;
		VtArray<int> faceVertexCounts;
		VtArray<int> faceVertexIndices;
		if( !mesh.GetPointsAttr().Get( &points ) || !mesh.GetFaceVertexCountsAttr().Get( &faceVertexCounts ) ||
			!mesh.GetFaceVertexIndicesAttr().Get( &faceVertexIndices ) )
		{
			return;
		}

		const GfMatrix4d transform = GetLocalToWorld( mesh.GetPrim() );
		const XMFLOAT3 color = GetDisplayColor( mesh.GetPrim() );
		size_t cursor = 0;
		for( int count : faceVertexCounts )
		{
			if( count < 3 || cursor + static_cast<size_t>( count ) > faceVertexIndices.size() )
			{
				cursor += std::max( count, 0 );
				continue;
			}

			for( int i = 1; i + 1 < count; ++i )
			{
				const int i0 = faceVertexIndices[ cursor ];
				const int i1 = faceVertexIndices[ cursor + static_cast<size_t>( i ) ];
				const int i2 = faceVertexIndices[ cursor + static_cast<size_t>( i + 1 ) ];
				if( i0 < 0 || i1 < 0 || i2 < 0 || i0 >= static_cast<int>( points.size() ) || i1 >= static_cast<int>( points.size() ) ||
					i2 >= static_cast<int>( points.size() ) )
				{
					continue;
				}

				const GfVec3d p0 = TransformPoint( transform, GfVec3d( points[ i0 ] ) );
				const GfVec3d p1 = TransformPoint( transform, GfVec3d( points[ i1 ] ) );
				const GfVec3d p2 = TransformPoint( transform, GfVec3d( points[ i2 ] ) );
				GfVec3d faceNormal = GfCross( p1 - p0, p2 - p0 );
				faceNormal.Normalize();
				AddTriangle( scene, p0, p1, p2, faceNormal, faceNormal, faceNormal, color );
			}
			cursor += static_cast<size_t>( count );
		}
		++scene.meshCount;
	}

	LoadedUsdScene LoadUsdScene( const std::filesystem::path& path )
	{
		UsdStageRefPtr stage = UsdStage::Open( path.string() );
		if( !stage )
		{
			throw std::runtime_error( "OpenUSD could not open the scene file." );
		}

		LoadedUsdScene scene;
		for( const UsdPrim& prim : stage->Traverse() )
		{
			scene.prims.push_back( GetPrimInfo( prim ) );

			if( prim.IsA<UsdGeomCube>() )
			{
				AppendUsdCube( scene, UsdGeomCube( prim ) );
			}
			else if( prim.IsA<UsdGeomSphere>() )
			{
				AppendUsdSphere( scene, UsdGeomSphere( prim ) );
			}
			else if( prim.IsA<UsdGeomMesh>() )
			{
				AppendUsdMesh( scene, UsdGeomMesh( prim ) );
			}
		}

		if( scene.indices.empty() )
		{
			throw std::runtime_error( "OpenUSD scene has no supported Cube, Sphere, or Mesh prims." );
		}
		return scene;
	}

	MeshGeometry CreateSceneGeometry( RenderDevice& ctx, const LoadedUsdScene& scene )
	{
		BufferDesc vertexDesc{};
		vertexDesc.debugName = "UsdStaticScene Vertex Buffer";
		vertexDesc.size = sizeof( SceneVertex ) * scene.vertices.size();
		vertexDesc.stride = sizeof( SceneVertex );
		vertexDesc.bufferType = BufferDesc::BufferType::VertexBuffer;
		vertexDesc.data = scene.vertices.data();
		vertexDesc.dataSize = vertexDesc.size;

		BufferDesc indexDesc{};
		indexDesc.debugName = "UsdStaticScene Index Buffer";
		indexDesc.size = sizeof( uint32_t ) * scene.indices.size();
		indexDesc.bufferType = BufferDesc::BufferType::IndexBuffer;
		indexDesc.data = scene.indices.data();
		indexDesc.dataSize = indexDesc.size;

		MeshGeometry geometry{};
		geometry.vertexBuffer = ctx.CreateBuffer( vertexDesc );
		geometry.indexBuffer = ctx.CreateBuffer( indexDesc );
		geometry.indexCount = static_cast<uint32_t>( scene.indices.size() );
		geometry.vertexCount = static_cast<uint32_t>( scene.vertices.size() );
		return geometry;
	}

	RenderPipelineState CreateScenePipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, bool wireframe )
	{
		static constexpr char vertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
	row_major float4x4 gWorldViewProjection;
	float3 gLightDirection;
	float gAmbient;
};

struct VSInput
{
	float3 position : POSITION0;
	float3 normal : NORMAL0;
	float3 color : COLOR0;
};

struct VSOutput
{
	float4 position : SV_Position;
	float3 normal : NORMAL0;
	float3 color : COLOR0;
};

VSOutput main(VSInput input)
{
	VSOutput output;
	output.position = mul(float4(input.position, 1.0f), gWorldViewProjection);
	output.normal = normalize(input.normal);
	output.color = input.color;
	return output;
}
)";

		static constexpr char pixelShader[] = R"(
cbuffer PushConstants : register(b0)
{
	row_major float4x4 gWorldViewProjection;
	float3 gLightDirection;
	float gAmbient;
};

float4 main(float4 position : SV_Position, float3 normal : NORMAL0, float3 color : COLOR0) : SV_Target0
{
	float3 n = normalize(normal);
	float3 l = normalize(gLightDirection);
	float ndotl = saturate(dot(n, l));
	float rim = pow(1.0f - saturate(abs(n.z)), 2.0f) * 0.08f;
	return float4(color * (gAmbient + ndotl * 0.84f + rim), 1.0f);
}
)";

		static constexpr char wirePixelShader[] = R"(
float4 main(float4 position : SV_Position, float3 normal : NORMAL0, float3 color : COLOR0) : SV_Target0
{
	return float4(0.02f, 0.025f, 0.03f, 1.0f);
}
)";

		RenderPipelineDesc desc{};
		desc.inputElements =
		{
			VertexInputElementDesc{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			VertexInputElementDesc{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			VertexInputElementDesc{ "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
		desc.vertexShader = { vertexShader, "main", "vs_6_6" };
		desc.fragmentShader = { wireframe ? wirePixelShader : pixelShader, "main", "ps_6_6" };
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
		desc.debugName = "UsdStaticScene Depth";
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
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = L"LightD3D12UsdStaticSceneWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t initialWidth = 1280;
		constexpr uint32_t initialHeight = 720;
		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 OpenUSD Static Scene",
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
		app.scenePath = std::filesystem::path( __FILE__ ).parent_path() / "scene.usd";
		SetWindowLongPtr( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( &app ) );

		ContextDesc contextDesc{};
		contextDesc.enableDebugLayer = true;
		contextDesc.swapchainBufferCount = 3;

		SwapchainDesc swapchainDesc{};
		swapchainDesc.window = MakeWin32WindowHandle( hwnd );
		swapchainDesc.width = initialWidth;
		swapchainDesc.height = initialHeight;
		swapchainDesc.vsync = true;

		app.deviceManager = std::make_unique<DeviceManager>( contextDesc, swapchainDesc );
		app.imguiRenderer = std::make_unique<ImguiRenderer>( *app.deviceManager, swapchainDesc.window );
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();

		RegisterOpenUsdPlugins();
		const LoadedUsdScene scene = LoadUsdScene( app.scenePath );
		app.prims = scene.prims;
		app.cubeCount = scene.cubeCount;
		app.sphereCount = scene.sphereCount;
		app.meshCount = scene.meshCount;
		app.geometry = CreateSceneGeometry( ctx, scene );
		app.pipeline = CreateScenePipeline( ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, false );
		app.wireframePipeline = CreateScenePipeline( ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT, true );
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
			app.smoothedFrameMs = std::lerp( app.smoothedFrameMs, deltaSeconds * 1000.0f, 0.08f );
			app.smoothedFps = 1000.0f / std::max( app.smoothedFrameMs, 0.001f );

			app.imguiRenderer->NewFrame();
			ImGui::SetNextWindowPos( ImVec2( 18.0f, 18.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowSize( ImVec2( 390.0f, 0.0f ), ImGuiCond_FirstUseEver );
			ImGui::Begin( "OpenUSD Static Scene" );
			ImGui::TextWrapped( "This USD has no scene lights. It demonstrates composition: parent Xforms move child Cubes/Spheres before they become D3D12 buffers." );
			ImGui::Separator();
			ImGui::TextWrapped( "File: %s", app.scenePath.string().c_str() );
			ImGui::Text( "USD prims: %u", static_cast<uint32_t>( app.prims.size() ) );
			ImGui::Text( "Cubes: %u", app.cubeCount );
			ImGui::Text( "Spheres: %u", app.sphereCount );
			ImGui::Text( "Meshes: %u", app.meshCount );
			ImGui::Text( "Vertices: %u", app.geometry.vertexCount );
			ImGui::Text( "Indices: %u", app.geometry.indexCount );
			ImGui::Separator();
			ImGui::Checkbox( "Wireframe overlay", &app.showWireframe );
			ImGui::SliderFloat( "Camera yaw", &app.cameraYaw, -180.0f, 180.0f );
			ImGui::SliderFloat( "Camera pitch", &app.cameraPitch, -70.0f, 70.0f );
			ImGui::SliderFloat( "Camera distance", &app.cameraDistance, 2.5f, 12.0f );
			ImGui::Separator();
			ImGui::Checkbox( "Show USD hierarchy", &app.showHierarchy );
			if( app.showHierarchy )
			{
				ImGui::BeginChild( "UsdHierarchy", ImVec2( 0.0f, 260.0f ), true );
				for( const ScenePrimInfo& prim : app.prims )
				{
					ImGui::Indent( static_cast<float>( prim.depth ) * 14.0f );
					ImGui::Text( "%s %s (%s)", prim.drawable ? "[draw]" : "[xform]", prim.path.c_str(), prim.type.c_str() );
					ImGui::Unindent( static_cast<float>( prim.depth ) * 14.0f );
				}
				ImGui::EndChild();
			}
			ImGui::Text( "Frame: %.3f ms", app.smoothedFrameMs );
			ImGui::Text( "FPS: %.1f", app.smoothedFps );
			ImGui::End();

			const uint32_t width = app.deviceManager->GetWidth();
			const uint32_t height = app.deviceManager->GetHeight();
			const float aspect = static_cast<float>( width ) / static_cast<float>( std::max( 1u, height ) );
			const float yaw = XMConvertToRadians( app.cameraYaw );
			const float pitch = XMConvertToRadians( app.cameraPitch );
			const XMVECTOR eye = XMVectorSet(
				std::cos( pitch ) * std::sin( yaw ) * app.cameraDistance,
				std::sin( pitch ) * app.cameraDistance,
				std::cos( pitch ) * std::cos( yaw ) * app.cameraDistance,
				1.0f );
			const XMMATRIX view = XMMatrixLookAtLH( eye, XMVectorZero(), XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f ) );
			const XMMATRIX projection = XMMatrixPerspectiveFovLH( XMConvertToRadians( 48.0f ), aspect, 0.1f, 100.0f );

			ScenePushConstants pushConstants{};
			XMStoreFloat4x4( &pushConstants.worldViewProjection, view * projection );

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.045f, 0.052f, 0.070f, 1.0f };
			renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
			renderPass.depthStencil.depthStoreOp = StoreOp::Store;
			renderPass.depthStencil.clearDepth = 1.0f;

			const TextureHandle currentTexture = renderDevice->GetCurrentSwapchainTexture();
			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;
			framebuffer.depthStencil.texture = app.depth.texture;

			ICommandBuffer& commandBuffer = renderDevice->AcquireCommandBuffer();
			commandBuffer.CmdBeginRendering( renderPass, framebuffer );
			commandBuffer.CmdPushDebugGroupLabel( "OpenUSD Static Scene", 0xff58c4dd );
			commandBuffer.CmdBindRenderPipeline( app.pipeline );
			commandBuffer.CmdBindVertexBuffer( app.geometry.vertexBuffer, sizeof( SceneVertex ) );
			commandBuffer.CmdBindIndexBuffer( app.geometry.indexBuffer, DXGI_FORMAT_R32_UINT );
			commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			commandBuffer.CmdDrawIndexed( app.geometry.indexCount );
			if( app.showWireframe )
			{
				commandBuffer.CmdBindRenderPipeline( app.wireframePipeline );
				commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
				commandBuffer.CmdDrawIndexed( app.geometry.indexCount );
			}
			commandBuffer.CmdPopDebugGroupLabel();
			app.imguiRenderer->Render( commandBuffer );
			commandBuffer.CmdEndRendering();
			renderDevice->Submit( commandBuffer, currentTexture );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		ctx.WaitIdle();
		if( app.geometry.vertexBuffer.Valid() )
		{
			ctx.Destroy( app.geometry.vertexBuffer );
		}
		if( app.geometry.indexBuffer.Valid() )
		{
			ctx.Destroy( app.geometry.indexBuffer );
		}
		DestroyDepthTarget( ctx, app.depth );
		app.imguiRenderer.reset();
		app.deviceManager.reset();
		if( IsWindow( hwnd ) != FALSE )
		{
			DestroyWindow( hwnd );
		}
		UnregisterClassW( windowClass.lpszClassName, instance );
		return 0;
	}
	catch( const std::exception& exception )
	{
		MessageBoxA( nullptr, exception.what(), "LightD3D12 OpenUSD Static Scene Error", MB_ICONERROR | MB_OK );
		return 1;
	}
}
