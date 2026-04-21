#pragma once

#include "RenderTypes.hpp"

#include "LightD3D12/LightD3D12.hpp"

#include <filesystem>

namespace mini2d
{
	class SpriteRenderer2D
	{
	public:
		void Initialize( lightd3d12::RenderDevice& renderDevice, DXGI_FORMAT backbufferFormat, const std::filesystem::path& enemyTexturePath );
		void Shutdown( lightd3d12::RenderDevice& renderDevice );
		void Render( lightd3d12::RenderDevice& renderDevice, lightd3d12::ICommandBuffer& commandBuffer, const RenderFrame& frame, uint32_t width, uint32_t height );

	private:
		lightd3d12::TextureHandle ResolveTextureHandle( SpriteId spriteId ) const;

		lightd3d12::RenderPipelineState pipeline_;
		lightd3d12::TextureHandle enemyTexture_ = {};
		lightd3d12::TextureHandle whiteTexture_ = {};
	};
}
