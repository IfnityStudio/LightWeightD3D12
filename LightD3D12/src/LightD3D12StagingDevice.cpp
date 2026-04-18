#include "LightD3D12StagingDevice.hpp"

#include "LightD3D12ManagerImpl.hpp"

namespace lightd3d12
{
	StagingDevice::StagingDevice( DeviceManager::Impl& manager ): manager_( manager )
	{
		if( manager_.device_ == nullptr )
		{
			throw std::runtime_error( "StagingDevice requires a valid D3D12 device." );
		}
	}

	void StagingDevice::BufferSubData( BufferResource& buffer, size_t dstOffset, size_t size, const void* data )
	{
		if( buffer.IsMapped() )
		{
			buffer.BufferSubData( dstOffset, size, data );
			return;
		}

		ComPtr<ID3D12Resource> stagingBuffer;
		CD3DX12_HEAP_PROPERTIES heapProps( D3D12_HEAP_TYPE_UPLOAD );
		const D3D12_RESOURCE_DESC BufferDesc = CD3DX12_RESOURCE_DESC::Buffer( size );

		detail::ThrowIfFailed(
			manager_.device_->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&BufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS( stagingBuffer.GetAddressOf() ) ),
			"Failed to create staging buffer." );

		void* mapped = nullptr;
		detail::ThrowIfFailed( stagingBuffer->Map( 0, nullptr, &mapped ), "Failed to map staging buffer." );
		std::memcpy( mapped, data, size );
		stagingBuffer->Unmap( 0, nullptr );

		auto& cmd = manager_.immediateCommands_->Acquire();
		const auto previousState = buffer.currentState_;
		if( previousState != D3D12_RESOURCE_STATE_COPY_DEST )
		{
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				buffer.resource_.Get(),
				previousState,
				D3D12_RESOURCE_STATE_COPY_DEST );
			cmd.commandList_->ResourceBarrier( 1, &barrier );
			buffer.currentState_ = D3D12_RESOURCE_STATE_COPY_DEST;
		}

		cmd.commandList_->CopyBufferRegion( buffer.resource_.Get(), dstOffset, stagingBuffer.Get(), 0, size );

		if( previousState != D3D12_RESOURCE_STATE_COPY_DEST )
		{
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				buffer.resource_.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				previousState );
			cmd.commandList_->ResourceBarrier( 1, &barrier );
			buffer.currentState_ = previousState;
		}

		const SubmitHandle handle = manager_.immediateCommands_->Submit( cmd );
		manager_.AddDeferredRelease(
			handle,
			[staging = std::move( stagingBuffer )]() mutable
			{
				staging.Reset();
			} );
	}

	void StagingDevice::TextureSubData2D( TextureResource& texture, const void* data, uint32_t rowPitch, uint32_t slicePitch )
	{
		if( data == nullptr || rowPitch == 0 || slicePitch == 0 )
		{
			return;
		}

		UINT64 uploadBufferSize = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
		UINT numRows = 0;
		UINT64 rowSizeInBytes = 0;
		manager_.device_->GetCopyableFootprints( &texture.desc_, 0, 1, 0, &layout, &numRows, &rowSizeInBytes, &uploadBufferSize );

		ComPtr<ID3D12Resource> stagingBuffer;
		CD3DX12_HEAP_PROPERTIES heapProps( D3D12_HEAP_TYPE_UPLOAD );
		const D3D12_RESOURCE_DESC BufferDesc = CD3DX12_RESOURCE_DESC::Buffer( uploadBufferSize );
		detail::ThrowIfFailed(
			manager_.device_->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&BufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS( stagingBuffer.GetAddressOf() ) ),
			"Failed to create texture staging buffer." );

		void* mapped = nullptr;
		detail::ThrowIfFailed( stagingBuffer->Map( 0, nullptr, &mapped ), "Failed to map texture staging buffer." );

		const auto* srcBytes = static_cast<const uint8_t*>( data );
		auto* dstBytes = static_cast<uint8_t*>( mapped );
		for( UINT row = 0; row < numRows; ++row )
		{
			std::memcpy(
				dstBytes + layout.Offset + row * layout.Footprint.RowPitch,
				srcBytes + row * rowPitch,
				std::min<size_t>( rowPitch, layout.Footprint.RowPitch ) );
		}

		stagingBuffer->Unmap( 0, nullptr );

		auto& cmd = manager_.immediateCommands_->Acquire();
		const auto previousState = texture.currentState_;
		if( previousState != D3D12_RESOURCE_STATE_COPY_DEST )
		{
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				texture.resource_.Get(),
				previousState,
				D3D12_RESOURCE_STATE_COPY_DEST );
			cmd.commandList_->ResourceBarrier( 1, &barrier );
			texture.currentState_ = D3D12_RESOURCE_STATE_COPY_DEST;
		}

		D3D12_TEXTURE_COPY_LOCATION dstLocation{};
		dstLocation.pResource = texture.resource_.Get();
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLocation.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION srcLocation{};
		srcLocation.pResource = stagingBuffer.Get();
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLocation.PlacedFootprint = layout;

		cmd.commandList_->CopyTextureRegion( &dstLocation, 0, 0, 0, &srcLocation, nullptr );

		if( previousState != D3D12_RESOURCE_STATE_COPY_DEST )
		{
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				texture.resource_.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				previousState );
			cmd.commandList_->ResourceBarrier( 1, &barrier );
			texture.currentState_ = previousState;
		}

		const SubmitHandle handle = manager_.immediateCommands_->Submit( cmd );
		manager_.AddDeferredRelease(
			handle,
			[staging = std::move( stagingBuffer )]() mutable
			{
				staging.Reset();
			} );
	}
}


