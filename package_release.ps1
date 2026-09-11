# Package the ordinary OptiScaler dependencies and the open-source NR forwarder.
# NVIDIA model/FG runtimes and unrelated optional payloads are never collected from build folders.
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$Version = 'nr-dev',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath
$stage = Join-Path $root "release/$Version"
$zip = Join-Path $root "release/OptiScaler-NR-$Version.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip)) {
    throw 'Release output already exists. Choose a new -Version; existing packages are not overwritten.'
}

if (-not $SkipBuild) {
    $msbuild = (Get-Command MSBuild.exe -ErrorAction SilentlyContinue).Source
    if (-not $msbuild) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild/Current/Bin/MSBuild.exe'
        }
    }
    if (-not $msbuild) { throw 'MSBuild.exe was not found. Use a Visual Studio developer PowerShell.' }
    & $msbuild (Join-Path $root 'OptiScaler.sln') /p:Configuration=Release /p:Platform=x64 /p:PostBuildEventUseInBuild=false /v:minimal /m
    if ($LASTEXITCODE -ne 0) { throw 'OptiScaler build failed.' }
}

$buildRoot = Join-Path $root 'x64/Release'
$forwarder = Join-Path $buildRoot 'a/nvngx.dll_dlssnr.dll'
$exports = @('dlssnr_call_create', 'dlssnr_call_evaluate_v2', 'dlssnr_call_set_extras',
             'dlssnr_vk_probe', 'dlssnr_vk_init', 'dlssnr_vk_create', 'dlssnr_vk_evaluate_v2', 'dlssnr_vk_release')
$binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($forwarder))
$missing = @($exports | Where-Object { $binaryText.IndexOf($_, [StringComparison]::Ordinal) -lt 0 })
if ($missing.Count) { throw "Stale forwarder: missing $($missing -join ', ')" }

# Validate every source before creating the staging tree. An explicit manifest prevents stale
# Streamline/MFG or discarded experiment files from a previous build entering this package.
$files = @{}
$files['OptiScaler.dll'] = Join-Path $buildRoot 'OptiScaler.dll'
$files['nvngx.dll_dlssnr.dll'] = $forwarder
foreach ($name in @('OptiScaler.ini', 'setup_windows.bat', 'setup_linux.sh', 'README.md', 'INSTALL-DLSSNR.md', 'LICENSE',
                    'Features.md', 'Config.md', 'Spoofing.md', 'images/gh-sponsor-red.png', 'images/bmac.png',
                    'OptiScaler/dlssnr/README.md')) {
    $files[$name] = Join-Path $root $name
}
foreach ($name in @('libxess.dll', 'libxess_dx11.dll', 'libxell.dll', 'libxess_fg.dll')) {
    $files["OptiScaler/$name"] = Join-Path $root "external/xess/bin/$name"
}
$files['OptiScaler/amd_fidelityfx_vk.dll'] = Join-Path $root 'external/FidelityFX-SDK/PrebuiltSignedDLL/amd_fidelityfx_vk.dll'
foreach ($name in @('amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll')) {
    $files["OptiScaler/$name"] = Join-Path $root "external/FidelityFX-SDK-v2/Kits/FidelityFX/signedbin/$name"
}
$files['OptiScaler/D3D12_OptiScaler/D3D12Core.dll'] = Join-Path $root 'external/directx_agility_sdk/lib/D3D12Core.dll'
$files['Licenses/XeSS_LICENSE.txt'] = Join-Path $root 'external/xess/LICENSE.txt'
$files['Licenses/FidelityFX_v1_LICENSE.md'] = Join-Path $root 'external/FidelityFX-SDK/docs/license.md'
$files['Licenses/FidelityFX_v2_LICENSE.md'] = Join-Path $root 'external/FidelityFX-SDK-v2/docs/license.md'
$files['Licenses/DirectX_LICENSE.txt'] = Join-Path $root 'external/directx_agility_sdk/LICENSE.txt'
$files['Licenses/RenoDX_ATTRIBUTION.txt'] = Join-Path $root 'Licenses/RenoDX_ATTRIBUTION.txt'
foreach ($name in @('CREDITS.md', 'NR-COMPATIBILITY.md', 'NR-MOTION-METADATA.md', 'PADDED-PRESR.md',
                    'DEFERRED-NR-DLSS.md', 'RESIDUAL-ACROSS-RR.md', 'COMPATIBILITY-CHANGES.md',
                    'ISSUE-REVIEW-2026-09-10.md', 'PR-REVIEW-20260909.md', 'VULKAN-PARITY-REVIEW.md')) {
    $files["docs/$name"] = Join-Path $root "docs/$name"
}
foreach ($entry in $files.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) {
        throw "Required release file is missing: $($entry.Value)"
    }
}

$ini = Get-Content -LiteralPath $files['OptiScaler.ini'] -Raw
if ($ini -match '(?mi)^Enabled=true\s*$') { throw 'A feature is enabled in the default INI.' }
foreach ($key in @('FinishedPicture', 'DeferredDLSS', 'UnlockPasses')) {
    if ($ini -match "(?mi)^$key=true\s*$") { throw "Experimental option $key is enabled in the default INI." }
}
if ($ini -notmatch '(?mi)^TargetProcessName=auto\s*$') { throw 'The INI contains a game-specific process filter.' }

New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($entry in $files.GetEnumerator()) {
    $destination = Join-Path $stage $entry.Key
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $entry.Value -Destination $destination
}
[IO.File]::WriteAllText((Join-Path $stage '!! EXTRACT ALL FILES TO GAME FOLDER !!'), '')

$checksums = Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($stage.TrimEnd('\', '/').Length).TrimStart('\', '/').Replace('\', '/')
    '{0} *{1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash, $relative
}
[IO.File]::WriteAllLines((Join-Path $stage 'SHA256SUMS.txt'), $checksums, [Text.UTF8Encoding]::new($false))
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Output "Created $zip"
