#pragma once

#include "LightD3D12/LightD3D12.hpp"

#include <memory>

namespace lightd3d12
{
	class ImguiRenderer final
	{
	public:
		ImguiRenderer( DeviceManager& deviceManager, NativeWindowHandle window );
		~ImguiRenderer();
		ImguiRenderer( const ImguiRenderer& ) = delete;
		ImguiRenderer& operator=( const ImguiRenderer& ) = delete;
		ImguiRenderer( ImguiRenderer&& ) = delete;
		ImguiRenderer& operator=( ImguiRenderer&& ) = delete;

		void NewFrame();
		void Render( ICommandBuffer& commandBuffer );
		bool ProcessMessage( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam );

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
