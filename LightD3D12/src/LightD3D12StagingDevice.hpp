#pragma once

#include "LightD3D12Internal.hpp"

namespace lightd3d12
{
	struct BufferResource;
	struct TextureResource;
	class DeviceManager::Impl;

	class StagingDevice final
	{
	public:
		struct TextureSubresourceUpload
		{
			const void* data = nullptr;
			uint32_t rowPitch = 0;
			uint32_t slicePitch = 0;
		};

		explicit StagingDevice( DeviceManager::Impl& manager );
		StagingDevice( const StagingDevice& ) = delete;
		StagingDevice& operator=( const StagingDevice& ) = delete;

		void BufferSubData( BufferResource& buffer, size_t dstOffset, size_t size, const void* data );
		void TextureSubData2D( TextureResource& texture, const void* data, uint32_t rowPitch, uint32_t slicePitch );
		void TextureSubData2D( TextureResource& texture, const TextureSubresourceUpload* subresources, uint32_t subresourceCount );

	private:
		DeviceManager::Impl& manager_;
	};
}


