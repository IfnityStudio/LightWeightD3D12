#pragma once

#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

namespace lightd3d12
{
	template<typename ImplObjType>
	class Handle final
	{
	public:
		Handle() = default;

		bool Valid() const noexcept
		{
			return gen_ != 0;
		}

		uint32_t Index() const noexcept
		{
			return index_;
		}

		uint32_t Gen() const noexcept
		{
			return gen_;
		}

		bool Empty() const noexcept
		{
			return gen_ == 0;
		}

		bool operator==( const Handle& other ) const noexcept
		{
			return index_ == other.index_ && gen_ == other.gen_;
		}

		bool operator!=( const Handle& other ) const noexcept
		{
			return !(*this == other);
		}

		explicit operator bool() const noexcept
		{
			return gen_ != 0;
		}

		operator uint32_t() const noexcept
		{
			return gen_ != 0 ? index_ : 0x00FFFFFFu;
		}

	private:
		Handle( uint32_t index, uint32_t gen ) noexcept: index_( index ), gen_( gen ) {}

		template<typename T>
		friend class SlotMap;

		uint32_t index_ = 0;
		uint32_t gen_ = 0;
	};

	template<typename ObjectType>
	class SlotMap final
	{
		struct Slot
		{
			ObjectType obj = {};
			uint32_t gen = 1;
			bool occupied = false;
		};

	public:
		Handle<ObjectType> Create( ObjectType&& obj )
		{
			uint32_t index = 0;
			if( !freeList_.empty() )
			{
				index = freeList_.back();
				freeList_.pop_back();
				slots_[ index ].obj = std::move( obj );
				slots_[ index ].occupied = true;
			}
			else
			{
				index = static_cast< uint32_t >( slots_.size() );
				slots_.push_back( { std::move( obj ), 1u, true } );
			}

			return Handle<ObjectType>( index, slots_[ index ].gen );
		}

		std::span<const ObjectType> GetSlotsSpan() const
		{
			static thread_local std::vector<ObjectType> ourBuffer;
			ourBuffer.clear();
			ourBuffer.reserve( slots_.size() );

			for( const auto& slot : slots_ )
			{
				if( slot.occupied )
				{
					ourBuffer.push_back( slot.obj );
				}
			}

			return std::span<const ObjectType>( ourBuffer.data(), ourBuffer.size() );
		}

		void Destroy( Handle<ObjectType> handle )
		{
			if( !handle.Valid() )
			{
				return;
			}

			const uint32_t index = handle.Index();
			assert( index < slots_.size() );
			assert( handle.Gen() == slots_[ index ].gen );

			slots_[ index ].obj = {};
			slots_[ index ].gen++;
			slots_[ index ].occupied = false;
			freeList_.push_back( index );
		}

		ObjectType* Get( Handle<ObjectType> handle )
		{
			if( !handle.Valid() )
			{
				return nullptr;
			}

			const uint32_t index = handle.Index();
			assert( index < slots_.size() );
			assert( handle.Gen() == slots_[ index ].gen );
			assert( slots_[ index ].occupied );
			return &slots_[ index ].obj;
		}

		const ObjectType* Get( Handle<ObjectType> handle ) const
		{
			if( !handle.Valid() )
			{
				return nullptr;
			}

			const uint32_t index = handle.Index();
			assert( index < slots_.size() );
			assert( handle.Gen() == slots_[ index ].gen );
			assert( slots_[ index ].occupied );
			return &slots_[ index ].obj;
		}

		ObjectType* GetByIndex( uint32_t index )
		{
			if( index >= slots_.size() )
			{
				return nullptr;
			}

			auto& slot = slots_[ index ];
			if( !slot.occupied )
			{
				return nullptr;
			}

			return &slot.obj;
		}

		const ObjectType* GetByIndex( uint32_t index ) const
		{
			if( index >= slots_.size() )
			{
				return nullptr;
			}

			const auto& slot = slots_[ index ];
			if( !slot.occupied )
			{
				return nullptr;
			}

			return &slot.obj;
		}

		Handle<ObjectType> Find( const ObjectType* obj ) const
		{
			if( obj == nullptr )
			{
				return {};
			}

			for( uint32_t i = 0; i < slots_.size(); ++i )
			{
				if( slots_[ i ].occupied && &slots_[ i ].obj == obj )
				{
					return Handle<ObjectType>( i, slots_[ i ].gen );
				}
			}

			return {};
		}

		uint32_t NumObjects() const noexcept
		{
			return static_cast< uint32_t >( slots_.size() - freeList_.size() );
		}

		void Clear()
		{
			slots_.clear();
			freeList_.clear();
		}

		std::vector<ObjectType*> GetAll()
		{
			std::vector<ObjectType*> result;
			result.reserve( slots_.size() );

			for( auto& slot : slots_ )
			{
				if( slot.occupied )
				{
					result.push_back( &slot.obj );
				}
			}

			return result;
		}

		uint32_t Size() const noexcept
		{
			return static_cast< uint32_t >( slots_.size() - freeList_.size() );
		}

	private:
		std::vector<Slot> slots_;
		std::vector<uint32_t> freeList_;
	};

	static_assert( sizeof( Handle<class SlotMapTestTag> ) == sizeof( uint64_t ) );
}

