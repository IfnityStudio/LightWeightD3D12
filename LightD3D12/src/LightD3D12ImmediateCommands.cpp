#include "LightD3D12ImmediateCommands.hpp"

#include <array>

namespace lightd3d12
{
	ImmediateCommands::ImmediateCommands( ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t numContexts ):
		device_( device ),
		queue_( queue ),
		buffers_( numContexts ),
		numAvailableCommandBuffers_( numContexts )
	{
		if( device_ == nullptr || queue_ == nullptr )
		{
			throw std::runtime_error( "ImmediateCommands requires a valid device and queue." );
		}

		for( uint32_t i = 0; i < buffers_.size(); ++i )
		{
			CommandListWrapper& buffer = buffers_[ i ];

			C_RESULT(
				device_->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS( buffer.allocator_.GetAddressOf() ) ),
				"Failed to create command allocator." );

			C_RESULT(
				device_->CreateCommandList(
					0,
					D3D12_COMMAND_LIST_TYPE_DIRECT,
					buffer.allocator_.Get(),
					nullptr,
					IID_PPV_ARGS( buffer.commandList_.GetAddressOf() ) ),
				"Failed to create command list." );

			buffer.commandList_->Close();

			C_RESULT(
				device_->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( buffer.fence_.GetAddressOf() ) ),
				"Failed to create fence for immediate command buffer." );

			buffer.fenceEvent_ = CreateEvent( nullptr, FALSE, FALSE, nullptr );
			if( buffer.fenceEvent_ == nullptr )
			{
				throw std::runtime_error( "Failed to create event for immediate command buffer fence." );
			}

			buffer.handle_.bufferIndex_ = i;
		}
	}

	ImmediateCommands::~ImmediateCommands()
	{
		for( CommandListWrapper& buffer : buffers_ )
		{
			if( buffer.fence_ && buffer.fence_->GetCompletedValue() < buffer.fenceValue_ )
			{
				buffer.fence_->SetEventOnCompletion( buffer.fenceValue_, buffer.fenceEvent_ );
				WaitForSingleObject( buffer.fenceEvent_, INFINITE );
			}

			if( buffer.fenceEvent_ != nullptr )
			{
				CloseHandle( buffer.fenceEvent_ );
				buffer.fenceEvent_ = nullptr;
			}

			if( buffer.allocator_ != nullptr && buffer.commandList_ != nullptr )
			{
				if( SUCCEEDED( buffer.allocator_->Reset() ) )
				{
					if( SUCCEEDED( buffer.commandList_->Reset( buffer.allocator_.Get(), nullptr ) ) )
					{
						buffer.commandList_->Close();
					}
				}
			}

			buffer.commandList_.Reset();
			buffer.allocator_.Reset();
			buffer.fence_.Reset();
		}
	}

	ImmediateCommands::CommandListWrapper& ImmediateCommands::Acquire()
	{
		if( numAvailableCommandBuffers_ == 0 )
		{
			Purge();
		}

		while( numAvailableCommandBuffers_ == 0 )
		{
			WaitForFirstAvailable();
		}

		CommandListWrapper* current = nullptr;
		for( CommandListWrapper& buffer : buffers_ )
		{
			if( !buffer.isEncoding_ && buffer.fenceValue_ == 0 )
			{
				current = &buffer;
				break;
			}
		}

		assert( current != nullptr );
		assert( numAvailableCommandBuffers_ > 0 );

		C_RESULT( current->allocator_->Reset(), "Failed to reset immediate command allocator." );
		C_RESULT( current->commandList_->Reset( current->allocator_.Get(), nullptr ), "Failed to reset immediate command list." );

		current->handle_.submitId_ = fenceCounter_;
		current->isEncoding_ = true;
		nextSubmitHandle_ = current->handle_;
		numAvailableCommandBuffers_--;
		return *current;
	}

	SubmitHandle ImmediateCommands::Submit( CommandListWrapper& wrapper )
	{
		return Submit( wrapper, nullptr );
	}

	SubmitHandle ImmediateCommands::Submit( CommandListWrapper& wrapper, ID3D12CommandList* submitPrologue )
	{
		assert( wrapper.isEncoding_ );

		C_RESULT( wrapper.commandList_->Close(), "Failed to close immediate command list." );

		std::array<ID3D12CommandList*, 2> commandLists = {};
		uint32_t numCommandLists = 0;
		if( submitPrologue != nullptr )
		{
			commandLists[ numCommandLists++ ] = submitPrologue;
		}
		commandLists[ numCommandLists++ ] = wrapper.commandList_.Get();

		queue_->ExecuteCommandLists( numCommandLists, commandLists.data() );
		C_RESULT( queue_->Signal( wrapper.fence_.Get(), fenceCounter_ ), "Failed to signal immediate command fence." );

		wrapper.fenceValue_ = fenceCounter_;
		wrapper.isEncoding_ = false;
		lastSubmitHandle_ = wrapper.handle_;
		lastSubmitHandle_.submitId_ = fenceCounter_;

		fenceCounter_++;
		if( fenceCounter_ == 0 )
		{
			fenceCounter_++;
		}

		return lastSubmitHandle_;
	}

	SubmitHandle ImmediateCommands::GetLastSubmitHandle() const noexcept
	{
		return lastSubmitHandle_;
	}

	SubmitHandle ImmediateCommands::GetNextSubmitHandle() const noexcept
	{
		return nextSubmitHandle_;
	}

	bool ImmediateCommands::IsReady( SubmitHandle handle, bool fastCheckNoD3D12 ) const
	{
		if( handle.Empty() )
		{
			return true;
		}

		assert( handle.bufferIndex_ < buffers_.size() );
		const CommandListWrapper& buffer = buffers_[ handle.bufferIndex_ ];

		if( buffer.handle_.submitId_ != handle.submitId_ )
		{
			return true;
		}

		if( buffer.fenceValue_ == 0 )
		{
			return true;
		}

		if( fastCheckNoD3D12 )
		{
			return false;
		}

		return buffer.fence_->GetCompletedValue() >= buffer.fenceValue_;
	}

	void ImmediateCommands::Wait( SubmitHandle handle )
	{
		if( handle.Empty() || IsReady( handle ) )
		{
			return;
		}

		assert( handle.bufferIndex_ < buffers_.size() );
		CommandListWrapper& buffer = buffers_[ handle.bufferIndex_ ];
		if( buffer.isEncoding_ )
		{
			throw std::runtime_error( "Waiting on an immediate command buffer that has not been submitted." );
		}

		if( buffer.fence_->GetCompletedValue() < buffer.fenceValue_ )
		{
			buffer.fence_->SetEventOnCompletion( buffer.fenceValue_, buffer.fenceEvent_ );
			WaitForSingleObject( buffer.fenceEvent_, INFINITE );
		}
	}

	void ImmediateCommands::WaitAll()
	{
		for( CommandListWrapper& buffer : buffers_ )
		{
			if( buffer.fenceValue_ == 0 || buffer.isEncoding_ )
			{
				continue;
			}

			if( buffer.fence_->GetCompletedValue() < buffer.fenceValue_ )
			{
				buffer.fence_->SetEventOnCompletion( buffer.fenceValue_, buffer.fenceEvent_ );
				WaitForSingleObject( buffer.fenceEvent_, INFINITE );
			}
		}

		Purge();
	}

	ImmediateCommands::CommandListWrapper* ImmediateCommands::FindOldestSubmittedBuffer() noexcept
	{
		if( buffers_.empty() )
		{
			return nullptr;
		}

		for( uint32_t i = 0; i < buffers_.size(); ++i )
		{
			CommandListWrapper& buffer = buffers_[ ( i + lastSubmitHandle_.bufferIndex_ + 1u ) % buffers_.size() ];
			if( buffer.fenceValue_ != 0 && !buffer.isEncoding_ )
			{
				return &buffer;
			}
		}

		return nullptr;
	}

	void ImmediateCommands::WaitForFirstAvailable()
	{
        Purge();
		if( numAvailableCommandBuffers_ > 0 )
		{
			return;
		}

		CommandListWrapper* oldestSubmittedBuffer = FindOldestSubmittedBuffer();
		if( oldestSubmittedBuffer == nullptr )
		{
			throw std::runtime_error( "No immediate command buffer is available to wait for." );
		}

		if( oldestSubmittedBuffer->fence_->GetCompletedValue() < oldestSubmittedBuffer->fenceValue_ )
		{
			oldestSubmittedBuffer->fence_->SetEventOnCompletion( oldestSubmittedBuffer->fenceValue_, oldestSubmittedBuffer->fenceEvent_ );
			WaitForSingleObject( oldestSubmittedBuffer->fenceEvent_, INFINITE );
		}

		Purge();
	}

	void ImmediateCommands::Purge()
	{
		if( buffers_.empty() )
		{
			return;
		}

		for( uint32_t i = 0; i < buffers_.size(); ++i )
		{
			CommandListWrapper& buffer = buffers_[ ( i + lastSubmitHandle_.bufferIndex_ + 1u ) % buffers_.size() ];
			if( buffer.fenceValue_ == 0 )
			{
				continue;
			}

			if( buffer.fence_->GetCompletedValue() >= buffer.fenceValue_ )
			{
				buffer.fenceValue_ = 0;
				numAvailableCommandBuffers_++;
			}
			else
			{
				return;
			}
		}
	}
}


