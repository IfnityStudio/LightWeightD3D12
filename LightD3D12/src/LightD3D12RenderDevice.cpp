#include "LightD3D12ManagerImpl.hpp"

#include "LightD3D12StagingDevice.hpp"
#include "LightD3D12Swapchain.hpp"

#include <dxcapi.h>
#include <d3dcompiler.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace lightd3d12
{
	namespace
	{
		struct CompiledShader final
		{
			ComPtr<ID3DBlob> d3dBlob_;
			ComPtr<IDxcBlob> dxcBlob_;

			D3D12_SHADER_BYTECODE Bytecode() const noexcept
			{
				if( dxcBlob_ != nullptr )
				{
					return { dxcBlob_->GetBufferPointer(), dxcBlob_->GetBufferSize() };
				}

				return { d3dBlob_->GetBufferPointer(), d3dBlob_->GetBufferSize() };
			}
		};

		bool IsShaderModel6Profile( const char* profile ) noexcept
		{
			return profile != nullptr && std::strstr( profile, "_6_" ) != nullptr;
		}

		std::wstring ToWide( const char* text )
		{
			if( text == nullptr )
			{
				return {};
			}

			const int size = MultiByteToWideChar( CP_UTF8, 0, text, -1, nullptr, 0 );
			if( size <= 0 )
			{
				return {};
			}

			std::wstring wide( static_cast<size_t>( size ), L'\0' );
			MultiByteToWideChar( CP_UTF8, 0, text, -1, wide.data(), size );
			if( !wide.empty() )
			{
				wide.pop_back();
			}
			return wide;
		}

		uint64_t StableHashString( std::string_view text ) noexcept
		{
			uint64_t hash = 14695981039346656037ull;
			for( const unsigned char character : text )
			{
				hash ^= character;
				hash *= 1099511628211ull;
			}

			return hash;
		}

		std::wstring SanitizeFileName( std::wstring text )
		{
			for( wchar_t& character : text )
			{
				switch( character )
				{
					case L'\\':
					case L'/':
					case L':':
					case L'*':
					case L'?':
					case L'"':
					case L'<':
					case L'>':
					case L'|':
					case L' ':
						character = L'_';
						break;

					default:
						break;
				}
			}

			return text;
		}

		std::filesystem::path GetShaderDebugOutputDirectory()
		{
			std::array<wchar_t, MAX_PATH> modulePath{};
			const DWORD length = GetModuleFileNameW( nullptr, modulePath.data(), static_cast<DWORD>( modulePath.size() ) );
			if( length == 0 )
			{
				return std::filesystem::current_path() / "ShaderPdbs";
			}

			return std::filesystem::path( modulePath.data(), modulePath.data() + length ).parent_path() / "ShaderPdbs";
		}

		void WriteBlobToFile( const std::filesystem::path& path, IDxcBlob* blob )
		{
			if( blob == nullptr )
			{
				return;
			}

			std::filesystem::create_directories( path.parent_path() );
			std::ofstream file( path, std::ios::binary | std::ios::trunc );
			if( !file )
			{
				throw std::runtime_error( "Failed to open shader PDB output file." );
			}

			file.write( reinterpret_cast<const char*>( blob->GetBufferPointer() ), static_cast<std::streamsize>( blob->GetBufferSize() ) );
			if( !file )
			{
				throw std::runtime_error( "Failed to write shader PDB output file." );
			}
		}

		DxcCreateInstanceProc LoadDxcCreateInstance()
		{
			static DxcCreateInstanceProc ourCreateInstance = []() -> DxcCreateInstanceProc
				{
					const std::array<std::filesystem::path, 4> ourCandidates = {
						std::filesystem::path( LR"(C:\Program Files\Microsoft PIX\2601.15\dxcompiler.dll)" ),
						std::filesystem::path( LR"(C:\Program Files\RenderDoc\plugins\d3d12\dxcompiler.dll)" ),
						std::filesystem::path( LR"(C:\Program Files\BraveSoftware\Brave-Browser\Application\147.1.89.137\dxcompiler.dll)" ),
						std::filesystem::current_path() / "dxcompiler.dll",
					};

					for( const auto& candidate : ourCandidates )
					{
						if( !std::filesystem::exists( candidate ) )
						{
							continue;
						}

						HMODULE module = LoadLibraryW( candidate.c_str() );
						if( module == nullptr )
						{
							continue;
						}

						auto* createInstance = reinterpret_cast<DxcCreateInstanceProc>( GetProcAddress( module, "DxcCreateInstance" ) );
						if( createInstance != nullptr )
						{
							return createInstance;
						}
					}

					return nullptr;
				}();

			if( ourCreateInstance == nullptr )
			{
				throw std::runtime_error( "Failed to locate dxcompiler.dll for DXC shader compilation." );
			}

			return ourCreateInstance;
		}

		CompiledShader CompileShader( const ShaderStageSource& stage, const char* defaultProfile )
		{
			const char* profile = stage.profile != nullptr ? stage.profile : defaultProfile;
			if( stage.source == nullptr || profile == nullptr )
			{
				throw std::runtime_error( "Shader source or profile is invalid." );
			}

			if( !IsShaderModel6Profile( profile ) )
			{
				CompiledShader compiledShader;
				ComPtr<ID3DBlob> errors;
				detail::ThrowIfFailed(
					D3DCompile(
						stage.source,
						std::strlen( stage.source ),
						nullptr,
						nullptr,
						nullptr,
						stage.entryPoint,
						profile,
						0,
						0,
						compiledShader.d3dBlob_.GetAddressOf(),
						errors.GetAddressOf() ),
					"Failed to compile shader with D3DCompile." );
				return compiledShader;
			}

			const auto createInstance = LoadDxcCreateInstance();

			ComPtr<IDxcUtils> utils;
			ComPtr<IDxcCompiler3> compiler;
			detail::ThrowIfFailed( createInstance( CLSID_DxcUtils, __uuidof( IDxcUtils ), reinterpret_cast<void**>( utils.GetAddressOf() ) ), "Failed to create IDxcUtils." );
			detail::ThrowIfFailed( createInstance( CLSID_DxcCompiler, __uuidof( IDxcCompiler3 ), reinterpret_cast<void**>( compiler.GetAddressOf() ) ), "Failed to create IDxcCompiler3." );

			ComPtr<IDxcIncludeHandler> includeHandler;
			detail::ThrowIfFailed( utils->CreateDefaultIncludeHandler( includeHandler.GetAddressOf() ), "Failed to create DXC include handler." );

			const std::wstring entryPoint = ToWide( stage.entryPoint != nullptr ? stage.entryPoint : "main" );
			const std::wstring targetProfile = ToWide( profile );
			const std::wstring sanitizedEntryPoint = SanitizeFileName( entryPoint );
			const std::wstring sanitizedProfile = SanitizeFileName( targetProfile );
			const uint64_t shaderHash = StableHashString( std::string_view( stage.source, std::strlen( stage.source ) ) ) ^
				StableHashString( std::string_view( stage.entryPoint != nullptr ? stage.entryPoint : "main" ) ) ^
				StableHashString( std::string_view( profile ) );
			const std::filesystem::path shaderDebugDirectory = GetShaderDebugOutputDirectory();
			const std::filesystem::path pdbPath =
				shaderDebugDirectory /
				( sanitizedEntryPoint + L"_" + sanitizedProfile + L"_" + std::to_wstring( shaderHash ) + L".pdb" );
			const std::wstring pdbPathWide = pdbPath.wstring();

			std::array<LPCWSTR, 12> arguments = {
				L"-E",
				entryPoint.c_str(),
				L"-T",
				targetProfile.c_str(),
				L"-HV",
				L"2021",
				DXC_ARG_WARNINGS_ARE_ERRORS,
				DXC_ARG_PACK_MATRIX_ROW_MAJOR,
				L"-Zi",
				L"-Fd",
				pdbPathWide.c_str(),
				DXC_ARG_DEBUG,
			};

			DxcBuffer sourceBuffer{};
			sourceBuffer.Ptr = stage.source;
			sourceBuffer.Size = std::strlen( stage.source );
			sourceBuffer.Encoding = DXC_CP_UTF8;

			ComPtr<IDxcResult> result;
			detail::ThrowIfFailed(
				compiler->Compile(
					&sourceBuffer,
					arguments.data(),
					static_cast<UINT32>( arguments.size() ),
					includeHandler.Get(),
					__uuidof( IDxcResult ),
					reinterpret_cast<void**>( result.GetAddressOf() ) ),
				"Failed to invoke DXC shader compilation." );

			HRESULT status = S_OK;
			detail::ThrowIfFailed( result->GetStatus( &status ), "Failed to query DXC compilation status." );
			if( FAILED( status ) )
			{
				ComPtr<IDxcBlobUtf8> errors;
				if( SUCCEEDED( result->GetOutput( DXC_OUT_ERRORS, __uuidof( IDxcBlobUtf8 ), reinterpret_cast<void**>( errors.GetAddressOf() ), nullptr ) ) && errors != nullptr && errors->GetStringLength() > 0 )
				{
					throw std::runtime_error( errors->GetStringPointer() );
				}

				throw std::runtime_error( "Failed to compile shader with DXC." );
			}

			CompiledShader compiledShader;
			detail::ThrowIfFailed(
				result->GetOutput( DXC_OUT_OBJECT, __uuidof( IDxcBlob ), reinterpret_cast<void**>( compiledShader.dxcBlob_.GetAddressOf() ), nullptr ),
				"Failed to retrieve DXC shader bytecode." );

			ComPtr<IDxcBlob> pdbBlob;
			if( SUCCEEDED( result->GetOutput( DXC_OUT_PDB, __uuidof( IDxcBlob ), reinterpret_cast<void**>( pdbBlob.GetAddressOf() ), nullptr ) ) && pdbBlob != nullptr )
			{
				WriteBlobToFile( pdbPath, pdbBlob.Get() );
			}

			return compiledShader;
		}

		TextureResource::ResolvedFormats ResolveTextureFormats( const TextureDesc& desc )
		{
			TextureResource::ResolvedFormats formats{};
			formats.resource_ = desc.format;

			const bool sampled = HasTextureUsage( desc.usage, TextureUsage::Sampled );
			const bool renderTarget = HasTextureUsage( desc.usage, TextureUsage::RenderTarget );
			const bool depthStencil = HasTextureUsage( desc.usage, TextureUsage::DepthStencil );
			const bool unorderedAccess = HasTextureUsage( desc.usage, TextureUsage::UnorderedAccess );

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

			formats.srv_ = sampled ? desc.format : DXGI_FORMAT_UNKNOWN;
			formats.rtv_ = renderTarget ? desc.format : DXGI_FORMAT_UNKNOWN;
			formats.uav_ = unorderedAccess ? desc.format : DXGI_FORMAT_UNKNOWN;
			return formats;
		}

		D3D12_RESOURCE_FLAGS ResolveTextureResourceFlags( TextureUsage usage ) noexcept
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

			if( HasTextureUsage( usage, TextureUsage::UnorderedAccess ) )
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

		const CompiledShader vertexShader = CompileShader( desc.vertexShader, "vs_5_1" );
		const CompiledShader fragmentShader = CompileShader( desc.fragmentShader, "ps_5_1" );

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = impl.rootSignature_.Get();
		psoDesc.VS = vertexShader.Bytecode();
		psoDesc.PS = fragmentShader.Bytecode();
		psoDesc.BlendState = desc.blendState;
		psoDesc.RasterizerState = desc.rasterizerState;
		psoDesc.DepthStencilState = desc.depthStencilState;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = desc.primitiveType;

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

		const CompiledShader computeShader = CompileShader( desc.computeShader, "cs_5_1" );

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

		TextureResource resource;
		resource.width_ = desc.width;
		resource.height_ = desc.height;
		resource.format_ = desc.format;
		resource.formats_ = ResolveTextureFormats( desc );
		resource.usageFlags_ = ResolveTextureResourceFlags( desc.usage );
		resource.currentState_ = desc.initialState;
		resource.isDepthFormat_ = TextureResource::IsDepthFormat( resource.formats_.dsv_ );
		resource.isStencilFormat_ = TextureResource::IsDepthStencilFormat( resource.formats_.dsv_ );

		resource.desc_ = CD3DX12_RESOURCE_DESC::Tex2D(
			resource.formats_.resource_,
			desc.width,
			desc.height,
			desc.depthOrArraySize,
			desc.mipLevels,
			1,
			0,
			resource.usageFlags_ );

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

		if( HasTextureUsage( desc.usage, TextureUsage::Sampled ) )
		{
			resource.srvIndex_ = impl.AllocateBindlessDescriptor();
			resource.srvHandle_ = impl.bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
			resource.srvHandle_.ptr += static_cast<SIZE_T>( resource.srvIndex_ ) * impl.bindlessDescriptorSize_;

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Format = resource.formats_.srv_;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = desc.mipLevels;
			impl.device_->CreateShaderResourceView( resource.resource_.Get(), &srvDesc, resource.srvHandle_ );
		}

		if( HasTextureUsage( desc.usage, TextureUsage::UnorderedAccess ) )
		{
			resource.uavIndex_ = impl.AllocateBindlessDescriptor();
			resource.uavHandle_ = impl.bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
			resource.uavHandle_.ptr += static_cast<SIZE_T>( resource.uavIndex_ ) * impl.bindlessDescriptorSize_;

			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = resource.formats_.uav_;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
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
			impl.stagingDevice_->TextureSubData2D( impl.GetTextureResource( handle ), desc.data, desc.rowPitch, desc.slicePitch );
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
		impl.FreeRtvDescriptor( resource->rtvIndex_ );
		impl.FreeDsvDescriptor( resource->dsvIndex_ );
		resource->resource_.Reset();
		impl.slotMapTextures_.Destroy( texture );
	}
}


