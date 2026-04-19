#include "LightD3D12CommandBuffer.hpp"

#include "LightD3D12ManagerImpl.hpp"

#include <cstring>

namespace lightd3d12
{
	namespace
	{
		D3D12_RENDER_PASS_BEGINNING_ACCESS CreateBeginningAccess( LoadOp loadOp, DXGI_FORMAT format, const std::array<float, 4>& clearColor )
		{
			D3D12_RENDER_PASS_BEGINNING_ACCESS beginningAccess{};

			switch( loadOp )
			{
				case LoadOp::Load:
					beginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
					break;

				case LoadOp::Clear:
					beginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
					beginningAccess.Clear.ClearValue.Format = format;
					std::memcpy( beginningAccess.Clear.ClearValue.Color, clearColor.data(), sizeof( float ) * clearColor.size() );
					break;

				case LoadOp::DontCare:
					beginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
					break;
			}

			return beginningAccess;
		}

		D3D12_RENDER_PASS_BEGINNING_ACCESS CreateDepthBeginningAccess( LoadOp loadOp, DXGI_FORMAT format, float clearDepth )
		{
			D3D12_RENDER_PASS_BEGINNING_ACCESS beginningAccess{};

			switch( loadOp )
			{
				case LoadOp::Load:
					beginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
					break;

				case LoadOp::Clear:
					beginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
					beginningAccess.Clear.ClearValue.DepthStencil.Depth = clearDepth;
					beginningAccess.Clear.ClearValue.Format = format;
					break;

				case LoadOp::DontCare:
					beginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
					break;
			}

			return beginningAccess;
		}

		D3D12_RENDER_PASS_BEGINNING_ACCESS CreateStencilBeginningAccess( LoadOp loadOp, DXGI_FORMAT format, uint8_t clearStencil )
		{
			D3D12_RENDER_PASS_BEGINNING_ACCESS beginningAccess{};

			switch( loadOp )
			{
				case LoadOp::Load:
					beginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
					break;

				case LoadOp::Clear:
					beginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
					beginningAccess.Clear.ClearValue.DepthStencil.Stencil = clearStencil;
					beginningAccess.Clear.ClearValue.Format = format;
					break;

				case LoadOp::DontCare:
					beginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
					break;
			}

			return beginningAccess;
		}

		D3D12_RENDER_PASS_ENDING_ACCESS CreateEndingAccess( StoreOp storeOp )
		{
			D3D12_RENDER_PASS_ENDING_ACCESS endingAccess{};
			endingAccess.Type = storeOp == StoreOp::Store ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
			return endingAccess;
		}

		D3D12_RENDER_PASS_BEGINNING_ACCESS CreateNoAccessBeginningAccess()
		{
			D3D12_RENDER_PASS_BEGINNING_ACCESS beginningAccess{};
			beginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
			return beginningAccess;
		}

		D3D12_RENDER_PASS_ENDING_ACCESS CreateNoAccessEndingAccess()
		{
			D3D12_RENDER_PASS_ENDING_ACCESS endingAccess{};
			endingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
			return endingAccess;
		}
	}

	CommandBufferImpl::CommandBufferImpl( DeviceManager::Impl& manager, ImmediateCommands::CommandListWrapper& wrapper ):
		manager_( manager ),
		wrapper_( wrapper )
	{
	}

	void CommandBufferImpl::CmdBeginRendering( const RenderPass& renderPass, const Framebuffer& framebuffer )
	{
		if( isRendering_ )
		{
			throw std::runtime_error( "Nested render passes are not supported." );
		}

		std::array<D3D12_RENDER_PASS_RENDER_TARGET_DESC, ourMaxColorAttachments> renderTargetDescs{};
		uint32_t numRenderTargets = 0;

		TextureResource* viewportTexture = nullptr;

		for( uint32_t index = 0; index < framebuffer.color.size(); ++index )
		{
			if( !framebuffer.color[ index ].texture.Valid() )
			{
				continue;
			}

			auto& colorTexture = manager_.GetTextureResource( framebuffer.color[ index ].texture );
			if( colorTexture.rtvHandle_.ptr == 0 )
			{
				throw std::runtime_error( "Color attachment does not have an RTV." );
			}

			if( colorTexture.currentState_ != D3D12_RESOURCE_STATE_RENDER_TARGET )
			{
				const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
					colorTexture.resource_.Get(),
					colorTexture.currentState_,
					D3D12_RESOURCE_STATE_RENDER_TARGET );
				wrapper_.commandList_->ResourceBarrier( 1, &barrier );
				colorTexture.currentState_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
			}

			renderTargetDescs[ numRenderTargets ].cpuDescriptor = colorTexture.rtvHandle_;
			renderTargetDescs[ numRenderTargets ].BeginningAccess = CreateBeginningAccess( renderPass.color[ index ].loadOp, colorTexture.format_, renderPass.color[ index ].clearColor );
			renderTargetDescs[ numRenderTargets ].EndingAccess = CreateEndingAccess( renderPass.color[ index ].storeOp );
			numRenderTargets++;

			if( viewportTexture == nullptr )
			{
				viewportTexture = &colorTexture;
			}
		}

		D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depthStencilDesc{};
		D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depthStencilDescPtr = nullptr;

		if( framebuffer.depthStencil.texture.Valid() )
		{
			auto& depthTexture = manager_.GetTextureResource( framebuffer.depthStencil.texture );
			if( depthTexture.dsvHandle_.ptr == 0 )
			{
				throw std::runtime_error( "Depth attachment does not have a DSV." );
			}

			if( depthTexture.currentState_ != D3D12_RESOURCE_STATE_DEPTH_WRITE )
			{
				const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
					depthTexture.resource_.Get(),
					depthTexture.currentState_,
					D3D12_RESOURCE_STATE_DEPTH_WRITE );
				wrapper_.commandList_->ResourceBarrier( 1, &barrier );
				depthTexture.currentState_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			}

			depthStencilDesc.cpuDescriptor = depthTexture.dsvHandle_;
			depthStencilDesc.DepthBeginningAccess = depthTexture.isDepthFormat_
				? CreateDepthBeginningAccess( renderPass.depthStencil.depthLoadOp, depthTexture.format_, renderPass.depthStencil.clearDepth )
				: CreateNoAccessBeginningAccess();
			depthStencilDesc.DepthEndingAccess = depthTexture.isDepthFormat_
				? CreateEndingAccess( renderPass.depthStencil.depthStoreOp )
				: CreateNoAccessEndingAccess();
			depthStencilDesc.StencilBeginningAccess = depthTexture.isStencilFormat_
				? CreateStencilBeginningAccess( renderPass.depthStencil.stencilLoadOp, depthTexture.format_, renderPass.depthStencil.clearStencil )
				: CreateNoAccessBeginningAccess();
			depthStencilDesc.StencilEndingAccess = depthTexture.isStencilFormat_
				? CreateEndingAccess( renderPass.depthStencil.stencilStoreOp )
				: CreateNoAccessEndingAccess();
			depthStencilDescPtr = &depthStencilDesc;

			if( viewportTexture == nullptr )
			{
				viewportTexture = &depthTexture;
			}
		}

		if( viewportTexture == nullptr )
		{
			throw std::runtime_error( "Framebuffer does not contain any attachments." );
		}

		wrapper_.commandList_->BeginRenderPass(
			numRenderTargets,
			numRenderTargets > 0 ? renderTargetDescs.data() : nullptr,
			depthStencilDescPtr,
			D3D12_RENDER_PASS_FLAG_NONE );

		D3D12_VIEWPORT viewport{};
		viewport.Width = static_cast<float>( viewportTexture->width_ );
		viewport.Height = static_cast<float>( viewportTexture->height_ );
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		wrapper_.commandList_->RSSetViewports( 1, &viewport );

		D3D12_RECT scissor{ 0, 0, static_cast<LONG>( viewportTexture->width_ ), static_cast<LONG>( viewportTexture->height_ ) };
		wrapper_.commandList_->RSSetScissorRects( 1, &scissor );
		isRendering_ = true;
	}

	void CommandBufferImpl::CmdEndRendering()
	{
		if( !isRendering_ )
		{
			return;
		}

		wrapper_.commandList_->EndRenderPass();
		isRendering_ = false;
	}

	void CommandBufferImpl::CmdTransitionTexture( TextureHandle texture, D3D12_RESOURCE_STATES newState )
	{
		TextureResource& resource = manager_.GetTextureResource( texture );
		if( resource.currentState_ == newState )
		{
			return;
		}

		const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			resource.resource_.Get(),
			resource.currentState_,
			newState );
		wrapper_.commandList_->ResourceBarrier( 1, &barrier );
		resource.currentState_ = newState;
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


