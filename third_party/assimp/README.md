# Assimp Integration

This folder is reserved for `Assimp` as an optional `third_party` dependency.

## Goal

The repository is prepared so `Assimp` can be enabled for importing:

- `.obj`
- `.gltf`
- `.glb`
- `.fbx`

The integration is intentionally not used by any sample yet.

## Expected Layout

Place the library here with a layout similar to:

- `third_party/assimp/include/assimp/scene.h`
- `third_party/assimp/lib/assimp-vc143-mtd.lib`
- `third_party/assimp/lib/assimp-vc143-mt.lib`
- `third_party/assimp/bin/assimp-vc143-mtd.dll`
- `third_party/assimp/bin/assimp-vc143-mt.dll`

If your package uses different filenames, override them through:

- `LightD3D12AssimpDebugLibrary`
- `LightD3D12AssimpReleaseLibrary`
- `LightD3D12AssimpDebugRuntimeDll`
- `LightD3D12AssimpReleaseRuntimeDll`

## Build Integration

The optional property sheet is:

- `LightD3D12.Assimp.props`

Enable it per project with:

```xml
<LightD3D12EnableAssimp>true</LightD3D12EnableAssimp>
```

## Wrapper

The engine exposes an optional wrapper:

- `LightAssimpImporter`

Public header:

- `LightD3D12/include/LightD3D12/LightAssimpImporter.hpp`

When `Assimp` is disabled, the wrapper stays compiled but importing throws with a clear error message.
