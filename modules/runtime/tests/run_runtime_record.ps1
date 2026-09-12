#requires -Version 7.2
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateSet('D3D12','Vulkan')][string]$Backend = 'D3D12',
    [switch]$Smoke
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime_profile_runner_support.ps1')
. (Join-Path $PSScriptRoot 'runtime_record_runner_support.ps1')
$recordErrors = [System.Collections.Generic.List[string]]::new()
$recordOutput = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $recordOutput) {
    if (-not (Test-Path -LiteralPath $recordOutput -PathType Container) -or @(Get-ChildItem -LiteralPath $recordOutput -Force).Count -ne 0) {
        Write-Error 'OutputDirectory must be absent or empty; existing evidence is preserved.'
        exit 1
    }
}
New-Item -ItemType Directory -Path $recordOutput -Force | Out-Null
$recordBinary = Get-ProfileFileEvidence $Executable $recordErrors 'Record benchmark executable'
$recordCmake = @(Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue)[0]
if (-not $recordCmake) { $recordErrors.Add('CMake is unavailable for the source identity audit.') }
$recordAudit = Join-Path $PSScriptRoot 'profile_build_identity.cmake'
$recordWarmup = if ($Smoke) { 2 } else { 120 }
$recordSamples = if ($Smoke) { 4 } else { 1000 }
$recordRounds = if ($Smoke) { 1 } else { 5 }
$recordBackendIndex = if ($Backend -eq 'D3D12') { 0 } else { 1 }
$recordFilter = "--gtest_filter=Backends/RuntimeRecordReference.MatchesHandwrittenRhiStateAndPixelsWithAlternatingRecordOrder/$recordBackendIndex"
$recordSavedEnvironment = @{}
foreach ($recordName in @('RADRAY_RECORD_PROFILE','RADRAY_RECORD_IDENTITY_ONLY')) { $recordSavedEnvironment[$recordName] = [Environment]::GetEnvironmentVariable($recordName, 'Process') }
$env:RADRAY_RECORD_PROFILE = if ($Smoke) { '0' } else { '1' }
$env:RADRAY_RECORD_IDENTITY_ONLY = '1'
$recordBuild = $null
$recordOptions = $null
$recordSource = $null
$recordDevice = $null
$recordRows = [System.Collections.Generic.List[object]]::new()
if ($recordErrors.Count -eq 0) {
    & $recordBinary.path $recordFilter *> (Join-Path $recordOutput 'identity.log')
    if ($LASTEXITCODE -ne 0) { $recordErrors.Add('Identity query failed.') }
    $recordIdentityLines = Get-Content -LiteralPath (Join-Path $recordOutput 'identity.log')
    $recordBuilds = @(Read-ProfileRecords $recordIdentityLines 'PROFILE_BUILD ' $recordErrors)
    $recordOptionRows = @(Read-ProfileRecords $recordIdentityLines 'RECORD_OPTIONS ' $recordErrors)
    if ($recordBuilds.Count -ne 1 -or $recordOptionRows.Count -ne 1) { $recordErrors.Add('Identity query must return exactly one build and options row.') }
    else {
        $recordBuild = $recordBuilds[0]
        $recordOptions = $recordOptionRows[0]
        $recordExpected = @{backend=$Backend; draws=1024; warmup=$recordWarmup; samples=$recordSamples; rounds=$recordRounds; width=16; height=16; driverValidation=$false; rgValidation='off'; gpuMarkers=$false; queueWait='per-pair'}
        Test-RecordOptions $recordExpected $recordOptions $recordErrors
        Test-RecordBuildConfiguration $recordBuild $Smoke.IsPresent $recordErrors
        if ($recordErrors.Count -eq 0) {
            $recordSource = Get-ProfileSourceAudit $recordOptions.sourceRoot (Join-Path $recordOutput 'before') $recordCmake.Source $recordAudit $recordErrors
            Test-ProfileSourceMatches $recordBuild $null $recordSource $recordErrors 'Record benchmark before'
        }
    }
}
$env:RADRAY_RECORD_IDENTITY_ONLY = $null
if ($recordErrors.Count -eq 0) {
    & $recordBinary.path $recordFilter *> (Join-Path $recordOutput 'record.log')
    if ($LASTEXITCODE -ne 0) { $recordErrors.Add('Record/state/pixel test failed; inspect record.log.') }
    $recordLines = Get-Content -LiteralPath (Join-Path $recordOutput 'record.log')
    $recordValidation = @(Read-ProfileRecords $recordLines 'RECORD_VALIDATION ' $recordErrors)
    if ($recordValidation.Count -ne 1 -or $recordValidation[0].stateTraceAndPixelsPassed -isnot [bool] -or $recordValidation[0].stateTraceAndPixelsPassed -ne $true -or $recordValidation[0].drawsPerPath -ne 1024 -or $recordValidation[0].traceDrawsPerPath -ne 1024 -or $recordValidation[0].pairedFrames -ne $recordRounds * ($recordWarmup + $recordSamples)) {
        $recordErrors.Add('Missing or failed effective-state and pixel evidence, including a skipped backend.')
    }
    $recordDevices = @(Read-ProfileRecords $recordLines 'RECORD_DEVICE ' $recordErrors)
    if ($recordDevices.Count -eq 1) { $recordDevice = $recordDevices[0] } else { $recordErrors.Add('Selected GPU identity is missing.') }
    $recordRunBuilds = @(Read-ProfileRecords $recordLines 'PROFILE_BUILD ' $recordErrors)
    $recordRunOptions = @(Read-ProfileRecords $recordLines 'RECORD_OPTIONS ' $recordErrors)
    if ($recordRunBuilds.Count -eq 1 -and $recordRunOptions.Count -eq 1) {
        Test-RecordBuildConfiguration $recordRunBuilds[0] $Smoke.IsPresent $recordErrors
        Test-RecordOptions $recordExpected $recordRunOptions[0] $recordErrors
        Compare-ProfileFields $recordBuild $recordRunBuilds[0] $recordBuild.PSObject.Properties.Name $recordErrors 'Record run build'
        Compare-ProfileFields $recordOptions $recordRunOptions[0] $recordOptions.PSObject.Properties.Name $recordErrors 'Record run options'
    } else { $recordErrors.Add('Run identity is missing or duplicated.') }
    foreach ($recordRow in (Read-RecordFrames $recordLines $recordErrors)) { $recordRows.Add($recordRow) }
    Test-RecordFrames $recordRows $recordWarmup $recordSamples $recordRounds $recordErrors
    $recordAfter = Get-ProfileSourceAudit $recordOptions.sourceRoot (Join-Path $recordOutput 'after') $recordCmake.Source $recordAudit $recordErrors
    Test-ProfileSourceMatches $recordBuild $recordSource $recordAfter $recordErrors 'Record benchmark after'
    $recordBinaryAfter = Get-ProfileFileEvidence $recordBinary.path $recordErrors 'Record executable after'
    Compare-ProfileFields $recordBinary $recordBinaryAfter @('sha256') $recordErrors 'Record executable'
}
$recordStatistics = Get-RecordStatistics $recordRows $Smoke.IsPresent $recordErrors
$recordManifest = [ordered]@{
    schema=2; scope='isolated 1024-draw recorder, paired native RHI within one graph; excludes prepare, submit, waits and integrated frame latency';
    executable=$recordBinary; build=$recordBuild; options=$recordOptions; source=$recordSource; device=$recordDevice;
    instrumentation=@{profilerEnabled=$recordBuild.profilerEnabled; detailedProfilingEnabled=$recordBuild.detailedProfilingEnabled;
        detailedProfilingEffective=if ($recordBuild.profilerEnabled -is [bool] -and $recordBuild.detailedProfilingEnabled -is [bool]) { $recordBuild.profilerEnabled -and $recordBuild.detailedProfilingEnabled } else { $null };
        profilerConnection='not observed; both record paths have one matching low-frequency scope'};
    os=[System.Runtime.InteropServices.RuntimeInformation]::OSDescription; cpu=$env:PROCESSOR_IDENTIFIER;
    adapters=@(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Select-Object Name,DriverVersion);
    rows=$recordRows.Count; rounds=$recordStatistics.rounds; runtimeP50Cv=$recordStatistics.runtimeP50Cv; referenceP50Cv=$recordStatistics.referenceP50Cv;
    performanceEvidenceEligible=$recordStatistics.performanceEvidenceEligible; p50Within115Percent=$recordStatistics.p50Within115Percent;
    p95Assessment='Inspect paired raw distributions and each round; no automatic no-regression claim.';
    errors=$recordErrors
}
Save-RecordEvidence $recordRows $recordManifest $recordOutput
foreach ($recordName in $recordSavedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($recordName, $recordSavedEnvironment[$recordName], 'Process') }
Write-Output ("Record evidence: " + (Join-Path $recordOutput 'manifest.json'))
if ($recordErrors.Count -ne 0) { $recordErrors | Write-Output; exit 1 }
