#include "World.hpp"

#include "Actors.hpp"
#include "Profiler.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <type_traits>
#include <variant>
#include <windows.h>

namespace mini2d
{
	namespace
	{
		Vector2 ComputeCellCenter( const Vector2& position, const Vector2& size )
		{
			return { position.x + size.x * 0.5f, position.y + size.y * 0.5f };
		}
	}

	World::World( const WorldConfig& config ):
		config_( config )
	{
	}

	Actor* World::FindActor( ActorId actorId )
	{
		const auto iterator = actors_.find( actorId );
		return iterator != actors_.end() ? iterator->second.get() : nullptr;
	}

	const Actor* World::FindActor( ActorId actorId ) const
	{
		const auto iterator = actors_.find( actorId );
		return iterator != actors_.end() ? iterator->second.get() : nullptr;
	}

	const GameInputState& World::GetInputState() const noexcept
	{
		return inputState_;
	}

	void World::SetInputState( const GameInputState& inputState ) noexcept
	{
		inputState_ = inputState;
	}

	Vector2 World::ClampToWorld( const Vector2& position, const Vector2& size ) const
	{
		return {
			ClampFloat( position.x, 0.0f, config_.width - size.x ),
			ClampFloat( position.y, 0.0f, config_.height - size.y )
		};
	}

	bool World::CanMoveActorTo( ActorId actorId, const Vector2& position, const Vector2& size ) const
	{
		const Vector2 clampedPosition = ClampToWorld( position, size );
		const Vector2 candidateCenter = ComputeCellCenter( clampedPosition, size );
		const int candidateCellX = static_cast<int>( candidateCenter.x / config_.cellSize );
		const int candidateCellY = static_cast<int>( candidateCenter.y / config_.cellSize );

		for( const auto& entry : actors_ )
		{
			const Actor& other = *entry.second;
			if( other.GetId() == actorId || !other.IsAlive() || !other.IsActive() || !other.BlocksCell() )
			{
				continue;
			}

			const Vector2 otherCenter = ComputeCellCenter( other.GetPosition(), other.GetSize() );
			const int otherCellX = static_cast<int>( otherCenter.x / config_.cellSize );
			const int otherCellY = static_cast<int>( otherCenter.y / config_.cellSize );
			if( candidateCellX == otherCellX && candidateCellY == otherCellY )
			{
				return false;
			}
		}

		return true;
	}

	void World::SetFrameContext( uint32_t frameNumber, float timeSeconds ) noexcept
	{
		frameNumber_ = frameNumber;
		timeSeconds_ = timeSeconds;
	}

	void World::SetWorldSize( float width, float height ) noexcept
	{
		config_.width = width;
		config_.height = height;
	}

	float World::GetWorldWidth() const noexcept
	{
		return config_.width;
	}

	float World::GetWorldHeight() const noexcept
	{
		return config_.height;
	}

	float World::GetCellSize() const noexcept
	{
		return config_.cellSize;
	}

	uint32_t World::GetFrameNumber() const noexcept
	{
		return frameNumber_;
	}

	float World::GetTimeSeconds() const noexcept
	{
		return timeSeconds_;
	}

	WorldSnapshot World::CreateSnapshot() const
	{
		MINI_PROFILE_FUNCTION();
		WorldSnapshot snapshot{};
		snapshot.worldWidth_ = config_.width;
		snapshot.worldHeight_ = config_.height;
		snapshot.cellSize_ = config_.cellSize;
		snapshot.frameNumber_ = frameNumber_;
		snapshot.timeSeconds_ = timeSeconds_;
		snapshot.actors_.reserve( actors_.size() );

		for( const auto& entry : actors_ )
		{
			const Actor& actor = *entry.second;
			ActorSnapshotData snapshotActor{};
			snapshotActor.id = actor.GetId();
			snapshotActor.kind = actor.GetKind();
			snapshotActor.name = actor.GetName();
			snapshotActor.position = actor.GetPosition();
			snapshotActor.size = actor.GetSize();
			snapshotActor.spriteId = actor.GetSpriteId();
			snapshotActor.active = actor.IsActive();
			snapshotActor.alive = actor.IsAlive();
			snapshotActor.blocksCell = actor.BlocksCell();
			snapshotActor.health = actor.GetHealth();
			snapshot.actors_.push_back( std::move( snapshotActor ) );
		}

		return snapshot;
	}

	std::vector<ActorId> World::GetTickableActors( TickGroup group ) const
	{
		std::vector<ActorId> actorIds;
		for( const auto& entry : actors_ )
		{
			const Actor& actor = *entry.second;
			if( !actor.IsAlive() || !actor.IsActive() )
			{
				continue;
			}

			const PrimaryActorTick& tick = actor.GetPrimaryActorTick();
			if( tick.CanTick() && tick.tickGroup == group )
			{
				actorIds.push_back( actor.GetId() );
			}
		}

		return actorIds;
	}

	void World::EnqueueCommand( GameCommand command )
	{
		queuedCommands_.push_back( std::move( command ) );
	}

	std::vector<GameCommand> World::ConsumeQueuedCommands()
	{
		std::vector<GameCommand> commands = std::move( queuedCommands_ );
		queuedCommands_.clear();
		return commands;
	}

	void World::ApplyCommands( const std::vector<GameCommand>& commands )
	{
		MINI_PROFILE_FUNCTION();
		for( const GameCommand& command : commands )
		{
			ApplyCommand( command );
		}

		RemoveDestroyedActors();
	}

	uint32_t World::CountActorsOfKind( ActorKind kind ) const
	{
		uint32_t count = 0;
		for( const auto& entry : actors_ )
		{
			const Actor& actor = *entry.second;
			if( actor.IsAlive() && actor.IsActive() && actor.GetKind() == kind )
			{
				++count;
			}
		}

		return count;
	}

	uint32_t World::GetPlayerHealth() const
	{
		for( const auto& entry : actors_ )
		{
			const Actor& actor = *entry.second;
			if( actor.GetKind() == ActorKind::Player && actor.IsAlive() )
			{
				return static_cast<uint32_t>( actor.GetHealth() );
			}
		}

		return 0;
	}

	std::vector<SpriteSceneProxy> World::BuildSceneProxies() const
	{
		MINI_PROFILE_FUNCTION();
		std::vector<SpriteSceneProxy> proxies;
		proxies.reserve( actors_.size() );

		for( const auto& entry : actors_ )
		{
			const Actor& actor = *entry.second;
			SpriteSceneProxy proxy = actor.CreateSceneProxy();
			if( proxy.visible )
			{
				proxies.push_back( std::move( proxy ) );
			}
		}

		std::sort(
			proxies.begin(),
			proxies.end(),
			[]( const SpriteSceneProxy& left, const SpriteSceneProxy& right )
			{
				if( left.renderLayer != right.renderLayer )
				{
					return left.renderLayer < right.renderLayer;
				}

				if( left.position.y != right.position.y )
				{
					return left.position.y < right.position.y;
				}

				return left.actorId < right.actorId;
			} );

		return proxies;
	}

	std::vector<std::string> World::GetRecentDebugLines() const
	{
		return { debugEvents_.begin(), debugEvents_.end() };
	}

	void World::AddDebugEvent( const std::string& message )
	{
		std::ostringstream line;
		line << "[" << static_cast<int>( frameNumber_ ) << "] " << message;
		debugEvents_.push_front( line.str() );
		while( debugEvents_.size() > config_.maxDebugEvents )
		{
			debugEvents_.pop_back();
		}

		std::string output = line.str() + "\n";
		OutputDebugStringA( output.c_str() );
	}

	void World::RemoveDestroyedActors()
	{
		for( auto iterator = actors_.begin(); iterator != actors_.end(); )
		{
			if( !iterator->second->IsAlive() )
			{
				iterator = actors_.erase( iterator );
			}
			else
			{
				++iterator;
			}
		}
	}

	Vector2 World::FindRespawnPosition( const Vector2& size ) const
	{
		const float padding = config_.cellSize;
		const std::array<Vector2, 5> candidates = {
			Vector2{ padding, padding },
			Vector2{ config_.width - size.x - padding, padding },
			Vector2{ padding, config_.height - size.y - padding },
			Vector2{ config_.width - size.x - padding, config_.height - size.y - padding },
			Vector2{ config_.width * 0.5f - size.x * 0.5f, config_.height * 0.5f - size.y * 0.5f }
		};

		Vector2 bestPosition = ClampToWorld( candidates.front(), size );
		float bestScore = -1.0f;

		for( const Vector2& candidate : candidates )
		{
			const Vector2 position = ClampToWorld( candidate, size );
			if( !CanMoveActorTo( ourInvalidActorId, position, size ) )
			{
				continue;
			}

			const Vector2 candidateCenter = ComputeCellCenter( position, size );
			float nearestEnemyDistanceSquared = std::numeric_limits<float>::max();
			bool foundEnemy = false;

			for( const auto& entry : actors_ )
			{
				const Actor& actor = *entry.second;
				if( !actor.IsAlive() || !actor.IsActive() || actor.GetKind() != ActorKind::Enemy )
				{
					continue;
				}

				const Vector2 enemyCenter = ComputeCellCenter( actor.GetPosition(), actor.GetSize() );
				const float distanceSquared = LengthSquared( candidateCenter - enemyCenter );
				nearestEnemyDistanceSquared = std::min( nearestEnemyDistanceSquared, distanceSquared );
				foundEnemy = true;
			}

			const float score = foundEnemy ? nearestEnemyDistanceSquared : std::numeric_limits<float>::max();
			if( score > bestScore )
			{
				bestScore = score;
				bestPosition = position;
			}
		}

		return bestPosition;
	}

	void World::ApplyCommand( const GameCommand& command )
	{
		std::visit(
			[this]( const auto& concreteCommand )
			{
				using CommandType = std::decay_t<decltype( concreteCommand )>;

				if constexpr( std::is_same<CommandType, MoveActorCommand>::value )
				{
					if( Actor* actor = FindActor( concreteCommand.actorId ) )
					{
						const Vector2 targetPosition = ClampToWorld( concreteCommand.newPosition, actor->GetSize() );
						if( CanMoveActorTo( actor->GetId(), targetPosition, actor->GetSize() ) )
						{
							actor->SetPosition( targetPosition );
						}
					}
				}
				else if constexpr( std::is_same<CommandType, DamageActorCommand>::value )
				{
					if( Actor* actor = FindActor( concreteCommand.actorId ) )
					{
						actor->ApplyDamage( concreteCommand.amount );
						std::ostringstream stream;
						stream << actor->GetName() << " took " << concreteCommand.amount << " damage";
						AddDebugEvent( stream.str() );

						if( !actor->IsAlive() )
						{
							if( actor->GetKind() == ActorKind::Player )
							{
								AddDebugEvent( actor->GetName() + " was defeated" );
								actor->SetPosition( FindRespawnPosition( actor->GetSize() ) );
								actor->Revive( 12 );
								AddDebugEvent( actor->GetName() + " respawned" );
							}
							else
							{
								AddDebugEvent( actor->GetName() + " was destroyed" );
							}
						}
					}
				}
				else if constexpr( std::is_same<CommandType, DestroyActorCommand>::value )
				{
					if( Actor* actor = FindActor( concreteCommand.actorId ) )
					{
						AddDebugEvent( actor->GetName() + " was destroyed" );
						actor->MarkDestroyed();
					}
				}
				else if constexpr( std::is_same<CommandType, SetSpriteCommand>::value )
				{
					if( Actor* actor = FindActor( concreteCommand.actorId ) )
					{
						actor->SetSpriteId( concreteCommand.spriteId );
					}
				}
				else if constexpr( std::is_same<CommandType, SetAnimationFrameCommand>::value )
				{
					if( Actor* actor = FindActor( concreteCommand.actorId ) )
					{
						actor->SetAnimationFrame( concreteCommand.frameIndex );
					}
				}
				else if constexpr( std::is_same<CommandType, SpawnEnemyCommand>::value )
				{
					EnemyActor& enemy = SpawnActor<EnemyActor>( concreteCommand.name );
					enemy.SetPosition( ClampToWorld( concreteCommand.position, enemy.GetSize() ) );
					std::ostringstream stream;
					stream << "Spawner created " << enemy.GetName();
					AddDebugEvent( stream.str() );
				}
			},
			command );
	}
}
