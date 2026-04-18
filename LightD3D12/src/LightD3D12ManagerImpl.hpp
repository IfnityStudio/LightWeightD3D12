#pragma once

#include "LightD3D12Internal.hpp"
#include "LightD3D12CommandBuffer.hpp"
#include "LightD3D12ImmediateCommands.hpp"
#include "LightD3D12Resources.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace lightd3d12
{
	class StagingDevice;
	class Swapchain;

	class DeviceManager::Impl final
	{
	public:
		explicit Impl( const ContextDesc& desc, const SwapchainDesc& swapchainDesc );
		~Impl();

		void Initialize();
		void InitializeFactory();
		void InitializeDevice();
		void InitializeCommandQueue();
		void InitializeDescriptorHeaps();
		void InitializeRootSignature();
		void InitializeCommandSignature();
		void CreateSwapchain();

		uint32_t AllocateBindlessDescriptor();
		uint32_t AllocateRtvDescriptor();
		uint32_t AllocateDsvDescriptor();
		void FreeBindlessDescriptor( uint32_t index );
		void FreeRtvDescriptor( uint32_t index );
		void FreeDsvDescriptor( uint32_t index );

		BufferResource& GetBufferResource( BufferHandle handle );
		const BufferResource& GetBufferResource( BufferHandle handle ) const;
		TextureResource& GetTextureResource( TextureHandle handle );
		const TextureResource& GetTextureResource( TextureHandle handle ) const;

		void AddDeferredRelease( SubmitHandle handle, std::function<void()>&& release );
		void ProcessDeferredReleases();
		void WaitForQueueIdle();
		void WaitIdle();
		void Resize( uint32_t width, uint32_t height );
		void Shutdown() noexcept;
		void ReportLiveObjects() noexcept;

		ContextDesc desc_;
		SwapchainDesc swapchainDesc_;
		ComPtr<IDXGIFactory6> factory_;
		ComPtr<IDXGIAdapter1> adapter_;
		ComPtr<ID3D12Device> device_;
		ComPtr<ID3D12CommandQueue> commandQueue_;
		ComPtr<ID3D12Fence> queueIdleFence_;
		HANDLE queueIdleEvent_ = nullptr;
		uint64_t queueIdleFenceValue_ = 0;
		ComPtr<ID3D12DescriptorHeap> bindlessHeap_;
		ComPtr<ID3D12DescriptorHeap> rtvHeap_;
		ComPtr<ID3D12DescriptorHeap> dsvHeap_;
		uint32_t bindlessDescriptorSize_ = 0;
		uint32_t rtvDescriptorSize_ = 0;
		uint32_t dsvDescriptorSize_ = 0;
		std::vector<uint32_t> freeBindlessDescriptors_;
		std::vector<uint32_t> freeRtvDescriptors_;
		std::vector<uint32_t> freeDsvDescriptors_;
		ComPtr<ID3D12RootSignature> rootSignature_;
		ComPtr<ID3D12CommandSignature> commandSignature_;
		std::unique_ptr<Swapchain> swapchain_;
		SlotMap<BufferResource> slotMapBuffers_;
		SlotMap<TextureResource> slotMapTextures_;
		std::unique_ptr<ImmediateCommands> immediateCommands_;
		std::unique_ptr<StagingDevice> stagingDevice_;
		std::unique_ptr<CommandBufferImpl> currentCommandBuffer_;
		bool bindlessSupported_ = false;

		struct DeferredRelease
		{
			SubmitHandle handle_;
			std::function<void()> release_;
		};

		std::deque<DeferredRelease> deferredReleases_;
	};
}
