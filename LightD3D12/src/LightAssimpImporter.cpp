#include "LightD3D12/LightAssimpImporter.hpp"

#include <algorithm>
#include <stdexcept>

#if defined( LIGHTD3D12_ENABLE_ASSIMP ) && LIGHTD3D12_ENABLE_ASSIMP
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

namespace lightd3d12
{
	namespace
	{
		constexpr std::array<std::string_view, 4> kSupportedExtensions = { ".obj", ".gltf", ".glb", ".fbx" };

		bool IsSupportedExtension( std::string_view extension ) noexcept
		{
			std::string normalized( extension );
			std::ranges::transform( normalized, normalized.begin(), []( const unsigned char value )
				{
					return static_cast<char>( std::tolower( value ) );
				} );

			return std::ranges::find( kSupportedExtensions, std::string_view( normalized ) ) != kSupportedExtensions.end();
		}
	}

	bool LightAssimpImporter::IsAvailable() noexcept
	{
#if defined( LIGHTD3D12_ENABLE_ASSIMP ) && LIGHTD3D12_ENABLE_ASSIMP
		return true;
#else
		return false;
#endif
	}

	bool LightAssimpImporter::CanImportExtension( std::string_view extension ) noexcept
	{
		if( !IsAvailable() )
		{
			return false;
		}

		return IsSupportedExtension( extension );
	}

	ImportedScene LightAssimpImporter::ImportScene( const std::filesystem::path& assetPath )
	{
#if defined( LIGHTD3D12_ENABLE_ASSIMP ) && LIGHTD3D12_ENABLE_ASSIMP
		if( !IsSupportedExtension( assetPath.extension().string() ) )
		{
			throw std::runtime_error( "Assimp importer does not support this extension in the current wrapper: " + assetPath.extension().string() );
		}

		Assimp::Importer importer;
		constexpr unsigned int importFlags =
			aiProcess_Triangulate |
			aiProcess_JoinIdenticalVertices |
			aiProcess_ImproveCacheLocality |
			aiProcess_SortByPType |
			aiProcess_GenSmoothNormals |
			aiProcess_CalcTangentSpace;

		const aiScene* scene = importer.ReadFile( assetPath.string(), importFlags );
		if( scene == nullptr || scene->mRootNode == nullptr )
		{
			throw std::runtime_error( "Assimp failed to import scene: " + std::string( importer.GetErrorString() ) );
		}

		ImportedScene result{};
		result.sourceFormatHint = assetPath.extension().string();
		result.meshes.reserve( scene->mNumMeshes );

		for( uint32_t meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex )
		{
			const aiMesh* mesh = scene->mMeshes[ meshIndex ];
			if( mesh == nullptr )
			{
				continue;
			}

			ImportedMesh importedMesh{};
			importedMesh.name = mesh->mName.C_Str();
			importedMesh.vertices.reserve( mesh->mNumVertices );

			if( mesh->mMaterialIndex < scene->mNumMaterials )
			{
				const aiMaterial* material = scene->mMaterials[ mesh->mMaterialIndex ];
				if( material != nullptr )
				{
					aiString materialName;
					if( material->Get( AI_MATKEY_NAME, materialName ) == aiReturn_SUCCESS )
					{
						importedMesh.materialName = materialName.C_Str();
					}
				}
			}

			for( uint32_t vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex )
			{
				ImportedVertex vertex{};
				const aiVector3D& position = mesh->mVertices[ vertexIndex ];
				vertex.position = { position.x, position.y, position.z };

				if( mesh->HasNormals() )
				{
					const aiVector3D& normal = mesh->mNormals[ vertexIndex ];
					vertex.normal = { normal.x, normal.y, normal.z };
				}

				if( mesh->HasTangentsAndBitangents() )
				{
					const aiVector3D& tangent = mesh->mTangents[ vertexIndex ];
					vertex.tangent = { tangent.x, tangent.y, tangent.z, 1.0f };
				}

				if( mesh->HasTextureCoords( 0 ) )
				{
					const aiVector3D& uv = mesh->mTextureCoords[ 0 ][ vertexIndex ];
					vertex.texCoord = { uv.x, uv.y };
				}

				importedMesh.vertices.push_back( vertex );
			}

			for( uint32_t faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex )
			{
				const aiFace& face = mesh->mFaces[ faceIndex ];
				for( uint32_t index = 0; index < face.mNumIndices; ++index )
				{
					importedMesh.indices.push_back( face.mIndices[ index ] );
				}
			}

			result.meshes.push_back( std::move( importedMesh ) );
		}

		return result;
#else
		( void )assetPath;
		throw std::runtime_error( "Assimp support is disabled. Enable LIGHTD3D12_ENABLE_ASSIMP via LightD3D12EnableAssimp and provide third_party/assimp." );
#endif
	}
}
