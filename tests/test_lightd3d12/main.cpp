#include <LightD3D12/HandleSlotMap.hpp>
#include <LightD3D12/LightAssimpImporter.hpp>
#include <LightD3D12/LightD3D12.hpp>
#include <LightD3D12/LightHLSLLoader.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace lightd3d12;

	struct TestFailure final : std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};

	struct TestOptions
	{
		bool runHardwareTests = false;
		bool listOnly = false;
		std::string categoryFilter;
	};

	struct TestCase
	{
		std::string category;
		std::string name;
		bool requiresHardware = false;
		std::function<void()> body;
	};

	struct TestSummary
	{
		uint32_t passed = 0;
		uint32_t failed = 0;
		uint32_t skipped = 0;
	};

	[[nodiscard]] bool IsTruthyText( std::string_view text ) noexcept
	{
		return text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON";
	}

	[[nodiscard]] bool IsTruthyEnvironmentVariable( const char* name )
	{
#if defined( _MSC_VER )
		char* value = nullptr;
		size_t valueLength = 0;
		if( _dupenv_s( &value, &valueLength, name ) != 0 || value == nullptr )
		{
			return false;
		}

		const std::string text( value, valueLength > 0 ? valueLength - 1 : 0 );
		std::free( value );
		return IsTruthyText( text );
#else
		const char* value = std::getenv( name );
		return value != nullptr && IsTruthyText( value );
#endif
	}

	[[nodiscard]] bool StartsWith( std::string_view text, std::string_view prefix ) noexcept
	{
		return text.size() >= prefix.size() && text.substr( 0, prefix.size() ) == prefix;
	}

	[[nodiscard]] TestOptions ParseOptions( int argc, char** argv )
	{
		TestOptions options{};
		options.runHardwareTests = IsTruthyEnvironmentVariable( "LIGHTD3D12_TEST_HARDWARE" );

		for( int index = 1; index < argc; ++index )
		{
			const std::string_view argument( argv[ index ] );
			if( argument == "--hardware" )
			{
				options.runHardwareTests = true;
			}
			else if( argument == "--list" )
			{
				options.listOnly = true;
			}
			else if( StartsWith( argument, "--category=" ) )
			{
				options.categoryFilter = std::string( argument.substr( std::string_view( "--category=" ).size() ) );
			}
			else
			{
				throw TestFailure( "Unknown argument: " + std::string( argument ) );
			}
		}

		return options;
	}

	[[nodiscard]] std::string MakeFailureMessage( const char* file, int line, const char* expression )
	{
		std::ostringstream message;
		message << file << ":" << line << " assertion failed: " << expression;
		return message.str();
	}

	template<typename Left, typename Right>
	[[nodiscard]] std::string MakeEqualityFailureMessage(
		const char* file,
		int line,
		const char* leftExpression,
		const char* rightExpression,
		const Left& left,
		const Right& right )
	{
		std::ostringstream message;
		message << file << ":" << line << " expected " << leftExpression << " == " << rightExpression
			<< ", got " << left << " and " << right;
		return message.str();
	}

#define TEST_REQUIRE( expression ) \
	do \
	{ \
		if( !( expression ) ) \
		{ \
			throw ::TestFailure( ::MakeFailureMessage( __FILE__, __LINE__, #expression ) ); \
		} \
	} while( false )

#define TEST_REQUIRE_EQ( left, right ) \
	do \
	{ \
		const auto leftValue = ( left ); \
		const auto rightValue = ( right ); \
		if( !( leftValue == rightValue ) ) \
		{ \
			throw ::TestFailure( ::MakeEqualityFailureMessage( __FILE__, __LINE__, #left, #right, leftValue, rightValue ) ); \
		} \
	} while( false )

	template<typename Callable>
	void TestRequireThrows( Callable&& callable, const char* expression )
	{
		try
		{
			callable();
		}
		catch( const std::exception& )
		{
			return;
		}

		throw TestFailure( std::string( "Expected exception from: " ) + expression );
	}

#define TEST_REQUIRE_THROWS( expression ) \
	::TestRequireThrows( [&]() { expression; }, #expression )

	struct DummyResource
	{
		int value = 0;
	};

	class ScopedDeviceManager final
	{
	public:
		ScopedDeviceManager()
		{
			ContextDesc desc{};
			desc.enableDebugLayer = false;
			desc.allowTearing = false;
			desc.bindlessCapacity = 64;
			desc.rtvCapacity = 16;
			desc.dsvCapacity = 8;
			manager_ = &DeviceManager::Initialize( desc );
		}

		~ScopedDeviceManager()
		{
			if( manager_ != nullptr )
			{
				manager_->WaitIdle();
				DeviceManager::ShutdownSingleton();
			}
		}

		ScopedDeviceManager( const ScopedDeviceManager& ) = delete;
		ScopedDeviceManager& operator=( const ScopedDeviceManager& ) = delete;

		[[nodiscard]] DeviceManager& Manager() const noexcept
		{
			return *manager_;
		}

		[[nodiscard]] RenderDevice& RenderDeviceRef() const noexcept
		{
			return *manager_->GetRenderDevice();
		}

	private:
		DeviceManager* manager_ = nullptr;
	};

	void RegisterCoreTests( std::vector<TestCase>& tests )
	{
		tests.push_back( TestCase
		{
			"Core.Handles",
			"DefaultHandlesAreEmptyAndUseTheInvalidIndexSentinel",
			false,
			[]
			{
				BufferHandle bufferHandle{};
				TextureHandle textureHandle{};

				TEST_REQUIRE( !bufferHandle.Valid() );
				TEST_REQUIRE( bufferHandle.Empty() );
				TEST_REQUIRE( !static_cast<bool>( bufferHandle ) );
				TEST_REQUIRE_EQ( static_cast<uint32_t>( bufferHandle ), 0x00FFFFFFu );

				TEST_REQUIRE( !textureHandle.Valid() );
				TEST_REQUIRE_EQ( bufferHandle.Index(), 0u );
				TEST_REQUIRE_EQ( bufferHandle.Gen(), 0u );
			}
		} );

		tests.push_back( TestCase
		{
			"Core.Handles",
			"SubmitHandlePacksAndUnpacksBufferIndexAndSubmitId",
			false,
			[]
			{
				SubmitHandle empty{};
				TEST_REQUIRE( empty.Empty() );
				TEST_REQUIRE_EQ( empty.Handle(), 0ull );

				constexpr uint32_t expectedBufferIndex = 17u;
				constexpr uint32_t expectedSubmitId = 42u;
				const SubmitHandle handle( ( static_cast<uint64_t>( expectedSubmitId ) << 32u ) | expectedBufferIndex );

				TEST_REQUIRE( !handle.Empty() );
				TEST_REQUIRE_EQ( handle.bufferIndex_, expectedBufferIndex );
				TEST_REQUIRE_EQ( handle.submitId_, expectedSubmitId );
				TEST_REQUIRE_EQ( handle.Handle(), ( static_cast<uint64_t>( expectedSubmitId ) << 32u ) | expectedBufferIndex );
			}
		} );

		tests.push_back( TestCase
		{
			"Core.SlotMap",
			"ReusesDestroyedSlotsWithANewGeneration",
			false,
			[]
			{
				SlotMap<DummyResource> resources;

				const auto first = resources.Create( DummyResource{ 10 } );
				const auto second = resources.Create( DummyResource{ 20 } );
				TEST_REQUIRE( first.Valid() );
				TEST_REQUIRE( second.Valid() );
				TEST_REQUIRE_EQ( resources.NumObjects(), 2u );
				TEST_REQUIRE_EQ( resources.Get( first )->value, 10 );
				TEST_REQUIRE_EQ( resources.Get( second )->value, 20 );
				TEST_REQUIRE_EQ( resources.Find( resources.Get( first ) ), first );

				resources.Destroy( first );
				TEST_REQUIRE_EQ( resources.NumObjects(), 1u );
				TEST_REQUIRE( resources.GetByIndex( first.Index() ) == nullptr );

				const auto reused = resources.Create( DummyResource{ 30 } );
				TEST_REQUIRE_EQ( reused.Index(), first.Index() );
				TEST_REQUIRE( reused.Gen() != first.Gen() );
				TEST_REQUIRE_EQ( resources.Get( reused )->value, 30 );
				TEST_REQUIRE_EQ( resources.GetAll().size(), 2ull );
				TEST_REQUIRE_EQ( resources.GetSlotsSpan().size(), 2ull );
			}
		} );

		tests.push_back( TestCase
		{
			"Core.CommandLabels",
			"BuildScopedCommandLabelExtractsReadableFunctionNames",
			false,
			[]
			{
				TEST_REQUIRE_EQ( BuildScopedCommandLabel( nullptr ), std::string{} );
				TEST_REQUIRE_EQ( BuildScopedCommandLabel( "" ), std::string{} );
				TEST_REQUIRE_EQ( BuildScopedCommandLabel( "void __cdecl RenderWaterPass(void)" ), std::string( "RenderWaterPass" ) );
				TEST_REQUIRE_EQ( BuildScopedCommandLabel( "class Foo __cdecl namespace_name::BuildThing(int)" ), std::string( "namespace_name::BuildThing" ) );
			}
		} );

		tests.push_back( TestCase
		{
			"Core.Descriptors",
			"DefaultDescriptorsExposeTheExpectedLightweightDefaults",
			false,
			[]
			{
				RenderPass renderPass{};
				TEST_REQUIRE_EQ( static_cast<int>( renderPass.color[ 0 ].loadOp ), static_cast<int>( LoadOp::Load ) );
				TEST_REQUIRE_EQ( renderPass.color[ 0 ].clearColor[ 3 ], 1.0f );
				TEST_REQUIRE_EQ( static_cast<int>( renderPass.depthStencil.depthLoadOp ), static_cast<int>( LoadOp::Load ) );
				TEST_REQUIRE_EQ( renderPass.depthStencil.clearDepth, 1.0f );

				BufferDesc bufferDesc{};
				TEST_REQUIRE_EQ( bufferDesc.size, 0ull );
				TEST_REQUIRE_EQ( bufferDesc.stride, 0u );
				TEST_REQUIRE_EQ( static_cast<int>( bufferDesc.bufferType ), static_cast<int>( BufferDesc::BufferType::Generic ) );
				TEST_REQUIRE( !bufferDesc.createConstantBufferView );

				TextureDesc textureDesc{};
				TEST_REQUIRE_EQ( textureDesc.width, 1u );
				TEST_REQUIRE_EQ( textureDesc.height, 1u );
				TEST_REQUIRE_EQ( textureDesc.countMipMap, static_cast<uint16_t>( 1u ) );
				TEST_REQUIRE_EQ( static_cast<int>( textureDesc.dimension ), static_cast<int>( TextureDimension::Texture2D ) );
				TEST_REQUIRE_EQ( static_cast<int>( textureDesc.usage ), static_cast<int>( TextureUsage::Sampled ) );

				RenderPipelineDesc pipelineDesc{};
				TEST_REQUIRE_EQ( static_cast<int>( pipelineDesc.primitiveType ), static_cast<int>( D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ) );
				TEST_REQUIRE_EQ( static_cast<int>( pipelineDesc.topology ), static_cast<int>( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST ) );
				TEST_REQUIRE_EQ( static_cast<int>( pipelineDesc.depthFormat ), static_cast<int>( DXGI_FORMAT_UNKNOWN ) );
			}
		} );

		tests.push_back( TestCase
		{
			"Core.BindingSlots",
			"LightD3D12DefinesReserveFreeAndEngineBindingSlotRanges",
			false,
			[]
			{
				static_assert( LIGHTD3D12_DESCRIPTOR_SLOT_INVALID == 0u );
				static_assert( BindingSlot<ConstantBufferSlot> );
				static_assert( BindingSlot<ShaderResourceSlot> );
				static_assert( BindingSlot<ReadWriteResourceSlot> );

				static_assert( LIGHTD3D12_FREE_CBV_SLOT_FIRST == 1u );
				static_assert( LIGHTD3D12_FREE_CBV_SLOT_COUNT == 5u );
				static_assert( LIGHTD3D12_ENGINE_CBV_SLOT_FIRST == 6u );
				static_assert( LIGHTD3D12_ENGINE_CBV_SLOT_COUNT == 5u );
				static_assert( LIGHTD3D12_CBV_SLOT_LAST == 10u );
				static_assert( ToSlotIndex( ConstantBufferSlot::FreeCB0 ) == 1u );
				static_assert( ToSlotIndex( ConstantBufferSlot::FreeCB4 ) == 5u );
				static_assert( ToSlotIndex( ConstantBufferSlot::EngineFrame ) == 6u );
				static_assert( ToSlotIndex( ConstantBufferSlot::EngineLighting ) == 10u );
				static_assert( IsFreeConstantBufferSlot( ConstantBufferSlot::FreeCB2 ) );
				static_assert( IsEngineConstantBufferSlot( ConstantBufferSlot::EngineCamera ) );
				static_assert( !IsValidConstantBufferSlot( ConstantBufferSlot::Invalid ) );

				static_assert( LIGHTD3D12_FREE_SRV_SLOT_FIRST == 11u );
				static_assert( LIGHTD3D12_FREE_SRV_SLOT_COUNT == 5u );
				static_assert( LIGHTD3D12_ENGINE_SRV_SLOT_FIRST == 16u );
				static_assert( LIGHTD3D12_ENGINE_SRV_SLOT_COUNT == 5u );
				static_assert( LIGHTD3D12_SRV_SLOT_LAST == 20u );
				static_assert( ToSlotIndex( ShaderResourceSlot::FreeSRV4 ) == 15u );
				static_assert( ToSlotIndex( ShaderResourceSlot::EngineTextures ) == 20u );
				static_assert( IsFreeShaderResourceSlot( ShaderResourceSlot::FreeSRV1 ) );
				static_assert( IsEngineShaderResourceSlot( ShaderResourceSlot::EngineMeshes ) );
				static_assert( !IsValidShaderResourceSlot( ShaderResourceSlot::Invalid ) );

				static_assert( LIGHTD3D12_FREE_RW_SLOT_FIRST == 21u );
				static_assert( LIGHTD3D12_FREE_RW_SLOT_COUNT == 3u );
				static_assert( LIGHTD3D12_ENGINE_RW_SLOT_FIRST == 24u );
				static_assert( LIGHTD3D12_ENGINE_RW_SLOT_COUNT == 2u );
				static_assert( LIGHTD3D12_RW_SLOT_LAST == 25u );
				static_assert( ToSlotIndex( ReadWriteResourceSlot::FreeRW2 ) == 23u );
				static_assert( ToSlotIndex( ReadWriteResourceSlot::EngineScratch1 ) == 25u );
				static_assert( IsFreeReadWriteResourceSlot( ReadWriteResourceSlot::FreeRW0 ) );
				static_assert( IsEngineReadWriteResourceSlot( ReadWriteResourceSlot::EngineScratch0 ) );
				static_assert( !IsValidReadWriteResourceSlot( ReadWriteResourceSlot::Invalid ) );
				static_assert( LIGHTD3D12_BINDLESS_DYNAMIC_SLOT_FIRST == 26u );
			}
		} );

		tests.push_back( TestCase
		{
			"Core.TextureUsage",
			"TextureUsageFlagsComposeAndQueryPredictably",
			false,
			[]
			{
				TextureUsage usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
				TEST_REQUIRE( HasTextureUsage( usage, TextureUsage::Sampled ) );
				TEST_REQUIRE( HasTextureUsage( usage, TextureUsage::RenderTarget ) );
				TEST_REQUIRE( !HasTextureUsage( usage, TextureUsage::DepthStencil ) );

				usage |= TextureUsage::UnorderedAccess;
				TEST_REQUIRE( HasTextureUsage( usage, TextureUsage::UnorderedAccess ) );
			}
		} );

		tests.push_back( TestCase
		{
			"Core.WindowHandles",
			"NativeWindowHandleTracksWin32HwndValidity",
			false,
			[]
			{
				NativeWindowHandle empty{};
				TEST_REQUIRE( !empty.Valid() );

				HWND fakeWindow = reinterpret_cast<HWND>( static_cast<uintptr_t>( 0x1234u ) );
				const NativeWindowHandle handle = MakeWin32WindowHandle( fakeWindow );
				TEST_REQUIRE( handle.Valid() );
				TEST_REQUIRE_EQ( static_cast<int>( handle.type ), static_cast<int>( NativeWindowHandle::Type::Win32Hwnd ) );
				TEST_REQUIRE( handle.value == fakeWindow );
			}
		} );

		tests.push_back( TestCase
		{
			"Core.ShaderLoading",
			"LightHlslLoaderResolvesCachesAndBuildsStageSources",
			false,
			[]
			{
				const std::filesystem::path previousRoot = LightHLSLLoader::GetRootDirectory();
				const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "test_lightd3d12_hlsl_loader";

				struct ScopedHlslLoaderState final
				{
					std::filesystem::path previousRoot;
					std::filesystem::path tempRoot;

					~ScopedHlslLoaderState()
					{
						LightHLSLLoader::ClearCache();
						LightHLSLLoader::SetRootDirectory( previousRoot );

						std::error_code ignored;
						std::filesystem::remove_all( tempRoot, ignored );
					}
				} stateGuard{ previousRoot, tempRoot };

				std::error_code ignored;
				std::filesystem::remove_all( tempRoot, ignored );
				std::filesystem::create_directories( tempRoot / "shaders" );

				const std::filesystem::path shaderPath = tempRoot / "shaders" / "solid_color.hlsl";
				{
					std::ofstream shaderFile( shaderPath, std::ios::binary );
					shaderFile << "float4 Shade() : SV_Target { return float4(0, 0, 1, 1); }\n";
				}

				LightHLSLLoader::ClearCache();
				LightHLSLLoader::SetRootDirectory( tempRoot );

				const std::filesystem::path expectedPath = std::filesystem::absolute( shaderPath ).lexically_normal();
				TEST_REQUIRE_EQ( LightHLSLLoader::ResolvePath( "shaders/solid_color.hlsl" ), expectedPath );

				const char* firstSource = LightHLSLLoader::LoadSource( "shaders/solid_color.hlsl" );
				TEST_REQUIRE( firstSource != nullptr );
				TEST_REQUIRE( std::string_view( firstSource ).find( "float4(0, 0, 1, 1)" ) != std::string_view::npos );

				{
					std::ofstream shaderFile( shaderPath, std::ios::binary | std::ios::trunc );
					shaderFile << "float4 Shade() : SV_Target { return float4(1, 0, 0, 1); }\n";
				}

				const char* cachedSource = LightHLSLLoader::LoadSource( "shaders/solid_color.hlsl" );
				TEST_REQUIRE( cachedSource == firstSource );
				TEST_REQUIRE( std::string_view( cachedSource ).find( "float4(1, 0, 0, 1)" ) == std::string_view::npos );

				LightHLSLLoader::ClearCache();
				const ShaderStageSource stage = LightHLSLLoader::LoadStage( "shaders/solid_color.hlsl", "ps_5_0", "Shade" );
				TEST_REQUIRE( stage.source != nullptr );
				TEST_REQUIRE( std::string_view( stage.source ).find( "float4(1, 0, 0, 1)" ) != std::string_view::npos );
				TEST_REQUIRE_EQ( std::string_view( stage.entryPoint ), std::string_view( "Shade" ) );
				TEST_REQUIRE_EQ( std::string_view( stage.profile ), std::string_view( "ps_5_0" ) );
				TEST_REQUIRE_EQ( std::filesystem::path( stage.sourceName ).lexically_normal(), expectedPath );
				TEST_REQUIRE( stage.includeDirectories.size() >= 2u );
			}
		} );

		tests.push_back( TestCase
		{
			"Import.Assimp",
			"CapabilityQueriesAreConsistentWithAssimpAvailability",
			false,
			[]
			{
				const bool available = LightAssimpImporter::IsAvailable();
				if( available )
				{
					TEST_REQUIRE( LightAssimpImporter::CanImportExtension( ".obj" ) );
					TEST_REQUIRE( LightAssimpImporter::CanImportExtension( ".OBJ" ) );
					TEST_REQUIRE( LightAssimpImporter::CanImportExtension( ".gltf" ) );
					TEST_REQUIRE( !LightAssimpImporter::CanImportExtension( ".txt" ) );
				}
				else
				{
					TEST_REQUIRE( !LightAssimpImporter::CanImportExtension( ".obj" ) );
					TEST_REQUIRE_THROWS( LightAssimpImporter::ImportScene( "missing.obj" ) );
				}
			}
		} );
	}

	void RegisterHardwareTests( std::vector<TestCase>& tests )
	{
		tests.push_back( TestCase
		{
			"Hardware.DeviceManager",
			"InitializesHeadlessDeviceAndReturnsRenderDevice",
			true,
			[]
			{
				ScopedDeviceManager device;
				TEST_REQUIRE( device.Manager().GetRenderDevice() != nullptr );
				TEST_REQUIRE( &DeviceManager::Get() == &device.Manager() );
			}
		} );

		tests.push_back( TestCase
		{
			"Hardware.BindingSlots",
			"AllocatesFreeBindingSlotsInOrderAndRejectsExhaustion",
			true,
			[]
			{
				ScopedDeviceManager device;
				RenderDevice& ctx = device.RenderDeviceRef();

				for( uint32_t expected = LIGHTD3D12_FREE_CBV_SLOT_FIRST;
					 expected < LIGHTD3D12_FREE_CBV_SLOT_FIRST + LIGHTD3D12_FREE_CBV_SLOT_COUNT;
					 ++expected )
				{
					const ConstantBufferSlot slot = ctx.GetAvailableConstantBuffer();
					TEST_REQUIRE_EQ( ToSlotIndex( slot ), expected );
					TEST_REQUIRE( IsFreeConstantBufferSlot( slot ) );
				}
				TEST_REQUIRE_THROWS( ctx.GetAvailableConstantBuffer() );

				for( uint32_t expected = LIGHTD3D12_FREE_SRV_SLOT_FIRST;
					 expected < LIGHTD3D12_FREE_SRV_SLOT_FIRST + LIGHTD3D12_FREE_SRV_SLOT_COUNT;
					 ++expected )
				{
					const ShaderResourceSlot slot = ctx.GetAvailableShaderResource();
					TEST_REQUIRE_EQ( ToSlotIndex( slot ), expected );
					TEST_REQUIRE( IsFreeShaderResourceSlot( slot ) );
				}
				TEST_REQUIRE_THROWS( ctx.GetAvailableShaderResource() );

				for( uint32_t expected = LIGHTD3D12_FREE_RW_SLOT_FIRST;
					 expected < LIGHTD3D12_FREE_RW_SLOT_FIRST + LIGHTD3D12_FREE_RW_SLOT_COUNT;
					 ++expected )
				{
					const ReadWriteResourceSlot slot = ctx.GetAvailableReadWriteResource();
					TEST_REQUIRE_EQ( ToSlotIndex( slot ), expected );
					TEST_REQUIRE( IsFreeReadWriteResourceSlot( slot ) );
				}
				TEST_REQUIRE_THROWS( ctx.GetAvailableReadWriteResource() );
			}
		} );

		tests.push_back( TestCase
		{
			"Hardware.Resources",
			"CreatesAndDestroysBasicBuffersAndSampledTexture",
			true,
			[]
			{
				ScopedDeviceManager device;
				RenderDevice& ctx = device.RenderDeviceRef();

				std::array<uint32_t, 4> initialData = { 1u, 2u, 3u, 4u };
				BufferDesc bufferDesc{};
				bufferDesc.debugName = "test_lightd3d12 basic buffer";
				bufferDesc.size = sizeof( initialData );
				bufferDesc.stride = sizeof( uint32_t );
				bufferDesc.bufferType = BufferDesc::BufferType::Generic;
				bufferDesc.data = initialData.data();
				bufferDesc.dataSize = sizeof( initialData );
				BufferHandle buffer = ctx.CreateBuffer( bufferDesc );
				TEST_REQUIRE( buffer.Valid() );
				TEST_REQUIRE_EQ( ctx.GetBindlessIndex( buffer ), LIGHTD3D12_DESCRIPTOR_SLOT_INVALID );
				TEST_REQUIRE_EQ( ctx.GetConstantBufferIndex( buffer ), LIGHTD3D12_DESCRIPTOR_SLOT_INVALID );
				ctx.Destroy( buffer );

				BufferDesc dynamicSrvBufferDesc{};
				dynamicSrvBufferDesc.debugName = "test_lightd3d12 dynamic srv buffer";
				dynamicSrvBufferDesc.size = sizeof( initialData );
				dynamicSrvBufferDesc.stride = sizeof( uint32_t );
				dynamicSrvBufferDesc.createShaderResourceView = true;
				dynamicSrvBufferDesc.data = initialData.data();
				dynamicSrvBufferDesc.dataSize = sizeof( initialData );
				BufferHandle dynamicSrvBuffer = ctx.CreateBuffer( dynamicSrvBufferDesc );
				TEST_REQUIRE( dynamicSrvBuffer.Valid() );
				TEST_REQUIRE( ctx.GetBindlessIndex( dynamicSrvBuffer ) >= LIGHTD3D12_BINDLESS_DYNAMIC_SLOT_FIRST );
				TEST_REQUIRE_EQ( ctx.GetConstantBufferIndex( dynamicSrvBuffer ), LIGHTD3D12_DESCRIPTOR_SLOT_INVALID );
				ctx.Destroy( dynamicSrvBuffer );

				BufferDesc fixedSrvBufferDesc{};
				fixedSrvBufferDesc.debugName = "test_lightd3d12 fixed slot srv buffer";
				fixedSrvBufferDesc.size = sizeof( initialData );
				fixedSrvBufferDesc.stride = sizeof( uint32_t );
				fixedSrvBufferDesc.data = initialData.data();
				fixedSrvBufferDesc.dataSize = sizeof( initialData );
				BufferHandle fixedSrvBuffer = ctx.CreateBuffer( fixedSrvBufferDesc, ShaderResourceSlot::FreeSRV0 );
				TEST_REQUIRE( fixedSrvBuffer.Valid() );
				TEST_REQUIRE_EQ( ctx.GetBindlessIndex( fixedSrvBuffer ), ToSlotIndex( ShaderResourceSlot::FreeSRV0 ) );
				std::array<uint32_t, 4> updatedData = { 4u, 3u, 2u, 1u };
				ctx.WriteBuffer( fixedSrvBuffer, 0, updatedData.data(), sizeof( updatedData ) );
				ctx.Destroy( fixedSrvBuffer );

				struct TestConstants
				{
					std::array<float, 4> color = { 0.25f, 0.5f, 1.0f, 1.0f };
				};

				TestConstants constants{};
				BufferDesc constantBufferDesc{};
				constantBufferDesc.debugName = "test_lightd3d12 constant buffer view";
				constantBufferDesc.size = sizeof( constants );
				constantBufferDesc.heapType = D3D12_HEAP_TYPE_UPLOAD;
				constantBufferDesc.data = &constants;
				constantBufferDesc.dataSize = sizeof( constants );
				BufferHandle constantBuffer = ctx.CreateBuffer( constantBufferDesc, ConstantBufferSlot::FreeCB0 );
				TEST_REQUIRE( constantBuffer.Valid() );
				TEST_REQUIRE_EQ( ctx.GetConstantBufferIndex( constantBuffer ), ToSlotIndex( ConstantBufferSlot::FreeCB0 ) );
				TEST_REQUIRE_EQ( ctx.GetBindlessIndex( constantBuffer ), LIGHTD3D12_DESCRIPTOR_SLOT_INVALID );
				ctx.Destroy( constantBuffer );

				BufferDesc dynamicConstantBufferDesc{};
				dynamicConstantBufferDesc.debugName = "test_lightd3d12 dynamic constant buffer view";
				dynamicConstantBufferDesc.size = sizeof( constants );
				dynamicConstantBufferDesc.heapType = D3D12_HEAP_TYPE_UPLOAD;
				dynamicConstantBufferDesc.createConstantBufferView = true;
				dynamicConstantBufferDesc.data = &constants;
				dynamicConstantBufferDesc.dataSize = sizeof( constants );
				BufferHandle dynamicConstantBuffer = ctx.CreateBuffer( dynamicConstantBufferDesc );
				TEST_REQUIRE( dynamicConstantBuffer.Valid() );
				TEST_REQUIRE( ctx.GetConstantBufferIndex( dynamicConstantBuffer ) >= LIGHTD3D12_BINDLESS_DYNAMIC_SLOT_FIRST );
				TEST_REQUIRE_EQ( ctx.GetBindlessIndex( dynamicConstantBuffer ), LIGHTD3D12_DESCRIPTOR_SLOT_INVALID );
				ctx.Destroy( dynamicConstantBuffer );

				TextureDesc textureDesc{};
				textureDesc.debugName = "test_lightd3d12 sampled texture";
				textureDesc.width = 4;
				textureDesc.height = 4;
				textureDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
				textureDesc.usage = TextureUsage::Sampled;
				TextureHandle texture = ctx.CreateTexture( textureDesc );
				TEST_REQUIRE( texture.Valid() );
				TEST_REQUIRE( ctx.GetBindlessIndex( texture ) >= LIGHTD3D12_BINDLESS_DYNAMIC_SLOT_FIRST );
				ctx.Destroy( texture );
			}
		} );

		tests.push_back( TestCase
		{
			"Hardware.Validation",
			"RejectsInvalidTextureUsageCombinations",
			true,
			[]
			{
				ScopedDeviceManager device;
				RenderDevice& ctx = device.RenderDeviceRef();

				TextureDesc invalidDesc{};
				invalidDesc.debugName = "invalid render target depth texture";
				invalidDesc.format = DXGI_FORMAT_D32_FLOAT;
				invalidDesc.usage = TextureUsage::RenderTarget | TextureUsage::DepthStencil;
				TEST_REQUIRE_THROWS( ctx.CreateTexture( invalidDesc ) );

				TextureDesc invalid3DDesc{};
				invalid3DDesc.debugName = "invalid 3D render target";
				invalid3DDesc.dimension = TextureDimension::Texture3D;
				invalid3DDesc.depthOrArraySize = 4;
				invalid3DDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
				TEST_REQUIRE_THROWS( ctx.CreateTexture( invalid3DDesc ) );
			}
		} );

		tests.push_back( TestCase
		{
			"Hardware.Pipelines",
			"CompilesMinimalRenderPipeline",
			true,
			[]
			{
				ScopedDeviceManager device;
				RenderDevice& ctx = device.RenderDeviceRef();

				RenderPipelineDesc desc{};
				desc.vertexShader.source = R"(
float4 main(uint vertexID : SV_VertexID) : SV_Position
{
    const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    return float4(positions[vertexID], 0.0, 1.0);
}
)";
				desc.vertexShader.entryPoint = "main";
				desc.vertexShader.profile = "vs_6_6";
				desc.fragmentShader.source = R"(
float4 main() : SV_Target0
{
    return float4(0.2, 0.4, 0.8, 1.0);
}
)";
				desc.fragmentShader.entryPoint = "main";
				desc.fragmentShader.profile = "ps_6_6";
				desc.color[ 0 ].format = DXGI_FORMAT_R8G8B8A8_UNORM;
				desc.depthFormat = DXGI_FORMAT_UNKNOWN;
				desc.depthStencilState.DepthEnable = FALSE;
				desc.depthStencilState.StencilEnable = FALSE;

				RenderPipelineState pipeline = ctx.CreateRenderPipeline( desc );
				TEST_REQUIRE( pipeline.Valid() );

				const std::filesystem::path previousRoot = LightHLSLLoader::GetRootDirectory();
				const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "test_lightd3d12_hlsl_include_pipeline";

				struct ScopedIncludePipelineShaderState final
				{
					std::filesystem::path previousRoot;
					std::filesystem::path tempRoot;

					~ScopedIncludePipelineShaderState()
					{
						LightHLSLLoader::ClearCache();
						LightHLSLLoader::SetRootDirectory( previousRoot );

						std::error_code ignored;
						std::filesystem::remove_all( tempRoot, ignored );
					}
				} shaderStateGuard{ previousRoot, tempRoot };

				std::error_code ignored;
				std::filesystem::remove_all( tempRoot, ignored );
				std::filesystem::create_directories( tempRoot );

				const std::filesystem::path includeShaderPath = tempRoot / "fixed_slot_include.hlsl";
				{
					std::ofstream shaderFile( includeShaderPath, std::ios::binary );
					shaderFile << R"(
#include "LightD3D12_Defines.hlsli"

float4 VSMain(uint vertexID : SV_VertexID) : SV_Position
{
    const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    return float4(positions[vertexID], 0.0, 1.0);
}

float4 PSMain() : SV_Target0
{
    const float tint = LIGHTD3D12_SRV_SLOT_FREESRV0 > LIGHTD3D12_CBV_SLOT_FREECB0 ? 0.75 : 0.25;
    return float4(tint, 0.35, 0.9, 1.0);
}
)";
				}

				LightHLSLLoader::ClearCache();
				LightHLSLLoader::SetRootDirectory( tempRoot );

				RenderPipelineDesc includeDesc{};
				includeDesc.vertexShader = LightHLSLLoader::LoadStage( "fixed_slot_include.hlsl", "vs_6_6", "VSMain" );
				includeDesc.fragmentShader = LightHLSLLoader::LoadStage( "fixed_slot_include.hlsl", "ps_6_6", "PSMain" );
				includeDesc.color[ 0 ].format = DXGI_FORMAT_R8G8B8A8_UNORM;
				includeDesc.depthFormat = DXGI_FORMAT_UNKNOWN;
				includeDesc.depthStencilState.DepthEnable = FALSE;
				includeDesc.depthStencilState.StencilEnable = FALSE;

				RenderPipelineState includePipeline = ctx.CreateRenderPipeline( includeDesc );
				TEST_REQUIRE( includePipeline.Valid() );
			}
		} );

		tests.push_back( TestCase
		{
			"Hardware.CommandBuffers",
			"RecordsAndSubmitsOffscreenClearPass",
			true,
			[]
			{
				ScopedDeviceManager device;
				RenderDevice& ctx = device.RenderDeviceRef();

				TextureDesc colorDesc{};
				colorDesc.debugName = "test_lightd3d12 offscreen clear target";
				colorDesc.width = 16;
				colorDesc.height = 16;
				colorDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
				colorDesc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
				colorDesc.useClearValue = true;
				colorDesc.clearValue.Format = colorDesc.format;
				colorDesc.clearValue.Color[ 0 ] = 0.0f;
				colorDesc.clearValue.Color[ 1 ] = 0.0f;
				colorDesc.clearValue.Color[ 2 ] = 0.0f;
				colorDesc.clearValue.Color[ 3 ] = 1.0f;
				TextureHandle color = ctx.CreateTexture( colorDesc );

				RenderPass pass{};
				pass.color[ 0 ].loadOp = LoadOp::Clear;
				pass.color[ 0 ].clearColor = { 0.1f, 0.2f, 0.3f, 1.0f };

				Framebuffer framebuffer{};
				framebuffer.color[ 0 ].texture = color;

				ICommandBuffer& commandBuffer = ctx.AcquireCommandBuffer();
				commandBuffer.CmdBeginRendering( pass, framebuffer );
				commandBuffer.CmdEndRendering();
				const SubmitHandle submit = ctx.Submit( commandBuffer );
				TEST_REQUIRE( !submit.Empty() );
				ctx.WaitIdle();
				ctx.Destroy( color );
			}
		} );
	}

	[[nodiscard]] std::vector<TestCase> BuildTestList()
	{
		std::vector<TestCase> tests;
		RegisterCoreTests( tests );
		RegisterHardwareTests( tests );
		return tests;
	}

	[[nodiscard]] bool MatchesFilter( const TestCase& test, const TestOptions& options )
	{
		return options.categoryFilter.empty() || StartsWith( test.category, options.categoryFilter );
	}

	void PrintUsage()
	{
		std::cout
			<< "test_lightd3d12 [--hardware] [--list] [--category=prefix]\n"
			<< "\n"
			<< "Default: runs CPU/dummy tests only.\n"
			<< "--hardware or LIGHTD3D12_TEST_HARDWARE=1 enables real D3D12 device tests.\n";
	}

	int RunTests( const TestOptions& options, const std::vector<TestCase>& tests )
	{
		if( options.listOnly )
		{
			for( const TestCase& test : tests )
			{
				if( MatchesFilter( test, options ) )
				{
					std::cout << ( test.requiresHardware ? "[hardware] " : "[cpu]      " )
						<< test.category << "." << test.name << "\n";
				}
			}
			return 0;
		}

		TestSummary summary{};
		for( const TestCase& test : tests )
		{
			if( !MatchesFilter( test, options ) )
			{
				continue;
			}

			const std::string fullName = test.category + "." + test.name;
			if( test.requiresHardware && !options.runHardwareTests )
			{
				++summary.skipped;
				std::cout << "[SKIP] " << fullName << " (enable with --hardware)\n";
				continue;
			}

			try
			{
				test.body();
				++summary.passed;
				std::cout << "[PASS] " << fullName << "\n";
			}
			catch( const std::exception& exception )
			{
				++summary.failed;
				std::cout << "[FAIL] " << fullName << "\n       " << exception.what() << "\n";
			}
		}

		std::cout << "\nSummary: " << summary.passed << " passed, "
			<< summary.failed << " failed, " << summary.skipped << " skipped\n";
		return summary.failed == 0 ? 0 : 1;
	}
}

int main( int argc, char** argv )
{
	try
	{
		const TestOptions options = ParseOptions( argc, argv );
		const std::vector<TestCase> tests = BuildTestList();
		return RunTests( options, tests );
	}
	catch( const std::exception& exception )
	{
		std::cerr << "test_lightd3d12 error: " << exception.what() << "\n\n";
		PrintUsage();
		return 2;
	}
}
