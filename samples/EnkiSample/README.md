# EnkiSample

Sample de Visual Studio que descarga `enkiTS` oficial desde GitHub cuando no existe todavia, lo compila como libreria estatica y lo linka contra un ejecutable minimo.

## Archivos

- `EnkiSample.vcxproj`: proyecto visible en la solucion.
- `CMakeLists.txt`: descarga y compila `enkiTS`.
- `main.cpp`: ejemplo minimo de tareas.

## Como funciona el `.vcxproj`

El proyecto es de tipo `Makefile`. Cuando pulsas compilar en Visual Studio:

1. Ejecuta CMake sobre esta carpeta.
2. Si `enkiTS` no existe, CMake descarga el `.zip` oficial desde GitHub.
3. CMake genera el build local del sample.
4. Se compila `EnkiSample.exe`.

## Donde cae la descarga

La descarga queda dentro del propio sample:

- `samples/EnkiSample/external/`

## Salida

El binario queda en:

- `samples/EnkiSample/build-vs/Debug/EnkiSample.exe`
- `samples/EnkiSample/build-vs/Release/EnkiSample.exe`
