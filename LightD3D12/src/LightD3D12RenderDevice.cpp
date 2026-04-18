#include "LightD3D12ManagerImpl.hpp"

#include "LightD3D12StagingDevice.hpp"
#include "LightD3D12Swapchain.hpp"

#include <d3dcompiler.h>
#include <cstring>

namespace lightd3d12
{
	RenderPipelineDesc::RenderPipelineDesc() noexcept
	{
		blendState = CD3DX12_BLEND_DESC( D3D12_DEFAULT );
		rasterizerState = CD3DX12_RASTERIZER_DESC( D3D12_DEFAULT );
		depthStencilState = CD3DX12_DEPTH_STENCIL_DESC( D3D12_DEFAULT );
	}

	RenderPipelineState::RenderPipelineState( RenderPipelineState&& other ) noexcept
	{
		*this = std::move( other );
	}

	RenderPipelineState& RenderPipelineState::operator=( RenderPipelineState&& other ) noexcept
	{
		if( this != &other )
		{
			pipelineState_ = std::move( other.pipelineState_ );
			topology_ = other.topology_;
			other.topology_ = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		}
		return *this;
	}

	bool RenderPipelineState::Valid() const noexcept
	{
		return pipelineState_ != nullptr;
	}

	RenderDevice::RenderDevice( DeviceManager& manager ) noexcept: manager_( &manager )
	{
	}

	ICommandBuffer& RenderDevice::AcquireCommandBuffer()
	{
		auto& impl = *manager_->impl_;
		if( impl.currentCommandBuffer_ != nullptr )
		{
			throw std::runtime_error( "Only one active command buffer is allowed per render device." );
		}

		impl.ProcessDeferredReleases();
		auto& wrapper = impl.immediateCommands_->Acquire();
		impl.currentCommandBuffer_ = std::make_unique<CommandBufferImpl>( impl, wrapper );
		return *impl.currentCommandBuffer_;
	}

	TextureHandle RenderDevice::GetCurrentSwapchainTexture() const
	{
		const auto& impl = *manager_->impl_;
		if( impl.swapchain_ == nullptr )
		{
			return {};
		}

		return impl.swapchain_->GetCurrentTexture();
	}

	SubmitHandle RenderDevice::Submit( ICommandBuffer& buffer, TextureHandle presentTexture )
	{
		auto& impl = *manager_->impl_;
		auto* commandBuffer = dynamic_cast<CommandBufferImpl*>( &buffer );
		if( commandBuffer == nullptr || commandBuffer != impl.currentCommandBuffer_.get() )
		{
			throw std::runtime_error( "The command buffer does not belong to this render device." );
		}

		auto& texture = impl.GetTextureResource( presentTexture );
		if( texture.currentState_ != D3D12_RESOURCE_STATE_PRESENT )
		{
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				texture.resource_.Get(),
				texture.currentState_,
				D3D12_RESOURCE_STATE_PRESENT );
			commandBuffer->Wrapper().commandList_->ResourceBarrier( 1, &barrier );
			texture.currentState_ = D3D12_RESOURCE_STATE_PRESENT;
		}

		const SubmitHandle handle = impl.immediateCommands_->Submit( commandBuffer->Wrapper() );
		if( impl.swapchain_ == nullptr )
		{
			throw std::runtime_error( "Swapchain is not initialized." );
		}
		impl.swapchain_->Present();
		impl.currentCommandBuffer_.reset();
		impl.ProcessDeferredReleases();
		return handle;
	}

	RenderPipelineState RenderDevice::CreateRenderPipeline( const RenderPipelineDesc& desc )
	{
		auto& impl = *manager_->impl_;
		if( desc.vertexShader.source == nullptr || desc.fragmentShader.source == nullptr )
		{
			throw std::runtime_error( "RenderPipelineDesc requires valid vertex and fragment shader source." );
		}

		ComPtr<ID3DBlob> vsBlob;
		ComPtr<ID3DBlob> psBlob;
		ComPtr<ID3DBlob> errors;

		detail::ThrowIfFailed(
			D3DCompile(
				desc.vertexShader.source,
				std::strlen( desc.vertexShader.source ),
				nullptr,
				nullptr,
				nullptr,
				desc.vertexShader.entryPoint,
				desc.vertexShader.profile ? desc.vertexShader.profile : "vs_5_1",
				0,
				0,
				vsBlob.GetAddressOf(),
				errors.GetAddressOf() ),
			"Failed to compile vertex shader." );

		errors.Reset();
		detail::ThrowIfFailed(
			D3DCompile(
				desc.fragmentShader.source,
				std::strlen( desc.fragmentShader.source ),
				nullptr,
				nullptr,
				nullptr,
				desc.fragmentShader.entryPoint,
				desc.fragmentShader.profile ? desc.fragmentShader.profile : "ps_5_1",
				0,
				0,
				psBlob.GetAddressOf(),
				errors.GetAddressOf() ),
			"Failed to compile fragment shader." );

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = impl.rootSignature_.Get();
		psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
		psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
		psoDesc.BlendState = desc.blendState;
		psoDesc.RasterizerState = desc.rasterizerState;
		psoDesc.DepthStencilState = desc.depthStencilState;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = desc.primitiveType;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[ 0 ] = desc.colorFormat;
		psoDesc.DSVFormat = desc.depthFormat;
		psoDesc.SampleDesc = { 1, 0 };

		RenderPipelineState pipeline;
		detail::ThrowIfFailed( impl.device_->CreateGraphicsPipelineState( &psoDesc, IID_PPV_ARGS( pipeline.pipelineState_.GetAddressOf() ) ), "Failed to create graphics pipeline state." );
		pipeline.topology_ = desc.topology;
		return pipeline;
	}

	BufferHandle RenderDevice::CreateBuffer( const BufferDesc& desc )
	{
		auto& impl = *manager_->impl_;
		if( desc.size == 0 )
		{
			throw std::runtime_error( "BufferDesc.size must be greater than zero." );
		}

		BufferResource resource;
		resource.bufferSize_ = desc.size;
		resource.bufferStride_ = desc.stride;
		resource.resourceFlags_ = desc.flags;
		resource.heapType_ = desc.heapType;
		resource.desc_ = BufferResource::BufferDesc( desc.size, desc.flags );
		resource.currentState_ = desc.heapType == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : desc.initialState;

		const auto heapProps = CD3DX12_HEAP_PROPERTIES( desc.heapType );
		detail::ThrowIfFailed(
			impl.device_->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&resource.desc_,
				resource.currentState_,
				nullptr,
				IID_PPV_ARGS( resource.resource_.GetAddressOf() ) ),
			"Failed to create buffer resource." );

		resource.gpuAddress_ = resource.resource_->GetGPUVirtualAddress();
		if( desc.heapType == D3D12_HEAP_TYPE_UPLOAD )
		{
			detail::ThrowIfFailed( resource.resource_->Map( 0, nullptr, &resource.mappedPtr_ ), "Failed to map upload buffer." );
		}

		if( desc.createShaderResourceView )
		{
			resource.srvIndex_ = impl.AllocateBindlessDescriptor();
			resource.srvHandle_ = impl.bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
			resource.srvHandle_.ptr += static_cast<SIZE_T>( resource.srvIndex_ ) * impl.bindlessDescriptorSize_;

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			if( desc.rawShaderResourceView )
			{
				srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
				srvDesc.Buffer.NumElements = static_cast<UINT>( desc.size / 4u );
				srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			}
			else
			{
				srvDesc.Format = DXGI_FORMAT_UNKNOWN;
				srvDesc.Buffer.StructureByteStride = desc.stride;
				srvDesc.Buffer.NumElements = desc.stride ? static_cast<UINT>( desc.size / desc.stride ) : 0u;
			}

			impl.device_->CreateShaderResourceView( resource.resource_.Get(), &srvDesc, resource.srvHandle_ );
		}

		if( desc.data != nullptr && desc.dataSize > 0 )
		{
			impl.stagingDevice_->BufferSubData( resource, 0, static_cast<size_t>( desc.dataSize ), desc.data );
		}

		return impl.slotMapBuffers_.Create( std::move( resource ) );
	}

	TextureHandle RenderDevice::CreateTexture( const TextureDesc& desc )
	{
		auto& impl = *manager_->impl_;
		TextureResource resource;
		resource.width_ = desc.width;
		resource.height_ = desc.height;
		resource.format_ = desc.format;
		resource.usageFlags_ = desc.flags;
		resource.currentState_ = desc.initialState;
		resource.isDepthFormat_ = TextureResource::IsDepthFormat( desc.format );
		resource.isStencilFormat_ = TextureResource::IsDepthStencilFormat( desc.format );

		resource.desc_ = CD3DX12_RESOURCE_DESC::Tex2D(
			desc.format,
			desc.width,
			desc.height,
			desc.depthOrArraySize,
			desc.mipLevels,
			1,
			0,
			desc.flags );

		const auto heapProps = CD3DX12_HEAP_PROPERTIES( D3D12_HEAP_TYPE_DEFAULT );
		const D3D12_CLEAR_VALUE* clearValue = desc.useClearValue ? &desc.clearValue : nullptr;
		detail::ThrowIfFailed(
			impl.device_->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&resource.desc_,
				desc.initialState,
				clearValue,
				IID_PPV_ARGS( resource.resource_.GetAddressOf() ) ),
			"Failed to create texture resource." );

		if( desc.createShaderResourceView )
		{
			resource.srvIndex_ = impl.AllocateBindlessDescriptor();
			resource.srvHandle_ = impl.bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
			resource.srvHandle_.ptr += static_cast<SIZE_T>( resource.srvIndex_ ) * impl.bindlessDescriptorSize_;

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Format = desc.format;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = desc.mipLevels;
			impl.device_->CreateShaderResourceView( resource.resource_.Get(), &srvDesc, resource.srvHandle_ );
		}

		if( desc.createRenderTargetView )
		{
			resource.rtvIndex_ = impl.AllocateRtvDescriptor();
			resource.rtvHandle_ = impl.rtvHeap_->GetCPUDescriptorHandleForHeapStart();
			resource.rtvHandle_.ptr += static_cast<SIZE_T>( resource.rtvIndex_ ) * impl.rtvDescriptorSize_;
			impl.device_->CreateRenderTargetView( resource.resource_.Get(), nullptr, resource.rtvHandle_ );
		}

		if( desc.createDepthStencilView )
		{
			resource.dsvIndex_ = impl.AllocateDsvDescriptor();
			resource.dsvHandle_ = impl.dsvHeap_->GetCPUDescriptorHandleForHeapStart();
			resource.dsvHandle_.ptr += static_cast<SIZE_T>( resource.dsvIndex_ ) * impl.dsvDescriptorSize_;
			impl.device_->CreateDepthStencilView( resource.resource_.Get(), nullptr, resource.dsvHandle_ );
		}

		const TextureHandle handle = impl.slotMapTextures_.Create( std::move( resource ) );
		if( desc.data != nullptr && desc.rowPitch > 0 && desc.slicePitch > 0 )
		{
			impl.stagingDevice_->TextureSubData2D( impl.GetTextureResource( handle ), desc.data, desc.rowPitch, desc.slicePitch );
		}

		return handle;
	}

	uint32_t RenderDevice::GetBindlessIndex( BufferHandle buffer ) const
	{
		return manager_->impl_->GetBufferResource( buffer ).srvIndex_;
	}

	uint32_t RenderDevice::GetBindlessIndex( TextureHandle texture ) const
	{
		return manager_->impl_->GetTextureResource( texture ).srvIndex_;
	}

	bool RenderDevice::BindlessSupported() const noexcept
	{
		return manager_->impl_->bindlessSupported_;
	}

	void RenderDevice::WaitIdle()
	{
		manager_->impl_->WaitIdle();
	}

	void RenderDevice::Destroy( BufferHandle buffer )
	{
		auto& impl = *manager_->impl_;
		impl.WaitIdle();
		auto* resource = impl.slotMapBuffers_.Get( buffer );
		if( resource == nullptr )
		{
			return;
		}

		if( resource->mappedPtr_ != nullptr )
		{
			resource->resource_->Unmap( 0, nullptr );
		}
		impl.FreeBindlessDescriptor( resource->srvIndex_ );
		resource->resource_.Reset();
		impl.slotMapBuffers_.Destroy( buffer );
	}

	void RenderDevice::Destroy( TextureHandle texture )
	{
		auto& impl = *manager_->impl_;
		impl.WaitIdle();
		auto* resource = impl.slotMapTextures_.Get( texture );
		if( resource == nullptr )
		{
			return;
		}

		impl.FreeBindlessDescriptor( resource->srvIndex_ );
		impl.FreeRtvDescriptor( resource->rtvIndex_ );
		impl.FreeDsvDescriptor( resource->dsvIndex_ );
		resource->resource_.Reset();
		impl.slotMapTextures_.Destroy( texture );
	}
}


