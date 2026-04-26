#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"
#include  "LightD3D12\LightAssimpImporter.hpp"

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
	struct ShadowPassPushConstants
	{
		std::array<float, 16> worldLightViewProjection = {};
		uint32_t objectType = 0;
		uint32_t padding_[ 3 ] = {};
	};

	static_assert( sizeof( ShadowPassPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct ScenePassPushConstants
	{
		std::array<float, 16> worldViewProjection = {};
		std::array<float, 16> world = {};
		std::array<float, 16> worldLightViewProjection = {};
		std::array<float, 4> lightPosition = {};
		std::array<float, 4> objectColor = {};
		uint32_t shadowMapIndex = 0;
		uint32_t objectType = 0;
		float shadowBias = 0.0015f;
		float padding = 0.0f;
	};

	static_assert( sizeof( ScenePassPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct PreviewPassPushConstants
	{
		uint32_t shadowMapIndex = 0;
	};

	static_assert( sizeof( PreviewPassPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct DepthTarget
	{
		TextureHandle texture = {};
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct ShadowResources
	{
		TextureHandle shadowMap = {};
		TextureHandle previewColor = {};
		uint32_t shadowMapSize = 1024;
		uint32_t previewSize = 512;
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState shadowPipeline;
		RenderPipelineState scenePipeline;
		RenderPipelineState previewPipeline;
		DepthTarget sceneDepth;
		ShadowResources shadow;
		bool running = true;
		bool minimized = false;
		bool assimpEnabled = false;
		bool pauseAnimation = false;
		float animationTime = 0.0f;
		float cubeRotationSpeed = 0.8f;
		float lightDistance = 7.5f;
		float lightHeight = 8.0f;
		float lightX = 0.0f;
		float lightZ = 0.0f;
		float shadowBias = 0.0020f;
	};

	enum ObjectType : uint32_t
	{
		ObjectType_Plane = 0,
		ObjectType_Cube = 1,
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

	XMMATRIX LoadMatrix( const std::array<float, 16>& value )
	{
		return XMMATRIX(
			value[ 0 ], value[ 1 ], value[ 2 ], value[ 3 ],
			value[ 4 ], value[ 5 ], value[ 6 ], value[ 7 ],
			value[ 8 ], value[ 9 ], value[ 10 ], value[ 11 ],
			value[ 12 ], value[ 13 ], value[ 14 ], value[ 15 ] );
	}

	RenderPipelineState CreateShadowPipeline( RenderDevice& ctx, DXGI_FORMAT depthFormat )
	{
		static constexpr char ourVertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldLightViewProjection;
    uint gObjectType;
    uint3 gPadding;
};

float3 GetPlanePosition(uint vertexID)
{
    static const float3 positions[6] =
    {
        float3(-5.0, 0.0, -5.0),
        float3( 5.0, 0.0, -5.0),
        float3( 5.0, 0.0,  5.0),
        float3(-5.0, 0.0, -5.0),
        float3( 5.0, 0.0,  5.0),
        float3(-5.0, 0.0,  5.0)
    };
    return positions[vertexID];
}

float3 GetCubePosition(uint vertexID)
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
    return positions[vertexID];
}

float4 main(uint vertexID : SV_VertexID) : SV_Position
{
    const float3 localPosition = gObjectType == 0 ? GetPlanePosition(vertexID) : GetCubePosition(vertexID);
    return mul(float4(localPosition, 1.0), gWorldLightViewProjection);
}
)";

		static constexpr char ourPixelShader[] = R"(
void main()
{
}
)";

		RenderPipelineDesc desc{};
		desc.vertexShader.source = ourVertexShader;
		desc.vertexShader.entryPoint = "main";
		desc.vertexShader.profile = "vs_6_6";
		desc.fragmentShader.source = ourPixelShader;
		desc.fragmentShader.entryPoint = "main";
		desc.fragmentShader.profile = "ps_6_6";
		desc.depthFormat = depthFormat;
		desc.colorFormat = DXGI_FORMAT_UNKNOWN;
		desc.rasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		// The sample geometry is generated procedurally, so disabling culling keeps the
		// demo readable even when individual faces are wound differently.
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	RenderPipelineState CreateScenePipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat )
	{
		static constexpr char ourVertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProjection;
    row_major float4x4 gWorld;
    row_major float4x4 gWorldLightViewProjection;
    float4 gLightPosition;
    float4 gObjectColor;
    uint gShadowMapIndex;
    uint gObjectType;
    float gShadowBias;
    float gPadding;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float4 lightClipPosition : TEXCOORD2;
    float3 color : COLOR0;
};

void GetPlaneVertex(uint vertexID, out float3 position, out float3 normal)
{
    static const float3 positions[6] =
    {
        float3(-5.0, 0.0, -5.0),
        float3( 5.0, 0.0, -5.0),
        float3( 5.0, 0.0,  5.0),
        float3(-5.0, 0.0, -5.0),
        float3( 5.0, 0.0,  5.0),
        float3(-5.0, 0.0,  5.0)
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
    output.lightClipPosition = mul(float4(localPosition, 1.0), gWorldLightViewProjection);
    output.color = gObjectColor.rgb;
    return output;
}
)";

		static constexpr char ourPixelShader[] = R"(
cbuffer PushConstants : register(b0)
{
    row_major float4x4 gWorldViewProjection;
    row_major float4x4 gWorld;
    row_major float4x4 gWorldLightViewProjection;
    float4 gLightPosition;
    float4 gObjectColor;
    uint gShadowMapIndex;
    uint gObjectType;
    float gShadowBias;
    float gPadding;
};

SamplerState gSampler : register(s0);

float ShadowFactor(float4 lightClipPosition)
{
    Texture2D<float4> shadowMap = ResourceDescriptorHeap[gShadowMapIndex];

    const float3 lightNdc = lightClipPosition.xyz / max(lightClipPosition.w, 0.0001);
    const float2 shadowUv = float2(lightNdc.x * 0.5 + 0.5, 0.5 - lightNdc.y * 0.5);
    const float currentDepth = lightNdc.z;

    if (shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0 || currentDepth <= 0.0 || currentDepth >= 1.0)
    {
        return 1.0;
    }

    const float sampledDepth = shadowMap.SampleLevel(gSampler, shadowUv, 0).r;
    return currentDepth - gShadowBias > sampledDepth ? 0.18 : 1.0;
}

float4 main(float4 position : SV_Position, float3 worldPosition : TEXCOORD0, float3 worldNormal : TEXCOORD1, float4 lightClipPosition : TEXCOORD2, float3 color : COLOR0) : SV_Target0
{
    const float3 normal = normalize(worldNormal);
    const float3 lightDirection = normalize(gLightPosition.xyz - worldPosition);
    const float lambert = saturate(dot(normal, lightDirection));
    const float shadow = ShadowFactor(lightClipPosition);
    const float ambient = 0.20;
    const float diffuse = lambert * shadow * 0.80;
    return float4(color * (ambient + diffuse), 1.0);
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
		// The sample geometry is generated procedurally, so disabling culling keeps the
		// demo readable even when individual faces are wound differently.
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	RenderPipelineState CreatePreviewPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat )
	{
		static constexpr char ourVertexShader[] = R"(
float4 main(uint vertexID : SV_VertexID) : SV_Position
{
    const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    return float4(positions[vertexID], 0.0, 1.0);
}
)";

		static constexpr char ourPixelShader[] = R"(
cbuffer PushConstants : register(b0)
{
    uint gShadowMapIndex;
};

SamplerState gSampler : register(s0);

float4 main(float4 position : SV_Position) : SV_Target0
{
    Texture2D<float4> shadowMap = ResourceDescriptorHeap[gShadowMapIndex];
    const float2 uv = position.xy / float2(512.0, 512.0);
    const float depth = shadowMap.SampleLevel(gSampler, uv, 0).r;
    const float depthVisualization = saturate(1.0 - depth);
    return float4(depthVisualization.xxx, 1.0);
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
		desc.depthFormat = DXGI_FORMAT_UNKNOWN;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = FALSE;
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

	void RecreateSceneDepth( AppState& app )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const uint32_t width = app.deviceManager->GetWidth();
		const uint32_t height = app.deviceManager->GetHeight();
		if( app.sceneDepth.texture.Valid() && app.sceneDepth.width == width && app.sceneDepth.height == height )
		{
			return;
		}

		DestroyDepthTarget( ctx, app.sceneDepth );

		TextureDesc depthDesc{};
		depthDesc.debugName = "HelloShadowMapping Scene Depth";
		depthDesc.width = width;
		depthDesc.height = height;
		depthDesc.format = DXGI_FORMAT_D32_FLOAT;
		depthDesc.usage = TextureUsage::DepthStencil;
		depthDesc.useClearValue = true;
		depthDesc.clearValue.Format = depthDesc.format;
		depthDesc.clearValue.DepthStencil.Depth = 1.0f;
		depthDesc.clearValue.DepthStencil.Stencil = 0;
		app.sceneDepth.texture = ctx.CreateTexture( depthDesc );
		app.sceneDepth.width = width;
		app.sceneDepth.height = height;
	}

	void DestroyShadowResources( RenderDevice& ctx, ShadowResources& shadow )
	{
		if( shadow.shadowMap.Valid() )
		{
			ctx.Destroy( shadow.shadowMap );
			shadow.shadowMap = {};
		}

		if( shadow.previewColor.Valid() )
		{
			ctx.Destroy( shadow.previewColor );
			shadow.previewColor = {};
		}
	}

	void RecreateShadowResources( AppState& app )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		if( app.shadow.shadowMap.Valid() && app.shadow.previewColor.Valid() )
		{
			return;
		}

		DestroyShadowResources( ctx, app.shadow );

		TextureDesc shadowMapDesc{};
		shadowMapDesc.debugName = "HelloShadowMapping ShadowMap";
		shadowMapDesc.width = app.shadow.shadowMapSize;
		shadowMapDesc.height = app.shadow.shadowMapSize;
		shadowMapDesc.format = DXGI_FORMAT_D32_FLOAT;
		shadowMapDesc.usage = TextureUsage::Sampled | TextureUsage::DepthStencil;
		shadowMapDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		shadowMapDesc.useClearValue = true;
		shadowMapDesc.clearValue.Format = DXGI_FORMAT_D32_FLOAT;
		shadowMapDesc.clearValue.DepthStencil.Depth = 1.0f;
		shadowMapDesc.clearValue.DepthStencil.Stencil = 0;
		app.shadow.shadowMap = ctx.CreateTexture( shadowMapDesc );

		TextureDesc previewDesc{};
		previewDesc.debugName = "HelloShadowMapping Shadow Preview";
		previewDesc.width = app.shadow.previewSize;
		previewDesc.height = app.shadow.previewSize;
		previewDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		previewDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
		previewDesc.useClearValue = true;
		previewDesc.clearValue.Format = previewDesc.format;
		previewDesc.clearValue.Color[ 0 ] = 0.0f;
		previewDesc.clearValue.Color[ 1 ] = 0.0f;
		previewDesc.clearValue.Color[ 2 ] = 0.0f;
		previewDesc.clearValue.Color[ 3 ] = 1.0f;
		app.shadow.previewColor = ctx.CreateTexture( previewDesc );
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

	void RecordShadowObject( ICommandBuffer& commandBuffer, const RenderPipelineState& pipeline, const XMMATRIX& worldMatrix, const XMMATRIX& lightViewProjection, ObjectType objectType )
	{
		ShadowPassPushConstants pushConstants{};
		pushConstants.worldLightViewProjection = ToArray( ToFloat4x4( XMMatrixMultiply( worldMatrix, lightViewProjection ) ) );
		pushConstants.objectType = static_cast<uint32_t>( objectType );

		commandBuffer.CmdBindRenderPipeline( pipeline );
		commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
		commandBuffer.CmdDraw( objectType == ObjectType_Plane ? 6u : 36u );
	}

	void RecordSceneObject(
		ICommandBuffer& commandBuffer,
		const RenderPipelineState& pipeline,
		const XMMATRIX& worldMatrix,
		const XMMATRIX& cameraViewProjection,
		const XMMATRIX& lightViewProjection,
		const XMFLOAT3& lightPosition,
		uint32_t shadowMapIndex,
		float shadowBias,
		ObjectType objectType,
		const std::array<float, 4>& color )
	{
		ScenePassPushConstants pushConstants{};
		pushConstants.worldViewProjection = ToArray( ToFloat4x4( XMMatrixMultiply( worldMatrix, cameraViewProjection ) ) );
		pushConstants.world = ToArray( ToFloat4x4( worldMatrix ) );
		pushConstants.worldLightViewProjection = ToArray( ToFloat4x4( XMMatrixMultiply( worldMatrix, lightViewProjection ) ) );
		pushConstants.lightPosition = { lightPosition.x, lightPosition.y, lightPosition.z, 1.0f };
		pushConstants.objectColor = color;
		pushConstants.shadowMapIndex = shadowMapIndex;
		pushConstants.objectType = static_cast<uint32_t>( objectType );
		pushConstants.shadowBias = shadowBias;

		commandBuffer.CmdBindRenderPipeline( pipeline );
		commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
		commandBuffer.CmdDraw( objectType == ObjectType_Plane ? 6u : 36u );
	}
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE, PWSTR, int showCommand )
{
	AppState app{};
	HWND hwnd = nullptr;
	WNDCLASSEXW windowClass{};

	try
	{

		app.assimpEnabled = LightAssimpImporter::IsAvailable();
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = L"LightD3D12HelloShadowMappingWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1440;
		constexpr uint32_t kInitialHeight = 900;

		hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Shadow Mapping",
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

		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		app.shadowPipeline = CreateShadowPipeline( ctx, DXGI_FORMAT_D32_FLOAT );
		app.scenePipeline = CreateScenePipeline( ctx, contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT );
		app.previewPipeline = CreatePreviewPipeline( ctx, DXGI_FORMAT_R8G8B8A8_UNORM );
		RecreateSceneDepth( app );
		RecreateShadowResources( app );

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

			RecreateSceneDepth( app );
			RecreateShadowResources( app );

			const auto now = std::chrono::steady_clock::now();
			float deltaSeconds = std::chrono::duration<float>( now - lastFrameTime ).count();
			lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );
			if( !app.pauseAnimation )
			{
				app.animationTime += deltaSeconds;
			}

			const float cubeRotation = app.animationTime * app.cubeRotationSpeed;

			const XMMATRIX planeWorld = XMMatrixIdentity();
			// Lift the cube slightly above the plane so the shadow separation stays visible.
			const XMMATRIX cubeWorld = XMMatrixRotationY( cubeRotation ) * XMMatrixTranslation( 0.0f, 1.35f, 0.0f );

			const XMVECTOR cameraPosition = XMVectorSet( 6.5f, 5.0f, -8.0f, 1.0f );
			const XMVECTOR cameraTarget = XMVectorSet( 0.0f, 0.8f, 0.0f, 1.0f );
			const XMVECTOR upVector = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
			const XMMATRIX cameraView = XMMatrixLookAtLH( cameraPosition, cameraTarget, upVector );
			const float aspectRatio = static_cast<float>( app.deviceManager->GetWidth() ) / static_cast<float>( app.deviceManager->GetHeight() );
			const XMMATRIX cameraProjection = XMMatrixPerspectiveFovLH( XMConvertToRadians( 60.0f ), aspectRatio, 0.1f, 50.0f );
			const XMMATRIX cameraViewProjection = XMMatrixMultiply( cameraView, cameraProjection );

			const XMFLOAT3 lightPosition = { app.lightX, app.lightHeight, app.lightZ };
			const XMVECTOR lightPositionVector = XMVectorSet( lightPosition.x, lightPosition.y, lightPosition.z, 1.0f );
			const XMVECTOR lightTarget = XMVectorSet( 0.0f, 0.0f, 0.0f, 1.0f );
			// Looking straight down with the usual world-up vector would create a degenerate
			// view matrix, so switch to Z-up for the top-down light view.
			const XMVECTOR lightUpVector = std::abs( app.lightX ) < 0.001f && std::abs( app.lightZ ) < 0.001f
				? XMVectorSet( 0.0f, 0.0f, 1.0f, 0.0f )
				: upVector;
			const XMMATRIX lightView = XMMatrixLookAtLH( lightPositionVector, lightTarget, lightUpVector );
			// Keep the whole plane + cube inside the light frustum so the shadow remains
			// stable and easy to inspect while moving the light in the UI.
			const XMMATRIX lightProjection = XMMatrixOrthographicLH( 16.0f, 16.0f, 0.5f, 30.0f );
			const XMMATRIX lightViewProjection = XMMatrixMultiply( lightView, lightProjection );

			if( app.imguiRenderer )
			{
				app.imguiRenderer->NewFrame();

				ImGui::SetNextWindowPos( ImVec2( 18.0f, 18.0f ), ImGuiCond_FirstUseEver );
				ImGui::SetNextWindowSize( ImVec2( 420.0f, 0.0f ), ImGuiCond_FirstUseEver );
				ImGui::Begin( "Hello Shadow Mapping" );
				ImGui::TextWrapped( "This sample renders a shadow map from the light point of view, previews that depth in ImGui, and then shades the cube and plane in the main pass." );
				ImGui::Separator();
				ImGui::Text( "Assimp: %s", app.assimpEnabled ? "enabled" : "disabled" );
				ImGui::Checkbox( "Pause cube rotation", &app.pauseAnimation );
				ImGui::SliderFloat( "Cube rotation speed", &app.cubeRotationSpeed, 0.0f, 2.5f, "%.2f" );
				ImGui::SliderFloat( "Light X", &app.lightX, -8.0f, 8.0f, "%.2f" );
				ImGui::SliderFloat( "Light height", &app.lightHeight, 2.0f, 12.0f, "%.2f" );
				ImGui::SliderFloat( "Light Z", &app.lightZ, -8.0f, 8.0f, "%.2f" );
				ImGui::SliderFloat( "Shadow bias", &app.shadowBias, 0.0001f, 0.02f, "%.4f", ImGuiSliderFlags_Logarithmic );
				ImGui::Separator();
				ImGui::Text( "Shadow map: %u x %u", app.shadow.shadowMapSize, app.shadow.shadowMapSize );
				ImGui::TextUnformatted( "Depth from the light point of view" );
				const D3D12_GPU_DESCRIPTOR_HANDLE previewHandle = app.imguiRenderer->GetTextureGpuDescriptor( app.shadow.previewColor );
				ImGui::Image( static_cast<ImTextureID>( previewHandle.ptr ), ImVec2( 256.0f, 256.0f ) );
				ImGui::End();
			}

			auto& commandBuffer = ctx.AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx.GetCurrentSwapchainTexture();

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloShadowMapping::ShadowPass", 0xff4cc9f0u );

				RenderPass shadowRenderPass{};
				shadowRenderPass.depthStencil.depthLoadOp = LoadOp::Clear;
				shadowRenderPass.depthStencil.depthStoreOp = StoreOp::Store;
				shadowRenderPass.depthStencil.clearDepth = 1.0f;

				Framebuffer shadowFramebuffer{};
				shadowFramebuffer.depthStencil.texture = app.shadow.shadowMap;

				commandBuffer.CmdBeginRendering( shadowRenderPass, shadowFramebuffer );
				RecordShadowObject( commandBuffer, app.shadowPipeline, planeWorld, lightViewProjection, ObjectType_Plane );
				RecordShadowObject( commandBuffer, app.shadowPipeline, cubeWorld, lightViewProjection, ObjectType_Cube );
				commandBuffer.CmdEndRendering();
				commandBuffer.CmdTransitionTexture( app.shadow.shadowMap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			}

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloShadowMapping::ShadowPreviewPass", 0xff90be6du );

				RenderPass previewRenderPass{};
				previewRenderPass.color[ 0 ].loadOp = LoadOp::Clear;
				previewRenderPass.color[ 0 ].clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };

				Framebuffer previewFramebuffer{};
				previewFramebuffer.color[ 0 ].texture = app.shadow.previewColor;

				PreviewPassPushConstants previewPushConstants{};
				previewPushConstants.shadowMapIndex = ctx.GetBindlessIndex( app.shadow.shadowMap );

				commandBuffer.CmdBeginRendering( previewRenderPass, previewFramebuffer );
				commandBuffer.CmdBindRenderPipeline( app.previewPipeline );
				commandBuffer.CmdPushConstants( &previewPushConstants, sizeof( previewPushConstants ) );
				commandBuffer.CmdDraw( 3 );
				commandBuffer.CmdEndRendering();
				commandBuffer.CmdTransitionTexture( app.shadow.previewColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			}

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloShadowMapping::ScenePass", 0xfff9c74fu );

				RenderPass renderPass{};
				renderPass.color[ 0 ].loadOp = LoadOp::Clear;
				renderPass.color[ 0 ].clearColor = { 0.08f, 0.10f, 0.14f, 1.0f };
				renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
				renderPass.depthStencil.depthStoreOp = StoreOp::Store;
				renderPass.depthStencil.clearDepth = 1.0f;

				Framebuffer framebuffer{};
				framebuffer.color[ 0 ].texture = currentTexture;
				framebuffer.depthStencil.texture = app.sceneDepth.texture;

				commandBuffer.CmdBeginRendering( renderPass, framebuffer );
				RecordSceneObject(
					commandBuffer,
					app.scenePipeline,
					planeWorld,
					cameraViewProjection,
					lightViewProjection,
					lightPosition,
					ctx.GetBindlessIndex( app.shadow.shadowMap ),
					app.shadowBias,
					ObjectType_Plane,
					{ 0.75f, 0.77f, 0.82f, 1.0f } );
				RecordSceneObject(
					commandBuffer,
					app.scenePipeline,
					cubeWorld,
					cameraViewProjection,
					lightViewProjection,
					lightPosition,
					ctx.GetBindlessIndex( app.shadow.shadowMap ),
					app.shadowBias,
					ObjectType_Cube,
					{ 0.88f, 0.45f, 0.24f, 1.0f } );
				commandBuffer.CmdEndRendering();
			}

			if( app.imguiRenderer )
			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloShadowMapping::ImguiPass", 0xfff9844au );

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
		DestroyDepthTarget( ctx, app.sceneDepth );
		DestroyShadowResources( ctx, app.shadow );
		app.shadowPipeline = {};
		app.scenePipeline = {};
		app.previewPipeline = {};
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

		const std::string message = std::string( "LightD3D12 HelloShadowMapping failed:\n" ) + exception.what();
		MessageBoxA( nullptr, message.c_str(), "LightD3D12", MB_ICONERROR | MB_OK );
		return 1;
	}
}
