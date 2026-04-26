#include "LightD3D12CommandBuffer.hpp"

#include "LightD3D12ManagerImpl.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#if defined( LIGHTD3D12_ENABLE_PIX )
	#define LIGHTD3D12_INTERNAL_PIX_ENABLED 1
#else
	#define LIGHTD3D12_INTERNAL_PIX_ENABLED 1
#endif

namespace lightd3d12
{
	namespace
	{
#if LIGHTD3D12_INTERNAL_PIX_ENABLED
		std::vector<uint32_t> ParseVersionComponents( const std::wstring& versionText )
		{
			std::vector<uint32_t> components;
			size_t start = 0;
			while( start < versionText.size() )
			{
				const size_t end = versionText.find( L'.', start );
				const std::wstring token = versionText.substr( start, end == std::wstring::npos ? std::wstring::npos : end - start );
				if( token.empty() )
				{
					components.push_back( 0 );
				}
				else
				{
					components.push_back( static_cast<uint32_t>( std::wcstoul( token.c_str(), nullptr, 10 ) ) );
				}

				if( end == std::wstring::npos )
				{
					break;
				}

				start = end + 1;
			}

			return components;
		}

		bool IsVersionGreater( const std::vector<uint32_t>& left, const std::vector<uint32_t>& right )
		{
			const size_t count = std::max( left.size(), right.size() );
			for( size_t index = 0; index < count; ++index )
			{
				const uint32_t leftValue = index < left.size() ? left[ index ] : 0u;
				const uint32_t rightValue = index < right.size() ? right[ index ] : 0u;
				if( leftValue != rightValue )
				{
					return leftValue > rightValue;
				}
			}

			return false;
		}

		std::filesystem::path FindPixEventRuntimePath()
		{
			// First allow a local copy next to the executable or on PATH.
			if( ::GetModuleHandleW( L"WinPixEventRuntime.dll" ) != nullptr )
			{
				return L"WinPixEventRuntime.dll";
			}

			// Then try the standard NuGet cache location, which is where the PIX event runtime is commonly installed.
			wchar_t* userProfileValue = nullptr;
			size_t userProfileLength = 0;
			if( _wdupenv_s( &userProfileValue, &userProfileLength, L"USERPROFILE" ) == 0 &&
				userProfileValue != nullptr &&
				userProfileValue[ 0 ] != L'\0' )
			{
				const std::filesystem::path runtimeRoot =
					std::filesystem::path( userProfileValue ) / ".nuget" / "packages" / "winpixeventruntime";
				free( userProfileValue );
				if( std::filesystem::exists( runtimeRoot ) )
				{
					std::filesystem::path latestRuntimePath;
					std::vector<uint32_t> latestVersion;
					for( const auto& entry : std::filesystem::directory_iterator( runtimeRoot ) )
					{
						if( !entry.is_directory() )
						{
							continue;
						}

						const std::filesystem::path candidateDll = entry.path() / "bin" / "x64" / "WinPixEventRuntime.dll";
						if( !std::filesystem::exists( candidateDll ) )
						{
							continue;
						}

						const std::vector<uint32_t> candidateVersion = ParseVersionComponents( entry.path().filename().wstring() );
						if( latestRuntimePath.empty() || IsVersionGreater( candidateVersion, latestVersion ) )
						{
							latestRuntimePath = candidateDll;
							latestVersion = candidateVersion;
						}
					}

					if( !latestRuntimePath.empty() )
					{
						return latestRuntimePath;
					}
				}
			}
			else
			{
				free( userProfileValue );
			}

			return {};
		}

		struct PixRuntime final
		{
			using BeginEventOnCommandListFn = void( WINAPI* )( ID3D12GraphicsCommandList*, UINT64, PCSTR );
			using EndEventOnCommandListFn = void( WINAPI* )( ID3D12GraphicsCommandList* );

			~PixRuntime()
			{
				if( module_ != nullptr )
				{
					::FreeLibrary( module_ );
				}
			}

			bool Available()
			{
				std::call_once( initFlag_, [this]()
					{
						const std::filesystem::path runtimePath = FindPixEventRuntimePath();
						if( runtimePath.empty() )
						{
							return;
						}

						module_ = ::LoadLibraryW( runtimePath.c_str() );
						if( module_ == nullptr )
						{
							return;
						}

						beginEventOnCommandList_ = reinterpret_cast<BeginEventOnCommandListFn>( ::GetProcAddress( module_, "PIXBeginEventOnCommandList" ) );
						endEventOnCommandList_ = reinterpret_cast<EndEventOnCommandListFn>( ::GetProcAddress( module_, "PIXEndEventOnCommandList" ) );
						if( beginEventOnCommandList_ == nullptr || endEventOnCommandList_ == nullptr )
						{
							::FreeLibrary( module_ );
							module_ = nullptr;
							beginEventOnCommandList_ = nullptr;
							endEventOnCommandList_ = nullptr;
							return;
						}

						available_ = true;
					} );

				return available_;
			}

			void BeginEvent( ID3D12GraphicsCommandList* commandList, uint64_t color, const char* label )
			{
				if( Available() )
				{
					beginEventOnCommandList_( commandList, color, label );
				}
			}

			void EndEvent( ID3D12GraphicsCommandList* commandList )
			{
				if( Available() )
				{
					endEventOnCommandList_( commandList );
				}
			}

		private:
			std::once_flag initFlag_;
			HMODULE module_ = nullptr;
			BeginEventOnCommandListFn beginEventOnCommandList_ = nullptr;
			EndEventOnCommandListFn endEventOnCommandList_ = nullptr;
			bool available_ = false;
		};

		PixRuntime& GetPixRuntime()
		{
			static PixRuntime runtime;
			return runtime;
		}
#endif

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

	CommandBufferImpl::TrackedTextureState& CommandBufferImpl::GetTrackedTextureState( TextureHandle texture )
	{
		for( auto& trackedTexture : trackedTextures_ )
		{
			if( trackedTexture.handle_ == texture )
			{
				return trackedTexture;
			}
		}

		const TextureResource& resource = manager_.GetTextureResource( texture );
		TrackedTextureState trackedTexture;
		trackedTexture.handle_ = texture;
		trackedTexture.initialState_ = resource.currentState_;
		trackedTexture.currentState_ = resource.currentState_;
		trackedTextures_.push_back( trackedTexture );
		return trackedTextures_.back();
	}

	void CommandBufferImpl::TransitionTexture( TextureHandle texture, TextureResource& resource, D3D12_RESOURCE_STATES newState )
	{
		auto& trackedTexture = GetTrackedTextureState( texture );
		if( trackedTexture.currentState_ == newState )
		{
			return;
		}

		const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			resource.resource_.Get(),
			trackedTexture.currentState_,
			newState );
		wrapper_.commandList_->ResourceBarrier( 1, &barrier );
		trackedTexture.currentState_ = newState;
	}

	CommandBufferImpl::SubmitFixupResources CommandBufferImpl::BuildSubmitFixup()
	{
		bool requiresFixup = false;
		for( const auto& trackedTexture : trackedTextures_ )
		{
			const TextureResource& resource = manager_.GetTextureResource( trackedTexture.handle_ );
			if( resource.currentState_ != trackedTexture.initialState_ )
			{
				requiresFixup = true;
				break;
			}
		}

		if( !requiresFixup )
		{
			return {};
		}

		SubmitFixupResources fixup;
		detail::ThrowIfFailed(
			manager_.device_->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS( fixup.allocator_.GetAddressOf() ) ),
			"Failed to create command allocator for submit fixup." );
		detail::ThrowIfFailed(
			manager_.device_->CreateCommandList(
				0,
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				fixup.allocator_.Get(),
				nullptr,
				IID_PPV_ARGS( fixup.commandList_.GetAddressOf() ) ),
			"Failed to create command list for submit fixup." );

		for( const auto& trackedTexture : trackedTextures_ )
		{
			const TextureResource& resource = manager_.GetTextureResource( trackedTexture.handle_ );
			if( resource.currentState_ == trackedTexture.initialState_ )
			{
				continue;
			}

			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				resource.resource_.Get(),
				resource.currentState_,
				trackedTexture.initialState_ );
			fixup.commandList_->ResourceBarrier( 1, &barrier );
		}

		detail::ThrowIfFailed( fixup.commandList_->Close(), "Failed to close submit fixup command list." );
		return fixup;
	}

	void CommandBufferImpl::CommitSubmittedTextureStates()
	{
		for( const auto& trackedTexture : trackedTextures_ )
		{
			TextureResource& resource = manager_.GetTextureResource( trackedTexture.handle_ );
			resource.currentState_ = trackedTexture.currentState_;
		}
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

			TransitionTexture( framebuffer.color[ index ].texture, colorTexture, D3D12_RESOURCE_STATE_RENDER_TARGET );

			renderTargetDescs[ numRenderTargets ].cpuDescriptor = colorTexture.rtvHandle_;
			renderTargetDescs[ numRenderTargets ].BeginningAccess = CreateBeginningAccess(
				renderPass.color[ index ].loadOp,
				colorTexture.formats_.rtv_ != DXGI_FORMAT_UNKNOWN ? colorTexture.formats_.rtv_ : colorTexture.format_,
				renderPass.color[ index ].clearColor );
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

			TransitionTexture( framebuffer.depthStencil.texture, depthTexture, D3D12_RESOURCE_STATE_DEPTH_WRITE );

			depthStencilDesc.cpuDescriptor = depthTexture.dsvHandle_;
			depthStencilDesc.DepthBeginningAccess = depthTexture.isDepthFormat_
				? CreateDepthBeginningAccess(
					renderPass.depthStencil.depthLoadOp,
					depthTexture.formats_.dsv_ != DXGI_FORMAT_UNKNOWN ? depthTexture.formats_.dsv_ : depthTexture.format_,
					renderPass.depthStencil.clearDepth )
				: CreateNoAccessBeginningAccess();
			depthStencilDesc.DepthEndingAccess = depthTexture.isDepthFormat_
				? CreateEndingAccess( renderPass.depthStencil.depthStoreOp )
				: CreateNoAccessEndingAccess();
			depthStencilDesc.StencilBeginningAccess = depthTexture.isStencilFormat_
				? CreateStencilBeginningAccess(
					renderPass.depthStencil.stencilLoadOp,
					depthTexture.formats_.dsv_ != DXGI_FORMAT_UNKNOWN ? depthTexture.formats_.dsv_ : depthTexture.format_,
					renderPass.depthStencil.clearStencil )
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
		TransitionTexture( texture, resource, newState );
	}

	void CommandBufferImpl::CmdBindRenderPipeline( const RenderPipelineState& pipeline )
	{
		ID3D12DescriptorHeap* heaps[] = { manager_.bindlessHeap_.Get() };
		wrapper_.commandList_->SetDescriptorHeaps( 1, heaps );
		wrapper_.commandList_->SetGraphicsRootSignature( manager_.rootSignature_.Get() );
		wrapper_.commandList_->SetPipelineState( pipeline.pipelineState_.Get() );
		wrapper_.commandList_->IASetPrimitiveTopology( pipeline.topology_ );
	}

	void CommandBufferImpl::CmdBindComputePipeline( const ComputePipelineState& pipeline )
	{
		ID3D12DescriptorHeap* heaps[] = { manager_.bindlessHeap_.Get() };
		wrapper_.commandList_->SetDescriptorHeaps( 1, heaps );
		wrapper_.commandList_->SetComputeRootSignature( manager_.rootSignature_.Get() );
		wrapper_.commandList_->SetPipelineState( pipeline.pipelineState_.Get() );
	}

	void CommandBufferImpl::CmdBindVertexBuffer( BufferHandle buffer, uint32_t stride, uint32_t offset, uint32_t slot )
	{
		const auto& resource = manager_.GetBufferResource( buffer );
		if( resource.bufferType_ != BufferDesc::BufferType::VertexBuffer )
		{
			throw std::runtime_error( "This buffer was not created as a vertex buffer." );
		}
		auto view = resource.GetVertexBufferView( stride );
		if( offset > view.SizeInBytes )
		{
			throw std::runtime_error( "Vertex buffer offset exceeds the buffer size." );
		}
		view.BufferLocation += offset;
		view.SizeInBytes -= offset;
		wrapper_.commandList_->IASetVertexBuffers( slot, 1, &view );
	}

	void CommandBufferImpl::CmdBindIndexBuffer( BufferHandle buffer, DXGI_FORMAT format, uint32_t offset )
	{
		const auto& resource = manager_.GetBufferResource( buffer );
		if( resource.bufferType_ != BufferDesc::BufferType::IndexBuffer )
		{
			throw std::runtime_error( "This buffer was not created as an index buffer." );
		}
		auto view = resource.GetIndexBufferView( format );
		if( offset > view.SizeInBytes )
		{
			throw std::runtime_error( "Index buffer offset exceeds the buffer size." );
		}
		view.BufferLocation += offset;
		view.SizeInBytes -= offset;
		wrapper_.commandList_->IASetIndexBuffer( &view );
	}

	void CommandBufferImpl::CmdPushConstants( const void* data, uint32_t sizeBytes, uint32_t offset32BitValues )
	{
		wrapper_.commandList_->SetGraphicsRootSignature( manager_.rootSignature_.Get() );
		wrapper_.commandList_->SetComputeRootSignature( manager_.rootSignature_.Get() );
		wrapper_.commandList_->SetGraphicsRoot32BitConstants( 0, sizeBytes / 4u, data, offset32BitValues );
		wrapper_.commandList_->SetComputeRoot32BitConstants( 0, sizeBytes / 4u, data, offset32BitValues );
	}

	void CommandBufferImpl::CmdPushDebugGroupLabel( const char* label, uint32_t color )
	{
		if( label != nullptr && label[ 0 ] != '\0' )
		{
			#if LIGHTD3D12_INTERNAL_PIX_ENABLED
				GetPixRuntime().BeginEvent( wrapper_.commandList_.Get(), static_cast<uint64_t>( color ), label );
			#else
				(void)color;
			#endif
			debugGroupDepth_++;
		}
	}

	void CommandBufferImpl::CmdPopDebugGroupLabel()
	{
		if( debugGroupDepth_ > 0 )
		{
			#if LIGHTD3D12_INTERNAL_PIX_ENABLED
				GetPixRuntime().EndEvent( wrapper_.commandList_.Get() );
			#endif
			debugGroupDepth_--;
		}
	}

	void CommandBufferImpl::CmdDraw( uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance )
	{
		wrapper_.commandList_->DrawInstanced( vertexCount, instanceCount, firstVertex, firstInstance );
	}

	void CommandBufferImpl::CmdDrawIndexed( uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance )
	{
		wrapper_.commandList_->DrawIndexedInstanced( indexCount, instanceCount, firstIndex, vertexOffset, firstInstance );
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

	void CommandBufferImpl::CmdDispatch( uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ )
	{
		wrapper_.commandList_->Dispatch( groupCountX, groupCountY, groupCountZ );
	}

	ID3D12GraphicsCommandList* CommandBufferImpl::GetNativeGraphicsCommandList()
	{
		return wrapper_.commandList_.Get();
	}
}


