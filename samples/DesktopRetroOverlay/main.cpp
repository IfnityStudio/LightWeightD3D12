#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightHLSLLoader.hpp"

#include <d3d11.h>
#include <d3d11_4.h>
#include <shellapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

using namespace lightd3d12;

namespace
{
	constexpr DWORD kWindowDisplayAffinityExcludeFromCapture = 0x00000011;
	constexpr UINT kTrayCallbackMessage = WM_APP + 1;
	constexpr UINT kTrayIconId = 1;

	enum TrayCommandId : UINT
	{
		TrayToggle = 100,
		TrayCrt,
		TrayPs2,
		TrayNewPixie,
		TrayAmber,
		TrayGreen,
		TraySettings,
		TrayQuit,
	};

	enum HotkeyId : int
	{
		HotkeyToggle = 1,
		HotkeyCrt,
		HotkeyPs2,
		HotkeyNewPixie,
		HotkeyAmber,
		HotkeyGreen,
		HotkeyMoreIntensity,
		HotkeyLessIntensity,
		HotkeySettings,
		HotkeyQuit,
	};

	enum SettingsCommandId : UINT
	{
		SettingsIntensityMore = 200,
		SettingsIntensityLess,
		SettingsHidePanel,
	};

	struct RetroPushConstants
	{
		uint32_t sourceTextureIndex = 0;
		uint32_t mode = 3;
		float intensity = 1.00f;
		float curvature = 0.032f;
		float scanlineStrength = 0.48f;
		float chromaticAberrationPixels = 0.65f;
		float noiseStrength = 0.0f;
		float time = 0.0f;
		float inverseWidth = 1.0f;
		float inverseHeight = 1.0f;
		uint32_t blurTextureIndex = 0;
		uint32_t reserved = 0;
	};

	static_assert( sizeof( RetroPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct NewPixieAccumulatePushConstants
	{
		uint32_t sourceTextureIndex = 0;
		uint32_t historyTextureIndex = 0;
		float persistence = 0.0f;
		float padding = 0.0f;
	};

	struct NewPixieBlurPushConstants
	{
		uint32_t sourceTextureIndex = 0;
		uint32_t unusedTextureIndex = 0;
		float stepX = 0.0f;
		float stepY = 0.0f;
	};

	void ThrowIfFailed( HRESULT result, const char* message )
	{
		if( FAILED( result ) )
		{
			char diagnostic[ 256 ]{};
			sprintf_s( diagnostic, "%s (HRESULT 0x%08X)", message, static_cast<unsigned int>( result ) );
			throw std::runtime_error( diagnostic );
		}
	}

	RECT GetMonitorRect( HMONITOR monitor )
	{
		MONITORINFO info{};
		info.cbSize = sizeof( info );
		if( !GetMonitorInfoW( monitor, &info ) )
		{
			throw std::runtime_error( "Failed to get the primary monitor bounds." );
		}

		return info.rcMonitor;
	}

	HMONITOR GetPrimaryMonitor()
	{
		const HMONITOR monitor = MonitorFromPoint( POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY );
		if( monitor == nullptr )
		{
			throw std::runtime_error( "Failed to locate the primary monitor." );
		}

		return monitor;
	}

	class DesktopDuplicator final
	{
	public:
		enum class UpdateResult
		{
			NoNewFrame,
			FrameCopied,
			AccessLost,
		};

		void Initialize( RenderDevice& renderDevice, HMONITOR monitor, HWND statusWindow )
		{
			statusWindow_ = statusWindow;
			SetWindowTextW( statusWindow_, L"LightD3D12 Desktop Retro Overlay | initializing native capture device" );
			ComPtr<IDXGIFactory6> factory;
			ThrowIfFailed(
				CreateDXGIFactory1( IID_PPV_ARGS( factory.GetAddressOf() ) ),
				"Failed to create the DXGI factory used to select the capture adapter." );

			const LUID targetAdapterLuid = renderDevice.GetNativeDevice()->GetAdapterLuid();
			ComPtr<IDXGIAdapter1> adapter;
			for( UINT adapterIndex = 0;; ++adapterIndex )
			{
				ComPtr<IDXGIAdapter1> candidate;
				const HRESULT result = factory->EnumAdapters1( adapterIndex, candidate.GetAddressOf() );
				if( result == DXGI_ERROR_NOT_FOUND )
				{
					break;
				}
				ThrowIfFailed( result, "Failed to enumerate the graphics adapters." );

				DXGI_ADAPTER_DESC1 candidateDesc{};
				ThrowIfFailed( candidate->GetDesc1( &candidateDesc ), "Failed to inspect a graphics adapter." );
				if( candidateDesc.AdapterLuid.HighPart == targetAdapterLuid.HighPart &&
					candidateDesc.AdapterLuid.LowPart == targetAdapterLuid.LowPart )
				{
					adapter = std::move( candidate );
					break;
				}
			}
			if( !adapter )
			{
				throw std::runtime_error( "Could not find the Direct3D 12 adapter for native Windows Graphics Capture." );
			}

			ThrowIfFailed(
				D3D11CreateDevice(
					adapter.Get(),
					D3D_DRIVER_TYPE_UNKNOWN,
					nullptr,
					D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
					nullptr,
					0,
					D3D11_SDK_VERSION,
					d3d11Device_.GetAddressOf(),
					nullptr,
					d3d11Context_.GetAddressOf() ),
				"Failed to create the native D3D11 device used by Windows Graphics Capture." );

			ThrowIfFailed(
				d3d11Device_.As( &d3d11Device5_ ),
				"The capture device does not support Direct3D 11 shared fences." );
			ThrowIfFailed(
				d3d11Context_.As( &d3d11Context4_ ),
				"The capture context does not support Direct3D 11 shared fences." );
			ThrowIfFailed(
				d3d11Device5_->CreateFence(
					0,
					D3D11_FENCE_FLAG_SHARED,
					IID_PPV_ARGS( captureFence11_.GetAddressOf() ) ),
				"Failed to create the Direct3D 11 capture fence." );
			HANDLE captureFenceHandle = nullptr;
			ThrowIfFailed(
				captureFence11_->CreateSharedHandle( nullptr, GENERIC_ALL, nullptr, &captureFenceHandle ),
				"Failed to share the Direct3D 11 capture fence." );
			const HRESULT openCaptureFenceResult = renderDevice.GetNativeDevice()->OpenSharedHandle(
				captureFenceHandle, IID_PPV_ARGS( captureFence12_.GetAddressOf() ) );
			CloseHandle( captureFenceHandle );
			ThrowIfFailed( openCaptureFenceResult, "Direct3D 12 could not open the capture synchronization fence." );
			ThrowIfFailed(
				renderDevice.GetNativeDevice()->CreateFence(
					0,
					D3D12_FENCE_FLAG_NONE,
					IID_PPV_ARGS( renderFence_.GetAddressOf() ) ),
				"Failed to create the Direct3D 12 fence for shared desktop textures." );
			renderFenceEvent_ = CreateEventW( nullptr, FALSE, FALSE, nullptr );
			if( renderFenceEvent_ == nullptr )
			{
				throw std::runtime_error( "Failed to create the desktop texture synchronization event." );
			}

			ComPtr<IDXGIDevice> dxgiDevice;
			ThrowIfFailed(
				d3d11Device_.As( &dxgiDevice ),
				"Failed to query the DXGI device used by Windows Graphics Capture." );
			ComPtr<IInspectable> inspectableDevice;
			ThrowIfFailed(
				CreateDirect3D11DeviceFromDXGIDevice(
					dxgiDevice.Get(), inspectableDevice.GetAddressOf() ),
				"Failed to create the Windows Runtime capture device." );
			captureDevice_ =
				{ inspectableDevice.Detach(), winrt::take_ownership_from_abi };
			SetWindowTextW( statusWindow_, L"LightD3D12 Desktop Retro Overlay | preparing native capture session" );
			Recreate( renderDevice, monitor );
			SetWindowTextW( statusWindow_, L"LightD3D12 Desktop Retro Overlay | waiting for capture frame" );
		}

		void Recreate( RenderDevice& renderDevice, HMONITOR monitor )
		{
			if( framePool_ && frameArrivedRegistrationActive_ )
			{
				framePool_.FrameArrived( frameArrivedToken_ );
				frameArrivedRegistrationActive_ = false;
			}
			{
				std::scoped_lock lock( pendingFrameMutex_ );
				pendingCaptureTexture_.Reset();
			}
			if( captureSession_ )
			{
				captureSession_.Close();
			}
			if( framePool_ )
			{
				framePool_.Close();
			}
			captureItem_ = nullptr;
			DestroySharedTextures( renderDevice );

			SetWindowTextW( statusWindow_, L"LightD3D12 Desktop Retro Overlay | selecting monitor for native capture" );
			const auto captureItemInterop = winrt::get_activation_factory<
				winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
				IGraphicsCaptureItemInterop>();
			ThrowIfFailed(
				captureItemInterop->CreateForMonitor(
					monitor,
					winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
					winrt::put_abi( captureItem_ ) ),
				"Windows Graphics Capture could not create a monitor capture item." );

			const winrt::Windows::Graphics::SizeInt32 size = captureItem_.Size();
			const uint32_t width = static_cast<uint32_t>( size.Width );
			const uint32_t height = static_cast<uint32_t>( size.Height );
			if( width == 0 || height == 0 )
			{
				throw std::runtime_error( "The selected monitor has an invalid size." );
			}

			SetWindowTextW( statusWindow_, L"LightD3D12 Desktop Retro Overlay | creating shared textures" );
			CreateSharedTextures( renderDevice, width, height );
			width_ = width;
			height_ = height;

			SetWindowTextW( statusWindow_, L"LightD3D12 Desktop Retro Overlay | creating native frame pool" );
			framePool_ = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
				captureDevice_,
				winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
				2,
				size );
			frameArrivedToken_ = framePool_.FrameArrived(
				[this]( const auto& sender, const auto& ) noexcept
				{
					try
					{
						auto frame = sender.TryGetNextFrame();
						if( !frame )
						{
							return;
						}

						const winrt::Windows::Graphics::SizeInt32 frameSize = frame.ContentSize();
						if( frameSize.Width != static_cast<int32_t>( width_ ) ||
							frameSize.Height != static_cast<int32_t>( height_ ) )
						{
							pendingResize_.store( true, std::memory_order_release );
							return;
						}

						ComPtr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>
							dxgiInterfaceAccess;
						auto surface = frame.Surface();
						ThrowIfFailed(
							reinterpret_cast<IInspectable*>( winrt::get_abi( surface ) )->QueryInterface(
								IID_PPV_ARGS( dxgiInterfaceAccess.GetAddressOf() ) ),
							"Windows Graphics Capture returned a surface without DXGI access." );

						ComPtr<ID3D11Texture2D> capturedTexture;
						ThrowIfFailed(
							dxgiInterfaceAccess->GetInterface(
								IID_PPV_ARGS( capturedTexture.GetAddressOf() ) ),
							"Windows Graphics Capture did not return a D3D11 texture." );

						// A Direct3D11CaptureFrame must leave this callback before the frame-pool
						// slot is reusable. Keep only its underlying D3D texture, as ShaderGlass
						// does, rather than retaining the WinRT frame object across callbacks.
						std::scoped_lock lock( pendingFrameMutex_ );
						pendingCaptureTexture_ = std::move( capturedTexture );
						arrivedFrameCount_.fetch_add( 1, std::memory_order_relaxed );
					}
					catch( ... )
					{
						// A capture callback must never propagate a WinRT exception.
					}
				} );
			frameArrivedRegistrationActive_ = true;
			SetWindowTextW( statusWindow_, L"LightD3D12 Desktop Retro Overlay | creating native capture session" );
			captureSession_ = framePool_.CreateCaptureSession( captureItem_ );
			if( const auto session2 = captureSession_.try_as<
				winrt::Windows::Graphics::Capture::IGraphicsCaptureSession2>() )
			{
				session2.IsCursorCaptureEnabled( false );
			}
			if( const auto session3 = captureSession_.try_as<
				winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>() )
			{
				try
				{
					session3.IsBorderRequired( false );
				}
				catch( ... )
				{
					// Windows may refuse this request on older builds or protected content.
				}
			}
			SetWindowTextW( statusWindow_, L"LightD3D12 Desktop Retro Overlay | starting native capture" );
			captureSession_.StartCapture();
			SetWindowTextW( statusWindow_, L"LightD3D12 Desktop Retro Overlay | native capture started" );
		}

		UpdateResult Update( RenderDevice& renderDevice )
		{
			if( pendingResize_.exchange( false, std::memory_order_acq_rel ) )
			{
				return UpdateResult::AccessLost;
			}

			ComPtr<ID3D11Texture2D> capturedTexture;
			{
				std::scoped_lock lock( pendingFrameMutex_ );
				if( !pendingCaptureTexture_ )
				{
					return UpdateResult::NoNewFrame;
				}
				capturedTexture = std::move( pendingCaptureTexture_ );
			}

			// Never stall the desktop path waiting for a texture that D3D12 is still
			// presenting. Use the next idle texture, or drop this frame and keep the
			// latest displayed one. That prevents capture latency from accumulating.
			SharedCaptureTexture* destination = nullptr;
			uint32_t destinationIndex = kNoTexture;
			for( uint32_t attempt = 0; attempt < sharedTextures_.size(); ++attempt )
			{
				const uint32_t candidateIndex = ( nextWriteTexture_ + attempt ) % sharedTextures_.size();
				SharedCaptureTexture& candidate = sharedTextures_[ candidateIndex ];
				if( !IsReadyForD3D11Write( candidate ) )
				{
					continue;
				}

				const HRESULT acquireResult = candidate.keyedMutex->AcquireSync( 0, 0 );
				if( acquireResult == static_cast<HRESULT>( WAIT_TIMEOUT ) )
				{
					continue;
				}
				ThrowIfFailed( acquireResult, "Failed to acquire the shared desktop capture texture." );
				destination = &candidate;
				destinationIndex = candidateIndex;
				break;
			}
			if( destination == nullptr )
			{
				return UpdateResult::NoNewFrame;
			}

			d3d11Context_->CopyResource( destination->d3d11Texture.Get(), capturedTexture.Get() );
			const uint64_t captureFenceValue = nextCaptureFenceValue_++;
			ThrowIfFailed(
				d3d11Context4_->Signal( captureFence11_.Get(), captureFenceValue ),
				"Failed to signal that a captured desktop frame is ready." );
			d3d11Context_->Flush();
			ThrowIfFailed(
				destination->keyedMutex->ReleaseSync( 0 ),
				"Failed to release the shared desktop capture texture." );
			ThrowIfFailed(
				renderDevice.GetNativeCommandQueue()->Wait( captureFence12_.Get(), captureFenceValue ),
				"Direct3D 12 could not wait for the captured desktop frame." );
			currentTexture_ = destinationIndex;
			nextWriteTexture_ = ( destinationIndex + 1u ) % sharedTextures_.size();
			copiedFrameCount_.fetch_add( 1, std::memory_order_relaxed );
			return UpdateResult::FrameCopied;
		}

		void MarkCurrentTextureSubmitted( RenderDevice& renderDevice )
		{
			if( currentTexture_ == kNoTexture )
			{
				return;
			}

			SharedCaptureTexture& texture = sharedTextures_[ currentTexture_ ];
			const uint64_t fenceValue = nextRenderFenceValue_++;
			ThrowIfFailed(
				renderDevice.GetNativeCommandQueue()->Signal( renderFence_.Get(), fenceValue ),
				"Failed to signal the desktop texture synchronization fence." );
			texture.lastD3D12UseFenceValue = fenceValue;
		}

		void Shutdown( RenderDevice& renderDevice )
		{
			if( framePool_ && frameArrivedRegistrationActive_ )
			{
				framePool_.FrameArrived( frameArrivedToken_ );
				frameArrivedRegistrationActive_ = false;
			}
			{
				std::scoped_lock lock( pendingFrameMutex_ );
				pendingCaptureTexture_.Reset();
			}
			if( captureSession_ )
			{
				captureSession_.Close();
			}
			if( framePool_ )
			{
				framePool_.Close();
			}
			captureSession_ = nullptr;
			framePool_ = nullptr;
			captureItem_ = nullptr;
			DestroySharedTextures( renderDevice );
			captureFence12_.Reset();
			captureFence11_.Reset();
			d3d11Context4_.Reset();
			d3d11Device5_.Reset();
			d3d11Context_.Reset();
			d3d11Device_.Reset();
			captureDevice_ = nullptr;
			renderFence_.Reset();
			if( renderFenceEvent_ != nullptr )
			{
				CloseHandle( renderFenceEvent_ );
				renderFenceEvent_ = nullptr;
			}
			width_ = 0;
			height_ = 0;
		}

		TextureHandle GetTexture() const noexcept
		{
			return currentTexture_ == kNoTexture ? TextureHandle{} : sharedTextures_[ currentTexture_ ].d3d12Texture;
		}
		uint32_t GetWidth() const noexcept { return width_; }
		uint32_t GetHeight() const noexcept { return height_; }
		uint64_t GetArrivedFrameCount() const noexcept { return arrivedFrameCount_.load( std::memory_order_relaxed ); }
		uint64_t GetCopiedFrameCount() const noexcept { return copiedFrameCount_.load( std::memory_order_relaxed ); }

	private:
		static constexpr uint32_t kSharedTextureCount = 3;
		static constexpr uint32_t kNoTexture = UINT32_MAX;

		struct SharedCaptureTexture final
		{
			ComPtr<ID3D11Texture2D> d3d11Texture;
			ComPtr<IDXGIKeyedMutex> keyedMutex;
			TextureHandle d3d12Texture = {};
			uint64_t lastD3D12UseFenceValue = 0;
		};

		void CreateSharedTextures( RenderDevice& renderDevice, uint32_t width, uint32_t height )
		{
			TextureDesc importDesc{};
			importDesc.debugName = "DesktopRetroOverlay Shared Capture Texture";
			importDesc.width = width;
			importDesc.height = height;
			importDesc.format = DXGI_FORMAT_B8G8R8A8_UNORM;
			importDesc.usage = TextureUsage::Sampled;
			// Direct3D 11 owns every write; COMMON is the hand-off state between APIs.
			importDesc.initialState = D3D12_RESOURCE_STATE_COMMON;

			for( SharedCaptureTexture& texture : sharedTextures_ )
			{
				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = width;
				desc.Height = height;
				desc.MipLevels = 1;
				desc.ArraySize = 1;
				desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				desc.SampleDesc.Count = 1;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
					D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
				ThrowIfFailed(
					d3d11Device_->CreateTexture2D( &desc, nullptr, texture.d3d11Texture.GetAddressOf() ),
					"Failed to create a shared desktop capture texture." );
				ThrowIfFailed(
					texture.d3d11Texture.As( &texture.keyedMutex ),
					"Failed to query the shared desktop texture mutex." );

				ComPtr<IDXGIResource1> dxgiTexture;
				ThrowIfFailed(
					texture.d3d11Texture.As( &dxgiTexture ),
					"Failed to query the shared desktop texture." );
				HANDLE sharedHandle = nullptr;
				ThrowIfFailed(
					dxgiTexture->CreateSharedHandle(
						nullptr,
						DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
						nullptr,
						&sharedHandle ),
					"Failed to create a shared handle for the desktop capture texture." );

				ComPtr<ID3D12Resource> d3d12Texture;
				const HRESULT openResult = renderDevice.GetNativeDevice()->OpenSharedHandle(
					sharedHandle, IID_PPV_ARGS( d3d12Texture.GetAddressOf() ) );
				CloseHandle( sharedHandle );
				ThrowIfFailed( openResult, "Direct3D 12 could not open the shared desktop capture texture." );
				texture.d3d12Texture = renderDevice.ImportTexture( d3d12Texture.Get(), importDesc );
			}
			currentTexture_ = kNoTexture;
			nextWriteTexture_ = 0;
		}

		void DestroySharedTextures( RenderDevice& renderDevice )
		{
			for( SharedCaptureTexture& texture : sharedTextures_ )
			{
				if( texture.d3d12Texture.Valid() )
				{
					renderDevice.Destroy( texture.d3d12Texture );
					texture.d3d12Texture = {};
				}
				texture.d3d11Texture.Reset();
				texture.keyedMutex.Reset();
				texture.lastD3D12UseFenceValue = 0;
			}
			currentTexture_ = kNoTexture;
			nextWriteTexture_ = 0;
		}

		bool IsReadyForD3D11Write( const SharedCaptureTexture& texture ) const noexcept
		{
			return texture.lastD3D12UseFenceValue == 0 ||
				renderFence_->GetCompletedValue() >= texture.lastD3D12UseFenceValue;
		}

		ComPtr<ID3D11Device> d3d11Device_;
		ComPtr<ID3D11DeviceContext> d3d11Context_;
		ComPtr<ID3D11Device5> d3d11Device5_;
		ComPtr<ID3D11DeviceContext4> d3d11Context4_;
		ComPtr<ID3D11Fence> captureFence11_;
		ComPtr<ID3D12Fence> captureFence12_;
		ComPtr<ID3D12Fence> renderFence_;
		HANDLE renderFenceEvent_ = nullptr;
		winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice captureDevice_ = nullptr;
		winrt::Windows::Graphics::Capture::GraphicsCaptureItem captureItem_ = nullptr;
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool_ = nullptr;
		winrt::Windows::Graphics::Capture::GraphicsCaptureSession captureSession_ = nullptr;
		winrt::event_token frameArrivedToken_ = {};
		std::mutex pendingFrameMutex_;
		ComPtr<ID3D11Texture2D> pendingCaptureTexture_;
		std::atomic_uint64_t arrivedFrameCount_ = 0;
		std::atomic_bool pendingResize_ = false;
		bool frameArrivedRegistrationActive_ = false;
		std::array<SharedCaptureTexture, kSharedTextureCount> sharedTextures_ = {};
		uint32_t currentTexture_ = kNoTexture;
		uint32_t nextWriteTexture_ = 0;
		uint64_t nextCaptureFenceValue_ = 1;
		uint64_t nextRenderFenceValue_ = 1;
		uint32_t width_ = 0;
		uint32_t height_ = 0;
		std::atomic_uint64_t copiedFrameCount_ = 0;
		HWND statusWindow_ = nullptr;
	};

	struct NewPixieTargets
	{
		std::array<TextureHandle, 2> history = {};
		TextureHandle blurHorizontal = {};
		TextureHandle blurVertical = {};
		uint32_t historyReadIndex = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		bool historyValid = false;
	};

	struct AppState
	{
		HWND hwnd = nullptr;
		HWND settingsHwnd = nullptr;
		HMONITOR monitor = nullptr;
		DeviceManager* deviceManager = nullptr;
		DesktopDuplicator desktopDuplicator;
		RenderPipelineState presentPipeline;
		RenderPipelineState newPixieAccumulatePipeline;
		RenderPipelineState newPixieBlurPipeline;
		NewPixieTargets newPixieTargets;
		bool running = true;
		bool overlayVisible = true;
		bool requestCaptureRecreate = false;
		float elapsedSeconds = 0.0f;
		RetroPushConstants settings{};
	};

	RenderPipelineState CreateRetroPipeline( RenderDevice& renderDevice, DXGI_FORMAT backbufferFormat )
	{
		RenderPipelineDesc desc{};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/DesktopRetroOverlayVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( "shaders/DesktopRetroOverlayPS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = backbufferFormat;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		return renderDevice.CreateRenderPipeline( desc );
	}

	RenderPipelineState CreateNewPixiePipeline(
		RenderDevice& renderDevice, DXGI_FORMAT renderTargetFormat, const char* fragmentShaderPath )
	{
		RenderPipelineDesc desc{};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/DesktopRetroOverlayVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( fragmentShaderPath, "ps_6_6" );
		desc.color[ 0 ].format = renderTargetFormat;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		return renderDevice.CreateRenderPipeline( desc );
	}

	void DestroyNewPixieTargets( RenderDevice& renderDevice, NewPixieTargets& targets )
	{
		for( TextureHandle& historyTexture : targets.history )
		{
			if( historyTexture.Valid() )
			{
				renderDevice.Destroy( historyTexture );
				historyTexture = {};
			}
		}
		if( targets.blurHorizontal.Valid() )
		{
			renderDevice.Destroy( targets.blurHorizontal );
			targets.blurHorizontal = {};
		}
		if( targets.blurVertical.Valid() )
		{
			renderDevice.Destroy( targets.blurVertical );
			targets.blurVertical = {};
		}
		targets.historyReadIndex = 0;
		targets.width = 0;
		targets.height = 0;
		targets.historyValid = false;
	}

	void RecreateNewPixieTargets(
		RenderDevice& renderDevice, NewPixieTargets& targets, uint32_t width, uint32_t height, DXGI_FORMAT format )
	{
		if( targets.history[ 0 ].Valid() && targets.history[ 1 ].Valid() &&
			targets.blurHorizontal.Valid() && targets.blurVertical.Valid() &&
			targets.width == width && targets.height == height )
		{
			return;
		}

		DestroyNewPixieTargets( renderDevice, targets );
		TextureDesc desc{};
		desc.width = width;
		desc.height = height;
		desc.format = format;
		desc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
		desc.useClearValue = true;
		desc.clearValue.Format = format;
		desc.clearValue.Color[ 0 ] = 0.0f;
		desc.clearValue.Color[ 1 ] = 0.0f;
		desc.clearValue.Color[ 2 ] = 0.0f;
		desc.clearValue.Color[ 3 ] = 1.0f;

		desc.debugName = "DesktopRetroOverlay NewPixie History A";
		targets.history[ 0 ] = renderDevice.CreateTexture( desc );
		desc.debugName = "DesktopRetroOverlay NewPixie History B";
		targets.history[ 1 ] = renderDevice.CreateTexture( desc );
		desc.debugName = "DesktopRetroOverlay NewPixie Horizontal Blur";
		targets.blurHorizontal = renderDevice.CreateTexture( desc );
		desc.debugName = "DesktopRetroOverlay NewPixie Vertical Blur";
		targets.blurVertical = renderDevice.CreateTexture( desc );
		targets.width = width;
		targets.height = height;
	}

	TextureHandle RecordNewPixiePasses(
		RenderDevice& renderDevice, ICommandBuffer& commandBuffer, AppState& app, TextureHandle capturedDesktop )
	{
		NewPixieTargets& targets = app.newPixieTargets;
		const uint32_t historyReadIndex = targets.historyReadIndex;
		const uint32_t historyWriteIndex = 1u - historyReadIndex;
		const TextureHandle historyRead = targets.history[ historyReadIndex ];
		const TextureHandle historyWrite = targets.history[ historyWriteIndex ];

		RenderPass pass{};
		pass.color[ 0 ].loadOp = LoadOp::Clear;
		pass.color[ 0 ].clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		Framebuffer framebuffer{};

		commandBuffer.CmdTransitionTexture( capturedDesktop, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		commandBuffer.CmdTransitionTexture( historyRead, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		framebuffer.color[ 0 ].texture = historyWrite;
		NewPixieAccumulatePushConstants accumulateConstants{};
		accumulateConstants.sourceTextureIndex = renderDevice.GetBindlessIndex( capturedDesktop );
		accumulateConstants.historyTextureIndex = renderDevice.GetBindlessIndex( historyRead );
		accumulateConstants.persistence = targets.historyValid ? 0.38f : 0.0f;
		commandBuffer.CmdBeginRendering( pass, framebuffer );
		commandBuffer.CmdBindRenderPipeline( app.newPixieAccumulatePipeline );
		commandBuffer.CmdPushConstants( &accumulateConstants, sizeof( accumulateConstants ) );
		commandBuffer.CmdDraw( 3 );
		commandBuffer.CmdEndRendering();
		commandBuffer.CmdTransitionTexture( capturedDesktop, D3D12_RESOURCE_STATE_COMMON );
		commandBuffer.CmdTransitionTexture( historyWrite, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );

		NewPixieBlurPushConstants blurConstants{};
		blurConstants.sourceTextureIndex = renderDevice.GetBindlessIndex( historyWrite );
		blurConstants.stepX = 0.85f / static_cast<float>( targets.width );
		framebuffer.color[ 0 ].texture = targets.blurHorizontal;
		commandBuffer.CmdBeginRendering( pass, framebuffer );
		commandBuffer.CmdBindRenderPipeline( app.newPixieBlurPipeline );
		commandBuffer.CmdPushConstants( &blurConstants, sizeof( blurConstants ) );
		commandBuffer.CmdDraw( 3 );
		commandBuffer.CmdEndRendering();
		commandBuffer.CmdTransitionTexture( targets.blurHorizontal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );

		blurConstants.sourceTextureIndex = renderDevice.GetBindlessIndex( targets.blurHorizontal );
		blurConstants.stepX = 0.0f;
		blurConstants.stepY = 0.85f / static_cast<float>( targets.height );
		framebuffer.color[ 0 ].texture = targets.blurVertical;
		commandBuffer.CmdBeginRendering( pass, framebuffer );
		commandBuffer.CmdBindRenderPipeline( app.newPixieBlurPipeline );
		commandBuffer.CmdPushConstants( &blurConstants, sizeof( blurConstants ) );
		commandBuffer.CmdDraw( 3 );
		commandBuffer.CmdEndRendering();
		commandBuffer.CmdTransitionTexture( targets.blurVertical, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );

		targets.historyReadIndex = historyWriteIndex;
		targets.historyValid = true;
		return historyWrite;
	}

	void PositionOverlay( AppState& app )
	{
		app.monitor = GetPrimaryMonitor();
		const RECT bounds = GetMonitorRect( app.monitor );
		SetWindowPos(
			app.hwnd,
			HWND_TOPMOST,
			bounds.left,
			bounds.top,
			bounds.right - bounds.left,
			bounds.bottom - bounds.top,
			SWP_NOACTIVATE | SWP_SHOWWINDOW );
	}

	void SetOverlayVisible( AppState& app, bool visible )
	{
		app.overlayVisible = visible;
		if( visible )
		{
			PositionOverlay( app );
		}
		else
		{
			ShowWindow( app.hwnd, SW_HIDE );
		}
	}

	void ApplyPreset( AppState& app, uint32_t mode )
	{
		app.settings.mode = mode;
		app.newPixieTargets.historyValid = false;
			switch( mode )
			{
				case 1:
					app.settings.intensity = 0.84f;
					app.settings.curvature = 0.010f;
					app.settings.scanlineStrength = 0.20f;
					app.settings.chromaticAberrationPixels = 0.05f;
					app.settings.noiseStrength = 0.010f;
					break;

				case 2:
					app.settings.intensity = 0.90f;
					app.settings.curvature = 0.012f;
					app.settings.scanlineStrength = 0.32f;
					app.settings.chromaticAberrationPixels = 0.04f;
					app.settings.noiseStrength = 0.016f;
					break;

			case 3:
				app.settings.intensity = 1.00f;
				app.settings.curvature = 0.032f;
				app.settings.scanlineStrength = 0.48f;
				app.settings.chromaticAberrationPixels = 0.65f;
				app.settings.noiseStrength = 0.0f;
				break;

			case 4:
				app.settings.intensity = 1.00f;
				app.settings.curvature = 0.038f;
				app.settings.scanlineStrength = 0.58f;
				app.settings.chromaticAberrationPixels = 0.0f;
				app.settings.noiseStrength = 0.0f;
				break;

				default:
					app.settings.intensity = 0.94f;
					app.settings.curvature = 0.015f;
					app.settings.scanlineStrength = 0.32f;
					app.settings.chromaticAberrationPixels = 0.20f;
					app.settings.noiseStrength = 0.025f;
				break;
		}
	}

	void ShowSettingsPanel( AppState& app )
	{
		if( app.settingsHwnd == nullptr || !IsWindow( app.settingsHwnd ) )
		{
			return;
		}
		SetWindowPos( app.settingsHwnd, HWND_TOPMOST, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW );
		SetForegroundWindow( app.settingsHwnd );
	}

	void ExecuteTrayCommand( AppState& app, UINT command )
	{
		switch( command )
		{
			case TrayToggle:
				SetOverlayVisible( app, !app.overlayVisible );
				break;
			case TrayCrt:
				ApplyPreset( app, 0 );
				break;
			case TrayPs2:
				ApplyPreset( app, 3 );
				break;
			case TrayNewPixie:
				ApplyPreset( app, 4 );
				break;
			case TrayAmber:
				ApplyPreset( app, 1 );
				break;
			case TrayGreen:
				ApplyPreset( app, 2 );
				break;
			case TraySettings:
				ShowSettingsPanel( app );
				break;
			case TrayQuit:
				app.running = false;
				break;
		}
	}

	void AddTrayIcon( HWND hwnd )
	{
		NOTIFYICONDATAW trayIcon{};
		trayIcon.cbSize = sizeof( trayIcon );
		trayIcon.hWnd = hwnd;
		trayIcon.uID = kTrayIconId;
		trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
		trayIcon.uCallbackMessage = kTrayCallbackMessage;
		trayIcon.hIcon = LoadIconW( nullptr, IDI_APPLICATION );
		wcsncpy_s( trayIcon.szTip, L"LightD3D12 Desktop Retro Overlay", _TRUNCATE );
		if( !Shell_NotifyIconW( NIM_ADD, &trayIcon ) )
		{
			throw std::runtime_error( "Failed to create the Desktop Retro Overlay tray icon." );
		}
	}

	void RemoveTrayIcon( HWND hwnd ) noexcept
	{
		NOTIFYICONDATAW trayIcon{};
		trayIcon.cbSize = sizeof( trayIcon );
		trayIcon.hWnd = hwnd;
		trayIcon.uID = kTrayIconId;
		Shell_NotifyIconW( NIM_DELETE, &trayIcon );
	}

	void ShowTrayMenu( AppState& app )
	{
		HMENU menu = CreatePopupMenu();
		if( menu == nullptr )
		{
			return;
		}

		AppendMenuW( menu, MF_STRING | ( app.overlayVisible ? MF_CHECKED : MF_UNCHECKED ),
			TrayToggle, L"Enable retro overlay" );
		AppendMenuW( menu, MF_SEPARATOR, 0, nullptr );
		AppendMenuW( menu, MF_STRING | ( app.settings.mode == 0 ? MF_CHECKED : MF_UNCHECKED ),
			TrayCrt, L"Simple CRTV" );
		AppendMenuW( menu, MF_STRING | ( app.settings.mode == 3 ? MF_CHECKED : MF_UNCHECKED ),
			TrayPs2, L"PS2 Clean (480i)" );
		AppendMenuW( menu, MF_STRING | ( app.settings.mode == 4 ? MF_CHECKED : MF_UNCHECKED ),
			TrayNewPixie, L"PS2 NewPixie CRT" );
		AppendMenuW( menu, MF_STRING | ( app.settings.mode == 1 ? MF_CHECKED : MF_UNCHECKED ),
			TrayAmber, L"Amber terminal" );
		AppendMenuW( menu, MF_STRING | ( app.settings.mode == 2 ? MF_CHECKED : MF_UNCHECKED ),
			TrayGreen, L"Green phosphor" );
		AppendMenuW( menu, MF_SEPARATOR, 0, nullptr );
		AppendMenuW( menu, MF_STRING, TraySettings, L"Open controls..." );
		AppendMenuW( menu, MF_STRING, TrayQuit, L"Quit" );

		POINT cursor{};
		GetCursorPos( &cursor );
		SetForegroundWindow( app.hwnd );
		const UINT command = TrackPopupMenu(
			menu,
			TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
			cursor.x,
			cursor.y,
			0,
			app.hwnd,
			nullptr );
		PostMessageW( app.hwnd, WM_NULL, 0, 0 );
		DestroyMenu( menu );
		ExecuteTrayCommand( app, command );
	}

	LRESULT CALLBACK SettingsWindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		if( message == WM_NCCREATE )
		{
			const auto* create = reinterpret_cast<const CREATESTRUCTW*>( lParam );
			SetWindowLongPtrW( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( create->lpCreateParams ) );
			return TRUE;
		}

		auto* app = reinterpret_cast<AppState*>( GetWindowLongPtrW( hwnd, GWLP_USERDATA ) );
		switch( message )
		{
			case WM_COMMAND:
				if( app == nullptr )
				{
					return 0;
				}
				switch( LOWORD( wParam ) )
				{
					case TrayToggle:
					case TrayCrt:
					case TrayPs2:
					case TrayNewPixie:
					case TrayAmber:
					case TrayGreen:
					case TrayQuit:
						ExecuteTrayCommand( *app, LOWORD( wParam ) );
						break;
					case SettingsIntensityMore:
						app->settings.intensity = std::min( 1.0f, app->settings.intensity + 0.05f );
						break;
					case SettingsIntensityLess:
						app->settings.intensity = std::max( 0.0f, app->settings.intensity - 0.05f );
						break;
					case SettingsHidePanel:
						ShowWindow( hwnd, SW_HIDE );
						break;
				}
				return 0;

			case WM_CLOSE:
				// Closing this panel keeps the desktop filter running; use "Quit overlay"
				// when the user wants to terminate the process.
				ShowWindow( hwnd, SW_HIDE );
				return 0;

			case WM_DESTROY:
				if( app != nullptr && app->settingsHwnd == hwnd )
				{
					app->settingsHwnd = nullptr;
				}
				return 0;

			default:
				return DefWindowProcW( hwnd, message, wParam, lParam );
		}
	}

	HWND CreateSettingsPanel( HINSTANCE instance, AppState& app )
	{
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( windowClass );
		windowClass.hInstance = instance;
		windowClass.lpfnWndProc = SettingsWindowProc;
		windowClass.hCursor = LoadCursorW( nullptr, IDC_ARROW );
		windowClass.hbrBackground = reinterpret_cast<HBRUSH>( COLOR_WINDOW + 1 );
		windowClass.lpszClassName = L"LightD3D12DesktopRetroOverlayControls";
		if( RegisterClassExW( &windowClass ) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS )
		{
			throw std::runtime_error( "Failed to register the retro overlay controls window." );
		}

		const RECT monitorBounds = GetMonitorRect( app.monitor );
		const int width = 340;
		const int height = 310;
		const int left = std::max( monitorBounds.left, monitorBounds.right - width - 36 );
		const int top = monitorBounds.top + 36;
		const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
		HWND settings = CreateWindowExW(
			WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
			windowClass.lpszClassName,
			L"Retro overlay controls",
			style | WS_VISIBLE,
			left,
			top,
			width,
			height,
			nullptr,
			nullptr,
			instance,
			&app );
		if( settings == nullptr )
		{
			throw std::runtime_error( "Failed to create the retro overlay controls window." );
		}

		CreateWindowW( L"STATIC", L"Retro post-processing", WS_CHILD | WS_VISIBLE,
			16, 14, 260, 24, settings, nullptr, instance, nullptr );
		CreateWindowW( L"STATIC", L"The filter remains click-through; use these controls at runtime.", WS_CHILD | WS_VISIBLE,
			16, 40, 305, 24, settings, nullptr, instance, nullptr );
		CreateWindowW( L"BUTTON", L"Simple CRTV", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			16, 74, 96, 30, settings, reinterpret_cast<HMENU>( static_cast<INT_PTR>( TrayCrt ) ), instance, nullptr );
		CreateWindowW( L"BUTTON", L"PS2 Clean", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			122, 74, 96, 30, settings, reinterpret_cast<HMENU>( static_cast<INT_PTR>( TrayPs2 ) ), instance, nullptr );
		CreateWindowW( L"BUTTON", L"NewPixie", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			228, 74, 96, 30, settings, reinterpret_cast<HMENU>( static_cast<INT_PTR>( TrayNewPixie ) ), instance, nullptr );
		CreateWindowW( L"BUTTON", L"Amber", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			16, 116, 150, 30, settings, reinterpret_cast<HMENU>( static_cast<INT_PTR>( TrayAmber ) ), instance, nullptr );
		CreateWindowW( L"BUTTON", L"Green", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			174, 116, 150, 30, settings, reinterpret_cast<HMENU>( static_cast<INT_PTR>( TrayGreen ) ), instance, nullptr );
		CreateWindowW( L"BUTTON", L"More intensity", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			16, 158, 150, 30, settings, reinterpret_cast<HMENU>( static_cast<INT_PTR>( SettingsIntensityMore ) ), instance, nullptr );
		CreateWindowW( L"BUTTON", L"Less intensity", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			174, 158, 150, 30, settings, reinterpret_cast<HMENU>( static_cast<INT_PTR>( SettingsIntensityLess ) ), instance, nullptr );
		CreateWindowW( L"BUTTON", L"Toggle overlay", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			16, 200, 150, 30, settings, reinterpret_cast<HMENU>( static_cast<INT_PTR>( TrayToggle ) ), instance, nullptr );
		CreateWindowW( L"BUTTON", L"Hide controls", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			174, 200, 150, 30, settings, reinterpret_cast<HMENU>( static_cast<INT_PTR>( SettingsHidePanel ) ), instance, nullptr );
		CreateWindowW( L"BUTTON", L"Quit overlay", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			16, 242, 308, 30, settings, reinterpret_cast<HMENU>( static_cast<INT_PTR>( TrayQuit ) ), instance, nullptr );
		return settings;
	}

	LRESULT CALLBACK WindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		auto* app = reinterpret_cast<AppState*>( GetWindowLongPtrW( hwnd, GWLP_USERDATA ) );

		switch( message )
		{
			case WM_NCHITTEST:
				return HTTRANSPARENT;

			case WM_ERASEBKGND:
				return 1;

			case WM_HOTKEY:
				if( app == nullptr )
				{
					return 0;
				}
				switch( static_cast<int>( wParam ) )
				{
					case HotkeyToggle:
						SetOverlayVisible( *app, !app->overlayVisible );
						break;
					case HotkeyCrt:
						ApplyPreset( *app, 0 );
						break;
					case HotkeyPs2:
						ApplyPreset( *app, 3 );
						break;
					case HotkeyNewPixie:
						ApplyPreset( *app, 4 );
						break;
					case HotkeyAmber:
						ApplyPreset( *app, 1 );
						break;
					case HotkeyGreen:
						ApplyPreset( *app, 2 );
						break;
					case HotkeyMoreIntensity:
						app->settings.intensity = std::min( 1.0f, app->settings.intensity + 0.05f );
						break;
					case HotkeyLessIntensity:
						app->settings.intensity = std::max( 0.0f, app->settings.intensity - 0.05f );
						break;
					case HotkeySettings:
						ShowSettingsPanel( *app );
						break;
					case HotkeyQuit:
						app->running = false;
						break;
				}
				return 0;

			case kTrayCallbackMessage:
				if( app != nullptr )
				{
					const UINT trayEvent = static_cast<UINT>( lParam );
					if( trayEvent == WM_LBUTTONUP )
					{
						SetOverlayVisible( *app, !app->overlayVisible );
					}
					else if( trayEvent == WM_RBUTTONUP || trayEvent == WM_CONTEXTMENU )
					{
						ShowTrayMenu( *app );
					}
				}
				return 0;

			case WM_DISPLAYCHANGE:
				if( app != nullptr )
				{
					app->requestCaptureRecreate = true;
					if( app->overlayVisible )
					{
						PositionOverlay( *app );
					}
				}
				return 0;

			case WM_SIZE:
				if( app != nullptr && app->deviceManager != nullptr )
				{
					const uint32_t width = LOWORD( lParam );
					const uint32_t height = HIWORD( lParam );
					if( width > 0 && height > 0 )
					{
						app->deviceManager->Resize( width, height );
					}
				}
				return 0;

			case WM_CLOSE:
				if( app != nullptr )
				{
					app->running = false;
				}
				return 0;

			case WM_DESTROY:
				PostQuitMessage( 0 );
				return 0;

			default:
				return DefWindowProcW( hwnd, message, wParam, lParam );
		}
	}

	void RegisterOverlayHotkeys( HWND hwnd ) noexcept
	{
		constexpr UINT modifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
		RegisterHotKey( hwnd, HotkeyToggle, modifiers, 'R' );
		RegisterHotKey( hwnd, HotkeyCrt, modifiers, '1' );
		RegisterHotKey( hwnd, HotkeyPs2, modifiers, '4' );
		RegisterHotKey( hwnd, HotkeyNewPixie, modifiers, '5' );
		RegisterHotKey( hwnd, HotkeyAmber, modifiers, '2' );
		RegisterHotKey( hwnd, HotkeyGreen, modifiers, '3' );
		RegisterHotKey( hwnd, HotkeyMoreIntensity, modifiers, VK_UP );
		RegisterHotKey( hwnd, HotkeyLessIntensity, modifiers, VK_DOWN );
		RegisterHotKey( hwnd, HotkeySettings, modifiers, 'P' );
		RegisterHotKey( hwnd, HotkeyQuit, modifiers, 'Q' );
	}

	void UnregisterOverlayHotkeys( HWND hwnd ) noexcept
	{
		for( int hotkey = HotkeyToggle; hotkey <= HotkeyQuit; ++hotkey )
		{
			UnregisterHotKey( hwnd, hotkey );
		}
	}
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE, PWSTR commandLine, int )
{
	try
	{
		const bool smokeTest = commandLine != nullptr &&
			wcsstr( commandLine, L"--smoke-test" ) != nullptr;
		winrt::init_apartment( winrt::apartment_type::multi_threaded );
		if( !winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported() )
		{
			throw std::runtime_error( "Windows Graphics Capture is not supported on this Windows installation." );
		}
		SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
		const HMONITOR primaryMonitor = GetPrimaryMonitor();
		const RECT primaryBounds = GetMonitorRect( primaryMonitor );

		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( windowClass );
		windowClass.hInstance = instance;
		windowClass.lpfnWndProc = WindowProc;
		windowClass.lpszClassName = L"LightD3D12DesktopRetroOverlayWindow";
		if( RegisterClassExW( &windowClass ) == 0 )
		{
			throw std::runtime_error( "Failed to register the desktop overlay window class." );
		}

		const DWORD extendedStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
			WS_EX_TRANSPARENT | WS_EX_LAYERED;
		HWND hwnd = CreateWindowExW(
			extendedStyle,
			windowClass.lpszClassName,
			L"LightD3D12 Desktop Retro Overlay",
			WS_POPUP,
			primaryBounds.left,
			primaryBounds.top,
			primaryBounds.right - primaryBounds.left,
			primaryBounds.bottom - primaryBounds.top,
			nullptr,
			nullptr,
			instance,
			nullptr );
		if( hwnd == nullptr )
		{
			throw std::runtime_error( "Failed to create the desktop overlay window." );
		}

		if( !SetLayeredWindowAttributes( hwnd, 0, 255, LWA_ALPHA ) ||
			!SetWindowDisplayAffinity( hwnd, kWindowDisplayAffinityExcludeFromCapture ) )
		{
			DestroyWindow( hwnd );
			throw std::runtime_error(
				"Windows could not exclude the overlay from capture. Windows 10 version 2004 or later is required to prevent capture feedback." );
		}

		AppState app{};
		app.hwnd = hwnd;
		app.monitor = primaryMonitor;
		SetWindowLongPtrW( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( &app ) );
		app.settingsHwnd = CreateSettingsPanel( instance, app );
		AddTrayIcon( hwnd );
		RegisterOverlayHotkeys( hwnd );

		ContextDesc contextDesc{};
		// Desktop capture has to run on the adapter that owns the monitor. On hybrid
		// laptops that is commonly the integrated adapter, not the dGPU chosen by the
		// high-performance preference.
		contextDesc.preferHighPerformanceAdapter = false;
		contextDesc.enableDebugLayer =
#if defined( _DEBUG )
			true;
#else
			false;
#endif
		contextDesc.swapchainBufferCount = 3;
		contextDesc.allowTearing = false;

		SwapchainDesc swapchainDesc{};
		swapchainDesc.window = MakeWin32WindowHandle( hwnd );
		swapchainDesc.width = static_cast<uint32_t>( primaryBounds.right - primaryBounds.left );
		swapchainDesc.height = static_cast<uint32_t>( primaryBounds.bottom - primaryBounds.top );
		swapchainDesc.vsync = true;

		app.deviceManager = &DeviceManager::Initialize( contextDesc, swapchainDesc );
		RenderDevice& renderDevice = *app.deviceManager->GetRenderDevice();
		LightHLSLLoader::SetRootDirectory( std::filesystem::path( __FILE__ ).parent_path() );
		app.presentPipeline = CreateRetroPipeline( renderDevice, contextDesc.swapchainFormat );
		app.newPixieAccumulatePipeline = CreateNewPixiePipeline(
			renderDevice, contextDesc.swapchainFormat, "shaders/NewPixieAccumulatePS.hlsl" );
		app.newPixieBlurPipeline = CreateNewPixiePipeline(
			renderDevice, contextDesc.swapchainFormat, "shaders/NewPixieBlurPS.hlsl" );
		app.desktopDuplicator.Initialize( renderDevice, app.monitor, hwnd );
		RecreateNewPixieTargets(
			renderDevice,
			app.newPixieTargets,
			app.desktopDuplicator.GetWidth(),
			app.desktopDuplicator.GetHeight(),
			contextDesc.swapchainFormat );

		// Windows Graphics Capture needs the UI queue to keep flowing during startup.
		// Keep the overlay transparent until the first valid frame avoids a black flash.
		SetLayeredWindowAttributes( hwnd, 0, 0, LWA_ALPHA );
		if( !smokeTest )
		{
			PositionOverlay( app );
		}
		bool hasFirstDesktopFrame = false;

		auto previousTime = std::chrono::steady_clock::now();
		auto nextTitleUpdate = previousTime;
		MSG message{};
		while( app.running )
		{
			while( PeekMessageW( &message, nullptr, 0, 0, PM_REMOVE ) )
			{
				if( message.message == WM_QUIT )
				{
					app.running = false;
					break;
				}
				TranslateMessage( &message );
				DispatchMessageW( &message );
			}

			if( !app.running )
			{
				break;
			}

			if( !app.overlayVisible )
			{
				WaitMessage();
				continue;
			}

			if( app.requestCaptureRecreate )
			{
				renderDevice.WaitIdle();
				PositionOverlay( app );
				app.desktopDuplicator.Recreate( renderDevice, app.monitor );
				RecreateNewPixieTargets(
					renderDevice,
					app.newPixieTargets,
					app.desktopDuplicator.GetWidth(),
					app.desktopDuplicator.GetHeight(),
					contextDesc.swapchainFormat );
				app.requestCaptureRecreate = false;
			}

			const DesktopDuplicator::UpdateResult updateResult = app.desktopDuplicator.Update( renderDevice );
			if( updateResult == DesktopDuplicator::UpdateResult::AccessLost )
			{
				renderDevice.WaitIdle();
				app.desktopDuplicator.Recreate( renderDevice, app.monitor );
				RecreateNewPixieTargets(
					renderDevice,
					app.newPixieTargets,
					app.desktopDuplicator.GetWidth(),
					app.desktopDuplicator.GetHeight(),
					contextDesc.swapchainFormat );
				continue;
			}
			if( updateResult == DesktopDuplicator::UpdateResult::FrameCopied && !hasFirstDesktopFrame )
			{
				hasFirstDesktopFrame = true;
				SetLayeredWindowAttributes( hwnd, 0, 255, LWA_ALPHA );
			}
			if( !hasFirstDesktopFrame )
			{
				Sleep( 1 );
				continue;
			}

			const auto now = std::chrono::steady_clock::now();
			const float deltaSeconds = std::clamp(
				std::chrono::duration<float>( now - previousTime ).count(), 0.0f, 0.05f );
			previousTime = now;
			app.elapsedSeconds += deltaSeconds;
			if( now >= nextTitleUpdate )
			{
				const uint64_t capturedFrames = app.desktopDuplicator.GetArrivedFrameCount();
				const uint64_t copiedFrames = app.desktopDuplicator.GetCopiedFrameCount();
				const std::wstring title = L"LightD3D12 Desktop Retro Overlay | captured: " +
					std::to_wstring( capturedFrames ) +
					L" | copied: " + std::to_wstring( copiedFrames );
				SetWindowTextW( hwnd, title.c_str() );
				std::ofstream statusFile( "DesktopRetroOverlay-status.txt", std::ios::trunc );
				statusFile << "captured=" << capturedFrames << " copied=" << copiedFrames;
				nextTitleUpdate = now + std::chrono::seconds( 1 );
			}

			const TextureHandle currentBackbuffer = renderDevice.GetCurrentSwapchainTexture();
			const TextureHandle capturedDesktop = app.desktopDuplicator.GetTexture();
			auto& commandBuffer = renderDevice.AcquireCommandBuffer();
			const bool newPixieMode = app.settings.mode == 4;
			const TextureHandle processedDesktop = newPixieMode
				? RecordNewPixiePasses( renderDevice, commandBuffer, app, capturedDesktop )
				: capturedDesktop;

			RenderPass pass{};
			pass.color[ 0 ].loadOp = LoadOp::Clear;
			pass.color[ 0 ].clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentBackbuffer;

			RetroPushConstants pushConstants = app.settings;
			pushConstants.sourceTextureIndex = renderDevice.GetBindlessIndex( processedDesktop );
			pushConstants.blurTextureIndex = newPixieMode
				? renderDevice.GetBindlessIndex( app.newPixieTargets.blurVertical )
				: pushConstants.sourceTextureIndex;
			pushConstants.time = app.elapsedSeconds;
			pushConstants.inverseWidth = 1.0f / static_cast<float>( app.desktopDuplicator.GetWidth() );
			pushConstants.inverseHeight = 1.0f / static_cast<float>( app.desktopDuplicator.GetHeight() );

			if( !newPixieMode )
			{
				commandBuffer.CmdTransitionTexture( capturedDesktop, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			}
			commandBuffer.CmdBeginRendering( pass, framebuffer );
			commandBuffer.CmdBindRenderPipeline( app.presentPipeline );
			commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
			commandBuffer.CmdDraw( 3 );
			commandBuffer.CmdEndRendering();
			if( !newPixieMode )
			{
				// COMMON is the required ownership hand-off state before D3D11 writes the next frame.
				commandBuffer.CmdTransitionTexture( capturedDesktop, D3D12_RESOURCE_STATE_COMMON );
			}
			renderDevice.Submit( commandBuffer, currentBackbuffer );
			app.desktopDuplicator.MarkCurrentTextureSubmitted( renderDevice );

			if( smokeTest )
			{
				app.running = false;
			}
		}

		if( app.settingsHwnd != nullptr && IsWindow( app.settingsHwnd ) )
		{
			DestroyWindow( app.settingsHwnd );
		}
		SetWindowLongPtrW( hwnd, GWLP_USERDATA, 0 );
		UnregisterOverlayHotkeys( hwnd );
		RemoveTrayIcon( hwnd );
		renderDevice.WaitIdle();
		app.desktopDuplicator.Shutdown( renderDevice );
		DestroyNewPixieTargets( renderDevice, app.newPixieTargets );
		app.newPixieBlurPipeline = {};
		app.newPixieAccumulatePipeline = {};
		app.presentPipeline = {};
		DeviceManager::ShutdownSingleton();
		app.deviceManager = nullptr;
		DestroyWindow( hwnd );
		winrt::uninit_apartment();
	}
	catch( const std::exception& exception )
	{
		std::ofstream diagnosticFile( "DesktopRetroOverlay-error.txt", std::ios::trunc );
		diagnosticFile << exception.what();
		DeviceManager::ShutdownSingleton();
		MessageBoxA( nullptr, exception.what(), "LightD3D12 Desktop Retro Overlay", MB_ICONERROR | MB_OK );
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
