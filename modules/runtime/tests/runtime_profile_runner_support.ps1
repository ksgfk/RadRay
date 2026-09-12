function Read-ProfileRecords($Lines, [string]$Prefix, $Errors) {
    foreach ($profileLine in $Lines) {
        if (-not $profileLine.StartsWith($Prefix)) { continue }
        $profileJson = $profileLine.Substring($Prefix.Length)
        if (-not (Test-Json -Json $profileJson -ErrorAction SilentlyContinue)) { $Errors.Add("Malformed $Prefix JSON record."); continue }
        $profileJson | ConvertFrom-Json
    }
}

function Compare-ProfileFields($Expected, $Observed, $Fields, $Errors, [string]$Description) {
    foreach ($profileField in $Fields) {
        if ($null -eq $Expected.$profileField -or $null -eq $Observed.$profileField -or $Expected.$profileField -cne $Observed.$profileField) {
            $Errors.Add("$Description differs or is missing: $profileField.")
        }
    }
}

function Get-ProfileFileEvidence([string]$Path, $Errors, [string]$Description) {
    if (-not $Path) { return $null }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { $Errors.Add("$Description is not an existing file: $Path"); return $null }
    $profileFile = Get-Item -LiteralPath $Path
    if ($profileFile.Length -eq 0) { $Errors.Add("$Description is empty: $Path") }
    return [ordered]@{path=$profileFile.FullName; bytes=$profileFile.Length; sha256=(Get-FileHash -LiteralPath $profileFile.FullName -Algorithm SHA256).Hash}
}

function Get-ProfileSourceAudit([string]$SourceRoot, [string]$OutputStem, [string]$Cmake, [string]$AuditScript, $Errors) {
    $profileAuditPath = "$OutputStem.source.json"
    & $Cmake "-DPROFILE_SOURCE_DIR=$SourceRoot" "-DPROFILE_AUDIT_OUTPUT=$profileAuditPath" -P $AuditScript > "$OutputStem.source.log" 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $profileAuditPath -PathType Leaf)) { $Errors.Add("Source audit failed; inspect $OutputStem.source.log."); return $null }
    $profileIdentity = Get-Content -LiteralPath $profileAuditPath -Raw | ConvertFrom-Json
    $profileShaderRoot = Join-Path $SourceRoot 'shaderlib'
    if (-not (Test-Path -LiteralPath $profileShaderRoot -PathType Container)) { $Errors.Add("Shader source directory is missing: $profileShaderRoot"); return $null }
    $profileShaderFiles = @(Get-ChildItem -LiteralPath $profileShaderRoot -Recurse -File | Where-Object { $_.Extension -in @('.hlsl','.hlsli') } | Sort-Object FullName | ForEach-Object {
        [ordered]@{path=[IO.Path]::GetRelativePath($profileShaderRoot,$_.FullName).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    })
    if ($profileShaderFiles.Count -eq 0) { $Errors.Add('No shader source files were found.') }
    $profileShaderText = ($profileShaderFiles | ForEach-Object { "$($_.path) $($_.sha256)" }) -join "`n"
    $profileShaderDigest = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($profileShaderText)))
    $profileDependency = Get-ProfileFileEvidence (Join-Path $SourceRoot 'project_manifest.json') $Errors 'Dependency manifest'
    $profileTracyTag = $null
    if ($profileDependency) {
        $profileProject = Get-Content -LiteralPath $profileDependency.path -Raw | ConvertFrom-Json
        $profileTracyTags = @($profileProject.ThirdParties | Where-Object Name -eq 'tracy' | ForEach-Object Tag)
        if ($profileTracyTags.Count -eq 1) { $profileTracyTag = $profileTracyTags[0] } else { $Errors.Add('Dependency manifest must contain one Tracy tag.') }
    }
    return [ordered]@{sourceRoot=$SourceRoot; identity=$profileIdentity; shaderSha256=$profileShaderDigest; shaderFiles=$profileShaderFiles; dependencyManifest=$profileDependency; tracyTag=$profileTracyTag}
}

function Test-ProfileSourceMatches($Build, $Expected, $Observed, $Errors, [string]$Description) {
    if (-not $Observed) { return }
    $profileFields = @('commit','trackedDirty','trackedDiffSha256','runtimeTestsSha256','runtimeSourcesSha256','harnessSha256')
    Compare-ProfileFields $Build $Observed.identity $profileFields $Errors "$Description build/source identity"
    if ($Expected) {
        Compare-ProfileFields $Expected.identity $Observed.identity $profileFields $Errors "$Description source stability"
        Compare-ProfileFields $Expected $Observed @('shaderSha256','tracyTag') $Errors "$Description shader/dependency stability"
        Compare-ProfileFields $Expected.dependencyManifest $Observed.dependencyManifest @('sha256') $Errors "$Description dependency manifest"
    }
}

function Test-ProfileFrameCounts($Frames, [string]$Fixture, [int]$Warmup, [int]$Samples, $Errors) {
    $profileExpectedGroups = if ($Fixture -eq 'micro') { 2 } else { 1 }
    $profileGroups = @($Frames | Group-Object moving)
    if ($profileGroups.Count -ne $profileExpectedGroups) { $Errors.Add("Expected $profileExpectedGroups frame scenarios, got $($profileGroups.Count).") }
    if ($Fixture -eq 'micro' -and @($Frames | Where-Object { $_.moving -isnot [bool] }).Count -ne 0) { $Errors.Add('Micro scenario flags must be boolean.') }
    $profileSerials = [System.Collections.Generic.HashSet[UInt64]]::new()
    foreach ($profileGroup in $profileGroups) {
        $profileRows = @($profileGroup.Group | Sort-Object sampleIndex)
        $profileWarmRows = @($profileRows | Where-Object { $_.warmup -eq $true })
        $profileSteadyRows = @($profileRows | Where-Object { $_.warmup -eq $false })
        if ($profileRows.Count -ne $Warmup + $Samples -or $profileWarmRows.Count -ne $Warmup -or $profileSteadyRows.Count -ne $Samples) { $Errors.Add("Scenario '$($profileGroup.Name)' has $($profileWarmRows.Count) warmup / $($profileSteadyRows.Count) steady rows; expected $Warmup / $Samples.") }
        for ($profileIndex = 0; $profileIndex -lt $profileRows.Count; ++$profileIndex) {
            $profileRow = $profileRows[$profileIndex]
            if ($profileRow.sampleIndex -ne $profileIndex -or $profileRow.round -ne 0 -or $profileRow.warmup -isnot [bool] -or $profileRow.warmup -ne ($profileIndex -lt $Warmup)) { $Errors.Add("Scenario '$($profileGroup.Name)' has a duplicate, missing, or misclassified sample index at $profileIndex."); break }
            if (($profileRow.frameSerial -isnot [long] -and $profileRow.frameSerial -isnot [int]) -or $profileRow.frameSerial -le 0 -or -not $profileSerials.Add([UInt64]$profileRow.frameSerial)) { $Errors.Add("Frame serial is missing or duplicated at sample $profileIndex."); break }
            if ($Fixture -eq 'integrated') {
                foreach ($profileExpected in @(@('resolvedViewCount',3), @('viewCount',3), @('fullViewCount',2), @('auxiliaryViewCount',1), @('outputCount',2), @('writtenOutputCount',2))) {
                    if ($profileRow.($profileExpected[0]) -cne $profileExpected[1]) { $Errors.Add("Frame $profileIndex has missing or incomplete actual $($profileExpected[0]).") }
                }
                if ($profileRow.stageCommandsKnown -ne $true -or $profileRow.actualMeshDraws -le 0 -or $profileRow.depthCommands -le 0 -or $profileRow.opaqueCommands -le 0 -or $profileRow.transparentCommands -le 0) {
                    $Errors.Add("Frame $profileIndex lacks known nonzero mesh/depth/opaque/transparent commands.")
                }
                if ($profileRow.reportExecutedPassesKnown -eq $true) {
                    if ($profileRow.reportExecutedPasses -le 0) { $Errors.Add("Frame $profileIndex lacks full report executed-pass evidence.") }
                } elseif ($profileRow.reportExecutedPassesKnown -ne $false -or $null -ne $profileRow.reportExecutedPasses) {
                    $Errors.Add("Frame $profileIndex must preserve unavailable report pass evidence as unknown/null.")
                }
            }
        }
        [ordered]@{scenario=$profileGroup.Name; warmup=$profileWarmRows.Count; steady=$profileSteadyRows.Count; total=$profileRows.Count}
    }
}

function Test-ProfileTimelines($Frames, $Timelines, $Errors) {
    if ($Timelines.Count -ne $Frames.Count) { $Errors.Add('Timeline/frame record counts differ.') }
    $profileBySerial = @{}
    foreach ($profileFrame in $Frames) { $profileBySerial[[string]$profileFrame.frameSerial] = $profileFrame }
    $profileSeen = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($profileTimeline in $Timelines) {
        $profileKey = [string]$profileTimeline.frameSerial
        if (-not $profileSeen.Add($profileKey) -or -not $profileBySerial.ContainsKey($profileKey)) { $Errors.Add('Timeline has a duplicate or foreign frame serial.'); continue }
        $profileFrame = $profileBySerial[$profileKey]
        if ($profileTimeline.flightIndex -ne $profileFrame.flightIndex -or $profileTimeline.round -ne $profileFrame.round) { $Errors.Add('Timeline frame/flight association differs from its frame row.') }
        if ($profileTimeline.submissionCallbacksAvailable -eq $true) {
            if ($null -eq $profileTimeline.submitCallbackNs -or $null -eq $profileTimeline.completionCallbackNs -or $profileTimeline.completionSucceeded -ne $true -or
                $profileTimeline.submitCallbackNs -lt $profileFrame.recordedNs -or $profileTimeline.completionCallbackNs -lt $profileTimeline.submitCallbackNs -or $profileTimeline.completionCallbackNs -gt $profileFrame.retireObservedNs -or
                $profileTimeline.inputToSubmitNs -ne ($profileTimeline.submitCallbackNs - $profileFrame.beginNs)) { $Errors.Add('Timeline has missing, failed, or non-monotonic submission/completion observations.') }
        } elseif ($profileTimeline.submissionCallbacksAvailable -ne $false -or $null -ne $profileTimeline.submitCallbackNs -or $null -ne $profileTimeline.completionCallbackNs -or $null -ne $profileTimeline.inputToSubmitNs) {
            $Errors.Add('Unavailable submission observations must retain explicit null timestamps.')
        }
    }
}

function Get-ProfileTracyTool([string]$Path, [string]$Kind, [string]$ExpectedTag, [string]$LogPath, $Errors) {
    if (-not $Path) { return $null }
    $profileTool = Get-ProfileFileEvidence $Path $Errors "Tracy $Kind tool"
    if (-not $profileTool) { return $null }
    & $profileTool.path --help > $LogPath 2>&1
    $profileHelp = Get-Content -LiteralPath $LogPath -Raw
    $profileMatch = [regex]::Match($profileHelp, "tracy-$Kind\s+(\d+\.\d+\.\d+)")
    $profileVersion = if ($profileMatch.Success) { $profileMatch.Groups[1].Value } else { $null }
    $profileVerified = $profileVersion -and "v$profileVersion" -ceq $ExpectedTag
    if (-not $profileVerified) { $Errors.Add("Tracy $Kind version '$profileVersion' does not match dependency tag '$ExpectedTag'.") }
    return [ordered]@{file=$profileTool; version=$profileVersion; expectedTag=$ExpectedTag; verified=[bool]$profileVerified; helpLog=$LogPath}
}
