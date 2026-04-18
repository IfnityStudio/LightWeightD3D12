#include "LightD3D12CommandBuffer.hpp"

#include "LightD3D12ManagerImpl.hpp"

#include <cstring>

namespace lightd3d12
{
	CommandBufferImpl::CommandBufferImpl( DeviceManager::Impl& manager, ImmediateCommands::CommandListWrapper& wrapper ):
		manager_( manager ),
		wrapper_( wrapper )
	{
	}

	void CommandBufferImpl::CmdBeginRendering( const RenderPass& renderPass, const Framebuffer& framebuffer )
	{
		auto& texture = manager_.GetTextureResource( framebuffer.color[ 0 ].texture );
		if( texture.currentState_ != D3D12_RESOURCE_STATE_RENDER_TARGET )
		{
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				texture.resource_.Get(),
				texture.currentState_,
				D3D12_RESOURCE_STATE_RENDER_TARGET );
			wrapper_.commandList_->ResourceBarrier( 1, &barrier );
			texture.currentState_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
		}

		wrapper_.commandList_->OMSetRenderTargets( 1, &texture.rtvHandle_, FALSE, nullptr );
		if( renderPass.color[ 0 ].loadOp == LoadOp::Clear )
		{
			wrapper_.commandList_->ClearRenderTargetView( texture.rtvHandle_, renderPass.color[ 0 ].clearColor.data(), 0, nullptr );
		}

		D3D12_VIEWPORT viewport{};
		viewport.Width = static_cast<float>( texture.width_ );
		viewport.Height = static_cast<float>( texture.height_ );
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		wrapper_.commandList_->RSSetViewports( 1, &viewport );

		D3D12_RECT scissor{ 0, 0, static_cast<LONG>( texture.width_ ), static_cast<LONG>( texture.height_ ) };
		wrapper_.commandList_->RSSetScissorRects( 1, &scissor );
		isRendering_ = true;
	}

	void CommandBufferImpl::CmdEndRendering()
	{
		isRendering_ = false;
	}

	void CommandBufferImpl::CmdBindRenderPipeline( const RenderPipelineState& pipeline )
	{
		ID3D12DescriptorHeap* heaps[] = { manager_.bindlessHeap_.Get() };
		wrapper_.commandList_->SetDescriptorHeaps( 1, heaps );
		wrapper_.commandList_->SetGraphicsRootSignature( manager_.rootSignature_.Get() );
		wrapper_.commandList_->SetPipelineState( pipeline.pipelineState_.Get() );
		wrapper_.commandList_->IASetPrimitiveTopology( pipeline.topology_ );
	}

	void CommandBufferImpl::CmdBindVertexBuffer( BufferHandle buffer, uint32_t stride, uint32_t offset )
	{
		const auto& resource = manager_.GetBufferResource( buffer );
		auto view = resource.GetVertexBufferView( stride );
		view.BufferLocation += offset;
		view.SizeInBytes -= offset;
		wrapper_.commandList_->IASetVertexBuffers( 0, 1, &view );
	}

	void CommandBufferImpl::CmdBindIndexBuffer( BufferHandle buffer, DXGI_FORMAT format, uint32_t offset )
	{
		const auto& resource = manager_.GetBufferResource( buffer );
		auto view = resource.GetIndexBufferView( format );
		view.BufferLocation += offset;
		view.SizeInBytes -= offset;
		wrapper_.commandList_->IASetIndexBuffer( &view );
	}

	void CommandBufferImpl::CmdPushConstants( const void* data, uint32_t sizeBytes, uint32_t offset32BitValues )
	{
		wrapper_.commandList_->SetGraphicsRoot32BitConstants( 0, sizeBytes / 4u, data, offset32BitValues );
	}

	void CommandBufferImpl::CmdPushDebugGroupLabel( const char* label, uint32_t )
	{
		if( label != nullptr && label[ 0 ] != '\0' )
		{
			// Raw D3D12 BeginEvent expects PIX-encoded metadata, not a plain C string.
			// Keep the API surface for future PIX integration, but avoid corrupting the command list.
			debugGroupDepth_++;
		}
	}

	void CommandBufferImpl::CmdPopDebugGroupLabel()
	{
		if( debugGroupDepth_ > 0 )
		{
			debugGroupDepth_--;
		}
	}

	void CommandBufferImpl::CmdDraw( uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance )
	{
		wrapper_.commandList_->DrawInstanced( vertexCount, instanceCount, firstVertex, firstInstance );
	}

	void CommandBufferImpl::CmdDrawIndexedIndirect( BufferHandle indirectBuffer, uint32_t drawCount, uint64_t byteOffset )
	{
		const auto& resource = manager_.GetBufferResource( indirectBuffer );
		wrapper_.commandList_->ExecuteIndirect(
			manager_.commandSignature_.Get(),
			drawCount,
			resource.resource_.Get(),
			byteOffset,
			nullptr,
			0 );
	}
}


