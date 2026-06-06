#include "LightD3D12ManagerImpl.hpp"

#include "LightD3D12BaseMips.hpp"
#include "LightD3D12ShaderCompiler.hpp"
#include "LightD3D12StagingDevice.hpp"
#include "LightD3D12Swapchain.hpp"

#include <algorithm>
#include <vector>

namespace lightd3d12
{
	namespace
	{
		struct TextureCreationPlan final
		{
			uint16_t mipLevels_ = 1;
			bool generateInitialMipChain_ = false;
			bool requiresTypedUavViews_ = false;
		};

		[[nodiscard]] uint16_t ClampTextureMipCount( uint32_t width, uint32_t height, uint16_t requestedMipCount ) noexcept
		{
			uint16_t maxMipCount = 1;
			while( width > 1 || height > 1 )
			{
				width = std::max( 1u, width >> 1u );
				height = std::max( 1u, height >> 1u );
				++maxMipCount;
			}

			return std::clamp<uint16_t>( requestedMipCount, 1u, maxMipCount );
		}

		[[nodiscard]] bool IsSrgbTextureFormat( DXGI_FORMAT format ) noexcept
		{
			return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		}

		[[nodiscard]] bool SupportsComputeMipGeneration( const TextureDesc& desc, uint16_t mipCount ) noexcept
		{
			return mipCount > 1 &&
				desc.data != nullptr &&
				desc.dimension == TextureDimension::Texture2D &&
				desc.depthOrArraySize == 1 &&
				( desc.format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB );
		}

		TextureCreationPlan BuildTextureCreationPlan( const TextureDesc& desc )
		{
			TextureCreationPlan plan{};
			const uint16_t requestedMipLevels = std::max<uint16_t>( 1u, desc.countMipMap );
			plan.mipLevels_ = ClampTextureMipCount( desc.width, desc.height, requestedMipLevels );
			plan.generateInitialMipChain_ = SupportsComputeMipGeneration( desc, plan.mipLevels_ );
			plan.requiresTypedUavViews_ = HasTextureUsage( desc.usage, TextureUsage::UnorderedAccess ) || plan.generateInitialMipChain_;

			if( requestedMipLevels > 1u && desc.data != nullptr && !plan.generateInitialMipChain_ )
			{
				throw std::runtime_error( "Compute mip generation currently supports only single Texture2D RGBA8 textures." );
			}

			return plan;
		}

		[[nodiscard]] DXGI_FORMAT ResolveTypedUavCompatibleResourceFormat( DXGI_FORMAT format, bool requiresTypedUavViews ) noexcept
		{
			if( requiresTypedUavViews && IsSrgbTextureFormat( format ) )
			{
				return DXGI_FORMAT_R8G8B8A8_TYPELESS;
			}

			return format;
		}

		[[nodiscard]] DXGI_FORMAT ResolveTextureUavFormat( DXGI_FORMAT format ) noexcept
		{
			if( IsSrgbTextureFormat( format ) )
			{
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			}

			return format;
		}

		TextureResource::ResolvedFormats ResolveTextureFormats( const TextureDesc& desc, bool requiresTypedUavViews )
		{
			TextureResource::ResolvedFormats formats{};
			formats.resource_ = desc.format;

			const bool sampled = HasTextureUsage( desc.usage, TextureUsage::Sampled );
			const bool renderTarget = HasTextureUsage( desc.usage, TextureUsage::RenderTarget );
			const bool depthStencil = HasTextureUsage( desc.usage, TextureUsage::DepthStencil );
			const bool unorderedAccess = HasTextureUsage( desc.usage, TextureUsage::UnorderedAccess ) || requiresTypedUavViews;

			if( depthStencil )
			{
				switch( desc.format )
				{
					case DXGI_FORMAT_D16_UNORM:
						formats.resource_ = DXGI_FORMAT_R16_TYPELESS;
						formats.dsv_ = DXGI_FORMAT_D16_UNORM;
						formats.srv_ = sampled ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_UNKNOWN;
						break;

					case DXGI_FORMAT_D24_UNORM_S8_UINT:
						formats.resource_ = DXGI_FORMAT_R24G8_TYPELESS;
						formats.dsv_ = DXGI_FORMAT_D24_UNORM_S8_UINT;
						formats.srv_ = sampled ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_UNKNOWN;
						break;

					case DXGI_FORMAT_D32_FLOAT:
						formats.resource_ = DXGI_FORMAT_R32_TYPELESS;
						formats.dsv_ = DXGI_FORMAT_D32_FLOAT;
						formats.srv_ = sampled ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN;
						break;

					case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
						formats.resource_ = DXGI_FORMAT_R32G8X24_TYPELESS;
						formats.dsv_ = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
						formats.srv_ = sampled ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_UNKNOWN;
						break;

					default:
						throw std::runtime_error( "Unsupported depth texture format." );
				}

				return formats;
			}

			if( unorderedAccess )
			{
				formats.resource_ = ResolveTypedUavCompatibleResourceFormat( desc.format, requiresTypedUavViews );
			}

			formats.srv_ = sampled ? desc.format : DXGI_FORMAT_UNKNOWN;
			formats.rtv_ = renderTarget ? desc.format : DXGI_FORMAT_UNKNOWN;
			formats.uav_ = unorderedAccess ? ResolveTextureUavFormat( desc.format ) : DXGI_FORMAT_UNKNOWN;
			return formats;
		}

		D3D12_RESOURCE_FLAGS ResolveTextureResourceFlags( TextureUsage usage, bool requiresTypedUavViews ) noexcept
		{
			D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
			if( HasTextureUsage( usage, TextureUsage::RenderTarget ) )
			{
				flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			}

			if( HasTextureUsage( usage, TextureUsage::DepthStencil ) )
			{
				flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			}

			if( HasTextureUsage( usage, TextureUsage::UnorderedAccess ) || requiresTypedUavViews )
			{
				flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			}

			return flags;
		}
	}

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

	ComputePipelineState::ComputePipelineState( ComputePipelineState&& other ) noexcept
	{
		*this = std::move( other );
	}

	ComputePipelineState& ComputePipelineState::operator=( ComputePipelineState&& other ) noexcept
	{
		if( this != &other )
		{
			pipelineState_ = std::move( other.pipelineState_ );
		}
		return *this;
	}

	bool ComputePipelineState::Valid() const noexcept
	{
		return pipelineState_ != nullptr;
	}

	RenderDevice::RenderDevice( DeviceManager& manager ) noexcept: manager_( &manager )
	{
	}

	ICommandBuffer& RenderDevice::AcquireCommandBuffer()
	{
		auto& impl = *manager_->impl_;
		std::unique_ptr<CommandBufferImpl>* availableSlot = nullptr;
		for(std::unique_ptr<CommandBufferImpl>& activeCommandBuffer : impl.activeCommandBuffers_ )
		{
			if( activeCommandBuffer == nullptr )
			{
				availableSlot = &activeCommandBuffer;
				break;
			}
		}

		if( availableSlot == nullptr )
		{
			throw std::runtime_error( "A maximum of four active command buffers are allowed per render device." );
		}

		impl.ProcessDeferredReleases();
		auto& wrapper = impl.immediateCommands_->Acquire();
		*availableSlot = std::make_unique<CommandBufferImpl>( impl, wrapper );
		return **availableSlot;
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
		DeviceManager::Impl& impl = *manager_->impl_;
		CommandBufferImpl* commandBuffer = dynamic_cast<CommandBufferImpl*>( &buffer );
		if( commandBuffer == nullptr )
		{
			throw std::runtime_error( "The command buffer does not belong to this render device." );
		}

		std::unique_ptr<CommandBufferImpl>* activeSlot = nullptr;
		for( auto& activeCommandBuffer : impl.activeCommandBuffers_ )
		{
			if( activeCommandBuffer.get() == commandBuffer )
			{
				activeSlot = &activeCommandBuffer;
				break;
			}
		}

		if( activeSlot == nullptr )
		{
			throw std::runtime_error( "The command buffer does not belong to this render device." );
		}

		if( commandBuffer->IsRendering() )
		{
			throw std::runtime_error( "Cannot submit a command buffer while a render pass is still active." );
		}

		if( presentTexture.Valid() )
		{
			commandBuffer->CmdTransitionTexture( presentTexture, D3D12_RESOURCE_STATE_PRESENT );
		}

		auto submitFixup = commandBuffer->BuildSubmitFixup();
		const SubmitHandle handle = impl.immediateCommands_->Submit( commandBuffer->Wrapper(), submitFixup.commandList_.Get() );
		commandBuffer->CommitSubmittedTextureStates();
		if( submitFixup.Valid() )
		{
			impl.AddDeferredRelease(
				handle,
				[allocator = std::move( submitFixup.allocator_ ), commandList = std::move( submitFixup.commandList_ )]() mutable
				{
					commandList.Reset();
					allocator.Reset();
				} );
		}

		activeSlot->reset();

		if( presentTexture.Valid() )
		{
			if( impl.swapchain_ == nullptr )
			{
				throw std::runtime_error( "Swapchain is not initialized." );
			}

			impl.swapchain_->Present();
		}
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

		const CompiledShader vertexShader = CompileShader( desc.vertexShader, "vs_6_6" );
		const CompiledShader fragmentShader = CompileShader( desc.fragmentShader, "ps_6_6" );

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = impl.rootSignature_.Get();
		psoDesc.VS = vertexShader.Bytecode();
		psoDesc.PS = fragmentShader.Bytecode();
		psoDesc.BlendState = desc.blendState;
		psoDesc.RasterizerState = desc.rasterizerState;
		psoDesc.DepthStencilState = desc.depthStencilState;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = desc.primitiveType;

		std::vector<D3D12_INPUT_ELEMENT_DESC> nativeInputElements;
		nativeInputElements.reserve( desc.inputElements.size() );
		for( const VertexInputElementDesc& inputElement : desc.inputElements )
		{
			if( inputElement.semanticName.empty() )
			{
				throw std::runtime_error( "RenderPipelineDesc input elements require a semantic name." );
			}

			D3D12_INPUT_ELEMENT_DESC nativeInputElement{};
			nativeInputElement.SemanticName = inputElement.semanticName.c_str();
			nativeInputElement.SemanticIndex = inputElement.semanticIndex;
			nativeInputElement.Format = inputElement.format;
			nativeInputElement.InputSlot = inputElement.inputSlot;
			nativeInputElement.AlignedByteOffset = inputElement.alignedByteOffset;
			nativeInputElement.InputSlotClass = inputElement.inputClassification;
			nativeInputElement.InstanceDataStepRate = inputElement.instanceDataStepRate;
			nativeInputElements.push_back( nativeInputElement );
		}
		psoDesc.InputLayout.pInputElementDescs = nativeInputElements.data();
		psoDesc.InputLayout.NumElements = static_cast<UINT>( nativeInputElements.size() );

		uint32_t numRenderTargets = 0;
		for( uint32_t index = 0; index < desc.color.size(); ++index )
		{
			if( desc.color[ index ].format == DXGI_FORMAT_UNKNOWN )
			{
				continue;
			}

			psoDesc.RTVFormats[ numRenderTargets ] = desc.color[ index ].format;
			numRenderTargets++;
		}

		if( numRenderTargets == 0 && desc.colorFormat != DXGI_FORMAT_UNKNOWN )
		{
			psoDesc.RTVFormats[ 0 ] = desc.colorFormat;
			numRenderTargets = 1;
		}

		psoDesc.NumRenderTargets = numRenderTargets;
		psoDesc.DSVFormat = desc.depthFormat;
		psoDesc.SampleDesc = { 1, 0 };

		RenderPipelineState pipeline;
		detail::ThrowIfFailed( impl.device_->CreateGraphicsPipelineState( &psoDesc, IID_PPV_ARGS( pipeline.pipelineState_.GetAddressOf() ) ), "Failed to create graphics pipeline state." );
		pipeline.topology_ = desc.topology;
		return pipeline;
	}

	ComputePipelineState RenderDevice::CreateComputePipeline( const ComputePipelineDesc& desc )
	{
		auto& impl = *manager_->impl_;
		if( desc.computeShader.source == nullptr )
		{
			throw std::runtime_error( "ComputePipelineDesc requires a valid compute shader source." );
		}

		const CompiledShader computeShader = CompileShader( desc.computeShader, "cs_6_6" );

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = impl.rootSignature_.Get();
		psoDesc.CS = computeShader.Bytecode();

		ComputePipelineState pipeline;
		detail::ThrowIfFailed( impl.device_->CreateComputePipelineState( &psoDesc, IID_PPV_ARGS( pipeline.pipelineState_.GetAddressOf() ) ), "Failed to create compute pipeline state." );
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
		resource.bufferType_ = desc.bufferType;
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
		if( HasTextureUsage( desc.usage, TextureUsage::RenderTarget ) && HasTextureUsage( desc.usage, TextureUsage::DepthStencil ) )
		{
			throw std::runtime_error( "A texture cannot be both RenderTarget and DepthStencil." );
		}

		if( HasTextureUsage( desc.usage, TextureUsage::DepthStencil ) && HasTextureUsage( desc.usage, TextureUsage::UnorderedAccess ) )
		{
			throw std::runtime_error( "DepthStencil textures cannot expose UAVs." );
		}

		const TextureCreationPlan creationPlan = BuildTextureCreationPlan( desc );

		if( desc.dimension == TextureDimension::Texture3D )
		{
			if( desc.depthOrArraySize <= 1 )
			{
				throw std::runtime_error( "Texture3D resources require depthOrArraySize > 1." );
			}

			if( HasTextureUsage( desc.usage, TextureUsage::RenderTarget ) || HasTextureUsage( desc.usage, TextureUsage::DepthStencil ) )
			{
				throw std::runtime_error( "This lightweight API currently supports Texture3D only for SRV/UAV usage." );
			}

			if( desc.data != nullptr )
			{
				throw std::runtime_error( "Texture3D CPU uploads are not implemented yet. Populate them with compute or add a staging path." );
			}
		}

		TextureResource resource;
		resource.width_ = desc.width;
		resource.height_ = desc.height;
		resource.mipLevels_ = creationPlan.mipLevels_;
		resource.depthOrArraySize_ = desc.depthOrArraySize;
		resource.dimension_ = desc.dimension;
		resource.format_ = desc.format;
		resource.formats_ = ResolveTextureFormats( desc, creationPlan.requiresTypedUavViews_ );
		resource.usageFlags_ = ResolveTextureResourceFlags( desc.usage, creationPlan.requiresTypedUavViews_ );
		resource.currentState_ = desc.initialState;
		resource.isDepthFormat_ = TextureResource::IsDepthFormat( resource.formats_.dsv_ );
		resource.isStencilFormat_ = TextureResource::IsDepthStencilFormat( resource.formats_.dsv_ );

		if( creationPlan.requiresTypedUavViews_ && resource.formats_.uav_ == DXGI_FORMAT_UNKNOWN )
		{
			throw std::runtime_error( "This texture format cannot expose the typed UAVs required by the requested usage." );
		}

		if( desc.dimension == TextureDimension::Texture3D )
		{
			resource.desc_ = CD3DX12_RESOURCE_DESC::Tex3D(
				resource.formats_.resource_,
				desc.width,
				desc.height,
				desc.depthOrArraySize,
				resource.mipLevels_,
				resource.usageFlags_ );
		}
		else
		{
			resource.desc_ = CD3DX12_RESOURCE_DESC::Tex2D(
				resource.formats_.resource_,
				desc.width,
				desc.height,
				desc.depthOrArraySize,
				resource.mipLevels_,
				1,
				0,
				resource.usageFlags_ );
		}

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

		const auto makeBindlessCpuHandle = [ &impl ]( uint32_t index )
		{
			D3D12_CPU_DESCRIPTOR_HANDLE handle = impl.bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<SIZE_T>( index ) * impl.bindlessDescriptorSize_;
			return handle;
		};

		if( HasTextureUsage( desc.usage, TextureUsage::Sampled ) )
		{
			resource.srvIndex_ = impl.AllocateBindlessDescriptor();
			resource.srvHandle_ = impl.bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
			resource.srvHandle_.ptr += static_cast<SIZE_T>( resource.srvIndex_ ) * impl.bindlessDescriptorSize_;

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Format = resource.formats_.srv_;
			if( desc.dimension == TextureDimension::Texture3D )
			{
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
				srvDesc.Texture3D.MipLevels = resource.mipLevels_;
			}
			else
			{
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = resource.mipLevels_;
			}
			impl.device_->CreateShaderResourceView( resource.resource_.Get(), &srvDesc, resource.srvHandle_ );
		}

		if( creationPlan.generateInitialMipChain_ )
		{
			resource.baseMipsUavCount_ = static_cast<uint16_t>( resource.mipLevels_ - 1u );
			resource.baseMipsUavBaseIndex_ = impl.AllocateBindlessDescriptorRange( resource.baseMipsUavCount_ );
			for( uint16_t mipLevel = 1; mipLevel < resource.mipLevels_; ++mipLevel )
			{
				D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				uavDesc.Format = resource.formats_.uav_;
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				uavDesc.Texture2D.MipSlice = mipLevel;

				impl.device_->CreateUnorderedAccessView(
					resource.resource_.Get(),
					nullptr,
					&uavDesc,
					makeBindlessCpuHandle( resource.baseMipsUavBaseIndex_ + static_cast<uint32_t>( mipLevel - 1u ) ) );
			}
		}

		if( HasTextureUsage( desc.usage, TextureUsage::UnorderedAccess ) )
		{
			resource.uavIndex_ = impl.AllocateBindlessDescriptor();
			resource.uavHandle_ = makeBindlessCpuHandle( resource.uavIndex_ );

			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = resource.formats_.uav_;
			uavDesc.ViewDimension = desc.dimension == TextureDimension::Texture3D ? D3D12_UAV_DIMENSION_TEXTURE3D : D3D12_UAV_DIMENSION_TEXTURE2D;
			if( desc.dimension == TextureDimension::Texture3D )
			{
				uavDesc.Texture3D.WSize = desc.depthOrArraySize;
			}
			impl.device_->CreateUnorderedAccessView( resource.resource_.Get(), nullptr, &uavDesc, resource.uavHandle_ );
		}

		if( HasTextureUsage( desc.usage, TextureUsage::RenderTarget ) )
		{
			resource.rtvIndex_ = impl.AllocateRtvDescriptor();
			resource.rtvHandle_ = impl.rtvHeap_->GetCPUDescriptorHandleForHeapStart();
			resource.rtvHandle_.ptr += static_cast<SIZE_T>( resource.rtvIndex_ ) * impl.rtvDescriptorSize_;
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = resource.formats_.rtv_;
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			impl.device_->CreateRenderTargetView( resource.resource_.Get(), &rtvDesc, resource.rtvHandle_ );
		}

		if( HasTextureUsage( desc.usage, TextureUsage::DepthStencil ) )
		{
			resource.dsvIndex_ = impl.AllocateDsvDescriptor();
			resource.dsvHandle_ = impl.dsvHeap_->GetCPUDescriptorHandleForHeapStart();
			resource.dsvHandle_.ptr += static_cast<SIZE_T>( resource.dsvIndex_ ) * impl.dsvDescriptorSize_;
			D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = resource.formats_.dsv_;
			dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			impl.device_->CreateDepthStencilView( resource.resource_.Get(), &dsvDesc, resource.dsvHandle_ );
		}

		const TextureHandle handle = impl.slotMapTextures_.Create( std::move( resource ) );
		if( desc.data != nullptr && desc.rowPitch > 0 && desc.slicePitch > 0 )
		{
			TextureResource& textureResource = impl.GetTextureResource( handle );
			impl.stagingDevice_->TextureSubData2D( textureResource, desc.data, desc.rowPitch, desc.slicePitch );
			if( creationPlan.generateInitialMipChain_ )
			{
				impl.baseMips_->Generate( textureResource, desc.initialState );
			}
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

	uint32_t RenderDevice::GetUnorderedAccessIndex( TextureHandle texture ) const
	{
		return manager_->impl_->GetTextureResource( texture ).uavIndex_;
	}

	ID3D12Device* RenderDevice::GetNativeDevice() const noexcept
	{
		return manager_->impl_->device_.Get();
	}

	ID3D12Resource* RenderDevice::GetNativeTextureResource( TextureHandle texture ) const
	{
		return manager_->impl_->GetTextureResource( texture ).resource_.Get();
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
		impl.FreeBindlessDescriptor( resource->uavIndex_ );
		impl.FreeBindlessDescriptorRange( resource->baseMipsUavBaseIndex_, resource->baseMipsUavCount_ );
		impl.FreeRtvDescriptor( resource->rtvIndex_ );
		impl.FreeDsvDescriptor( resource->dsvIndex_ );
		resource->resource_.Reset();
		impl.slotMapTextures_.Destroy( texture );
	}
}


