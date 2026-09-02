[CmdletBinding()]
param(
    [string] $SourceDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string] $BuildDir = '',
    [string] $OutputDir = '',
    [string] $Configuration = 'Release',
    [ValidateRange(1, 64)] [int] $Parallel = 4,
    [switch] $WithTests,
    [switch] $AudioDiagnostics,
    [string] $AsioSdkPath = '',
    [switch] $BuildOnly,
    [switch] $TestOnly
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
    param(
        [Parameter(Mandatory = $true)] [string] $Kind,
        [Parameter(Mandatory = $true)] [object[]] $Candidates
    )
    if ($Candidates.Count -ne 1) {
        $paths = $Candidates | ForEach-Object { $_.FullName }
        throw "Expected exactly one $Kind artifact, found $($Candidates.Count): $($paths -join ', ')"
    }
    return $Candidates[0]
}

if ($env:OS -ne 'Windows_NT') {
    throw 'build_mdmm.ps1 requires Windows.'
}
if ($BuildOnly -and $TestOnly) {
    throw '-BuildOnly and -TestOnly are mutually exclusive.'
}

$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path
if (-not $BuildDir) { $BuildDir = Join-Path $SourceDir 'build\windows-mdmm' }
if (-not $OutputDir) { $OutputDir = Join-Path $SourceDir 'artifacts\windows-mdmm' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($BuildDir -eq $SourceDir -or $OutputDir -eq $SourceDir) {
    throw 'BuildDir and OutputDir must not be the source directory.'
}
if ($TestOnly -and -not (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) {
    throw "Test-only mode requires a configured build tree: $BuildDir"
}

$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ctest = (Get-Command ctest -ErrorAction Stop).Source
$git = (Get-Command git -ErrorAction Stop).Source
$sourceCommit = (& $git -C $SourceDir rev-parse HEAD).Trim()
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

if (-not $TestOnly) {
    $configureArgs = @(
        '-S', $SourceDir,
        '-B', $BuildDir,
        '-G', 'Visual Studio 17 2022',
        '-A', 'x64',
        "-DBUILD_TESTING=$(if ($WithTests) { 'ON' } else { 'OFF' })",
        '-Dgearmulator_BUILD_JUCEPLUGIN=ON',
        '-Dgearmulator_BUILD_FX_PLUGIN=OFF',
        '-Dgearmulator_BUILD_JUCEPLUGIN_VST2=OFF',
        '-Dgearmulator_BUILD_JUCEPLUGIN_VST3=ON',
        '-Dgearmulator_BUILD_JUCEPLUGIN_CLAP=OFF',
        '-Dgearmulator_BUILD_JUCEPLUGIN_LV2=OFF',
        '-Dgearmulator_BUILD_JUCEPLUGIN_AU=OFF',
        '-Dgearmulator_BUILD_JUCEPLUGIN_Standalone=ON',
        '-Dgearmulator_SYNTH_ELEKTRON=ON',
        '-Dgearmulator_SYNTH_OSIRUS=OFF',
        '-Dgearmulator_SYNTH_OSTIRUS=OFF',
        '-Dgearmulator_SYNTH_VAVRA=OFF',
        '-Dgearmulator_SYNTH_XENIA=OFF',
        '-Dgearmulator_SYNTH_NODALRED2X=OFF',
        '-Dgearmulator_SYNTH_JE8086=OFF'
        "-Dgearmulator_MDMM_AUDIO_DIAGNOSTICS=$(if ($AudioDiagnostics) { 'ON' } else { 'OFF' })"
        "-Dgearmulator_BUILD_COMMIT=$sourceCommit"
    )
    if ($AsioSdkPath) {
        $AsioSdkPath = (Resolve-Path -LiteralPath $AsioSdkPath).Path
        if (-not (Test-Path -LiteralPath (Join-Path $AsioSdkPath 'iasiodrv.h'))) {
            throw "ASIO SDK path does not contain iasiodrv.h: $AsioSdkPath"
        }
        $configureArgs += "-Dgearmulator_ASIO_SDK_PATH=$AsioSdkPath"
    }
    Invoke-Native -FilePath $cmake -Arguments $configureArgs

    $targets = @(
        'mdJucePlugin_VST3',
        'mmJucePlugin_VST3',
        'mdJucePlugin_Standalone',
        'mmJucePlugin_Standalone',
        'pluginTester'
    )
    if ($WithTests) {
        $targets += @('synthLibAudioTest', 'mdLibTest', 'mdAudioQueueTest',
            'mdAudioFirmwareTest', 'mdAudioIoLayoutTest', 'mdAudioProbePlugin_VST3')
    }
    Invoke-Native -FilePath $cmake -Arguments (@(
        '--build', $BuildDir,
        '--config', $Configuration,
        '--target') + $targets + @('--parallel', "$Parallel", '--', '/verbosity:minimal'))

    if ($BuildOnly) {
        Write-Host "WINDOWS_MDMM_BUILD_TREE=$BuildDir"
        return
    }
}

if ($WithTests) {
    Invoke-Native -FilePath $ctest -Arguments @(
        '--test-dir', $BuildDir,
        '-C', $Configuration,
        '--output-on-failure',
        '--tests-regex', '^(synthLibAudioTest|mdLibTests|mdAudioQueueTest|mdAudioFirmwareTest|mdAudioIoLayoutTest|mdAudioProbePluginVST3IdentityTest)$'
    )
}

$productRoot = Join-Path $SourceDir "bin\plugins\$Configuration"
$mdVst3 = Get-Item -LiteralPath (Join-Path $productRoot 'VST3\Gearmulator MD.vst3')
$mmVst3 = Get-Item -LiteralPath (Join-Path $productRoot 'VST3\Gearmulator MM.vst3')
$mdStandalone = Get-Item -LiteralPath (Join-Path $productRoot 'Standalone\Gearmulator MD.exe')
$mmStandalone = Get-Item -LiteralPath (Join-Path $productRoot 'Standalone\Gearmulator MM.exe')
$pluginTester = Find-ExactlyOne -Kind 'VST3 host' -Candidates @(
    Get-ChildItem -LiteralPath $BuildDir -Recurse -File -Filter 'pluginTester.exe')

foreach ($bundle in @($mdVst3, $mmVst3)) {
    Get-ChildItem -LiteralPath $bundle.FullName -Recurse -File -Filter 'moduleinfo.json' |
        Remove-Item -Force
}
Invoke-Native -FilePath $pluginTester.FullName -Arguments @(
    '-verify-audio-buses', '-blocks', '16', '-plugin', $mdVst3.FullName)
Invoke-Native -FilePath $pluginTester.FullName -Arguments @(
    '-verify-audio-buses', '-blocks', '16', '-plugin', $mmVst3.FullName)

$artifacts = @($mdVst3, $mmVst3, $mdStandalone, $mmStandalone)
$forbiddenPayloads = @(
    Get-ChildItem -LiteralPath $artifacts.FullName -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -match '^\.(bin|rom|nvram|syx|wav)$' }
)
if ($forbiddenPayloads.Count -ne 0) {
    throw 'Firmware or private runtime material found in final artifacts.'
}

$receiptArtifacts = foreach ($artifact in $artifacts) {
    $hashTarget = $artifact
    if ($artifact.PSIsContainer) {
        $hashTarget = Find-ExactlyOne -Kind "$($artifact.BaseName) module" -Candidates @(
            Get-ChildItem -LiteralPath $artifact.FullName -Recurse -File -Filter '*.vst3')
    }
    [ordered]@{
        name = $artifact.Name
        bytes = $hashTarget.Length
        sha256 = (Get-FileHash -LiteralPath $hashTarget.FullName -Algorithm SHA256).Hash
    }
}

$dspCommit = (& $git -C (Join-Path $SourceDir 'source\dsp56300') rev-parse HEAD).Trim()
$mc68kCommit = (& $git -C (Join-Path $SourceDir 'source\mc68k') rev-parse HEAD).Trim()
$receipt = [ordered]@{
    schema = 'gearmulator-elektron-windows-build-v1'
    created_utc = [DateTime]::UtcNow.ToString('o')
    configuration = $Configuration
    architecture = 'x64'
    source_commit = $sourceCommit
    dsp56300_commit = $dspCommit
    mc68k_commit = $mc68kCommit
    firmware_included = $false
    tests_run = [bool]$WithTests
    audio_diagnostics = [bool]$AudioDiagnostics
    asio_enabled = [bool]$AsioSdkPath
    artifacts = $receiptArtifacts
}

$receiptPath = Join-Path $OutputDir 'Gearmulator-Elektron-Windows-x64-receipt.json'
$receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
$zipPath = Join-Path $OutputDir 'Gearmulator-Elektron-Windows-x64.zip'
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
$packageFiles = @(
    $artifacts.FullName
    (Join-Path $SourceDir 'LICENSE.md')
)
if ($AsioSdkPath) {
    $asioLicense = Join-Path (Split-Path -Parent $AsioSdkPath) 'LICENSE.txt'
    if (-not (Test-Path -LiteralPath $asioLicense)) {
        throw "ASIO-enabled artifacts require the SDK license notice: $asioLicense"
    }
    $packageFiles += $asioLicense
}
Compress-Archive -LiteralPath $packageFiles -DestinationPath $zipPath -CompressionLevel Optimal

Write-Host "WINDOWS_MDMM_ZIP=$zipPath"
Write-Host "WINDOWS_MDMM_RECEIPT=$receiptPath"
Get-FileHash -LiteralPath $zipPath -Algorithm SHA256
