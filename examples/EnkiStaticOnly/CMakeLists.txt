cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(EnkiStaticOnly LANGUAGES CXX)
endif()

include(FetchContent)

set(ENKITS_GIT_REPOSITORY "https://github.com/dougbinks/enkiTS.git" CACHE STRING "enkiTS Git repository.")
set(ENKITS_GIT_TAG "03e6a2c0c97208ade44478d617d2002b0f95faf4" CACHE STRING "enkiTS Git tag or commit.")

FetchContent_Declare(
    enkits
    GIT_REPOSITORY "${ENKITS_GIT_REPOSITORY}"
    GIT_TAG "${ENKITS_GIT_TAG}"
    GIT_SHALLOW TRUE
)

FetchContent_GetProperties(enkits)
if(NOT enkits_POPULATED)
    FetchContent_Populate(enkits)
endif()

add_library(enkiTS STATIC
    "${enkits_SOURCE_DIR}/src/TaskScheduler.cpp"
)

add_library(enkiTS::enkiTS ALIAS enkiTS)

target_compile_features(enkiTS PUBLIC cxx_std_17)
target_include_directories(enkiTS PUBLIC "${enkits_SOURCE_DIR}/src")

if(MSVC)
    target_compile_options(enkiTS PRIVATE /W4 /EHsc /permissive-)
endif()

add_executable(EnkiStaticOnly
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
)

target_compile_features(EnkiStaticOnly PRIVATE cxx_std_17)
target_link_libraries(EnkiStaticOnly PRIVATE enkiTS::enkiTS)

if(MSVC)
    target_compile_options(EnkiStaticOnly PRIVATE /W4 /EHsc /permissive-)
endif()
