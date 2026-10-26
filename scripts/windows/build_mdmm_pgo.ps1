[CmdletBinding()]
param(
    [string] $SourceDir = '',
    [string] $BuildDir = '',
    [string] $OutputDir = '',
    [string] $ProfileDir = '',
    [Parameter(Mandatory = $true)] [string] $MdFirmware,
    [Parameter(Mandatory = $true)] [string] $MmFirmware,
    [ValidateRange(1, 64)] [int] $Parallel = 4,
    [ValidateRange(20, 600)] [int] $TrainingSeconds = 20,
    [ValidateSet('Visual Studio 17 2022', 'Ninja Multi-Config')]
    [string] $Generator = 'Visual Studio 17 2022',
    [string] $CompilerLauncher = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)] [string] $FilePath,
        [Parameter(Mandatory = $true)] [string[]] $Arguments
    )
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Native command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

function Find-ExactlyOne {
    param([string] $Root, [string] $Filter)
    $matches = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Filter $Filter)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one $Filter below $Root, found $($matches.Count)."
    }
    return $matches[0].FullName
}

function Find-PgoRuntimeDirectory {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "Visual Studio locator not found: $vswhere"
    }
    $installationPath = (& $vswhere -latest -products * -property installationPath).Trim()
    if (-not $installationPath) { throw 'No Visual Studio installation was found.' }
    $runtime = @(Get-ChildItem (Join-Path $installationPath 'VC\Tools\MSVC') `
        -Recurse -File -Filter pgort140.dll |
        Where-Object FullName -like '*\bin\Hostx64\x64\pgort140.dll')
    if ($runtime.Count -ne 1) {
        throw "Expected one x64 pgort140.dll, found $($runtime.Count)."
    }
    return $runtime[0].DirectoryName
}

if ($env:OS -ne 'Windows_NT') { throw 'build_mdmm_pgo.ps1 requires Windows.' }
if (-not $SourceDir) {
    $SourceDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path
$MdFirmware = (Resolve-Path -LiteralPath $MdFirmware).Path
$MmFirmware = (Resolve-Path -LiteralPath $MmFirmware).Path
if (-not $BuildDir) { $BuildDir = Join-Path $SourceDir 'build\windows-mdmm-pgo' }
if (-not $OutputDir) { $OutputDir = Join-Path $SourceDir 'artifacts\windows-mdmm-pgo' }
if (-not $ProfileDir) { $ProfileDir = Join-Path $BuildDir 'profiles' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$ProfileDir = [IO.Path]::GetFullPath($ProfileDir)

$buildScript = Join-Path $PSScriptRoot 'build_mdmm.ps1'
$common = @{
    SourceDir = $SourceDir
    BuildDir = $BuildDir
    OutputDir = $OutputDir
    Configuration = 'Release'
    Parallel = $Parallel
    Generator = $Generator
    PgoDirectory = $ProfileDir
}
if ($CompilerLauncher) { $common.CompilerLauncher = $CompilerLauncher }

New-Item -ItemType Directory -Path $ProfileDir -Force | Out-Null
Get-ChildItem -LiteralPath $ProfileDir -File |
    Where-Object { $_.Extension -in @('.pgc', '.pgd') } |
    Remove-Item -Force

& $buildScript @common -PgoMode generate -BuildOnly
if ($LASTEXITCODE -ne 0) { throw 'Instrumented Windows build failed.' }

$productRoot = Join-Path $SourceDir 'bin\plugins\Release'
$vst3Root = Join-Path $productRoot 'VST3'
$pluginTester = Find-ExactlyOne -Root $BuildDir -Filter 'pluginTester.exe'
$pgoRuntimeDirectory = Find-PgoRuntimeDirectory
Get-ChildItem -LiteralPath $vst3Root -Recurse -File -Filter '*.pgc' `
    -ErrorAction SilentlyContinue | Remove-Item -Force
$training = @(
    @{ Name = 'MD'; Plugin = Join-Path $vst3Root 'Gearmulator MD.vst3'; Firmware = $MdFirmware },
    @{ Name = 'MM'; Plugin = Join-Path $vst3Root 'Gearmulator MM.vst3'; Firmware = $MmFirmware }
)

try {
    $previousPath = $env:PATH
    $env:PATH = "$pgoRuntimeDirectory;$previousPath"
    foreach ($item in $training) {
        $privateCopy = Join-Path $vst3Root ([IO.Path]::GetFileName($item.Firmware))
        Copy-Item -LiteralPath $item.Firmware -Destination $privateCopy -Force
        try {
            Push-Location $vst3Root
            Invoke-Native -FilePath $pluginTester -Arguments @(
                '-plugin', $item.Plugin,
                '-seconds', "$TrainingSeconds",
                '-blocksize', '128',
                '-samplerate', '48000'
            )
        } finally {
            Pop-Location
            Remove-Item -LiteralPath $privateCopy -Force -ErrorAction SilentlyContinue
        }
    }
} finally {
    $env:PATH = $previousPath
    Get-ChildItem -LiteralPath $vst3Root -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -match '^\.(bin|rom|nvram|syx)$' } |
        Remove-Item -Force
}

foreach ($target in @('mdJucePlugin_VST3', 'mmJucePlugin_VST3')) {
    $pgd = Join-Path $ProfileDir "$target.pgd"
    if (-not (Test-Path -LiteralPath $pgd)) { throw "Missing profile database: $pgd" }
}
$generatedProfiles = @(Get-ChildItem -LiteralPath $vst3Root -Recurse -File -Filter '*.pgc')
foreach ($profile in $generatedProfiles) {
    Move-Item -LiteralPath $profile.FullName -Destination $ProfileDir -Force
}
$counts = @(Get-ChildItem -LiteralPath $ProfileDir -File -Filter '*.pgc')
if ($counts.Count -eq 0) { throw "Training produced no .pgc files in $ProfileDir" }

& $buildScript @common -PgoMode use
if ($LASTEXITCODE -ne 0) { throw 'Profile-use Windows build or validation failed.' }

Write-Host "WINDOWS_MDMM_PGO_OUTPUT=$OutputDir"
