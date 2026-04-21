#pragma once

#include "EngineTypes.hpp"

#include <string>
#include <vector>

namespace mini2d
{
	class World;

	struct ActorSnapshotData
	{
		ActorId id = ourInvalidActorId;
		ActorKind kind = ActorKind::Enemy;
		std::string name;
		Vector2 position = {};
		Vector2 size = {};
		SpriteId spriteId = SpriteId::WhiteSquare;
		bool active = true;
		bool alive = true;
		bool blocksCell = true;
		int health = 0;
	};

	class WorldSnapshot
	{
	public:
		const ActorSnapshotData* FindActor( ActorId actorId ) const;
		const ActorSnapshotData* FindFirstActorOfKind( ActorKind kind ) const;
		bool IsCellBlocked( const Vector2& candidatePosition, const Vector2& candidateSize, ActorId ignoredActorId ) const;
		Vector2 ClampToWorld( const Vector2& position, const Vector2& size ) const;
		float GetWorldWidth() const noexcept;
		float GetWorldHeight() const noexcept;
		float GetCellSize() const noexcept;
		uint32_t GetFrameNumber() const noexcept;
		float GetTimeSeconds() const noexcept;

	private:
		friend class World;

		std::vector<ActorSnapshotData> actors_ = {};
		float worldWidth_ = 0.0f;
		float worldHeight_ = 0.0f;
		float cellSize_ = 1.0f;
		uint32_t frameNumber_ = 0;
		float timeSeconds_ = 0.0f;
	};
}
