#include "LightD3D12ManagerImpl.hpp"

#include "LightD3D12StagingDevice.hpp"
#include "LightD3D12Swapchain.hpp"

#include <algorithm>

namespace lightd3d12
{
	DeviceManager::Impl::Impl( const ContextDesc& desc, const SwapchainDesc& swapchainDesc ):
		desc_( desc ),
		swapchainDesc_( swapchainDesc )
	{
	}

	DeviceManager::Impl::~Impl()
	{
		Shutdown();
	}

	void DeviceManager::Impl::Initialize()
	{
		InitializeFactory();
		InitializeDevice();
		InitializeCommandQueue();
		detail::ThrowIfFailed( device_->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( queueIdleFence_.GetAddressOf() ) ), "Failed to create queue idle fence." );
		queueIdleEvent_ = CreateEvent( nullptr, FALSE, FALSE, nullptr );
		if( queueIdleEvent_ == nullptr )
		{
			throw std::runtime_error( "Failed to create queue idle event." );
		}
		InitializeDescriptorHeaps();
		InitializeRootSignature();
		InitializeCommandSignature();
		immediateCommands_ = std::make_unique<ImmediateCommands>( device_.Get(), commandQueue_.Get(), std::max<uint32_t>( 1u, desc_.framesInFlight ) );
		stagingDevice_ = std::make_unique<StagingDevice>( *this );
		CreateSwapchain();
	}

	void DeviceManager::Impl::InitializeFactory()
	{
		UINT flags = 0;
#if defined( _DEBUG )
		if( desc_.enableDebugLayer )
		{
			ComPtr<ID3D12Debug> debugController;
			if( SUCCEEDED( D3D12GetDebugInterface( IID_PPV_ARGS( debugController.GetAddressOf() ) ) ) )
			{
				debugController->EnableDebugLayer();
				flags |= DXGI_CREATE_FACTORY_DEBUG;
			}
		}
#endif
		detail::ThrowIfFailed( CreateDXGIFactory2( flags, IID_PPV_ARGS( factory_.GetAddressOf() ) ), "Failed to create DXGI factory." );
	}

	void DeviceManager::Impl::InitializeDevice()
	{
		auto tryAdapter = [ this ]( IDXGIAdapter1* candidate )->bool
			{
				return SUCCEEDED( D3D12CreateDevice( candidate, desc_.minimumFeatureLevel, IID_PPV_ARGS( device_.GetAddressOf() ) ) );
			};

		if( desc_.preferHighPerformanceAdapter )
		{
			for( UINT adapterIndex = 0;; ++adapterIndex )
			{
				ComPtr<IDXGIAdapter1> candidate;
				if( factory_->EnumAdapterByGpuPreference( adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS( candidate.GetAddressOf() ) ) == DXGI_ERROR_NOT_FOUND )
				{
					break;
				}

				DXGI_ADAPTER_DESC1 adapterDesc{};
				candidate->GetDesc1( &adapterDesc );
				if( ( adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) != 0 )
				{
					continue;
				}

				if( tryAdapter( candidate.Get() ) )
				{
					adapter_ = candidate;
					break;
				}
			}
		}

		if( device_ == nullptr )
		{
			for( UINT adapterIndex = 0;; ++adapterIndex )
			{
				ComPtr<IDXGIAdapter1> candidate;
				if( factory_->EnumAdapters1( adapterIndex, candidate.GetAddressOf() ) == DXGI_ERROR_NOT_FOUND )
				{
					break;
				}

				DXGI_ADAPTER_DESC1 adapterDesc{};
				candidate->GetDesc1( &adapterDesc );
				if( ( adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) != 0 )
				{
					continue;
				}

				if( tryAdapter( candidate.Get() ) )
				{
					adapter_ = candidate;
					break;
				}
			}
		}

		if( device_ == nullptr )
		{
			ComPtr<IDXGIAdapter> warpAdapter;
			detail::ThrowIfFailed( factory_->EnumWarpAdapter( IID_PPV_ARGS( warpAdapter.GetAddressOf() ) ), "Failed to enumerate WARP adapter." );
			detail::ThrowIfFailed( D3D12CreateDevice( warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS( device_.GetAddressOf() ) ), "Failed to create D3D12 device." );
		}

		D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
		if( SUCCEEDED( device_->CheckFeatureSupport( D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof( options ) ) ) )
		{
			bindlessSupported_ = options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2;
		}
	}

	void DeviceManager::Impl::InitializeCommandQueue()
	{
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		detail::ThrowIfFailed( device_->CreateCommandQueue( &queueDesc, IID_PPV_ARGS( commandQueue_.GetAddressOf() ) ), "Failed to create command queue." );
	}

	void DeviceManager::Impl::InitializeDescriptorHeaps()
	{
		D3D12_DESCRIPTOR_HEAP_DESC bindlessDesc{};
		bindlessDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		bindlessDesc.NumDescriptors = desc_.bindlessCapacity;
		bindlessDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		detail::ThrowIfFailed( device_->CreateDescriptorHeap( &bindlessDesc, IID_PPV_ARGS( bindlessHeap_.GetAddressOf() ) ), "Failed to create bindless heap." );

		D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
		rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvDesc.NumDescriptors = desc_.rtvCapacity;
		detail::ThrowIfFailed( device_->CreateDescriptorHeap( &rtvDesc, IID_PPV_ARGS( rtvHeap_.GetAddressOf() ) ), "Failed to create RTV heap." );

		D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
		dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvDesc.NumDescriptors = desc_.dsvCapacity;
		detail::ThrowIfFailed( device_->CreateDescriptorHeap( &dsvDesc, IID_PPV_ARGS( dsvHeap_.GetAddressOf() ) ), "Failed to create DSV heap." );

		bindlessDescriptorSize_ = device_->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
		rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_RTV );
		dsvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );

		freeBindlessDescriptors_.reserve( desc_.bindlessCapacity );
		for( uint32_t index = desc_.bindlessCapacity; index > 0; --index )
		{
			freeBindlessDescriptors_.push_back( index - 1u );
		}

		freeRtvDescriptors_.reserve( desc_.rtvCapacity );
		for( uint32_t index = desc_.rtvCapacity; index > 0; --index )
		{
			freeRtvDescriptors_.push_back( index - 1u );
		}

		freeDsvDescriptors_.reserve( desc_.dsvCapacity );
		for( uint32_t index = desc_.dsvCapacity; index > 0; --index )
		{
			freeDsvDescriptors_.push_back( index - 1u );
		}
	}

	void DeviceManager::Impl::InitializeRootSignature()
	{
		D3D12_ROOT_PARAMETER1 parameters[ 2 ] = {};
		parameters[ 0 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[ 0 ].Constants.Num32BitValues = 63;
		parameters[ 0 ].Constants.ShaderRegister = 0;
		parameters[ 0 ].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		parameters[ 1 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[ 1 ].Constants.Num32BitValues = 1;
		parameters[ 1 ].Constants.ShaderRegister = 1;
		parameters[ 1 ].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_STATIC_SAMPLER_DESC sampler{};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.ShaderRegister = 0;
		sampler.RegisterSpace = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
		rootDesc.Desc_1_1.NumParameters = 2;
		rootDesc.Desc_1_1.pParameters = parameters;
		rootDesc.Desc_1_1.NumStaticSamplers = 1;
		rootDesc.Desc_1_1.pStaticSamplers = &sampler;
		rootDesc.Desc_1_1.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

		ComPtr<ID3DBlob> serialized;
		ComPtr<ID3DBlob> errors;
		detail::ThrowIfFailed( D3D12SerializeVersionedRootSignature( &rootDesc, serialized.GetAddressOf(), errors.GetAddressOf() ), "Failed to serialize root signature." );
		detail::ThrowIfFailed(
			device_->CreateRootSignature( 0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS( rootSignature_.GetAddressOf() ) ),
			"Failed to create root signature." );
	}

	void DeviceManager::Impl::InitializeCommandSignature()
	{
		D3D12_INDIRECT_ARGUMENT_DESC arguments[ 2 ] = {};
		arguments[ 0 ].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
		arguments[ 0 ].Constant.RootParameterIndex = 1;
		arguments[ 0 ].Constant.Num32BitValuesToSet = 1;
		arguments[ 1 ].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

		D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
		signatureDesc.ByteStride = sizeof( uint32_t ) + sizeof( D3D12_DRAW_INDEXED_ARGUMENTS );
		signatureDesc.NumArgumentDescs = 2;
		signatureDesc.pArgumentDescs = arguments;

		detail::ThrowIfFailed(
			device_->CreateCommandSignature( &signatureDesc, rootSignature_.Get(), IID_PPV_ARGS( commandSignature_.GetAddressOf() ) ),
			"Failed to create command signature." );
	}

	uint32_t DeviceManager::Impl::AllocateBindlessDescriptor()
	{
		if( freeBindlessDescriptors_.empty() )
		{
			throw std::runtime_error( "Bindless descriptor heap is exhausted." );
		}

		const uint32_t index = freeBindlessDescriptors_.back();
		freeBindlessDescriptors_.pop_back();
		return index;
	}

	uint32_t DeviceManager::Impl::AllocateRtvDescriptor()
	{
		if( freeRtvDescriptors_.empty() )
		{
			throw std::runtime_error( "RTV descriptor heap is exhausted." );
		}

		const uint32_t index = freeRtvDescriptors_.back();
		freeRtvDescriptors_.pop_back();
		return index;
	}

	uint32_t DeviceManager::Impl::AllocateDsvDescriptor()
	{
		if( freeDsvDescriptors_.empty() )
		{
			throw std::runtime_error( "DSV descriptor heap is exhausted." );
		}

		const uint32_t index = freeDsvDescriptors_.back();
		freeDsvDescriptors_.pop_back();
		return index;
	}

	void DeviceManager::Impl::FreeBindlessDescriptor( uint32_t index )
	{
		if( index != UINT32_MAX )
		{
			freeBindlessDescriptors_.push_back( index );
		}
	}

	void DeviceManager::Impl::FreeRtvDescriptor( uint32_t index )
	{
		if( index != UINT32_MAX )
		{
			freeRtvDescriptors_.push_back( index );
		}
	}

	void DeviceManager::Impl::FreeDsvDescriptor( uint32_t index )
	{
		if( index != UINT32_MAX )
		{
			freeDsvDescriptors_.push_back( index );
		}
	}

	BufferResource& DeviceManager::Impl::GetBufferResource( BufferHandle handle )
	{
		auto* resource = slotMapBuffers_.Get( handle );
		if( resource == nullptr )
		{
			throw std::runtime_error( "Invalid buffer handle." );
		}
		return *resource;
	}

	const BufferResource& DeviceManager::Impl::GetBufferResource( BufferHandle handle ) const
	{
		const auto* resource = slotMapBuffers_.Get( handle );
		if( resource == nullptr )
		{
			throw std::runtime_error( "Invalid buffer handle." );
		}
		return *resource;
	}

	TextureResource& DeviceManager::Impl::GetTextureResource( TextureHandle handle )
	{
		auto* resource = slotMapTextures_.Get( handle );
		if( resource == nullptr )
		{
			throw std::runtime_error( "Invalid texture handle." );
		}
		return *resource;
	}

	const TextureResource& DeviceManager::Impl::GetTextureResource( TextureHandle handle ) const
	{
		const auto* resource = slotMapTextures_.Get( handle );
		if( resource == nullptr )
		{
			throw std::runtime_error( "Invalid texture handle." );
		}
		return *resource;
	}

	void DeviceManager::Impl::AddDeferredRelease( SubmitHandle handle, std::function<void()>&& release )
	{
		deferredReleases_.push_back( { handle, std::move( release ) } );
	}

	void DeviceManager::Impl::ProcessDeferredReleases()
	{
		while( !deferredReleases_.empty() )
		{
			if( !immediateCommands_->IsReady( deferredReleases_.front().handle_ ) )
			{
				break;
			}

			deferredReleases_.front().release_();
			deferredReleases_.pop_front();
		}
	}

	void DeviceManager::Impl::WaitForQueueIdle()
	{
		if( commandQueue_ == nullptr || queueIdleFence_ == nullptr )
		{
			return;
		}

		queueIdleFenceValue_++;
		detail::ThrowIfFailed( commandQueue_->Signal( queueIdleFence_.Get(), queueIdleFenceValue_ ), "Failed to signal queue idle fence." );
		if( queueIdleFence_->GetCompletedValue() < queueIdleFenceValue_ )
		{
			detail::ThrowIfFailed( queueIdleFence_->SetEventOnCompletion( queueIdleFenceValue_, queueIdleEvent_ ), "Failed to wait for queue idle fence." );
			WaitForSingleObject( queueIdleEvent_, INFINITE );
		}
	}

	void DeviceManager::Impl::WaitIdle()
	{
		if( immediateCommands_ )
		{
			immediateCommands_->WaitAll();
		}

		WaitForQueueIdle();

		ProcessDeferredReleases();
		while( !deferredReleases_.empty() )
		{
			deferredReleases_.front().release_();
			deferredReleases_.pop_front();
		}
	}

	void DeviceManager::Impl::Shutdown() noexcept
	{
		currentCommandBuffer_.reset();

		try
		{
			WaitIdle();
		}
		catch( ... )
		{
		}

		stagingDevice_.reset();
		immediateCommands_.reset();

		for( auto* buffer : slotMapBuffers_.GetAll() )
		{
			if( buffer != nullptr && buffer->mappedPtr_ != nullptr && buffer->resource_ != nullptr )
			{
				buffer->resource_->Unmap( 0, nullptr );
				buffer->mappedPtr_ = nullptr;
			}
		}

		if( swapchain_ != nullptr )
		{
			const HWND hwnd = detail::GetHwnd( swapchainDesc_.window );
			const bool hasLiveWindow = hwnd != nullptr && IsWindow( hwnd ) != FALSE;
			IDXGISwapChain4* nativeSwapchain = swapchain_->GetSwapchain();
			if( factory_ != nullptr && hasLiveWindow )
			{
				factory_->MakeWindowAssociation( hwnd, 0 );
			}
			if( nativeSwapchain != nullptr && hasLiveWindow )
			{
				BOOL isFullscreen = FALSE;
				if( SUCCEEDED( nativeSwapchain->GetFullscreenState( &isFullscreen, nullptr ) ) && isFullscreen )
				{
					nativeSwapchain->SetFullscreenState( FALSE, nullptr );
				}
			}
		}
		swapchain_.reset();

		slotMapTextures_.Clear();
		slotMapBuffers_.Clear();
		deferredReleases_.clear();

		commandSignature_.Reset();
		rootSignature_.Reset();
		dsvHeap_.Reset();
		rtvHeap_.Reset();
		bindlessHeap_.Reset();
		queueIdleFence_.Reset();
		if( queueIdleEvent_ != nullptr )
		{
			CloseHandle( queueIdleEvent_ );
			queueIdleEvent_ = nullptr;
		}

		commandQueue_.Reset();
		device_.Reset();
		adapter_.Reset();
		factory_.Reset();
	}

	void DeviceManager::Impl::ReportLiveObjects() noexcept
	{
#if defined( _DEBUG )
		if( !desc_.enableDebugLayer )
		{
			return;
		}

		if( device_ != nullptr )
		{
			ComPtr<ID3D12DebugDevice> debugDevice;
			if( SUCCEEDED( device_.As( &debugDevice ) ) && debugDevice != nullptr )
			{
				debugDevice->ReportLiveDeviceObjects( D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL );
			}
		}
#endif
	}

	void DeviceManager::Impl::CreateSwapchain()
	{
		swapchain_ = std::make_unique<Swapchain>(
			*this,
			detail::GetHwnd( swapchainDesc_.window ),
			swapchainDesc_.width,
			swapchainDesc_.height );
	}

	void DeviceManager::Impl::Resize( uint32_t width, uint32_t height )
	{
		if( width == 0 || height == 0 )
		{
			return;
		}

		WaitIdle();
		swapchainDesc_.width = width;
		swapchainDesc_.height = height;
		if( swapchain_ != nullptr )
		{
			swapchain_->Resize( width, height );
		}
	}

	DeviceManager::DeviceManager( const ContextDesc& desc, const SwapchainDesc& swapchainDesc ):
		impl_( std::make_unique<Impl>( desc, swapchainDesc ) ),
		renderDevice_( *this )
	{
		impl_->Initialize();
	}

	DeviceManager::~DeviceManager() = default;

	RenderDevice* DeviceManager::GetRenderDevice() noexcept
	{
		return &renderDevice_;
	}

	const RenderDevice* DeviceManager::GetRenderDevice() const noexcept
	{
		return &renderDevice_;
	}

	void DeviceManager::Resize( uint32_t width, uint32_t height )
	{
		impl_->Resize( width, height );
	}

	uint32_t DeviceManager::GetWidth() const noexcept
	{
		return impl_->swapchainDesc_.width;
	}

	uint32_t DeviceManager::GetHeight() const noexcept
	{
		return impl_->swapchainDesc_.height;
	}

	bool DeviceManager::IsVsyncEnabled() const noexcept
	{
		return impl_->swapchainDesc_.vsync;
	}

	void DeviceManager::SetVsync( bool enabled ) noexcept
	{
		impl_->swapchainDesc_.vsync = enabled;
	}

	void DeviceManager::WaitIdle()
	{
		impl_->WaitIdle();
	}
}


