#pragma once

#include "GameCommand.hpp"
#include "RenderTypes.hpp"
#include "TickFunction.hpp"

#include <string>
#include <vector>

namespace mini2d
{
	class World;
	class WorldSnapshot;

	class Actor
	{
	public:
		Actor( ActorId id, std::string name, ActorKind kind );
		virtual ~Actor() = default;

		ActorId GetId() const noexcept;
		const std::string& GetName() const noexcept;
		ActorKind GetKind() const noexcept;
		const Vector2& GetPosition() const noexcept;
		void SetPosition( const Vector2& position ) noexcept;
		const Vector2& GetSize() const noexcept;
		void SetSize( const Vector2& size ) noexcept;
		SpriteId GetSpriteId() const noexcept;
		void SetSpriteId( SpriteId spriteId ) noexcept;
		const LinearColor& GetTint() const noexcept;
		void SetTint( const LinearColor& tint ) noexcept;
		int GetHealth() const noexcept;
		void SetHealth( int health ) noexcept;
		void ApplyDamage( int amount ) noexcept;
		void Revive( int health ) noexcept;
		bool IsActive() const noexcept;
		void SetActive( bool active ) noexcept;
		bool IsAlive() const noexcept;
		void MarkDestroyed() noexcept;
		bool IsVisible() const noexcept;
		void SetVisible( bool visible ) noexcept;
		bool BlocksCell() const noexcept;
		void SetBlocksCell( bool blocksCell ) noexcept;
		uint32_t GetAnimationFrame() const noexcept;
		void SetAnimationFrame( uint32_t frameIndex ) noexcept;
		int GetRenderLayer() const noexcept;
		void SetRenderLayer( int renderLayer ) noexcept;

		PrimaryActorTick& GetPrimaryActorTick() noexcept;
		const PrimaryActorTick& GetPrimaryActorTick() const noexcept;

		virtual void BeginPlay( World& world );
		virtual void Tick( World& world, float deltaTime );
		virtual std::vector<GameCommand> TickAsync( const WorldSnapshot& snapshot, float deltaTime ) const;
		virtual SpriteSceneProxy CreateSceneProxy() const;

	private:
		ActorId id_ = ourInvalidActorId;
		std::string name_;
		ActorKind kind_ = ActorKind::Enemy;
		Vector2 position_ = {};
		Vector2 size_ = { 32.0f, 32.0f };
		SpriteId spriteId_ = SpriteId::WhiteSquare;
		LinearColor tint_ = {};
		int health_ = 1;
		bool active_ = true;
		bool alive_ = true;
		bool visible_ = true;
		bool blocksCell_ = true;
		uint32_t animationFrame_ = 0;
		int renderLayer_ = 0;
		PrimaryActorTick primaryActorTick_ = {};
	};
}
