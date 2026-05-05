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
		explicit StagingDevice( DeviceManager::Impl& manager );
		StagingDevice( const StagingDevice& ) = delete;
		StagingDevice& operator=( const StagingDevice& ) = delete;

		void BufferSubData( BufferResource& buffer, size_t dstOffset, size_t size, const void* data );
		void TextureSubData2D( TextureResource& texture, const void* data, uint32_t rowPitch, uint32_t slicePitch );

	private:
		DeviceManager::Impl& manager_;
	};
}


