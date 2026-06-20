#include "LightD3D12/LightD3D12.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>

using namespace lightd3d12;
 //hello this message only helps to reduce the 
namespace APP
{
	struct AppState;

	struct WindowState
	{
		AppState* app = nullptr;
		HWND hwnd = nullptr;
		SwapchainHandle swapchain = {};
		std::array<float, 3> baseColor = { 0.0f, 0.0f, 0.0f };
		float phaseOffset = 0.0f;
		bool alive = true;
		bool minimized = false;
	};

	struct AppState
	{
		static constexpr size_t ourWindowCount = 2;

		DeviceManager* deviceManager = nullptr;
		std::array<WindowState, ourWindowCount> windows = {};
		bool running = true;
		uint32_t aliveWindowCount = 0;
	};

	constexpr wchar_t ourWindowClassName[] = L"LightD3D12MultiWindow";
	constexpr uint32_t ourWindowWidth = 640;
	constexpr uint32_t ourWindowHeight = 480;

	void ReleaseWindowResources( WindowState& window )
	{
		if( window.app != nullptr && window.app->deviceManager != nullptr && window.swapchain.Valid() )
		{
			window.app->deviceManager->DestroySwapchain( window.swapchain );
			window.swapchain = {};
		}
	}

	std::array<float, 4> BuildClearColor( const WindowState& window, float elapsedSeconds )
	{
		const float pulse = 0.25f + 0.25f * ( 1.0f + std::sinf( elapsedSeconds * 1.35f + window.phaseOffset ) );
		return
		{
			std::min( 1.0f, 0.04f + window.baseColor[ 0 ] * pulse ),
			std::min( 1.0f, 0.04f + window.baseColor[ 1 ] * pulse ),
			std::min( 1.0f, 0.04f + window.baseColor[ 2 ] * pulse ),
			1.0f
		};
	}

	void RenderWindow( WindowState& window, float elapsedSeconds )
	{
		if( !window.alive || window.minimized || window.app == nullptr || window.app->deviceManager == nullptr || !window.swapchain.Valid() )
		{
			return;
		}

		RenderDevice& renderDevice = *window.app->deviceManager->GetRenderDevice();
		auto& commandBuffer = renderDevice.AcquireCommandBuffer();
		const TextureHandle currentTexture = renderDevice.GetCurrentSwapchainTexture( window.swapchain );
		if( !currentTexture.Valid() )
		{
			return;
		}

		RenderPass renderPass{};
		renderPass.color[ 0 ].loadOp = LoadOp::Clear;
		renderPass.color[ 0 ].clearColor = BuildClearColor( window, elapsedSeconds );

		Framebuffer framebuffer{};
		framebuffer.color[ 0 ].texture = currentTexture;

		commandBuffer.CmdBeginRendering( renderPass, framebuffer );
		commandBuffer.CmdEndRendering();
		renderDevice.Submit( commandBuffer, currentTexture );
	}

	void CleanupApp( AppState& app, HINSTANCE instance )
	{
		for( WindowState& window : app.windows )
		{
			ReleaseWindowResources( window );
			if( window.hwnd != nullptr && IsWindow( window.hwnd ) != FALSE )
			{
				DestroyWindow( window.hwnd );
			}
			window.hwnd = nullptr;
		}

		if( app.deviceManager != nullptr )
		{
			app.deviceManager->WaitIdle();
			DeviceManager::ShutdownSingleton();
			app.deviceManager = nullptr;
		}

		UnregisterClassW( ourWindowClassName, instance );
	}

	LRESULT CALLBACK WindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		if( message == WM_NCCREATE )
		{
			const auto* createInfo = reinterpret_cast<const CREATESTRUCTW*>( lParam );
			auto* window = reinterpret_cast<WindowState*>( createInfo->lpCreateParams );
			SetWindowLongPtr( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( window ) );
			if( window != nullptr )
			{
				window->hwnd = hwnd;
			}
		}

		auto* window = reinterpret_cast<WindowState*>( GetWindowLongPtr( hwnd, GWLP_USERDATA ) );

		switch( message )
		{
			case WM_SIZE:
			{
				if( window != nullptr && window->app != nullptr && window->app->deviceManager != nullptr && window->swapchain.Valid() )
				{
					const uint32_t width = LOWORD( lParam );
					const uint32_t height = HIWORD( lParam );
					window->minimized = width == 0 || height == 0;
					if( !window->minimized )
					{
						window->app->deviceManager->Resize( window->swapchain, width, height );
					}
				}
				return 0;
			}

			case WM_CLOSE:
				DestroyWindow( hwnd );
				return 0;

			case WM_DESTROY:
			{
				if( window != nullptr )
				{
					window->alive = false;
					window->minimized = true;
					window->hwnd = nullptr;
					if( window->app != nullptr && window->app->aliveWindowCount > 0 )
					{
						window->app->aliveWindowCount--;
						if( window->app->aliveWindowCount == 0 )
						{
							window->app->running = false;
							PostQuitMessage( 0 );
						}
					}
				}
				return 0;
			}

			case WM_NCDESTROY:
				SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
				break;

			default:
				return DefWindowProc( hwnd, message, wParam, lParam );
		}

		return DefWindowProc( hwnd, message, wParam, lParam );
	}

	void CreateRenderWindow( WindowState& window, HINSTANCE instance, const wchar_t* title, int x, int y )
	{
		window.hwnd = CreateWindowExW(
			0,
			ourWindowClassName,
			title,
			WS_OVERLAPPEDWINDOW,
			x,
			y,
			static_cast<int>( ourWindowWidth ),
			static_cast<int>( ourWindowHeight ),
			nullptr,
			nullptr,
			instance,
			&window );

		if( window.hwnd == nullptr )
		{
			throw std::runtime_error( "Failed to create Win32 window." );
		}
	}

	void InitializeSwapchain( DeviceManager& deviceManager, WindowState& window )
	{
		SwapchainDesc swapchainDesc{};
		swapchainDesc.window = MakeWin32WindowHandle( window.hwnd );
		swapchainDesc.width = ourWindowWidth;
		swapchainDesc.height = ourWindowHeight;
		swapchainDesc.vsync = true;

		window.swapchain = deviceManager.CreateSwapchain( swapchainDesc );
	}
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE, PWSTR, int showCommand )
{
  using namespace APP;

	AppState app{};

	try
	{
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEXW );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = ourWindowClassName;
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		if( RegisterClassExW( &windowClass ) == 0 )
		{
			throw std::runtime_error( "Failed to register Win32 window class." );
		}

		app.aliveWindowCount = static_cast<uint32_t>( app.windows.size() );

		app.windows[ 0 ].app = &app;
		app.windows[ 0 ].baseColor = { 0.95f, 0.25f, 0.20f };
		app.windows[ 0 ].phaseOffset = 0.0f;

		app.windows[ 1 ].app = &app;
		app.windows[ 1 ].baseColor = { 0.18f, 0.45f, 0.95f };
		app.windows[ 1 ].phaseOffset = 1.9f;

		ContextDesc contextDesc{};
		contextDesc.enableDebugLayer = true;
		contextDesc.swapchainBufferCount = 3;
		app.deviceManager = &DeviceManager::Initialize( contextDesc );

		CreateRenderWindow( app.windows[ 0 ], instance, L"Window A - Swapchain A", 80, 80 );
		CreateRenderWindow( app.windows[ 1 ], instance, L"Window B - Swapchain B", 760, 120 );

		InitializeSwapchain( *app.deviceManager, app.windows[ 0 ] );
		InitializeSwapchain( *app.deviceManager, app.windows[ 1 ] );

		for( WindowState& window : app.windows )
		{
			ShowWindow( window.hwnd, showCommand );
			UpdateWindow( window.hwnd );
		}

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

			for( WindowState& window : app.windows )
			{
				if( !window.alive )
				{
					ReleaseWindowResources( window );
				}
			}

			if( !app.running )
			{
				break;
			}

			const float elapsedSeconds = std::chrono::duration<float>( std::chrono::steady_clock::now() - startTime ).count();
			bool renderedAnyWindow = false;
			for( WindowState& window : app.windows )
			{
				if( window.alive && !window.minimized && app.deviceManager != nullptr && window.swapchain.Valid() )
				{
					RenderWindow( window, elapsedSeconds );
					renderedAnyWindow = true;
				}
			}

			if( !renderedAnyWindow )
			{
				Sleep( 1 );
			}
		}

		CleanupApp( app, instance );
		return 0;
	}
	catch( const std::exception& )
	{
		CleanupApp( app, instance );
		MessageBoxA( nullptr, "LightD3D12 MultiWindow failed.", "LightD3D12", MB_ICONERROR | MB_OK );
		return 1;
	}
}
