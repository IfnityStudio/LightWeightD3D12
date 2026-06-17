#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>

using namespace lightd3d12;

namespace
{
	constexpr uint32_t kFieldResolution = 5;
	constexpr uint32_t kArrowsPerAxis = kFieldResolution;
	constexpr uint32_t kArrowCount = kArrowsPerAxis * kArrowsPerAxis * kArrowsPerAxis;
	constexpr uint32_t kVerticesPerArrow = 6; // shaft + two small head strokes
	constexpr uint32_t kFieldVertexCount = kArrowCount * kVerticesPerArrow;

	struct VectorFieldPushConstants
	{
		float cameraPitch = 0.0f;
		float cameraYaw = 0.0f;
		float cameraDistance = 5.2f;
		float aspectRatio = 1.0f;

		float timeSeconds = 0.0f;
		float fieldScale = 0.28f;
		float fieldStrength = 1.0f;
		float animationSpeed = 0.0f;
		float fieldMode = 0.0f;

		float padding0 = 0.0f;
		float padding1 = 0.0f;
		float padding2 = 0.0f;
	};

	static_assert( sizeof( VectorFieldPushConstants ) / sizeof( uint32_t ) <= 63 );

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
		RenderPipelineState vectorFieldPipeline;
		DepthTarget depthTarget;
		float cameraPitch = -0.38f;
		float cameraYaw = 0.72f;
		float cameraDistance = 5.2f;
		float fieldScale = 0.28f;
		float fieldStrength = 1.0f;
		float animationSpeed = 0.0f;
		int fieldMode = 0;
		POINT lastMousePosition = {};
		bool hasLastMousePosition = false;
		bool running = true;
		bool minimized = false;
	};

	void ResetCamera( AppState& app )
	{
		app.cameraPitch = -0.38f;
		app.cameraYaw = 0.72f;
		app.cameraDistance = 5.2f;
		app.hasLastMousePosition = false;
	}

	void UpdateCameraInput( AppState& app, HWND hwnd )
	{
		POINT mousePosition = {};
		if( GetCursorPos( &mousePosition ) == FALSE || ScreenToClient( hwnd, &mousePosition ) == FALSE )
		{
			app.hasLastMousePosition = false;
			return;
		}

		RECT clientRect = {};
		GetClientRect( hwnd, &clientRect );
		const bool isInsideWindow = mousePosition.x >= clientRect.left && mousePosition.x < clientRect.right &&
			mousePosition.y >= clientRect.top && mousePosition.y < clientRect.bottom;
		const bool isLeftDown = ( GetAsyncKeyState( VK_LBUTTON ) & 0x8000 ) != 0;
		const bool isRightDown = ( GetAsyncKeyState( VK_RBUTTON ) & 0x8000 ) != 0;
		const bool imguiWantsMouse = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;

		if( !isInsideWindow || imguiWantsMouse || ( !isLeftDown && !isRightDown ) )
		{
			app.hasLastMousePosition = false;
			return;
		}

		if( !app.hasLastMousePosition )
		{
			app.lastMousePosition = mousePosition;
			app.hasLastMousePosition = true;
			return;
		}

		const float deltaX = static_cast<float>( mousePosition.x - app.lastMousePosition.x );
		const float deltaY = static_cast<float>( mousePosition.y - app.lastMousePosition.y );
		app.lastMousePosition = mousePosition;

		if( isRightDown )
		{
			app.cameraDistance = std::clamp( app.cameraDistance + deltaY * 0.02f, 2.0f, 12.0f );
			return;
		}

		if( isLeftDown )
		{
			app.cameraYaw += deltaX * 0.006f;
			app.cameraPitch = std::clamp( app.cameraPitch + deltaY * 0.006f, -1.45f, 1.45f );
		}
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
		depthDesc.debugName = "Vector Fields 3D Depth";
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

	RenderPipelineState CreateVectorFieldPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat )
	{
		static constexpr char vertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    float gCameraPitch;
    float gCameraYaw;
    float gCameraDistance;
    float gAspectRatio;

    float gTimeSeconds;
    float gFieldScale;
    float gFieldStrength;
    float gAnimationSpeed;
    float gFieldMode;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

float3 WorldToView(float3 p)
{
    const float cx = cos(gCameraPitch);
    const float sx = sin(gCameraPitch);
    const float cy = cos(gCameraYaw);
    const float sy = sin(gCameraYaw);

    p = float3(p.x, p.y * cx - p.z * sx, p.y * sx + p.z * cx);
    p = float3(p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy);
    return p;
}

float3 EvaluateVectorField(float3 p)
{
    const float t = gTimeSeconds * gAnimationSpeed;

    if (gFieldMode < 0.5)
    {
        // Swirl around the Y axis.
        return normalize(float3(-p.z, 0.35 * sin(p.x * 2.0 + t), p.x) + 0.0001);
    }

    if (gFieldMode < 1.5)
    {
        // Sink/source style field pointing toward the center.
        return normalize(-p + 0.25 * float3(sin(t), cos(t * 0.7), sin(t * 1.3)) + 0.0001);
    }

    // Wave field: useful to see how neighbouring vectors can vary smoothly.
    return normalize(float3(
        sin(p.y * 2.4 + t),
        cos(p.z * 2.1 + t),
        sin(p.x * 2.0 - t)) + 0.0001);
}

float4 Project(float3 worldPosition)
{
    float3 viewPosition = WorldToView(worldPosition);
    viewPosition.z += gCameraDistance;

    const float perspective = 1.7 / max(viewPosition.z, 0.1);
    const float2 clipXY = float2(viewPosition.x * perspective / gAspectRatio, viewPosition.y * perspective);
    const float clipZ = saturate((viewPosition.z - 0.1) / 16.0);
    return float4(clipXY, clipZ, 1.0);
}

VSOutput main(uint vertexID : SV_VertexID)
{
    const uint verticesPerArrow = 6;
    const uint resolution = 5;
    const uint arrowID = vertexID / verticesPerArrow;
    const uint vertexInArrow = vertexID % verticesPerArrow;

    const uint x = arrowID % resolution;
    const uint y = (arrowID / resolution) % resolution;
    const uint z = arrowID / (resolution * resolution);

    const float gridScale = 2.0 / float(resolution - 1);
    const float3 basePosition = float3(x, y, z) * gridScale - 1.0;

    float3 direction = EvaluateVectorField(basePosition) * gFieldStrength;
    const float magnitude = saturate(length(direction));
    direction = normalize(direction + 0.0001);

    const float arrowLength = gFieldScale * (0.35 + 0.65 * magnitude);
    const float3 start = basePosition - direction * arrowLength * 0.35;
    const float3 end = basePosition + direction * arrowLength * 0.65;

    float3 up = abs(direction.y) > 0.92 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    float3 side = normalize(cross(direction, up));
    const float3 headA = end - direction * arrowLength * 0.32 + side * arrowLength * 0.16;
    const float3 headB = end - direction * arrowLength * 0.32 - side * arrowLength * 0.16;

    float3 arrowPoint = start;
    if (vertexInArrow == 1) arrowPoint = end;
    if (vertexInArrow == 2) arrowPoint = end;
    if (vertexInArrow == 3) arrowPoint = headA;
    if (vertexInArrow == 4) arrowPoint = end;
    if (vertexInArrow == 5) arrowPoint = headB;

    VSOutput output;
    output.position = Project(arrowPoint);

    const float heightTint = saturate(basePosition.y * 0.5 + 0.5);
    output.color = lerp(float3(0.18, 0.55, 1.0), float3(1.0, 0.72, 0.22), magnitude);
    output.color = lerp(output.color, float3(0.35, 1.0, 0.55), heightTint * 0.25);
    return output;
}
)";

		static constexpr char pixelShader[] = R"(
float4 main(float4 position : SV_Position, float3 color : COLOR0) : SV_Target0
{
    return float4(color, 1.0);
}
)";

		RenderPipelineDesc desc{};
		desc.vertexShader.source = vertexShader;
		desc.vertexShader.entryPoint = "main";
		desc.vertexShader.profile = "vs_6_6";
		desc.fragmentShader.source = pixelShader;
		desc.fragmentShader.entryPoint = "main";
		desc.fragmentShader.profile = "ps_6_6";
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = depthFormat;
		desc.primitiveType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		desc.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		desc.depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
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
	HWND hwnd = nullptr;

	try
	{
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = L"LightD3D12VectorFields3DWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t initialWidth = 1280;
		constexpr uint32_t initialHeight = 720;
		hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Vector Fields 3D",
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
		app.vectorFieldPipeline = CreateVectorFieldPipeline(
			*app.deviceManager->GetRenderDevice(),
			contextDesc.swapchainFormat,
			DXGI_FORMAT_D32_FLOAT );
		RecreateDepthTarget( app );

		const auto startTime = std::chrono::steady_clock::now();
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

			RecreateDepthTarget( app );

			const float elapsedSeconds = std::chrono::duration<float>( std::chrono::steady_clock::now() - startTime ).count();

			if( app.imguiRenderer )
			{
				app.imguiRenderer->NewFrame();
				UpdateCameraInput( app, hwnd );
				ImGui::SetNextWindowPos( ImVec2( 16.0f, 16.0f ), ImGuiCond_Once );
				ImGui::SetNextWindowSize( ImVec2( 430.0f, 0.0f ), ImGuiCond_Once );
				ImGui::Begin( "Vector Fields 3D" );
				ImGui::TextWrapped( "A vector field returns a direction for any 3D point. This sample visualizes that field with arrows." );
				ImGui::Separator();
				const char* fieldModes[] = { "Swirl", "Sink", "Wave" };
				ImGui::Combo( "Field function", &app.fieldMode, fieldModes, IM_ARRAYSIZE( fieldModes ) );
				ImGui::SliderFloat( "Arrow scale", &app.fieldScale, 0.05f, 0.55f );
				ImGui::SliderFloat( "Field strength", &app.fieldStrength, 0.1f, 2.5f );
				ImGui::SliderFloat( "Animation speed", &app.animationSpeed, 0.0f, 4.0f );
				ImGui::Separator();
				ImGui::Text( "Camera controls" );
				ImGui::TextWrapped( "Left mouse drag: rotate the camera. Right mouse drag: zoom in/out." );
				ImGui::Text( "Yaw %.2f  Pitch %.2f  Distance %.2f", app.cameraYaw, app.cameraPitch, app.cameraDistance );
				if( ImGui::Button( "Reset camera" ) )
				{
					ResetCamera( app );
				}
				ImGui::Separator();
				ImGui::Text( "Grid: %u x %u x %u", kArrowsPerAxis, kArrowsPerAxis, kArrowsPerAxis );
				ImGui::Text( "Arrows: %u", kArrowCount );
				ImGui::TextWrapped( "Next step: replace EvaluateVectorField(p) with a 3D texture, a simulation buffer, or CPU-authored field data." );
				ImGui::End();
			}
			else
			{
				UpdateCameraInput( app, hwnd );
			}

			VectorFieldPushConstants pushConstants{};
			pushConstants.cameraPitch = app.cameraPitch;
			pushConstants.cameraYaw = app.cameraYaw;
			pushConstants.cameraDistance = app.cameraDistance;
			pushConstants.aspectRatio = static_cast<float>( app.deviceManager->GetWidth() ) /
				static_cast<float>( std::max( 1u, app.deviceManager->GetHeight() ) );
			pushConstants.timeSeconds = elapsedSeconds;
			pushConstants.fieldScale = app.fieldScale;
			pushConstants.fieldStrength = app.fieldStrength;
			pushConstants.animationSpeed = app.animationSpeed;
			pushConstants.fieldMode = static_cast<float>( app.fieldMode );

			ICommandBuffer& buffer = ctx->AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx->GetCurrentSwapchainTexture();

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.025f, 0.03f, 0.04f, 1.0f };
			renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
			renderPass.depthStencil.depthStoreOp = StoreOp::Store;
			renderPass.depthStencil.clearDepth = 1.0f;

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;
			framebuffer.depthStencil.texture = app.depthTarget.texture;

			buffer.CmdBeginRendering( renderPass, framebuffer );
			buffer.CmdBindRenderPipeline( app.vectorFieldPipeline );
			buffer.CmdPushDebugGroupLabel( "Render Vector Field", 0xff3fc1ff );
			buffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			buffer.CmdDraw( kFieldVertexCount );
			buffer.CmdPopDebugGroupLabel();

			if( app.imguiRenderer )
			{
				app.imguiRenderer->Render( buffer );
			}

			buffer.CmdEndRendering();
			ctx->Submit( buffer, currentTexture );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		if( app.deviceManager )
		{
			app.deviceManager->WaitIdle();
			if( app.depthTarget.texture.Valid() )
			{
				app.deviceManager->GetRenderDevice()->Destroy( app.depthTarget.texture );
				app.depthTarget.texture = {};
			}
		}

		app.vectorFieldPipeline = {};
		app.imguiRenderer.reset();
		app.deviceManager.reset();
		if( IsWindow( hwnd ) != FALSE )
		{
			DestroyWindow( hwnd );
		}
		UnregisterClassW( windowClass.lpszClassName, instance );
		return 0;
	}
	catch( const std::exception& error )
	{
		if( hwnd != nullptr && IsWindow( hwnd ) != FALSE )
		{
			SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		}
		MessageBoxA( nullptr, error.what(), "LightD3D12 Vector Fields 3D failed", MB_ICONERROR | MB_OK );
		return 1;
	}
}
