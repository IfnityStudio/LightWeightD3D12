#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "LightD3D12/HandleSlotMap.hpp"

namespace lightd3d12
{
	using Microsoft::WRL::ComPtr;

	struct BufferResource;
	struct TextureResource;

	struct NativeWindowHandle
	{
		enum class Type
		{
			Win32Hwnd
		};

		Type type = Type::Win32Hwnd;
		void* value = nullptr;

		constexpr bool Valid() const noexcept
		{
			return value != nullptr;
		}
	};

	inline NativeWindowHandle MakeWin32WindowHandle( HWND hwnd ) noexcept
	{
		return { NativeWindowHandle::Type::Win32Hwnd, hwnd };
	}

	using TextureHandle = Handle<TextureResource>;
	using BufferHandle = Handle<BufferResource>;

	struct SubmitHandle
	{
		uint32_t bufferIndex_ = 0;
		uint32_t submitId_ = 0;

		SubmitHandle() = default;

		explicit SubmitHandle( uint64_t handle ) noexcept:
			bufferIndex_( static_cast< uint32_t >( handle & 0xffffffffu ) ),
			submitId_( static_cast< uint32_t >( handle >> 32u ) )
		{
		}

		constexpr bool Empty() const noexcept
		{
			return submitId_ == 0;
		}

		constexpr uint64_t Handle() const noexcept
		{
			return ( static_cast< uint64_t >( submitId_ ) << 32u ) + bufferIndex_;
		}
	};

	enum class LoadOp : uint8_t
	{
		Load,
		Clear,
		DontCare
	};

	enum class StoreOp : uint8_t
	{
		Store,
		DontCare
	};

	struct ColorAttachmentDesc
	{
		LoadOp loadOp = LoadOp::Load;
		StoreOp storeOp = StoreOp::Store;
		std::array<float, 4> clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
	};

	struct DepthStencilAttachmentDesc
	{
		LoadOp depthLoadOp = LoadOp::Load;
		StoreOp depthStoreOp = StoreOp::Store;
		LoadOp stencilLoadOp = LoadOp::Load;
		StoreOp stencilStoreOp = StoreOp::Store;
		float clearDepth = 1.0f;
		uint8_t clearStencil = 0;
	};

	struct RenderPass
	{
		std::array<ColorAttachmentDesc, 1> color = {};
		DepthStencilAttachmentDesc depthStencil = {};
	};

	struct ColorAttachment
	{
		TextureHandle texture = {};
	};

	struct DepthStencilAttachment
	{
		TextureHandle texture = {};
	};

	struct Framebuffer
	{
		std::array<ColorAttachment, 1> color = {};
		DepthStencilAttachment depthStencil = {};
	};

	struct ShaderStageSource
	{
		const char* source = nullptr;
		const char* entryPoint = "main";
		const char* profile = nullptr;
	};

	struct RenderPipelineDesc
	{
		RenderPipelineDesc() noexcept;

		ShaderStageSource vertexShader = {};
		ShaderStageSource fragmentShader = {};
		D3D12_BLEND_DESC blendState = {};
		D3D12_RASTERIZER_DESC rasterizerState = {};
		D3D12_DEPTH_STENCIL_DESC depthStencilState = {};
		D3D12_PRIMITIVE_TOPOLOGY_TYPE primitiveType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		DXGI_FORMAT colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
	};

	struct BufferDesc
	{
		std::string debugName;
		uint64_t size = 0;
		uint32_t stride = 0;
		D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
		D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
		bool createShaderResourceView = false;
		bool rawShaderResourceView = false;
		const void* data = nullptr;
		uint64_t dataSize = 0;
	};

	struct TextureDesc
	{
		std::string debugName;
		uint32_t width = 1;
		uint32_t height = 1;
		uint16_t mipLevels = 1;
		uint16_t depthOrArraySize = 1;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
		D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
		D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
		bool createShaderResourceView = true;
		bool createRenderTargetView = false;
		bool createDepthStencilView = false;
		const void* data = nullptr;
		uint32_t rowPitch = 0;
		uint32_t slicePitch = 0;
		bool useClearValue = false;
		D3D12_CLEAR_VALUE clearValue{};
	};

	struct ContextDesc
	{
		bool enableDebugLayer = true;
		bool preferHighPerformanceAdapter = true;
		bool allowTearing = true;
		uint32_t framesInFlight = 3;
		uint32_t bindlessCapacity = 4096;
		uint32_t rtvCapacity = 256;
		uint32_t dsvCapacity = 64;
		uint32_t swapchainBufferCount = 3;
		DXGI_FORMAT swapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		D3D_FEATURE_LEVEL minimumFeatureLevel = D3D_FEATURE_LEVEL_12_0;
	};

	struct SwapchainDesc
	{
		NativeWindowHandle window = {};
		uint32_t width = 1;
		uint32_t height = 1;
		bool vsync = true;
	};

	class RenderPipelineState
	{
	public:
		RenderPipelineState() = default;
		RenderPipelineState( RenderPipelineState&& other ) noexcept;
		RenderPipelineState& operator=( RenderPipelineState&& other ) noexcept;
		RenderPipelineState( const RenderPipelineState& ) = delete;
		RenderPipelineState& operator=( const RenderPipelineState& ) = delete;
		~RenderPipelineState() = default;

		bool Valid() const noexcept;

	private:
		friend class Context;
		friend class RenderDevice;
		friend class CommandBufferImpl;

		ComPtr<ID3D12PipelineState> pipelineState_;
		D3D_PRIMITIVE_TOPOLOGY topology_ = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	};

	class ICommandBuffer
	{
	public:
		virtual ~ICommandBuffer() = default;

		virtual void CmdBeginRendering( const RenderPass& renderPass, const Framebuffer& framebuffer ) = 0;
		virtual void CmdEndRendering() = 0;
		virtual void CmdBindRenderPipeline( const RenderPipelineState& pipeline ) = 0;
		virtual void CmdBindVertexBuffer( BufferHandle buffer, uint32_t stride = 0, uint32_t offset = 0 ) = 0;
		virtual void CmdBindIndexBuffer( BufferHandle buffer, DXGI_FORMAT format = DXGI_FORMAT_R32_UINT, uint32_t offset = 0 ) = 0;
		virtual void CmdPushConstants( const void* data, uint32_t sizeBytes, uint32_t offset32BitValues = 0 ) = 0;
		virtual void CmdPushDebugGroupLabel( const char* label, uint32_t color ) = 0;
		virtual void CmdPopDebugGroupLabel() = 0;
		virtual void CmdDraw( uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0 ) = 0;
		virtual void CmdDrawIndexedIndirect( BufferHandle indirectBuffer, uint32_t drawCount, uint64_t byteOffset = 0 ) = 0;
	};

	class DeviceManager;
	class ImguiRenderer;

	class RenderDevice
	{
	public:
		ICommandBuffer& AcquireCommandBuffer();
		TextureHandle GetCurrentSwapchainTexture() const;
		SubmitHandle Submit( ICommandBuffer& buffer, TextureHandle presentTexture );

		RenderPipelineState CreateRenderPipeline( const RenderPipelineDesc& desc );
		BufferHandle CreateBuffer( const BufferDesc& desc );
		TextureHandle CreateTexture( const TextureDesc& desc );
		uint32_t GetBindlessIndex( BufferHandle buffer ) const;
		uint32_t GetBindlessIndex( TextureHandle texture ) const;
		bool BindlessSupported() const noexcept;
		void WaitIdle();
		void Destroy( BufferHandle buffer );
		void Destroy( TextureHandle texture );

	private:
		friend class DeviceManager;

		explicit RenderDevice( DeviceManager& manager ) noexcept;

		DeviceManager* manager_ = nullptr;
	};

	class DeviceManager
	{
	public:
		class Impl;

		DeviceManager( const ContextDesc& desc, const SwapchainDesc& swapchainDesc );
		~DeviceManager();
		DeviceManager( DeviceManager&& ) = delete;
		DeviceManager& operator=( DeviceManager&& ) = delete;
		DeviceManager( const DeviceManager& ) = delete;
		DeviceManager& operator=( const DeviceManager& ) = delete;

		RenderDevice* GetRenderDevice() noexcept;
		const RenderDevice* GetRenderDevice() const noexcept;

		void Resize( uint32_t width, uint32_t height );
		uint32_t GetWidth() const noexcept;
		uint32_t GetHeight() const noexcept;
		bool IsVsyncEnabled() const noexcept;
		void SetVsync( bool enabled ) noexcept;
		void WaitIdle();

	private:
		friend class RenderDevice;
		friend class ImguiRenderer;
		std::unique_ptr<Impl> impl_;
		RenderDevice renderDevice_;
	};
}

