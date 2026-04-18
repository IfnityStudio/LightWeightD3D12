#pragma once

#include "LightD3D12Internal.hpp"
#include "LightD3D12ImmediateCommands.hpp"

namespace lightd3d12
{
	class DeviceManager::Impl;

	class CommandBufferImpl final: public ICommandBuffer
	{
	public:
		CommandBufferImpl( DeviceManager::Impl& manager, ImmediateCommands::CommandListWrapper& wrapper );

		void CmdBeginRendering( const RenderPass& renderPass, const Framebuffer& framebuffer ) override;
		void CmdEndRendering() override;
		void CmdBindRenderPipeline( const RenderPipelineState& pipeline ) override;
		void CmdBindVertexBuffer( BufferHandle buffer, uint32_t stride = 0, uint32_t offset = 0 ) override;
		void CmdBindIndexBuffer( BufferHandle buffer, DXGI_FORMAT format = DXGI_FORMAT_R32_UINT, uint32_t offset = 0 ) override;
		void CmdPushConstants( const void* data, uint32_t sizeBytes, uint32_t offset32BitValues = 0 ) override;
		void CmdPushDebugGroupLabel( const char* label, uint32_t color ) override;
		void CmdPopDebugGroupLabel() override;
		void CmdDraw( uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0 ) override;
		void CmdDrawIndexedIndirect( BufferHandle indirectBuffer, uint32_t drawCount, uint64_t byteOffset = 0 ) override;

		ImmediateCommands::CommandListWrapper& Wrapper() noexcept
		{
			return wrapper_;
		}

	private:
		DeviceManager::Impl& manager_;
		ImmediateCommands::CommandListWrapper& wrapper_;
		bool isRendering_ = false;
		uint32_t debugGroupDepth_ = 0;
	};
}


