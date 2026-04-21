#pragma once

#include "EngineTypes.hpp"

#include <string>
#include <vector>

namespace mini2d
{
	struct SpriteSceneProxy
	{
		ActorId actorId = ourInvalidActorId;
		SpriteId spriteId = SpriteId::WhiteSquare;
		Vector2 position = {};
		Vector2 size = {};
		LinearColor tint = {};
		int renderLayer = 0;
		bool visible = true;
		uint32_t animationFrame = 0;
	};

	struct RenderFrame
	{
		uint32_t frameNumber = 0;
		float deltaTime = 0.0f;
		float framesPerSecond = 0.0f;
		uint32_t actorCount = 0;
		uint32_t enemyCount = 0;
		uint32_t playerHealth = 0;
		uint32_t syncTicks = 0;
		uint32_t asyncTicks = 0;
		uint32_t commandsApplied = 0;
		std::vector<SpriteSceneProxy> sprites = {};
		std::vector<std::string> debugLines = {};
	};
}
