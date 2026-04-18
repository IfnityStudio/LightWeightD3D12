#pragma once

#include "LightD3D12Internal.hpp"

namespace lightd3d12
{
	struct BufferResource final
	{
		[[nodiscard]] uint8_t* GetMappedPtr() const noexcept
		{
			return static_cast<uint8_t*>( mappedPtr_ );
		}

		[[nodiscard]] bool IsMapped() const noexcept
		{
			return mappedPtr_ != nullptr;
		}

		void BufferSubData( size_t offset, size_t size, const void* data );
		[[nodiscard]] D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView( uint32_t stride = 0 ) const noexcept;
		[[nodiscard]] D3D12_INDEX_BUFFER_VIEW GetIndexBufferView( DXGI_FORMAT format = DXGI_FORMAT_R32_UINT ) const noexcept;

		static D3D12_RESOURCE_DESC BufferDesc( uint64_t size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE ) noexcept;

		ComPtr<ID3D12Resource> resource_;
		D3D12_GPU_VIRTUAL_ADDRESS gpuAddress_ = 0;
		uint64_t bufferSize_ = 0;
		uint32_t bufferStride_ = 0;
		D3D12_RESOURCE_STATES currentState_ = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_FLAGS resourceFlags_ = D3D12_RESOURCE_FLAG_NONE;
		D3D12_RESOURCE_DESC desc_ = {};
		D3D12_HEAP_TYPE heapType_ = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_CPU_DESCRIPTOR_HANDLE srvHandle_{ 0 };
		uint32_t srvIndex_ = UINT32_MAX;
		void* mappedPtr_ = nullptr;
	};

	struct TextureResource final
	{
		[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE GetRTV() const noexcept
		{
			return rtvHandle_;
		}

		[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE GetDSV() const noexcept
		{
			return dsvHandle_;
		}

		[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE GetSRV() const noexcept
		{
			return srvHandle_;
		}

		static bool IsDepthFormat( DXGI_FORMAT format ) noexcept;
		static bool IsDepthStencilFormat( DXGI_FORMAT format ) noexcept;

		ComPtr<ID3D12Resource> resource_;
		D3D12_RESOURCE_FLAGS usageFlags_ = D3D12_RESOURCE_FLAG_NONE;
		D3D12_RESOURCE_STATES currentState_ = D3D12_RESOURCE_STATE_COMMON;
		DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
		D3D12_RESOURCE_DESC desc_ = {};
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle_{ 0 };
		D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle_{ 0 };
		D3D12_CPU_DESCRIPTOR_HANDLE srvHandle_{ 0 };
		uint32_t rtvIndex_ = UINT32_MAX;
		uint32_t dsvIndex_ = UINT32_MAX;
		uint32_t srvIndex_ = UINT32_MAX;
		uint32_t width_ = 0;
		uint32_t height_ = 0;
		bool isDepthFormat_ = false;
		bool isStencilFormat_ = false;
		bool isSwapchainImage_ = false;
	};
}
