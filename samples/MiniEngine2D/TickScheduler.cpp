#include "TickScheduler.hpp"

#include "Actor.hpp"
#include "Profiler.hpp"
#include "TaskScheduler.hpp"
#include "World.hpp"
#include "WorldSnapshot.hpp"

#include <array>
#include <future>
#include <unordered_map>

namespace mini2d
{
	TickExecutionStats TickScheduler::ExecuteFrame( World& world, TaskScheduler& taskScheduler, float deltaTime )
	{
		MINI_PROFILE_FUNCTION();
		TickExecutionStats stats{};
		static constexpr std::array<TickGroup, 4> ourTickOrder = {
			TickGroup::PrePhysics,
			TickGroup::DuringPhysics,
			TickGroup::PostPhysics,
			TickGroup::PostUpdateWork
		};

		for( TickGroup group : ourTickOrder )
		{
			const std::vector<std::vector<ActorId>> layers = BuildTickLayers( world, group );
			for( const std::vector<ActorId>& layer : layers )
			{
				MINI_PROFILE_SCOPE( "TickScheduler::ExecuteLayer" );
				const WorldSnapshot snapshot = world.CreateSnapshot();
				std::vector<std::future<std::vector<GameCommand>>> asyncJobs;

				for( ActorId actorId : layer )
				{
					Actor* actor = world.FindActor( actorId );
					if( actor == nullptr || !actor->IsAlive() || !actor->IsActive() || !actor->GetPrimaryActorTick().CanTick() )
					{
						continue;
					}

					if( actor->GetPrimaryActorTick().runOnAnyThread )
					{
						const Actor* constActor = actor;
						asyncJobs.push_back( taskScheduler.Submit( [snapshot, constActor, deltaTime]()
							{
								return constActor->TickAsync( snapshot, deltaTime );
							} ) );
						++stats.asyncTicks;
					}
					else
					{
						actor->Tick( world, deltaTime );
						++stats.syncTicks;
					}
				}

				std::vector<GameCommand> commands = world.ConsumeQueuedCommands();
				for( std::future<std::vector<GameCommand>>& asyncJob : asyncJobs )
				{
					std::vector<GameCommand> asyncCommands = asyncJob.get();
					commands.insert( commands.end(), asyncCommands.begin(), asyncCommands.end() );
				}

				stats.commandsApplied += static_cast<uint32_t>( commands.size() );
				world.ApplyCommands( commands );
			}
		}

		return stats;
	}

	std::vector<std::vector<ActorId>> TickScheduler::BuildTickLayers( const World& world, TickGroup group ) const
	{
		MINI_PROFILE_SCOPE( "TickScheduler::BuildTickLayers" );
		const std::vector<ActorId> actorIds = world.GetTickableActors( group );
		std::unordered_map<ActorId, size_t> indicesByActorId;
		indicesByActorId.reserve( actorIds.size() );

		for( size_t index = 0; index < actorIds.size(); ++index )
		{
			indicesByActorId.emplace( actorIds[ index ], index );
		}

		std::vector<uint32_t> incomingCounts( actorIds.size(), 0u );
		std::vector<std::vector<ActorId>> outgoingEdges( actorIds.size() );

		for( ActorId actorId : actorIds )
		{
			const Actor* actor = world.FindActor( actorId );
			if( actor == nullptr )
			{
				continue;
			}

			const size_t actorIndex = indicesByActorId[ actorId ];
			for( ActorId prerequisiteId : actor->GetPrimaryActorTick().prerequisites )
			{
				const auto prerequisiteIterator = indicesByActorId.find( prerequisiteId );
				if( prerequisiteIterator == indicesByActorId.end() )
				{
					continue;
				}

				outgoingEdges[ prerequisiteIterator->second ].push_back( actorId );
				++incomingCounts[ actorIndex ];
			}
		}

		std::vector<ActorId> readyActors;
		for( ActorId actorId : actorIds )
		{
			if( incomingCounts[ indicesByActorId[ actorId ] ] == 0u )
			{
				readyActors.push_back( actorId );
			}
		}

		std::vector<std::vector<ActorId>> layers;
		std::vector<bool> emitted( actorIds.size(), false );
		size_t processedCount = 0;

		while( !readyActors.empty() )
		{
			std::vector<ActorId> currentLayer = std::move( readyActors );
			readyActors.clear();
			layers.push_back( currentLayer );

			for( ActorId actorId : layers.back() )
			{
				const size_t actorIndex = indicesByActorId[ actorId ];
				emitted[ actorIndex ] = true;
				++processedCount;

				for( ActorId dependentActorId : outgoingEdges[ actorIndex ] )
				{
					const size_t dependentIndex = indicesByActorId[ dependentActorId ];
					if( incomingCounts[ dependentIndex ] > 0u )
					{
						--incomingCounts[ dependentIndex ];
						if( incomingCounts[ dependentIndex ] == 0u )
						{
							readyActors.push_back( dependentActorId );
						}
					}
				}
			}
		}

		if( processedCount != actorIds.size() )
		{
			std::vector<ActorId> fallbackLayer;
			for( ActorId actorId : actorIds )
			{
				const size_t actorIndex = indicesByActorId[ actorId ];
				if( !emitted[ actorIndex ] )
				{
					fallbackLayer.push_back( actorId );
				}
			}

			if( !fallbackLayer.empty() )
			{
				layers.push_back( std::move( fallbackLayer ) );
			}
		}

		return layers;
	}
}
