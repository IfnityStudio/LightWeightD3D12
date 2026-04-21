#include "Actors.hpp"

#include "World.hpp"
#include "WorldSnapshot.hpp"

#include <array>
#include <sstream>

namespace mini2d
{
	PlayerActor::PlayerActor( ActorId id, std::string name ):
		Actor( id, std::move( name ), ActorKind::Player )
	{
		SetSize( { 36.0f, 36.0f } );
		SetTint( { 0.25f, 0.72f, 1.0f, 1.0f } );
		SetSpriteId( SpriteId::WhiteSquare );
		SetRenderLayer( 50 );
		SetHealth( 12 );
		GetPrimaryActorTick().tickGroup = TickGroup::PrePhysics;
		GetPrimaryActorTick().runOnAnyThread = false;
	}

	void PlayerActor::Tick( World& world, float deltaTime )
	{
		const GameInputState& input = world.GetInputState();
		Vector2 direction{};
		if( input.moveLeft )
		{
			direction.x -= 1.0f;
		}
		if( input.moveRight )
		{
			direction.x += 1.0f;
		}
		if( input.moveUp )
		{
			direction.y -= 1.0f;
		}
		if( input.moveDown )
		{
			direction.y += 1.0f;
		}

		if( LengthSquared( direction ) <= 0.0001f )
		{
			return;
		}

		const Vector2 moveDirection = Normalize( direction );
		const Vector2 desiredPosition = GetPosition() + moveDirection * ( moveSpeed_ * deltaTime );
		const Vector2 clampedPosition = world.ClampToWorld( desiredPosition, GetSize() );
		if( world.CanMoveActorTo( GetId(), clampedPosition, GetSize() ) )
		{
			SetPosition( clampedPosition );
		}
	}

	EnemyActor::EnemyActor( ActorId id, std::string name, float moveSpeed ):
		Actor( id, std::move( name ), ActorKind::Enemy ),
		moveSpeed_( moveSpeed )
	{
		SetSize( { 42.0f, 42.0f } );
		SetTint( { 1.0f, 1.0f, 1.0f, 1.0f } );
		SetSpriteId( SpriteId::Enemy );
		SetRenderLayer( 40 );
		SetHealth( 1 );
		GetPrimaryActorTick().tickGroup = TickGroup::DuringPhysics;
		GetPrimaryActorTick().runOnAnyThread = true;
	}

	std::vector<GameCommand> EnemyActor::TickAsync( const WorldSnapshot& snapshot, float deltaTime ) const
	{
		std::vector<GameCommand> commands;

		const ActorSnapshotData* self = snapshot.FindActor( GetId() );
		const ActorSnapshotData* player = snapshot.FindFirstActorOfKind( ActorKind::Player );
		if( self == nullptr || player == nullptr || !self->alive || !player->alive )
		{
			return commands;
		}

		const Vector2 toPlayer = player->position - self->position;
		const float distance = Length( toPlayer );
		if( distance <= attackRange_ )
		{
			if( snapshot.GetFrameNumber() % 12u == 0u )
			{
				commands.push_back( DamageActorCommand{ player->id, 1 } );
			}
			return commands;
		}

		const Vector2 moveDirection = Normalize( toPlayer );
		const Vector2 desiredPosition = snapshot.ClampToWorld( self->position + moveDirection * ( moveSpeed_ * deltaTime ), self->size );
		if( !snapshot.IsCellBlocked( desiredPosition, self->size, self->id ) )
		{
			commands.push_back( MoveActorCommand{ self->id, desiredPosition } );
			return commands;
		}

		const Vector2 axisMoveX = snapshot.ClampToWorld( { desiredPosition.x, self->position.y }, self->size );
		if( !snapshot.IsCellBlocked( axisMoveX, self->size, self->id ) )
		{
			commands.push_back( MoveActorCommand{ self->id, axisMoveX } );
			return commands;
		}

		const Vector2 axisMoveY = snapshot.ClampToWorld( { self->position.x, desiredPosition.y }, self->size );
		if( !snapshot.IsCellBlocked( axisMoveY, self->size, self->id ) )
		{
			commands.push_back( MoveActorCommand{ self->id, axisMoveY } );
		}

		return commands;
	}

	SpawnerActor::SpawnerActor( ActorId id, std::string name ):
		Actor( id, std::move( name ), ActorKind::Spawner ),
		randomGenerator_( std::random_device{}() )
	{
		SetSize( { 30.0f, 30.0f } );
		SetTint( { 0.25f, 1.0f, 0.45f, 1.0f } );
		SetSpriteId( SpriteId::WhiteSquare );
		SetRenderLayer( 20 );
		SetBlocksCell( false );
		GetPrimaryActorTick().tickGroup = TickGroup::PostUpdateWork;
		GetPrimaryActorTick().runOnAnyThread = false;
	}

	void SpawnerActor::Tick( World& world, float deltaTime )
	{
		remainingTime_ -= deltaTime;
		if( remainingTime_ > 0.0f )
		{
			return;
		}

		remainingTime_ += spawnInterval_;
		if( world.CountActorsOfKind( ActorKind::Enemy ) >= 10u )
		{
			return;
		}

		const float cell = world.GetCellSize();
		const float maxX = world.GetWorldWidth() - cell * 2.0f;
		const float maxY = world.GetWorldHeight() - cell * 2.0f;
		const std::array<Vector2, 4> spawnPoints = {
			Vector2{ cell, cell },
			Vector2{ maxX, cell },
			Vector2{ cell, maxY },
			Vector2{ maxX, maxY }
		};

		std::uniform_int_distribution<size_t> distribution( 0, spawnPoints.size() - 1 );
		const Vector2 spawnPosition = spawnPoints[ distribution( randomGenerator_ ) ];

		std::ostringstream stream;
		stream << "Enemy_" << spawnCounter_++;
		world.EnqueueCommand( SpawnEnemyCommand{ stream.str(), spawnPosition } );
	}

	CursorActor::CursorActor( ActorId id, std::string name ):
		Actor( id, std::move( name ), ActorKind::Cursor )
	{
		SetSize( { 14.0f, 14.0f } );
		SetTint( { 1.0f, 0.25f, 0.35f, 0.95f } );
		SetSpriteId( SpriteId::WhiteSquare );
		SetRenderLayer( 110 );
		SetBlocksCell( false );
		GetPrimaryActorTick().tickGroup = TickGroup::PostUpdateWork;
		GetPrimaryActorTick().runOnAnyThread = false;
	}

	void CursorActor::Tick( World& world, float )
	{
		const Vector2 targetPosition = world.ClampToWorld(
			{
				world.GetInputState().mousePosition.x - GetSize().x * 0.5f,
				world.GetInputState().mousePosition.y - GetSize().y * 0.5f
			},
			GetSize() );
		SetPosition( targetPosition );
	}

	SpriteFollowerActor::SpriteFollowerActor( ActorId id, std::string name, ActorId targetActorId ):
		Actor( id, std::move( name ), ActorKind::Follower ),
		targetActorId_( targetActorId )
	{
		SetSize( { 18.0f, 18.0f } );
		SetTint( { 1.0f, 0.85f, 0.10f, 0.85f } );
		SetSpriteId( SpriteId::WhiteSquare );
		SetRenderLayer( 105 );
		SetBlocksCell( false );
		GetPrimaryActorTick().tickGroup = TickGroup::PostUpdateWork;
		GetPrimaryActorTick().runOnAnyThread = false;
		GetPrimaryActorTick().AddPrerequisite( targetActorId_ );
	}

	void SpriteFollowerActor::Tick( World& world, float deltaTime )
	{
		const Actor* target = world.FindActor( targetActorId_ );
		if( target == nullptr )
		{
			return;
		}

		const Vector2 targetPosition = target->GetPosition() + offset_;
		const float alpha = ClampFloat( deltaTime * followStrength_, 0.0f, 1.0f );
		SetPosition( world.ClampToWorld( Lerp( GetPosition(), targetPosition, alpha ), GetSize() ) );
	}
}
