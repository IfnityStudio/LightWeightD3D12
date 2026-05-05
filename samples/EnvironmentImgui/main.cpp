#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lightd3d12;

namespace
{
	// ============================================================
	// 1. ENUM LIST
	// ============================================================

	enum EnvFloatValue
	{
		Env_BloomStrength,
		Env_BloomDirtAlpha,
		Env_BloomThreshold,
		Env_BloomTentFilterScaleX,
		Env_BloomTentFilterScaleY,

		NUM_ENV_FLOATS
	};

	enum EnvBoolValue
	{
		GFX_ENV_BOOL_BEFORE_FIRST = NUM_ENV_FLOATS - 1,

		Env_BloomEnabled,
		Env_BloomOverrideConsoleVars,

		NUM_ENV_BOOLS
	};

	enum EnvValueType
	{
		Env_Float = 0,
		Env_Bool
	};

	const char* GetFloatName( EnvFloatValue value )
	{
		switch( value )
		{
			case Env_BloomStrength:
				return "BloomStrength";
			case Env_BloomDirtAlpha:
				return "BloomDirtAlpha";
			case Env_BloomThreshold:
				return "BloomThreshold";
			case Env_BloomTentFilterScaleX:
				return "BloomTentFilterScaleX";
			case Env_BloomTentFilterScaleY:
				return "BloomTentFilterScaleY";
			default:
				return "UnknownFloat";
		}
	}

	const char* GetBoolName( EnvBoolValue value )
	{
		switch( value )
		{
			case Env_BloomEnabled:
				return "BloomEnabled";
			case Env_BloomOverrideConsoleVars:
				return "BloomOverrideConsoleVars";
			default:
				return "UnknownBool";
		}
	}

	// ============================================================
	// 2. FINAL RUNTIME VALUES CONTAINER
	// ============================================================

	class EnvironmentValues
	{
	public:
		EnvironmentValues()
		{
			myFloats.fill( 0.0f );
			myBools.fill( false );
		}

		float GetValue( EnvFloatValue index ) const
		{
			return myFloats[ index ];
		}

		bool GetValue( EnvBoolValue index ) const
		{
			return myBools[ index - GFX_ENV_BOOL_BEFORE_FIRST - 1 ];
		}

		void SetFloat( uint16_t index, float value )
		{
			myFloats[ index ] = value;
		}

		void SetBool( uint16_t index, bool value )
		{
			myBools[ index - GFX_ENV_BOOL_BEFORE_FIRST - 1 ] = value;
		}

		void LerpFloat( uint16_t index, float value, float lerp )
		{
			myFloats[ index ] = myFloats[ index ] * ( 1.0f - lerp ) + value * lerp;
		}

	private:
		std::array<float, NUM_ENV_FLOATS> myFloats;
		std::array<bool, NUM_ENV_BOOLS - GFX_ENV_BOOL_BEFORE_FIRST - 1> myBools;
	};

	// ============================================================
	// 3. SERIALIZED VALUE
	// ============================================================

	class EnvironmentValue
	{
	public:
		EnvironmentValue( EnvValueType type, uint16_t valueIndex ):
			myType( type ),
			myValueIndex( valueIndex )
		{}

		virtual ~EnvironmentValue() = default;

		virtual void UpdateValue( EnvironmentValues& values, float time, float lerp ) const = 0;

		EnvValueType GetType() const
		{
			return myType;
		}

		uint16_t GetValueIndex() const
		{
			return myValueIndex;
		}

	protected:
		EnvValueType myType;
		uint16_t myValueIndex;
	};

	// ============================================================
	// 4. FLOAT VALUE
	// ============================================================

	class EnvironmentFloatValue final : public EnvironmentValue
	{
	public:
		EnvironmentFloatValue( uint16_t valueIndex, float defaultValue ):
			EnvironmentValue( Env_Float, valueIndex ),
			myValue( defaultValue )
		{}

		void UpdateValue( EnvironmentValues& values, float, float lerp ) const override
		{
			values.LerpFloat( myValueIndex, myValue, lerp );
		}

		float myValue;
	};

	// ============================================================
	// 5. BOOL VALUE
	// ============================================================

	class EnvironmentBoolValue final : public EnvironmentValue
	{
	public:
		EnvironmentBoolValue( uint16_t valueIndex, bool defaultValue ):
			EnvironmentValue( Env_Bool, valueIndex ),
			myValue( defaultValue )
		{}

		void UpdateValue( EnvironmentValues& values, float, float lerp ) const override
		{
			if( lerp > 0.5f )
			{
				values.SetBool( myValueIndex, myValue );
			}
		}

		bool myValue;
	};

	// ============================================================
	// 6. VALUES DATA
	// ============================================================

	class EnvironmentValuesData
	{
	public:
		~EnvironmentValuesData()
		{
			for( EnvironmentValue* value : myValues )
			{
				delete value;
			}
		}

		EnvironmentValuesData() = default;
		EnvironmentValuesData( const EnvironmentValuesData& ) = delete;
		EnvironmentValuesData& operator=( const EnvironmentValuesData& ) = delete;

		void UpdateValues( EnvironmentValues& values, float time, float lerp ) const
		{
			for( const auto* value : myValues )
			{
				value->UpdateValue( values, time, lerp );
			}
		}

		std::vector<EnvironmentValue*> myValues;
	};

	// ============================================================
	// 7. PRESET
	// ============================================================

	class EnvironmentPreset
	{
	public:
		EnvironmentPreset( const std::string& name, float lerpFactor = 1.0f ):
			myName( name ),
			myLerpFactor( lerpFactor )
		{}

		void GetValues( EnvironmentValues& outValues, float time ) const
		{
			myData.UpdateValues( outValues, time, myLerpFactor );
		}

		EnvironmentValuesData myData;
		std::string myName;
		float myLerpFactor;
	};

	// ============================================================
	// 8. ENVIRONMENT
	// ============================================================

	class Environment
	{
	public:
		void AddPreset( EnvironmentPreset* preset )
		{
			myPresets.push_back( preset );
		}

		void GenerateValues( EnvironmentValues& outValues, float time ) const
		{
			for( const auto* preset : myPresets )
			{
				preset->GetValues( outValues, time );
			}
		}

	private:
		std::vector<EnvironmentPreset*> myPresets;
	};

	// ============================================================
	// 9. RENDERER SETTINGS STRUCT
	// ============================================================

	struct BloomSettings
	{
		bool myEnable = false;
		float myStrength = 0.0f;
		float myDirtAlpha = 0.0f;
		float myThreshold = 0.0f;
	};

	// ============================================================
	// 10. FROM ENVIRONMENT
	// ============================================================

	void FromEnvironment( BloomSettings* outSettings, const EnvironmentValues& env )
	{
		outSettings->myEnable = env.GetValue( Env_BloomEnabled );
		outSettings->myStrength = env.GetValue( Env_BloomStrength );
		outSettings->myDirtAlpha = env.GetValue( Env_BloomDirtAlpha );
		outSettings->myThreshold = env.GetValue( Env_BloomThreshold );
	}

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		EnvironmentPreset defaultPreset{ "Hardcoded Defaults" };
		EnvironmentPreset artistPreset{ "Artist Time of Day", 1.0f };
		EnvironmentPreset volumePreset{ "Indoor Volume", 0.6f };
		Environment environment;
		float timeOfDay = 12.0f;
		bool running = true;
		bool minimized = false;
	};

	void InitializeEnvironmentExample( AppState& app )
	{
		app.defaultPreset.myData.myValues =
		{
			new EnvironmentBoolValue( Env_BloomEnabled, true ),
			new EnvironmentFloatValue( Env_BloomStrength, 0.5f ),
			new EnvironmentFloatValue( Env_BloomDirtAlpha, 0.3f ),
			new EnvironmentFloatValue( Env_BloomThreshold, 1.0f ),
			new EnvironmentFloatValue( Env_BloomTentFilterScaleX, 1.75f ),
			new EnvironmentFloatValue( Env_BloomTentFilterScaleY, 1.25f ),
			new EnvironmentBoolValue( Env_BloomOverrideConsoleVars, false )
		};

		app.artistPreset.myData.myValues =
		{
			new EnvironmentFloatValue( Env_BloomStrength, 0.8f ),
			new EnvironmentFloatValue( Env_BloomThreshold, 0.5f )
		};

		app.volumePreset.myData.myValues =
		{
			new EnvironmentFloatValue( Env_BloomStrength, 0.2f ),
			new EnvironmentFloatValue( Env_BloomDirtAlpha, 0.0f )
		};

		app.environment.AddPreset( &app.defaultPreset );
		app.environment.AddPreset( &app.artistPreset );
		app.environment.AddPreset( &app.volumePreset );
	}

	void DrawEnvironmentValueEditor( EnvironmentValue* value )
	{
		if( value->GetType() == Env_Float )
		{
			auto* floatValue = static_cast<EnvironmentFloatValue*>( value );
			const auto index = static_cast<EnvFloatValue>( floatValue->GetValueIndex() );
			ImGui::SliderFloat( GetFloatName( index ), &floatValue->myValue, 0.0f, 5.0f, "%.3f" );
			return;
		}

		auto* boolValue = static_cast<EnvironmentBoolValue*>( value );
		const auto index = static_cast<EnvBoolValue>( boolValue->GetValueIndex() );
		ImGui::Checkbox( GetBoolName( index ), &boolValue->myValue );
	}

	void DrawPresetEditor( EnvironmentPreset& preset )
	{
		if( ImGui::TreeNodeEx( preset.myName.c_str(), ImGuiTreeNodeFlags_DefaultOpen ) )
		{
			ImGui::SliderFloat( "Lerp factor", &preset.myLerpFactor, 0.0f, 1.0f, "%.3f" );
			for( EnvironmentValue* value : preset.myData.myValues )
			{
				DrawEnvironmentValueEditor( value );
			}
			ImGui::TreePop();
		}
	}

	void DrawEnvironmentUi( AppState& app )
	{
		EnvironmentValues envValues;
		app.environment.GenerateValues( envValues, app.timeOfDay );

		BloomSettings bloomSettings;
		FromEnvironment( &bloomSettings, envValues );

		const bool useEnvOverride = envValues.GetValue( Env_BloomOverrideConsoleVars );
		const float tentFilterX = useEnvOverride ? envValues.GetValue( Env_BloomTentFilterScaleX ) : 1.75f;
		const float tentFilterY = useEnvOverride ? envValues.GetValue( Env_BloomTentFilterScaleY ) : 1.25f;

		ImGui::SetNextWindowPos( ImVec2( 20.0f, 20.0f ), ImGuiCond_Once );
		ImGui::SetNextWindowSize( ImVec2( 620.0f, 760.0f ), ImGuiCond_Once );
		ImGui::Begin( "Environment Values" );

		ImGui::TextWrapped( "This is the original environment/preset example, but editable in ImGui. Presets are applied in order: defaults, artist, volume." );
		ImGui::SliderFloat( "Time of day", &app.timeOfDay, 0.0f, 24.0f, "%.2f" );
		ImGui::Separator();

		DrawPresetEditor( app.defaultPreset );
		DrawPresetEditor( app.artistPreset );
		DrawPresetEditor( app.volumePreset );

		ImGui::Separator();
		ImGui::Text( "Final Blended Values" );
		ImGui::BulletText( "BloomEnabled: %s", bloomSettings.myEnable ? "true" : "false" );
		ImGui::BulletText( "BloomStrength: %.3f", bloomSettings.myStrength );
		ImGui::BulletText( "BloomDirtAlpha: %.3f", bloomSettings.myDirtAlpha );
		ImGui::BulletText( "BloomThreshold: %.3f", bloomSettings.myThreshold );
		ImGui::BulletText( "TentFilterScaleX: %.3f override=%s", tentFilterX, useEnvOverride ? "true" : "false" );
		ImGui::BulletText( "TentFilterScaleY: %.3f", tentFilterY );

		ImGui::Separator();
		ImGui::TextWrapped( "Bool values are not lerped. They apply when the preset lerp factor is greater than 0.5, matching the original example." );
		ImGui::End();
	}

	LRESULT CALLBACK WindowProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		auto* app = reinterpret_cast<AppState*>( GetWindowLongPtr( hwnd, GWLP_USERDATA ) );

		if( app != nullptr && app->imguiRenderer && app->imguiRenderer->ProcessMessage( hwnd, message, wParam, lParam ) )
		{
			return 1;
		}

		switch( message )
		{
			case WM_SIZE:
			{
				if( app != nullptr && app->deviceManager )
				{
					const uint32_t width = LOWORD( lParam );
					const uint32_t height = HIWORD( lParam );
					app->minimized = width == 0 || height == 0;
					if( !app->minimized )
					{
						app->deviceManager->Resize( width, height );
					}
				}
				return 0;
			}

			case WM_DESTROY:
				PostQuitMessage( 0 );
				return 0;

			case WM_CLOSE:
				if( app != nullptr )
				{
					app->running = false;
					app->minimized = true;
				}
				return 0;

			default:
				return DefWindowProc( hwnd, message, wParam, lParam );
		}
	}
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE, PWSTR, int showCommand )
{
	try
	{
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = L"LightD3D12EnvironmentImguiWindow";
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1280;
		constexpr uint32_t kInitialHeight = 900;

		HWND hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Environment ImGui",
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			static_cast<int>( kInitialWidth ),
			static_cast<int>( kInitialHeight ),
			nullptr,
			nullptr,
			instance,
			nullptr );

		if( hwnd == nullptr )
		{
			throw std::runtime_error( "Failed to create Win32 window." );
		}

		ShowWindow( hwnd, showCommand );
		UpdateWindow( hwnd );

		AppState app{};
		InitializeEnvironmentExample( app );
		SetWindowLongPtr( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( &app ) );

		ContextDesc contextDesc{};
		contextDesc.enableDebugLayer = true;
		contextDesc.swapchainBufferCount = 3;

		SwapchainDesc swapchainDesc{};
		swapchainDesc.window = MakeWin32WindowHandle( hwnd );
		swapchainDesc.width = kInitialWidth;
		swapchainDesc.height = kInitialHeight;
		swapchainDesc.vsync = true;

		app.deviceManager = std::make_unique<DeviceManager>( contextDesc, swapchainDesc );
		app.imguiRenderer = std::make_unique<ImguiRenderer>( *app.deviceManager, swapchainDesc.window );

		ImGui::GetIO().FontGlobalScale = 1.4f;
		ImGui::GetStyle().ScaleAllSizes( 1.4f );

		MSG message{};
		while( app.running )
		{
			while( PeekMessage( &message, nullptr, 0, 0, PM_REMOVE ) )
			{
				if( message.message == WM_QUIT )
				{
					app.running = false;
					break;
				}

				TranslateMessage( &message );
				DispatchMessage( &message );
			}

			RenderDevice* ctx = app.deviceManager ? app.deviceManager->GetRenderDevice() : nullptr;
			if( !app.running || app.minimized || ctx == nullptr )
			{
				continue;
			}

			app.imguiRenderer->NewFrame();
			DrawEnvironmentUi( app );

			ICommandBuffer& buffer = ctx->AcquireCommandBuffer();
			const TextureHandle currentTexture = ctx->GetCurrentSwapchainTexture();

			RenderPass renderPass{};
			renderPass.color[ 0 ].loadOp = LoadOp::Clear;
			renderPass.color[ 0 ].clearColor = { 0.045f, 0.05f, 0.065f, 1.0f };

			Framebuffer framebuffer{};
			framebuffer.color[ 0 ].texture = currentTexture;

			buffer.CmdBeginRendering( renderPass, framebuffer );
			app.imguiRenderer->Render( buffer );
			buffer.CmdEndRendering();
			ctx->Submit( buffer, currentTexture );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		if( app.deviceManager )
		{
			app.deviceManager->WaitIdle();
		}
		app.imguiRenderer.reset();
		app.deviceManager.reset();
		if( IsWindow( hwnd ) != FALSE )
		{
			DestroyWindow( hwnd );
		}
		UnregisterClassW( windowClass.lpszClassName, instance );
		return 0;
	}
	catch( const std::exception& exception )
	{
		MessageBoxA( nullptr, exception.what(), "LightD3D12 Environment ImGui failed.", MB_ICONERROR | MB_OK );
		return 1;
	}
}
