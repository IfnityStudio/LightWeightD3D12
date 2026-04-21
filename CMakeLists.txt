cmake_minimum_required(VERSION 3.24)

project(LightD3D12 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "LightD3D12 currently supports only Win32 builds.")
endif()

option(LIGHTD3D12_BUILD_SAMPLES "Build LightD3D12 sample applications." ON)

set(ourLightD3D12Root "${CMAKE_CURRENT_SOURCE_DIR}")
set(ourLibraryRoot "${ourLightD3D12Root}/LightD3D12")
set(ourImguiRoot "${ourLightD3D12Root}/third_party/imgui")

set(ourPublicHeaders
    "${ourLibraryRoot}/include/LightD3D12/HandleSlotMap.hpp"
    "${ourLibraryRoot}/include/LightD3D12/LightD3D12.hpp"
    "${ourLibraryRoot}/include/LightD3D12/LightD3D12Imgui.hpp"
)

set(ourPrivateHeaders
    "${ourLibraryRoot}/src/LightD3D12Internal.hpp"
    "${ourLibraryRoot}/src/LightD3D12Resources.hpp"
    "${ourLibraryRoot}/src/LightD3D12ImmediateCommands.hpp"
    "${ourLibraryRoot}/src/LightD3D12CommandBuffer.hpp"
    "${ourLibraryRoot}/src/LightD3D12StagingDevice.hpp"
    "${ourLibraryRoot}/src/LightD3D12ManagerImpl.hpp"
    "${ourLibraryRoot}/src/LightD3D12Swapchain.hpp"
)

set(ourLibrarySources
    "${ourLibraryRoot}/src/LightD3D12Resources.cpp"
    "${ourLibraryRoot}/src/LightD3D12ImmediateCommands.cpp"
    "${ourLibraryRoot}/src/LightD3D12CommandBuffer.cpp"
    "${ourLibraryRoot}/src/LightD3D12StagingDevice.cpp"
    "${ourLibraryRoot}/src/LightD3D12Swapchain.cpp"
    "${ourLibraryRoot}/src/LightD3D12DeviceManager.cpp"
    "${ourLibraryRoot}/src/LightD3D12RenderDevice.cpp"
    "${ourLibraryRoot}/src/LightD3D12Imgui.cpp"
)

set(ourImguiSources
    "${ourImguiRoot}/imgui.cpp"
    "${ourImguiRoot}/imgui_draw.cpp"
    "${ourImguiRoot}/imgui_tables.cpp"
    "${ourImguiRoot}/imgui_widgets.cpp"
    "${ourImguiRoot}/backends/imgui_impl_dx12.cpp"
    "${ourImguiRoot}/backends/imgui_impl_win32.cpp"
)

set(ourImguiHeaders
    "${ourImguiRoot}/imconfig.h"
    "${ourImguiRoot}/imgui.h"
    "${ourImguiRoot}/imgui_internal.h"
    "${ourImguiRoot}/imstb_rectpack.h"
    "${ourImguiRoot}/imstb_textedit.h"
    "${ourImguiRoot}/imstb_truetype.h"
    "${ourImguiRoot}/backends/imgui_impl_dx12.h"
    "${ourImguiRoot}/backends/imgui_impl_win32.h"
)

add_library(LightD3D12 STATIC
    ${ourPublicHeaders}
    ${ourPrivateHeaders}
    ${ourLibrarySources}
    ${ourImguiHeaders}
    ${ourImguiSources}
)

add_library(LightD3D12::LightD3D12 ALIAS LightD3D12)

target_compile_features(LightD3D12 PUBLIC cxx_std_20)

target_compile_definitions(LightD3D12
    PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
)

target_include_directories(LightD3D12
    PUBLIC
        "${ourLibraryRoot}/include"
        "${ourImguiRoot}"
    PRIVATE
        "${ourLightD3D12Root}/D3D12"
        "${ourImguiRoot}/backends"
)

target_link_libraries(LightD3D12
    PUBLIC
        d3d12
        dxgi
        dxguid
        d3dcompiler
)

if(MSVC)
    target_compile_options(LightD3D12 PRIVATE /W4 /EHsc /permissive- /MP)
endif()

function(ourSetOutputDirectories targetName)
    foreach(ourConfig IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${ourConfig}" ourConfigUpper)
        set_target_properties(${targetName} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${ourConfigUpper} "${ourLightD3D12Root}/build/${ourConfig}"
            LIBRARY_OUTPUT_DIRECTORY_${ourConfigUpper} "${ourLightD3D12Root}/build/${ourConfig}"
            ARCHIVE_OUTPUT_DIRECTORY_${ourConfigUpper} "${ourLightD3D12Root}/build/${ourConfig}"
        )
    endforeach()
endfunction()

function(ourAddSample targetName sourceFile)
    add_executable(${targetName} WIN32 "${sourceFile}" ${ARGN})
    target_link_libraries(${targetName} PRIVATE LightD3D12)

    if(MSVC)
        target_compile_options(${targetName} PRIVATE /W4 /EHsc /permissive- /MP)
    endif()

    ourSetOutputDirectories(${targetName})
endfunction()

ourSetOutputDirectories(LightD3D12)

if(LIGHTD3D12_BUILD_SAMPLES)
    ourAddSample(
        LightD3D12HelloTriangle
        "${ourLightD3D12Root}/samples/HelloTriangle/main.cpp"
    )

    ourAddSample(
        LightD3D12HelloDeferred
        "${ourLightD3D12Root}/samples/HelloDeferred/main.cpp"
    )

    ourAddSample(
        LightD3D12HelloSprites
        "${ourLightD3D12Root}/samples/HelloSprites/main.cpp"
        "${ourLightD3D12Root}/samples/HelloSprites/enemy.png"
    )

    ourAddSample(
        LightD3D12HelloCube
        "${ourLightD3D12Root}/samples/HelloCube/main.cpp"
    )

    target_link_libraries(LightD3D12HelloSprites PRIVATE windowscodecs ole32)

    set_property(DIRECTORY "${ourLightD3D12Root}" PROPERTY VS_STARTUP_PROJECT LightD3D12HelloTriangle)
endif()

source_group(TREE "${ourLightD3D12Root}" FILES
    ${ourPublicHeaders}
    ${ourPrivateHeaders}
    ${ourLibrarySources}
    ${ourImguiHeaders}
    ${ourImguiSources}
)

if(LIGHTD3D12_BUILD_SAMPLES)
    source_group(TREE "${ourLightD3D12Root}" FILES
        "${ourLightD3D12Root}/samples/HelloTriangle/main.cpp"
        "${ourLightD3D12Root}/samples/HelloDeferred/main.cpp"
        "${ourLightD3D12Root}/samples/HelloSprites/main.cpp"
        "${ourLightD3D12Root}/samples/HelloSprites/enemy.png"
        "${ourLightD3D12Root}/samples/HelloCube/main.cpp"
    )
endif()
