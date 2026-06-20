#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <DirectXMath.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>

using namespace DirectX;
using namespace lightd3d12;

namespace
{
	constexpr uint32_t kSphereSlices = 48;
	constexpr uint32_t kSphereStacks = 24;
	constexpr uint32_t kSphereVertexCount = kSphereSlices * kSphereStacks * 6;

	struct ShPushConstants
	{
		XMFLOAT4 sh0;
		XMFLOAT4 shY;
		XMFLOAT4 shZ;
		XMFLOAT4 shX;
		XMFLOAT4 lightDir0;
		XMFLOAT4 lightDir1;
		XMFLOAT4 lightDir2;
		XMFLOAT4 lightColor0;
		XMFLOAT4 lightColor1;
		XMFLOAT4 lightColor2;
		XMFLOAT4 params; // normalRotationX, normalRotationY, aspect, exposure
		XMFLOAT4 mode; // 0 direct, 1 SH, 2 split comparison
	};

	static_assert( sizeof( ShPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct ShRgb
	{
		std::array<XMFLOAT3, 4> coeffs{};
	};

	struct LightControl
	{
		XMFLOAT3 color;
		float intensity;
		float yawDeg;
		float pitchDeg;
		bool enabled;
	};

	struct DepthTarget
	{
		TextureHandle texture = {};
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct AppState
	{
		DeviceManager* deviceManager = nullptr;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState pipeline;
		DepthTarget depthTarget;
		std::array<LightControl, 3> lights = {
			LightControl{ XMFLOAT3{ 1.0f, 0.78f, 0.48f }, 2.7f, -35.0f, 28.0f, true },
			LightControl{ XMFLOAT3{ 0.18f, 0.45f, 1.0f }, 1.0f, 120.0f, 12.0f, false },
			LightControl{ XMFLOAT3{ 0.45f, 1.0f, 0.36f }, 0.7f, 210.0f, -8.0f, false },
		};
		float rotationX = -12.0f;
		float rotationY = 24.0f;
		float exposure = 1.25f;
		int mode = 2;
		bool running = true;
		bool minimized = false;
	};

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
		depthDesc.debugName = "Spherical Harmonics Mesh Depth";
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

	XMFLOAT3 Add( const XMFLOAT3& a, const XMFLOAT3& b )
	{
		return { a.x + b.x, a.y + b.y, a.z + b.z };
	}

	XMFLOAT3 Mul( const XMFLOAT3& a, float s )
	{
		return { a.x * s, a.y * s, a.z * s };
	}

	XMFLOAT3 DirectionFromAngles( float yawDeg, float pitchDeg )
	{
		const float yaw = XMConvertToRadians( yawDeg );
		const float pitch = XMConvertToRadians( pitchDeg );
		const float cp = std::cos( pitch );
		return { std::cos( yaw ) * cp, std::sin( pitch ), std::sin( yaw ) * cp };
	}

	std::array<float, 4> EvalShBasisL1( const XMFLOAT3& dir )
	{
		constexpr float c0 = 0.2820947918f;
		constexpr float c1 = 0.4886025119f;
		return { c0, c1 * dir.y, c1 * dir.z, c1 * dir.x };
	}

	ShRgb ProjectLightsToSh( const std::array<LightControl, 3>& lights )
	{
		constexpr float pi = 3.1415926535f;
		ShRgb radiance;

		for( const LightControl& light : lights )
		{
			if( !light.enabled )
			{
				continue;
			}

			const XMFLOAT3 dir = DirectionFromAngles( light.yawDeg, light.pitchDeg );
			const XMFLOAT3 radianceColor = Mul( light.color, light.intensity );
			const std::array<float, 4> basis = EvalShBasisL1( dir );
			for( size_t i = 0; i != basis.size(); ++i )
			{
				radiance.coeffs[ i ] = Add( radiance.coeffs[ i ], Mul( radianceColor, basis[ i ] ) );
			}
		}

		// Convert radiance coefficients to diffuse irradiance coefficients.
		// Band 0 keeps average light. Band 1 keeps broad directionality.
		ShRgb irradiance = radiance;
		irradiance.coeffs[ 0 ] = Mul( irradiance.coeffs[ 0 ], pi );
		for( size_t i = 1; i != irradiance.coeffs.size(); ++i )
		{
			irradiance.coeffs[ i ] = Mul( irradiance.coeffs[ i ], 2.0f * pi / 3.0f );
		}
		return irradiance;
	}

	XMFLOAT4 ToFloat4( const XMFLOAT3& v, float w = 0.0f )
	{
		return { v.x, v.y, v.z, w };
	}

	RenderPipelineState CreatePipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat )
	{
		static constexpr char vertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    float4 gSh0;
    float4 gShY;
    float4 gShZ;
    float4 gShX;
    float4 gLightDir0;
    float4 gLightDir1;
    float4 gLightDir2;
    float4 gLightColor0;
    float4 gLightColor1;
    float4 gLightColor2;
    float4 gParams;
    float4 gMode;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    float2 screenHint : TEXCOORD0;
};

float2 GetSphereUv(uint vertexID)
{
    const uint slices = 48;
    const uint stacks = 24;
    const uint tri = vertexID / 3;
    const uint local = vertexID % 3;
    const uint cell = tri / 2;
    const uint triInCell = tri % 2;
    const uint stack = cell / slices;
    const uint slice = cell % slices;

    const float u0 = float(slice) / float(slices);
    const float u1 = float(slice + 1) / float(slices);
    const float v0 = float(stack) / float(stacks);
    const float v1 = float(stack + 1) / float(stacks);

    if (triInCell == 0)
    {
        if (local == 0) return float2(u0, v0);
        if (local == 1) return float2(u0, v1);
        return float2(u1, v1);
    }

    if (local == 0) return float2(u0, v0);
    if (local == 1) return float2(u1, v1);
    return float2(u1, v0);
}

float3 Rotate(float3 p)
{
    const float sx = sin(gParams.x);
    const float cx = cos(gParams.x);
    const float sy = sin(gParams.y);
    const float cy = cos(gParams.y);

    p = float3(p.x, p.y * cx - p.z * sx, p.y * sx + p.z * cx);
    p = float3(p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy);
    return p;
}

VSOutput main(uint vertexID : SV_VertexID)
{
    const float2 uv = GetSphereUv(vertexID);
    const float theta = uv.y * 3.14159265;
    const float phi = uv.x * 6.2831853;

    float3 localNormal = normalize(float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi)));

    // Keep the mesh fixed. Rotate only the normal used to query lighting so the debug view is easy to read.
    float3 worldPosition = localNormal * 1.65;
    worldPosition.z += 4.6;

    const float perspective = 1.65 / worldPosition.z;
    const float2 clipXY = float2(worldPosition.x * perspective / gParams.z, worldPosition.y * perspective);

    VSOutput output;
    output.position = float4(clipXY, saturate((worldPosition.z - 2.2) / 6.0), 1.0);
    output.normal = normalize(Rotate(localNormal));
    output.screenHint = clipXY;
    return output;
}
)";

		static constexpr char pixelShader[] = R"(
cbuffer PushConstants : register(b0)
{
    float4 gSh0;
    float4 gShY;
    float4 gShZ;
    float4 gShX;
    float4 gLightDir0;
    float4 gLightDir1;
    float4 gLightDir2;
    float4 gLightColor0;
    float4 gLightColor1;
    float4 gLightColor2;
    float4 gParams;
    float4 gMode;
};

float3 EvalDirect(float3 n)
{
    float3 light = 0.015;
    light += gLightColor0.rgb * saturate(dot(n, normalize(gLightDir0.xyz)));
    light += gLightColor1.rgb * saturate(dot(n, normalize(gLightDir1.xyz)));
    light += gLightColor2.rgb * saturate(dot(n, normalize(gLightDir2.xyz)));
    return light;
}

float3 EvalSH(float3 n)
{
    const float y0 = 0.2820947918;
    const float yY = 0.4886025119 * n.y;
    const float yZ = 0.4886025119 * n.z;
    const float yX = 0.4886025119 * n.x;
    return max(gSh0.rgb * y0 + gShY.rgb * yY + gShZ.rgb * yZ + gShX.rgb * yX, 0.0.xxx);
}

float4 main(float4 position : SV_Position, float3 normal : NORMAL0, float2 screenHint : TEXCOORD0) : SV_Target0
{
    normal = normalize(normal);

    const float3 baseColor = float3(0.86, 0.84, 0.78);
    const float3 direct = EvalDirect(normal);
    const float3 sh = EvalSH(normal);

    float3 light = direct;
    if (gMode.x > 2.5)
    {
        const float3 diff = abs(direct - sh) * 4.0;
        return float4(saturate(diff), 1.0);
    }
    if (gMode.x > 1.5)
    {
        light = screenHint.x < 0.0 ? direct : sh;
    }
    else if (gMode.x > 0.5)
    {
        light = sh;
    }

    const float rim = pow(saturate(1.0 - abs(normal.z)), 2.0) * 0.045;
    float3 color = baseColor * light * gParams.w + rim;

    if (gMode.x > 1.5 && abs(screenHint.x) < 0.008)
    {
        color = float3(1.0, 1.0, 1.0);
    }

    return float4(saturate(color), 1.0);
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
		desc.depthStencilState.DepthEnable = TRUE;
		desc.depthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		desc.depthStencilState.StencilEnable = FALSE;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		return ctx.CreateRenderPipeline( desc );
	}

	ShPushConstants BuildPushConstants( const AppState& app, float aspect )
	{
		const ShRgb sh = ProjectLightsToSh( app.lights );

		ShPushConstants constants{};
		constants.sh0 = ToFloat4( sh.coeffs[ 0 ] );
		constants.shY = ToFloat4( sh.coeffs[ 1 ] );
		constants.shZ = ToFloat4( sh.coeffs[ 2 ] );
		constants.shX = ToFloat4( sh.coeffs[ 3 ] );

		const XMFLOAT3 dir0 = app.lights[ 0 ].enabled ? DirectionFromAngles( app.lights[ 0 ].yawDeg, app.lights[ 0 ].pitchDeg ) : XMFLOAT3{};
		const XMFLOAT3 dir1 = app.lights[ 1 ].enabled ? DirectionFromAngles( app.lights[ 1 ].yawDeg, app.lights[ 1 ].pitchDeg ) : XMFLOAT3{};
		const XMFLOAT3 dir2 = app.lights[ 2 ].enabled ? DirectionFromAngles( app.lights[ 2 ].yawDeg, app.lights[ 2 ].pitchDeg ) : XMFLOAT3{};

		constants.lightDir0 = ToFloat4( dir0 );
		constants.lightDir1 = ToFloat4( dir1 );
		constants.lightDir2 = ToFloat4( dir2 );
		constants.lightColor0 = ToFloat4( Mul( app.lights[ 0 ].color, app.lights[ 0 ].enabled ? app.lights[ 0 ].intensity : 0.0f ) );
		constants.lightColor1 = ToFloat4( Mul( app.lights[ 1 ].color, app.lights[ 1 ].enabled ? app.lights[ 1 ].intensity : 0.0f ) );
		constants.lightColor2 = ToFloat4( Mul( app.lights[ 2 ].color, app.lights[ 2 ].enabled ? app.lights[ 2 ].intensity : 0.0f ) );
		constants.params = { XMConvertToRadians( app.rotationX ), XMConvertToRadians( app.rotationY ), aspect, app.exposure };
		constants.mode = { static_cast<float>( app.mode ), 0.0f, 0.0f, 0.0f };
		return constants;
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
		windowClass.lpszClassName = L"LightD3D12SphericalHarmonicsSimpleWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t initialWidth = 1280;
		constexpr uint32_t initialHeight = 720;
		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Spherical Harmonics Simple",
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

		app.deviceManager = &DeviceManager::Initialize( contextDesc, swapchainDesc );
		app.imguiRenderer = std::make_unique<ImguiRenderer>( *app.deviceManager, swapchainDesc.window );
		app.pipeline = CreatePipeline( *app.deviceManager->GetRenderDevice(), contextDesc.swapchainFormat, DXGI_FORMAT_D32_FLOAT );
		RecreateDepthTarget( app );

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

			const uint32_t width = app.deviceManager->GetWidth();
			const uint32_t height = app.deviceManager->GetHeight();
			RecreateDepthTarget( app );
			const float aspect = static_cast<float>( width ) / static_cast<float>( std::max( 1u, height ) );
			const ShPushConstants pushConstants = BuildPushConstants( app, aspect );

			if( app.imguiRenderer )
			{
				app.imguiRenderer->NewFrame();
				ImGui::SetNextWindowPos( ImVec2( 16.0f, 16.0f ), ImGuiCond_Once );
				ImGui::SetNextWindowSize( ImVec2( 500.0f, 0.0f ), ImGuiCond_Once );
				ImGui::Begin( "Spherical Harmonics L1" );
				ImGui::TextWrapped( "Izquierda = luz directa. Derecha = la misma luz comprimida en 4 coeficientes SH." );
				ImGui::TextWrapped( "La esfera se queda fija; los sliders rotan la normal usada para consultar la luz/SH. Asi se ve claro que una probe responde a direcciones." );
				ImGui::Separator();
				const char* modes[] = { "Direct only", "SH only", "Split compare", "Difference x4" };
				ImGui::Combo( "View", &app.mode, modes, IM_ARRAYSIZE( modes ) );
				ImGui::SliderFloat( "Exposure", &app.exposure, 0.1f, 2.5f );
				ImGui::SliderFloat( "Normal rotation X", &app.rotationX, -90.0f, 90.0f );
				ImGui::SliderFloat( "Normal rotation Y", &app.rotationY, -180.0f, 180.0f );

				for( size_t i = 0; i != app.lights.size(); ++i )
				{
					LightControl& light = app.lights[ i ];
					char label[ 64 ] = {};
					snprintf( label, sizeof( label ), "Light %zu", i + 1 );
					if( ImGui::TreeNode( label ) )
					{
						ImGui::Checkbox( "Enabled", &light.enabled );
						ImGui::ColorEdit3( "Color", &light.color.x );
						ImGui::SliderFloat( "Intensity", &light.intensity, 0.0f, 4.0f );
						ImGui::SliderFloat( "Yaw", &light.yawDeg, -180.0f, 180.0f );
						ImGui::SliderFloat( "Pitch", &light.pitchDeg, -85.0f, 85.0f );
						ImGui::TreePop();
					}
				}

				ImGui::Separator();
				ImGui::TextWrapped( "Idea: SH no guarda '3 luces'. Guarda una funcion suave: dado una normal, dime cuanta luz viene de esa direccion." );
				ImGui::Text( "Coef 0 = media. Coef Y/Z/X = direccion gruesa." );
				ImGui::End();
			}

			ICommandBuffer& buffer = ctx->AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx->GetCurrentSwapchainTexture();

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.035f, 0.04f, 0.05f, 1.0f };
			renderPass.depthStencil.depthLoadOp = LoadOp::Clear;
			renderPass.depthStencil.depthStoreOp = StoreOp::Store;
			renderPass.depthStencil.clearDepth = 1.0f;

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;
			framebuffer.depthStencil.texture = app.depthTarget.texture;

			buffer.CmdBeginRendering( renderPass, framebuffer );
			buffer.CmdBindRenderPipeline( app.pipeline );
			buffer.CmdPushDebugGroupLabel( "Spherical Harmonics L1", 0xffffb347 );
			buffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			buffer.CmdDraw( kSphereVertexCount );
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
			if( RenderDevice* ctx = app.deviceManager->GetRenderDevice() )
			{
				DestroyDepthTarget( *ctx, app.depthTarget );
			}
		}
	}
	catch( const std::exception& error )
	{
		DeviceManager::ShutdownSingleton();
		MessageBoxA( nullptr, error.what(), "SphericalHarmonicsSimple failed", MB_ICONERROR | MB_OK );
		return 1;
	}

	return 0;
}



