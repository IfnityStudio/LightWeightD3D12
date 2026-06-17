#include "LightD3D12ManagerImpl.hpp"

#include "LightD3D12BaseMips.hpp"
#include "LightD3D12StagingDevice.hpp"
#include "LightD3D12Swapchain.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <vector>

namespace lightd3d12
{
	namespace
	{
		std::mutex gDeviceManagerSingletonMutex;
		std::unique_ptr<DeviceManager> gDeviceManagerSingleton;
		uint32_t gDeviceManagerSingletonReferenceCount = 0;

		bool ContextDescsAreCompatible( const ContextDesc& left, const ContextDesc& right ) noexcept
		{
			return left.enableDebugLayer == right.enableDebugLayer &&
				left.preferHighPerformanceAdapter == right.preferHighPerformanceAdapter &&
				left.allowTearing == right.allowTearing &&
				left.enablePixGpuCapture == right.enablePixGpuCapture &&
				left.framesInFlight == right.framesInFlight &&
				left.bindlessCapacity == right.bindlessCapacity &&
				left.rtvCapacity == right.rtvCapacity &&
				left.dsvCapacity == right.dsvCapacity &&
				left.swapchainBufferCount == right.swapchainBufferCount &&
				left.swapchainFormat == right.swapchainFormat &&
				left.minimumFeatureLevel == right.minimumFeatureLevel;
		}

		void ReleaseDeviceManagerSingleton() noexcept
		{
			std::unique_ptr<DeviceManager> singletonToDestroy;
			{
				std::lock_guard lock( gDeviceManagerSingletonMutex );
				if( gDeviceManagerSingletonReferenceCount == 0 )
				{
					return;
				}

				--gDeviceManagerSingletonReferenceCount;
				if( gDeviceManagerSingletonReferenceCount == 0 )
				{
					singletonToDestroy = std::move( gDeviceManagerSingleton );
				}
			}

			singletonToDestroy.reset();
		}

#if defined( LIGHTD3D12_ENABLE_PIX )
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

		bool TryLoadPixGpuCapturerInternal()
		{
			static bool ourAttemptedLoad = false;
			if( ourAttemptedLoad )
			{
				return ::GetModuleHandleW( L"WinPixGpuCapturer.dll" ) != nullptr;
			}

			ourAttemptedLoad = true;
			if( ::GetModuleHandleW( L"WinPixGpuCapturer.dll" ) != nullptr )
			{
				return true;
			}

			// First allow a local copy next to the executable or on PATH.
			if( ::LoadLibraryW( L"WinPixGpuCapturer.dll" ) != nullptr )
			{
				return true;
			}

			wchar_t* programFilesValue = nullptr;
			size_t programFilesLength = 0;
			if( _wdupenv_s( &programFilesValue, &programFilesLength, L"ProgramFiles" ) != 0 || programFilesValue == nullptr || programFilesValue[ 0 ] == L'\0' )
			{
				free( programFilesValue );
				return false;
			}

			const std::filesystem::path pixInstallRoot = std::filesystem::path( programFilesValue ) / "Microsoft PIX";
			free( programFilesValue );
			if( !std::filesystem::exists( pixInstallRoot ) )
			{
				return false;
			}

			std::filesystem::path latestCapturerPath;
			std::vector<uint32_t> latestVersion;

			for( const auto& entry : std::filesystem::directory_iterator( pixInstallRoot ) )
			{
				if( !entry.is_directory() )
				{
					continue;
				}

				const std::filesystem::path candidateDll = entry.path() / "WinPixGpuCapturer.dll";
				if( !std::filesystem::exists( candidateDll ) )
				{
					continue;
				}

				const std::vector<uint32_t> candidateVersion = ParseVersionComponents( entry.path().filename().wstring() );
				if( latestCapturerPath.empty() || IsVersionGreater( candidateVersion, latestVersion ) )
				{
					latestCapturerPath = candidateDll;
					latestVersion = candidateVersion;
				}
			}

			if( !latestCapturerPath.empty() )
			{
				if( ::LoadLibraryW( latestCapturerPath.c_str() ) != nullptr )
				{
					return true;
				}
			}

			return false;
		}
#endif
	}

	bool TryLoadPixGpuCapturer() noexcept
	{
#if defined( LIGHTD3D12_ENABLE_PIX )
		return TryLoadPixGpuCapturerInternal();
#else
		return false;
#endif
	}

	bool IsPixGpuCapturerLoaded() noexcept
	{
#if defined( LIGHTD3D12_ENABLE_PIX )
		return ::GetModuleHandleW( L"WinPixGpuCapturer.dll" ) != nullptr;
#else
		return false;
#endif
	}

	DeviceManager::Impl::Impl( const ContextDesc& desc ):
		desc_( desc )
	{
	}

	DeviceManager::Impl::~Impl()
	{
		Shutdown();
	}

	void DeviceManager::Impl::Initialize()
	{
		if( desc_.enablePixGpuCapture )
		{
			// PIX GPU capture attach only works if the capturer DLL is loaded before any D3D12 device creation.
			TryLoadPixGpuCapturer();
		}

		InitializeFactory();
		InitializeDevice();
		InitializeCommandQueues();
		InitializeDescriptorHeaps();
		InitializeRootSignature();
		InitializeCommandSignature();
		baseMips_ = std::make_unique<BaseMips>( *this );
		stagingDevice_ = std::make_unique<StagingDevice>( *this );
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
		C_RESULT( CreateDXGIFactory2( flags, IID_PPV_ARGS( factory_.GetAddressOf() ) ), "Failed to create DXGI factory." );
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
			C_RESULT( factory_->EnumWarpAdapter( IID_PPV_ARGS( warpAdapter.GetAddressOf() ) ), "Failed to enumerate WARP adapter." );
			C_RESULT( D3D12CreateDevice( warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS( device_.GetAddressOf() ) ), "Failed to create D3D12 device." );
		}

		D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
		if( SUCCEEDED( device_->CheckFeatureSupport( D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof( options ) ) ) )
		{
			bindlessSupported_ = options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2;
		}
	}

	void DeviceManager::Impl::InitializeCommandQueues()
	{
		#if LIGHTD3D12_SINGLE_DIRECT_QUEUE
		InitializeQueueContext( graphicsQueue_, D3D12_COMMAND_LIST_TYPE_DIRECT );
		#else 
			throw std::runtime_error("Error Lightd3d12 not multiple queue supported please use LIGHTD3D12_SINGLE_DIRECT_QUEUE define set 1");
		#endif
	}

	void DeviceManager::Impl::InitializeQueueContext( QueueContext& context, D3D12_COMMAND_LIST_TYPE type )
	{
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = type;
		C_RESULT( device_->CreateCommandQueue( &queueDesc, IID_PPV_ARGS( context.commandQueue_.GetAddressOf() ) ), "Failed to create command queue." );
		C_RESULT( device_->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( context.queueIdleFence_.GetAddressOf() ) ), "Failed to create queue idle fence." );
		context.queueIdleEvent_ = CreateEvent( nullptr, FALSE, FALSE, nullptr );
		if( context.queueIdleEvent_ == nullptr )
		{
			throw std::runtime_error( "Failed to create queue idle event." );
		}

		context.immediateCommands_ = std::make_unique<ImmediateCommands>(
			device_.Get(),
			context.commandQueue_.Get(),
			std::max( std::max<uint32_t>( 1u, desc_.framesInFlight ), ourMaxActiveCommandBuffers ) );
	}

	DeviceManager::Impl::QueueContext& DeviceManager::Impl::GetQueueContext( QueueType type ) noexcept
	{
#if LIGHTD3D12_SINGLE_DIRECT_QUEUE
		static_cast<void>( type );
		return graphicsQueue_;
#else
		switch( type )
		{
			case QueueType::Graphics:
				return graphicsQueue_;

			case QueueType::Compute:
				return computeQueue_;

			case QueueType::Copy:
				return copyQueue_;
		}

		return graphicsQueue_;
#endif
	}

	const DeviceManager::Impl::QueueContext& DeviceManager::Impl::GetQueueContext( QueueType type ) const noexcept
	{
#if LIGHTD3D12_SINGLE_DIRECT_QUEUE
		static_cast<void>( type );
		return graphicsQueue_;
#else
		switch( type )
		{
			case QueueType::Graphics:
				return graphicsQueue_;

			case QueueType::Compute:
				return computeQueue_;

			case QueueType::Copy:
				return copyQueue_;
		}

		return graphicsQueue_;
#endif
	}

	DeviceManager::Impl::QueueContext& DeviceManager::Impl::GetGraphicsQueueContext() noexcept
	{
		return GetQueueContext( QueueType::Graphics );
	}

	const DeviceManager::Impl::QueueContext& DeviceManager::Impl::GetGraphicsQueueContext() const noexcept
	{
		return GetQueueContext( QueueType::Graphics );
	}

	void DeviceManager::Impl::InitializeDescriptorHeaps()
	{
		D3D12_DESCRIPTOR_HEAP_DESC bindlessDesc{};
		bindlessDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		bindlessDesc.NumDescriptors = desc_.bindlessCapacity;
		bindlessDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		C_RESULT( device_->CreateDescriptorHeap( &bindlessDesc, IID_PPV_ARGS( bindlessHeap_.GetAddressOf() ) ), "Failed to create bindless heap." );

		D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
		rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvDesc.NumDescriptors = desc_.rtvCapacity;
		C_RESULT( device_->CreateDescriptorHeap( &rtvDesc, IID_PPV_ARGS( rtvHeap_.GetAddressOf() ) ), "Failed to create RTV heap." );

		D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
		dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvDesc.NumDescriptors = desc_.dsvCapacity;
		C_RESULT( device_->CreateDescriptorHeap( &dsvDesc, IID_PPV_ARGS( dsvHeap_.GetAddressOf() ) ), "Failed to create DSV heap." );

		bindlessDescriptorSize_ = device_->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
		rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_RTV );
		dsvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );

		freeBindlessRanges_.clear();
		freeBindlessRanges_.push_back( DescriptorRange{ .start_ = 0u, .count_ = desc_.bindlessCapacity } );

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
		// Clamp is safer as the engine-wide default because ImGui also uses this root signature.
		// UI atlases and preview images can show wrapped glyph bleed if the shared sampler uses WRAP.
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.MinLOD = 0.0f;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.MipLODBias = 0.0f;
		sampler.ShaderRegister = 0;
		sampler.RegisterSpace = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
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
		C_RESULT( D3D12SerializeVersionedRootSignature( &rootDesc, serialized.GetAddressOf(), errors.GetAddressOf() ), "Failed to serialize root signature." );
		C_RESULT(
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

		C_RESULT(
			device_->CreateCommandSignature( &signatureDesc, rootSignature_.Get(), IID_PPV_ARGS( commandSignature_.GetAddressOf() ) ),
			"Failed to create command signature." );
	}

	uint32_t DeviceManager::Impl::AllocateBindlessDescriptor()
	{
		return AllocateBindlessDescriptorRange( 1u );
	}

	uint32_t DeviceManager::Impl::AllocateBindlessDescriptorRange( uint32_t count )
	{
		if( count == 0 )
		{
			throw std::runtime_error( "Bindless descriptor allocation count must be greater than zero." );
		}

		for( size_t rangeIndex = 0; rangeIndex < freeBindlessRanges_.size(); ++rangeIndex )
		{
			DescriptorRange& range = freeBindlessRanges_[ rangeIndex ];
			if( range.count_ < count )
			{
				continue;
			}

			const uint32_t start = range.start_;
			range.start_ += count;
			range.count_ -= count;
			if( range.count_ == 0 )
			{
				freeBindlessRanges_.erase( freeBindlessRanges_.begin() + static_cast<ptrdiff_t>( rangeIndex ) );
			}

			return start;
		}

		throw std::runtime_error( "Bindless descriptor heap is exhausted." );
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
		FreeBindlessDescriptorRange( index, 1u );
	}

	void DeviceManager::Impl::FreeBindlessDescriptorRange( uint32_t index, uint32_t count )
	{
		if( index == UINT32_MAX || count == 0 )
		{
			return;
		}

		DescriptorRange mergedRange{ .start_ = index, .count_ = count };
		auto insertIt = freeBindlessRanges_.begin();
		while( insertIt != freeBindlessRanges_.end() && insertIt->start_ < mergedRange.start_ )
		{
			++insertIt;
		}

		insertIt = freeBindlessRanges_.insert( insertIt, mergedRange );

		if( insertIt != freeBindlessRanges_.begin() )
		{
			auto previous = insertIt - 1;
			if( previous->start_ + previous->count_ == insertIt->start_ )
			{
				previous->count_ += insertIt->count_;
				insertIt = freeBindlessRanges_.erase( insertIt );
				insertIt = previous;
			}
		}

		auto next = insertIt + 1;
		if( next != freeBindlessRanges_.end() && insertIt->start_ + insertIt->count_ == next->start_ )
		{
			insertIt->count_ += next->count_;
			freeBindlessRanges_.erase( next );
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
		GetGraphicsQueueContext().deferredReleases_.push_back( { handle, std::move( release ) } );
	}

	void DeviceManager::Impl::ProcessDeferredReleases()
	{
		ProcessDeferredReleases( GetGraphicsQueueContext() );
	}

	void DeviceManager::Impl::ProcessDeferredReleases( QueueContext& context )
	{
		while( !context.deferredReleases_.empty() )
		{
			if( context.immediateCommands_ == nullptr || !context.immediateCommands_->IsReady( context.deferredReleases_.front().handle_ ) )
			{
				break;
			}

			context.deferredReleases_.front().release_();
			context.deferredReleases_.pop_front();
		}
	}

	void DeviceManager::Impl::WaitForQueueIdle()
	{
		WaitForQueueIdle( GetGraphicsQueueContext() );
	}

	void DeviceManager::Impl::WaitForQueueIdle( QueueContext& context )
	{
		if( context.commandQueue_ == nullptr || context.queueIdleFence_ == nullptr )
		{
			return;
		}

		context.queueIdleFenceValue_++;
		C_RESULT( context.commandQueue_->Signal( context.queueIdleFence_.Get(), context.queueIdleFenceValue_ ), "Failed to signal queue idle fence." );
		if( context.queueIdleFence_->GetCompletedValue() < context.queueIdleFenceValue_ )
		{
			C_RESULT( context.queueIdleFence_->SetEventOnCompletion( context.queueIdleFenceValue_, context.queueIdleEvent_ ), "Failed to wait for queue idle fence." );
			WaitForSingleObject( context.queueIdleEvent_, INFINITE );
		}
	}

	void DeviceManager::Impl::WaitIdle()
	{
		auto waitQueueIdle = [ this ]( QueueContext& context )
		{
			if( context.immediateCommands_ )
			{
				context.immediateCommands_->WaitAll();
			}

			WaitForQueueIdle( context );
			ProcessDeferredReleases( context );
			while( !context.deferredReleases_.empty() )
			{
				context.deferredReleases_.front().release_();
				context.deferredReleases_.pop_front();
			}
		};

		waitQueueIdle( graphicsQueue_ );
#if !LIGHTD3D12_SINGLE_DIRECT_QUEUE
		waitQueueIdle( computeQueue_ );
		waitQueueIdle( copyQueue_ );
#endif
	}

	void DeviceManager::Impl::Shutdown() noexcept
	{
		auto resetActiveBuffers = []( QueueContext& context )
		{
			for( auto& activeCommandBuffer : context.activeCommandBuffers_ )
			{
				activeCommandBuffer.reset();
			}
		};

		resetActiveBuffers( graphicsQueue_ );
#if !LIGHTD3D12_SINGLE_DIRECT_QUEUE
		resetActiveBuffers( computeQueue_ );
		resetActiveBuffers( copyQueue_ );
#endif

		try
		{
			WaitIdle();
		}
		catch( ... )
		{
		}

		stagingDevice_.reset();
		graphicsQueue_.immediateCommands_.reset();
#if !LIGHTD3D12_SINGLE_DIRECT_QUEUE
		computeQueue_.immediateCommands_.reset();
		copyQueue_.immediateCommands_.reset();
#endif
		baseMips_.reset();

		for( auto* buffer : slotMapBuffers_.GetAll() )
		{
			if( buffer != nullptr && buffer->mappedPtr_ != nullptr && buffer->resource_ != nullptr )
			{
				buffer->resource_->Unmap( 0, nullptr );
				buffer->mappedPtr_ = nullptr;
			}
		}

		for( auto* swapchainResource : slotMapSwapchains_.GetAll() )
		{
			if( swapchainResource == nullptr || swapchainResource->swapchain_ == nullptr )
			{
				continue;
			}

			const HWND hwnd = detail::GetHwnd( swapchainResource->desc_.window );
			const bool hasLiveWindow = hwnd != nullptr && IsWindow( hwnd ) != FALSE;
			IDXGISwapChain4* nativeSwapchain = swapchainResource->swapchain_->GetSwapchain();
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
		slotMapSwapchains_.Clear();

		slotMapTextures_.Clear();
		slotMapBuffers_.Clear();
		graphicsQueue_.deferredReleases_.clear();
#if !LIGHTD3D12_SINGLE_DIRECT_QUEUE
		computeQueue_.deferredReleases_.clear();
		copyQueue_.deferredReleases_.clear();
#endif

		commandSignature_.Reset();
		rootSignature_.Reset();
		dsvHeap_.Reset();
		rtvHeap_.Reset();
		bindlessHeap_.Reset();
		auto releaseQueueContext = []( QueueContext& context )
		{
			context.queueIdleFence_.Reset();
			if( context.queueIdleEvent_ != nullptr )
			{
				CloseHandle( context.queueIdleEvent_ );
				context.queueIdleEvent_ = nullptr;
			}

			context.commandQueue_.Reset();
		};

		releaseQueueContext( graphicsQueue_ );
#if !LIGHTD3D12_SINGLE_DIRECT_QUEUE
		releaseQueueContext( computeQueue_ );
		releaseQueueContext( copyQueue_ );
#endif
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

	SwapchainHandle DeviceManager::Impl::CreateSwapchain( const SwapchainDesc& desc )
	{
		SwapchainResource swapchainResource{};
		swapchainResource.desc_ = desc;
		const SwapchainHandle handle = slotMapSwapchains_.Create( std::move( swapchainResource ) );
		auto* resource = slotMapSwapchains_.Get( handle );
		if( resource == nullptr )
		{
			throw std::runtime_error( "Failed to allocate swapchain slot." );
		}

		try
		{
			resource->swapchain_ = std::make_unique<Swapchain>(
				*this,
				handle,
				detail::GetHwnd( desc.window ),
				desc.width,
				desc.height );
		}
		catch( ... )
		{
			slotMapSwapchains_.Destroy( handle );
			throw;
		}

		return handle;
	}

	void DeviceManager::Impl::DestroySwapchain( SwapchainHandle swapchain ) noexcept
	{
		auto* resource = slotMapSwapchains_.Get( swapchain );
		if( resource == nullptr )
		{
			return;
		}

		resource->swapchain_.reset();
		slotMapSwapchains_.Destroy( swapchain );
	}

	Swapchain* DeviceManager::Impl::GetSwapchain( SwapchainHandle swapchain ) noexcept
	{
		auto* resource = slotMapSwapchains_.Get( swapchain );
		return resource != nullptr ? resource->swapchain_.get() : nullptr;
	}

	const Swapchain* DeviceManager::Impl::GetSwapchain( SwapchainHandle swapchain ) const noexcept
	{
		const auto* resource = slotMapSwapchains_.Get( swapchain );
		return resource != nullptr ? resource->swapchain_.get() : nullptr;
	}

	SwapchainDesc* DeviceManager::Impl::GetSwapchainDesc( SwapchainHandle swapchain ) noexcept
	{
		auto* resource = slotMapSwapchains_.Get( swapchain );
		return resource != nullptr ? &resource->desc_ : nullptr;
	}

	const SwapchainDesc* DeviceManager::Impl::GetSwapchainDesc( SwapchainHandle swapchain ) const noexcept
	{
		const auto* resource = slotMapSwapchains_.Get( swapchain );
		return resource != nullptr ? &resource->desc_ : nullptr;
	}

	Swapchain* DeviceManager::Impl::GetOwningSwapchain( TextureHandle texture ) noexcept
	{
		const auto* resource = slotMapTextures_.Get( texture );
		if( resource == nullptr || !resource->swapchain_.Valid() )
		{
			return nullptr;
		}

		return GetSwapchain( resource->swapchain_ );
	}

	DeviceManager::DeviceManager( const ContextDesc& desc ):
		impl_( std::make_unique<Impl>( desc ) ),
		renderDevice_( *this )
	{
		impl_->Initialize();
	}

	DeviceManager& DeviceManager::Initialize( const ContextDesc& desc )
	{
		std::lock_guard lock( gDeviceManagerSingletonMutex );
		if( gDeviceManagerSingleton == nullptr )
		{
			gDeviceManagerSingleton = std::unique_ptr<DeviceManager>( new DeviceManager( desc ) );
		}
		else if( !ContextDescsAreCompatible( gDeviceManagerSingleton->impl_->desc_, desc ) )
		{
			throw std::runtime_error( "DeviceManager singleton already initialized with an incompatible ContextDesc." );
		}

		++gDeviceManagerSingletonReferenceCount;
		return *gDeviceManagerSingleton;
	}

	DeviceManager& DeviceManager::Initialize( const ContextDesc& desc, const SwapchainDesc& primarySwapchainDesc )
	{
		DeviceManager& manager = Initialize( desc );
		try
		{
			if( !manager.primarySwapchain_.Valid() )
			{
				manager.primarySwapchain_ = manager.CreateSwapchain( primarySwapchainDesc );
			}
		}
		catch( ... )
		{
			ReleaseDeviceManagerSingleton();
			throw;
		}

		return manager;
	}


	DeviceManager& DeviceManager::Get()
	{
          DeviceManager* manager = gDeviceManagerSingleton.get();
		if( manager == nullptr )
		{
			throw std::runtime_error( "DeviceManager singleton is not initialized." );
		}

		return *manager;
	}

	void DeviceManager::ShutdownSingleton()
	{
		ReleaseDeviceManagerSingleton();
	}

	DeviceManager::~DeviceManager() = default;

	SwapchainHandle DeviceManager::CreateSwapchain( const SwapchainDesc& desc )
	{
		const SwapchainHandle handle = impl_->CreateSwapchain( desc );
		if( !primarySwapchain_.Valid() )
		{
			primarySwapchain_ = handle;
		}

		return handle;
	}

	void DeviceManager::DestroySwapchain( SwapchainHandle swapchain )
	{
		if( !swapchain.Valid() || impl_ == nullptr )
		{
			return;
		}

		WaitIdle();
		impl_->DestroySwapchain( swapchain );
		if( primarySwapchain_ == swapchain )
		{
			primarySwapchain_ = {};
		}
	}

	RenderDevice* DeviceManager::GetRenderDevice() noexcept
	{
		return &renderDevice_;
	}

	const RenderDevice* DeviceManager::GetRenderDevice() const noexcept
	{
		return &renderDevice_;
	}

	SwapchainHandle DeviceManager::RequirePrimarySwapchain() const
	{
		return primarySwapchain_;
	}

	void DeviceManager::Resize( uint32_t width, uint32_t height )
	{
		Resize( primarySwapchain_, width, height );
	}

	void DeviceManager::Resize( SwapchainHandle swapchain, uint32_t width, uint32_t height )
	{
		if( width == 0 || height == 0 || impl_ == nullptr )
		{
			return;
		}

		SwapchainDesc* swapchainDesc = impl_->GetSwapchainDesc( swapchain );
		Swapchain* nativeSwapchain = impl_->GetSwapchain( swapchain );
		if( swapchainDesc == nullptr || nativeSwapchain == nullptr )
		{
			return;
		}

		WaitIdle();
		swapchainDesc->width = width;
		swapchainDesc->height = height;
		nativeSwapchain->Resize( width, height );
	}

	uint32_t DeviceManager::GetWidth() const noexcept
	{
		return GetWidth( primarySwapchain_ );
	}

	uint32_t DeviceManager::GetWidth( SwapchainHandle swapchain ) const noexcept
	{
		const SwapchainDesc* swapchainDesc = impl_ != nullptr ? impl_->GetSwapchainDesc( swapchain ) : nullptr;
		return swapchainDesc != nullptr ? swapchainDesc->width : 0u;
	}

	uint32_t DeviceManager::GetHeight() const noexcept
	{
		return GetHeight( primarySwapchain_ );
	}

	uint32_t DeviceManager::GetHeight( SwapchainHandle swapchain ) const noexcept
	{
		const SwapchainDesc* swapchainDesc = impl_ != nullptr ? impl_->GetSwapchainDesc( swapchain ) : nullptr;
		return swapchainDesc != nullptr ? swapchainDesc->height : 0u;
	}

	bool DeviceManager::IsVsyncEnabled() const noexcept
	{
		return IsVsyncEnabled( primarySwapchain_ );
	}

	bool DeviceManager::IsVsyncEnabled( SwapchainHandle swapchain ) const noexcept
	{
		const SwapchainDesc* swapchainDesc = impl_ != nullptr ? impl_->GetSwapchainDesc( swapchain ) : nullptr;
		return swapchainDesc != nullptr ? swapchainDesc->vsync : true;
	}

	void DeviceManager::SetVsync( bool enabled ) noexcept
	{
		SetVsync( primarySwapchain_, enabled );
	}

	void DeviceManager::SetVsync( SwapchainHandle swapchain, bool enabled ) noexcept
	{
		SwapchainDesc* swapchainDesc = impl_ != nullptr ? impl_->GetSwapchainDesc( swapchain ) : nullptr;
		if( swapchainDesc != nullptr )
		{
			swapchainDesc->vsync = enabled;
		}
	}

	void DeviceManager::WaitIdle()
	{
		impl_->WaitIdle();
	}
}


