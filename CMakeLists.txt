cmake_minimum_required(VERSION 3.24)

project(LightD3D12 LANGUAGES CXX)

add_library(LightD3D12 STATIC
    LightD3D12/include/LightD3D12/HandleSlotMap.hpp
    LightD3D12/include/LightD3D12/LightD3D12.hpp
    LightD3D12/src/LightD3D12Internal.hpp
    LightD3D12/src/LightD3D12Resources.hpp
    LightD3D12/src/LightD3D12Resources.cpp
    LightD3D12/src/LightD3D12ImmediateCommands.hpp
    LightD3D12/src/LightD3D12ImmediateCommands.cpp
    LightD3D12/src/LightD3D12CommandBuffer.hpp
    LightD3D12/src/LightD3D12CommandBuffer.cpp
    LightD3D12/src/LightD3D12StagingDevice.hpp
    LightD3D12/src/LightD3D12StagingDevice.cpp
    LightD3D12/src/LightD3D12ManagerImpl.hpp
    LightD3D12/src/LightD3D12DeviceManager.cpp
    LightD3D12/src/LightD3D12RenderDevice.cpp
)

target_include_directories(LightD3D12
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/LightD3D12/include
        ${CMAKE_CURRENT_SOURCE_DIR}/D3D12
)

target_compile_features(LightD3D12 PUBLIC cxx_std_20)

target_compile_definitions(LightD3D12
    PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
)

target_link_libraries(LightD3D12
    PUBLIC
        d3d12
        dxgi
        dxguid
        d3dcompiler
)

if(MSVC)
    target_compile_options(LightD3D12 PRIVATE /W4 /EHsc /permissive- /Zc:preprocessor)
endif()

add_executable(LightD3D12HelloTriangle
    samples/HelloTriangle/main.cpp
)

target_link_libraries(LightD3D12HelloTriangle PRIVATE LightD3D12)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT LightD3D12HelloTriangle)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
    LightD3D12/include/LightD3D12/HandleSlotMap.hpp
    LightD3D12/include/LightD3D12/LightD3D12.hpp
    LightD3D12/src/LightD3D12Internal.hpp
    LightD3D12/src/LightD3D12Resources.hpp
    LightD3D12/src/LightD3D12Resources.cpp
    LightD3D12/src/LightD3D12ImmediateCommands.hpp
    LightD3D12/src/LightD3D12ImmediateCommands.cpp
    LightD3D12/src/LightD3D12CommandBuffer.hpp
    LightD3D12/src/LightD3D12CommandBuffer.cpp
    LightD3D12/src/LightD3D12StagingDevice.hpp
    LightD3D12/src/LightD3D12StagingDevice.cpp
    LightD3D12/src/LightD3D12ManagerImpl.hpp
    LightD3D12/src/LightD3D12DeviceManager.cpp
    LightD3D12/src/LightD3D12RenderDevice.cpp
    samples/HelloTriangle/main.cpp
)
