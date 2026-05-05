#pragma once

#include "LightD3D12Internal.hpp"

#include <dxcapi.h>

namespace lightd3d12
{
	struct CompiledShader final
	{
		ComPtr<ID3DBlob> d3dBlob_;
		ComPtr<IDxcBlob> dxcBlob_;

		[[nodiscard]] D3D12_SHADER_BYTECODE Bytecode() const noexcept
		{
			if( dxcBlob_ != nullptr )
			{
				return { dxcBlob_->GetBufferPointer(), dxcBlob_->GetBufferSize() };
			}

			return { d3dBlob_->GetBufferPointer(), d3dBlob_->GetBufferSize() };
		}
	};

	CompiledShader CompileShader( const ShaderStageSource& stage, const char* defaultProfile );
}
