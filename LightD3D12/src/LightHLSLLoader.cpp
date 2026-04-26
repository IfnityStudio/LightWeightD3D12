#include "LightD3D12/LightHLSLLoader.hpp"

#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace lightd3d12
{
	namespace
	{
		struct HlslLoaderState
		{
			std::filesystem::path rootDirectory = std::filesystem::current_path();
			std::unordered_map<std::filesystem::path, std::string> cache;
			std::mutex mutex;
		};

		HlslLoaderState& GetState()
		{
			static HlslLoaderState state;
			return state;
		}

		std::filesystem::path NormalizePath( const std::filesystem::path& path )
		{
			return std::filesystem::absolute( path ).lexically_normal();
		}

		std::filesystem::path ResolvePathUnlocked( HlslLoaderState& state, const std::filesystem::path& shaderPath )
		{
			if( shaderPath.is_absolute() )
			{
				return NormalizePath( shaderPath );
			}

			return NormalizePath( state.rootDirectory / shaderPath );
		}

		std::string ReadTextFile( const std::filesystem::path& path )
		{
			std::ifstream file( path, std::ios::binary );
			if( !file )
			{
				throw std::runtime_error( "Failed to open HLSL file: " + path.string() );
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
	}

	void LightHLSLLoader::SetRootDirectory( const std::filesystem::path& rootDirectory )
	{
		auto& state = GetState();
		std::lock_guard lock( state.mutex );
		state.rootDirectory = NormalizePath( rootDirectory );
	}

	std::filesystem::path LightHLSLLoader::GetRootDirectory()
	{
		auto& state = GetState();
		std::lock_guard lock( state.mutex );
		return state.rootDirectory;
	}

	std::filesystem::path LightHLSLLoader::ResolvePath( const std::filesystem::path& shaderPath )
	{
		auto& state = GetState();
		std::lock_guard lock( state.mutex );
		return ResolvePathUnlocked( state, shaderPath );
	}

	const char* LightHLSLLoader::LoadSource( const std::filesystem::path& shaderPath )
	{
		auto& state = GetState();
		std::lock_guard lock( state.mutex );
		const std::filesystem::path resolvedPath = ResolvePathUnlocked( state, shaderPath );

		auto it = state.cache.find( resolvedPath );
		if( it == state.cache.end() )
		{
			it = state.cache.emplace( resolvedPath, ReadTextFile( resolvedPath ) ).first;
		}

		return it->second.c_str();
	}

	ShaderStageSource LightHLSLLoader::LoadStage( const std::filesystem::path& shaderPath, const char* profile, const char* entryPoint )
	{
		ShaderStageSource stage{};
		stage.source = LoadSource( shaderPath );
		stage.entryPoint = entryPoint;
		stage.profile = profile;
		return stage;
	}

	void LightHLSLLoader::ClearCache() noexcept
	{
		auto& state = GetState();
		std::lock_guard lock( state.mutex );
		state.cache.clear();
	}
}
