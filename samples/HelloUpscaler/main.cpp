#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#ifndef LIGHTD3D12_HAS_AMD_FSR_SDK
	#error HelloUpscaler requires LIGHTD3D12_HAS_AMD_FSR_SDK=1. Verify LightD3D12.FSR.props and the AMD FSR SDK layout.
#endif

#include <ffx_api.hpp>
#include <dx12/ffx_api_dx12.hpp>
#include <ffx_upscale.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

using namespace lightd3d12;

namespace
{
	constexpr float ourCameraNear = 0.1f;
	constexpr float ourCameraFar = 30.0f;
	constexpr float ourCameraFovYRadians = 1.05f;

	struct Matrix4
	{
		std::array<float, 16> values = {};
	};

	struct ScenePushConstants
	{
		std::array<float, 16> currentJitteredMvp = {};
		std::array<float, 16> currentUnjitteredMvp = {};
		std::array<float, 16> previousUnjitteredMvp = {};
		float time = 0.0f;
		float padding_[ 3 ] = {};
	};

	static_assert( sizeof( ScenePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct PresentPushConstants
	{
		uint32_t sceneTextureIndex = 0;
		uint32_t upscaledTextureIndex = 0;
		uint32_t viewMode = 0;
		uint32_t upscalerEnabled = 0;
		float splitPosition = 0.5f;
		float zoomFactor = 8.0f;
		float differenceAmplify = 6.0f;
		uint32_t renderWidth = 1;
		uint32_t renderHeight = 1;
		float padding_ = 0.0f;
	};

	static_assert( sizeof( PresentPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct OffscreenTargets
	{
		TextureHandle sceneColor = {};
		TextureHandle sceneDepth = {};
		TextureHandle motionVectors = {};
		TextureHandle upscaledOutput = {};
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		uint32_t displayWidth = 0;
		uint32_t displayHeight = 0;
	};

	struct FsrState
	{
		ffx::Context context = nullptr;
		uint32_t contextFlags = 0;
		uint32_t maxRenderWidth = 0;
		uint32_t maxRenderHeight = 0;
		uint32_t maxUpscaleWidth = 0;
		uint32_t maxUpscaleHeight = 0;
		int32_t jitterPhaseCount = 1;
		float estimatedMemoryUsageMb = 0.0f;
	};

	enum class UpscaleQualityMode : int
	{
		NativeAA = 0,
		Quality = 1,
		Balanced = 2,
		Performance = 3,
		UltraPerformance = 4,
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState scenePipeline;
		RenderPipelineState presentPipeline;
		OffscreenTargets targets;
		FsrState fsr;
		bool running = true;
		bool minimized = false;
		bool imguiMessageReady = false;
		bool fsrEnabled = true;
		bool enableSharpening = true;
		bool drawDebugView = false;
		bool pauseAnimation = false;
		bool vsync = true;
		bool requestHistoryReset = true;
		float sharpness = 0.2f;
		float rotationSpeed = 1.0f;
		float cubeScale = 0.92f;
		float splitPosition = 0.5f;
		float differenceAmplify = 6.0f;
		float zoomFactor = 8.0f;
		float animationTime = 0.0f;
		float previousAnimationTime = 0.0f;
		float smoothedFrameMs = 16.6f;
		float smoothedFps = 60.0f;
		float lastJitterX = 0.0f;
		float lastJitterY = 0.0f;
		uint32_t frameIndex = 0;
		int qualityModeIndex = static_cast<int>( UpscaleQualityMode::Quality );
		int presentModeIndex = 1;
		std::string lastFfxMessage;
		std::string lastFfxMessageType = "No messages";
	};

	constexpr std::array<const char*, 5> ourUpscaleModeNames = {
		"Native AA (1.0x)",
		"Quality (1.5x)",
		"Balanced (1.7x)",
		"Performance (2.0x)",
		"Ultra Performance (3.0x)",
	};

	constexpr std::array<const char*, 4> ourPresentModeNames = {
		"Final output",
		"Split: raw internal vs FSR",
		"Difference heatmap",
		"Center zoom",
	};

	AppState* ourFfxMessageSink = nullptr;

	Matrix4 MakeIdentity()
	{
		Matrix4 matrix{};
		matrix.values[ 0 ] = 1.0f;
		matrix.values[ 5 ] = 1.0f;
		matrix.values[ 10 ] = 1.0f;
		matrix.values[ 15 ] = 1.0f;
		return matrix;
	}

	Matrix4 Multiply( const Matrix4& left, const Matrix4& right )
	{
		Matrix4 result{};
		for( int row = 0; row < 4; ++row )
		{
			for( int column = 0; column < 4; ++column )
			{
				float sum = 0.0f;
				for( int index = 0; index < 4; ++index )
				{
					sum += left.values[ row * 4 + index ] * right.values[ index * 4 + column ];
				}
				result.values[ row * 4 + column ] = sum;
			}
		}
		return result;
	}

	Matrix4 MakeScale( float scale )
	{
		Matrix4 matrix = MakeIdentity();
		matrix.values[ 0 ] = scale;
		matrix.values[ 5 ] = scale;
		matrix.values[ 10 ] = scale;
		return matrix;
	}

	Matrix4 MakeRotationX( float radians )
	{
		const float cosine = std::cos( radians );
		const float sine = std::sin( radians );

		Matrix4 matrix = MakeIdentity();
		matrix.values[ 5 ] = cosine;
		matrix.values[ 6 ] = sine;
		matrix.values[ 9 ] = -sine;
		matrix.values[ 10 ] = cosine;
		return matrix;
	}

	Matrix4 MakeRotationY( float radians )
	{
		const float cosine = std::cos( radians );
		const float sine = std::sin( radians );

		Matrix4 matrix = MakeIdentity();
		matrix.values[ 0 ] = cosine;
		matrix.values[ 2 ] = -sine;
		matrix.values[ 8 ] = sine;
		matrix.values[ 10 ] = cosine;
		return matrix;
	}

	Matrix4 MakeRotationZ( float radians )
	{
		const float cosine = std::cos( radians );
		const float sine = std::sin( radians );

		Matrix4 matrix = MakeIdentity();
		matrix.values[ 0 ] = cosine;
		matrix.values[ 1 ] = sine;
		matrix.values[ 4 ] = -sine;
		matrix.values[ 5 ] = cosine;
		return matrix;
	}

	Matrix4 MakeTranslation( float x, float y, float z )
	{
		Matrix4 matrix = MakeIdentity();
		matrix.values[ 12 ] = x;
		matrix.values[ 13 ] = y;
		matrix.values[ 14 ] = z;
		return matrix;
	}

	Matrix4 MakePerspectiveFovLH( float fovYRadians, float aspectRatio, float zNear, float zFar )
	{
		Matrix4 matrix{};
		const float yScale = 1.0f / std::tan( fovYRadians * 0.5f );
		const float xScale = yScale / aspectRatio;
		matrix.values[ 0 ] = xScale;
		matrix.values[ 5 ] = yScale;
		matrix.values[ 10 ] = zFar / ( zFar - zNear );
		matrix.values[ 11 ] = 1.0f;
		matrix.values[ 14 ] = -( zNear * zFar ) / ( zFar - zNear );
		return matrix;
	}

	Matrix4 ApplyProjectionJitter( Matrix4 projection, float jitterClipX, float jitterClipY )
	{
		projection.values[ 8 ] += jitterClipX;
		projection.values[ 9 ] += jitterClipY;
		return projection;
	}

	Matrix4 BuildModelMatrix( float time, float cubeScale )
	{
		Matrix4 model = MakeScale( cubeScale );
		model = Multiply( model, MakeRotationX( time * 0.71f ) );
		model = Multiply( model, MakeRotationY( time ) );
		model = Multiply( model, MakeRotationZ( time * 0.37f ) );
		model = Multiply(
			model,
			MakeTranslation(
				std::sin( time * 0.9f ) * 0.38f,
				std::cos( time * 0.7f ) * 0.16f,
				4.7f ) );
		return model;
	}

	void CopyMatrix( const Matrix4& source, std::array<float, 16>& destination )
	{
		destination = source.values;
	}

	std::string NarrowFromWide( const wchar_t* text )
	{
		if( text == nullptr || text[ 0 ] == L'\0' )
		{
			return {};
		}

		const int requiredSize = WideCharToMultiByte( CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr );
		if( requiredSize <= 0 )
		{
			return {};
		}

		std::string result( static_cast<size_t>( requiredSize ), '\0' );
		const int convertedSize = WideCharToMultiByte( CP_UTF8, 0, text, -1, result.data(), requiredSize, nullptr, nullptr );
		if( convertedSize <= 0 )
		{
			return {};
		}
		result.resize( static_cast<size_t>( convertedSize - 1 ) );
		return result;
	}

	void FfxMessageCallback( uint32_t type, const wchar_t* message )
	{
		if( ourFfxMessageSink == nullptr )
		{
			return;
		}

		ourFfxMessageSink->lastFfxMessage = NarrowFromWide( message );
		switch( type )
		{
			case FFX_API_MESSAGE_TYPE_ERROR:
				ourFfxMessageSink->lastFfxMessageType = "Error";
				break;

			case FFX_API_MESSAGE_TYPE_WARNING:
				ourFfxMessageSink->lastFfxMessageType = "Warning";
				break;

			default:
				ourFfxMessageSink->lastFfxMessageType = "Info";
				break;
		}
	}

	void ThrowIfFfxFailed( ffx::ReturnCode returnCode, const char* message )
	{
		if( returnCode != ffx::ReturnCode::Ok )
		{
			throw std::runtime_error( message );
		}
	}

	uint32_t ToFfxQualityMode( int qualityModeIndex )
	{
		switch( static_cast<UpscaleQualityMode>( qualityModeIndex ) )
		{
			case UpscaleQualityMode::NativeAA:
				return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;

			case UpscaleQualityMode::Quality:
				return FFX_UPSCALE_QUALITY_MODE_QUALITY;

			case UpscaleQualityMode::Balanced:
				return FFX_UPSCALE_QUALITY_MODE_BALANCED;

			case UpscaleQualityMode::Performance:
				return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;

			case UpscaleQualityMode::UltraPerformance:
				return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
		}

		return FFX_UPSCALE_QUALITY_MODE_QUALITY;
	}

	void ComputeDesiredRenderResolution( const AppState& app, uint32_t displayWidth, uint32_t displayHeight, uint32_t& renderWidth, uint32_t& renderHeight )
	{
		if( !app.fsrEnabled )
		{
			renderWidth = displayWidth;
			renderHeight = displayHeight;
			return;
		}

		ffx::QueryDescUpscaleGetRenderResolutionFromQualityMode resolutionQuery{};
		resolutionQuery.displayWidth = displayWidth;
		resolutionQuery.displayHeight = displayHeight;
		resolutionQuery.qualityMode = ToFfxQualityMode( app.qualityModeIndex );
		resolutionQuery.pOutRenderWidth = &renderWidth;
		resolutionQuery.pOutRenderHeight = &renderHeight;
		ThrowIfFfxFailed( ffx::Query( resolutionQuery ), "Failed to query FSR render resolution." );
	}

	uint32_t BuildFsrContextFlags()
	{
		uint32_t flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
		return flags;
	}

	void DestroyFsrContext( AppState& app )
	{
		if( app.fsr.context == nullptr )
		{
			return;
		}

		if( app.deviceManager != nullptr )
		{
			app.deviceManager->WaitIdle();
		}

		ffx::DestroyContext( app.fsr.context );
		app.fsr = {};
		app.lastJitterX = 0.0f;
		app.lastJitterY = 0.0f;
	}

	void DestroyTargets( RenderDevice& ctx, OffscreenTargets& targets )
	{
		if( targets.sceneColor.Valid() )
		{
			ctx.Destroy( targets.sceneColor );
			targets.sceneColor = {};
		}

		if( targets.sceneDepth.Valid() )
		{
			ctx.Destroy( targets.sceneDepth );
			targets.sceneDepth = {};
		}

		if( targets.motionVectors.Valid() )
		{
			ctx.Destroy( targets.motionVectors );
			targets.motionVectors = {};
		}

		if( targets.upscaledOutput.Valid() )
		{
			ctx.Destroy( targets.upscaledOutput );
			targets.upscaledOutput = {};
		}

		targets.renderWidth = 0;
		targets.renderHeight = 0;
		targets.displayWidth = 0;
		targets.displayHeight = 0;
	}

	void RecreateTargets( AppState& app )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const uint32_t displayWidth = app.deviceManager->GetWidth();
		const uint32_t displayHeight = app.deviceManager->GetHeight();

		uint32_t desiredRenderWidth = displayWidth;
		uint32_t desiredRenderHeight = displayHeight;
		ComputeDesiredRenderResolution( app, displayWidth, displayHeight, desiredRenderWidth, desiredRenderHeight );

		if( app.targets.sceneColor.Valid() &&
			app.targets.sceneDepth.Valid() &&
			app.targets.motionVectors.Valid() &&
			app.targets.upscaledOutput.Valid() &&
			app.targets.renderWidth == desiredRenderWidth &&
			app.targets.renderHeight == desiredRenderHeight &&
			app.targets.displayWidth == displayWidth &&
			app.targets.displayHeight == displayHeight )
		{
			return;
		}

		ctx.WaitIdle();
		DestroyTargets( ctx, app.targets );

		TextureDesc sceneColorDesc{};
		sceneColorDesc.debugName = "HelloUpscaler Scene Color";
		sceneColorDesc.width = desiredRenderWidth;
		sceneColorDesc.height = desiredRenderHeight;
		sceneColorDesc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		sceneColorDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
		sceneColorDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		sceneColorDesc.useClearValue = true;
		sceneColorDesc.clearValue.Format = sceneColorDesc.format;
		sceneColorDesc.clearValue.Color[ 0 ] = 0.03f;
		sceneColorDesc.clearValue.Color[ 1 ] = 0.05f;
		sceneColorDesc.clearValue.Color[ 2 ] = 0.08f;
		sceneColorDesc.clearValue.Color[ 3 ] = 1.0f;
		app.targets.sceneColor = ctx.CreateTexture( sceneColorDesc );

		TextureDesc depthDesc{};
		depthDesc.debugName = "HelloUpscaler Scene Depth";
		depthDesc.width = desiredRenderWidth;
		depthDesc.height = desiredRenderHeight;
		depthDesc.format = DXGI_FORMAT_D32_FLOAT;
		depthDesc.usage = TextureUsage::Sampled | TextureUsage::DepthStencil;
		depthDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		depthDesc.useClearValue = true;
		depthDesc.clearValue.Format = depthDesc.format;
		depthDesc.clearValue.DepthStencil.Depth = 1.0f;
		depthDesc.clearValue.DepthStencil.Stencil = 0;
		app.targets.sceneDepth = ctx.CreateTexture( depthDesc );

		TextureDesc motionDesc{};
		motionDesc.debugName = "HelloUpscaler Motion Vectors";
		motionDesc.width = desiredRenderWidth;
		motionDesc.height = desiredRenderHeight;
		motionDesc.format = DXGI_FORMAT_R16G16_FLOAT;
		motionDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
		motionDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		motionDesc.useClearValue = true;
		motionDesc.clearValue.Format = motionDesc.format;
		motionDesc.clearValue.Color[ 0 ] = 0.0f;
		motionDesc.clearValue.Color[ 1 ] = 0.0f;
		motionDesc.clearValue.Color[ 2 ] = 0.0f;
		motionDesc.clearValue.Color[ 3 ] = 0.0f;
		app.targets.motionVectors = ctx.CreateTexture( motionDesc );

		TextureDesc upscaledOutputDesc{};
		upscaledOutputDesc.debugName = "HelloUpscaler Output";
		upscaledOutputDesc.width = displayWidth;
		upscaledOutputDesc.height = displayHeight;
		upscaledOutputDesc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		upscaledOutputDesc.usage = TextureUsage::Sampled | TextureUsage::UnorderedAccess;
		upscaledOutputDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		app.targets.upscaledOutput = ctx.CreateTexture( upscaledOutputDesc );

		app.targets.renderWidth = desiredRenderWidth;
		app.targets.renderHeight = desiredRenderHeight;
		app.targets.displayWidth = displayWidth;
		app.targets.displayHeight = displayHeight;
		app.requestHistoryReset = true;
	}

	void EnsureFsrContext( AppState& app )
	{
		if( !app.fsrEnabled )
		{
			DestroyFsrContext( app );
			return;
		}

		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const uint32_t desiredFlags = BuildFsrContextFlags();
		if( app.fsr.context != nullptr &&
			app.fsr.maxRenderWidth == app.targets.renderWidth &&
			app.fsr.maxRenderHeight == app.targets.renderHeight &&
			app.fsr.maxUpscaleWidth == app.targets.displayWidth &&
			app.fsr.maxUpscaleHeight == app.targets.displayHeight &&
			app.fsr.contextFlags == desiredFlags )
		{
			return;
		}

		DestroyFsrContext( app );

		FfxApiEffectMemoryUsage memoryUsage{};
		ffx::QueryDescUpscaleGetGPUMemoryUsageV2 memoryQuery{};
		memoryQuery.device = ctx.GetNativeDevice();
		memoryQuery.maxRenderSize = { app.targets.renderWidth, app.targets.renderHeight };
		memoryQuery.maxUpscaleSize = { app.targets.displayWidth, app.targets.displayHeight };
		memoryQuery.flags = desiredFlags;
		memoryQuery.gpuMemoryUsageUpscaler = &memoryUsage;
		if( ffx::Query( memoryQuery ) == ffx::ReturnCode::Ok )
		{
			app.fsr.estimatedMemoryUsageMb = static_cast<float>( memoryUsage.totalUsageInBytes ) / ( 1024.0f * 1024.0f );
		}

		ffx::CreateBackendDX12Desc backendDesc{};
		backendDesc.device = ctx.GetNativeDevice();

		ffx::CreateContextDescUpscale createUpscaleDesc{};
		createUpscaleDesc.flags = desiredFlags;
		createUpscaleDesc.maxRenderSize = { app.targets.renderWidth, app.targets.renderHeight };
		createUpscaleDesc.maxUpscaleSize = { app.targets.displayWidth, app.targets.displayHeight };
		createUpscaleDesc.fpMessage = nullptr;

		ffx::CreateContextDescUpscaleVersion createVersionDesc{};
		createVersionDesc.version = FFX_UPSCALER_VERSION;

		ThrowIfFfxFailed(
			ffx::CreateContext( app.fsr.context, nullptr, createUpscaleDesc, backendDesc, createVersionDesc ),
			"Failed to create the FSR upscaler context." );

		ffx::QueryDescUpscaleGetJitterPhaseCount jitterPhaseQuery{};
		jitterPhaseQuery.renderWidth = app.targets.renderWidth;
		jitterPhaseQuery.displayWidth = app.targets.displayWidth;
		jitterPhaseQuery.pOutPhaseCount = &app.fsr.jitterPhaseCount;
		ThrowIfFfxFailed(
			ffx::Query( app.fsr.context, jitterPhaseQuery ),
			"Failed to query the FSR jitter phase count." );

		app.fsr.contextFlags = desiredFlags;
		app.fsr.maxRenderWidth = app.targets.renderWidth;
		app.fsr.maxRenderHeight = app.targets.renderHeight;
		app.fsr.maxUpscaleWidth = app.targets.displayWidth;
		app.fsr.maxUpscaleHeight = app.targets.displayHeight;
		app.fsr.jitterPhaseCount = std::max( app.fsr.jitterPhaseCount, 1 );
		app.frameIndex = 0;
		app.requestHistoryReset = true;
	}

	void UpdateJitter( AppState& app )
	{
		if( !app.fsrEnabled || app.fsr.context == nullptr )
		{
			app.lastJitterX = 0.0f;
			app.lastJitterY = 0.0f;
			return;
		}

		if( app.requestHistoryReset )
		{
			app.frameIndex = 0;
		}

		const int32_t jitterIndex = static_cast<int32_t>( app.frameIndex % static_cast<uint32_t>( app.fsr.jitterPhaseCount ) );
		ffx::QueryDescUpscaleGetJitterOffset jitterOffsetQuery{};
		jitterOffsetQuery.index = jitterIndex;
		jitterOffsetQuery.phaseCount = app.fsr.jitterPhaseCount;
		jitterOffsetQuery.pOutX = &app.lastJitterX;
		jitterOffsetQuery.pOutY = &app.lastJitterY;
		ThrowIfFfxFailed(
			ffx::Query( app.fsr.context, jitterOffsetQuery ),
			"Failed to query the FSR jitter offset." );
	}

	struct SceneMatrices
	{
		Matrix4 currentJitteredMvp = {};
		Matrix4 currentUnjitteredMvp = {};
		Matrix4 previousUnjitteredMvp = {};
	};

	SceneMatrices BuildSceneMatrices( const AppState& app )
	{
		const float aspectRatio = static_cast<float>( app.targets.renderWidth ) / static_cast<float>( app.targets.renderHeight );
		const Matrix4 projection = MakePerspectiveFovLH( ourCameraFovYRadians, aspectRatio, ourCameraNear, ourCameraFar );

		float jitterClipX = 0.0f;
		float jitterClipY = 0.0f;
		if( app.fsrEnabled )
		{
			jitterClipX = -2.0f * app.lastJitterX / static_cast<float>( app.targets.renderWidth );
			jitterClipY = 2.0f * app.lastJitterY / static_cast<float>( app.targets.renderHeight );
		}

		const Matrix4 currentModel = BuildModelMatrix( app.animationTime, app.cubeScale );
		const Matrix4 previousModel = BuildModelMatrix( app.previousAnimationTime, app.cubeScale );

		SceneMatrices matrices{};
		matrices.currentUnjitteredMvp = Multiply( currentModel, projection );
		matrices.currentJitteredMvp = Multiply( currentModel, ApplyProjectionJitter( projection, jitterClipX, jitterClipY ) );
		matrices.previousUnjitteredMvp = Multiply( previousModel, projection );
		return matrices;
	}

	RenderPipelineState CreateScenePipeline( RenderDevice& ctx, DXGI_FORMAT sceneColorFormat, DXGI_FORMAT motionVectorFormat, DXGI_FORMAT depthFormat )
	{
		static constexpr char ourVertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    row_major float4x4 gCurrentJitteredMvp;
    row_major float4x4 gCurrentUnjitteredMvp;
    row_major float4x4 gPreviousUnjitteredMvp;
    float gTime;
    float3 gPadding;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 localPosition : TEXCOORD0;
    float3 localNormal : TEXCOORD1;
    float2 motionVector : TEXCOORD2;
};

float2 SafeNdc(float4 clipPosition)
{
    return clipPosition.xy / max(abs(clipPosition.w), 1.0e-5);
}

void GetCubeVertex(uint vertexID, out float3 position, out float3 normal)
{
    static const float3 positions[36] =
    {
        float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0),
        float3(-1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),
        float3(-1.0, -1.0,  1.0), float3(-1.0, -1.0, -1.0), float3(-1.0,  1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3(-1.0,  1.0, -1.0), float3(-1.0,  1.0,  1.0),
        float3( 1.0, -1.0, -1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0),
        float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0,  1.0), float3( 1.0,  1.0, -1.0),
        float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0),
        float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0, -1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0, -1.0), float3(-1.0, -1.0, -1.0)
    };

    static const float3 normals[36] =
    {
        float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0),
        float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0), float3( 0.0,  0.0, -1.0),
        float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0),
        float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0), float3( 0.0,  0.0,  1.0),
        float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0),
        float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0), float3(-1.0,  0.0,  0.0),
        float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0),
        float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0), float3( 1.0,  0.0,  0.0),
        float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0),
        float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0), float3( 0.0,  1.0,  0.0),
        float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0),
        float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0), float3( 0.0, -1.0,  0.0)
    };

    position = positions[vertexID];
    normal = normals[vertexID];
}

VSOutput main(uint vertexID : SV_VertexID)
{
    float3 localPosition;
    float3 localNormal;
    GetCubeVertex(vertexID, localPosition, localNormal);

    const float4 local = float4(localPosition, 1.0);
    const float4 currentJittered = mul(local, gCurrentJitteredMvp);
    const float4 currentUnjittered = mul(local, gCurrentUnjitteredMvp);
    const float4 previousUnjittered = mul(local, gPreviousUnjitteredMvp);

    VSOutput output;
    output.position = currentJittered;
    output.localPosition = localPosition;
    output.localNormal = localNormal;
    output.motionVector = SafeNdc(previousUnjittered) - SafeNdc(currentUnjittered);
    return output;
}
)";

		static constexpr char ourPixelShader[] = R"(
cbuffer PushConstants : register(b0)
{
    row_major float4x4 gCurrentJitteredMvp;
    row_major float4x4 gCurrentUnjitteredMvp;
    row_major float4x4 gPreviousUnjitteredMvp;
    float gTime;
    float3 gPadding;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 localPosition : TEXCOORD0;
    float3 localNormal : TEXCOORD1;
    float2 motionVector : TEXCOORD2;
};

struct PSOutput
{
    float4 color : SV_Target0;
    float2 motion : SV_Target1;
};

PSOutput main(PSInput input)
{
    const float3 baseColor = 0.25 + 0.75 * abs(input.localNormal);
    const float stripeA = 0.5 + 0.5 * sin(input.localPosition.x * 18.0 + gTime * 3.0);
    const float stripeB = 0.5 + 0.5 * sin(input.localPosition.y * 12.0 - gTime * 1.5);
    const float2 microPatternUv = (input.localPosition.xy + 1.0) * 6.0;
    const float2 microLine = abs(frac(microPatternUv) - 0.5);
    const float microGrid = 1.0 - saturate(min(microLine.x, microLine.y) * 10.0);
    const float highlight = 0.55 + 0.45 * stripeA * stripeB;
    const float3 lighting = normalize(float3(-0.45, 0.80, -0.35));
    const float lambert = saturate(dot(normalize(input.localNormal), lighting)) * 0.75 + 0.25;

    PSOutput output;
    output.color = float4(lerp(baseColor * lambert * highlight, float3(1.0, 0.97, 0.82), microGrid * 0.35), 1.0);
    output.motion = input.motionVector;
    return output;
}
)";

		RenderPipelineDesc desc{};
		desc.vertexShader.source = ourVertexShader;
		desc.vertexShader.entryPoint = "main";
		desc.vertexShader.profile = "vs_6_6";
		desc.fragmentShader.source = ourPixelShader;
		desc.fragmentShader.entryPoint = "main";
		desc.fragmentShader.profile = "ps_6_6";
		desc.color[ 0 ].format = sceneColorFormat;
		desc.color[ 1 ].format = motionVectorFormat;
		desc.depthFormat = depthFormat;
		desc.rasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	RenderPipelineState CreatePresentPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat )
	{
		static constexpr char ourVertexShader[] = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    VSOutput output;
    output.position = float4(positions[vertexID], 0.0, 1.0);
    output.uv = float2(positions[vertexID].x * 0.5 + 0.5, 0.5 - positions[vertexID].y * 0.5);
    return output;
}
)";

		static constexpr char ourPixelShader[] = R"(
cbuffer PushConstants : register(b0)
{
    uint gSceneTextureIndex;
    uint gUpscaledTextureIndex;
    uint gViewMode;
    uint gUpscalerEnabled;
    float gSplitPosition;
    float gZoomFactor;
    float gDifferenceAmplify;
    uint gRenderWidth;
    uint gRenderHeight;
    float gPadding;
};

SamplerState gSampler : register(s0);

float3 SampleSceneNearest(Texture2D<float4> sceneTexture, float2 uv)
{
    const int2 dimensions = int2(max(gRenderWidth, 1u), max(gRenderHeight, 1u));
    const int2 pixel = clamp(int2(floor(uv * float2(dimensions))), int2(0, 0), dimensions - 1);
    return sceneTexture.Load(int3(pixel, 0)).rgb;
}

float2 ApplyZoom(float2 uv)
{
    return (uv - 0.5) / max(gZoomFactor, 1.0) + 0.5;
}

float GridOverlay(float2 uv)
{
    const float2 pixelCoord = uv * float2(max(gRenderWidth, 1u), max(gRenderHeight, 1u));
    const float2 distanceToEdge = abs(frac(pixelCoord) - 0.5);
    const float gridLine = 1.0 - saturate(min(distanceToEdge.x, distanceToEdge.y) * 7.0);
    return gridLine;
}

float3 DifferenceHeatmap(float value)
{
    value = saturate(value);
    if (value < 0.33)
    {
        return lerp(float3(0.06, 0.10, 0.40), float3(0.00, 0.80, 1.00), value / 0.33);
    }
    if (value < 0.66)
    {
        return lerp(float3(0.00, 0.80, 1.00), float3(1.00, 0.92, 0.20), (value - 0.33) / 0.33);
    }
    return lerp(float3(1.00, 0.92, 0.20), float3(1.00, 0.15, 0.10), (value - 0.66) / 0.34);
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> sceneTexture = ResourceDescriptorHeap[gSceneTextureIndex];
    Texture2D<float4> upscaledTexture = ResourceDescriptorHeap[gUpscaledTextureIndex];

    const float3 sceneLinear = sceneTexture.SampleLevel(gSampler, uv, 0).rgb;
    const float3 sceneNearest = SampleSceneNearest(sceneTexture, uv);
    const float3 upscaledColor = upscaledTexture.SampleLevel(gSampler, uv, 0).rgb;

    if (gUpscalerEnabled == 0u)
    {
        return float4(sceneLinear, 1.0);
    }

    float3 color = upscaledColor;
    if (gViewMode == 1u)
    {
        color = uv.x < gSplitPosition ? sceneNearest : upscaledColor;
        if (abs(uv.x - gSplitPosition) < 0.0025)
        {
            color = float3(1.0, 0.95, 0.30);
        }
    }
    else if (gViewMode == 2u)
    {
        const float3 difference = abs(upscaledColor - sceneNearest);
        const float metric = dot(difference, float3(0.299, 0.587, 0.114)) * gDifferenceAmplify;
        color = DifferenceHeatmap(metric);
    }
    else if (gViewMode == 3u)
    {
        const float2 zoomUv = ApplyZoom(uv);
        const float3 zoomNearest = SampleSceneNearest(sceneTexture, zoomUv);
        const float3 zoomUpscaled = upscaledTexture.SampleLevel(gSampler, zoomUv, 0).rgb;
        color = uv.x < gSplitPosition ? zoomNearest : zoomUpscaled;
        if (uv.x < gSplitPosition)
        {
            const float grid = GridOverlay(zoomUv);
            color = lerp(color, float3(1.0, 0.90, 0.30), grid * 0.35);
        }
        if (abs(uv.x - gSplitPosition) < 0.0025)
        {
            color = float3(1.0, 0.95, 0.30);
        }
    }

    return float4(color, 1.0);
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

	void DispatchUpscaler( AppState& app, RenderDevice& ctx, ICommandBuffer& commandBuffer, float deltaSeconds )
	{
		commandBuffer.CmdTransitionTexture(
			app.targets.sceneColor,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
		commandBuffer.CmdTransitionTexture(
			app.targets.sceneDepth,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
		commandBuffer.CmdTransitionTexture(
			app.targets.motionVectors,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
		commandBuffer.CmdTransitionTexture( app.targets.upscaledOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );

		// FSR is integrated as a native DX12 dispatch, so we hand the SDK the command list
		// and the exact resources produced by the sample's render pass.
		ffx::DispatchDescUpscale dispatchDesc{};
		dispatchDesc.commandList = commandBuffer.GetNativeGraphicsCommandList();
		dispatchDesc.color = ffxApiGetResourceDX12(
			ctx.GetNativeTextureResource( app.targets.sceneColor ),
			FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
		dispatchDesc.depth = ffxApiGetResourceDX12(
			ctx.GetNativeTextureResource( app.targets.sceneDepth ),
			FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
		dispatchDesc.motionVectors = ffxApiGetResourceDX12(
			ctx.GetNativeTextureResource( app.targets.motionVectors ),
			FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
		dispatchDesc.exposure = ffxApiGetResourceDX12( nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
		dispatchDesc.reactive = ffxApiGetResourceDX12( nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
		dispatchDesc.transparencyAndComposition = ffxApiGetResourceDX12( nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
		dispatchDesc.output = ffxApiGetResourceDX12(
			ctx.GetNativeTextureResource( app.targets.upscaledOutput ),
			FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
			FFX_API_RESOURCE_USAGE_UAV );
		dispatchDesc.jitterOffset.x = -app.lastJitterX;
		dispatchDesc.jitterOffset.y = -app.lastJitterY;
		dispatchDesc.motionVectorScale.x = static_cast<float>( app.targets.renderWidth );
		dispatchDesc.motionVectorScale.y = static_cast<float>( app.targets.renderHeight );
		dispatchDesc.renderSize = { app.targets.renderWidth, app.targets.renderHeight };
		dispatchDesc.upscaleSize = { app.targets.displayWidth, app.targets.displayHeight };
		dispatchDesc.enableSharpening = app.enableSharpening;
		dispatchDesc.sharpness = app.sharpness;
		dispatchDesc.frameTimeDelta = deltaSeconds * 1000.0f;
		dispatchDesc.preExposure = 1.0f;
		dispatchDesc.reset = app.requestHistoryReset;
		dispatchDesc.cameraNear = ourCameraNear;
		dispatchDesc.cameraFar = ourCameraFar;
		dispatchDesc.cameraFovAngleVertical = ourCameraFovYRadians;
		dispatchDesc.viewSpaceToMetersFactor = 1.0f;
		dispatchDesc.flags = app.drawDebugView ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0u;
		ThrowIfFfxFailed( ffx::Dispatch( app.fsr.context, dispatchDesc ), "Failed to dispatch the FSR upscaler." );

		commandBuffer.CmdTransitionTexture(
			app.targets.upscaledOutput,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
		app.requestHistoryReset = false;
		app.frameIndex++;
	}

	LRESULT CALLBACK WindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		auto* app = reinterpret_cast<AppState*>( GetWindowLongPtr( hwnd, GWLP_USERDATA ) );

		if( app != nullptr && app->imguiMessageReady && app->imguiRenderer && app->imguiRenderer->ProcessMessage( hwnd, message, wParam, lParam ) )
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
						app->requestHistoryReset = true;
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
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = L"LightD3D12HelloUpscalerWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1440;
		constexpr uint32_t kInitialHeight = 900;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Upscaler",
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
			throw std::runtime_error( "Failed to create the Win32 window." );
		}

		ShowWindow( hwnd, showCommand );
		UpdateWindow( hwnd );

		AppState app{};
		app.vsync = true;
		ourFfxMessageSink = &app;

		ContextDesc contextDesc{};
		contextDesc.enableDebugLayer = true;
		contextDesc.swapchainBufferCount = 3;

		SwapchainDesc swapchainDesc{};
		swapchainDesc.window = MakeWin32WindowHandle( hwnd );
		swapchainDesc.width = kInitialWidth;
		swapchainDesc.height = kInitialHeight;
		swapchainDesc.vsync = app.vsync;

		app.deviceManager = std::make_unique<DeviceManager>( contextDesc, swapchainDesc );
		app.imguiRenderer = std::make_unique<ImguiRenderer>( *app.deviceManager, swapchainDesc.window );
		app.imguiMessageReady = true;
		SetWindowLongPtr( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( &app ) );

		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		app.scenePipeline = CreateScenePipeline( ctx, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_D32_FLOAT );
		app.presentPipeline = CreatePresentPipeline( ctx, contextDesc.swapchainFormat );
		RecreateTargets( app );
		EnsureFsrContext( app );

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

			const auto now = std::chrono::steady_clock::now();
			float deltaSeconds = std::chrono::duration<float>( now - lastFrameTime ).count();
			lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );

			const float currentFrameMs = deltaSeconds * 1000.0f;
			app.smoothedFrameMs = app.smoothedFrameMs * 0.9f + currentFrameMs * 0.1f;
			app.smoothedFps = 1000.0f / std::max( app.smoothedFrameMs, 0.001f );

			app.previousAnimationTime = app.animationTime;
			if( !app.pauseAnimation )
			{
				app.animationTime += deltaSeconds * app.rotationSpeed;
			}
			else
			{
				app.previousAnimationTime = app.animationTime;
			}

			const uint32_t displayWidth = app.deviceManager->GetWidth();
			const uint32_t displayHeight = app.deviceManager->GetHeight();
			uint32_t previewRenderWidth = displayWidth;
			uint32_t previewRenderHeight = displayHeight;
			ComputeDesiredRenderResolution( app, displayWidth, displayHeight, previewRenderWidth, previewRenderHeight );

			app.imguiRenderer->NewFrame();
			ImGui::SetNextWindowPos( ImVec2( 20.0f, 20.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowSize( ImVec2( 460.0f, 0.0f ), ImGuiCond_FirstUseEver );
			ImGui::Begin( "Hello Upscaler" );
			ImGui::TextWrapped( "Didactic sample: the scene is rasterized at an internal resolution, motion vectors and depth are generated, and AMD FSR reconstructs the final image." );
			ImGui::Separator();
			ImGui::Text( "FPS: %.1f", app.smoothedFps );
			ImGui::Text( "Frame time: %.2f ms", app.smoothedFrameMs );
			ImGui::Checkbox( "Enable FSR upscaler", &app.fsrEnabled );
			ImGui::Checkbox( "Enable sharpening", &app.enableSharpening );
			ImGui::Checkbox( "Show upscaler debug view", &app.drawDebugView );
			ImGui::Checkbox( "Pause animation", &app.pauseAnimation );
			if( ImGui::Checkbox( "Enable VSync", &app.vsync ) )
			{
				app.deviceManager->SetVsync( app.vsync );
				app.requestHistoryReset = true;
			}
			ImGui::Combo( "FSR mode", &app.qualityModeIndex, ourUpscaleModeNames.data(), static_cast<int>( ourUpscaleModeNames.size() ) );
			ImGui::Combo( "View mode", &app.presentModeIndex, ourPresentModeNames.data(), static_cast<int>( ourPresentModeNames.size() ) );
			ImGui::SliderFloat( "Sharpness", &app.sharpness, 0.0f, 1.0f, "%.2f" );
			ImGui::SliderFloat( "Rotation speed", &app.rotationSpeed, 0.0f, 2.5f, "%.2f" );
			ImGui::SliderFloat( "Cube size", &app.cubeScale, 0.45f, 1.25f, "%.2f" );
			if( app.presentModeIndex == 1 || app.presentModeIndex == 3 )
			{
				ImGui::SliderFloat( "Split position", &app.splitPosition, 0.15f, 0.85f, "%.2f" );
			}
			if( app.presentModeIndex == 2 )
			{
				ImGui::SliderFloat( "Difference amplify", &app.differenceAmplify, 1.0f, 20.0f, "%.1f" );
			}
			if( app.presentModeIndex == 3 )
			{
				ImGui::SliderFloat( "Center zoom", &app.zoomFactor, 2.0f, 20.0f, "%.1f x" );
			}
			if( ImGui::Button( "Reset FSR history" ) )
			{
				app.requestHistoryReset = true;
			}

			ImGui::Separator();
			ImGui::Text( "Display: %u x %u", displayWidth, displayHeight );
			ImGui::Text( "Internal render: %u x %u", previewRenderWidth, previewRenderHeight );
			ImGui::Text( "Per-dimension ratio: %.2fx", static_cast<float>( displayWidth ) / static_cast<float>( previewRenderWidth ) );
			if( app.fsrEnabled )
			{
				ImGui::TextWrapped( "FSR enabled: the scene renders at a lower internal resolution and the SDK reconstructs the final image. Native AA keeps native resolution but still uses temporal reconstruction." );
				ImGui::Text( "Current jitter: (%.3f, %.3f)", app.lastJitterX, app.lastJitterY );
				ImGui::Text( "Approx. FSR VRAM: %.1f MB", app.fsr.estimatedMemoryUsageMb );
				if( app.presentModeIndex == 1 )
				{
					ImGui::TextWrapped( "Split mode: left = raw internal render with nearest sampling. Right = FSR result. This makes the difference easier to inspect." );
				}
				else if( app.presentModeIndex == 2 )
				{
					ImGui::TextWrapped( "Heatmap: blue = little change, yellow/red = FSR is reconstructing a lot compared with the raw internal render." );
				}
				else if( app.presentModeIndex == 3 )
				{
					ImGui::TextWrapped( "Zoom mode: magnified center inspection. Left = raw internal render with a pixel grid. Right = FSR output." );
				}
			}
			else
			{
				ImGui::TextWrapped( "FSR disabled: the sample renders at native resolution and presents the scene texture directly." );
			}

			if( !app.lastFfxMessage.empty() )
			{
				const ImVec4 warningColor = app.lastFfxMessageType == "Error" ? ImVec4( 0.95f, 0.35f, 0.32f, 1.0f ) :
					ImVec4( 0.95f, 0.82f, 0.22f, 1.0f );
				ImGui::Separator();
				ImGui::TextColored( warningColor, "SDK %s", app.lastFfxMessageType.c_str() );
				ImGui::TextWrapped( "%s", app.lastFfxMessage.c_str() );
			}
			ImGui::End();

			RecreateTargets( app );
			EnsureFsrContext( app );
			UpdateJitter( app );

			auto& commandBuffer = ctx.AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx.GetCurrentSwapchainTexture();
			const SceneMatrices sceneMatrices = BuildSceneMatrices( app );

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloUpscaler::ScenePass", 0xff4cc9f0u );

				RenderPass scenePass{};
				scenePass.color[ 0 ].loadOp = LoadOp::Clear;
				scenePass.color[ 0 ].clearColor = { 0.03f, 0.05f, 0.08f, 1.0f };
				scenePass.color[ 1 ].loadOp = LoadOp::Clear;
				scenePass.color[ 1 ].clearColor = { 0.0f, 0.0f, 0.0f, 0.0f };
				scenePass.depthStencil.depthLoadOp = LoadOp::Clear;
				scenePass.depthStencil.depthStoreOp = StoreOp::Store;
				scenePass.depthStencil.clearDepth = 1.0f;

				Framebuffer sceneFramebuffer{};
				sceneFramebuffer.color[ 0 ].texture = app.targets.sceneColor;
				sceneFramebuffer.color[ 1 ].texture = app.targets.motionVectors;
				sceneFramebuffer.depthStencil.texture = app.targets.sceneDepth;

				ScenePushConstants scenePushConstants{};
				CopyMatrix( sceneMatrices.currentJitteredMvp, scenePushConstants.currentJitteredMvp );
				CopyMatrix( sceneMatrices.currentUnjitteredMvp, scenePushConstants.currentUnjitteredMvp );
				CopyMatrix( sceneMatrices.previousUnjitteredMvp, scenePushConstants.previousUnjitteredMvp );
				scenePushConstants.time = app.animationTime;

				commandBuffer.CmdBeginRendering( scenePass, sceneFramebuffer );
				commandBuffer.CmdBindRenderPipeline( app.scenePipeline );
				commandBuffer.CmdPushConstants( &scenePushConstants, sizeof( scenePushConstants ) );
				commandBuffer.CmdDraw( 36 );
				commandBuffer.CmdEndRendering();

				commandBuffer.CmdTransitionTexture(
					app.targets.sceneColor,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
				commandBuffer.CmdTransitionTexture(
					app.targets.motionVectors,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
				commandBuffer.CmdTransitionTexture(
					app.targets.sceneDepth,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
			}

			//if( app.fsrEnabled )
			//{
			//	LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloUpscaler::FsrDispatch", 0xff90be6du );
			//	DispatchUpscaler( app, ctx, commandBuffer, deltaSeconds );
			//}

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloUpscaler::PresentPass", 0xfff9c74fu );

				RenderPass presentPass{};
				presentPass.color[ 0 ].loadOp = LoadOp::Clear;
				presentPass.color[ 0 ].clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };

				Framebuffer presentFramebuffer{};
				presentFramebuffer.color[ 0 ].texture = currentTexture;

				PresentPushConstants presentPushConstants{};
				presentPushConstants.sceneTextureIndex = ctx.GetBindlessIndex( app.targets.sceneColor );
				presentPushConstants.upscaledTextureIndex = ctx.GetBindlessIndex( app.fsrEnabled ? app.targets.upscaledOutput : app.targets.sceneColor );
				presentPushConstants.viewMode = static_cast<uint32_t>( std::clamp( app.presentModeIndex, 0, static_cast<int>( ourPresentModeNames.size() - 1 ) ) );
				presentPushConstants.upscalerEnabled = app.fsrEnabled ? 1u : 0u;
				presentPushConstants.splitPosition = app.splitPosition;
				presentPushConstants.zoomFactor = app.zoomFactor;
				presentPushConstants.differenceAmplify = app.differenceAmplify;
				presentPushConstants.renderWidth = app.targets.renderWidth;
				presentPushConstants.renderHeight = app.targets.renderHeight;

				commandBuffer.CmdBeginRendering( presentPass, presentFramebuffer );
				commandBuffer.CmdBindRenderPipeline( app.presentPipeline );
				commandBuffer.CmdPushConstants( &presentPushConstants, sizeof( presentPushConstants ) );
				commandBuffer.CmdDraw( 3 );
				commandBuffer.CmdEndRendering();
			}

			{
				LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloUpscaler::ImguiPass", 0xfff9844au );

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

		app.imguiMessageReady = false;
		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		DestroyFsrContext( app );
		ctx.WaitIdle();
		DestroyTargets( ctx, app.targets );
		app.scenePipeline = {};
		app.presentPipeline = {};
		app.imguiRenderer.reset();
		app.deviceManager.reset();
		ourFfxMessageSink = nullptr;
		DestroyWindow( hwnd );
	}
	catch( const std::exception& exception )
	{
		MessageBoxA( nullptr, exception.what(), "LightD3D12 Hello Upscaler", MB_ICONERROR | MB_OK );
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
