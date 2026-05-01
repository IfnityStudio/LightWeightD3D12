#pragma once

#include "LightD3D12Internal.hpp"
#include "LightD3D12Resources.hpp"

#include <array>

namespace lightd3d12
{
	inline constexpr uint32_t ourMaxSwapchainBuffers = 3;

	class DeviceManager::Impl;

	class Swapchain final
	{
	public:
		Swapchain( DeviceManager::Impl& ctx, HWND hwnd, uint32_t width, uint32_t height );
		~Swapchain();
		Swapchain( const Swapchain& ) = delete;
		Swapchain& operator=( const Swapchain& ) = delete;

		void Present();
		void Resize( uint32_t width, uint32_t height );
		TextureHandle GetCurrentTexture();
		uint32_t GetCurrentBackBufferIndex() const noexcept;
		DXGI_FORMAT GetSurfaceFormat() const noexcept;
		IDXGISwapChain4* GetSwapchain() const noexcept;

	private:
		void DestroyBackBuffers() noexcept;
		void RecreateBackBuffers();
		bool CheckVSyncEnabled() const noexcept;

		DeviceManager::Impl& ctx_;
		ComPtr<IDXGISwapChain4> swapchain_;
		std::array<ComPtr<ID3D12Resource>, ourMaxSwapchainBuffers> backBuffers_;
		std::array<TextureHandle, ourMaxSwapchainBuffers> backBufferHandles_;
		std::array<D3D12_CPU_DESCRIPTOR_HANDLE, ourMaxSwapchainBuffers> rtvHandles_;
		uint32_t width_ = 0;
		uint32_t height_ = 0;
		uint32_t numSwapchainImages_ = 0;
		uint32_t currentBackBufferIndex_ = 0;
		DXGI_FORMAT surfaceFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;
	};
}
