#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lightd3d12
{
	struct ImportedVertex
	{
		std::array<float, 3> position = {};
		std::array<float, 3> normal = { 0.0f, 1.0f, 0.0f };
		std::array<float, 4> tangent = { 0.0f, 0.0f, 0.0f, 1.0f };
		std::array<float, 2> texCoord = {};
	};

	struct ImportedMesh
	{
		std::string name;
		std::string materialName;
		std::vector<ImportedVertex> vertices;
		std::vector<uint32_t> indices;
	};

	struct ImportedScene
	{
		std::string sourceFormatHint;
		std::vector<ImportedMesh> meshes;
	};

	class LightAssimpImporter final
	{
	public:
		static bool IsAvailable() noexcept;
		static bool CanImportExtension( std::string_view extension ) noexcept;
		static ImportedScene ImportScene( const std::filesystem::path& assetPath );

	private:
		LightAssimpImporter() = delete;
	};
}
