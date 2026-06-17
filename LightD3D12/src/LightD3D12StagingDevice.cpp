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

		C_RESULT(
			manager_.device_->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&BufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS( stagingBuffer.GetAddressOf() ) ),
			"Failed to create staging buffer." );

		void* mapped = nullptr;
		C_RESULT( stagingBuffer->Map( 0, nullptr, &mapped ), "Failed to map staging buffer." );
		std::memcpy( mapped, data, size );
		stagingBuffer->Unmap( 0, nullptr );

		auto& queue = manager_.GetGraphicsQueueContext();
		auto& cmd = queue.immediateCommands_->Acquire();
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

		const SubmitHandle handle = queue.immediateCommands_->Submit( cmd );
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

		if( texture.dimension_ != TextureDimension::Texture2D || texture.depthOrArraySize_ != 1 )
		{
			throw std::runtime_error( "TextureSubData2D only supports single-slice Texture2D uploads." );
		}

		UINT64 uploadBufferSize = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
		UINT numRows = 0;
		UINT64 rowSizeInBytes = 0;
		manager_.device_->GetCopyableFootprints( &texture.desc_, 0, 1, 0, &layout, &numRows, &rowSizeInBytes, &uploadBufferSize );

		if( rowPitch < rowSizeInBytes )
		{
			throw std::runtime_error( "TextureSubData2D rowPitch is smaller than the required source row size." );
		}

		UINT64 requiredSlicePitch = rowSizeInBytes;
		if( numRows > 1u )
		{
			requiredSlicePitch += static_cast<UINT64>( numRows - 1u ) * static_cast<UINT64>( rowPitch );
		}

		if( slicePitch < requiredSlicePitch )
		{
			throw std::runtime_error( "TextureSubData2D slicePitch is smaller than the required source data size." );
		}

		ComPtr<ID3D12Resource> stagingBuffer;
		CD3DX12_HEAP_PROPERTIES heapProps( D3D12_HEAP_TYPE_UPLOAD );
		const D3D12_RESOURCE_DESC BufferDesc = CD3DX12_RESOURCE_DESC::Buffer( uploadBufferSize );
		C_RESULT(
			manager_.device_->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&BufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS( stagingBuffer.GetAddressOf() ) ),
			"Failed to create texture staging buffer." );

		void* mapped = nullptr;
		C_RESULT( stagingBuffer->Map( 0, nullptr, &mapped ), "Failed to map texture staging buffer." );

		auto* dstBytes = static_cast<uint8_t*>( mapped );
		const auto* srcBytes = static_cast<const uint8_t*>( data );
		for( UINT row = 0; row < numRows; ++row )
		{
			std::memcpy(
				dstBytes + layout.Offset + static_cast<size_t>( row ) * layout.Footprint.RowPitch,
				srcBytes + static_cast<size_t>( row ) * rowPitch,
				static_cast<size_t>( rowSizeInBytes ) );
		}

		stagingBuffer->Unmap( 0, nullptr );

		auto& queue = manager_.GetGraphicsQueueContext();
		auto& cmd = queue.immediateCommands_->Acquire();
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

		const SubmitHandle handle = queue.immediateCommands_->Submit( cmd );
		manager_.AddDeferredRelease(
			handle,
			[staging = std::move( stagingBuffer )]() mutable
			{
				staging.Reset();
			} );
	}

	void StagingDevice::TextureData2D( TextureResource& texture, void* outData, uint32_t rowPitch, uint32_t slicePitch )
	{
		if( outData == nullptr || rowPitch == 0 || slicePitch == 0 )
		{
			return;
		}

		if( texture.dimension_ != TextureDimension::Texture2D || texture.depthOrArraySize_ != 1 )
		{
			throw std::runtime_error( "TextureData2D only supports single-slice Texture2D downloads." );
		}

		if( texture.desc_.SampleDesc.Count > 1 )
		{
			throw std::runtime_error( "TextureData2D does not support multisampled textures." );
		}

		UINT64 readbackBufferSize = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
		UINT numRows = 0;
		UINT64 rowSizeInBytes = 0;
		manager_.device_->GetCopyableFootprints( &texture.desc_, 0, 1, 0, &layout, &numRows, &rowSizeInBytes, &readbackBufferSize );

		if( rowPitch < rowSizeInBytes )
		{
			throw std::runtime_error( "TextureData2D rowPitch is smaller than the required destination row size." );
		}

		UINT64 requiredSlicePitch = rowSizeInBytes;
		if( numRows > 1u )
		{
			requiredSlicePitch += static_cast<UINT64>( numRows - 1u ) * static_cast<UINT64>( rowPitch );
		}

		if( slicePitch < requiredSlicePitch )
		{
			throw std::runtime_error( "TextureData2D slicePitch is smaller than the required destination data size." );
		}

		ComPtr<ID3D12Resource> readbackBuffer;
		CD3DX12_HEAP_PROPERTIES heapProps( D3D12_HEAP_TYPE_READBACK );
		const D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer( readbackBufferSize );
		C_RESULT(
			manager_.device_->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&bufferDesc,
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS( readbackBuffer.GetAddressOf() ) ),
			"Failed to create texture readback buffer." );

		auto& queue = manager_.GetGraphicsQueueContext();
		auto& cmd = queue.immediateCommands_->Acquire();
		const auto previousState = texture.currentState_;
		if( previousState != D3D12_RESOURCE_STATE_COPY_SOURCE )
		{
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				texture.resource_.Get(),
				previousState,
				D3D12_RESOURCE_STATE_COPY_SOURCE );
			cmd.commandList_->ResourceBarrier( 1, &barrier );
			texture.currentState_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
		}

		D3D12_TEXTURE_COPY_LOCATION dstLocation{};
		dstLocation.pResource = readbackBuffer.Get();
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dstLocation.PlacedFootprint = layout;

		D3D12_TEXTURE_COPY_LOCATION srcLocation{};
		srcLocation.pResource = texture.resource_.Get();
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcLocation.SubresourceIndex = 0;

		cmd.commandList_->CopyTextureRegion( &dstLocation, 0, 0, 0, &srcLocation, nullptr );

		if( previousState != D3D12_RESOURCE_STATE_COPY_SOURCE )
		{
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				texture.resource_.Get(),
				D3D12_RESOURCE_STATE_COPY_SOURCE,
				previousState );
			cmd.commandList_->ResourceBarrier( 1, &barrier );
			texture.currentState_ = previousState;
		}

		const SubmitHandle handle = queue.immediateCommands_->Submit( cmd );
		queue.immediateCommands_->Wait( handle );

		void* mapped = nullptr;
		const D3D12_RANGE readRange{ layout.Offset, layout.Offset + readbackBufferSize };
		C_RESULT( readbackBuffer->Map( 0, &readRange, &mapped ), "Failed to map texture readback buffer." );

		const auto* srcBytes = static_cast<const uint8_t*>( mapped );
		auto* dstBytes = static_cast<uint8_t*>( outData );
		for( UINT row = 0; row < numRows; ++row )
		{
			std::memcpy(
				dstBytes + static_cast<size_t>( row ) * rowPitch,
				srcBytes + layout.Offset + static_cast<size_t>( row ) * layout.Footprint.RowPitch,
				static_cast<size_t>( rowSizeInBytes ) );
		}

		const D3D12_RANGE writtenRange{ 0, 0 };
		readbackBuffer->Unmap( 0, &writtenRange );
	}
}


