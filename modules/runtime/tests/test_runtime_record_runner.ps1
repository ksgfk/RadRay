#requires -Version 7.2
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime_profile_runner_support.ps1')
. (Join-Path $PSScriptRoot 'runtime_record_runner_support.ps1')
function Assert-RecordRunner([bool]$Condition, [string]$Message) {
    if (-not $Condition) { Write-Error $Message; exit 1 }
}
function New-RecordTestLines {
    @(for ($recordIndex = 0; $recordIndex -lt 30; ++$recordIndex) {
        'RECORD_FRAME round={0} frame={1} warmup={2} runtimeFirst={3} runtimeNs=110 referenceNs=100 draws=1024' -f
            [Math]::Floor($recordIndex / 6), $recordIndex, [int](($recordIndex % 6) -lt 2), [int]($recordIndex % 2 -eq 0)
    })
}
function New-RecordTestBuild {
    [pscustomobject]@{commit='fixture'; trackedDirty=$false; trackedDiffSha256='fixture'; runtimeTestsSha256='fixture'; runtimeSourcesSha256='fixture'; harnessSha256='fixture';
        compiler='fixture'; flags='fixture'; configuration='Release'; profilerEnabled=$true; detailedProfilingEnabled=$false}
}
$recordTestDirectory = Join-Path ([IO.Path]::GetTempPath()) ('RadRay-record-runner-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $recordTestDirectory | Out-Null
$recordCases = [ordered]@{
    zeroReference = { param($Lines) @($Lines | ForEach-Object { $_.Replace('referenceNs=100', 'referenceNs=0') }) }
    zeroRuntime = { param($Lines) @($Lines | ForEach-Object { $_.Replace('runtimeNs=110', 'runtimeNs=0') }) }
    referenceOverflow = { param($Lines) $Lines[2] = $Lines[2].Replace('referenceNs=100', 'referenceNs=18446744073709551616'); $Lines }
    runtimeOverflow = { param($Lines) $Lines[2] = $Lines[2].Replace('runtimeNs=110', 'runtimeNs=9223372036854775808'); $Lines }
    frameOverflow = { param($Lines) $Lines[2] = $Lines[2].Replace('frame=2 ', 'frame=2147483648 '); $Lines }
    roundOverflow = { param($Lines) $Lines[2] = $Lines[2].Replace('round=0 ', 'round=2147483648 '); $Lines }
    drawsOverflow = { param($Lines) $Lines[2] = $Lines[2].Replace('draws=1024', 'draws=2147483648'); $Lines }
    negative = { param($Lines) $Lines[2] = $Lines[2].Replace('referenceNs=100', 'referenceNs=-1'); $Lines }
    malformed = { param($Lines) $Lines[2] = 'RECORD_FRAME invalid'; $Lines }
    missing = { param($Lines) $Lines[0..28] }
    duplicate = { param($Lines) $Lines[3] = $Lines[2]; $Lines }
    warmup = { param($Lines) $Lines[2] = $Lines[2].Replace('warmup=0', 'warmup=1'); $Lines }
    order = { param($Lines) $Lines[2] = $Lines[2].Replace('runtimeFirst=1', 'runtimeFirst=0'); $Lines }
    identity = { param($Lines) $Lines }
    configuration = { param($Lines) $Lines }
    unknownProfiler = { param($Lines) $Lines }
    typedProfiler = { param($Lines) $Lines }
    typedOptions = { param($Lines) $Lines }
}
foreach ($recordCase in $recordCases.Keys) {
    $recordErrors = [System.Collections.Generic.List[string]]::new()
    $recordLines = @(& $recordCases[$recordCase] (New-RecordTestLines))
    $recordRows = @(Read-RecordFrames $recordLines $recordErrors)
    Test-RecordFrames $recordRows 2 4 5 $recordErrors
    $recordBuild = New-RecordTestBuild
    if ($recordCase -eq 'identity') { Compare-ProfileFields @{sha256='compiled'} @{sha256='changed'} @('sha256') $recordErrors 'binary' }
    if ($recordCase -eq 'configuration') { $recordBuild.configuration = 'Debug' }
    if ($recordCase -eq 'unknownProfiler') { $recordBuild.PSObject.Properties.Remove('profilerEnabled') }
    if ($recordCase -eq 'typedProfiler') { $recordBuild.profilerEnabled = 'true' }
    Test-RecordBuildConfiguration $recordBuild $false $recordErrors
    if ($recordCase -eq 'typedOptions') { Test-RecordOptions @{driverValidation=$false; samples=4} ([pscustomobject]@{sourceRoot='fixture'; driverValidation='false'; samples='4'}) $recordErrors }
    Assert-RecordRunner ($recordErrors.Count -gt 0) "$recordCase did not produce a validation error."
    $recordStatistics = Get-RecordStatistics $recordRows $false $recordErrors
    Assert-RecordRunner (-not $recordStatistics.performanceEvidenceEligible -and -not $recordStatistics.p50Within115Percent) "$recordCase remained eligible."
    Assert-RecordRunner (@($recordStatistics.rounds | Where-Object { $null -ne $_.p50Ratio -or $null -ne $_.p95Ratio }).Count -eq 0) "$recordCase retained a comparison ratio."
    $recordOutput = Join-Path $recordTestDirectory $recordCase
    New-Item -ItemType Directory -Path $recordOutput | Out-Null
    Set-Content -LiteralPath (Join-Path $recordOutput 'record.log') -Value $recordLines
    $recordStatistics['errors'] = $recordErrors
    Save-RecordEvidence $recordRows $recordStatistics $recordOutput
    $recordSaved = Get-Content -LiteralPath (Join-Path $recordOutput 'manifest.json') -Raw | ConvertFrom-Json
    Assert-RecordRunner ($recordSaved.errors.Count -gt 0 -and -not $recordSaved.performanceEvidenceEligible -and (Test-Path -LiteralPath (Join-Path $recordOutput 'frames.jsonl'))) "$recordCase lost its rejected evidence."
}
$recordErrors = [System.Collections.Generic.List[string]]::new()
$recordRows = @(Read-RecordFrames (New-RecordTestLines) $recordErrors)
Test-RecordFrames $recordRows 2 4 5 $recordErrors
Test-RecordBuildConfiguration (New-RecordTestBuild) $false $recordErrors
$recordStatistics = Get-RecordStatistics $recordRows $false $recordErrors
Assert-RecordRunner ($recordErrors.Count -eq 0 -and $recordStatistics.performanceEvidenceEligible -and $recordStatistics.p50Within115Percent -and $recordStatistics.rounds[0].p50Ratio -eq 1.1) 'Valid synthetic paired records were rejected or summarized incorrectly.'
Assert-RecordRunner (-not (Get-RecordStatistics $recordRows $true $recordErrors).performanceEvidenceEligible) 'Smoke evidence became eligible.'
$recordRows[2].runtimeNs = [long]::MaxValue
$recordRows[2].referenceNs = [long]::MaxValue
$recordStatistics = Get-RecordStatistics $recordRows $false $recordErrors
Assert-RecordRunner ($recordStatistics.rounds[0].runtimeNs.p99 -eq [long]::MaxValue) 'A representable timestamp was truncated.'
$recordEmptyErrors = [System.Collections.Generic.List[string]]::new()
$recordEmptyErrors.Add('Identity rejected before execution.')
$recordEmpty = Get-RecordStatistics @() $false $recordEmptyErrors
$recordEmpty['errors'] = $recordEmptyErrors
$recordEmptyOutput = Join-Path $recordTestDirectory 'empty'
New-Item -ItemType Directory -Path $recordEmptyOutput | Out-Null
Save-RecordEvidence @() $recordEmpty $recordEmptyOutput
Assert-RecordRunner ((Test-Path -LiteralPath (Join-Path $recordEmptyOutput 'manifest.json')) -and (Test-Path -LiteralPath (Join-Path $recordEmptyOutput 'frames.jsonl'))) 'A run rejected before sampling lost its evidence files.'
# Exercise the actual runner's pre-execution rejection without compiling or launching any GPU test.
$recordPwsh = @(Get-Command pwsh -CommandType Application)[0].Source
$recordFailedOutput = Join-Path $recordTestDirectory 'missing-executable'
& $recordPwsh -NoProfile -File (Join-Path $PSScriptRoot 'run_runtime_record.ps1') -Executable (Join-Path $recordTestDirectory 'absent.exe') -OutputDirectory $recordFailedOutput > (Join-Path $recordTestDirectory 'missing-executable.log') 2>&1
Assert-RecordRunner ($LASTEXITCODE -eq 1 -and (Test-Path -LiteralPath (Join-Path $recordFailedOutput 'manifest.json'))) 'The actual runner did not retain a manifest after rejecting a missing executable.'
"Runtime record runner rejection checks passed ($($recordCases.Count) rejected cases, valid/statistical boundaries and actual-runner preflight). Evidence: $recordTestDirectory"
