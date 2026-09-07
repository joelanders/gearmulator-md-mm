# Isolated diagnostic branch: build only the private-fixture comparison runner.
param([string]$SourceDir, [string]$BuildDir, [string]$OutputDir,
    [string]$Configuration='Release', [int]$Parallel=4,
    [string]$Generator='Ninja Multi-Config', [string]$CompilerLauncher='',
    [switch]$WithTests, [switch]$BuildOnly, [switch]$TestOnly)
$ErrorActionPreference='Stop'
if (!$TestOnly) {
    & cmake -S $SourceDir -B $BuildDir -G $Generator `
      -DBUILD_TESTING=ON -Dgearmulator_BUILD_JUCEPLUGIN=OFF `
      -Dgearmulator_SYNTH_ELEKTRON=ON -Dgearmulator_SYNTH_OSIRUS=OFF `
      -Dgearmulator_SYNTH_OSTIRUS=OFF -Dgearmulator_SYNTH_VAVRA=OFF `
      -Dgearmulator_SYNTH_XENIA=OFF -Dgearmulator_SYNTH_NODALRED2X=OFF `
      -Dgearmulator_SYNTH_JE8086=OFF -DGEARMULATOR_MSVC_EMBED_DEBUG_INFO=ON `
      "-DCMAKE_C_COMPILER_LAUNCHER=$CompilerLauncher" "-DCMAKE_CXX_COMPILER_LAUNCHER=$CompilerLauncher"
    if ($LASTEXITCODE) { throw 'Configure failed' }
    & cmake --build $BuildDir --config $Configuration --parallel $Parallel --target mdManualFirmwareComparison dsp56kAccumulatorTests mdHostRxTimingTest
    if ($LASTEXITCODE) { throw 'Build failed' }
}
if ($BuildOnly) { exit 0 }
& ctest --test-dir $BuildDir -C $Configuration --output-on-failure --no-tests=error --tests-regex '^(dsp56300_accumulatorTests|mdHostRxTimingTest)$'
if ($LASTEXITCODE) { throw 'Regression tests failed' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
Get-ChildItem $BuildDir -Recurse -Filter '*.exe' | Where-Object { $_.Name -in @('mdManualFirmwareComparison.exe','dsp56kAccumulatorTests.exe') -and $_.FullName -match $Configuration } | Copy-Item -Destination $OutputDir
$identity = [ordered]@{ plugin=(git -C $SourceDir rev-parse HEAD); dsp=(git -C "$SourceDir/source/dsp56300" rev-parse HEAD); mcu=(git -C "$SourceDir/source/mc68k" rev-parse HEAD); firmware_included=$false }
$identity | ConvertTo-Json | Set-Content (Join-Path $OutputDir 'source-identity.json')
Copy-Item "$BuildDir/Testing/Temporary/LastTest.log" $OutputDir
