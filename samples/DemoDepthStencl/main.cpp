#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

using namespace DirectX;
using namespace lightd3d12;

namespace
{
	enum class DemoMode : uint32_t
	{
		Depth = 0,
		StencilMask = 1,
		StencilOutline = 2,
	};

	enum ObjectType : uint32_t
	{
		ObjectType_Plane = 0,
		ObjectType_Cube = 1,
	};

	enum VisualMode : uint32_t
	{
		VisualMode_Solid = 0,
		VisualMode_Stripes = 1,
		VisualMode_Checker = 2,
	};

	struct ScenePushConstants
	{
		std::array<float, 16> worldViewProjection = {};
		std::array<float, 16> world = {};
		std::array<float, 4> lightDirection = {};
		std::array<float, 4> objectColor = {};
		uint32_t objectType = 0;
		uint32_t visualMode = 0;
		float patternScale = 1.0f;
		float time = 0.0f;
	};

	static_assert( sizeof( ScenePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct DepthStencilTarget
	{
		TextureHandle texture = {};
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct OrbitCamera
	{
		XMFLOAT3 focus = { 0.0f, 1.3f, 0.0f };
		float yaw = 0.0f;
		float pitch = 0.28f;
		float distance = 11.5f;
		bool dragging = false;
		POINT lastMousePosition = {};
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState sceneDepthPipeline;
		RenderPipelineState sceneReadOnlyDepthPipeline;
		RenderPipelineState sceneNoDepthPipeline;
		RenderPipelineState stencilWriterPipeline;
		RenderPipelineState stencilInsidePipeline;
		RenderPipelineState stencilOutsidePipeline;
		RenderPipelineState outlineBasePipeline;
		RenderPipelineState outlineStencilPipeline;
		RenderPipelineState outlineNoStencilPipeline;
		DepthStencilTarget depthStencilTarget;
		OrbitCamera camera;
		DemoMode demoMode = DemoMode::Depth;
		bool running = true;
		bool minimized = false;
		bool pauseAnimation = false;
		bool depthTestEnabled = true;
		bool depthWriteEnabled = true;
		bool drawFrontCubeFirst = false;
		bool stencilEnabled = true;
		bool stencilDrawInsideMask = true;
		bool stencilShowMaskCard = true;
		bool outlineStencilEnabled = true;
		bool outlineShowExpandedCube = true;
		float animationTime = 0.0f;
		float cubeRotationSpeed = 0.9f;
	};

	XMFLOAT4X4 ToFloat4x4( FXMMATRIX matrix )
	{
		XMFLOAT4X4 value{};
		XMStoreFloat4x4( &value, matrix );
		return value;
	}

	std::array<float, 16> ToArray( const XMFLOAT4X4& matrix )
	{
		return {
			matrix._11, matrix._12, matrix._13, matrix._14,
			matrix._21, matrix._22, matrix._23, matrix._24,
			matrix._31, matrix._32, matrix._33, matrix._34,
			matrix._41, matrix._42, matrix._43, matrix._44,
		};
	}

	OrbitCamera CreateDefaultCamera( DemoMode mode )
	{
		OrbitCamera camera{};
		switch( mode )
		{
			case DemoMode::Depth:
				camera.focus = { 0.0f, 1.3f, 0.0f };
				camera.yaw = 0.0f;
				camera.pitch = 0.28f;
				camera.distance = 11.5f;
				break;

			case DemoMode::StencilMask:
				camera.focus = { 0.0f, 1.85f, 1.7f };
				camera.yaw = 0.0f;
				camera.pitch = 0.0f;
				camera.distance = 9.2f;
				break;

			case DemoMode::StencilOutline:
				camera.focus = { 0.0f, 1.6f, 0.5f };
				camera.yaw = 0.0f;
				camera.pitch = 0.08f;
				camera.distance = 8.6f;
				break;
		}

		return camera;
	}

	void ResetCameraForMode( AppState& app )
	{
		app.camera = CreateDefaultCamera( app.demoMode );
	}

	bool ImguiWantsMouseCapture()
	{
		if( ImGui::GetCurrentContext() == nullptr )
		{
			return false;
		}

		return ImGui::GetIO().WantCaptureMouse;
	}

	XMVECTOR ComputeCameraPosition( const OrbitCamera& camera )
	{
		const float cosPitch = std::cos( camera.pitch );
		const XMVECTOR forward = XMVectorSet(
			std::sin( camera.yaw ) * cosPitch,
			std::sin( camera.pitch ),
			std::cos( camera.yaw ) * cosPitch,
			0.0f );
		const XMVECTOR focus = XMLoadFloat3( &camera.focus );
		return XMVectorSubtract( focus, XMVectorScale( forward, camera.distance ) );
	}

	POINT GetMousePointFromLParam( LPARAM lParam )
	{
		POINT point{};
		point.x = static_cast<LONG>( static_cast<int16_t>( LOWORD( lParam ) ) );
		point.y = static_cast<LONG>( static_cast<int16_t>( HIWORD( lParam ) ) );
		return point;
	}

	struct PipelineConfig
	{
		bool depthEnable = true;
		D3D12_DEPTH_WRITE_MASK depthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		D3D12_COMPARISON_FUNC depthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		bool stencilEnable = false;
		D3D12_COMPARISON_FUNC stencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		D3D12_STENCIL_OP stencilPassOp = D3D12_STENCIL_OP_KEEP;
		uint8_t colorWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	};

	RenderPipelineState CreateScenePipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, const PipelineConfig& config )
	{
		static constexpr char ourVertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProjection;
    row_major float4x4 gWorld;
    float4 gLightDirection;
    float4 gObjectColor;
    uint gObjectType;
    uint gVisualMode;
    float gPatternScale;
    float gTime;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float3 color : COLOR0;
    uint visualMode : TEXCOORD2;
    float patternScale : TEXCOORD3;
    float time : TEXCOORD4;
};

void GetPlaneVertex(uint vertexID, out float3 position, out float3 normal)
{
    static const float3 positions[6] =
    {
        float3(-1.0, 0.0, -1.0),
        float3( 1.0, 0.0, -1.0),
        float3( 1.0, 0.0,  1.0),
        float3(-1.0, 0.0, -1.0),
        float3( 1.0, 0.0,  1.0),
        float3(-1.0, 0.0,  1.0)
    };

    position = positions[vertexID];
    normal = float3(0.0, 1.0, 0.0);
}

void GetCubeVertex(uint vertexID, out float3 position, out float3 normal)
{
    static const float3 positions[36] =
    {
        float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0),
        float3(-1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0), float3( 1.0, -1.0,  1.0),
        float3(-1.0, -1.0,  1.0), float3(-1.0,  1.0,  1.0), float3( 1.0,  1.0,  1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0, -1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0, -1.0), float3(-1.0, -1.0, -1.0),
        float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0),
        float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),
        float3( 1.0, -1.0, -1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0),
        float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0,  1.0), float3( 1.0,  1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3(-1.0, -1.0, -1.0), float3(-1.0,  1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3(-1.0,  1.0, -1.0), float3(-1.0,  1.0,  1.0)
    };

    static const float3 normals[36] =
    {
        float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0),
        float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0),
        float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0),
        float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0),
        float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0),
        float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0),
        float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0),
        float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0),
        float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0),
        float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0),
        float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0),
        float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0)
    };

    position = positions[vertexID];
    normal = normals[vertexID];
}

VSOutput main(uint vertexID : SV_VertexID)
{
    float3 localPosition;
    float3 localNormal;

    if (gObjectType == 0)
    {
        GetPlaneVertex(vertexID, localPosition, localNormal);
    }
    else
    {
        GetCubeVertex(vertexID, localPosition, localNormal);
    }

    const float4 worldPosition = mul(float4(localPosition, 1.0), gWorld);
    const float3 worldNormal = normalize(mul(float4(localNormal, 0.0), gWorld).xyz);

    VSOutput output;
    output.position = mul(float4(localPosition, 1.0), gWorldViewProjection);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = worldNormal;
    output.color = gObjectColor.rgb;
    output.visualMode = gVisualMode;
    output.patternScale = gPatternScale;
    output.time = gTime;
    return output;
}
)";

		static constexpr char ourPixelShader[] = R"(
float4 main(
    float4 position : SV_Position,
    float3 worldPosition : TEXCOORD0,
    float3 worldNormal : TEXCOORD1,
    float3 color : COLOR0,
    uint visualMode : TEXCOORD2,
    float patternScale : TEXCOORD3,
    float time : TEXCOORD4) : SV_Target0
{
    float3 baseColor = color;

    if (visualMode == 1)
    {
        const float stripe = 0.5 + 0.5 * sin((worldPosition.x + worldPosition.z) * patternScale + time * 3.0);
        baseColor = lerp(baseColor * 0.35, baseColor, stripe);
    }
    else if (visualMode == 2)
    {
        const float2 checkerUv = worldPosition.xz * patternScale;
        const float checker = fmod(floor(checkerUv.x) + floor(checkerUv.y), 2.0);
        baseColor *= checker > 0.5 ? 1.00 : 0.68;
    }

    const float3 normal = normalize(worldNormal);
    const float3 lightDirection = normalize(float3(0.35, 1.0, 0.20));
    const float lambert = 0.25 + 0.75 * saturate(dot(normal, lightDirection));
    return float4(baseColor * lambert, 1.0);
}
)";

		RenderPipelineDesc desc{};
		desc.vertexShader.source = ourVertexShader;
		desc.vertexShader.entryPoint = "main";
		desc.vertexShader.profile = "vs_6_6";
		desc.fragmentShader.source = ourPixelShader;
		desc.fragmentShader.entryPoint = "main";
		desc.fragmentShader.profile = "ps_6_6";
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = depthFormat;
		desc.rasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = config.depthEnable ? TRUE : FALSE;
		desc.depthStencilState.DepthWriteMask = config.depthWriteMask;
		desc.depthStencilState.DepthFunc = config.depthFunc;
		desc.depthStencilState.StencilEnable = config.stencilEnable ? TRUE : FALSE;
		desc.depthStencilState.StencilReadMask = 0xff;
		desc.depthStencilState.StencilWriteMask = 0xff;
		desc.depthStencilState.FrontFace.StencilFunc = config.stencilFunc;
		desc.depthStencilState.FrontFace.StencilPassOp = config.stencilPassOp;
		desc.depthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		desc.depthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		desc.depthStencilState.BackFace.StencilFunc = config.stencilFunc;
		desc.depthStencilState.BackFace.StencilPassOp = config.stencilPassOp;
		desc.depthStencilState.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		desc.depthStencilState.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		desc.blendState.RenderTarget[ 0 ].RenderTargetWriteMask = config.colorWriteMask;
		return ctx.CreateRenderPipeline( desc );
	}

	void DestroyDepthStencilTarget( RenderDevice& ctx, DepthStencilTarget& target )
	{
		if( target.texture.Valid() )
		{
			ctx.Destroy( target.texture );
			target.texture = {};
		}

		target.width = 0;
		target.height = 0;
	}

	void RecreateDepthStencilTarget( AppState& app )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const uint32_t width = app.deviceManager->GetWidth();
		const uint32_t height = app.deviceManager->GetHeight();
		if( app.depthStencilTarget.texture.Valid() && app.depthStencilTarget.width == width && app.depthStencilTarget.height == height )
		{
			return;
		}

		DestroyDepthStencilTarget( ctx, app.depthStencilTarget );

		TextureDesc depthDesc{};
		depthDesc.debugName = "DemoDepthStencl DepthStencil";
		depthDesc.width = width;
		depthDesc.height = height;
		depthDesc.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		depthDesc.usage = TextureUsage::DepthStencil;
		depthDesc.useClearValue = true;
		depthDesc.clearValue.Format = depthDesc.format;
		depthDesc.clearValue.DepthStencil.Depth = 1.0f;
		depthDesc.clearValue.DepthStencil.Stencil = 0;
		app.depthStencilTarget.texture = ctx.CreateTexture( depthDesc );
		app.depthStencilTarget.width = width;
		app.depthStencilTarget.height = height;
	}

	void RecordObject(
		ICommandBuffer& commandBuffer,
		const RenderPipelineState& pipeline,
		const XMMATRIX& worldMatrix,
		const XMMATRIX& viewProjection,
		const XMFLOAT3& lightDirection,
		ObjectType objectType,
		const std::array<float, 4>& color,
		VisualMode visualMode,
		float patternScale,
		float time )
	{
		ScenePushConstants pushConstants{};
		pushConstants.worldViewProjection = ToArray( ToFloat4x4( XMMatrixMultiply( worldMatrix, viewProjection ) ) );
		pushConstants.world = ToArray( ToFloat4x4( worldMatrix ) );
		pushConstants.lightDirection = { lightDirection.x, lightDirection.y, lightDirection.z, 0.0f };
		pushConstants.objectColor = color;
		pushConstants.objectType = static_cast<uint32_t>( objectType );
		pushConstants.visualMode = static_cast<uint32_t>( visualMode );
		pushConstants.patternScale = patternScale;
		pushConstants.time = time;

		commandBuffer.CmdBindRenderPipeline( pipeline );
		commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
		commandBuffer.CmdDraw( objectType == ObjectType_Plane ? 6u : 36u );
	}

	void RecordDepthLesson( AppState& app, ICommandBuffer& commandBuffer, const XMMATRIX& viewProjection, float time )
	{
		const XMFLOAT3 lightDirection = { -0.35f, -1.0f, -0.20f };
		const XMMATRIX floorWorld = XMMatrixScaling( 8.0f, 1.0f, 8.0f );
		const XMMATRIX frontCubeWorld =
			XMMatrixRotationY( time * 0.9f ) *
			XMMatrixTranslation( 0.0f, 1.15f, -0.7f );
		const XMMATRIX backCubeWorld =
			XMMatrixRotationY( -time * 0.55f ) *
			XMMatrixTranslation( 0.0f, 1.15f, 1.05f );

		const RenderPipelineState* cubePipeline = &app.sceneDepthPipeline;
		if( !app.depthTestEnabled )
		{
			cubePipeline = &app.sceneNoDepthPipeline;
		}
		else if( !app.depthWriteEnabled )
		{
			cubePipeline = &app.sceneReadOnlyDepthPipeline;
		}

		RecordObject(
			commandBuffer,
			app.sceneDepthPipeline,
			floorWorld,
			viewProjection,
			lightDirection,
			ObjectType_Plane,
			{ 0.72f, 0.76f, 0.82f, 1.0f },
			VisualMode_Checker,
			1.0f,
			time );

		if( app.drawFrontCubeFirst )
		{
			RecordObject(
				commandBuffer,
				*cubePipeline,
				frontCubeWorld,
				viewProjection,
				lightDirection,
				ObjectType_Cube,
				{ 0.95f, 0.48f, 0.21f, 1.0f },
				VisualMode_Solid,
				1.0f,
				time );
			RecordObject(
				commandBuffer,
				*cubePipeline,
				backCubeWorld,
				viewProjection,
				lightDirection,
				ObjectType_Cube,
				{ 0.20f, 0.55f, 0.98f, 1.0f },
				VisualMode_Solid,
				1.0f,
				time );
		}
		else
		{
			RecordObject(
				commandBuffer,
				*cubePipeline,
				backCubeWorld,
				viewProjection,
				lightDirection,
				ObjectType_Cube,
				{ 0.20f, 0.55f, 0.98f, 1.0f },
				VisualMode_Solid,
				1.0f,
				time );
			RecordObject(
				commandBuffer,
				*cubePipeline,
				frontCubeWorld,
				viewProjection,
				lightDirection,
				ObjectType_Cube,
				{ 0.95f, 0.48f, 0.21f, 1.0f },
				VisualMode_Solid,
				1.0f,
				time );
		}
	}

	std::string BuildStencilMaskExplanation( const AppState& app )
	{
		if( !app.stencilEnabled )
		{
			return "Stencil test desactivado: el cubo amarillo se dibuja entero. El marco blanco solo marca la zona donde el pass de escritura deja stencil distinto de cero.";
		}

		if( app.stencilDrawInsideMask )
		{
			return "Stencil test activo con compare NOT_EQUAL y referencia 0: solo ves la parte del cubo que cae dentro de la ventana central.";
		}

		return "Stencil test activo con compare EQUAL y referencia 0: ves el cubo fuera de la ventana, pero desaparece justo en el centro donde el stencil ya fue marcado.";
	}

	std::string BuildStencilOutlineExplanation( const AppState& app )
	{
		if( !app.outlineShowExpandedCube )
		{
			return "Solo ves el pass base: el cubo naranja escribe color y ademas deja stencil distinto de cero en sus pixeles visibles.";
		}

		if( !app.outlineStencilEnabled )
		{
			return "Stencil desactivado en el pass expandido: el cubo cian escalado invade toda la silueta, no solo el borde.";
		}

		return "Stencil activo en el pass expandido: los pixeles del cubo naranja ya marcaron stencil != 0, asi que el cubo cian solo sobrevive fuera de esa silueta y queda el contorno.";
	}

	void RecordStencilMaskLesson( AppState& app, ICommandBuffer& commandBuffer, const XMMATRIX& viewProjection, float time )
	{
		const XMFLOAT3 lightDirection = { -0.35f, -1.0f, -0.20f };
		const float maskCenterY = 1.85f;
		const float maskZ = 0.2f;
		const float windowHalfWidth = 0.95f;
		const float windowHalfHeight = 0.95f;
		const float frameThickness = 0.12f;

		const XMMATRIX maskPlaneWorld =
			XMMatrixScaling( windowHalfWidth, 1.0f, windowHalfHeight ) *
			XMMatrixRotationX( XM_PIDIV2 ) *
			XMMatrixTranslation( 0.0f, maskCenterY, maskZ );
		const XMMATRIX frameLeftWorld =
			XMMatrixScaling( frameThickness, 1.0f, windowHalfHeight + frameThickness ) *
			XMMatrixRotationX( XM_PIDIV2 ) *
			XMMatrixTranslation( -( windowHalfWidth + frameThickness ), maskCenterY, maskZ - 0.02f );
		const XMMATRIX frameRightWorld =
			XMMatrixScaling( frameThickness, 1.0f, windowHalfHeight + frameThickness ) *
			XMMatrixRotationX( XM_PIDIV2 ) *
			XMMatrixTranslation( windowHalfWidth + frameThickness, maskCenterY, maskZ - 0.02f );
		const XMMATRIX frameTopWorld =
			XMMatrixScaling( windowHalfWidth, 1.0f, frameThickness ) *
			XMMatrixRotationX( XM_PIDIV2 ) *
			XMMatrixTranslation( 0.0f, maskCenterY + windowHalfHeight + frameThickness, maskZ - 0.02f );
		const XMMATRIX frameBottomWorld =
			XMMatrixScaling( windowHalfWidth, 1.0f, frameThickness ) *
			XMMatrixRotationX( XM_PIDIV2 ) *
			XMMatrixTranslation( 0.0f, maskCenterY - windowHalfHeight - frameThickness, maskZ - 0.02f );
		const XMMATRIX maskedCubeWorld =
			XMMatrixScaling( 1.45f, 1.45f, 1.45f ) *
			XMMatrixRotationY( time * 0.85f ) *
			XMMatrixTranslation( 0.0f, maskCenterY, 2.4f );

		if( app.stencilShowMaskCard )
		{
			RecordObject(
				commandBuffer,
				app.sceneNoDepthPipeline,
				frameLeftWorld,
				viewProjection,
				lightDirection,
				ObjectType_Plane,
				{ 0.92f, 0.94f, 0.98f, 1.0f },
				VisualMode_Solid,
				1.0f,
				time );
			RecordObject(
				commandBuffer,
				app.sceneNoDepthPipeline,
				frameRightWorld,
				viewProjection,
				lightDirection,
				ObjectType_Plane,
				{ 0.92f, 0.94f, 0.98f, 1.0f },
				VisualMode_Solid,
				1.0f,
				time );
			RecordObject(
				commandBuffer,
				app.sceneNoDepthPipeline,
				frameTopWorld,
				viewProjection,
				lightDirection,
				ObjectType_Plane,
				{ 0.92f, 0.94f, 0.98f, 1.0f },
				VisualMode_Solid,
				1.0f,
				time );
			RecordObject(
				commandBuffer,
				app.sceneNoDepthPipeline,
				frameBottomWorld,
				viewProjection,
				lightDirection,
				ObjectType_Plane,
				{ 0.92f, 0.94f, 0.98f, 1.0f },
				VisualMode_Solid,
				1.0f,
				time );
		}

		RecordObject(
			commandBuffer,
			app.stencilWriterPipeline,
			maskPlaneWorld,
			viewProjection,
			lightDirection,
			ObjectType_Plane,
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			VisualMode_Solid,
			1.0f,
			time );

		RecordObject(
			commandBuffer,
			app.stencilEnabled ? ( app.stencilDrawInsideMask ? app.stencilInsidePipeline : app.stencilOutsidePipeline ) : app.sceneDepthPipeline,
			maskedCubeWorld,
			viewProjection,
			lightDirection,
			ObjectType_Cube,
			{ 0.98f, 0.84f, 0.30f, 1.0f },
			VisualMode_Solid,
			1.0f,
			time );
	}

	void RecordStencilOutlineLesson( AppState& app, ICommandBuffer& commandBuffer, const XMMATRIX& viewProjection, float time )
	{
		const XMFLOAT3 lightDirection = { -0.35f, -1.0f, -0.20f };
		const XMMATRIX floorWorld = XMMatrixScaling( 8.0f, 1.0f, 8.0f );
		const XMMATRIX baseCubeWorld =
			XMMatrixRotationY( time * 0.85f ) *
			XMMatrixTranslation( 0.0f, 1.35f, 0.2f );
		const XMMATRIX expandedCubeWorld =
			XMMatrixScaling( 1.14f, 1.14f, 1.14f ) *
			XMMatrixRotationY( time * 0.85f ) *
			XMMatrixTranslation( 0.0f, 1.35f, 0.2f );

		RecordObject(
			commandBuffer,
			app.sceneDepthPipeline,
			floorWorld,
			viewProjection,
			lightDirection,
			ObjectType_Plane,
			{ 0.72f, 0.76f, 0.82f, 1.0f },
			VisualMode_Checker,
			1.0f,
			time );
		RecordObject(
			commandBuffer,
			app.outlineBasePipeline,
			baseCubeWorld,
			viewProjection,
			lightDirection,
			ObjectType_Cube,
			{ 0.95f, 0.48f, 0.21f, 1.0f },
			VisualMode_Solid,
			1.0f,
			time );

		if( app.outlineShowExpandedCube )
		{
			RecordObject(
				commandBuffer,
				app.outlineStencilEnabled ? app.outlineStencilPipeline : app.outlineNoStencilPipeline,
				expandedCubeWorld,
				viewProjection,
				lightDirection,
				ObjectType_Cube,
				{ 0.16f, 0.90f, 0.92f, 1.0f },
				VisualMode_Solid,
				1.0f,
				time );
		}
	}

	std::string BuildDepthExplanation( const AppState& app )
	{
		if( !app.depthTestEnabled )
		{
			return "Depth test desactivado: manda el orden de dibujo. El ultimo cubo renderizado tapa al otro aunque este mas lejos.";
		}

		if( app.depthWriteEnabled )
		{
			return "Depth test y depth write activos: el cubo cercano gana aunque lo dibujes antes o despues. Ese es el caso normal del z-buffer.";
		}

		if( app.drawFrontCubeFirst )
		{
			return "Depth test activo pero sin escribir profundidad: el cubo cercano se dibuja primero, pero el lejano todavia pasa el test despues y puede sobreescribirlo.";
		}

		return "Depth test activo pero sin escribir profundidad: en este orden parece correcto solo porque el cubo cercano se dibuja el ultimo.";
	}

	LRESULT CALLBACK WindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		auto* app = reinterpret_cast<AppState*>( GetWindowLongPtr( hwnd, GWLP_USERDATA ) );

		switch( message )
		{
			case WM_RBUTTONDOWN:
			{
				if( app != nullptr && !ImguiWantsMouseCapture() )
				{
					app->camera.dragging = true;
					app->camera.lastMousePosition = GetMousePointFromLParam( lParam );
					SetCapture( hwnd );
				}
				return 0;
			}

			case WM_RBUTTONUP:
			{
				if( app != nullptr )
				{
					app->camera.dragging = false;
					if( GetCapture() == hwnd )
					{
						ReleaseCapture();
					}
				}
				return 0;
			}

			case WM_CAPTURECHANGED:
			case WM_KILLFOCUS:
			{
				if( app != nullptr )
				{
					app->camera.dragging = false;
				}
				return 0;
			}

			case WM_MOUSEMOVE:
			{
				if( app != nullptr && app->camera.dragging )
				{
					const POINT currentMousePosition = GetMousePointFromLParam( lParam );
					const int deltaX = currentMousePosition.x - app->camera.lastMousePosition.x;
					const int deltaY = currentMousePosition.y - app->camera.lastMousePosition.y;
					app->camera.lastMousePosition = currentMousePosition;

					constexpr float kRotationSpeed = 0.01f;
					app->camera.yaw += static_cast<float>( deltaX ) * kRotationSpeed;
					app->camera.pitch = std::clamp(
						app->camera.pitch + static_cast<float>( deltaY ) * kRotationSpeed,
						-1.35f,
						1.35f );
					return 0;
				}
				break;
			}

			case WM_MOUSEWHEEL:
			{
				if( app != nullptr && !ImguiWantsMouseCapture() )
				{
					const float wheelDelta = static_cast<float>( GET_WHEEL_DELTA_WPARAM( wParam ) ) / static_cast<float>( WHEEL_DELTA );
					app->camera.distance = std::clamp(
						app->camera.distance - wheelDelta * 0.8f,
						2.5f,
						25.0f );
					return 0;
				}
				break;
			}
			default:
				break;
		}

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
	AppState app{};
	HWND hwnd = nullptr;
	WNDCLASSEXW windowClass{};

	try
	{
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = L"LightD3D12DemoDepthStenclWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1440;
		constexpr uint32_t kInitialHeight = 900;

		hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Demo Depth Stencil",
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
		ResetCameraForMode( app );

		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		app.sceneDepthPipeline = CreateScenePipeline(
			ctx,
			contextDesc.swapchainFormat,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			PipelineConfig{} );
		app.sceneReadOnlyDepthPipeline = CreateScenePipeline(
			ctx,
			contextDesc.swapchainFormat,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			PipelineConfig{
				.depthEnable = true,
				.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO,
				.depthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL,
				.stencilEnable = false,
				.stencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
				.stencilPassOp = D3D12_STENCIL_OP_KEEP,
				.colorWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
			} );
		app.sceneNoDepthPipeline = CreateScenePipeline(
			ctx,
			contextDesc.swapchainFormat,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			PipelineConfig{
				.depthEnable = false,
				.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO,
				.depthFunc = D3D12_COMPARISON_FUNC_ALWAYS,
				.stencilEnable = false,
				.stencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
				.stencilPassOp = D3D12_STENCIL_OP_KEEP,
				.colorWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
			} );
		app.stencilWriterPipeline = CreateScenePipeline(
			ctx,
			contextDesc.swapchainFormat,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			PipelineConfig{
				.depthEnable = false,
				.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO,
				.depthFunc = D3D12_COMPARISON_FUNC_ALWAYS,
				.stencilEnable = true,
				.stencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
				.stencilPassOp = D3D12_STENCIL_OP_INCR_SAT,
				.colorWriteMask = 0,
			} );
		app.stencilInsidePipeline = CreateScenePipeline(
			ctx,
			contextDesc.swapchainFormat,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			PipelineConfig{
				.depthEnable = true,
				.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL,
				.depthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL,
				.stencilEnable = true,
				.stencilFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL,
				.stencilPassOp = D3D12_STENCIL_OP_KEEP,
				.colorWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
			} );
		app.stencilOutsidePipeline = CreateScenePipeline(
			ctx,
			contextDesc.swapchainFormat,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			PipelineConfig{
				.depthEnable = true,
				.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL,
				.depthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL,
				.stencilEnable = true,
				.stencilFunc = D3D12_COMPARISON_FUNC_EQUAL,
				.stencilPassOp = D3D12_STENCIL_OP_KEEP,
				.colorWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
			} );
		app.outlineBasePipeline = CreateScenePipeline(
			ctx,
			contextDesc.swapchainFormat,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			PipelineConfig{
				.depthEnable = true,
				.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL,
				.depthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL,
				.stencilEnable = true,
				.stencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
				.stencilPassOp = D3D12_STENCIL_OP_INCR_SAT,
				.colorWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
			} );
		app.outlineStencilPipeline = CreateScenePipeline(
			ctx,
			contextDesc.swapchainFormat,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			PipelineConfig{
				.depthEnable = true,
				.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO,
				.depthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL,
				.stencilEnable = true,
				.stencilFunc = D3D12_COMPARISON_FUNC_EQUAL,
				.stencilPassOp = D3D12_STENCIL_OP_KEEP,
				.colorWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
			} );
		app.outlineNoStencilPipeline = CreateScenePipeline(
			ctx,
			contextDesc.swapchainFormat,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			PipelineConfig{
				.depthEnable = true,
				.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO,
				.depthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL,
				.stencilEnable = false,
				.stencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
				.stencilPassOp = D3D12_STENCIL_OP_KEEP,
				.colorWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
			} );

		RecreateDepthStencilTarget( app );

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

			RecreateDepthStencilTarget( app );

			const auto now = std::chrono::steady_clock::now();
			float deltaSeconds = std::chrono::duration<float>( now - lastFrameTime ).count();
			lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );
			if( !app.pauseAnimation )
			{
				app.animationTime += deltaSeconds * app.cubeRotationSpeed;
			}

			const XMVECTOR cameraTarget = XMLoadFloat3( &app.camera.focus );
			const XMVECTOR cameraPosition = ComputeCameraPosition( app.camera );
			const XMVECTOR upVector = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
			const XMMATRIX viewMatrix = XMMatrixLookAtLH( cameraPosition, cameraTarget, upVector );
			const float aspectRatio = static_cast<float>( app.deviceManager->GetWidth() ) / static_cast<float>( app.deviceManager->GetHeight() );
			const XMMATRIX projectionMatrix = XMMatrixPerspectiveFovLH( XMConvertToRadians( 58.0f ), aspectRatio, 0.1f, 60.0f );
			const XMMATRIX viewProjection = XMMatrixMultiply( viewMatrix, projectionMatrix );

			if( app.imguiRenderer )
			{
				app.imguiRenderer->NewFrame();

				ImGui::SetNextWindowPos( ImVec2( 18.0f, 18.0f ), ImGuiCond_FirstUseEver );
				ImGui::SetNextWindowSize( ImVec2( 520.0f, 0.0f ), ImGuiCond_FirstUseEver );
				ImGui::Begin( "Demo Depth Stencil" );

				static const char* ourModeNames[] = {
					"Depth",
					"Stencil Mask",
					"Stencil Outline",
				};
				int modeIndex = static_cast<int>( app.demoMode );
				if( ImGui::Combo( "Lesson", &modeIndex, ourModeNames, IM_ARRAYSIZE( ourModeNames ) ) )
				{
					app.demoMode = static_cast<DemoMode>( modeIndex );
					ResetCameraForMode( app );
				}

				ImGui::Checkbox( "Pause animation", &app.pauseAnimation );
				ImGui::SliderFloat( "Animation speed", &app.cubeRotationSpeed, 0.0f, 2.5f, "%.2f" );
				if( ImGui::Button( "Reset camera" ) )
				{
					ResetCameraForMode( app );
				}
				ImGui::SameLine();
				ImGui::TextDisabled( "RMB orbit, wheel zoom" );
				ImGui::Text( "Camera distance: %.2f", app.camera.distance );
				ImGui::Separator();

				switch( app.demoMode )
				{
					case DemoMode::Depth:
					{
						ImGui::TextWrapped( "Dos cubos comparten casi la misma proyeccion en pantalla. Usa los controles para ver cuando manda el z-buffer y cuando manda el orden de dibujo." );
						ImGui::Checkbox( "Depth test enabled", &app.depthTestEnabled );
						ImGui::Checkbox( "Depth write enabled", &app.depthWriteEnabled );
						ImGui::Checkbox( "Draw front cube first", &app.drawFrontCubeFirst );
						ImGui::Separator();
						ImGui::Text( "DepthEnable: %s", app.depthTestEnabled ? "TRUE" : "FALSE" );
						ImGui::Text( "DepthWriteMask: %s", app.depthWriteEnabled ? "ALL" : "ZERO" );
						ImGui::Text( "Order: %s", app.drawFrontCubeFirst ? "front -> back" : "back -> front" );
						const std::string explanation = BuildDepthExplanation( app );
						ImGui::Separator();
						ImGui::TextWrapped( "%s", explanation.c_str() );
						break;
					}

					case DemoMode::StencilMask:
						ImGui::TextWrapped( "Stencil buffer = otro valor por pixel. Aqui lo usamos como mascara: la ventana central marca pixeles y el cubo luego pregunta si esta dentro o fuera de esa zona." );
						ImGui::Checkbox( "Stencil test enabled", &app.stencilEnabled );
						ImGui::Checkbox( "Draw cube inside mask", &app.stencilDrawInsideMask );
						ImGui::Checkbox( "Show mask frame", &app.stencilShowMaskCard );
						ImGui::Separator();
						ImGui::Text( "Paso 0: clear stencil = 0" );
						ImGui::Text( "Paso 1: la ventana invisible escribe stencil con ALWAYS + INCR_SAT" );
						ImGui::Text(
							"Paso 2: el cubo compara con %s frente a la ref implicita 0",
							app.stencilDrawInsideMask ? "NOT_EQUAL" : "EQUAL" );
						{
							const std::string explanation = BuildStencilMaskExplanation( app );
							ImGui::Separator();
							ImGui::TextWrapped( "%s", explanation.c_str() );
						}
						break;

					case DemoMode::StencilOutline:
						ImGui::TextWrapped( "El outline usa dos cubos. El naranja se dibuja normal y ademas marca stencil. El cian es un cubo un poco mas grande que solo debe sobrevivir fuera de la silueta naranja." );
						ImGui::Checkbox( "Use stencil on outline pass", &app.outlineStencilEnabled );
						ImGui::Checkbox( "Show expanded cube pass", &app.outlineShowExpandedCube );
						ImGui::Separator();
						ImGui::Text( "Paso 0: clear stencil = 0" );
						ImGui::Text( "Paso 1: cubo naranja -> StencilFunc = ALWAYS, StencilPassOp = INCR_SAT" );
						ImGui::Text(
							"Paso 2: cubo cian escalado -> %s",
							app.outlineStencilEnabled ? "StencilFunc = EQUAL con ref 0" : "sin stencil test" );
						{
							const std::string explanation = BuildStencilOutlineExplanation( app );
							ImGui::Separator();
							ImGui::TextWrapped( "%s", explanation.c_str() );
						}
						break;
				}

				ImGui::End();
			}

			auto& commandBuffer = ctx.AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx.GetCurrentSwapchainTexture();

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.06f, 0.08f, 0.12f, 1.0f };
			renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
			renderPass.depthStencil.depthStoreOp = StoreOp::Store;
			renderPass.depthStencil.clearDepth = 1.0f;
			renderPass.depthStencil.stencilLoadOp = LoadOp::Clear;
			renderPass.depthStencil.stencilStoreOp = StoreOp::Store;
			renderPass.depthStencil.clearStencil = 0;

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;
			framebuffer.depthStencil.texture = app.depthStencilTarget.texture;

			commandBuffer.CmdBeginRendering( renderPass, framebuffer );
			switch( app.demoMode )
			{
				case DemoMode::Depth:
					RecordDepthLesson( app, commandBuffer, viewProjection, app.animationTime );
					break;

				case DemoMode::StencilMask:
					RecordStencilMaskLesson( app, commandBuffer, viewProjection, app.animationTime );
					break;

				case DemoMode::StencilOutline:
					RecordStencilOutlineLesson( app, commandBuffer, viewProjection, app.animationTime );
					break;
			}
			commandBuffer.CmdEndRendering();

			if( app.imguiRenderer )
			{
				RenderPass imguiPass{};
				imguiPass.color[ 0 ].loadOp = LoadOp::Load;

				Framebuffer imguiFramebuffer{};
				imguiFramebuffer.color[ 0 ].texture = currentTexture;

				commandBuffer.CmdBeginRendering( imguiPass, imguiFramebuffer );
				app.imguiRenderer->Render( commandBuffer );
				commandBuffer.CmdEndRendering();
			}

			ctx.Submit( commandBuffer, currentTexture );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		app.deviceManager->WaitIdle();
		app.imguiRenderer.reset();
		DestroyDepthStencilTarget( ctx, app.depthStencilTarget );
		app.outlineNoStencilPipeline = {};
		app.outlineStencilPipeline = {};
		app.outlineBasePipeline = {};
		app.stencilOutsidePipeline = {};
		app.stencilInsidePipeline = {};
		app.stencilWriterPipeline = {};
		app.sceneNoDepthPipeline = {};
		app.sceneReadOnlyDepthPipeline = {};
		app.sceneDepthPipeline = {};
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
		if( hwnd != nullptr && IsWindow( hwnd ) != FALSE )
		{
			SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
			DestroyWindow( hwnd );
		}
		if( windowClass.lpszClassName != nullptr )
		{
			UnregisterClassW( windowClass.lpszClassName, instance );
		}

		const std::string message = std::string( "LightD3D12 DemoDepthStencl failed:\n" ) + exception.what();
		MessageBoxA( nullptr, message.c_str(), "LightD3D12", MB_ICONERROR | MB_OK );
		return 1;
	}
}
