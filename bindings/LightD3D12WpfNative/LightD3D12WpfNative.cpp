#include "LightD3D12/LightD3D12.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

using namespace lightd3d12;

namespace
{
	constexpr wchar_t kRenderWindowClassName[] = L"LightD3D12WpfNativeRenderWindow";

	struct TrianglePushConstants
	{
		float time = 0.0f;
		float aspectRatio = 1.0f;
		float padding_[ 2 ] = {};
	};

	static_assert( sizeof( TrianglePushConstants ) / sizeof( uint32_t ) <= 63 );

	struct NativeContext
	{
		HWND parentHwnd = nullptr;
		HWND renderHwnd = nullptr;
		uint32_t width = 1;
		uint32_t height = 1;
		std::unique_ptr<DeviceManager> deviceManager;
		RenderPipelineState trianglePipeline;
		std::array<float, 4> clearColor = { 0.04f, 0.06f, 0.09f, 1.0f };
		float animationSpeed = 1.0f;
		float animationTime = 0.0f;
		std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();
	};

	thread_local std::string gLastError;
	std::once_flag gRegisterWindowClassOnce;

	void SetLastErrorMessage( const char* message )
	{
		gLastError = message != nullptr ? message : "Unknown LightD3D12 WPF native error.";
	}

	void SetLastErrorMessage( const std::exception& exception )
	{
		gLastError = exception.what();
	}

	LRESULT CALLBACK RenderWindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		switch( message )
		{
			case WM_ERASEBKGND:
				return 1;

			default:
				return DefWindowProcW( hwnd, message, wParam, lParam );
		}
	}

	void RegisterRenderWindowClass()
	{
		std::call_once( gRegisterWindowClassOnce, []
			{
				WNDCLASSEXW windowClass{};
				windowClass.cbSize = sizeof( WNDCLASSEXW );
				windowClass.lpfnWndProc = RenderWindowProc;
				windowClass.hInstance = GetModuleHandleW( nullptr );
				windowClass.lpszClassName = kRenderWindowClassName;
				windowClass.hCursor = LoadCursorW( nullptr, IDC_ARROW );

				const ATOM atom = RegisterClassExW( &windowClass );
				if( atom == 0 )
				{
					const DWORD error = GetLastError();
					if( error != ERROR_CLASS_ALREADY_EXISTS )
					{
						throw std::runtime_error( "Failed to register the LightD3D12 WPF render window class." );
					}
				}
			} );
	}

	RenderPipelineState CreateTrianglePipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat )
	{
		static constexpr char kVertexShader[] = R"(
cbuffer PushConstants : register(b0)
{
    float gTime;
    float gAspectRatio;
    float2 gPadding;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    const float2 basePositions[3] =
    {
        float2( 0.0,  0.62),
        float2( 0.64, -0.46),
        float2(-0.64, -0.46)
    };

    const float3 colors[3] =
    {
        float3(1.00, 0.44, 0.18),
        float3(0.18, 0.78, 1.00),
        float3(0.62, 1.00, 0.36)
    };

    const float angle = gTime * 0.85;
    const float s = sin(angle);
    const float c = cos(angle);
    float2 p = basePositions[vertexID];
    p = float2(p.x * c - p.y * s, p.x * s + p.y * c);
    p.x /= max(gAspectRatio, 0.001);

    VSOutput output;
    output.position = float4(p, 0.0, 1.0);
    output.color = colors[vertexID];
    return output;
}
)";

		static constexpr char kPixelShader[] = R"(
float4 main(float4 position : SV_Position, float3 color : COLOR0) : SV_Target0
{
    return float4(color, 1.0);
}
)";

		RenderPipelineDesc desc{};
		desc.vertexShader.source = kVertexShader;
		desc.vertexShader.entryPoint = "main";
		desc.vertexShader.profile = "vs_6_6";
		desc.fragmentShader.source = kPixelShader;
		desc.fragmentShader.entryPoint = "main";
		desc.fragmentShader.profile = "ps_6_6";
		desc.color[ 0 ].format = colorFormat;
		desc.depthFormat = DXGI_FORMAT_UNKNOWN;
		desc.rasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	NativeContext* AsContext( void* handle )
	{
		return static_cast<NativeContext*>( handle );
	}
}

extern "C"
{
	__declspec( dllexport ) void* LightWpf_Create( HWND parentHwnd, uint32_t width, uint32_t height )
	{
		try
		{
			gLastError.clear();
			if( parentHwnd == nullptr )
			{
				throw std::runtime_error( "The WPF parent HWND is null." );
			}

			RegisterRenderWindowClass();

			auto context = std::make_unique<NativeContext>();
			context->parentHwnd = parentHwnd;
			context->width = std::max( width, 1u );
			context->height = std::max( height, 1u );

			context->renderHwnd = CreateWindowExW(
				0,
				kRenderWindowClassName,
				L"",
				WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
				0,
				0,
				static_cast<int>( context->width ),
				static_cast<int>( context->height ),
				parentHwnd,
				nullptr,
				GetModuleHandleW( nullptr ),
				nullptr );

			if( context->renderHwnd == nullptr )
			{
				throw std::runtime_error( "Failed to create the child render HWND." );
			}

			ContextDesc contextDesc{};
#if defined( _DEBUG )
			contextDesc.enableDebugLayer = true;
#else
			contextDesc.enableDebugLayer = false;
#endif
			contextDesc.swapchainBufferCount = 3;

			SwapchainDesc swapchainDesc{};
			swapchainDesc.window = MakeWin32WindowHandle( context->renderHwnd );
			swapchainDesc.width = context->width;
			swapchainDesc.height = context->height;
			swapchainDesc.vsync = true;

			context->deviceManager = std::make_unique<DeviceManager>( contextDesc, swapchainDesc );
			context->trianglePipeline = CreateTrianglePipeline( *context->deviceManager->GetRenderDevice(), contextDesc.swapchainFormat );
			context->lastFrameTime = std::chrono::steady_clock::now();

			return context.release();
		}
		catch( const std::exception& exception )
		{
			SetLastErrorMessage( exception );
			return nullptr;
		}
	}

	__declspec( dllexport ) void LightWpf_Destroy( void* nativeContext )
	{
		try
		{
			gLastError.clear();
			std::unique_ptr<NativeContext> context( AsContext( nativeContext ) );
			if( !context )
			{
				return;
			}

			if( context->deviceManager )
			{
				context->deviceManager->WaitIdle();
			}

			context->trianglePipeline = {};
			context->deviceManager.reset();

			if( context->renderHwnd != nullptr )
			{
				DestroyWindow( context->renderHwnd );
				context->renderHwnd = nullptr;
			}
		}
		catch( const std::exception& exception )
		{
			SetLastErrorMessage( exception );
		}
	}

	__declspec( dllexport ) HWND LightWpf_GetChildWindow( void* nativeContext )
	{
		const NativeContext* context = AsContext( nativeContext );
		return context != nullptr ? context->renderHwnd : nullptr;
	}

	__declspec( dllexport ) int LightWpf_Resize( void* nativeContext, uint32_t width, uint32_t height )
	{
		try
		{
			gLastError.clear();
			NativeContext* context = AsContext( nativeContext );
			if( context == nullptr || context->deviceManager == nullptr )
			{
				throw std::runtime_error( "LightWpf_Resize was called with an invalid context." );
			}

			context->width = std::max( width, 1u );
			context->height = std::max( height, 1u );
			SetWindowPos(
				context->renderHwnd,
				nullptr,
				0,
				0,
				static_cast<int>( context->width ),
				static_cast<int>( context->height ),
				SWP_NOZORDER | SWP_NOACTIVATE );
			context->deviceManager->Resize( context->width, context->height );
			return 1;
		}
		catch( const std::exception& exception )
		{
			SetLastErrorMessage( exception );
			return 0;
		}
	}

	__declspec( dllexport ) int LightWpf_Render( void* nativeContext )
	{
		try
		{
			gLastError.clear();
			NativeContext* context = AsContext( nativeContext );
			if( context == nullptr || context->deviceManager == nullptr )
			{
				throw std::runtime_error( "LightWpf_Render was called with an invalid context." );
			}

			const auto now = std::chrono::steady_clock::now();
			float deltaSeconds = std::chrono::duration<float>( now - context->lastFrameTime ).count();
			context->lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );
			context->animationTime += deltaSeconds * context->animationSpeed;

			RenderDevice& ctx = *context->deviceManager->GetRenderDevice();
			ICommandBuffer& commandBuffer = ctx.AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx.GetCurrentSwapchainTexture();

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = context->clearColor;

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;

			TrianglePushConstants pushConstants{};
			pushConstants.time = context->animationTime;
			pushConstants.aspectRatio = static_cast<float>( context->width ) / static_cast<float>( std::max( context->height, 1u ) );

			commandBuffer.CmdBeginRendering( renderPass, framebuffer );
			commandBuffer.CmdBindRenderPipeline( context->trianglePipeline );
			commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			commandBuffer.CmdDraw( 3 );
			commandBuffer.CmdEndRendering();

			ctx.Submit( commandBuffer, currentTexture );
			return 1;
		}
		catch( const std::exception& exception )
		{
			SetLastErrorMessage( exception );
			return 0;
		}
	}

	__declspec( dllexport ) void LightWpf_SetClearColor( void* nativeContext, float red, float green, float blue )
	{
		NativeContext* context = AsContext( nativeContext );
		if( context == nullptr )
		{
			return;
		}

		context->clearColor = {
			std::clamp( red, 0.0f, 1.0f ),
			std::clamp( green, 0.0f, 1.0f ),
			std::clamp( blue, 0.0f, 1.0f ),
			1.0f
		};
	}

	__declspec( dllexport ) void LightWpf_SetAnimationSpeed( void* nativeContext, float speed )
	{
		NativeContext* context = AsContext( nativeContext );
		if( context == nullptr )
		{
			return;
		}

		context->animationSpeed = std::clamp( speed, 0.0f, 4.0f );
	}

	__declspec( dllexport ) int LightWpf_GetLastError( char* buffer, int capacity )
	{
		if( buffer == nullptr || capacity <= 0 )
		{
			return static_cast<int>( gLastError.size() );
		}

		const int copyCount = std::min<int>( static_cast<int>( gLastError.size() ), capacity - 1 );
		if( copyCount > 0 )
		{
			std::memcpy( buffer, gLastError.data(), static_cast<size_t>( copyCount ) );
		}
		buffer[ copyCount ] = '\0';
		return copyCount;
	}
}
