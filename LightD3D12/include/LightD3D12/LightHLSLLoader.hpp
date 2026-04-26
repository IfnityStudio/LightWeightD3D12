#pragma once

#include "LightD3D12/LightD3D12.hpp"

#include <filesystem>

namespace lightd3d12
{
	class LightHLSLLoader final
	{
	public:
		static void SetRootDirectory( const std::filesystem::path& rootDirectory );
		static std::filesystem::path GetRootDirectory();
		static std::filesystem::path ResolvePath( const std::filesystem::path& shaderPath );
		static const char* LoadSource( const std::filesystem::path& shaderPath );
		static ShaderStageSource LoadStage( const std::filesystem::path& shaderPath, const char* profile, const char* entryPoint = "main" );
		static void ClearCache() noexcept;

	private:
		LightHLSLLoader() = delete;
	};
}
