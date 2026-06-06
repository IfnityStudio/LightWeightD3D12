# Ray Marching Guide

Este sample está pensado como una guía interactiva para entender ray marching dentro de `LightD3D12`.

## Qué enseña

La demo avanza desde lo más básico hasta una escena más rica:

1. Construir un rayo por pixel.
2. Hacer `sphere tracing` sobre una SDF.
3. Sacar normales de la SDF y sombrear.
4. Combinar primitivas y transformar el espacio.
5. Añadir capas avanzadas: sombras suaves, AO, niebla y emisivo.

Todo se controla desde `ImGui`, activando pasos de forma progresiva.

## Cómo está montado en el engine

El sample usa una estructura muy simple:

- Un `fullscreen triangle` generado por `SV_VertexID`.
- Un `pixel shader` que hace el ray marching.
- Un segundo pass con `ImGui` sobre el backbuffer ya renderizado.

No hay mallas CPU ni buffers de vértices para la escena principal. El “modelo” vive en la función `EvaluateScene()` del shader:

- [main.cpp](./main.cpp)
- [RayMarchingGuideVS.hlsl](./shaders/RayMarchingGuideVS.hlsl)
- [RayMarchingGuidePS.hlsl](./shaders/RayMarchingGuidePS.hlsl)

## Mapa mental rápido

Ray marching no “interseca triángulos”.

En vez de eso:

1. Lanzas un rayo desde cámara.
2. Evalúas una función de distancia firmada `map(p)`.
3. Avanzas exactamente esa distancia.
4. Repites hasta tocar superficie o salir del rango máximo.

Pseudocódigo:

```hlsl
t = 0;
for(i = 0; i < MAX_STEPS; ++i)
{
    p = ro + rd * t;
    d = map(p);
    if(d < EPSILON) { hit; break; }
    t += d;
}
```

## Lecciones

### 1. Construir el rayo

- Paso 1: UV de pantalla.
- Paso 2: dirección 3D del rayo.
- Paso 3: cielo/suelo usando solo `rd`.

Objetivo:
entender que todo empieza en convertir un pixel 2D en una dirección 3D.

### 2. Primer hit

- Paso 1: hit / miss contra una esfera.
- Paso 2: calor por número de pasos.
- Paso 3: profundidad `t`.

Objetivo:
ver que la SDF no da color, da distancia segura para avanzar.

Nota:
la vista de profundidad enseÃ±a distancia lineal a cÃ¡mara normalizada por `gMaxDistance`; si el rayo no impacta, el fondo se muestra como distancia mÃ¡xima.

### 3. Normales y sombreado

- Paso 1: normales por diferencias finitas.
- Paso 2: Lambert.
- Paso 3: ambiente + especular + plano suelo.

Objetivo:
entender cómo una SDF pasa de “matemática” a “superficie iluminada”.

### 4. Construir escena

- Paso 1: unión de esfera y caja.
- Paso 2: `smooth union`.
- Paso 3: repetición espacial.
- Paso 4: `twist`.

Objetivo:
mostrar que muchas veces no transformas objetos, transformas el dominio antes de evaluar la SDF.

### 5. Avanzado

- Paso 1: soft shadows.
- Paso 2: ambient occlusion.
- Paso 3: fog.
- Paso 4: remate emisivo.

Objetivo:
cerrar el pipeline mental completo de una escena ray marched.

## Qué tocar si quieres seguir

- Cambia `EvaluateScene()` para meter otras primitivas.
- Añade más modos de depuración en `ResolveViewMode()`.
- Sube o baja `MAX_STEPS`, el `epsilon` adaptativo y `gMaxDistance`.
- Prueba otras deformaciones sobre el dominio antes de evaluar la SDF.

## Idea clave

La pieza más importante para interiorizar ray marching es esta:

> La escena no es una lista de mallas; la escena es una función.

Ese cambio de mentalidad es justo lo que este sample intenta enseñar paso a paso.
