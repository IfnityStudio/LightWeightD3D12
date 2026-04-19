#include "LightD3D12/LightD3D12Imgui.hpp"

#include "LightD3D12CommandBuffer.hpp"
#include "LightD3D12Internal.hpp"
#include "LightD3D12ManagerImpl.hpp"

#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/backends/imgui_impl_dx12.h"
#include "../../third_party/imgui/backends/imgui_impl_win32.h"

#include <stdexcept>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam );

namespace lightd3d12
{
	struct ImguiRenderer::Impl final
	{
		Impl( DeviceManager& deviceManager, NativeWindowHandle window ):
			deviceManager_( deviceManager ),
			window_( window )
		{
			Initialize();
		}

		~Impl()
		{
			Shutdown();
		}

		void Initialize()
		{
			const HWND hwnd = detail::GetHwnd( window_ );
			auto& manager = *deviceManager_.impl_;

			IMGUI_CHECKVERSION();
			context_ = ImGui::CreateContext();
			if( context_ == nullptr )
			{
				throw std::runtime_error( "Failed to create Dear ImGui context." );
			}

			MakeCurrent();
			ImGuiIO& io = ImGui::GetIO();
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			ImGui::StyleColorsDark();

			if( !ImGui_ImplWin32_Init( hwnd ) )
			{
				ImGui::DestroyContext( context_ );
				context_ = nullptr;
				throw std::runtime_error( "Failed to initialize Dear ImGui Win32 backend." );
			}

			ImGui_ImplDX12_InitInfo initInfo{};
			initInfo.Device = manager.device_.Get();
			initInfo.CommandQueue = manager.commandQueue_.Get();
			initInfo.NumFramesInFlight = static_cast< int >( std::max( 1u, manager.desc_.framesInFlight ) );
			initInfo.RTVFormat = manager.desc_.swapchainFormat;
			initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
			initInfo.UserData = this;
			initInfo.SrvDescriptorHeap = manager.bindlessHeap_.Get();
			initInfo.SrvDescriptorAllocFn = &AllocateSrvDescriptor;
			initInfo.SrvDescriptorFreeFn = &FreeSrvDescriptor;

			if( !ImGui_ImplDX12_Init( &initInfo ) )
			{
				ImGui_ImplWin32_Shutdown();
				ImGui::DestroyContext( context_ );
				context_ = nullptr;
				throw std::runtime_error( "Failed to initialize Dear ImGui DX12 backend." );
			}

			initialized_ = true;
		}

		void Shutdown() noexcept
		{
			if( context_ == nullptr )
			{
				return;
			}

			if( deviceManager_.impl_ != nullptr )
			{
				try
				{
					deviceManager_.WaitIdle();
				}
				catch( ... )
				{
				}
			}

			MakeCurrent();
			if( initialized_ )
			{
				ImGui_ImplDX12_Shutdown();
				ImGui_ImplWin32_Shutdown();
				initialized_ = false;
			}

			ImGui::DestroyContext( context_ );
			context_ = nullptr;
		}

		void MakeCurrent() const noexcept
		{
			if( context_ != nullptr )
			{
				ImGui::SetCurrentContext( context_ );
			}
		}

		static void AllocateSrvDescriptor( ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuDescriptor, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuDescriptor )
		{
			auto* self = static_cast< Impl* >( info->UserData );
			auto& manager = *self->deviceManager_.impl_;

			const uint32_t descriptorIndex = manager.AllocateBindlessDescriptor();
			const SIZE_T descriptorOffset = static_cast< SIZE_T >( descriptorIndex ) * manager.bindlessDescriptorSize_;

			*outCpuDescriptor = manager.bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
			outCpuDescriptor->ptr += descriptorOffset;

			*outGpuDescriptor = manager.bindlessHeap_->GetGPUDescriptorHandleForHeapStart();
			outGpuDescriptor->ptr += descriptorOffset;
		}

		static void FreeSrvDescriptor( ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor, D3D12_GPU_DESCRIPTOR_HANDLE )
		{
			auto* self = static_cast< Impl* >( info->UserData );
			auto& manager = *self->deviceManager_.impl_;
			const D3D12_CPU_DESCRIPTOR_HANDLE heapStart = manager.bindlessHeap_->GetCPUDescriptorHandleForHeapStart();

			if( cpuDescriptor.ptr < heapStart.ptr || manager.bindlessDescriptorSize_ == 0 )
			{
				return;
			}

			const SIZE_T descriptorOffset = cpuDescriptor.ptr - heapStart.ptr;
			const uint32_t descriptorIndex = static_cast< uint32_t >( descriptorOffset / manager.bindlessDescriptorSize_ );
			manager.FreeBindlessDescriptor( descriptorIndex );
		}

		DeviceManager& deviceManager_;
		NativeWindowHandle window_ = {};
		ImGuiContext* context_ = nullptr;
		bool initialized_ = false;
	};

	ImguiRenderer::ImguiRenderer( DeviceManager& deviceManager, NativeWindowHandle window ):
		impl_( std::make_unique< Impl >( deviceManager, window ) )
	{
	}

	ImguiRenderer::~ImguiRenderer() = default;

	void ImguiRenderer::NewFrame()
	{
		impl_->MakeCurrent();
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
	}

	void ImguiRenderer::Render( ICommandBuffer& commandBuffer )
	{
		impl_->MakeCurrent();
		ImGui::Render();

		auto* commandBufferImpl = dynamic_cast< CommandBufferImpl* >( &commandBuffer );
		if( commandBufferImpl == nullptr )
		{
			throw std::runtime_error( "ImGui rendering requires a LightD3D12 command buffer." );
		}

		auto& manager = *impl_->deviceManager_.impl_;
		ID3D12DescriptorHeap* heaps[] = { manager.bindlessHeap_.Get() };
		commandBufferImpl->Wrapper().commandList_->SetDescriptorHeaps( 1, heaps );
		ImGui_ImplDX12_RenderDrawData( ImGui::GetDrawData(), commandBufferImpl->Wrapper().commandList_.Get() );
	}

	bool ImguiRenderer::ProcessMessage( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		impl_->MakeCurrent();
		return ImGui_ImplWin32_WndProcHandler( hwnd, message, wParam, lParam ) != 0;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE ImguiRenderer::GetTextureGpuDescriptor( TextureHandle texture ) const
	{
		auto& manager = *impl_->deviceManager_.impl_;
		const TextureResource& resource = manager.GetTextureResource( texture );
		if( resource.srvIndex_ == UINT32_MAX )
		{
			throw std::runtime_error( "Texture does not have an SRV descriptor." );
		}

		D3D12_GPU_DESCRIPTOR_HANDLE handle = manager.bindlessHeap_->GetGPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast< UINT64 >( resource.srvIndex_ ) * manager.bindlessDescriptorSize_;
		return handle;
	}
}
