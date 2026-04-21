#pragma once

#include "EngineTypes.hpp"

#include <vector>

namespace mini2d
{
	class TaskScheduler;
	class World;

	struct TickExecutionStats
	{
		uint32_t syncTicks = 0;
		uint32_t asyncTicks = 0;
		uint32_t commandsApplied = 0;
	};

	class TickScheduler
	{
	public:
		TickExecutionStats ExecuteFrame( World& world, TaskScheduler& taskScheduler, float deltaTime );

	private:
		std::vector<std::vector<ActorId>> BuildTickLayers( const World& world, TickGroup group ) const;
	};
}
