#pragma once

#include "LightD3D12Internal.hpp"

#include <vector>

namespace lightd3d12
{
	class ImmediateCommands final
	{
	public:
		struct CommandListWrapper
		{
			ComPtr<ID3D12CommandAllocator> allocator_;
			ComPtr<ID3D12GraphicsCommandList4> commandList_;
			ComPtr<ID3D12Fence> fence_;
			SubmitHandle handle_{};
			HANDLE fenceEvent_ = nullptr;
			uint64_t fenceValue_ = 0;
			bool isEncoding_ = false;
		};

		ImmediateCommands( ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t numContexts );
		~ImmediateCommands();
		ImmediateCommands( const ImmediateCommands& ) = delete;
		ImmediateCommands& operator=( const ImmediateCommands& ) = delete;

		CommandListWrapper& Acquire();
		SubmitHandle Submit( CommandListWrapper& wrapper );
		SubmitHandle GetLastSubmitHandle() const noexcept;
		SubmitHandle GetNextSubmitHandle() const noexcept;
		bool IsReady( SubmitHandle handle, bool fastCheckNoD3D12 = false ) const;
		void Wait( SubmitHandle handle );
		void WaitAll();

	private:
		void Purge();

	private:
		ID3D12Device* device_ = nullptr;
		ID3D12CommandQueue* queue_ = nullptr;
		std::vector<CommandListWrapper> buffers_;
		SubmitHandle lastSubmitHandle_ = {};
		SubmitHandle nextSubmitHandle_ = {};
		uint32_t numAvailableCommandBuffers_ = 0;
		uint32_t fenceCounter_ = 1;
	};
}
