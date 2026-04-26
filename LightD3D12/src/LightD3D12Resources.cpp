#include "LightD3D12Resources.hpp"

#include <cstring>

namespace lightd3d12
{
	void BufferResource::BufferSubData( size_t offset, size_t size, const void* data )
	{
		assert( mappedPtr_ != nullptr );
		assert( offset + size <= bufferSize_ );

		if( data != nullptr )
		{
			std::memcpy( static_cast<uint8_t*>( mappedPtr_ ) + offset, data, size );
		}
		else
		{
			std::memset( static_cast<uint8_t*>( mappedPtr_ ) + offset, 0, size );
		}
	}

	D3D12_VERTEX_BUFFER_VIEW BufferResource::GetVertexBufferView( uint32_t stride ) const noexcept
	{
		assert( bufferType_ == BufferDesc::BufferType::VertexBuffer );
		D3D12_VERTEX_BUFFER_VIEW view{};
		view.BufferLocation = gpuAddress_;
		view.SizeInBytes = static_cast<UINT>( bufferSize_ );
		view.StrideInBytes = stride ? stride : bufferStride_;
		return view;
	}

	D3D12_INDEX_BUFFER_VIEW BufferResource::GetIndexBufferView( DXGI_FORMAT format ) const noexcept
	{
		assert( bufferType_ == BufferDesc::BufferType::IndexBuffer );
		D3D12_INDEX_BUFFER_VIEW view{};
		view.BufferLocation = gpuAddress_;
		view.SizeInBytes = static_cast<UINT>( bufferSize_ );
		view.Format = format;
		return view;
	}

	D3D12_RESOURCE_DESC BufferResource::BufferDesc( uint64_t size, D3D12_RESOURCE_FLAGS flags ) noexcept
	{
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Alignment = 0;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = flags;
		return desc;
	}

	bool TextureResource::IsDepthFormat( DXGI_FORMAT format ) noexcept
	{
		switch( format )
		{
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				return true;
			default:
				return false;
		}
	}

	bool TextureResource::IsDepthStencilFormat( DXGI_FORMAT format ) noexcept
	{
		return format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	}
}
