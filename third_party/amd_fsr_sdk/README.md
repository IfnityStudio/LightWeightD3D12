# AMD FSR SDK staging area

This folder is the expected drop location for the official AMD FSR SDK package
used by the Visual Studio solution.

Expected layout after extracting the SDK:

- `third_party/amd_fsr_sdk/Kits/FidelityFX/api/include/ffx_api.h`
- `third_party/amd_fsr_sdk/Kits/FidelityFX/upscalers/include/ffx_upscale.h`
- `third_party/amd_fsr_sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.lib`
- `third_party/amd_fsr_sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll`
- `third_party/amd_fsr_sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll`

Integration model used by this repo:

1. The project links against `amd_fidelityfx_loader_dx12.lib`.
2. The runtime copies the signed DX12 DLLs to `$(OutDir)` after build.
3. Projects opt in with `LightD3D12EnableAmdFsrSdk=true`.

The shared wiring lives in [LightD3D12.FSR.props](..\..\LightD3D12.FSR.props).

License note:

- This repo uses the AMD FSR SDK package as third-party software.
- The applicable AMD SDK license text is kept in `third_party/amd_fsr_sdk_repo/docs/license.md`.
- If you redistribute the AMD binaries or package contents from this repo, keep the AMD copyright and license notices together with that redistribution.
