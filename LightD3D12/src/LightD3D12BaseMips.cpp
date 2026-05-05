#include "LightD3D12BaseMips.hpp"

#include "LightD3D12ManagerImpl.hpp"
#include "LightD3D12Resources.hpp"
#include "LightD3D12ShaderCompiler.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace lightd3d12
{
	namespace
	{
		constexpr uint32_t ourThreadGroupSize = 8u;

		[[nodiscard]] bool IsSrgbFormat( DXGI_FORMAT format ) noexcept
		{
			return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		}

		[[nodiscard]] uint32_t CalculateSubresourceIndex( uint32_t mipSlice, uint16_t mipLevels ) noexcept
		{
			return D3D12CalcSubresource( mipSlice, 0, 0, mipLevels, 1 );
		}

		[[nodiscard]] std::string ReadTextFile( const std::filesystem::path& path )
		{
			std::ifstream file( path, std::ios::binary );
			if( !file )
			{
				throw std::runtime_error( "Failed to open BaseMips shader file: " + path.string() );
			}

			const std::string source( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
			if( source.size() >= 3 &&
				static_cast<unsigned char>( source[ 0 ] ) == 0xef &&
				static_cast<unsigned char>( source[ 1 ] ) == 0xbb &&
				static_cast<unsigned char>( source[ 2 ] ) == 0xbf )
			{
				return source.substr( 3 );
			}

			return source;
		}

		[[nodiscard]] const char* LoadBaseMipsShaderSource()
		{
			static const std::string source = []
			{
				const std::filesystem::path shaderPath =
					std::filesystem::path( __FILE__ ).parent_path() /
					"shaders" /
					"LightD3D12BaseMipsCS.hlsl";
				return ReadTextFile( shaderPath );
			}();

			return source.c_str();
		}
	}

	BaseMips::BaseMips( DeviceManager::Impl& manager ): manager_( manager )
	{
		ComputePipelineDesc pipelineDesc{};
		pipelineDesc.computeShader.source = LoadBaseMipsShaderSource();
		pipelineDesc.computeShader.entryPoint = "main";
		pipelineDesc.computeShader.profile = "cs_6_6";

		const CompiledShader shader = CompileShader( pipelineDesc.computeShader, "cs_6_6" );

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = manager_.rootSignature_.Get();
		psoDesc.CS = shader.Bytecode();

		detail::ThrowIfFailed(
			manager_.device_->CreateComputePipelineState( &psoDesc, IID_PPV_ARGS( pipelineState_.GetAddressOf() ) ),
			"Failed to create BaseMips compute pipeline state." );
	}

	void BaseMips::Generate( TextureResource& texture, D3D12_RESOURCE_STATES finalState )
	{
		if( texture.mipLevels_ <= 1 )
		{
			return;
		}

		if( texture.srvIndex_ == UINT32_MAX || texture.baseMipsUavBaseIndex_ == UINT32_MAX || texture.baseMipsUavCount_ == 0 )
		{
			throw std::runtime_error( "BaseMips requires a valid SRV and destination mip UAV range." );
		}

		auto& wrapper = manager_.immediateCommands_->Acquire();
		ID3D12GraphicsCommandList4* commandList = wrapper.commandList_.Get();

		ID3D12DescriptorHeap* descriptorHeaps[] = { manager_.bindlessHeap_.Get() };
		commandList->SetDescriptorHeaps( 1, descriptorHeaps );
		commandList->SetComputeRootSignature( manager_.rootSignature_.Get() );
		commandList->SetPipelineState( pipelineState_.Get() );

		TransitionSubresource(
			commandList,
			texture.resource_.Get(),
			CalculateSubresourceIndex( 0u, texture.mipLevels_ ),
			finalState,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );

		uint32_t sourceWidth = texture.width_;
		uint32_t sourceHeight = texture.height_;
		for( uint16_t mipLevel = 1; mipLevel < texture.mipLevels_; ++mipLevel )
		{
			const uint32_t destinationWidth = std::max( 1u, sourceWidth >> 1u );
			const uint32_t destinationHeight = std::max( 1u, sourceHeight >> 1u );
			const uint32_t destinationSubresource = CalculateSubresourceIndex( mipLevel, texture.mipLevels_ );

			TransitionSubresource(
				commandList,
				texture.resource_.Get(),
				destinationSubresource,
				finalState,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS );

			PushConstants constants{};
			constants.sourceTextureIndex = texture.srvIndex_;
			constants.destinationTextureIndex = texture.baseMipsUavBaseIndex_ + static_cast<uint32_t>( mipLevel - 1u );
			constants.sourceMipLevel = mipLevel - 1u;
			constants.destinationWidth = destinationWidth;
			constants.destinationHeight = destinationHeight;
			constants.writeSrgb = IsSrgbFormat( texture.format_ ) ? 1u : 0u;

			commandList->SetComputeRoot32BitConstants( 0, sizeof( PushConstants ) / sizeof( uint32_t ), &constants, 0 );
			commandList->Dispatch(
				( destinationWidth + ourThreadGroupSize - 1u ) / ourThreadGroupSize,
				( destinationHeight + ourThreadGroupSize - 1u ) / ourThreadGroupSize,
				1u );

			if( mipLevel + 1u < texture.mipLevels_ )
			{
				TransitionSubresource(
					commandList,
					texture.resource_.Get(),
					destinationSubresource,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
			}

			sourceWidth = destinationWidth;
			sourceHeight = destinationHeight;
		}

		TransitionSubresource(
			commandList,
			texture.resource_.Get(),
			CalculateSubresourceIndex( 0u, texture.mipLevels_ ),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			finalState );

		for( uint16_t mipLevel = 1; mipLevel < texture.mipLevels_; ++mipLevel )
		{
			TransitionSubresource(
				commandList,
				texture.resource_.Get(),
				CalculateSubresourceIndex( mipLevel, texture.mipLevels_ ),
				mipLevel + 1u < texture.mipLevels_ ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				finalState );
		}

		const SubmitHandle handle = manager_.immediateCommands_->Submit( wrapper );
		manager_.ProcessDeferredReleases();
		manager_.immediateCommands_->Wait( handle );
	}

	void BaseMips::TransitionSubresource(
		ID3D12GraphicsCommandList* commandList,
		ID3D12Resource* resource,
		uint32_t subresource,
		D3D12_RESOURCE_STATES before,
		D3D12_RESOURCE_STATES after ) noexcept
	{
		if( before == after )
		{
			return;
		}

		const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition( resource, before, after, subresource );
		commandList->ResourceBarrier( 1, &barrier );
	}
}
