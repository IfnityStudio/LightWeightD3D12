#include "Actor.hpp"

namespace mini2d
{
	Actor::Actor( ActorId id, std::string name, ActorKind kind ):
		id_( id ),
		name_( std::move( name ) ),
		kind_( kind )
	{
	}

	ActorId Actor::GetId() const noexcept
	{
		return id_;
	}

	const std::string& Actor::GetName() const noexcept
	{
		return name_;
	}

	ActorKind Actor::GetKind() const noexcept
	{
		return kind_;
	}

	const Vector2& Actor::GetPosition() const noexcept
	{
		return position_;
	}

	void Actor::SetPosition( const Vector2& position ) noexcept
	{
		position_ = position;
	}

	const Vector2& Actor::GetSize() const noexcept
	{
		return size_;
	}

	void Actor::SetSize( const Vector2& size ) noexcept
	{
		size_ = size;
	}

	SpriteId Actor::GetSpriteId() const noexcept
	{
		return spriteId_;
	}

	void Actor::SetSpriteId( SpriteId spriteId ) noexcept
	{
		spriteId_ = spriteId;
	}

	const LinearColor& Actor::GetTint() const noexcept
	{
		return tint_;
	}

	void Actor::SetTint( const LinearColor& tint ) noexcept
	{
		tint_ = tint;
	}

	int Actor::GetHealth() const noexcept
	{
		return health_;
	}

	void Actor::SetHealth( int health ) noexcept
	{
		health_ = health;
	}

	void Actor::ApplyDamage( int amount ) noexcept
	{
		health_ -= amount;
		if( health_ <= 0 )
		{
			health_ = 0;
			alive_ = false;
			active_ = false;
		}
	}

	void Actor::Revive( int health ) noexcept
	{
		health_ = std::max( 1, health );
		alive_ = true;
		active_ = true;
	}

	bool Actor::IsActive() const noexcept
	{
		return active_;
	}

	void Actor::SetActive( bool active ) noexcept
	{
		active_ = active;
	}

	bool Actor::IsAlive() const noexcept
	{
		return alive_;
	}

	void Actor::MarkDestroyed() noexcept
	{
		alive_ = false;
		active_ = false;
	}

	bool Actor::IsVisible() const noexcept
	{
		return visible_;
	}

	void Actor::SetVisible( bool visible ) noexcept
	{
		visible_ = visible;
	}

	bool Actor::BlocksCell() const noexcept
	{
		return blocksCell_;
	}

	void Actor::SetBlocksCell( bool blocksCell ) noexcept
	{
		blocksCell_ = blocksCell;
	}

	uint32_t Actor::GetAnimationFrame() const noexcept
	{
		return animationFrame_;
	}

	void Actor::SetAnimationFrame( uint32_t frameIndex ) noexcept
	{
		animationFrame_ = frameIndex;
	}

	int Actor::GetRenderLayer() const noexcept
	{
		return renderLayer_;
	}

	void Actor::SetRenderLayer( int renderLayer ) noexcept
	{
		renderLayer_ = renderLayer;
	}

	PrimaryActorTick& Actor::GetPrimaryActorTick() noexcept
	{
		return primaryActorTick_;
	}

	const PrimaryActorTick& Actor::GetPrimaryActorTick() const noexcept
	{
		return primaryActorTick_;
	}

	void Actor::BeginPlay( World& )
	{
	}

	void Actor::Tick( World&, float )
	{
	}

	std::vector<GameCommand> Actor::TickAsync( const WorldSnapshot&, float ) const
	{
		return {};
	}

	SpriteSceneProxy Actor::CreateSceneProxy() const
	{
		SpriteSceneProxy proxy{};
		proxy.actorId = id_;
		proxy.spriteId = spriteId_;
		proxy.position = position_;
		proxy.size = size_;
		proxy.tint = tint_;
		proxy.renderLayer = renderLayer_;
		proxy.visible = visible_ && alive_ && active_;
		proxy.animationFrame = animationFrame_;
		return proxy;
	}
}
