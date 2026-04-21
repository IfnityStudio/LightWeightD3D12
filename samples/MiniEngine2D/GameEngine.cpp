#include "GameEngine.hpp"

#include "Profiler.hpp"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace mini2d
{
	GameEngine* GameEngine::ourInstance_ = nullptr;
	RenderThread GameEngine::ourRenderThread_ = {};

	namespace
	{
		const wchar_t* ourWindowClassName = L"MiniEngine2DWindowClass";
	}

	GameEngine::GameEngine()
	{
		if( ourInstance_ != nullptr )
		{
			throw std::runtime_error( "Only one GameEngine instance is allowed." );
		}

		ourInstance_ = this;
	}

	GameEngine::~GameEngine()
	{
		if( ourInstance_ == this )
		{
			ourInstance_ = nullptr;
		}
	}

	GameEngine& GameEngine::Get()
	{
		if( ourInstance_ == nullptr )
		{
			throw std::runtime_error( "GameEngine has not been created yet." );
		}

		return *ourInstance_;
	}

	RenderThread& GameEngine::GetRender()
	{
		return ourRenderThread_;
	}

	int GameEngine::Run( HINSTANCE instance, int showCommand )
	{
		try
		{
			SetProfilerThreadName( "GameThread" );

			if( !Initialize( instance, showCommand ) )
			{
				return 1;
			}

			const auto startTime = std::chrono::steady_clock::now();
			auto previousTime = startTime;

			MSG message{};
			while( running_ )
			{
				while( PeekMessage( &message, nullptr, 0, 0, PM_REMOVE ) )
				{
					if( message.message == WM_QUIT )
					{
						running_ = false;
						break;
					}

					TranslateMessage( &message );
					DispatchMessage( &message );
				}

				if( !running_ )
				{
					break;
				}

				const auto currentTime = std::chrono::steady_clock::now();
				const float deltaTime = ClampFloat( std::chrono::duration<float>( currentTime - previousTime ).count(), 0.0f, 0.05f );
				previousTime = currentTime;

				if( minimized_ )
				{
					Sleep( 16 );
					continue;
				}

				accumulatedTime_ = std::chrono::duration<float>( currentTime - startTime ).count();
				const float instantaneousFramesPerSecond = deltaTime > 0.0001f ? 1.0f / deltaTime : 0.0f;
				smoothedFramesPerSecond_ = smoothedFramesPerSecond_ * 0.9f + instantaneousFramesPerSecond * 0.1f;

				TickFrame( deltaTime );
			}

			Shutdown();
			return 0;
		}
		catch( const std::exception& )
		{
			Shutdown();
			MessageBoxA( nullptr, "MiniEngine2D failed.", "MiniEngine2D", MB_OK | MB_ICONERROR );
			return 1;
		}
	}

	bool GameEngine::Initialize( HINSTANCE instance, int showCommand )
	{
		MINI_PROFILE_FUNCTION();
		instance_ = instance;

		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEXW );
		windowClass.lpfnWndProc = StaticWindowProc;
		windowClass.hInstance = instance_;
		windowClass.lpszClassName = ourWindowClassName;
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		hwnd_ = CreateWindowExW(
			0,
			ourWindowClassName,
			L"MiniEngine2D",
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			static_cast<int>( windowWidth_ ),
			static_cast<int>( windowHeight_ ),
			nullptr,
			nullptr,
			instance_,
			this );

		if( hwnd_ == nullptr )
		{
			throw std::runtime_error( "Failed to create main window." );
		}

		ShowWindow( hwnd_, showCommand );
		UpdateWindow( hwnd_ );

		InitializeWorld();

		RenderThread::Config renderConfig{};
		renderConfig.window = lightd3d12::MakeWin32WindowHandle( hwnd_ );
		renderConfig.width = windowWidth_;
		renderConfig.height = windowHeight_;
		renderConfig.vsync = true;
		renderConfig.assetRoot = std::filesystem::path( __FILE__ ).parent_path();
		GetRender().Start( renderConfig );
		GetRender().Resize( windowWidth_, windowHeight_, false );

		world_->AddDebugEvent( "Engine boot completed" );
		return true;
	}

	void GameEngine::InitializeWorld()
	{
		MINI_PROFILE_FUNCTION();
		WorldConfig worldConfig{};
		worldConfig.width = static_cast<float>( windowWidth_ );
		worldConfig.height = static_cast<float>( windowHeight_ );
		worldConfig.cellSize = 48.0f;
		world_ = std::make_unique<World>( worldConfig );

		PlayerActor& player = world_->SpawnActor<PlayerActor>( "Player" );
		player.SetPosition( { 480.0f, 280.0f } );

		SpawnerActor& spawner = world_->SpawnActor<SpawnerActor>( "Spawner" );
		spawner.SetPosition( { 72.0f, 72.0f } );

		CursorActor& cursor = world_->SpawnActor<CursorActor>( "Cursor" );
		cursor.SetPosition( { 200.0f, 120.0f } );

		SpriteFollowerActor& follower = world_->SpawnActor<SpriteFollowerActor>( "Follower", cursor.GetId() );
		follower.SetPosition( { 260.0f, 160.0f } );

		EnemyActor& firstEnemy = world_->SpawnActor<EnemyActor>( "Enemy_0" );
		firstEnemy.SetPosition( { 860.0f, 180.0f } );

		EnemyActor& secondEnemy = world_->SpawnActor<EnemyActor>( "Enemy_1" );
		secondEnemy.SetPosition( { 930.0f, 540.0f } );

		world_->AddDebugEvent( "World initialized with player, cursor, follower and spawner" );
	}

	void GameEngine::Shutdown()
	{
		MINI_PROFILE_FUNCTION();
		GetRender().Stop();
		world_.reset();

		if( hwnd_ != nullptr )
		{
			DestroyWindow( hwnd_ );
			hwnd_ = nullptr;
		}

		if( instance_ != nullptr )
		{
			UnregisterClassW( ourWindowClassName, instance_ );
			instance_ = nullptr;
		}
	}

	void GameEngine::UpdateInput()
	{
		MINI_PROFILE_FUNCTION();
		GameInputState input{};
		input.moveLeft = ( GetAsyncKeyState( 'A' ) & 0x8000 ) != 0 || ( GetAsyncKeyState( VK_LEFT ) & 0x8000 ) != 0;
		input.moveRight = ( GetAsyncKeyState( 'D' ) & 0x8000 ) != 0 || ( GetAsyncKeyState( VK_RIGHT ) & 0x8000 ) != 0;
		input.moveUp = ( GetAsyncKeyState( 'W' ) & 0x8000 ) != 0 || ( GetAsyncKeyState( VK_UP ) & 0x8000 ) != 0;
		input.moveDown = ( GetAsyncKeyState( 'S' ) & 0x8000 ) != 0 || ( GetAsyncKeyState( VK_DOWN ) & 0x8000 ) != 0;

		POINT mousePosition{};
		GetCursorPos( &mousePosition );
		ScreenToClient( hwnd_, &mousePosition );
		input.mousePosition = { static_cast<float>( mousePosition.x ), static_cast<float>( mousePosition.y ) };

		world_->SetInputState( input );
	}

	void GameEngine::TickFrame( float deltaTime )
	{
		MINI_PROFILE_FUNCTION();
		MINI_PROFILE_FRAME();
		lastDeltaTime_ = deltaTime;
		UpdateInput();
		world_->SetFrameContext( frameNumber_, accumulatedTime_ );
		lastTickStats_ = tickScheduler_.ExecuteFrame( *world_, taskScheduler_, deltaTime );
		GetRender().SubmitFrame( BuildRenderFrame() );
		UpdateWindowTitle();
		++frameNumber_;
	}

	RenderFrame GameEngine::BuildRenderFrame() const
	{
		MINI_PROFILE_FUNCTION();
		std::vector<SpriteSceneProxy> sceneProxies = world_->BuildSceneProxies();

		RenderFrame frame{};
		frame.frameNumber = frameNumber_;
		frame.deltaTime = lastDeltaTime_;
		frame.framesPerSecond = smoothedFramesPerSecond_;
		frame.actorCount = static_cast<uint32_t>( sceneProxies.size() );
		frame.enemyCount = world_->CountActorsOfKind( ActorKind::Enemy );
		frame.playerHealth = world_->GetPlayerHealth();
		frame.syncTicks = lastTickStats_.syncTicks;
		frame.asyncTicks = lastTickStats_.asyncTicks;
		frame.commandsApplied = lastTickStats_.commandsApplied;
		frame.sprites = std::move( sceneProxies );
		frame.debugLines = world_->GetRecentDebugLines();
		return frame;
	}

	void GameEngine::UpdateWindowTitle() const
	{
		std::wostringstream stream;
		stream << L"MiniEngine2D | FPS " << static_cast<int>( smoothedFramesPerSecond_ )
			   << L" | Enemies " << world_->CountActorsOfKind( ActorKind::Enemy )
			   << L" | Player HP " << world_->GetPlayerHealth();
		SetWindowTextW( hwnd_, stream.str().c_str() );
	}

	LRESULT CALLBACK GameEngine::StaticWindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		GameEngine* engine = nullptr;
		if( message == WM_NCCREATE )
		{
			const CREATESTRUCTW* createStruct = reinterpret_cast<CREATESTRUCTW*>( lParam );
			engine = reinterpret_cast<GameEngine*>( createStruct->lpCreateParams );
			SetWindowLongPtrW( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( engine ) );
		}
		else
		{
			engine = reinterpret_cast<GameEngine*>( GetWindowLongPtrW( hwnd, GWLP_USERDATA ) );
		}

		return engine != nullptr ? engine->WindowProc( hwnd, message, wParam, lParam ) : DefWindowProcW( hwnd, message, wParam, lParam );
	}

	LRESULT GameEngine::WindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		if( GetRender().ProcessMessage( hwnd, message, wParam, lParam ) )
		{
			return 0;
		}

		switch( message )
		{
			case WM_SIZE:
			{
				windowWidth_ = LOWORD( lParam );
				windowHeight_ = HIWORD( lParam );
				minimized_ = windowWidth_ == 0 || windowHeight_ == 0;
				if( world_ )
				{
					world_->SetWorldSize( static_cast<float>( windowWidth_ ), static_cast<float>( windowHeight_ ) );
				}
				GetRender().Resize( windowWidth_, windowHeight_, minimized_ );
				return 0;
			}

			case WM_DESTROY:
				running_ = false;
				if( hwnd_ == hwnd )
				{
					hwnd_ = nullptr;
				}
				PostQuitMessage( 0 );
				return 0;

			case WM_CLOSE:
				running_ = false;
				DestroyWindow( hwnd );
				return 0;

			default:
				return DefWindowProcW( hwnd, message, wParam, lParam );
		}
	}
}
