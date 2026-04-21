#pragma once

#include "Actor.hpp"

#include <random>

namespace mini2d
{
	class PlayerActor final : public Actor
	{
	public:
		PlayerActor( ActorId id, std::string name );
		void Tick( World& world, float deltaTime ) override;

	private:
		float moveSpeed_ = 220.0f;
	};

	class EnemyActor final : public Actor
	{
	public:
		EnemyActor( ActorId id, std::string name, float moveSpeed = 90.0f );
		std::vector<GameCommand> TickAsync( const WorldSnapshot& snapshot, float deltaTime ) const override;

	private:
		float moveSpeed_ = 90.0f;
		float attackRange_ = 28.0f;
	};

	class SpawnerActor final : public Actor
	{
	public:
		SpawnerActor( ActorId id, std::string name );
		void Tick( World& world, float deltaTime ) override;

	private:
		float spawnInterval_ = 1.5f;
		float remainingTime_ = 0.75f;
		uint32_t spawnCounter_ = 0;
		std::mt19937 randomGenerator_;
	};

	class CursorActor final : public Actor
	{
	public:
		CursorActor( ActorId id, std::string name );
		void Tick( World& world, float deltaTime ) override;
	};

	class SpriteFollowerActor final : public Actor
	{
	public:
		SpriteFollowerActor( ActorId id, std::string name, ActorId targetActorId );
		void Tick( World& world, float deltaTime ) override;

	private:
		ActorId targetActorId_ = ourInvalidActorId;
		Vector2 offset_ = { 26.0f, 26.0f };
		float followStrength_ = 8.0f;
	};
}
