#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <shellapi.h>

using namespace lightd3d12;

namespace
{
	struct ComputePushConstants
	{
		uint32_t outputTextureIndex = 0;
		uint32_t textureWidth = 0;
		uint32_t textureHeight = 0;
		uint32_t viewMode = 0;
		uint32_t dispatchGroupCountX = 1;
		uint32_t dispatchGroupCountY = 1;
		uint32_t threadsPerGroupX = 1;
		uint32_t threadsPerGroupY = 1;
		float time = 0.0f;
		uint32_t showGroupGrid = 1;
		uint32_t animatePalette = 0;
		uint32_t padding = 0;
	};

	static_assert( sizeof( ComputePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct ComputeGroupPreset
	{
		const char* label = "";
		uint32_t threadCountX = 8;
		uint32_t threadCountY = 8;
	};

	constexpr std::array<ComputeGroupPreset, 4> ourGroupPresets = {
		ComputeGroupPreset{ "4 x 4 threads", 4, 4 },
		ComputeGroupPreset{ "8 x 8 threads", 8, 8 },
		ComputeGroupPreset{ "16 x 16 threads", 16, 16 },
		ComputeGroupPreset{ "32 x 8 threads", 32, 8 },
	};

	constexpr std::array<const char*, 5> ourViewModeLabels = {
		"DispatchThreadID",
		"GroupID",
		"GroupThreadID",
		"GroupIndex",
		"Group Pattern",
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		std::array<ComputePipelineState, ourGroupPresets.size()> computePipelines;
		TextureHandle outputTexture = {};
		uint32_t outputWidth = 0;
		uint32_t outputHeight = 0;
		int presetIndex = 1;
		int dispatchGroupCountX = 12;
		int dispatchGroupCountY = 10;
		int viewModeIndex = 0;
		bool outputDirty = true;
		bool running = true;
		bool minimized = false;
		bool pauseAnimation = false;
		bool showGroupGrid = true;
		bool animatePalette = true;
		float simulationTime = 0.0f;
	};

	struct HoveredThreadInfo
	{
		bool valid = false;
		uint32_t dispatchThreadX = 0;
		uint32_t dispatchThreadY = 0;
		uint32_t groupX = 0;
		uint32_t groupY = 0;
		uint32_t groupThreadX = 0;
		uint32_t groupThreadY = 0;
		uint32_t groupIndex = 0;
	};

	const char* GetGroupPresetLabel( void*, int index )
	{
		if( index < 0 || index >= static_cast<int>( ourGroupPresets.size() ) )
		{
			return nullptr;
		}

		return ourGroupPresets[ static_cast<size_t>( index ) ].label;
	}

	bool CommandLineHasFlag( const wchar_t* commandLine, const wchar_t* flag )
	{
		if( commandLine == nullptr || flag == nullptr )
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

	std::string BuildComputeShaderSource( const ComputeGroupPreset& preset )
	{
		std::string source = R"(
cbuffer PushConstants : register(b0)
{
    uint gOutputTextureIndex;
    uint gTextureWidth;
    uint gTextureHeight;
    uint gViewMode;
    uint gDispatchGroupCountX;
    uint gDispatchGroupCountY;
    uint gThreadsPerGroupX;
    uint gThreadsPerGroupY;
    float gTime;
    uint gShowGroupGrid;
    uint gAnimatePalette;
    uint gPadding;
};

float3 HueToRgb(float hue)
{
    const float3 k = float3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    const float3 p = abs(frac(hue + k) * 6.0 - 3.0);
    return saturate(p - 1.0);
}

float3 MakeGroupColor(uint groupLinearIndex, uint totalGroupCount, float timeOffset, uint animatePalette)
{
    const float normalized = (groupLinearIndex + 0.5) / max((float)totalGroupCount, 1.0);
    const float animatedHue = normalized + (animatePalette != 0 ? timeOffset * 0.08 : 0.0);
    return lerp(float3(0.08, 0.10, 0.14), HueToRgb(frac(animatedHue)), 0.92);
}

[numthreads()";
		source += std::to_string( preset.threadCountX );
		source += ", ";
		source += std::to_string( preset.threadCountY );
		source += R"(, 1)]
void main(
    uint3 dispatchThreadID : SV_DispatchThreadID,
    uint3 groupID : SV_GroupID,
    uint3 groupThreadID : SV_GroupThreadID,
    uint groupIndex : SV_GroupIndex)
{
    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[gOutputTextureIndex];
    if (dispatchThreadID.x >= gTextureWidth || dispatchThreadID.y >= gTextureHeight)
    {
        return;
    }

    const uint totalGroupCount = max(gDispatchGroupCountX * gDispatchGroupCountY, 1u);
    const uint threadsPerGroup = max(gThreadsPerGroupX * gThreadsPerGroupY, 1u);
    const uint groupLinearIndex = groupID.x + groupID.y * gDispatchGroupCountX;
    const float normalizedDispatchX = (dispatchThreadID.x + 0.5) / max((float)gTextureWidth, 1.0);
    const float normalizedDispatchY = (dispatchThreadID.y + 0.5) / max((float)gTextureHeight, 1.0);
    const float normalizedGroupThreadX = (groupThreadID.x + 0.5) / max((float)gThreadsPerGroupX, 1.0);
    const float normalizedGroupThreadY = (groupThreadID.y + 0.5) / max((float)gThreadsPerGroupY, 1.0);
    const float normalizedGroupIndex = (groupIndex + 0.5) / max((float)threadsPerGroup, 1.0);

    float3 color = float3(0.1, 0.12, 0.15);

    switch (gViewMode)
    {
        case 0:
            color = float3(normalizedDispatchX, normalizedDispatchY, 0.18 + 0.72 * normalizedGroupIndex);
            break;

        case 1:
            color = MakeGroupColor(groupLinearIndex, totalGroupCount, gTime, gAnimatePalette);
            break;

        case 2:
            color = float3(normalizedGroupThreadX, normalizedGroupThreadY, normalizedGroupIndex);
            break;

        case 3:
            color = HueToRgb(frac(normalizedGroupIndex + (gAnimatePalette != 0 ? gTime * 0.16 : 0.0)));
            break;

        default:
        {
            const float checker = ((groupID.x + groupID.y) & 1u) == 0u ? 0.22 : 0.34;
            color = lerp(float3(checker, checker, checker), MakeGroupColor(groupLinearIndex, totalGroupCount, gTime, gAnimatePalette), 0.88);
            break;
        }
    }

    // Highlight group boundaries so the relationship between Dispatch and numthreads is visible.
    if (gShowGroupGrid != 0u && (groupThreadID.x == 0u || groupThreadID.y == 0u))
    {
        color = lerp(color, float3(1.0, 1.0, 1.0), 0.68);
    }

    outputTexture[dispatchThreadID.xy] = float4(color, 1.0);
}
)";
		return source;
	}

	ComputePipelineState CreateComputePipelinePreset( RenderDevice& ctx, const ComputeGroupPreset& preset )
	{
		const std::string source = BuildComputeShaderSource( preset );

		ComputePipelineDesc desc{};
		desc.computeShader.source = source.c_str();
		desc.computeShader.entryPoint = "main";
		desc.computeShader.profile = "cs_6_6";
		return ctx.CreateComputePipeline( desc );
	}

	void DestroyOutputTexture( RenderDevice& ctx, AppState& app )
	{
		if( app.outputTexture.Valid() )
		{
			ctx.Destroy( app.outputTexture );
			app.outputTexture = {};
		}

		app.outputWidth = 0;
		app.outputHeight = 0;
	}

	void RecreateOutputTexture( AppState& app )
	{
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		const ComputeGroupPreset& preset = ourGroupPresets[ static_cast<size_t>( app.presetIndex ) ];
		const uint32_t targetWidth = static_cast<uint32_t>( app.dispatchGroupCountX ) * preset.threadCountX;
		const uint32_t targetHeight = static_cast<uint32_t>( app.dispatchGroupCountY ) * preset.threadCountY;

		if( app.outputTexture.Valid() && app.outputWidth == targetWidth && app.outputHeight == targetHeight )
		{
			app.outputDirty = false;
			return;
		}

		DestroyOutputTexture( ctx, app );

		TextureDesc desc{};
		desc.debugName = "HelloCompute Output";
		desc.width = targetWidth;
		desc.height = targetHeight;
		desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.usage = TextureUsage::Sampled | TextureUsage::UnorderedAccess;
		desc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		app.outputTexture = ctx.CreateTexture( desc );
		app.outputWidth = targetWidth;
		app.outputHeight = targetHeight;
		app.outputDirty = false;
	}

	HoveredThreadInfo BuildHoveredThreadInfo( const AppState& app, const ImVec2 imageOrigin, const ImVec2 imageSize )
	{
		HoveredThreadInfo info{};
		if( app.outputWidth == 0 || app.outputHeight == 0 )
		{
			return info;
		}

		if( !ImGui::IsItemHovered() || imageSize.x <= 0.0f || imageSize.y <= 0.0f )
		{
			return info;
		}

		const ImVec2 mousePosition = ImGui::GetIO().MousePos;
		const float localX = std::clamp( mousePosition.x - imageOrigin.x, 0.0f, std::max( imageSize.x - 0.001f, 0.0f ) );
		const float localY = std::clamp( mousePosition.y - imageOrigin.y, 0.0f, std::max( imageSize.y - 0.001f, 0.0f ) );
		info.dispatchThreadX = std::min<uint32_t>( static_cast<uint32_t>( localX / imageSize.x * static_cast<float>( app.outputWidth ) ), app.outputWidth - 1u );
		info.dispatchThreadY = std::min<uint32_t>( static_cast<uint32_t>( localY / imageSize.y * static_cast<float>( app.outputHeight ) ), app.outputHeight - 1u );

		const ComputeGroupPreset& preset = ourGroupPresets[ static_cast<size_t>( app.presetIndex ) ];
		info.groupX = info.dispatchThreadX / preset.threadCountX;
		info.groupY = info.dispatchThreadY / preset.threadCountY;
		info.groupThreadX = info.dispatchThreadX % preset.threadCountX;
		info.groupThreadY = info.dispatchThreadY % preset.threadCountY;
		info.groupIndex = info.groupThreadY * preset.threadCountX + info.groupThreadX;
		info.valid = true;
		return info;
	}

	void RecordComputePass( ICommandBuffer& commandBuffer, AppState& app )
	{
		const ComputeGroupPreset& preset = ourGroupPresets[ static_cast<size_t>( app.presetIndex ) ];
		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();

		ComputePushConstants pushConstants{};
		pushConstants.outputTextureIndex = ctx.GetUnorderedAccessIndex( app.outputTexture );
		pushConstants.textureWidth = app.outputWidth;
		pushConstants.textureHeight = app.outputHeight;
		pushConstants.viewMode = static_cast<uint32_t>( app.viewModeIndex );
		pushConstants.dispatchGroupCountX = static_cast<uint32_t>( app.dispatchGroupCountX );
		pushConstants.dispatchGroupCountY = static_cast<uint32_t>( app.dispatchGroupCountY );
		pushConstants.threadsPerGroupX = preset.threadCountX;
		pushConstants.threadsPerGroupY = preset.threadCountY;
		pushConstants.time = app.simulationTime;
		pushConstants.showGroupGrid = app.showGroupGrid ? 1u : 0u;
		pushConstants.animatePalette = app.animatePalette ? 1u : 0u;

		LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloCompute::RecordComputePass", 0xff4cc9f0u );

		// The texture is shown in ImGui as an SRV, so switch it to UAV for compute writes first.
		commandBuffer.CmdTransitionTexture( app.outputTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
		commandBuffer.CmdBindComputePipeline( app.computePipelines[ static_cast<size_t>( app.presetIndex ) ] );
		commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );

		// Each dispatched thread writes exactly one pixel, which makes thread IDs easy to inspect.
		commandBuffer.CmdDispatch(
			static_cast<uint32_t>( app.dispatchGroupCountX ),
			static_cast<uint32_t>( app.dispatchGroupCountY ),
			1 );

		// Move the same texture back to SRV state so ImGui can sample it on the backbuffer pass.
		commandBuffer.CmdTransitionTexture( app.outputTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	}

	void RecordImguiPass( ICommandBuffer& commandBuffer, TextureHandle backbuffer, ImguiRenderer& imguiRenderer )
	{
		LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "HelloCompute::RecordImguiPass", 0xfff9c74fu );

		RenderPass renderPass{};
		renderPass.color[ 0 ].loadOp = LoadOp::Clear;
		renderPass.color[ 0 ].clearColor = { 0.06f, 0.08f, 0.11f, 1.0f };

		Framebuffer framebuffer{};
		framebuffer.color[ 0 ].texture = backbuffer;

		commandBuffer.CmdBeginRendering( renderPass, framebuffer );
		imguiRenderer.Render( commandBuffer );
		commandBuffer.CmdEndRendering();
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
		windowClass.lpszClassName = L"LightD3D12HelloComputeWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1440;
		constexpr uint32_t kInitialHeight = 900;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Hello Compute",
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
		contextDesc.enablePixGpuCapture = CommandLineHasFlag( GetCommandLineW(), L"--pix" );
		contextDesc.swapchainBufferCount = 3;

		SwapchainDesc swapchainDesc{};
		swapchainDesc.window = MakeWin32WindowHandle( hwnd );
		swapchainDesc.width = kInitialWidth;
		swapchainDesc.height = kInitialHeight;
		swapchainDesc.vsync = true;

		app.deviceManager = std::make_unique<DeviceManager>( contextDesc, swapchainDesc );
		app.imguiRenderer = std::make_unique<ImguiRenderer>( *app.deviceManager, swapchainDesc.window );

		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		for( size_t index = 0; index < ourGroupPresets.size(); ++index )
		{
			app.computePipelines[ index ] = CreateComputePipelinePreset( ctx, ourGroupPresets[ index ] );
		}
		RecreateOutputTexture( app );

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

			const auto now = std::chrono::steady_clock::now();
			float deltaSeconds = std::chrono::duration<float>( now - lastFrameTime ).count();
			lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );
			if( !app.pauseAnimation )
			{
				app.simulationTime += deltaSeconds;
			}

			if( app.outputDirty )
			{
				RecreateOutputTexture( app );
			}

			HoveredThreadInfo hoveredThread{};
			if( app.imguiRenderer )
			{
				app.imguiRenderer->NewFrame();

				const ComputeGroupPreset& preset = ourGroupPresets[ static_cast<size_t>( app.presetIndex ) ];
				const uint32_t totalThreadCount = app.outputWidth * app.outputHeight;
				const uint32_t threadsPerGroup = preset.threadCountX * preset.threadCountY;

				ImGui::SetNextWindowPos( ImVec2( 16.0f, 16.0f ), ImGuiCond_FirstUseEver );
				ImGui::SetNextWindowSize( ImVec2( 460.0f, 0.0f ), ImGuiCond_FirstUseEver );
				ImGui::Begin( "Hello Compute" );
				ImGui::TextWrapped( "This sample generates the image entirely in a compute shader. ImGui only presents the SRV produced by the compute pass." );
				ImGui::Separator();
				ImGui::Text( "FPS: %.1f", ImGui::GetIO().Framerate );
				ImGui::Text( "Texture: %u x %u", app.outputWidth, app.outputHeight );
				ImGui::Text( "Dispatch(%d, %d, 1)", app.dispatchGroupCountX, app.dispatchGroupCountY );
				ImGui::Text( "[numthreads(%u, %u, 1)]", preset.threadCountX, preset.threadCountY );
				ImGui::Text( "Threads per group: %u", threadsPerGroup );
				ImGui::Text( "Total dispatched threads: %u", totalThreadCount );

				int presetIndex = app.presetIndex;
				if( ImGui::Combo( "Group preset", &presetIndex, &GetGroupPresetLabel, nullptr, static_cast<int>( ourGroupPresets.size() ) ) )
				{
					app.presetIndex = presetIndex;
					app.outputDirty = true;
				}

				if( ImGui::SliderInt( "Dispatch groups X", &app.dispatchGroupCountX, 1, 32 ) )
				{
					app.outputDirty = true;
				}
				if( ImGui::SliderInt( "Dispatch groups Y", &app.dispatchGroupCountY, 1, 32 ) )
				{
					app.outputDirty = true;
				}

				ImGui::Combo( "Visualization", &app.viewModeIndex, ourViewModeLabels.data(), static_cast<int>( ourViewModeLabels.size() ) );
				ImGui::Checkbox( "Show group grid", &app.showGroupGrid );
				ImGui::Checkbox( "Animate palette", &app.animatePalette );
				ImGui::SameLine();
				ImGui::Checkbox( "Pause animation", &app.pauseAnimation );

				if( ImGui::Button( "Reset to 8 x 8 groups" ) )
				{
					app.dispatchGroupCountX = 8;
					app.dispatchGroupCountY = 8;
					app.outputDirty = true;
				}

				ImGui::Separator();
				ImGui::TextUnformatted( "Hovered thread inspection" );
				if( app.outputTexture.Valid() )
				{
					const D3D12_GPU_DESCRIPTOR_HANDLE previewHandle = app.imguiRenderer->GetTextureGpuDescriptor( app.outputTexture );
					const ImVec2 available = ImGui::GetContentRegionAvail();
					const float scaleX = std::max( available.x, 64.0f ) / std::max( static_cast<float>( app.outputWidth ), 1.0f );
					const float scaleY = std::max( available.y, 240.0f ) / std::max( static_cast<float>( app.outputHeight ), 1.0f );
					const float scale = std::max( 0.25f, std::min( scaleX, scaleY ) );
					const ImVec2 imageSize(
						static_cast<float>( app.outputWidth ) * scale,
						static_cast<float>( app.outputHeight ) * scale );

					ImGui::Image( static_cast<ImTextureID>( previewHandle.ptr ), imageSize );
					hoveredThread = BuildHoveredThreadInfo( app, ImGui::GetItemRectMin(), ImGui::GetItemRectSize() );
				}

				if( hoveredThread.valid )
				{
					ImGui::Text( "DispatchThreadID: (%u, %u)", hoveredThread.dispatchThreadX, hoveredThread.dispatchThreadY );
					ImGui::Text( "GroupID: (%u, %u)", hoveredThread.groupX, hoveredThread.groupY );
					ImGui::Text( "GroupThreadID: (%u, %u)", hoveredThread.groupThreadX, hoveredThread.groupThreadY );
					ImGui::Text( "GroupIndex: %u", hoveredThread.groupIndex );
				}
				else
				{
					ImGui::TextUnformatted( "Hover the image to inspect the current thread coordinates." );
				}

				ImGui::End();
			}

			auto& commandBuffer = ctx->AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx->GetCurrentSwapchainTexture();

			RecordComputePass( commandBuffer, app );
			RecordImguiPass( commandBuffer, currentTexture, *app.imguiRenderer );

			ctx->Submit( commandBuffer, currentTexture );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		if( app.deviceManager )
		{
			app.deviceManager->WaitIdle();
			RenderDevice* renderDevice = app.deviceManager->GetRenderDevice();
			DestroyOutputTexture( *renderDevice, app );
		}
		for( auto& pipeline : app.computePipelines )
		{
			pipeline = {};
		}
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
		MessageBoxA( nullptr, "LightD3D12 HelloCompute failed.", "LightD3D12", MB_ICONERROR | MB_OK );
		return 1;
	}
}
