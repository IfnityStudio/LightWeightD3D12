# LIGHTD3D12

## Generate a Visual Studio solution

From a clean clone on Windows, double-click:

```
GenerateSolution.bat
```

The script asks which optional features you want:

- `Enable Assimp?`
- `Enable FSR?`
- `Enable OpenUSD?`

It then generates `LightD3D12.generated.sln`, writes the matching generated vcpkg manifest under `build\generated\vcpkg`, initializes repository submodules, and installs the selected packages into `vcpkg_installed\x64-windows`.

Open `LightD3D12.generated.sln` after the script finishes. Assimp is installed from vcpkg with the default dynamic `x64-windows` triplet, so samples that enable Assimp link against the vcpkg `.lib` and deploy the runtime `.dll` next to the executable.

## Manual bootstrap

The generator calls the bootstrap script for you, but you can still run it directly:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-vcpkg.ps1
```

Useful direct options:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-vcpkg.ps1 -VcpkgRoot ..\shared-vcpkg
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-vcpkg.ps1 -SkipAmdFsrSdk
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-vcpkg.ps1 -SkipInstall
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-vcpkg.ps1 -ManifestRoot build\generated\vcpkg
```

