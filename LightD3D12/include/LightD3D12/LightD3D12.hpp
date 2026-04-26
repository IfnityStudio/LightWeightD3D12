#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
	static constexpr uint32_t ourMaxColorAttachments = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;

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

	// Call this before creating DeviceManager if you want PIX GPU capture attach support.
	// When PIX support is disabled or the capturer DLL is unavailable, this safely returns false.
	bool TryLoadPixGpuCapturer() noexcept;
	bool IsPixGpuCapturerLoaded() noexcept;

	using TextureHandle = Handle<TextureResource>;
	using BufferHandle = Handle<BufferResource>;

	inline std::string BuildScopedCommandLabel( const char* functionSignature )
	{
		if( functionSignature == nullptr || functionSignature[ 0 ] == '\0' )
		{
			return {};
		}

		std::string_view signature( functionSignature );
		std::string_view functionName = signature;
		if( const size_t openParenthesis = signature.find( '(' ); openParenthesis != std::string_view::npos )
		{
			functionName = signature.substr( 0, openParenthesis );
		}

		while( !functionName.empty() && functionName.back() == ' ' )
		{
			functionName.remove_suffix( 1 );
		}

		if( const size_t lastSpace = functionName.rfind( ' ' ); lastSpace != std::string_view::npos )
		{
			functionName.remove_prefix( lastSpace + 1 );
		}

		return std::string( functionName );
	}

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
		std::array<ColorAttachmentDesc, ourMaxColorAttachments> color = {};
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
		std::array<ColorAttachment, ourMaxColorAttachments> color = {};
		DepthStencilAttachment depthStencil = {};
	};

	struct RenderPipelineColorAttachmentDesc
	{
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	};

	struct VertexInputElementDesc
	{
		std::string semanticName;
		uint32_t semanticIndex = 0;
		DXGI_FORMAT format = DXGI_FORMAT_R32G32B32_FLOAT;
		uint32_t inputSlot = 0;
		uint32_t alignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
		D3D12_INPUT_CLASSIFICATION inputClassification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		uint32_t instanceDataStepRate = 0;
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

		std::array<RenderPipelineColorAttachmentDesc, ourMaxColorAttachments> color = {};
		std::vector<VertexInputElementDesc> inputElements;
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

	struct ComputePipelineDesc
	{
		ShaderStageSource computeShader = {};
	};

	struct BufferDesc
	{
		enum class BufferType : uint8_t
		{
			Generic,
			VertexBuffer,
			IndexBuffer,
		};

		std::string debugName;
		uint64_t size = 0;
		uint32_t stride = 0;
		BufferType bufferType = BufferType::Generic;
		D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
		D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
		bool createShaderResourceView = false;
		bool rawShaderResourceView = false;
		const void* data = nullptr;
		uint64_t dataSize = 0;
	};

	enum class TextureUsage : uint32_t
	{
		None = 0,
		Sampled = 1u << 0,
		RenderTarget = 1u << 1,
		DepthStencil = 1u << 2,
		UnorderedAccess = 1u << 3,
	};

	constexpr TextureUsage operator|( TextureUsage lhs, TextureUsage rhs ) noexcept
	{
		return static_cast<TextureUsage>( static_cast<uint32_t>( lhs ) | static_cast<uint32_t>( rhs ) );
	}

	constexpr TextureUsage operator&( TextureUsage lhs, TextureUsage rhs ) noexcept
	{
		return static_cast<TextureUsage>( static_cast<uint32_t>( lhs ) & static_cast<uint32_t>( rhs ) );
	}

	inline TextureUsage& operator|=( TextureUsage& lhs, TextureUsage rhs ) noexcept
	{
		lhs = lhs | rhs;
		return lhs;
	}

	constexpr bool HasTextureUsage( TextureUsage usage, TextureUsage bit ) noexcept
	{
		return ( usage & bit ) != TextureUsage::None;
	}

	struct TextureDesc
	{
		std::string debugName;
		uint32_t width = 1;
		uint32_t height = 1;
		uint16_t mipLevels = 1;
		uint16_t depthOrArraySize = 1;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
		TextureUsage usage = TextureUsage::Sampled;
		D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
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
		bool enablePixGpuCapture = false;
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

	class ComputePipelineState
	{
	public:
		ComputePipelineState() = default;
		ComputePipelineState( ComputePipelineState&& other ) noexcept;
		ComputePipelineState& operator=( ComputePipelineState&& other ) noexcept;
		ComputePipelineState( const ComputePipelineState& ) = delete;
		ComputePipelineState& operator=( const ComputePipelineState& ) = delete;
		~ComputePipelineState() = default;

		bool Valid() const noexcept;

	private:
		friend class Context;
		friend class RenderDevice;
		friend class CommandBufferImpl;

		ComPtr<ID3D12PipelineState> pipelineState_;
	};

	class ICommandBuffer
	{
	public:
		virtual ~ICommandBuffer() = default;

		virtual void CmdBeginRendering( const RenderPass& renderPass, const Framebuffer& framebuffer ) = 0;
		virtual void CmdEndRendering() = 0;
		virtual void CmdTransitionTexture( TextureHandle texture, D3D12_RESOURCE_STATES newState ) = 0;
		virtual void CmdBindRenderPipeline( const RenderPipelineState& pipeline ) = 0;
		virtual void CmdBindComputePipeline( const ComputePipelineState& pipeline ) = 0;
		virtual void CmdBindVertexBuffer( BufferHandle buffer, uint32_t stride = 0, uint32_t offset = 0, uint32_t slot = 0 ) = 0;
		virtual void CmdBindIndexBuffer( BufferHandle buffer, DXGI_FORMAT format = DXGI_FORMAT_R32_UINT, uint32_t offset = 0 ) = 0;
		virtual void CmdPushConstants( const void* data, uint32_t sizeBytes, uint32_t offset32BitValues = 0 ) = 0;
		virtual void CmdPushDebugGroupLabel( const char* label, uint32_t color ) = 0;
		virtual void CmdPopDebugGroupLabel() = 0;
		virtual void CmdDraw( uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0 ) = 0;
		virtual void CmdDrawIndexed( uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0 ) = 0;
		virtual void CmdDrawIndexedIndirect( BufferHandle indirectBuffer, uint32_t drawCount, uint64_t byteOffset = 0 ) = 0;
		virtual void CmdDispatch( uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1 ) = 0;
		virtual ID3D12GraphicsCommandList* GetNativeGraphicsCommandList() = 0;
	};

	class ScopedCommandDebugGroup final
	{
	public:
		ScopedCommandDebugGroup( ICommandBuffer& commandBuffer, std::string label, uint32_t color = 0xff4cc9f0u ):
			commandBuffer_( &commandBuffer ),
			label_( std::move( label ) ),
			active_( !label_.empty() )
		{
			if( active_ )
			{
				commandBuffer_->CmdPushDebugGroupLabel( label_.c_str(), color );
			}
		}

		ScopedCommandDebugGroup( ICommandBuffer& commandBuffer, const char* label, uint32_t color = 0xff4cc9f0u ):
			ScopedCommandDebugGroup( commandBuffer, label != nullptr ? std::string( label ) : std::string{}, color )
		{
		}

		~ScopedCommandDebugGroup()
		{
			if( active_ )
			{
				commandBuffer_->CmdPopDebugGroupLabel();
			}
		}

		ScopedCommandDebugGroup( const ScopedCommandDebugGroup& ) = delete;
		ScopedCommandDebugGroup& operator=( const ScopedCommandDebugGroup& ) = delete;

	private:
		ICommandBuffer* commandBuffer_ = nullptr;
		std::string label_;
		bool active_ = false;
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
		ComputePipelineState CreateComputePipeline( const ComputePipelineDesc& desc );
		BufferHandle CreateBuffer( const BufferDesc& desc );
		TextureHandle CreateTexture( const TextureDesc& desc );
		uint32_t GetBindlessIndex( BufferHandle buffer ) const;
		uint32_t GetBindlessIndex( TextureHandle texture ) const;
		uint32_t GetUnorderedAccessIndex( TextureHandle texture ) const;
		ID3D12Device* GetNativeDevice() const noexcept;
		ID3D12Resource* GetNativeTextureResource( TextureHandle texture ) const;
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

#define LIGHTD3D12_DETAIL_CONCAT_INNER( a, b ) a##b
#define LIGHTD3D12_DETAIL_CONCAT( a, b ) LIGHTD3D12_DETAIL_CONCAT_INNER( a, b )
#if defined( _MSC_VER )
	#define LIGHTD3D12_DETAIL_FUNCTION_SIGNATURE __FUNCSIG__
#elif defined( __clang__ ) || defined( __GNUC__ )
	#define LIGHTD3D12_DETAIL_FUNCTION_SIGNATURE __PRETTY_FUNCTION__
#else
	#define LIGHTD3D12_DETAIL_FUNCTION_SIGNATURE __FUNCTION__
#endif
#define LIGHTD3D12_CMD_SCOPE( commandBuffer ) ::lightd3d12::ScopedCommandDebugGroup LIGHTD3D12_DETAIL_CONCAT( lightd3d12CmdScope_, __LINE__ )( commandBuffer, ::lightd3d12::BuildScopedCommandLabel( LIGHTD3D12_DETAIL_FUNCTION_SIGNATURE ) )
#define LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, label, color ) ::lightd3d12::ScopedCommandDebugGroup LIGHTD3D12_DETAIL_CONCAT( lightd3d12CmdScope_, __LINE__ )( commandBuffer, label, color )

