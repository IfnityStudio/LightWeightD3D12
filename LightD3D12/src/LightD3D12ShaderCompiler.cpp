#include "LightD3D12ShaderCompiler.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace lightd3d12
{
	namespace
	{
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

		std::filesystem::path GetExecutableDirectory()
		{
			std::array<wchar_t, MAX_PATH> modulePath{};
			const DWORD length = GetModuleFileNameW( nullptr, modulePath.data(), static_cast<DWORD>( modulePath.size() ) );
			if( length == 0 )
			{
				return std::filesystem::current_path();
			}

			return std::filesystem::path( modulePath.data(), modulePath.data() + length ).parent_path();
		}

		std::wstring ToLowerAscii( std::wstring text )
		{
			std::transform( text.begin(), text.end(), text.begin(), []( wchar_t character )
				{
					return static_cast<wchar_t>( towlower( character ) );
				} );
			return text;
		}

		bool IsBlockedDxCompilerPath( const std::filesystem::path& path )
		{
			const std::wstring normalized = ToLowerAscii( path.wstring() );
			return normalized.find( L"\\renderdoc\\plugins\\d3d12\\dxcompiler.dll" ) != std::wstring::npos ||
				normalized.find( L"\\bravesoftware\\brave-browser\\" ) != std::wstring::npos;
		}

		bool HasSiblingDxil( const std::filesystem::path& compilerPath )
		{
			std::error_code error;
			return std::filesystem::exists( compilerPath.parent_path() / "dxil.dll", error );
		}

		std::vector<uint32_t> ParseDottedVersion( std::wstring_view versionText )
		{
			std::vector<uint32_t> parts;
			uint32_t current = 0;
			bool hasDigits = false;

			for( const wchar_t character : versionText )
			{
				if( character >= L'0' && character <= L'9' )
				{
					hasDigits = true;
					current = current * 10u + static_cast<uint32_t>( character - L'0' );
					continue;
				}

				if( character == L'.' )
				{
					parts.push_back( hasDigits ? current : 0u );
					current = 0;
					hasDigits = false;
					continue;
				}

				return {};
			}

			if( hasDigits )
			{
				parts.push_back( current );
			}

			return parts;
		}

		bool IsVersionGreater( const std::vector<uint32_t>& left, const std::vector<uint32_t>& right )
		{
			const size_t partCount = std::max( left.size(), right.size() );
			for( size_t index = 0; index < partCount; ++index )
			{
				const uint32_t leftPart = index < left.size() ? left[ index ] : 0u;
				const uint32_t rightPart = index < right.size() ? right[ index ] : 0u;
				if( leftPart != rightPart )
				{
					return leftPart > rightPart;
				}
			}

			return false;
		}

		std::filesystem::path FindLatestWindowsSdkDxCompiler()
		{
			wchar_t* programFilesX86Raw = nullptr;
			size_t programFilesX86Length = 0;
			if( _wdupenv_s( &programFilesX86Raw, &programFilesX86Length, L"ProgramFiles(x86)" ) != 0 || programFilesX86Raw == nullptr )
			{
				return {};
			}
			const std::filesystem::path programFilesX86Path( programFilesX86Raw );
			free( programFilesX86Raw );

			const std::filesystem::path sdkBinDirectory = programFilesX86Path / "Windows Kits" / "10" / "bin";
			std::error_code error;
			if( !std::filesystem::exists( sdkBinDirectory, error ) )
			{
				return {};
			}

			std::filesystem::path bestCompilerPath;
			std::vector<uint32_t> bestVersionParts;
			for( const auto& entry : std::filesystem::directory_iterator( sdkBinDirectory, error ) )
			{
				if( error || !entry.is_directory( error ) )
				{
					continue;
				}

				const std::vector<uint32_t> versionParts = ParseDottedVersion( entry.path().filename().wstring() );
				if( versionParts.empty() )
				{
					continue;
				}

				const std::filesystem::path candidateCompiler = entry.path() / "x64" / "dxcompiler.dll";
				if( !HasSiblingDxil( candidateCompiler ) )
				{
					continue;
				}

				if( bestCompilerPath.empty() || IsVersionGreater( versionParts, bestVersionParts ) )
				{
					bestCompilerPath = candidateCompiler;
					bestVersionParts = versionParts;
				}
			}

			return bestCompilerPath;
		}

		HMODULE TryLoadDxCompilerFromPath( const std::filesystem::path& candidate )
		{
			std::error_code error;
			if( !std::filesystem::exists( candidate, error ) || IsBlockedDxCompilerPath( candidate ) )
			{
				return nullptr;
			}

			if( !HasSiblingDxil( candidate ) )
			{
				return nullptr;
			}

			return LoadLibraryExW(
				candidate.c_str(),
				nullptr,
				LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 );
		}

		HMODULE LoadDxCompilerModule()
		{
			const std::filesystem::path executableDirectory = GetExecutableDirectory();
			const std::filesystem::path currentDirectory = std::filesystem::current_path();
			const std::filesystem::path sdkCompiler = FindLatestWindowsSdkDxCompiler();

			std::vector<std::filesystem::path> candidates;
			candidates.reserve( 3 );
			candidates.push_back( executableDirectory / "dxcompiler.dll" );
			if( currentDirectory != executableDirectory )
			{
				candidates.push_back( currentDirectory / "dxcompiler.dll" );
			}
			if( !sdkCompiler.empty() )
			{
				candidates.push_back( sdkCompiler );
			}

			for( const auto& candidate : candidates )
			{
				if( HMODULE module = TryLoadDxCompilerFromPath( candidate ); module != nullptr )
				{
					return module;
				}
			}

			HMODULE module = LoadLibraryExW(
				L"dxcompiler.dll",
				nullptr,
				LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS );
			if( module != nullptr )
			{
				std::array<wchar_t, MAX_PATH> loadedPath{};
				const DWORD length = GetModuleFileNameW( module, loadedPath.data(), static_cast<DWORD>( loadedPath.size() ) );
				if( length == 0 )
				{
					FreeLibrary( module );
					return nullptr;
				}

				const std::filesystem::path loadedModulePath( loadedPath.data(), loadedPath.data() + length );
				if( IsBlockedDxCompilerPath( loadedModulePath ) || !HasSiblingDxil( loadedModulePath ) )
				{
					FreeLibrary( module );
					return nullptr;
				}
			}

			return module;
		}

		DxcCreateInstanceProc LoadDxcCreateInstance()
		{
			static DxcCreateInstanceProc ourCreateInstance = []() -> DxcCreateInstanceProc
				{
					HMODULE module = LoadDxCompilerModule();
					if( module == nullptr )
					{
						return nullptr;
					}

					return reinterpret_cast<DxcCreateInstanceProc>( GetProcAddress( module, "DxcCreateInstance" ) );
				}();

			if( ourCreateInstance == nullptr )
			{
				throw std::runtime_error( "Failed to locate dxcompiler.dll for DXC shader compilation." );
			}

			return ourCreateInstance;
		}
	}

	CompiledShader CompileShader( const ShaderStageSource& stage, const char* defaultProfile )
	{
		const char* profile = stage.profile != nullptr ? stage.profile : defaultProfile;
		if( stage.source == nullptr || profile == nullptr )
		{
			throw std::runtime_error( "Shader source or profile is invalid." );
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
}
