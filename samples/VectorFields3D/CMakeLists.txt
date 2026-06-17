add_executable(LightD3D12VectorFields3D WIN32
    main.cpp
)

target_link_libraries(LightD3D12VectorFields3D PRIVATE LightD3D12::LightD3D12)

if(MSVC)
    target_compile_options(LightD3D12VectorFields3D PRIVATE /W4 /EHsc /permissive- /MP)
endif()
