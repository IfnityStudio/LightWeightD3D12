#include "LightD3D12Swapchain.hpp"

#include "LightD3D12ManagerImpl.hpp"

namespace lightd3d12
{
	Swapchain::Swapchain( DeviceManager::Impl& ctx, HWND hwnd, uint32_t width, uint32_t height ):
		ctx_( ctx ),
		width_( width ),
		height_( height )
	{
		DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
		swapchainDesc.Width = width_;
		swapchainDesc.Height = height_;
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
		detail::ThrowIfFailed(
			ctx_.factory_->CreateSwapChainForHwnd(
				ctx_.commandQueue_.Get(),
				hwnd,
				&swapchainDesc,
				nullptr,
				nullptr,
				tempSwapchain.GetAddressOf() ),
			"Failed to create swapchain." );

		detail::ThrowIfFailed( tempSwapchain.As( &swapchain_ ), "Failed to query IDXGISwapChain4." );
		ctx_.factory_->MakeWindowAssociation( hwnd, DXGI_MWA_NO_ALT_ENTER );

		surfaceFormat_ = swapchainDesc.Format;
		numSwapchainImages_ = swapchainDesc.BufferCount;
		currentBackBufferIndex_ = swapchain_->GetCurrentBackBufferIndex();

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
		detail::ThrowIfFailed( swapchain_->Present( CheckVSyncEnabled() ? 1u : 0u, presentFlags ), "Failed to Present swapchain." );
		currentBackBufferIndex_ = swapchain_->GetCurrentBackBufferIndex();
	}

	void Swapchain::Resize( uint32_t width, uint32_t height )
	{
		if( swapchain_ == nullptr || width == 0 || height == 0 )
		{
			return;
		}

		DestroyBackBuffers();

		width_ = width;
		height_ = height;
		detail::ThrowIfFailed(
			swapchain_->ResizeBuffers(
				ctx_.desc_.swapchainBufferCount,
				width_,
				height_,
				surfaceFormat_,
				ctx_.desc_.allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u ),
			"Failed to Resize swapchain." );

		numSwapchainImages_ = ctx_.desc_.swapchainBufferCount;
		currentBackBufferIndex_ = swapchain_->GetCurrentBackBufferIndex();
		RecreateBackBuffers();
	}

	TextureHandle Swapchain::GetCurrentTexture()
	{
		if( swapchain_ == nullptr || backBufferHandles_.empty() )
		{
			return {};
		}

		currentBackBufferIndex_ = swapchain_->GetCurrentBackBufferIndex();
		if( currentBackBufferIndex_ >= backBufferHandles_.size() )
		{
			return {};
		}

		return backBufferHandles_[ currentBackBufferIndex_ ];
	}

	uint32_t Swapchain::GetCurrentBackBufferIndex() const noexcept
	{
		return currentBackBufferIndex_;
	}

	DXGI_FORMAT Swapchain::GetSurfaceFormat() const noexcept
	{
		return surfaceFormat_;
	}

	IDXGISwapChain4* Swapchain::GetSwapchain() const noexcept
	{
		return swapchain_.Get();
	}

	void Swapchain::DestroyBackBuffers() noexcept
	{
		for( size_t index = 0; index < backBufferHandles_.size(); ++index )
		{
			const TextureHandle handle = backBufferHandles_[ index ];
			auto* texture = ctx_.slotMapTextures_.Get( handle );
			if( texture != nullptr )
			{
				ctx_.FreeRtvDescriptor( texture->rtvIndex_ );
				texture->resource_.Reset();
				ctx_.slotMapTextures_.Destroy( handle );
			}

			if( index < backBuffers_.size() )
			{
				backBuffers_[ index ].Reset();
			}
		}

		backBufferHandles_.clear();
		backBuffers_.clear();
		rtvHandles_.clear();
		currentBackBufferIndex_ = 0;
	}

	void Swapchain::RecreateBackBuffers()
	{
		backBuffers_.resize( numSwapchainImages_ );
		backBufferHandles_.resize( numSwapchainImages_ );
		rtvHandles_.resize( numSwapchainImages_ );

		for( uint32_t index = 0; index < numSwapchainImages_; ++index )
		{
			ComPtr<ID3D12Resource> buffer;
			detail::ThrowIfFailed( swapchain_->GetBuffer( index, IID_PPV_ARGS( buffer.GetAddressOf() ) ), "Failed to get swapchain back buffer." );

			backBuffers_[ index ] = buffer;

			TextureResource texture;
			texture.resource_ = buffer;
			texture.usageFlags_ = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			texture.currentState_ = D3D12_RESOURCE_STATE_PRESENT;
			texture.format_ = surfaceFormat_;
			texture.desc_ = buffer->GetDesc();
			texture.width_ = width_;
			texture.height_ = height_;
			texture.isSwapchainImage_ = true;
			texture.isDepthFormat_ = TextureResource::IsDepthFormat( surfaceFormat_ );
			texture.isStencilFormat_ = TextureResource::IsDepthStencilFormat( surfaceFormat_ );
			texture.rtvIndex_ = ctx_.AllocateRtvDescriptor();
			texture.rtvHandle_ = ctx_.rtvHeap_->GetCPUDescriptorHandleForHeapStart();
			texture.rtvHandle_.ptr += static_cast<SIZE_T>( texture.rtvIndex_ ) * ctx_.rtvDescriptorSize_;

			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = surfaceFormat_;
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
		return ctx_.swapchainDesc_.vsync;
	}
}


