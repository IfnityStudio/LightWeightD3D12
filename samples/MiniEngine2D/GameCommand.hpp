#pragma once

#include "EngineTypes.hpp"

#include <string>
#include <variant>

namespace mini2d
{
	struct MoveActorCommand
	{
		ActorId actorId = ourInvalidActorId;
		Vector2 newPosition = {};
	};

	struct DamageActorCommand
	{
		ActorId actorId = ourInvalidActorId;
		int amount = 0;
	};

	struct DestroyActorCommand
	{
		ActorId actorId = ourInvalidActorId;
	};

	struct SetSpriteCommand
	{
		ActorId actorId = ourInvalidActorId;
		SpriteId spriteId = SpriteId::WhiteSquare;
	};

	struct SetAnimationFrameCommand
	{
		ActorId actorId = ourInvalidActorId;
		uint32_t frameIndex = 0;
	};

	struct SpawnEnemyCommand
	{
		std::string name;
		Vector2 position = {};
	};

	using GameCommand = std::variant<
		MoveActorCommand,
		DamageActorCommand,
		DestroyActorCommand,
		SetSpriteCommand,
		SetAnimationFrameCommand,
		SpawnEnemyCommand>;
}
