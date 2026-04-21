#pragma once

#include "Actor.hpp"
#include "WorldSnapshot.hpp"

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mini2d
{
	struct WorldConfig
	{
		float width = 1280.0f;
		float height = 720.0f;
		float cellSize = 48.0f;
		size_t maxDebugEvents = 8;
	};

	class World
	{
	public:
		explicit World( const WorldConfig& config );

		template<typename ActorType, typename... Args>
		ActorType& SpawnActor( std::string name, Args&&... args )
		{
			const ActorId newActorId = nextActorId_++;
			auto actor = std::make_unique<ActorType>( newActorId, std::move( name ), std::forward<Args>( args )... );
			ActorType& actorReference = *actor;
			actors_.emplace( newActorId, std::move( actor ) );
			actorReference.BeginPlay( *this );
			return actorReference;
		}

		Actor* FindActor( ActorId actorId );
		const Actor* FindActor( ActorId actorId ) const;
		const GameInputState& GetInputState() const noexcept;
		void SetInputState( const GameInputState& inputState ) noexcept;
		Vector2 ClampToWorld( const Vector2& position, const Vector2& size ) const;
		bool CanMoveActorTo( ActorId actorId, const Vector2& position, const Vector2& size ) const;
		void SetFrameContext( uint32_t frameNumber, float timeSeconds ) noexcept;
		void SetWorldSize( float width, float height ) noexcept;
		float GetWorldWidth() const noexcept;
		float GetWorldHeight() const noexcept;
		float GetCellSize() const noexcept;
		uint32_t GetFrameNumber() const noexcept;
		float GetTimeSeconds() const noexcept;

		WorldSnapshot CreateSnapshot() const;
		std::vector<ActorId> GetTickableActors( TickGroup group ) const;
		void EnqueueCommand( GameCommand command );
		std::vector<GameCommand> ConsumeQueuedCommands();
		void ApplyCommands( const std::vector<GameCommand>& commands );
		uint32_t CountActorsOfKind( ActorKind kind ) const;
		uint32_t GetPlayerHealth() const;
		std::vector<SpriteSceneProxy> BuildSceneProxies() const;
		std::vector<std::string> GetRecentDebugLines() const;
		void AddDebugEvent( const std::string& message );
		void RemoveDestroyedActors();

	private:
		void ApplyCommand( const GameCommand& command );
		Vector2 FindRespawnPosition( const Vector2& size ) const;

		WorldConfig config_;
		std::unordered_map<ActorId, std::unique_ptr<Actor>> actors_ = {};
		std::vector<GameCommand> queuedCommands_ = {};
		std::deque<std::string> debugEvents_ = {};
		GameInputState inputState_ = {};
		ActorId nextActorId_ = 1;
		uint32_t frameNumber_ = 0;
		float timeSeconds_ = 0.0f;
	};
}
