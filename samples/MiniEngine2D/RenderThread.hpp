#pragma once

#include "RenderQueue.hpp"
#include "SpriteRenderer2D.hpp"

#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mini2d
{
	class RenderThread
	{
	public:
		struct Config
		{
			lightd3d12::NativeWindowHandle window = {};
			uint32_t width = 1280;
			uint32_t height = 720;
			bool vsync = true;
			std::filesystem::path assetRoot;
		};

		RenderThread() = default;
		~RenderThread();

		void Start( const Config& config );
		void SubmitFrame( RenderFrame&& frame );
		bool ProcessMessage( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam );
		void Resize( uint32_t width, uint32_t height, bool minimized );
		void Stop();

	private:
		struct QueuedImguiMessage
		{
			HWND hwnd = nullptr;
			UINT message = 0;
			WPARAM wParam = 0;
			LPARAM lParam = 0;
		};

		void ThreadMain( Config config );

		std::thread thread_;
		RenderQueue queue_;
		std::atomic<bool> stopRequested_ = false;
		std::atomic<bool> minimized_ = false;
		std::atomic<uint32_t> pendingWidth_ = 0;
		std::atomic<uint32_t> pendingHeight_ = 0;

		std::mutex initMutex_;
		std::condition_variable initCondition_;
		bool initialized_ = false;
		std::string initError_;

		std::unique_ptr<lightd3d12::ImguiRenderer> imguiRenderer_;
		std::atomic<bool> imguiReady_ = false;
		std::mutex imguiMessageMutex_;
		std::vector<QueuedImguiMessage> queuedImguiMessages_;
	};
}
