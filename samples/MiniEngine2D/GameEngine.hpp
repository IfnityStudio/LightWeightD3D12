#pragma once

#include "Actors.hpp"
#include "RenderThread.hpp"
#include "TaskScheduler.hpp"
#include "TickScheduler.hpp"
#include "World.hpp"

#include <memory>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace mini2d
{
	class GameEngine
	{
	public:
		GameEngine();
		~GameEngine();

		static GameEngine& Get();
		static RenderThread& GetRender();

		int Run( HINSTANCE instance, int showCommand );

	private:
		bool Initialize( HINSTANCE instance, int showCommand );
		void InitializeWorld();
		void Shutdown();
		void UpdateInput();
		void TickFrame( float deltaTime );
		RenderFrame BuildRenderFrame() const;
		void UpdateWindowTitle() const;

		static LRESULT CALLBACK StaticWindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam );
		LRESULT WindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam );

		HINSTANCE instance_ = nullptr;
		HWND hwnd_ = nullptr;
		bool running_ = true;
		bool minimized_ = false;
		uint32_t windowWidth_ = 1280;
		uint32_t windowHeight_ = 720;
		uint32_t frameNumber_ = 0;
		float accumulatedTime_ = 0.0f;
		float lastDeltaTime_ = 1.0f / 60.0f;
		float smoothedFramesPerSecond_ = 60.0f;
		TickExecutionStats lastTickStats_ = {};
		std::unique_ptr<World> world_;
		TaskScheduler taskScheduler_;
		TickScheduler tickScheduler_;

		static GameEngine* ourInstance_;
		static RenderThread ourRenderThread_;
	};
}
