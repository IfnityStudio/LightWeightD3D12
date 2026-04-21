#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace mini2d
{
	using ActorId = uint32_t;
	static constexpr ActorId ourInvalidActorId = 0;

	struct Vector2
	{
		float x = 0.0f;
		float y = 0.0f;
	};

	inline Vector2 operator+( const Vector2& left, const Vector2& right )
	{
		return { left.x + right.x, left.y + right.y };
	}

	inline Vector2 operator-( const Vector2& left, const Vector2& right )
	{
		return { left.x - right.x, left.y - right.y };
	}

	inline Vector2 operator*( const Vector2& value, float scalar )
	{
		return { value.x * scalar, value.y * scalar };
	}

	inline Vector2 operator/( const Vector2& value, float scalar )
	{
		return { value.x / scalar, value.y / scalar };
	}

	inline Vector2& operator+=( Vector2& left, const Vector2& right )
	{
		left.x += right.x;
		left.y += right.y;
		return left;
	}

	inline float LengthSquared( const Vector2& value )
	{
		return value.x * value.x + value.y * value.y;
	}

	inline float Length( const Vector2& value )
	{
		return std::sqrt( LengthSquared( value ) );
	}

	inline Vector2 Normalize( const Vector2& value )
	{
		const float length = Length( value );
		if( length <= 0.0001f )
		{
			return {};
		}

		return value / length;
	}

	inline float ClampFloat( float value, float minimum, float maximum )
	{
		return std::max( minimum, std::min( value, maximum ) );
	}

	inline Vector2 Lerp( const Vector2& start, const Vector2& end, float alpha )
	{
		return start + ( end - start ) * alpha;
	}

	struct LinearColor
	{
		float r = 1.0f;
		float g = 1.0f;
		float b = 1.0f;
		float a = 1.0f;
	};

	enum class TickGroup : uint8_t
	{
		PrePhysics,
		DuringPhysics,
		PostPhysics,
		PostUpdateWork
	};

	inline const char* ToString( TickGroup group )
	{
		switch( group )
		{
			case TickGroup::PrePhysics:
				return "PrePhysics";
			case TickGroup::DuringPhysics:
				return "DuringPhysics";
			case TickGroup::PostPhysics:
				return "PostPhysics";
			case TickGroup::PostUpdateWork:
				return "PostUpdateWork";
			default:
				return "Unknown";
		}
	}

	enum class ActorKind : uint8_t
	{
		Player,
		Enemy,
		Spawner,
		Cursor,
		Follower
	};

	inline const char* ToString( ActorKind kind )
	{
		switch( kind )
		{
			case ActorKind::Player:
				return "Player";
			case ActorKind::Enemy:
				return "Enemy";
			case ActorKind::Spawner:
				return "Spawner";
			case ActorKind::Cursor:
				return "Cursor";
			case ActorKind::Follower:
				return "Follower";
			default:
				return "Unknown";
		}
	}

	enum class SpriteId : uint8_t
	{
		WhiteSquare,
		Enemy
	};

	struct GameInputState
	{
		bool moveUp = false;
		bool moveDown = false;
		bool moveLeft = false;
		bool moveRight = false;
		Vector2 mousePosition = {};
	};
}
