#requires -Version 7.2
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime_profile_runner_support.ps1')
. (Join-Path $PSScriptRoot 'runtime_profile_capture_support.ps1')
function Assert-ProfileRunner([bool]$Condition, [string]$Message) {
    if (-not $Condition) { Write-Error $Message; exit 1 }
}
function New-ProfileRows {
    @(for ($profileIndex = 0; $profileIndex -lt 6; ++$profileIndex) {
        [pscustomobject]@{sampleIndex=$profileIndex; round=0; warmup=($profileIndex -lt 2); frameSerial=($profileIndex + 1); flightIndex=($profileIndex % 2); beginNs=100; recordedNs=200; retireObservedNs=500;
            resolvedViewCount=3; viewCount=3; fullViewCount=2; auxiliaryViewCount=1; outputCount=2; writtenOutputCount=2;
            stageCommandsKnown=$true; actualMeshDraws=7000; depthCommands=1750; opaqueCommands=2625; transparentCommands=375; reportExecutedPassesKnown=$false; reportExecutedPasses=$null}
    })
}
$profileErrors = [System.Collections.Generic.List[string]]::new()
$profileRows = New-ProfileRows
$profileCounts = @(Test-ProfileFrameCounts $profileRows 'integrated' 2 4 $profileErrors)
Assert-ProfileRunner ($profileErrors.Count -eq 0 -and $profileCounts.Count -eq 1 -and $profileCounts[0].steady -eq 4) 'Complete interleaved flight samples were rejected.'
foreach ($profileMutation in @(@('viewCount',1), @('fullViewCount',0), @('outputCount',1), @('writtenOutputCount',1), @('depthCommands',0), @('stageCommandsKnown',$false), @('reportExecutedPasses',0))) {
    $profileRows = New-ProfileRows
    $profileRows[0].($profileMutation[0]) = $profileMutation[1]
    $profileErrors.Clear()
    Test-ProfileFrameCounts $profileRows 'integrated' 2 4 $profileErrors | Out-Null
    Assert-ProfileRunner ($profileErrors.Count -gt 0) 'An incomplete observer-only frame or unknown-as-zero evidence was accepted.'
}
$profileRows = New-ProfileRows
$profileErrors.Clear()
Test-ProfileFrameCounts $profileRows[0..4] 'integrated' 2 4 $profileErrors | Out-Null
Assert-ProfileRunner ($profileErrors.Count -gt 0) 'A truncated successful process log was accepted.'
$profileErrors.Clear()
$profileRows[3].sampleIndex = 2
Test-ProfileFrameCounts $profileRows 'integrated' 2 4 $profileErrors | Out-Null
Assert-ProfileRunner ($profileErrors.Count -gt 0) 'A duplicate sample with a missing index was accepted.'
$profileRows = New-ProfileRows
$profileRows[3].frameSerial = $profileRows[2].frameSerial
$profileErrors.Clear()
Test-ProfileFrameCounts $profileRows 'integrated' 2 4 $profileErrors | Out-Null
Assert-ProfileRunner ($profileErrors.Count -gt 0) 'A reused frame serial was accepted.'
$profileRows = New-ProfileRows
$profileRows[2].warmup = $true
$profileErrors.Clear()
Test-ProfileFrameCounts $profileRows 'integrated' 2 4 $profileErrors | Out-Null
Assert-ProfileRunner ($profileErrors.Count -gt 0) 'A steady row relabeled as warmup was accepted.'
$profileRows = New-ProfileRows
$profileMicro = @(
    foreach ($profileMoving in @($false,$true)) {
        foreach ($profileRow in (New-ProfileRows)) {
            $profileRow | Add-Member -NotePropertyName moving -NotePropertyValue $profileMoving
            if ($profileMoving) { $profileRow.frameSerial += 6 }
            $profileRow
        }
    }
)
$profileErrors.Clear()
$profileCounts = @(Test-ProfileFrameCounts $profileMicro 'micro' 2 4 $profileErrors)
Assert-ProfileRunner ($profileErrors.Count -eq 0 -and $profileCounts.Count -eq 2) 'Independent micro scenarios were combined.'
$profileTimelines = @($profileRows | ForEach-Object {
    [pscustomobject]@{frameSerial=$_.frameSerial; flightIndex=$_.flightIndex; round=0; submissionCallbacksAvailable=$true; submitCallbackNs=250; completionCallbackNs=400; completionSucceeded=$true; inputToSubmitNs=150}
})
$profileErrors.Clear()
Test-ProfileTimelines $profileRows $profileTimelines $profileErrors
Assert-ProfileRunner ($profileErrors.Count -eq 0) 'Valid callback observations were rejected.'
$profileTimelines[3].flightIndex = 0
$profileErrors.Clear()
Test-ProfileTimelines $profileRows $profileTimelines $profileErrors
Assert-ProfileRunner ($profileErrors.Count -gt 0) 'A callback attributed to the wrong flight was accepted.'
$profileTimelines[3].flightIndex = 1
$profileTimelines[0].completionCallbackNs = 200
$profileErrors.Clear()
Test-ProfileTimelines $profileRows $profileTimelines $profileErrors
Assert-ProfileRunner ($profileErrors.Count -gt 0) 'Completion before submission was accepted.'
$profileErrors.Clear()
Compare-ProfileFields @{sha256='old'} @{sha256='new'} @('sha256') $profileErrors 'binary'
Assert-ProfileRunner ($profileErrors.Count -eq 1) 'Changed binary identity was accepted.'
$profileErrors.Clear()
Read-ProfileRecords @('PROFILE_FRAME {bad json}') 'PROFILE_FRAME ' $profileErrors | Out-Null
Assert-ProfileRunner ($profileErrors.Count -eq 1) 'Malformed frame JSON was accepted.'
$profileClientEndpoint = [pscustomobject]@{OwningProcess=71; State='Established'; LocalAddress='127.0.0.1'; RemoteAddress='127.0.0.1'; LocalPort=8086; RemotePort=50500}
$profileCaptureEndpoint = [pscustomobject]@{OwningProcess=72; State='Established'; LocalAddress='127.0.0.1'; RemoteAddress='127.0.0.1'; LocalPort=50500; RemotePort=8086}
Assert-ProfileRunner (Test-ProfileEndpointPair $profileClientEndpoint $profileCaptureEndpoint 71 72 8086) 'The matching owned localhost TCP pair was rejected.'
foreach ($profileMutation in @(
    @{field='OwningProcess'; value=73}, @{field='State'; value='Listen'}, @{field='LocalPort'; value=50501}, @{field='RemotePort'; value=8087}, @{field='LocalAddress'; value='127.0.0.2'}
)) {
    $profileOldValue = $profileCaptureEndpoint.($profileMutation.field)
    $profileCaptureEndpoint.($profileMutation.field) = $profileMutation.value
    Assert-ProfileRunner (-not (Test-ProfileEndpointPair $profileClientEndpoint $profileCaptureEndpoint 71 72 8086)) 'An unrelated or disconnected TCP endpoint was accepted.'
    $profileCaptureEndpoint.($profileMutation.field) = $profileOldValue
}
$profileErrors.Clear()
$profileCapturePlan = New-ProfileCapturePlan (Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N'))) '0123456789abcdef0123456789abcdef' 'capture.exe' 'export.exe' 8086 $profileErrors
Assert-ProfileRunner ($profileErrors.Count -eq 0 -and $profileCapturePlan.zoneArguments[0] -eq '--unwrap' -and $profileCapturePlan.messageArguments[0] -eq '--messages') 'Capture plan did not preserve inclusive unfiltered exports.'
$profileErrors.Clear()
New-ProfileCapturePlan 'unused' 'stale-run' 'capture.exe' 'export.exe' 8086 $profileErrors | Out-Null
Assert-ProfileRunner ($profileErrors.Count -gt 0) 'An invalid capture permit identity was accepted.'
'Runtime profile runner protocol checks passed.'
