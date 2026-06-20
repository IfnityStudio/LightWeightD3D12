# LIGHTD3D12

## Bootstrap dependencies

From a clean clone on Windows, run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-vcpkg.ps1
```

That script prepares local dependencies for the Visual Studio solution:

- Clones and bootstraps `vcpkg` into `third_party\vcpkg` when no `VcpkgRoot` is provided.
- Installs the manifest packages from `vcpkg.json` into `vcpkg_installed\x64-windows`, currently `directxtk12`, `usd`, and `winpixevent`.
- Downloads the latest official AMD FidelityFX SDK release package and stages the signed DX12 `.lib/.dll` files under `third_party\amd_fsr_sdk`.

Useful options:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-vcpkg.ps1 -VcpkgRoot D:\vcpkg
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-vcpkg.ps1 -SkipAmdFsrSdk
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-vcpkg.ps1 -SkipInstall
```

After that, open `LightD3D12.sln` normally. `Directory.Build.props` points MSBuild at the local vcpkg manifest install, `SdkMeshPowerplant` uses `DirectXTK12`, `UsdStaticScene` uses the `usd` package from vcpkg, and `Upscaler` uses the AMD FidelityFX files staged by the script.

