cmake_minimum_required(VERSION 3.24)

project(EnkiSample LANGUAGES CXX)

include(FetchContent)

set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(ENKITS_ARCHIVE_URL "https://github.com/dougbinks/enkiTS/archive/03e6a2c0c97208ade44478d617d2002b0f95faf4.zip" CACHE STRING "enkiTS source archive URL.")

FetchContent_Declare(
    enkits
    URL "${ENKITS_ARCHIVE_URL}"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
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

add_executable(EnkiSample
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
)

target_compile_features(EnkiSample PRIVATE cxx_std_17)
target_link_libraries(EnkiSample PRIVATE enkiTS::enkiTS)

if(MSVC)
    target_compile_options(EnkiSample PRIVATE /W4 /EHsc /permissive-)
endif()
