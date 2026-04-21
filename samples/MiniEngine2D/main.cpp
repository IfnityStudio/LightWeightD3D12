#include "GameEngine.hpp"

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE, PWSTR, int showCommand )
{
	mini2d::GameEngine engine;
	return engine.Run( instance, showCommand );
}
