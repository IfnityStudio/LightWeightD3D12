# test_lightd3d12

Proyecto de tests con asserts simples para LightD3D12, sin framework externo.

Por defecto ejecuta solo tests CPU/dummy. Estos no necesitan ventana ni GPU:

```powershell
.\build\Debug\test_lightd3d12.exe
```

Los tests que crean un dispositivo D3D12 real son opt-in:

```powershell
.\build\Debug\test_lightd3d12.exe --hardware
```

Tambien puedes listar o filtrar por categoria:

```powershell
.\build\Debug\test_lightd3d12.exe --list
.\build\Debug\test_lightd3d12.exe --category=Core
```

Categorias iniciales:

- `Core.*`: handles, slot map, descriptores, flags, ventanas y carga HLSL.
- `Import.*`: disponibilidad y consultas de importacion Assimp.
- `Hardware.*`: device manager, recursos, validacion, pipelines y command buffers reales.
