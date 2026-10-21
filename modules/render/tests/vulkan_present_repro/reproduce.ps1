param(
    [Parameter(Mandatory = $true)][string]$Sdk,
    [string]$BuildDirectory = (Join-Path (Get-Location).Path 'build_vk_present_repro'),
    [ValidateRange(1, 100)][int]$Repeat = 5,
    [string]$Generator = 'Visual Studio 18 2026'
)

$ErrorActionPreference = 'Stop'
$sdkPath = (Resolve-Path -LiteralPath $Sdk).Path
$buildPath = [System.IO.Path]::GetFullPath($BuildDirectory)
cmake -S $PSScriptRoot -B $buildPath -G $Generator -A x64 `
    "-DVulkan_INCLUDE_DIR=$sdkPath/Include" "-DVulkan_LIBRARY=$sdkPath/Lib/vulkan-1.lib"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
foreach ($config in @('Debug', 'Release')) {
    cmake --build $buildPath --config $config --parallel 4
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$results = @()
foreach ($config in @('Debug', 'Release')) {
    foreach ($mode in @('baseline', 'empty-predecessor', 'no-timeline-chain', 'one-window')) {
        $runArguments = @()
        if ($mode -ne 'baseline') { $runArguments += "--$mode" }
        foreach ($iteration in 1..$Repeat) {
            $log = Join-Path $buildPath "${config}_${mode}_${iteration}.log"
            cmake -E env "VK_LAYER_PATH=$sdkPath/Bin" `
                "$buildPath/$config/vulkan_present_repro.exe" @runArguments > $log 2>&1
            $runExit = $LASTEXITCODE
            $content = Get-Content -Raw -LiteralPath $log
            $totals = [regex]::Match($content, 'TOTAL errors=(\d+) WRITE_AFTER_PRESENT=(\d+)')
            $errors = if ($totals.Success) { [int]$totals.Groups[1].Value } else { -1 }
            $hazards = if ($totals.Success) { [int]$totals.Groups[2].Value } else { -1 }
            $cleanupDiagnostic = $content.Contains('phase=cleanup')
            $matched = if ($mode -eq 'baseline') {
                $runExit -eq 1 -and $errors -gt 0 -and $errors -eq $hazards -and !$cleanupDiagnostic
            } else {
                $runExit -eq 0 -and $errors -eq 0 -and !$cleanupDiagnostic
            }
            $results += [PSCustomObject]@{
                Config = $config; Mode = $mode; Iteration = $iteration; Exit = $runExit
                Errors = $errors; WriteAfterPresent = $hazards; CleanupDiagnostic = $cleanupDiagnostic
                MatchesSdk357Observation = $matched; Log = $log
            }
        }
    }
}
$results | Export-Csv (Join-Path $buildPath 'results.csv') -NoTypeInformation
$results | Format-Table Config, Mode, Iteration, Exit, Errors, WriteAfterPresent, MatchesSdk357Observation
# A changed baseline result on another layer version is evidence to inspect, not automatically a regression.
if ($results.Where({ !$_.MatchesSdk357Observation }).Count) { exit 1 }
