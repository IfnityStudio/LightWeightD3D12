#include "LightD3D12Swapchain.hpp"

#include "LightD3D12ManagerImpl.hpp"

#include <stdexcept>

namespace lightd3d12
{
	Swapchain::Swapchain( DeviceManager::Impl& ctx, SwapchainHandle swapchainHandle, HWND hwnd, uint32_t width, uint32_t height ):
		ctx_( ctx ),
		swapchainHandle_( swapchainHandle )
	{
		if( ctx_.desc_.swapchainBufferCount == 0 || ctx_.desc_.swapchainBufferCount > ourMaxSwapchainBuffers )
		{
			throw std::runtime_error( "Swapchain buffer count exceeds fixed back buffer storage." );
		}

		DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
		swapchainDesc.Width = width;
		swapchainDesc.Height = height;
		swapchainDesc.Format = ctx_.desc_.swapchainFormat;
		swapchainDesc.BufferCount = ctx_.desc_.swapchainBufferCount;
		swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swapchainDesc.SampleDesc.Count = 1;
		swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
		swapchainDesc.Scaling = DXGI_SCALING_NONE;
		swapchainDesc.Stereo = FALSE;
		swapchainDesc.Flags = ctx_.desc_.allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

		ComPtr<IDXGISwapChain1> tempSwapchain;
		C_RESULT(
			ctx_.factory_->CreateSwapChainForHwnd(
				ctx_.GetGraphicsQueueContext().commandQueue_.Get(),
				hwnd,
				&swapchainDesc,
				nullptr,
				nullptr,
				tempSwapchain.GetAddressOf() ),
			"Failed to create swapchain." );

		C_RESULT( tempSwapchain.As( &swapchain_ ), "Failed to query IDXGISwapChain4." );
		ctx_.factory_->MakeWindowAssociation( hwnd, DXGI_MWA_NO_ALT_ENTER );

		properties_.surfaceFormat_ = swapchainDesc.Format;
		properties_.numSwapchainImages_ = swapchainDesc.BufferCount;
		properties_.currentBackBufferIndex_ = swapchain_->GetCurrentBackBufferIndex();
		properties_.width_ = width;
		properties_.height_ = height;

		RecreateBackBuffers();
	}

	Swapchain::~Swapchain()
	{
		DestroyBackBuffers();
		swapchain_.Reset();
	}

	void Swapchain::Present()
	{
		const UINT presentFlags = ( !CheckVSyncEnabled() && ctx_.desc_.allowTearing ) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
		C_RESULT( swapchain_->Present( CheckVSyncEnabled() ? 1u : 0u, presentFlags ), "Failed to Present swapchain." );
		properties_.currentBackBufferIndex_ = swapchain_->GetCurrentBackBufferIndex();
	}

	void Swapchain::Resize( uint32_t width, uint32_t height )
	{
		if( swapchain_ == nullptr || width == 0 || height == 0 )
		{
			return;
		}

		DestroyBackBuffers();

		properties_.width_ = width;
		properties_.height_ = height;
		C_RESULT(
			swapchain_->ResizeBuffers(
				ctx_.desc_.swapchainBufferCount,
				properties_.width_,
				properties_.height_,
				properties_.surfaceFormat_,
				ctx_.desc_.allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u ),
			"Failed to Resize swapchain." );

		properties_.numSwapchainImages_ = ctx_.desc_.swapchainBufferCount;
		properties_.currentBackBufferIndex_ = swapchain_->GetCurrentBackBufferIndex();
		RecreateBackBuffers();
	}

	TextureHandle Swapchain::GetCurrentTexture()
	{
		if( swapchain_ == nullptr || properties_.numSwapchainImages_ == 0 )
		{
			return {};
		}

		properties_.currentBackBufferIndex_ = swapchain_->GetCurrentBackBufferIndex();
		if( properties_.currentBackBufferIndex_ >= properties_.numSwapchainImages_ )
		{
			return {};
		}

		return backBufferHandles_[ properties_.currentBackBufferIndex_ ];
	}

	uint32_t Swapchain::GetCurrentBackBufferIndex() const noexcept
	{
		return properties_.currentBackBufferIndex_;
	}

	DXGI_FORMAT Swapchain::GetSurfaceFormat() const noexcept
	{
		return properties_.surfaceFormat_;
	}

	IDXGISwapChain4* Swapchain::GetSwapchain() const noexcept
	{
		return swapchain_.Get();
	}

	void Swapchain::DestroyBackBuffers() noexcept
	{
		for( uint32_t index = 0; index < properties_.numSwapchainImages_; ++index )
		{
			const TextureHandle handle = backBufferHandles_[ index ];
			auto* texture = ctx_.slotMapTextures_.Get( handle );
			if( texture != nullptr )
			{
				ctx_.FreeRtvDescriptor( texture->rtvIndex_ );
				texture->resource_.Reset();
				ctx_.slotMapTextures_.Destroy( handle );
			}

			backBufferHandles_[ index ] = {};
			rtvHandles_[ index ] = {};
			backBuffers_[ index ].Reset();
		}

		properties_.numSwapchainImages_ = 0;
		properties_.currentBackBufferIndex_ = 0;
	}

	void Swapchain::RecreateBackBuffers()
	{
		if( properties_.numSwapchainImages_ > backBuffers_.size() )
		{
			throw std::runtime_error( "Swapchain image count exceeds fixed back buffer storage." );
		}

		for( uint32_t index = 0; index < properties_.numSwapchainImages_; ++index )
		{
			ComPtr<ID3D12Resource> buffer;
			C_RESULT( swapchain_->GetBuffer( index, IID_PPV_ARGS( buffer.GetAddressOf() ) ), "Failed to get swapchain back buffer." );

			backBuffers_[ index ] = buffer;

			TextureResource texture;
			texture.resource_ = buffer;
			texture.usageFlags_ = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			texture.currentState_ = D3D12_RESOURCE_STATE_PRESENT;
			texture.format_ = properties_.surfaceFormat_;
			texture.formats_.resource_ = properties_.surfaceFormat_;
			texture.formats_.rtv_ = properties_.surfaceFormat_;
			texture.desc_ = buffer->GetDesc();
			texture.width_ = properties_.width_;
			texture.height_ = properties_.height_;
			texture.isSwapchainImage_ = true;
			texture.swapchain_ = swapchainHandle_;
			texture.isDepthFormat_ = TextureResource::IsDepthFormat( properties_.surfaceFormat_ );
			texture.isStencilFormat_ = TextureResource::IsDepthStencilFormat( properties_.surfaceFormat_ );
			texture.rtvIndex_ = ctx_.AllocateRtvDescriptor();
			texture.rtvHandle_ = ctx_.rtvHeap_->GetCPUDescriptorHandleForHeapStart();
			texture.rtvHandle_.ptr += static_cast<SIZE_T>( texture.rtvIndex_ ) * ctx_.rtvDescriptorSize_;

			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = properties_.surfaceFormat_;
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			rtvDesc.Texture2D.MipSlice = 0;
			rtvDesc.Texture2D.PlaneSlice = 0;
			ctx_.device_->CreateRenderTargetView( buffer.Get(), &rtvDesc, texture.rtvHandle_ );

			rtvHandles_[ index ] = texture.rtvHandle_;
			backBufferHandles_[ index ] = ctx_.slotMapTextures_.Create( std::move( texture ) );
		}
	}

	bool Swapchain::CheckVSyncEnabled() const noexcept
	{
		const SwapchainDesc* swapchainDesc = ctx_.GetSwapchainDesc( swapchainHandle_ );
		return swapchainDesc != nullptr ? swapchainDesc->vsync : true;
	}
}


