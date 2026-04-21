#pragma once

#include <tracy/Tracy.hpp>

namespace mini2d
{
	inline void SetProfilerThreadName( const char* threadName )
	{
		tracy::SetThreadName( threadName );
	}
}

#define MINI_PROFILE_SCOPE(name) ZoneScopedN( name )
#define MINI_PROFILE_FUNCTION() ZoneScoped
#define MINI_PROFILE_FRAME() FrameMark
