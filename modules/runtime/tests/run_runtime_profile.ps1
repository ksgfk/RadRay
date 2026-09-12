#requires -Version 7.2
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [string]$BaselineExecutable = '',
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateSet('D3D12', 'Vulkan')][string]$Backend = 'D3D12',
    [ValidateSet('micro', 'integrated')][string]$Fixture = 'integrated',
    [ValidateSet('off', 'full')][string]$Validation = 'off',
    [ValidateSet('minimal', 'counters', 'full')][string]$Report = 'minimal',
    [int]$Primitives = 1000, [int]$Warmup = 120, [int]$Samples = 1000, [int]$Rounds = 5,
    [switch]$GpuMarkers, [switch]$DriverValidation, [switch]$SerializeReport, [switch]$LowChange,
    [string]$TracyCapture = '', [string]$CpuSampling = '', [string]$TracyExportArguments = '',
    [string]$TracyCaptureTool = '', [string]$TracyExportTool = '', [string[]]$TracyExports = @(),
    [switch]$CaptureEachRun, [string]$PythonExecutable = 'python', [int]$RunTimeoutSeconds = 180,
    [string]$CandidateCoverageAudit = '', [string]$BaselineCoverageAudit = ''
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'runtime_profile_runner_support.ps1')
. (Join-Path $PSScriptRoot 'runtime_profile_capture_support.ps1')
if ($Warmup -lt 1 -or $Samples -lt 1 -or $Rounds -lt 1 -or $Primitives -lt 1) { Write-Error 'Counts must be positive.'; exit 1 }
if ($SerializeReport -and $Report -ne 'full') { Write-Error 'Serialization requires a full report.'; exit 1 }
if ($CaptureEachRun -and ($Fixture -ne 'integrated' -or $TracyCapture -or $TracyExports.Count -ne 0 -or -not $TracyCaptureTool -or -not $TracyExportTool -or $RunTimeoutSeconds -lt 1)) { Write-Error 'CaptureEachRun requires the integrated fixture, both Tracy tools, a positive timeout, and no preexisting capture/exports.'; exit 1 }
$profileExecutables = [ordered]@{candidate=(Resolve-Path -LiteralPath $Executable).Path}
if ($BaselineExecutable) { $profileExecutables['baseline'] = (Resolve-Path -LiteralPath $BaselineExecutable).Path }
$profileOutput = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if ((Test-Path -LiteralPath $profileOutput) -and @(Get-ChildItem -LiteralPath $profileOutput -Force).Count -ne 0) { Write-Error 'OutputDirectory must be empty so existing measurements are preserved.'; exit 1 }
New-Item -ItemType Directory -Path $profileOutput -Force | Out-Null
$profileCmake = @(Get-Command cmake -CommandType Application)[0].Source
$profilePython = if ($CaptureEachRun) { @(Get-Command $PythonExecutable -CommandType Application)[0].Source } else { $null }
$profileAuditScript = Join-Path $PSScriptRoot 'profile_build_identity.cmake'
$profileErrors = [System.Collections.Generic.List[string]]::new()
$profileRuns = [System.Collections.Generic.List[object]]::new()
$profileInitial = [ordered]@{}
$profileEnvironmentNames = @('RADRAY_RUNTIME_PROFILE','RADRAY_PROFILE_WARMUP','RADRAY_PROFILE_SAMPLES','RADRAY_PROFILE_ROUNDS','RADRAY_PROFILE_PRIMITIVES','RADRAY_PROFILE_VALIDATION','RADRAY_PROFILE_REPORT','RADRAY_PROFILE_GPU_MARKERS','RADRAY_PROFILE_DRIVER_VALIDATION','RADRAY_PROFILE_SERIALIZE','RADRAY_PROFILE_LOW_CHANGE','RADRAY_PROFILE_IDENTITY_ONLY','RADRAY_PROFILE_RUN_ID','RADRAY_PROFILE_CAPTURE_GATE')
$profilePreviousEnvironment = @{}
foreach ($profileName in $profileEnvironmentNames) { $profilePreviousEnvironment[$profileName] = [Environment]::GetEnvironmentVariable($profileName) }
$env:RADRAY_RUNTIME_PROFILE = '1'
$env:RADRAY_PROFILE_CAPTURE_GATE = $null
$env:RADRAY_PROFILE_WARMUP = "$Warmup"
$env:RADRAY_PROFILE_SAMPLES = "$Samples"
$env:RADRAY_PROFILE_ROUNDS = '1'
$env:RADRAY_PROFILE_PRIMITIVES = "$Primitives"
$env:RADRAY_PROFILE_VALIDATION = $Validation
$env:RADRAY_PROFILE_REPORT = $Report
$env:RADRAY_PROFILE_GPU_MARKERS = $(if ($GpuMarkers) { '1' } else { $null })
$env:RADRAY_PROFILE_DRIVER_VALIDATION = $(if ($DriverValidation) { '1' } else { $null })
$env:RADRAY_PROFILE_SERIALIZE = $(if ($SerializeReport) { '1' } else { $null })
$env:RADRAY_PROFILE_LOW_CHANGE = $(if ($LowChange) { '1' } else { $null })
$profileTestName = if ($Fixture -eq 'micro') { 'StageCostsAndWarmResourceCounts' } else { 'ThreeViewForwardSteadyState' }
$profileBackendIndex = if ($Backend -eq 'D3D12') { '0' } else { '1' }
$profileFilter = "--gtest_filter=Backends/RuntimeProfile.$profileTestName/$profileBackendIndex"
$profileMetadataPrefixes = [ordered]@{build='PROFILE_BUILD '; options='PROFILE_OPTIONS '; instrumentation='PROFILE_INSTRUMENTATION '; assets='PROFILE_ASSETS '; phaseContract='PROFILE_PHASE_CONTRACT '}
$env:RADRAY_PROFILE_IDENTITY_ONLY = '1'
foreach ($profileLabel in $profileExecutables.Keys) {
    $profileStem = Join-Path $profileOutput "$profileLabel-identity"
    $profileBinary = Get-ProfileFileEvidence $profileExecutables[$profileLabel] $profileErrors "$profileLabel executable"
    & $profileBinary.path $profileFilter > "$profileStem.log" 2>&1
    if ($LASTEXITCODE -ne 0) { $profileErrors.Add("$profileLabel identity-only run failed.") }
    $profileLines = Get-Content -LiteralPath "$profileStem.log"
    $profileMeta = [ordered]@{}
    foreach ($profileKey in $profileMetadataPrefixes.Keys) {
        $profileRecords = @(Read-ProfileRecords $profileLines $profileMetadataPrefixes[$profileKey] $profileErrors)
        if ($profileRecords.Count -ne 1) { $profileErrors.Add("$profileLabel must emit exactly one $profileKey identity record; rebuild with this harness.") }
        $profileMeta[$profileKey] = if ($profileRecords.Count -eq 1) { $profileRecords[0] } else { $null }
    }
    if (@($profileLines | Where-Object { $_.StartsWith('PROFILE_FRAME ') }).Count -ne 0) { $profileErrors.Add("$profileLabel ignored identity-only mode; rebuild with this harness.") }
    if ($profileErrors.Count -ne 0) { break }
    if ($CaptureEachRun -and $profileMeta.build.profilerEnabled -ne $true) { $profileErrors.Add("$profileLabel must enable the CPU profiler for capture.") }
    $profileSource = Get-ProfileSourceAudit $profileMeta.assets.sourceRoot $profileStem $profileCmake $profileAuditScript $profileErrors
    Test-ProfileSourceMatches $profileMeta.build $null $profileSource $profileErrors $profileLabel
    $profileBinaryAfter = Get-ProfileFileEvidence $profileBinary.path $profileErrors "$profileLabel executable after identity query"
    Compare-ProfileFields $profileBinary $profileBinaryAfter @('sha256') $profileErrors "$profileLabel executable"
    $profileExpectedOptions = @{warmup=$Warmup; samples=$Samples; rounds=1; primitives=$Primitives; backend=$Backend; rgValidation=$Validation; rgReport=$Report; gpuMarkers=[bool]$GpuMarkers; driverValidation=[bool]$DriverValidation; serializeReport=[bool]$SerializeReport}
    Compare-ProfileFields $profileExpectedOptions $profileMeta.options $profileExpectedOptions.Keys $profileErrors "$profileLabel effective options"
    if ($profileSource) { $profileInitial[$profileLabel] = [ordered]@{binary=$profileBinary; metadata=$profileMeta; source=$profileSource} }
}
$env:RADRAY_PROFILE_IDENTITY_ONLY = $null
if ($profileInitial.Count -eq 2) {
    Compare-ProfileFields $profileInitial.candidate.metadata.build $profileInitial.baseline.metadata.build @('harnessSha256','compiler','flags','configuration','profilerEnabled','detailedProfilingEnabled') $profileErrors 'Paired build configuration/harness'
    Compare-ProfileFields $profileInitial.candidate.metadata.options $profileInitial.baseline.metadata.options $profileInitial.candidate.metadata.options.PSObject.Properties.Name $profileErrors 'Paired fixture options'
    Compare-ProfileFields $profileInitial.candidate.metadata.instrumentation $profileInitial.baseline.metadata.instrumentation @('detailedProfiling','frameAndPhaseProfiling') $profileErrors 'Paired instrumentation'
    Compare-ProfileFields $profileInitial.candidate.source $profileInitial.baseline.source @('shaderSha256','tracyTag') $profileErrors 'Paired shader/dependency versions'
}
$profileEvidence = [ordered]@{capture=(Get-ProfileFileEvidence $TracyCapture $profileErrors 'Tracy capture'); cpuSampling=(Get-ProfileFileEvidence $CpuSampling $profileErrors 'CPU sampling'); exports=@()}
foreach ($profileExport in $TracyExports) { $profileEvidence.exports += Get-ProfileFileEvidence $profileExport $profileErrors 'Tracy export' }
if (($TracyCapture -or $TracyExports.Count -ne 0) -and (-not $TracyCaptureTool -or -not $TracyExportTool)) { $profileErrors.Add('Referenced Tracy evidence requires -TracyCaptureTool and -TracyExportTool so their versions can be checked.') }
$profileExpectedTracy = if ($profileInitial.Contains('candidate')) { $profileInitial.candidate.source.tracyTag } else { $null }
$profileEvidence['captureTool'] = Get-ProfileTracyTool $TracyCaptureTool 'capture' $profileExpectedTracy (Join-Path $profileOutput 'tracy-capture-help.log') $profileErrors
$profileEvidence['exportTool'] = Get-ProfileTracyTool $TracyExportTool 'csvexport' $profileExpectedTracy (Join-Path $profileOutput 'tracy-export-help.log') $profileErrors
$profileCoverage = [ordered]@{candidate=(Get-ProfileFileEvidence $CandidateCoverageAudit $profileErrors 'Candidate coverage audit'); baseline=(Get-ProfileFileEvidence $BaselineCoverageAudit $profileErrors 'Baseline coverage audit')}
for ($profileRound = 0; $profileRound -lt $Rounds -and $profileErrors.Count -eq 0; ++$profileRound) {
    $profileOrder = @($profileExecutables.Keys)
    if ($profileRound % 2 -eq 0) { [array]::Reverse($profileOrder) }
    foreach ($profileLabel in $profileOrder) {
        $profileInitialRun = $profileInitial[$profileLabel]
        $profileStem = Join-Path $profileOutput "$profileLabel-round-$profileRound"
        $profileBefore = Get-ProfileSourceAudit $profileInitialRun.source.sourceRoot "$profileStem-before" $profileCmake $profileAuditScript $profileErrors
        Test-ProfileSourceMatches $profileInitialRun.metadata.build $profileInitialRun.source $profileBefore $profileErrors "$profileLabel round $profileRound before"
        $profileBinaryBefore = Get-ProfileFileEvidence $profileExecutables[$profileLabel] $profileErrors "$profileLabel executable before round"
        Compare-ProfileFields $profileInitialRun.binary $profileBinaryBefore @('sha256') $profileErrors "$profileLabel executable before round"
        if ($profileErrors.Count -ne 0) { break }
        $profileBegin = [DateTime]::UtcNow
        $profileRunId = [Guid]::NewGuid().ToString('N')
        $env:RADRAY_PROFILE_RUN_ID = $profileRunId
        $profileTrace = $null
        if ($CaptureEachRun) {
            $profileTrace = Invoke-ProfileCapturedRun $profileBinaryBefore.path $profileFilter $profileStem $profileRunId $profileEvidence.captureTool.file.path $profileEvidence.exportTool.file.path $RunTimeoutSeconds $profileErrors
            $profileExit = if ($profileTrace -and $profileTrace.client) { $profileTrace.client.exitCode } else { -1 }
        } else {
            & $profileBinaryBefore.path $profileFilter > "$profileStem.log" 2>&1
            $profileExit = $LASTEXITCODE
        }
        $profileEnd = [DateTime]::UtcNow
        $profileLines = @(Get-Content -LiteralPath "$profileStem.log" -ErrorAction SilentlyContinue)
        $profileRunRecords = @(Read-ProfileRecords $profileLines 'PROFILE_RUN ' $profileErrors)
        if ($profileRunRecords.Count -ne 1 -or $profileRunRecords[0].id -cne $profileRunId) { $profileErrors.Add('Run identity is missing or differs from this invocation.') }
        if ($profileExit -ne 0) { $profileErrors.Add("$profileLabel round $profileRound exited with $profileExit.") }
        $profileMeta = [ordered]@{}
        foreach ($profileKey in $profileMetadataPrefixes.Keys) {
            $profileRecords = @(Read-ProfileRecords $profileLines $profileMetadataPrefixes[$profileKey] $profileErrors)
            if ($profileRecords.Count -ne 1) { $profileErrors.Add("$profileLabel round $profileRound has an invalid $profileKey record count.") }
            $profileMeta[$profileKey] = if ($profileRecords.Count -eq 1) { $profileRecords[0] } else { $null }
            Compare-ProfileFields $profileInitialRun.metadata[$profileKey] $profileMeta[$profileKey] $profileInitialRun.metadata[$profileKey].PSObject.Properties.Name $profileErrors "$profileLabel round $profileRound $profileKey"
        }
        $profileFrames = @(Read-ProfileRecords $profileLines 'PROFILE_FRAME ' $profileErrors)
        $profileCounts = @(Test-ProfileFrameCounts $profileFrames $Fixture $Warmup $Samples $profileErrors)
        if ($Fixture -eq 'integrated') {
            $profileTimelines = @(Read-ProfileRecords $profileLines 'PROFILE_TIMELINE ' $profileErrors)
            Test-ProfileTimelines $profileFrames $profileTimelines $profileErrors
        }
        $profileSidecars = [ordered]@{frames='PROFILE_FRAME '; calls='PROFILE_COMMAND_CALLS '; publication='PROFILE_PUBLICATION '; resources='PROFILE_RESOURCES '; timeline='PROFILE_TIMELINE '; serialization='PROFILE_REPORT_SERIALIZATION '}
        $profileSidecarEvidence = [ordered]@{}
        foreach ($profileSidecar in $profileSidecars.Keys) {
            $profilePrefix = $profileSidecars[$profileSidecar]
            @($profileLines | Where-Object { $_.StartsWith($profilePrefix) } | ForEach-Object { $_.Substring($profilePrefix.Length) }) | Set-Content -LiteralPath "$profileStem.$profileSidecar.jsonl" -Encoding utf8
            if ($profileSidecar -in @('frames','timeline')) { $profileSidecarEvidence[$profileSidecar] = Get-ProfileFileEvidence "$profileStem.$profileSidecar.jsonl" $profileErrors "Run $profileSidecar sidecar" }
        }
        $profileSummary = @(Read-ProfileRecords $profileLines 'PROFILE ' $profileErrors)
        if ($profileSummary.Count -eq 0 -or @($profileSummary | Where-Object { $_.samples -ne $Samples }).Count -ne 0) { $profileErrors.Add("$profileLabel round $profileRound has missing or incomplete steady summaries.") }
        $profileAfter = Get-ProfileSourceAudit $profileInitialRun.source.sourceRoot "$profileStem-after" $profileCmake $profileAuditScript $profileErrors
        Test-ProfileSourceMatches $profileInitialRun.metadata.build $profileInitialRun.source $profileAfter $profileErrors "$profileLabel round $profileRound after"
        $profileBinaryAfter = Get-ProfileFileEvidence $profileExecutables[$profileLabel] $profileErrors "$profileLabel executable after round"
        Compare-ProfileFields $profileInitialRun.binary $profileBinaryAfter @('sha256') $profileErrors "$profileLabel executable after round"
        $profileRuns.Add([ordered]@{label=$profileLabel; round=$profileRound; runId=$profileRunId; binary=$profileBinaryBefore; startedUtc=$profileBegin.ToString('o'); completedUtc=$profileEnd.ToString('o'); exitCode=$profileExit; metadata=$profileMeta; sourceBefore=$profileBefore; sourceAfter=$profileAfter; counts=$profileCounts; summary=$profileSummary; log="$profileStem.log"; sidecars=$profileSidecarEvidence; trace=$profileTrace})
        if ($profileErrors.Count -ne 0) { break }
    }
}
foreach ($profileName in $profileEnvironmentNames) { [Environment]::SetEnvironmentVariable($profileName, $profilePreviousEnvironment[$profileName]) }
$profileFinal = [ordered]@{}
foreach ($profileLabel in $profileInitial.Keys) {
    $profileFinalBinary = Get-ProfileFileEvidence $profileExecutables[$profileLabel] $profileErrors "$profileLabel final executable"
    $profileFinalSource = Get-ProfileSourceAudit $profileInitial[$profileLabel].source.sourceRoot (Join-Path $profileOutput "$profileLabel-final") $profileCmake $profileAuditScript $profileErrors
    Compare-ProfileFields $profileInitial[$profileLabel].binary $profileFinalBinary @('sha256') $profileErrors "$profileLabel final executable"
    Test-ProfileSourceMatches $profileInitial[$profileLabel].metadata.build $profileInitial[$profileLabel].source $profileFinalSource $profileErrors "$profileLabel final"
    $profileFinal[$profileLabel] = [ordered]@{binary=$profileFinalBinary; source=$profileFinalSource}
}
foreach ($profileArtifact in @($profileEvidence.capture, $profileEvidence.cpuSampling, $profileCoverage.candidate, $profileCoverage.baseline) + $profileEvidence.exports) {
    if ($profileArtifact) {
        $profileArtifactAfter = Get-ProfileFileEvidence $profileArtifact.path $profileErrors 'Evidence after sampling'
        Compare-ProfileFields $profileArtifact $profileArtifactAfter @('sha256') $profileErrors 'External evidence stability'
    }
}
foreach ($profileTool in @($profileEvidence.captureTool, $profileEvidence.exportTool)) {
    if ($profileTool) {
        $profileToolAfter = Get-ProfileFileEvidence $profileTool.file.path $profileErrors 'Tracy tool after sampling'
        Compare-ProfileFields $profileTool.file $profileToolAfter @('sha256') $profileErrors 'Tracy tool stability'
    }
}
$profileStability = [System.Collections.Generic.List[object]]::new()
foreach ($profileLabel in $profileExecutables.Keys) {
    $profileStageSamples = @($profileRuns | Where-Object { $_.label -eq $profileLabel } | ForEach-Object { $_.summary })
    foreach ($profileGroup in ($profileStageSamples | Group-Object stage,moving)) {
        $profileMedianValues = @($profileGroup.Group | ForEach-Object { [double]$_.p50Ms })
        $profileMean = ($profileMedianValues | Measure-Object -Average).Average
        $profileVariance = 0.0
        foreach ($profileValue in $profileMedianValues) { $profileVariance += [Math]::Pow($profileValue - $profileMean, 2) }
        $profileCv = if ($profileMean -eq 0) { 0.0 } else { [Math]::Sqrt($profileVariance / $profileMedianValues.Count) / $profileMean }
        $profileStability.Add([ordered]@{label=$profileLabel; stage=$profileGroup.Group[0].stage; moving=$profileGroup.Group[0].moving; p50Rounds=$profileMedianValues; p50Cv=$profileCv; stable=($profileMedianValues.Count -ge 5 -and $profileCv -le .05)})
    }
}
$profileCompleted = $profileErrors.Count -eq 0 -and $profileRuns.Count -eq $Rounds * $profileExecutables.Count
$profileManifest = [ordered]@{
    schemaVersion=4; fixture=$Fixture; scenario=$(if ($Fixture -eq 'micro') { 'static-and-all-transforms' } elseif ($LowChange) { 'low-change-1pct-transforms' } else { 'static' }); backend=$Backend
    procedure=[ordered]@{warmup=$Warmup; samples=$Samples; rounds=$Rounds; alternated=($profileExecutables.Count -eq 2); protocolConforming=($profileCompleted -and $Warmup -ge 120 -and $Samples -eq 1000 -and $Rounds -eq 5)}
    platform=[Environment]::OSVersion.VersionString; cpu=@(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors); gpu=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate)
    tracy=[ordered]@{mode=$(if ($CaptureEachRun) { 'owned-per-run-capture' } else { 'external-attachments-unverified' }); expectedTag=$profileExpectedTracy; evidence=$profileEvidence; exportArguments=$TracyExportArguments; sourceKey=@('name','src_file','src_line'); scopeMode='inclusive; self requires a separate source-keyed export'; captureProvenanceVerified=$false; cpuSamplingContentsVerified=$false; coverageAudits=$profileCoverage}
    preparationTotalNs=$null; gtRenderPreparationNs=$null; drawWorkBuildNs=$null; workerActiveNs=$null; totalComplete=$false; steadyTotalComplete=$false
    initial=$profileInitial; final=$profileFinal; runs=$profileRuns; stability=$profileStability; completed=$profileCompleted; errors=$profileErrors
}
$profileManifest | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath (Join-Path $profileOutput 'manifest.json') -Encoding utf8
if ($CaptureEachRun) {
    # The parser hashes this immutable sampling snapshot. Later join results update only manifest.json.
    $profileSnapshot = Join-Path $profileOutput 'sampling-manifest.json'
    $profileManifest | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $profileSnapshot -Encoding utf8
    foreach ($profileRun in $profileRuns) {
        if (-not $profileRun.trace -or -not $profileRun.trace.zones -or -not $profileRun.trace.messages) { continue }
        $profileJoinDirectory = Join-Path $profileOutput "joined/$($profileRun.label)/round-$($profileRun.round)"
        $profileJoinArguments = @((Join-Path $PSScriptRoot 'runtime_profile_trace.py'),'--manifest',$profileSnapshot,'--label',$profileRun.label,'--round',"$($profileRun.round)",'--zones',$profileRun.trace.zones.path,'--messages',$profileRun.trace.messages.path,'--output-directory',$profileJoinDirectory)
        if ($profileCoverage[$profileRun.label]) { $profileJoinArguments += @('--coverage-audit',$profileCoverage[$profileRun.label].path) }
        & $profilePython @profileJoinArguments > "$($profileRun.log).join.log" 2>&1
        $profileJoinExit = $LASTEXITCODE
        $profileJoinPath = Join-Path $profileJoinDirectory 'summary.json'
        $profileRun.trace.join = Get-ProfileFileEvidence $profileJoinPath $profileErrors 'This run trace join summary'
        if ($profileJoinExit -ne 0) { $profileErrors.Add("Trace join failed for $($profileRun.label), round $($profileRun.round).") }
        if ($profileRun.trace.join) {
            $profileJoinedText = Get-Content -LiteralPath $profileJoinPath -Raw
            if (Test-Json $profileJoinedText -ErrorAction SilentlyContinue) {
                $profileJoined = $profileJoinedText | ConvertFrom-Json
                $profileRun.trace.provenanceVerified = $profileJoined.captureProvenanceVerified -eq $true -and $profileJoinExit -eq 0
                $profileRun.trace['totalComplete'] = $profileJoined.totalComplete -eq $true -and $profileJoinExit -eq 0
                $profileRun.trace['steadyTotalComplete'] = $profileJoined.steadyTotalComplete -eq $true -and $profileJoinExit -eq 0
                $profileRun.trace['phaseCoverage'] = $profileJoined.phaseCoverage
            } else { $profileErrors.Add('Trace join summary is malformed.') }
        }
    }
    $profileManifest.tracy.captureProvenanceVerified = $profileRuns.Count -eq $Rounds * $profileExecutables.Count -and @($profileRuns | Where-Object { -not $_.trace.provenanceVerified }).Count -eq 0
    $profileManifest.totalComplete = $profileManifest.tracy.captureProvenanceVerified -and @($profileRuns | Where-Object { -not $_.trace.totalComplete }).Count -eq 0
    $profileManifest.steadyTotalComplete = $profileManifest.tracy.captureProvenanceVerified -and @($profileRuns | Where-Object { -not $_.trace.steadyTotalComplete }).Count -eq 0
    if ($profileExecutables.Count -eq 2) {
        $profileComparison = Join-Path $profileOutput 'preparation-comparison.json'
        & $profilePython (Join-Path $PSScriptRoot 'runtime_profile_compare.py') --baseline-directory (Join-Path $profileOutput 'joined/baseline') --candidate-directory (Join-Path $profileOutput 'joined/candidate') --output $profileComparison > (Join-Path $profileOutput 'comparison.log') 2>&1
        if ($LASTEXITCODE -ne 0) { $profileErrors.Add('Preparation comparison failed its evidence checks.') }
        $profileManifest['comparison'] = Get-ProfileFileEvidence $profileComparison $profileErrors 'Preparation comparison'
    }
    $profileCompleted = $profileCompleted -and $profileErrors.Count -eq 0
    $profileManifest.completed = $profileCompleted
    $profileManifest.procedure.protocolConforming = $profileManifest.procedure.protocolConforming -and $profileCompleted
    $profileManifest | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath (Join-Path $profileOutput 'manifest.json') -Encoding utf8
}
if (-not $profileCompleted) { Write-Error 'Profile evidence checks failed; inspect manifest.json errors and preserved logs.'; exit 1 }
