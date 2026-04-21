#pragma once

#include "EngineTypes.hpp"

#include <vector>

namespace mini2d
{
	struct PrimaryActorTick
	{
		bool canEverTick = true;
		bool tickEnabled = true;
		bool runOnAnyThread = false;
		TickGroup tickGroup = TickGroup::PrePhysics;
		std::vector<ActorId> prerequisites = {};

		bool CanTick() const
		{
			return canEverTick && tickEnabled;
		}

		void AddPrerequisite( ActorId actorId )
		{
			if( actorId == ourInvalidActorId )
			{
				return;
			}

			for( const ActorId prerequisite : prerequisites )
			{
				if( prerequisite == actorId )
				{
					return;
				}
			}

			prerequisites.push_back( actorId );
		}
	};
}
