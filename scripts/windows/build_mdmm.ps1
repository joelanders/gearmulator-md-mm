[CmdletBinding()]
param(
    [string] $SourceDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string] $BuildDir = '',
    [string] $OutputDir = '',
    [string] $Configuration = 'Release',
    [ValidateRange(1, 64)] [int] $Parallel = 4,
    [ValidateSet('Visual Studio 17 2022', 'Ninja Multi-Config')]
    [string] $Generator = 'Visual Studio 17 2022',
    [string] $CompilerLauncher = '',
    [ValidateSet('none', 'generate', 'use')]
    [string] $PgoMode = 'none',
    [string] $PgoDirectory = '',
    [switch] $WithTests,
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
if ($PgoMode -ne 'none' -and -not $PgoDirectory) {
    throw '-PgoDirectory is required when -PgoMode is generate or use.'
}
if ($PgoMode -ne 'none' -and $Configuration -ne 'Release') {
    throw 'MSVC PGO is supported only for Release configurations.'
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
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

if (-not $TestOnly) {
    $configureArgs = @(
        '-S', $SourceDir,
        '-B', $BuildDir,
        '-G', $Generator,
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
        '-Dgearmulator_SYNTH_JE8086=OFF',
        "-DGEARMULATOR_MDMM_MSVC_PGO_MODE=$PgoMode"
    )
    if ($PgoMode -ne 'none') {
        $configureArgs += @(
            "-DGEARMULATOR_MDMM_MSVC_PGO_DIRECTORY=$([IO.Path]::GetFullPath($PgoDirectory))"
        )
    }
    if ($Generator -eq 'Visual Studio 17 2022') {
        $configureArgs += @('-A', 'x64')
    } else {
        $configureArgs += @(
            '-DCMAKE_C_COMPILER=cl',
            '-DCMAKE_CXX_COMPILER=cl'
        )
    }
    if ($CompilerLauncher) {
        $configureArgs += @(
            "-DCMAKE_C_COMPILER_LAUNCHER=$CompilerLauncher",
            "-DCMAKE_CXX_COMPILER_LAUNCHER=$CompilerLauncher"
        )
        if ($Generator -eq 'Ninja Multi-Config') {
            # A shared /Zi compiler PDB races under parallel cached builds.
            # /Z7 keeps equivalent debug data in each object and is cacheable.
            $configureArgs += '-DGEARMULATOR_MSVC_EMBED_DEBUG_INFO=ON'
        }
    }
    Invoke-Native -FilePath $cmake -Arguments $configureArgs

    $nativeBuildArgs = @()
    if ($Generator -eq 'Visual Studio 17 2022') {
        $nativeBuildArgs = @('--', '/verbosity:minimal')
    }

    $targets = @(
        'mdJucePlugin_VST3',
        'mmJucePlugin_VST3',
        'mdJucePlugin_Standalone',
        'mmJucePlugin_Standalone',
        'pluginTester'
    )
    if ($WithTests) {
        $targets += @('baseLibBinaryStreamTest', 'synthLibAudioTest', 'mdLibTest', 'mdAudioQueueTest',
            'mdAudioFirmwareTest', 'mdAudioIoLayoutTest', 'mdProjectStateRestoreTest', 'mdProgramChangeFirmwareTest',
            'mdAudioProbePlugin_VST3', 'vst3ProgramChangeTest', 'mdProgramChangeProbe_VST3')
    }
    Invoke-Native -FilePath $cmake -Arguments (@(
        '--build', $BuildDir,
        '--config', $Configuration,
        '--target') + $targets + @('--parallel', "$Parallel") + $nativeBuildArgs)

    if ($WithTests) {
        Invoke-Native -FilePath $cmake -Arguments (@(
            '--build', $BuildDir,
            '--config', $Configuration,
            '--target',
            'midiOutputDispatcherTest',
            'mdAutomationMidiTest',
            'mdAutomationParameterTest',
            'mdAutomationRobustnessTest',
            '--parallel', "$Parallel") + $nativeBuildArgs)
    }

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
        '--tests-regex', '^(baseLibBinaryStreamTest|synthLibAudioTest|mdLibTests|mdAudioQueueTest|mdAudioFirmwareTest|mdAudioIoLayoutTest|mdProjectStateRestoreTest|mdProgramChangeFirmwareTest|mdAudioProbePluginVST3IdentityTest|mdVst3ProgramChange(Test|OptOutTest)|(md|mm)JucePlugin_VST3ProgramChangeTest)$'
    )
    Invoke-Native -FilePath $ctest -Arguments @(
        '--test-dir', $BuildDir,
        '-C', $Configuration,
        '--output-on-failure',
        '--tests-regex', '^(midiOutputDispatcherTest|mdAutomationMidiTest|mdAutomationParameterTest|mdAutomationArchitectureTest)$'
    )
}

$configuredPgo = Select-String -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') `
    -Pattern '^GEARMULATOR_MDMM_MSVC_PGO_MODE:STRING=(.*)$'
if (@($configuredPgo).Count -ne 1 -or $configuredPgo.Matches[0].Groups[1].Value -ne $PgoMode) {
    throw 'Requested PGO mode does not match the configured build tree.'
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
$previousPath = $env:PATH
try {
    if ($PgoMode -eq 'generate') {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $installationPath = (& $vswhere -latest -products * -property installationPath).Trim()
        if (-not $installationPath) { throw 'No Visual Studio installation was found.' }
        $runtime = @(Get-ChildItem (Join-Path $installationPath 'VC\Tools\MSVC') `
            -Recurse -File -Filter pgort140.dll |
            Where-Object FullName -like '*\bin\Hostx64\x64\pgort140.dll' |
            Sort-Object FullName -Descending)
        if ($runtime.Count -eq 0) { throw 'The x64 MSVC PGO runtime was not found.' }
        $env:PATH = "$($runtime[0].DirectoryName);$previousPath"
    }
    foreach ($bundle in @($mdVst3, $mmVst3)) {
        Invoke-Native -FilePath $pluginTester.FullName -Arguments @(
            '-verify-audio-buses', '-automation-smoke', '-blocks', '16',
            '-plugin', $bundle.FullName)
    }
} finally {
    $env:PATH = $previousPath
}

if ($PgoMode -eq 'generate') {
    foreach ($bundle in @($mdVst3, $mmVst3)) {
        Get-ChildItem -LiteralPath $bundle.FullName -Recurse -File -Filter '*.pgc' |
            Move-Item -Destination $PgoDirectory -Force
    }
    foreach ($target in @('mdJucePlugin_VST3', 'mmJucePlugin_VST3')) {
        if (-not (Get-ChildItem -LiteralPath $PgoDirectory -File -Filter "$target!*.pgc")) {
            throw "Instrumented smoke test produced no execution counts for $target."
        }
    }
    Write-Host "WINDOWS_MDMM_SMOKE_PROFILES=$PgoDirectory"
    # Instrumented executables require developer runtimes and are not packages.
    return
}

$artifacts = @($mdVst3, $mmVst3, $mdStandalone, $mmStandalone)
$forbiddenPayloads = @(
    Get-ChildItem -LiteralPath $artifacts.FullName -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -match '^\.(bin|rom|nvram|syx|wav|pgc|pgd|gcda|profraw|profdata)$' }
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

$sourceCommit = (& $git -C $SourceDir rev-parse HEAD).Trim()
$dspCommit = (& $git -C $SourceDir rev-parse 'HEAD:source/dsp56300').Trim()
$mc68kCommit = (& $git -C $SourceDir rev-parse 'HEAD:source/mc68k').Trim()
$pgoTargets = @()
if ($PgoMode -ne 'none') {
    $cacheLine = Select-String -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') `
        -Pattern '^GEARMULATOR_MDMM_MSVC_OPTIMIZATION_APPLIED_TARGETS:INTERNAL=(.*)$'
    if ($cacheLine.Matches.Count -ne 1) {
        throw 'The configured build did not record its MSVC PGO targets.'
    }
    $pgoTargets = @($cacheLine.Matches[0].Groups[1].Value -split ';' |
        Where-Object { $_ })
    if ($PgoMode -eq 'use') {
        $expectedPgoTargets = @('mdJucePlugin_VST3', 'mmJucePlugin_VST3',
            'mdJucePlugin_Standalone', 'mmJucePlugin_Standalone')
        if (@(Compare-Object $expectedPgoTargets $pgoTargets).Count -ne 0) {
            throw "Profile-use build did not cover every product target: $($pgoTargets -join ', ')"
        }
    }
}
$receipt = [ordered]@{
    schema = 'gearmulator-elektron-windows-build-v1'
    created_utc = [DateTime]::UtcNow.ToString('o')
    configuration = $Configuration
    architecture = 'x64'
    pgo_mode = $PgoMode
    pgo_targets = $pgoTargets
    source_commit = $sourceCommit
    dsp56300_commit = $dspCommit
    mc68k_commit = $mc68kCommit
    firmware_included = $false
    tests_run = $true
    plugin_smoke_tests_run = $true
    full_test_suite_run = [bool]$WithTests
    artifacts = $receiptArtifacts
}

$receiptPath = Join-Path $OutputDir 'Gearmulator-Elektron-Windows-x64-receipt.json'
$receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
$zipPath = Join-Path $OutputDir 'Gearmulator-Elektron-Windows-x64.zip'
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
$packageStage = Join-Path $OutputDir ('.package-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $packageStage | Out-Null
try {
    # Loading a plug-in may create runtime preferences beside its module. Ship
    # only the actual module, bundle resources, standalone programs and license.
    foreach ($bundle in @($mdVst3, $mmVst3)) {
        $module = Find-ExactlyOne -Kind "$($bundle.Name) module" -Candidates @(
            Get-ChildItem -LiteralPath $bundle.FullName -Recurse -File -Filter '*.vst3')
        $contents = Join-Path $packageStage "$($bundle.Name)\Contents"
        $moduleDir = Join-Path $contents 'x86_64-win'
        New-Item -ItemType Directory -Path $moduleDir -Force | Out-Null
        Copy-Item -LiteralPath $module.FullName -Destination $moduleDir
        $resources = Join-Path $bundle.FullName 'Contents\Resources'
        if (Test-Path -LiteralPath $resources) {
            Copy-Item -LiteralPath $resources -Destination $contents -Recurse
        }
    }
    Copy-Item -LiteralPath $mdStandalone.FullName, $mmStandalone.FullName,
        (Join-Path $SourceDir 'LICENSE.md') -Destination $packageStage
    Compress-Archive -LiteralPath @(Get-ChildItem -LiteralPath $packageStage |
        ForEach-Object FullName) -DestinationPath $zipPath -CompressionLevel Optimal
} finally {
    Remove-Item -LiteralPath $packageStage -Recurse -Force
}

Write-Host "WINDOWS_MDMM_ZIP=$zipPath"
Write-Host "WINDOWS_MDMM_RECEIPT=$receiptPath"
Get-FileHash -LiteralPath $zipPath -Algorithm SHA256
