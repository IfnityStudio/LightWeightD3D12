#include "LightD3D12/LightD3D12.hpp"
#include "LightD3D12/LightD3D12Imgui.hpp"
#include "LightD3D12/LightHLSLLoader.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include <shellapi.h>

using namespace lightd3d12;

namespace
{
	struct Float3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	[[nodiscard]] Float3 operator+( const Float3& left, const Float3& right ) noexcept
	{
		return { left.x + right.x, left.y + right.y, left.z + right.z };
	}

	[[nodiscard]] Float3 operator-( const Float3& left, const Float3& right ) noexcept
	{
		return { left.x - right.x, left.y - right.y, left.z - right.z };
	}

	[[nodiscard]] Float3 operator*( const Float3& value, float scalar ) noexcept
	{
		return { value.x * scalar, value.y * scalar, value.z * scalar };
	}

	[[nodiscard]] float Dot( const Float3& left, const Float3& right ) noexcept
	{
		return left.x * right.x + left.y * right.y + left.z * right.z;
	}

	[[nodiscard]] Float3 Cross( const Float3& left, const Float3& right ) noexcept
	{
		return {
			left.y * right.z - left.z * right.y,
			left.z * right.x - left.x * right.z,
			left.x * right.y - left.y * right.x
		};
	}

	[[nodiscard]] Float3 Normalize( const Float3& value ) noexcept
	{
		const float lengthSquared = Dot( value, value );
		if( lengthSquared <= 0.000001f )
		{
			return { 0.0f, 0.0f, 1.0f };
		}

		const float inverseLength = 1.0f / std::sqrt( lengthSquared );
		return value * inverseLength;
	}

	struct RayMarchPushConstants
	{
		float cameraOriginX = 0.0f;
		float cameraOriginY = 0.0f;
		float cameraOriginZ = 0.0f;
		float time = 0.0f;

		float cameraForwardX = 0.0f;
		float cameraForwardY = 0.0f;
		float cameraForwardZ = 1.0f;
		float aspectRatio = 1.0f;

		float cameraRightX = 1.0f;
		float cameraRightY = 0.0f;
		float cameraRightZ = 0.0f;
		float tanHalfFov = 0.57735f;

		float cameraUpX = 0.0f;
		float cameraUpY = 1.0f;
		float cameraUpZ = 0.0f;
		float maxDistance = 26.0f;

		uint32_t lessonIndex = 0;
		uint32_t activeStep = 1;
		uint32_t debugView = 0;
		uint32_t flags = 0;

		float lightDirectionX = -0.45f;
		float lightDirectionY = -0.82f;
		float lightDirectionZ = -0.34f;
		float smoothUnionK = 0.45f;

		float repeatSpacing = 2.6f;
		float fogDensity = 0.035f;
		float groundY = 0.0f;
		float invViewportHeight = 0.0f;
		float inspectUvX = 0.5f;
		float inspectUvY = 0.5f;
		uint32_t inspectFlags = 0;
		uint32_t inspectPadding = 0;
	};

	static_assert( sizeof( RayMarchPushConstants ) / sizeof( uint32_t ) <= 63 );

	struct StepInfo
	{
		const char* title = "";
		const char* explanation = "";
	};

	struct LessonInfo
	{
		const char* title = "";
		const char* summary = "";
		const char* pseudocode = "";
		std::array<StepInfo, 4> steps = {};
		int stepCount = 0;
	};

	enum DebugView : int
	{
		DebugView_Auto = 0,
		DebugView_Uv = 1,
		DebugView_RayDirection = 2,
		DebugView_Sky = 3,
		DebugView_HitMask = 4,
		DebugView_MarchSteps = 5,
		DebugView_Depth = 6,
		DebugView_Normals = 7,
		DebugView_Diffuse = 8,
		DebugView_Shadow = 9,
		DebugView_AmbientOcclusion = 10,
		DebugView_Final = 11,
	};

	constexpr std::array<const char*, 12> ourDebugViewNames = {
		"Auto",
		"UV",
		"Direccion del rayo",
		"Cielo / horizonte",
		"Hit / miss",
		"Pasos del march",
		"Profundidad",
		"Normales",
		"Lambert",
		"Sombras suaves",
		"Ambient occlusion",
		"Final"
	};

	constexpr std::array<LessonInfo, 5> ourLessons = {
		LessonInfo{
			"1. Construir el rayo",
			"Antes de hablar de distancias, necesitamos saber que rayo estamos lanzando por cada pixel. Esta leccion solo construye la direccion de camara y enseña como un fullscreen triangle puede generar una imagen completa.",
			"float2 ndc = uv * 2 - 1;\nfloat3 rd = normalize(forward + right * ndc.x * aspect * tanHalfFov + up * ndc.y * tanHalfFov);",
			{
				StepInfo{ "UV de pantalla", "Primero mira el lienzo como coordenadas 0..1. Si entiendes UV, luego entiendes como convertirlas a NDC." },
				StepInfo{ "Direccion del rayo", "Ahora cada pixel se convierte en una direccion 3D saliendo de la camara." },
				StepInfo{ "Horizonte", "Usamos solo la direccion del rayo para pintar cielo y suelo. Aun no hay escena; solo orientacion." },
				StepInfo{}
			},
			3
		},
		LessonInfo{
			"2. Primer hit con sphere tracing",
			"Ray marching usa una funcion de distancia firmada: en cada paso preguntamos cuanto falta hasta la superficie y avanzamos exactamente esa distancia.",
			"t = 0;\nfor(i = 0; i < MAX_STEPS; ++i)\n{\n    p = ro + rd * t;\n    d = map(p);\n    if(d < EPSILON) hit;\n    t += d;\n}",
			{
				StepInfo{ "Silueta", "Solo queremos saber si el rayo toca o no una esfera. Blanco = hit, oscuro = miss." },
				StepInfo{ "Conteo de pasos", "Visualiza cuantas iteraciones costó llegar. Donde hay curvatura o tangencias suele costar mas." },
				StepInfo{ "Profundidad", "Una vez golpeas la superficie, el valor t ya te da profundidad util para sombrear o mezclar niebla." },
				StepInfo{}
			},
			3
		},
		LessonInfo{
			"3. Normales y sombreado",
			"Cuando ya tienes el punto de impacto, puedes muestrear la SDF alrededor para estimar la normal y empezar a iluminar la superficie.",
			"float3 n = normalize(float3(\n    map(p + ex) - map(p - ex),\n    map(p + ey) - map(p - ey),\n    map(p + ez) - map(p - ez)));\nfloat lambert = saturate(dot(n, lightDir));",
			{
				StepInfo{ "Normales", "La normal sale de diferencias finitas sobre la SDF. Es la puerta de entrada a toda la iluminacion." },
				StepInfo{ "Lambert", "Con una luz direccional ya aparece volumen: dot(normal, lightDir)." },
				StepInfo{ "Sombreado final basico", "Añadimos ambiente, especular suave y un plano suelo para leer mejor la escala." },
				StepInfo{}
			},
			3
		},
		LessonInfo{
			"4. Construir una escena",
			"Una sola esfera enseña el algoritmo, pero la gracia del ray marching llega cuando combinas primitivas y transformas el espacio.",
			"hero = union(sdSphere(...), sdBox(...));\nhero = smoothUnion(heroA, heroB, k);\nrepeated = repeatXZ(p, spacing);\ntwisted = twistY(p, amount);",
			{
				StepInfo{ "Union de primitivas", "La escena minima mezcla esfera y caja quedandote siempre con la distancia menor." },
				StepInfo{ "Smooth union", "En vez de una costura dura, mezclamos ambos campos para fundir las formas." },
				StepInfo{ "Repeticion", "No duplicas mallas: repites el espacio y reutilizas la misma SDF una y otra vez." },
				StepInfo{ "Twist / deformacion", "Transformar el dominio antes de evaluar la SDF te permite deformar la escena entera." }
			},
			4
		},
		LessonInfo{
			"5. Bloque avanzado",
			"Ya tenemos escena y sombreado. Este ultimo bloque añade trucos clasicos que vuelven el resultado mas creible: sombras suaves, AO, niebla y un toque emisivo.",
			"shadow = softShadow(p + n * bias, lightDir);\nao = ambientOcclusion(p, n);\ncolor = lerp(color, sky, fog);\ncolor += emissive;",
			{
				StepInfo{ "Sombras suaves", "Otro march secundario hacia la luz aproxima cuanto volumen bloquea la iluminacion." },
				StepInfo{ "Ambient occlusion", "Varios samples pequeños sobre la normal te dicen cuan encerrado está el punto." },
				StepInfo{ "Niebla atmosferica", "La profundidad t se reaprovecha para fundir la escena con el cielo." },
				StepInfo{ "Toque final", "Cerramos con una forma emisiva y todas las capas activas a la vez." }
			},
			4
		}
	};

	struct LessonExplainer
	{
		const char* whatGoesIn = "";
		const char* whatHappens = "";
		const char* whatComesOut = "";
		const char* shaderMap = "";
	};

	constexpr std::array<LessonExplainer, 5> ourLessonExplainers = {
		LessonExplainer{
			"Entrada: la posicion 2D del pixel en pantalla, escrita como UV entre 0 y 1.",
			"Proceso: primero UV pasa a NDC. Luego ese punto se abre con FOV y aspect ratio. Al final se mezcla con los ejes forward, right y up de la camara.",
			"Salida: una direccion 3D. Ese es el rayo que sale de la camara para ese pixel.",
			"Shader: BuildRayContext() -> ResolveViewMode() -> RenderLessonOne() -> SkyColor()"
		},
		LessonExplainer{
			"Entrada: origen del rayo, direccion del rayo y una esfera SDF.",
			"Proceso: medimos distancia a la esfera, avanzamos justo esa distancia y repetimos hasta tocar o salirnos.",
			"Salida: hit o miss, numero de pasos y distancia recorrida.",
			"Shader: EvaluateLessonTwoScene() -> EvaluateScene() -> RayMarch() -> RenderMiss()/RenderHitDebug()"
		},
		LessonExplainer{
			"Entrada: punto de impacto, escena SDF y direccion de luz.",
			"Proceso: se estiman normales con diferencias finitas y luego se calcula iluminacion sencilla.",
			"Salida: la esfera deja de ser una mascara y empieza a parecer una superficie real.",
			"Shader: EvaluateLessonThreeScene() -> EstimateNormal() -> RenderHitDebug() -> ShadeSurface()"
		},
		LessonExplainer{
			"Entrada: punto p, primitivas SDF y parametros de mezcla y deformacion.",
			"Proceso: unimos figuras, repetimos el espacio y lo torcemos antes de medir distancia.",
			"Salida: una escena completa sin mallas, solo con matematicas.",
			"Shader: EvaluateLessonFourScene() -> SurfaceUnion() -> SurfaceSmoothUnion() -> Repeat2D() -> TwistY()"
		},
		LessonExplainer{
			"Entrada: punto de impacto, normal, profundidad y luz.",
			"Proceso: se hace un march hacia la luz, varias muestras de AO, mezcla de niebla y una pieza emisiva.",
			"Salida: mas sensacion de contacto, atmosfera y energia.",
			"Shader: EvaluateLessonFiveScene() -> SoftShadow() -> AmbientOcclusion() -> ShadeSurface()"
		}
	};

	struct AppState
	{
		std::unique_ptr<DeviceManager> deviceManager;
		std::unique_ptr<ImguiRenderer> imguiRenderer;
		RenderPipelineState rayMarchPipeline;
		bool running = true;
		bool minimized = false;
		bool pauseAnimation = false;
		bool animateScene = true;
		bool autoOrbitCamera = true;
		int lessonIndex = 0;
		int stepIndex = 0;
		int debugViewIndex = DebugView_Auto;
		float cameraYaw = 0.32f;
		float cameraPitch = -0.24f;
		float cameraDistance = 6.0f;
		float verticalFovDegrees = 48.0f;
		float smoothUnionK = 0.45f;
		float repeatSpacing = 2.6f;
		float fogDensity = 0.035f;
		float maxDistance = 26.0f;
		float rayExplainUvX = 0.5f;
		float rayExplainUvY = 0.5f;
		float simulationTime = 0.0f;
		float smoothedFrameMs = 16.6f;
		float smoothedFps = 60.0f;
	};

	struct RayFormationInfo
	{
		float uvX = 0.5f;
		float uvY = 0.5f;
		float ndcX = 0.0f;
		float ndcY = 0.0f;
		float offsetX = 0.0f;
		float offsetY = 0.0f;
		float aspectRatio = 1.0f;
		float tanHalfFov = 0.57735f;
		Float3 forward{};
		Float3 right{};
		Float3 up{};
		Float3 preNormalize{};
		Float3 direction{};
	};

	[[nodiscard]] const char* GetLessonLabel( void*, int index )
	{
		if( index < 0 || index >= static_cast<int>( ourLessons.size() ) )
		{
			return nullptr;
		}

		return ourLessons[ static_cast<size_t>( index ) ].title;
	}

	[[nodiscard]] const char* GetDebugViewLabel( void*, int index )
	{
		if( index < 0 || index >= static_cast<int>( ourDebugViewNames.size() ) )
		{
			return nullptr;
		}

		return ourDebugViewNames[ static_cast<size_t>( index ) ];
	}

	bool CommandLineHasFlag( const wchar_t* commandLine, const wchar_t* flag )
	{
		if( commandLine == nullptr || flag == nullptr )
		{
			return false;
		}

		int argumentCount = 0;
		LPWSTR* arguments = CommandLineToArgvW( commandLine, &argumentCount );
		if( arguments == nullptr )
		{
			return false;
		}

		bool found = false;
		for( int index = 0; index < argumentCount; ++index )
		{
			if( _wcsicmp( arguments[ index ], flag ) == 0 )
			{
				found = true;
				break;
			}
		}

		LocalFree( arguments );
		return found;
	}

	void ClampLessonState( AppState& app ) noexcept
	{
		app.lessonIndex = std::clamp( app.lessonIndex, 0, static_cast<int>( ourLessons.size() ) - 1 );
		const LessonInfo& lesson = ourLessons[ static_cast<size_t>( app.lessonIndex ) ];
		app.stepIndex = std::clamp( app.stepIndex, 0, lesson.stepCount - 1 );
		app.debugViewIndex = std::clamp( app.debugViewIndex, 0, static_cast<int>( ourDebugViewNames.size() ) - 1 );
	}

	void ResetCameraForLesson( AppState& app ) noexcept
	{
		switch( app.lessonIndex )
		{
			case 0:
				app.cameraYaw = 0.20f;
				app.cameraPitch = -0.10f;
				app.cameraDistance = 5.5f;
				break;

			case 1:
				app.cameraYaw = 0.34f;
				app.cameraPitch = -0.18f;
				app.cameraDistance = 5.8f;
				break;

			case 2:
				app.cameraYaw = 0.40f;
				app.cameraPitch = -0.24f;
				app.cameraDistance = 6.0f;
				break;

			case 3:
				app.cameraYaw = 0.48f;
				app.cameraPitch = -0.28f;
				app.cameraDistance = 6.6f;
				break;

			default:
				app.cameraYaw = 0.56f;
				app.cameraPitch = -0.30f;
				app.cameraDistance = 7.2f;
				break;
		}
	}

	void SelectLesson( AppState& app, int lessonIndex )
	{
		app.lessonIndex = std::clamp( lessonIndex, 0, static_cast<int>( ourLessons.size() ) - 1 );
		app.stepIndex = 0;
		app.debugViewIndex = DebugView_Auto;
		ResetCameraForLesson( app );
	}

	void MoveStep( AppState& app, int delta )
	{
		const LessonInfo& lesson = ourLessons[ static_cast<size_t>( app.lessonIndex ) ];
		app.stepIndex = std::clamp( app.stepIndex + delta, 0, lesson.stepCount - 1 );
	}

	[[nodiscard]] RenderPipelineState CreateRayMarchPipeline( RenderDevice& ctx, DXGI_FORMAT colorFormat )
	{
		RenderPipelineDesc desc{};
		desc.vertexShader = LightHLSLLoader::LoadStage( "shaders/RayMarchingGuideVS.hlsl", "vs_6_6" );
		desc.fragmentShader = LightHLSLLoader::LoadStage( "shaders/RayMarchingGuidePS.hlsl", "ps_6_6" );
		desc.color[ 0 ].format = colorFormat;
		desc.rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.depthStencilState.DepthEnable = FALSE;
		desc.depthStencilState.StencilEnable = FALSE;
		return ctx.CreateRenderPipeline( desc );
	}

	void ComputeCameraBasis( const AppState& app, Float3& origin, Float3& forward, Float3& right, Float3& up )
	{
		const float orbitYaw = app.cameraYaw + ( app.autoOrbitCamera ? app.simulationTime * 0.22f : 0.0f );
		const float pitch = app.cameraPitch;

		forward = Normalize( {
			std::cos( pitch ) * std::sin( orbitYaw ),
			std::sin( pitch ),
			std::cos( pitch ) * std::cos( orbitYaw )
		} );

		const Float3 focus = { 0.0f, 0.95f, 0.0f };
		origin = focus - forward * app.cameraDistance;
		right = Normalize( Cross( { 0.0f, 1.0f, 0.0f }, forward ) );
		up = Normalize( Cross( forward, right ) );
	}

	[[nodiscard]] RayMarchPushConstants BuildPushConstants( const AppState& app, float aspectRatio, float invViewportHeight )
	{
		Float3 origin{};
		Float3 forward{};
		Float3 right{};
		Float3 up{};
		ComputeCameraBasis( app, origin, forward, right, up );

		const Float3 lightDirection = Normalize( { -0.45f, -0.82f, -0.34f } );

		RayMarchPushConstants pushConstants{};
		pushConstants.cameraOriginX = origin.x;
		pushConstants.cameraOriginY = origin.y;
		pushConstants.cameraOriginZ = origin.z;
		pushConstants.time = app.simulationTime;

		pushConstants.cameraForwardX = forward.x;
		pushConstants.cameraForwardY = forward.y;
		pushConstants.cameraForwardZ = forward.z;
		pushConstants.aspectRatio = aspectRatio;

		pushConstants.cameraRightX = right.x;
		pushConstants.cameraRightY = right.y;
		pushConstants.cameraRightZ = right.z;
		pushConstants.tanHalfFov = std::tan( app.verticalFovDegrees * 0.5f * 3.1415926535f / 180.0f );

		pushConstants.cameraUpX = up.x;
		pushConstants.cameraUpY = up.y;
		pushConstants.cameraUpZ = up.z;
		pushConstants.maxDistance = app.maxDistance;

		pushConstants.lessonIndex = static_cast<uint32_t>( app.lessonIndex );
		pushConstants.activeStep = static_cast<uint32_t>( app.stepIndex + 1 );
		pushConstants.debugView = static_cast<uint32_t>( app.debugViewIndex );
		pushConstants.flags = app.animateScene ? 1u : 0u;

		pushConstants.lightDirectionX = lightDirection.x;
		pushConstants.lightDirectionY = lightDirection.y;
		pushConstants.lightDirectionZ = lightDirection.z;
		pushConstants.smoothUnionK = app.smoothUnionK;

		pushConstants.repeatSpacing = app.repeatSpacing;
		pushConstants.fogDensity = app.fogDensity;
		pushConstants.groundY = 0.0f;
		pushConstants.invViewportHeight = invViewportHeight;
		pushConstants.inspectUvX = app.rayExplainUvX;
		pushConstants.inspectUvY = app.rayExplainUvY;
		pushConstants.inspectFlags = 1u;
		return pushConstants;
	}

	[[nodiscard]] RayFormationInfo BuildRayFormationInfo( const AppState& app, float uvX, float uvY )
	{
		RayFormationInfo info{};
		info.uvX = uvX;
		info.uvY = uvY;

		Float3 origin{};
		ComputeCameraBasis( app, origin, info.forward, info.right, info.up );

		const float width = ( app.deviceManager != nullptr ) ? std::max( 1.0f, static_cast<float>( app.deviceManager->GetWidth() ) ) : 1.0f;
		const float height = ( app.deviceManager != nullptr ) ? std::max( 1.0f, static_cast<float>( app.deviceManager->GetHeight() ) ) : 1.0f;
		info.aspectRatio = width / height;
		info.tanHalfFov = std::tan( app.verticalFovDegrees * 0.5f * 3.1415926535f / 180.0f );

		info.ndcX = uvX * 2.0f - 1.0f;
		info.ndcY = 1.0f - uvY * 2.0f;
		info.offsetX = info.ndcX * info.aspectRatio * info.tanHalfFov;
		info.offsetY = info.ndcY * info.tanHalfFov;

		info.preNormalize =
			info.forward +
			info.right * info.offsetX +
			info.up * info.offsetY;
		info.direction = Normalize( info.preNormalize );
		return info;
	}

	void DrawLessonOneInspector( AppState& app )
	{
		ImGui::Separator();
		ImGui::TextUnformatted( "Como nace un rayo" );
		ImGui::TextWrapped( "Cada pixel de pantalla hace exactamente estas cuentas. Si entiendes este bloque, entiendes la base del ray marching." );
		ImGui::TextWrapped( "Regla clave: el pixel del centro UV=(0.5, 0.5) siempre da NDC=(0, 0), asi que su rayo sale en la direccion forward de la camara." );

		ImGui::SliderFloat( "UV X", &app.rayExplainUvX, 0.0f, 1.0f, "%.2f" );
		ImGui::SliderFloat( "UV Y", &app.rayExplainUvY, 0.0f, 1.0f, "%.2f" );

		const RayFormationInfo info = BuildRayFormationInfo( app, app.rayExplainUvX, app.rayExplainUvY );

		ImGui::BeginChild( "LessonOneInspector", ImVec2( 0.0f, 210.0f ), true );
		ImGui::TextWrapped( "1. UV elegida: (%.2f, %.2f)", info.uvX, info.uvY );
		ImGui::TextWrapped( "2. NDC = (uv.x * 2 - 1, 1 - uv.y * 2) = (%.2f, %.2f)", info.ndcX, info.ndcY );
		ImGui::TextWrapped( "3. Offset en el plano de vision = (ndc.x * aspect * tanHalfFov, ndc.y * tanHalfFov) = (%.2f, %.2f)", info.offsetX, info.offsetY );
		ImGui::TextWrapped( "4. Vector antes de normalizar = forward + right * %.2f + up * %.2f", info.offsetX, info.offsetY );
		ImGui::TextWrapped( "   forward = (%.2f, %.2f, %.2f)", info.forward.x, info.forward.y, info.forward.z );
		ImGui::TextWrapped( "   right   = (%.2f, %.2f, %.2f)", info.right.x, info.right.y, info.right.z );
		ImGui::TextWrapped( "   up      = (%.2f, %.2f, %.2f)", info.up.x, info.up.y, info.up.z );
		ImGui::TextWrapped( "   suma    = (%.2f, %.2f, %.2f)", info.preNormalize.x, info.preNormalize.y, info.preNormalize.z );
		ImGui::TextWrapped( "5. Rayo final = normalize(suma) = (%.2f, %.2f, %.2f)", info.direction.x, info.direction.y, info.direction.z );
		ImGui::EndChild();

		ImGui::TextWrapped( "Mueve UV X e UV Y y mira la imagen: el marcador amarillo enseña que pixel estas inspeccionando y la previsualizacion en pantalla enseña el resultado de ese pixel." );
		ImGui::TextWrapped( "Lectura rapida: si subes UV Y hacia 0.0 el rayo se inclina hacia arriba; si bajas UV Y hacia 1.0 se inclina hacia abajo. Si mueves UV X hacia 1.0 el rayo se abre hacia la derecha de la camara." );
	}

	void DrawStepTimeline( const AppState& app )
	{
		const LessonInfo& lesson = ourLessons[ static_cast<size_t>( app.lessonIndex ) ];
		for( int index = 0; index < lesson.stepCount; ++index )
		{
			const bool activated = index <= app.stepIndex;
			const bool current = index == app.stepIndex;
			const ImVec4 color = current ? ImVec4( 1.0f, 0.88f, 0.48f, 1.0f ) : ( activated ? ImVec4( 0.45f, 0.95f, 0.62f, 1.0f ) : ImVec4( 0.55f, 0.58f, 0.62f, 1.0f ) );
			ImGui::PushStyleColor( ImGuiCol_Text, color );
			ImGui::BulletText( "Paso %d. %s", index + 1, lesson.steps[ static_cast<size_t>( index ) ].title );
			ImGui::PopStyleColor();
			if( current )
			{
				ImGui::TextWrapped( "%s", lesson.steps[ static_cast<size_t>( index ) ].explanation );
			}
		}
	}

	void DrawGuideUi( AppState& app )
	{
		ClampLessonState( app );
		const LessonInfo& lesson = ourLessons[ static_cast<size_t>( app.lessonIndex ) ];
		const LessonExplainer& explainer = ourLessonExplainers[ static_cast<size_t>( app.lessonIndex ) ];

		ImGui::SetNextWindowPos( ImVec2( 18.0f, 18.0f ), ImGuiCond_FirstUseEver );
		ImGui::SetNextWindowSize( ImVec2( 470.0f, 760.0f ), ImGuiCond_FirstUseEver );
		ImGui::Begin( "Ray Marching Guide" );

		ImGui::TextWrapped( "Ejemplo docente montado sobre LightD3D12: fullscreen triangle + pixel shader ray marched + overlay ImGui para avanzar paso a paso." );
		ImGui::Separator();

		int lessonIndex = app.lessonIndex;
		if( ImGui::Combo( "Leccion", &lessonIndex, &GetLessonLabel, nullptr, static_cast<int>( ourLessons.size() ) ) )
		{
			SelectLesson( app, lessonIndex );
		}

		if( ImGui::Button( "Leccion anterior" ) )
		{
			SelectLesson( app, std::max( 0, app.lessonIndex - 1 ) );
		}
		ImGui::SameLine();
		if( ImGui::Button( "Leccion siguiente" ) )
		{
			SelectLesson( app, std::min( static_cast<int>( ourLessons.size() ) - 1, app.lessonIndex + 1 ) );
		}

		ImGui::Separator();
		ImGui::TextWrapped( "%s", lesson.summary );

		ImGui::Separator();
		ImGui::TextUnformatted( "Que entra y que sale" );
		ImGui::TextWrapped( "%s", explainer.whatGoesIn );
		ImGui::TextWrapped( "%s", explainer.whatHappens );
		ImGui::TextWrapped( "%s", explainer.whatComesOut );
		ImGui::TextWrapped( "%s", explainer.shaderMap );

		std::array<char, 48> progressLabel{};
		std::snprintf( progressLabel.data(), progressLabel.size(), "Paso %d / %d", app.stepIndex + 1, lesson.stepCount );
		ImGui::ProgressBar(
			static_cast<float>( app.stepIndex + 1 ) / static_cast<float>( lesson.stepCount ),
			ImVec2( -FLT_MIN, 0.0f ),
			progressLabel.data() );

		int visibleStep = app.stepIndex + 1;
		if( ImGui::SliderInt( "Activacion", &visibleStep, 1, lesson.stepCount ) )
		{
			app.stepIndex = visibleStep - 1;
		}

		if( ImGui::Button( "Paso anterior" ) )
		{
			MoveStep( app, -1 );
		}
		ImGui::SameLine();
		if( ImGui::Button( "Paso siguiente" ) )
		{
			MoveStep( app, 1 );
		}

		ImGui::Separator();
		DrawStepTimeline( app );

		if( app.lessonIndex == 0 )
		{
			DrawLessonOneInspector( app );
		}

		ImGui::Separator();
		ImGui::TextUnformatted( "Pseudocodigo mental" );
		ImGui::BeginChild( "RayMarchPseudo", ImVec2( 0.0f, 120.0f ), true );
		ImGui::TextUnformatted( lesson.pseudocode );
		ImGui::EndChild();

		ImGui::Separator();
		ImGui::TextUnformatted( "Exploracion" );
		ImGui::Combo( "Vista", &app.debugViewIndex, &GetDebugViewLabel, nullptr, static_cast<int>( ourDebugViewNames.size() ) );
		ImGui::Checkbox( "Animar escena", &app.animateScene );
		ImGui::SameLine();
		ImGui::Checkbox( "Pausar tiempo", &app.pauseAnimation );
		ImGui::Checkbox( "Orbita automatica", &app.autoOrbitCamera );

		if( !app.autoOrbitCamera )
		{
			float pitchUi = -app.cameraPitch;
			ImGui::SliderFloat( "Yaw", &app.cameraYaw, -3.14159f, 3.14159f, "%.2f rad" );
			if( ImGui::SliderFloat( "Pitch", &pitchUi, -1.15f, 1.15f, "%.2f rad" ) )
			{
				app.cameraPitch = -pitchUi;
			}
		}
		else
		{
			ImGui::TextWrapped( "La camara gira despacio sola para que puedas leer volumen y sombras sin tocar nada." );
		}

		ImGui::SliderFloat( "Distancia camara", &app.cameraDistance, 3.0f, 12.0f, "%.2f" );
		ImGui::SliderFloat( "FOV vertical", &app.verticalFovDegrees, 30.0f, 70.0f, "%.1f deg" );
		ImGui::SliderFloat( "Smooth union k", &app.smoothUnionK, 0.08f, 1.2f, "%.2f" );
		ImGui::SliderFloat( "Repeat spacing", &app.repeatSpacing, 1.25f, 4.5f, "%.2f" );
		ImGui::SliderFloat( "Fog density", &app.fogDensity, 0.0f, 0.10f, "%.3f" );
		ImGui::SliderFloat( "Max distance", &app.maxDistance, 8.0f, 40.0f, "%.1f" );

		if( ImGui::Button( "Reset camera de la leccion" ) )
		{
			ResetCameraForLesson( app );
		}

		ImGui::Separator();
		ImGui::Text( "FPS: %.1f", app.smoothedFps );
		ImGui::Text( "Frame time: %.2f ms", app.smoothedFrameMs );
		ImGui::Text( "Vista actual: %s", ourDebugViewNames[ static_cast<size_t>( app.debugViewIndex ) ] );
		ImGui::TextWrapped( "Tip: deja la vista en Auto mientras avanzas la leccion. Cambia a otras vistas cuando quieras inspeccionar una capa concreta del shader." );
		ImGui::End();
	}

	void RecordScenePass( ICommandBuffer& commandBuffer, TextureHandle currentTexture, AppState& app )
	{
		LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "RayMarchingGuide::ScenePass", 0xff4cc9f0u );

		const float viewportWidth = std::max( 1.0f, static_cast<float>( app.deviceManager->GetWidth() ) );
		const float viewportHeight = std::max( 1.0f, static_cast<float>( app.deviceManager->GetHeight() ) );
		const float aspectRatio = viewportWidth / viewportHeight;
		const RayMarchPushConstants pushConstants = BuildPushConstants( app, aspectRatio, 1.0f / viewportHeight );

		RenderPass renderPass{};
		renderPass.color[ 0 ].loadOp = LoadOp::Clear;
		renderPass.color[ 0 ].clearColor = { 0.015f, 0.018f, 0.026f, 1.0f };

		Framebuffer framebuffer{};
		framebuffer.color[ 0 ].texture = currentTexture;

		commandBuffer.CmdBeginRendering( renderPass, framebuffer );
		commandBuffer.CmdBindRenderPipeline( app.rayMarchPipeline );
		commandBuffer.CmdPushConstants( &pushConstants, sizeof( pushConstants ) );
		commandBuffer.CmdDraw( 3 );
		commandBuffer.CmdEndRendering();
	}

	void RecordImguiPass( ICommandBuffer& commandBuffer, TextureHandle currentTexture, ImguiRenderer& imguiRenderer )
	{
		LIGHTD3D12_CMD_SCOPE_NAMED( commandBuffer, "RayMarchingGuide::ImguiPass", 0xfff9c74fu );

		RenderPass renderPass{};
		renderPass.color[ 0 ].loadOp = LoadOp::Load;

		Framebuffer framebuffer{};
		framebuffer.color[ 0 ].texture = currentTexture;

		commandBuffer.CmdBeginRendering( renderPass, framebuffer );
		imguiRenderer.Render( commandBuffer );
		commandBuffer.CmdEndRendering();
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
			{
				if( app != nullptr )
				{
					app->running = false;
					app->minimized = true;
				}
				return 0;
			}

			default:
				return DefWindowProc( hwnd, message, wParam, lParam );
		}
	}
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE, PWSTR, int showCommand )
{
	constexpr const wchar_t* kWindowClassName = L"LightD3D12RayMarchingGuideWindow";
	HWND hwnd = nullptr;
	AppState app{};

	try
	{
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof( WNDCLASSEX );
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = kWindowClassName;
		windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
		RegisterClassExW( &windowClass );

		constexpr uint32_t kInitialWidth = 1600;
		constexpr uint32_t kInitialHeight = 960;

		hwnd = CreateWindowExW(
			0,
			windowClass.lpszClassName,
			L"LightD3D12 Ray Marching Guide",
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

		ResetCameraForLesson( app );
		SetWindowLongPtr( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( &app ) );

		ContextDesc contextDesc{};
		contextDesc.enableDebugLayer = true;
		contextDesc.enablePixGpuCapture = CommandLineHasFlag( GetCommandLineW(), L"--pix" );
		contextDesc.swapchainBufferCount = 3;

		SwapchainDesc swapchainDesc{};
		swapchainDesc.window = MakeWin32WindowHandle( hwnd );
		swapchainDesc.width = kInitialWidth;
		swapchainDesc.height = kInitialHeight;
		swapchainDesc.vsync = true;

		app.deviceManager = std::make_unique<DeviceManager>( contextDesc, swapchainDesc );
		app.imguiRenderer = std::make_unique<ImguiRenderer>( *app.deviceManager, swapchainDesc.window );

		LightHLSLLoader::SetRootDirectory( std::filesystem::path( __FILE__ ).parent_path() );

		RenderDevice& ctx = *app.deviceManager->GetRenderDevice();
		app.rayMarchPipeline = CreateRayMarchPipeline( ctx, contextDesc.swapchainFormat );

		auto lastFrameTime = std::chrono::steady_clock::now();
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

			RenderDevice* renderDevice = app.deviceManager ? app.deviceManager->GetRenderDevice() : nullptr;
			if( !app.running || app.minimized || renderDevice == nullptr || app.imguiRenderer == nullptr )
			{
				continue;
			}

			const auto now = std::chrono::steady_clock::now();
			float deltaSeconds = std::chrono::duration<float>( now - lastFrameTime ).count();
			lastFrameTime = now;
			deltaSeconds = std::clamp( deltaSeconds, 0.0f, 0.05f );

			if( !app.pauseAnimation )
			{
				app.simulationTime += deltaSeconds;
			}

			const float frameMs = deltaSeconds * 1000.0f;
			app.smoothedFrameMs += ( frameMs - app.smoothedFrameMs ) * 0.08f;
			app.smoothedFps = app.smoothedFrameMs > 0.001f ? 1000.0f / app.smoothedFrameMs : 0.0f;

			app.imguiRenderer->NewFrame();
			DrawGuideUi( app );

			ICommandBuffer& commandBuffer = renderDevice->AcquireCommandBuffer();
			const TextureHandle currentTexture = renderDevice->GetCurrentSwapchainTexture();

			RecordScenePass( commandBuffer, currentTexture, app );
			RecordImguiPass( commandBuffer, currentTexture, *app.imguiRenderer );
			renderDevice->Submit( commandBuffer, currentTexture );
		}

		SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		if( app.deviceManager )
		{
			app.deviceManager->WaitIdle();
		}
		app.rayMarchPipeline = {};
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
		if( hwnd != nullptr && IsWindow( hwnd ) != FALSE )
		{
			SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 );
		}
		MessageBoxA( nullptr, exception.what(), "LightD3D12 Ray Marching Guide failed.", MB_ICONERROR | MB_OK );
		return 1;
	}
}
