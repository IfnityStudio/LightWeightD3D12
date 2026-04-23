#pragma once

#if defined( TRACY_ENABLE )
	#include <tracy/Tracy.hpp>
#endif

namespace mini2d
{
	inline void SetProfilerThreadName( const char* threadName )
	{
		#if defined( TRACY_ENABLE )
			tracy::SetThreadName( threadName );
		#else
			(void)threadName;
		#endif
	}
}

#if defined( TRACY_ENABLE )
	#define MINI_PROFILE_SCOPE(name) ZoneScopedN( name )
	#define MINI_PROFILE_FUNCTION() ZoneScoped
	#define MINI_PROFILE_FRAME() FrameMark
#else
	#define MINI_PROFILE_SCOPE(name) (void)(name)
	#define MINI_PROFILE_FUNCTION() ((void)0)
	#define MINI_PROFILE_FRAME() ((void)0)
#endif
