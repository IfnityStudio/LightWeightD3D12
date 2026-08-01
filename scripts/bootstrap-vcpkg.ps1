param(
    [string]$VcpkgRoot = "",
    [string]$Triplet = "x64-windows",
    [string]$ManifestRoot = "",
    [switch]$SkipInstall,
    [switch]$SkipSubmodules,
    [switch]$SkipAmdFsrSdk
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path

function Test-SubPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $separators = [char[]]@([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    $fullPath = [System.IO.Path]::GetFullPath($Path).TrimEnd($separators)
    $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd($separators)
    if ($fullPath.Equals($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }

    return $fullPath.StartsWith($fullRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
}

function Copy-DirectorySafe {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (-not (Test-Path $Source)) {
        return $false
    }

    if (-not (Test-SubPath -Path $Destination -Root $repoRoot)) {
        throw "Refusing to write outside repository: $Destination"
    }

    if (Test-Path $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
    return $true
}

function Install-AmdFsrSdk {
    $amdRoot = Join-Path $repoRoot "third_party\amd_fsr_sdk"
    $amdFidelityFxRoot = Join-Path $amdRoot "Kits\FidelityFX"
    $amdSignedBin = Join-Path $amdFidelityFxRoot "signedbin"
    $apiIncludeDir = Join-Path $amdFidelityFxRoot "api\include"
    $upscalersIncludeDir = Join-Path $amdFidelityFxRoot "upscalers\include"
    $loaderLib = Join-Path $amdSignedBin "amd_fidelityfx_loader_dx12.lib"
    $loaderDll = Join-Path $amdSignedBin "amd_fidelityfx_loader_dx12.dll"
    $upscalerDll = Join-Path $amdSignedBin "amd_fidelityfx_upscaler_dx12.dll"

    if (
        (Test-Path $loaderLib) -and
        (Test-Path $loaderDll) -and
        (Test-Path $upscalerDll) -and
        (Test-Path (Join-Path $apiIncludeDir "ffx_api.h")) -and
        (Test-Path (Join-Path $upscalersIncludeDir "ffx_upscale.h"))
    ) {
        Write-Host "AMD FSR SDK files already exist: $amdFidelityFxRoot"
        return
    }

    $cacheDir = Join-Path $repoRoot "third_party\.cache\amd_fsr_sdk"
    $extractDir = Join-Path $cacheDir "extract"

    New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
    if (Test-Path $extractDir) {
        Remove-Item -LiteralPath $extractDir -Recurse -Force
    }

    Write-Host "Querying latest AMD FidelityFX SDK release..."
    $headers = @{ "User-Agent" = "LightD3D12-bootstrap" }
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/latest" -Headers $headers
    $asset = $release.assets |
        Where-Object { $_.name -match "^FidelityFX-Samples-.*-prebuilt\.zip$" } |
        Select-Object -First 1

    if (-not $asset) {
        $asset = $release.assets |
            Where-Object { $_.name -match "^FidelityFX-Samples-.*-source\.zip$" } |
            Select-Object -First 1
    }

    if (-not $asset) {
        throw "Could not find a FidelityFX SDK zip in the latest GitHub release."
    }

    $zipPath = Join-Path $cacheDir $asset.name
    if (-not (Test-Path $zipPath)) {
        Write-Host "Downloading AMD FidelityFX SDK: $($asset.name)"
        Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath -Headers $headers
    }
    else {
        Write-Host "Using cached AMD FidelityFX SDK zip: $zipPath"
    }

    Write-Host "Extracting AMD FidelityFX SDK..."
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractDir -Force

    # SDK 2.3 prebuilt packages contain the runtime DLLs inside each sample,
    # but no longer contain the loader import library or public headers.
    # Prefer the FSR sample runtime directory and support older signedbin layouts.
    $runtimeLoaderFiles = @(
        Get-ChildItem -Path $extractDir -Filter "amd_fidelityfx_loader_dx12.dll" -Recurse
    )
    $runtimeLoaderFile = $runtimeLoaderFiles |
        Where-Object { $_.FullName -match "[\\/]Samples[\\/]Upscalers[\\/]FidelityFX_FSR[\\/]" } |
        Select-Object -First 1
    if (-not $runtimeLoaderFile) {
        $runtimeLoaderFile = $runtimeLoaderFiles | Select-Object -First 1
    }
    if (-not $runtimeLoaderFile) {
        throw "The downloaded AMD FidelityFX SDK package did not contain amd_fidelityfx_loader_dx12.dll."
    }

    $runtimeSourceDir = Split-Path -Parent $runtimeLoaderFile.FullName
    New-Item -ItemType Directory -Force -Path $amdSignedBin | Out-Null
    foreach ($runtimeName in @(
        "amd_fidelityfx_loader_dx12.dll",
        "amd_fidelityfx_upscaler_dx12.dll",
        "amd_fidelityfx_framegeneration_dx12.dll"
    )) {
        $runtimeSource = Join-Path $runtimeSourceDir $runtimeName
        if (Test-Path $runtimeSource) {
            Copy-Item -LiteralPath $runtimeSource -Destination (Join-Path $amdSignedBin $runtimeName) -Force
        }
    }

    $loaderLibFile = Get-ChildItem -Path $extractDir -Filter "amd_fidelityfx_loader_dx12.lib" -Recurse |
        Select-Object -First 1
    if ($loaderLibFile) {
        Copy-Item -LiteralPath $loaderLibFile.FullName -Destination $loaderLib -Force
    }
    else {
        $loaderLibUrl =
            "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$($release.tag_name)/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.lib"
        Write-Host "Downloading AMD FidelityFX loader import library for $($release.tag_name)..."
        Invoke-WebRequest -Uri $loaderLibUrl -OutFile $loaderLib -Headers $headers
    }

    $sourceApiInclude = Get-ChildItem -Path $extractDir -Filter "ffx_api.h" -Recurse |
        Select-Object -First 1 |
        ForEach-Object { Split-Path -Parent $_.FullName }
    $sourceUpscalersInclude = Get-ChildItem -Path $extractDir -Filter "ffx_upscale.h" -Recurse |
        Select-Object -First 1 |
        ForEach-Object { Split-Path -Parent $_.FullName }

    if (-not $sourceApiInclude -or -not (Copy-DirectorySafe -Source $sourceApiInclude -Destination $apiIncludeDir)) {
        $fallbackApiInclude = Join-Path $repoRoot "third_party\amd_fsr_sdk_repo\Kits\FidelityFX\api\include"
        if (-not (Copy-DirectorySafe -Source $fallbackApiInclude -Destination $apiIncludeDir)) {
            throw "AMD FidelityFX API headers were not found in either the release package or third_party\amd_fsr_sdk_repo."
        }
    }

    if (-not $sourceUpscalersInclude -or -not (Copy-DirectorySafe -Source $sourceUpscalersInclude -Destination $upscalersIncludeDir)) {
        $fallbackUpscalersInclude = Join-Path $repoRoot "third_party\amd_fsr_sdk_repo\Kits\FidelityFX\upscalers\include"
        if (-not (Copy-DirectorySafe -Source $fallbackUpscalersInclude -Destination $upscalersIncludeDir)) {
            throw "AMD FidelityFX upscaler headers were not found in either the release package or third_party\amd_fsr_sdk_repo."
        }
    }

    foreach ($requiredPath in @(
        $loaderLib,
        $loaderDll,
        $upscalerDll,
        (Join-Path $apiIncludeDir "ffx_api.h"),
        (Join-Path $upscalersIncludeDir "ffx_upscale.h")
    )) {
        if (-not (Test-Path $requiredPath)) {
            throw "AMD FSR SDK installation is incomplete. Missing: $requiredPath"
        }
    }

    Write-Host "AMD FSR SDK files installed to: $amdFidelityFxRoot"
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = Join-Path $repoRoot "third_party\vcpkg"
}
elseif (-not [System.IO.Path]::IsPathRooted($VcpkgRoot)) {
    $VcpkgRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $VcpkgRoot))
}

if ([string]::IsNullOrWhiteSpace($ManifestRoot)) {
    $ManifestRoot = $repoRoot
}
else {
    $ManifestRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $ManifestRoot))
}

if (-not (Test-SubPath -Path $ManifestRoot -Root $repoRoot)) {
    throw "ManifestRoot must be inside the repository: $ManifestRoot"
}

if (-not $SkipSubmodules -and (Test-Path (Join-Path $repoRoot ".git")) -and (Test-Path (Join-Path $repoRoot ".gitmodules"))) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw "git is required to initialize repository submodules."
    }

    Write-Host "Initializing repository submodules..."
    git -C $repoRoot submodule update --init --recursive
}

if (-not (Test-Path $VcpkgRoot)) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw "git is required to clone vcpkg."
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $VcpkgRoot) | Out-Null
    git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
}

$vcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path $vcpkgExe)) {
    $bootstrap = Join-Path $VcpkgRoot "bootstrap-vcpkg.bat"
    if (-not (Test-Path $bootstrap)) {
        throw "Could not find bootstrap-vcpkg.bat in '$VcpkgRoot'."
    }

    & $bootstrap -disableMetrics
}

if ($SkipInstall) {
    Write-Host "vcpkg is ready at: $VcpkgRoot"
    Write-Host "Skipped package install because -SkipInstall was used."
}
else {
    $installRoot = Join-Path $repoRoot "vcpkg_installed"
    & $vcpkgExe install "--triplet=$Triplet" "--x-manifest-root=$ManifestRoot" "--x-install-root=$installRoot"

    Write-Host ""
    Write-Host "vcpkg packages installed."
    Write-Host "VcpkgManifestRoot=$ManifestRoot"
    Write-Host "VCPKG_ROOT=$VcpkgRoot"
    Write-Host "VcpkgInstalledDir=$installRoot\$Triplet\"
}

if ($SkipAmdFsrSdk) {
    Write-Host "Skipped AMD FSR SDK download because -SkipAmdFsrSdk was used."
}
else {
    Install-AmdFsrSdk
}
