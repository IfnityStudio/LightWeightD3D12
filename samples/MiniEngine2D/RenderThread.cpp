#include "RenderThread.hpp"

#include "Profiler.hpp"

#include <imgui.h>

#include <objbase.h>
#include <stdexcept>

namespace mini2d
{
	using namespace lightd3d12;

	namespace
	{
		bool IsImguiInputMessage( UINT message )
		{
			switch( message )
			{
				case WM_MOUSEMOVE:
				case WM_LBUTTONDOWN:
				case WM_LBUTTONUP:
				case WM_LBUTTONDBLCLK:
				case WM_RBUTTONDOWN:
				case WM_RBUTTONUP:
				case WM_RBUTTONDBLCLK:
				case WM_MBUTTONDOWN:
				case WM_MBUTTONUP:
				case WM_MBUTTONDBLCLK:
				case WM_XBUTTONDOWN:
				case WM_XBUTTONUP:
				case WM_XBUTTONDBLCLK:
				case WM_MOUSEWHEEL:
				case WM_MOUSEHWHEEL:
				case WM_KEYDOWN:
				case WM_KEYUP:
				case WM_SYSKEYDOWN:
				case WM_SYSKEYUP:
				case WM_CHAR:
				case WM_SYSCHAR:
				case WM_SETCURSOR:
				case WM_SETFOCUS:
				case WM_KILLFOCUS:
					return true;

				default:
					return false;
			}
		}
	}

	RenderThread::~RenderThread()
	{
		Stop();
	}

	void RenderThread::Start( const Config& config )
	{
		Stop();
		queue_.Reset();
		stopRequested_ = false;
		minimized_ = false;
		pendingWidth_ = config.width;
		pendingHeight_ = config.height;
		imguiReady_.store( false, std::memory_order_release );

		{
			std::lock_guard<std::mutex> lock( initMutex_ );
			initialized_ = false;
			initError_.clear();
		}

		{
			std::lock_guard<std::mutex> lock( imguiMessageMutex_ );
			queuedImguiMessages_.clear();
		}

		thread_ = std::thread( [this, config]()
			{
				ThreadMain( config );
			} );

		std::unique_lock<std::mutex> lock( initMutex_ );
		initCondition_.wait( lock, [this]()
			{
				return initialized_;
			} );

		if( !initError_.empty() )
		{
			const std::string error = initError_;
			lock.unlock();
			Stop();
			throw std::runtime_error( error );
		}
	}

	void RenderThread::SubmitFrame( RenderFrame&& frame )
	{
		MINI_PROFILE_SCOPE( "RenderThread::SubmitFrame" );
		queue_.Submit( std::move( frame ) );
	}

	bool RenderThread::ProcessMessage( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		if( !imguiReady_.load( std::memory_order_acquire ) || !IsImguiInputMessage( message ) )
		{
			return false;
		}

		std::lock_guard<std::mutex> lock( imguiMessageMutex_ );
		queuedImguiMessages_.push_back( QueuedImguiMessage{
			.hwnd = hwnd,
			.message = message,
			.wParam = wParam,
			.lParam = lParam,
		} );
		return true;
	}

	void RenderThread::Resize( uint32_t width, uint32_t height, bool minimized )
	{
		pendingWidth_ = width;
		pendingHeight_ = height;
		minimized_ = minimized;
	}

	void RenderThread::Stop()
	{
		stopRequested_ = true;
		queue_.Stop();

		if( thread_.joinable() )
		{
			thread_.join();
		}

		imguiReady_.store( false, std::memory_order_release );
	}

	void RenderThread::ThreadMain( Config config )
	{
		SetProfilerThreadName( "RenderThread" );

		const HRESULT comResult = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
		const bool shouldUninitializeCom = comResult == S_OK || comResult == S_FALSE;

		try
		{
			ContextDesc contextDesc{};
			contextDesc.enableDebugLayer = true;
			contextDesc.swapchainBufferCount = 3;

			SwapchainDesc swapchainDesc{};
			swapchainDesc.window = config.window;
			swapchainDesc.width = config.width;
			swapchainDesc.height = config.height;
			swapchainDesc.vsync = config.vsync;

			auto deviceManager = std::make_unique<DeviceManager>( contextDesc, swapchainDesc );
			SpriteRenderer2D spriteRenderer;
			spriteRenderer.Initialize( *deviceManager->GetRenderDevice(), contextDesc.swapchainFormat, config.assetRoot / "enemy.png" );

			try
			{
				imguiRenderer_ = std::make_unique<ImguiRenderer>( *deviceManager, config.window );
				imguiReady_.store( true, std::memory_order_release );
			}
			catch( const std::exception& )
			{
				imguiReady_.store( false, std::memory_order_release );
				OutputDebugStringA( "ImguiRenderer initialization failed. MiniEngine2D will run without ImGui overlay.\n" );
			}

			{
				std::lock_guard<std::mutex> lock( initMutex_ );
				initialized_ = true;
			}
			initCondition_.notify_one();

			RenderFrame frame;
			while( !stopRequested_ )
			{
				MINI_PROFILE_SCOPE( "RenderThread::Frame" );

				if( !queue_.WaitAndPop( frame ) )
				{
					break;
				}

				if( stopRequested_ || minimized_ )
				{
					continue;
				}

				const uint32_t desiredWidth = pendingWidth_.load();
				const uint32_t desiredHeight = pendingHeight_.load();
				if( desiredWidth == 0 || desiredHeight == 0 )
				{
					continue;
				}

				if( desiredWidth != deviceManager->GetWidth() || desiredHeight != deviceManager->GetHeight() )
				{
					deviceManager->Resize( desiredWidth, desiredHeight );
				}

				const bool hasImgui = imguiReady_.load( std::memory_order_acquire ) && imguiRenderer_ != nullptr;
				if( hasImgui )
				{
					std::vector<QueuedImguiMessage> pendingMessages;
					{
						std::lock_guard<std::mutex> lock( imguiMessageMutex_ );
						pendingMessages.swap( queuedImguiMessages_ );
					}

					for( const QueuedImguiMessage& message : pendingMessages )
					{
						imguiRenderer_->ProcessMessage( message.hwnd, message.message, message.wParam, message.lParam );
					}

					imguiRenderer_->NewFrame();
					ImGui::SetNextWindowPos( ImVec2( 16.0f, 16.0f ), ImGuiCond_FirstUseEver );
					ImGui::SetNextWindowSize( ImVec2( 560.0f, 480.0f ), ImGuiCond_FirstUseEver );
					ImGui::Begin( "Mini Engine Console" );
					ImGui::Text( "Frame: %u", frame.frameNumber );
					ImGui::Text( "FPS: %.1f", frame.framesPerSecond );
					ImGui::Text( "Actors: %u", frame.actorCount );
					ImGui::Text( "Enemies: %u", frame.enemyCount );
					ImGui::Text( "Player HP: %u", frame.playerHealth );
					ImGui::Text( "Sync ticks: %u", frame.syncTicks );
					ImGui::Text( "Async ticks: %u", frame.asyncTicks );
					ImGui::Text( "Applied commands: %u", frame.commandsApplied );
					ImGui::Separator();
					ImGui::TextWrapped( "CPU profiling now uses Tracy. Open Tracy Profiler and connect to this process to inspect scopes, threads and frame timing." );
					ImGui::Separator();
					ImGui::BeginChild( "LogOutput", ImVec2( 0.0f, 0.0f ), true );
					for( const std::string& line : frame.debugLines )
					{
						ImGui::TextUnformatted( line.c_str() );
					}
					ImGui::EndChild();
					ImGui::End();
				}

				RenderDevice& renderDevice = *deviceManager->GetRenderDevice();
				ICommandBuffer& commandBuffer = renderDevice.AcquireCommandBuffer();
				const TextureHandle backbuffer = renderDevice.GetCurrentSwapchainTexture();

				RenderPass renderPass{};
				renderPass.color[ 0 ].loadOp = LoadOp::Clear;
				renderPass.color[ 0 ].clearColor = { 0.07f, 0.08f, 0.12f, 1.0f };

				Framebuffer framebuffer{};
				framebuffer.color[ 0 ].texture = backbuffer;

				commandBuffer.CmdBeginRendering( renderPass, framebuffer );
				spriteRenderer.Render( renderDevice, commandBuffer, frame, deviceManager->GetWidth(), deviceManager->GetHeight() );
				if( hasImgui )
				{
					imguiRenderer_->Render( commandBuffer );
				}
				commandBuffer.CmdEndRendering();
				renderDevice.Submit( commandBuffer, backbuffer );
			}

			deviceManager->WaitIdle();
			spriteRenderer.Shutdown( *deviceManager->GetRenderDevice() );
			imguiReady_.store( false, std::memory_order_release );
			imguiRenderer_.reset();
			deviceManager.reset();
		}
		catch( const std::exception& exception )
		{
			std::lock_guard<std::mutex> lock( initMutex_ );
			if( !initialized_ )
			{
				initError_ = exception.what();
				initialized_ = true;
				initCondition_.notify_one();
			}
		}

		if( shouldUninitializeCom )
		{
			CoUninitialize();
		}
	}
}
