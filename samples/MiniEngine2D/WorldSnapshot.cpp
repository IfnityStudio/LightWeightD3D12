#include "WorldSnapshot.hpp"

namespace mini2d
{
	namespace
	{
		Vector2 ComputeCellCenter( const Vector2& position, const Vector2& size )
		{
			return { position.x + size.x * 0.5f, position.y + size.y * 0.5f };
		}
	}

	const ActorSnapshotData* WorldSnapshot::FindActor( ActorId actorId ) const
	{
		for( const ActorSnapshotData& actor : actors_ )
		{
			if( actor.id == actorId )
			{
				return &actor;
			}
		}

		return nullptr;
	}

	const ActorSnapshotData* WorldSnapshot::FindFirstActorOfKind( ActorKind kind ) const
	{
		for( const ActorSnapshotData& actor : actors_ )
		{
			if( actor.kind == kind && actor.active && actor.alive )
			{
				return &actor;
			}
		}

		return nullptr;
	}

	bool WorldSnapshot::IsCellBlocked( const Vector2& candidatePosition, const Vector2& candidateSize, ActorId ignoredActorId ) const
	{
		const Vector2 candidateCenter = ComputeCellCenter( candidatePosition, candidateSize );
		const int candidateCellX = static_cast<int>( candidateCenter.x / cellSize_ );
		const int candidateCellY = static_cast<int>( candidateCenter.y / cellSize_ );

		for( const ActorSnapshotData& actor : actors_ )
		{
			if( !actor.active || !actor.alive || !actor.blocksCell || actor.id == ignoredActorId )
			{
				continue;
			}

			const Vector2 actorCenter = ComputeCellCenter( actor.position, actor.size );
			const int actorCellX = static_cast<int>( actorCenter.x / cellSize_ );
			const int actorCellY = static_cast<int>( actorCenter.y / cellSize_ );
			if( actorCellX == candidateCellX && actorCellY == candidateCellY )
			{
				return true;
			}
		}

		return false;
	}

	Vector2 WorldSnapshot::ClampToWorld( const Vector2& position, const Vector2& size ) const
	{
		return {
			ClampFloat( position.x, 0.0f, worldWidth_ - size.x ),
			ClampFloat( position.y, 0.0f, worldHeight_ - size.y )
		};
	}

	float WorldSnapshot::GetWorldWidth() const noexcept
	{
		return worldWidth_;
	}

	float WorldSnapshot::GetWorldHeight() const noexcept
	{
		return worldHeight_;
	}

	float WorldSnapshot::GetCellSize() const noexcept
	{
		return cellSize_;
	}

	uint32_t WorldSnapshot::GetFrameNumber() const noexcept
	{
		return frameNumber_;
	}

	float WorldSnapshot::GetTimeSeconds() const noexcept
	{
		return timeSeconds_;
	}
}
