function Read-RecordFrames($Lines, $Errors) {
    foreach ($recordLine in $Lines) {
        if (-not $recordLine.StartsWith('RECORD_FRAME ')) { continue }
        $recordMatch = [regex]::Match($recordLine, '^RECORD_FRAME round=([0-9]+) frame=([0-9]+) warmup=([01]) runtimeFirst=([01]) runtimeNs=([0-9]+) referenceNs=([0-9]+) draws=([0-9]+)$')
        if (-not $recordMatch.Success) { $Errors.Add('Malformed record frame row.'); continue }
        [int]$recordRound = 0
        [int]$recordFrame = 0
        [int]$recordDraws = 0
        [long]$recordRuntime = 0
        [long]$recordReference = 0
        if (-not [int]::TryParse($recordMatch.Groups[1].Value, [ref]$recordRound) -or
            -not [int]::TryParse($recordMatch.Groups[2].Value, [ref]$recordFrame) -or
            -not [long]::TryParse($recordMatch.Groups[5].Value, [ref]$recordRuntime) -or
            -not [long]::TryParse($recordMatch.Groups[6].Value, [ref]$recordReference) -or
            -not [int]::TryParse($recordMatch.Groups[7].Value, [ref]$recordDraws)) {
            $Errors.Add('Record frame integer exceeds its supported range; the original row remains in record.log.')
            continue
        }
        [pscustomobject]@{round=$recordRound; frame=$recordFrame; warmup=$recordMatch.Groups[3].Value -eq '1'; runtimeFirst=$recordMatch.Groups[4].Value -eq '1'; runtimeNs=$recordRuntime; referenceNs=$recordReference; draws=$recordDraws}
    }
}

function Test-RecordFrames($Rows, [int]$Warmup, [int]$Samples, [int]$Rounds, $Errors) {
    if ($Rows.Count -ne $Rounds * ($Warmup + $Samples)) { $Errors.Add('Wrong total paired frame count.') }
    for ($recordIndex = 0; $recordIndex -lt $Rows.Count; ++$recordIndex) {
        $recordRow = $Rows[$recordIndex]
        if ($recordRow.frame -ne $recordIndex -or $recordRow.round -ne [Math]::Floor($recordIndex / ($Warmup + $Samples)) -or
            $recordRow.warmup -ne (($recordIndex % ($Warmup + $Samples)) -lt $Warmup) -or $recordRow.runtimeFirst -ne ($recordIndex % 2 -eq 0) -or
            $recordRow.draws -ne 1024 -or $recordRow.runtimeNs -le 0 -or $recordRow.referenceNs -le 0) {
            $Errors.Add("Invalid, duplicate, or missing frame identity/timing at $recordIndex.")
            break
        }
    }
}

function Test-RecordBuildConfiguration($Build, [bool]$Smoke, $Errors) {
    foreach ($recordField in @('commit','trackedDiffSha256','runtimeTestsSha256','runtimeSourcesSha256','harnessSha256','compiler','flags','configuration')) {
        if ($Build.$recordField -isnot [string] -or [string]::IsNullOrWhiteSpace($Build.$recordField)) { $Errors.Add("Missing or invalid build field: $recordField.") }
    }
    foreach ($recordField in @('trackedDirty','profilerEnabled','detailedProfilingEnabled')) {
        if ($Build.$recordField -isnot [bool]) { $Errors.Add("Build field must be an explicit boolean: $recordField.") }
    }
    if (-not $Smoke -and $Build.configuration -cne 'Release') { $Errors.Add('Performance acceptance requires Release.') }
}

function Test-RecordOptions($Expected, $Observed, $Errors) {
    Compare-ProfileFields $Expected $Observed $Expected.Keys $Errors 'Record fixture options'
    foreach ($recordField in $Expected.Keys) {
        if (($Expected[$recordField] -is [bool] -and $Observed.$recordField -isnot [bool]) -or
            ($Expected[$recordField] -is [int] -and $Observed.$recordField -isnot [int] -and $Observed.$recordField -isnot [long]) -or
            ($Expected[$recordField] -is [string] -and $Observed.$recordField -isnot [string])) { $Errors.Add("Invalid option type: $recordField.") }
    }
    if ($Observed.sourceRoot -isnot [string] -or [string]::IsNullOrWhiteSpace($Observed.sourceRoot)) { $Errors.Add('Source root must be a nonempty string.') }
}

function Get-RecordQuantile($Values, [double]$Percentile) {
    $recordSorted = @($Values | Sort-Object)
    if ($recordSorted.Count -eq 0) { return $null }
    return $recordSorted[[Math]::Max(0, [int][Math]::Ceiling($recordSorted.Count * $Percentile) - 1)]
}

function Get-RecordCv($Values) {
    if ($Values.Count -lt 2) { return $null }
    $recordMean = ($Values | Measure-Object -Average).Average
    if ($recordMean -le 0) { return $null }
    $recordVariance = ($Values | ForEach-Object { ($_ - $recordMean) * ($_ - $recordMean) } | Measure-Object -Average).Average
    return [Math]::Sqrt($recordVariance) / $recordMean
}

function Get-RecordStatistics($Rows, [bool]$Smoke, $Errors) {
    $recordSummaries = @(
        foreach ($recordGroup in ($Rows | Where-Object { -not $_.warmup } | Group-Object round)) {
            $recordSummary = [ordered]@{round=[int]$recordGroup.Name; samples=$recordGroup.Count}
            foreach ($recordPhase in @('runtimeNs','referenceNs')) {
                $recordValues = @($recordGroup.Group | ForEach-Object { $_.$recordPhase })
                $recordSummary[$recordPhase] = [ordered]@{p50=(Get-RecordQuantile $recordValues 0.50); p95=(Get-RecordQuantile $recordValues 0.95); p99=(Get-RecordQuantile $recordValues 0.99)}
            }
            $recordSummary['p50Ratio'] = $null
            $recordSummary['p95Ratio'] = $null
            if ($Errors.Count -eq 0 -and $recordSummary.referenceNs.p50 -gt 0 -and $recordSummary.referenceNs.p95 -gt 0) {
                $recordSummary.p50Ratio = $recordSummary.runtimeNs.p50 / $recordSummary.referenceNs.p50
                $recordSummary.p95Ratio = $recordSummary.runtimeNs.p95 / $recordSummary.referenceNs.p95
            }
            $recordSummary
        }
    )
    $recordRuntimeCv = $null
    $recordReferenceCv = $null
    if ($Errors.Count -eq 0) {
        $recordRuntimeCv = Get-RecordCv @($recordSummaries | ForEach-Object { $_.runtimeNs.p50 })
        $recordReferenceCv = Get-RecordCv @($recordSummaries | ForEach-Object { $_.referenceNs.p50 })
    }
    $recordEligible = -not $Smoke -and $Errors.Count -eq 0 -and $recordSummaries.Count -eq 5 -and $null -ne $recordRuntimeCv -and $null -ne $recordReferenceCv -and $recordRuntimeCv -le 0.05 -and $recordReferenceCv -le 0.05
    return [ordered]@{rounds=$recordSummaries; runtimeP50Cv=$recordRuntimeCv; referenceP50Cv=$recordReferenceCv;
        performanceEvidenceEligible=$recordEligible; p50Within115Percent=($recordEligible -and @($recordSummaries | Where-Object { $null -eq $_.p50Ratio -or $_.p50Ratio -gt 1.15 }).Count -eq 0)}
}

function Save-RecordEvidence($Rows, $Manifest, [string]$OutputDirectory) {
    $recordFrameLines = @($Rows | ForEach-Object { $_ | ConvertTo-Json -Compress })
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'frames.jsonl') -Value $recordFrameLines -Encoding utf8
    $Manifest | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Encoding utf8
}
