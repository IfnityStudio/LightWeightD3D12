# EnkiStaticOnly

Ejemplo minimo y aislado para usar `enkiTS` como libreria estatica con CMake.

## Que hace

- Usa `FetchContent` para descargar `enkiTS`.
- Compila `enkiTS` como `STATIC`.
- Expone el target `enkiTS::enkiTS`.
- Construye un ejecutable de consola con tres tareas que se lanzan en paralelo.
- Hace `WaitforAll()` antes de continuar, que seria el punto donde en un engine seguirias con el render.

## Archivos

- `CMakeLists.txt`: descarga y compila `enkiTS` como estatica.
- `main.cpp`: ejemplo minimo de uso.

## Como configurarlo

Como proyecto aislado:

```powershell
cmake -S examples/EnkiStaticOnly -B build/EnkiStaticOnly
```

Para compilar:

```powershell
cmake --build build/EnkiStaticOnly --config Debug
```

Para ejecutarlo:

```powershell
.\build\EnkiStaticOnly\Debug\EnkiStaticOnly.exe
```

Como parte del repo principal:

```powershell
cmake -S . -B build\cmake -DLIGHTD3D12_BUILD_ENKI_STATIC_ONLY_EXAMPLE=ON -DLIGHTD3D12_BUILD_SAMPLES=OFF
cmake --build build\cmake --config Debug --target EnkiStaticOnly
```

Con presets desde la raiz del repo:

```powershell
cmake --preset enki-static-only
cmake --build --preset build-enki-static-only-debug
```

## Como linkarlo en otro proyecto

Si quieres copiar este enfoque a otro `CMakeLists.txt`, la parte importante es:

```cmake
include(FetchContent)

FetchContent_Declare(
    enkits
    GIT_REPOSITORY https://github.com/dougbinks/enkiTS.git
    GIT_TAG 03e6a2c0c97208ade44478d617d2002b0f95faf4
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

target_include_directories(enkiTS PUBLIC "${enkits_SOURCE_DIR}/src")

target_link_libraries(TuEjecutable PRIVATE enkiTS::enkiTS)
```

## Nota

La libreria queda estatica porque se crea con:

```cmake
add_library(enkiTS STATIC ...)
```

Eso no cambia automaticamente el runtime de MSVC a estatico. Si mas adelante quieres tambien CRT estatico, entonces habria que ajustar `CMAKE_MSVC_RUNTIME_LIBRARY`.
