function New-ProfileChild([string]$Executable, [string[]]$Arguments, [string]$Stem, $Errors) {
    $ErrorActionPreference = 'Continue'
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { $Errors.Add("Missing child executable: $Executable"); return $null }
    if ((Test-Path -LiteralPath "$Stem.stdout.log") -or (Test-Path -LiteralPath "$Stem.stderr.log")) {
        $Errors.Add("Child logs already exist: $Stem"); return $null
    }
    $profileStart = [Diagnostics.ProcessStartInfo]::new()
    $profileStart.FileName = $Executable
    $profileStart.UseShellExecute = $false
    $profileStart.CreateNoWindow = $true
    $profileStart.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $profileStart.RedirectStandardOutput = $true
    $profileStart.RedirectStandardError = $true
    foreach ($profileArgument in $Arguments) { $profileStart.ArgumentList.Add($profileArgument) }
    $profileProcess = [Diagnostics.Process]::new()
    $profileProcess.StartInfo = $profileStart
    $profileStarted = $profileProcess.Start()
    if (-not $profileStarted) { $Errors.Add("Could not start child: $Executable"); return $null }
    $profileStdout = [IO.File]::Open("$Stem.stdout.log", [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $profileStderr = [IO.File]::Open("$Stem.stderr.log", [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    return [pscustomobject]@{process=$profileProcess; stdout=$profileProcess.StandardOutput.BaseStream.CopyToAsync($profileStdout); stderr=$profileProcess.StandardError.BaseStream.CopyToAsync($profileStderr);
        stdoutFile=$profileStdout; stderrFile=$profileStderr;
        stem=$Stem; executable=$Executable; arguments=$Arguments; pid=$profileProcess.Id; processStartTicks=$profileProcess.StartTime.ToUniversalTime().Ticks; startedUtc=[DateTime]::UtcNow.ToString('o')}
}

function Complete-ProfileChild($Child, [int]$TimeoutSeconds, $Errors, [switch]$Stop) {
    if (-not $Child) { return $null }
    if ($Child.process.Id -ne $Child.pid -or $Child.process.StartTime.ToUniversalTime().Ticks -ne $Child.processStartTicks) {
        $Errors.Add('Owned process identity changed before completion; refusing to wait or stop it.'); return $null
    }
    $profileDeadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (-not $Stop -and -not $Child.process.HasExited -and [DateTime]::UtcNow -lt $profileDeadline) { Start-Sleep -Milliseconds 50 }
    $profileTerminated = -not $Child.process.HasExited
    if ($profileTerminated) {
        $Errors.Add("Owned child did not finish: $($Child.executable), pid $($Child.pid).")
        if ($Child.process.Id -ne $Child.pid -or $Child.process.StartTime.ToUniversalTime().Ticks -ne $Child.processStartTicks) {
            $Errors.Add('Owned process identity changed; refusing to stop it.'); return $null
        }
        # Keep the original process handle; never resolve a recycled PID or terminate unrelated descendants.
        $Child.process.Kill()
    }
    if (-not $Child.process.WaitForExit(10000)) { $Errors.Add('Owned process did not acknowledge termination; its raw log files remain open.'); return $null }
    $Child.stdout.GetAwaiter().GetResult() | Out-Null
    $Child.stderr.GetAwaiter().GetResult() | Out-Null
    $Child.stdoutFile.Dispose()
    $Child.stderrFile.Dispose()
    $profileResult = [ordered]@{executable=$Child.executable; arguments=$Child.arguments; pid=$Child.pid; processStartTicks=$Child.processStartTicks; startedUtc=$Child.startedUtc;
        completedUtc=[DateTime]::UtcNow.ToString('o'); exitCode=$Child.process.ExitCode; terminated=$profileTerminated; identityVerified=$true;
        stdout="$($Child.stem).stdout.log"; stderr="$($Child.stem).stderr.log"}
    $Child.process.Dispose()
    return $profileResult
}

function Test-ProfileOwnedChild($Child) {
    return $Child -and -not $Child.process.HasExited -and $Child.process.Id -eq $Child.pid -and $Child.process.StartTime.ToUniversalTime().Ticks -eq $Child.processStartTicks
}

function Test-ProfileEndpointPair($Client, $Capture, [int]$ClientId, [int]$CaptureId, [int]$Port) {
    return $ClientId -gt 0 -and $CaptureId -gt 0 -and $ClientId -ne $CaptureId -and
        $Client.OwningProcess -eq $ClientId -and $Capture.OwningProcess -eq $CaptureId -and
        $Client.State -eq 'Established' -and $Capture.State -eq 'Established' -and
        $Client.LocalPort -eq $Port -and $Capture.RemotePort -eq $Port -and
        $Client.RemotePort -eq $Capture.LocalPort -and $Client.LocalAddress -eq $Capture.RemoteAddress -and
        $Client.RemoteAddress -eq $Capture.LocalAddress -and $Client.LocalAddress -eq '127.0.0.1' -and $Client.RemoteAddress -eq '127.0.0.1'
}

function Get-ProfileCaptureConnection($Client, $Capture, [int]$Port) {
    if (-not (Test-ProfileOwnedChild $Client) -or -not (Test-ProfileOwnedChild $Capture)) { return $null }
    $profileClients = @(Get-NetTCPConnection -OwningProcess $Client.pid -LocalPort $Port -State Established -ErrorAction SilentlyContinue)
    $profileCaptures = @(Get-NetTCPConnection -OwningProcess $Capture.pid -RemotePort $Port -State Established -ErrorAction SilentlyContinue)
    foreach ($profileClient in $profileClients) {
        foreach ($profileCapture in $profileCaptures) {
            if ((Test-ProfileEndpointPair $profileClient $profileCapture $Client.pid $Capture.pid $Port) -and
                (Test-ProfileOwnedChild $Client) -and (Test-ProfileOwnedChild $Capture)) {
                return [ordered]@{observedUtc=[DateTime]::UtcNow.ToString('o'); port=$Port;
                    client=[ordered]@{pid=$Client.pid; processStartTicks=$Client.processStartTicks; localAddress=$profileClient.LocalAddress; localPort=$profileClient.LocalPort; remoteAddress=$profileClient.RemoteAddress; remotePort=$profileClient.RemotePort; state='Established'};
                    capture=[ordered]@{pid=$Capture.pid; processStartTicks=$Capture.processStartTicks; localAddress=$profileCapture.LocalAddress; localPort=$profileCapture.LocalPort; remoteAddress=$profileCapture.RemoteAddress; remotePort=$profileCapture.RemotePort; state='Established'}}
            }
        }
    }
    return $null
}

function New-ProfileCapturePlan([string]$Stem, [string]$RunId, [string]$CaptureTool, [string]$ExportTool, [int]$Port, $Errors) {
    if ($RunId -cnotmatch '^[a-f0-9]{32}$' -or $Port -lt 1 -or $Port -gt 65535) { $Errors.Add('Invalid capture run identity or TCP port.'); return $null }
    $profilePlan = [ordered]@{runId=$RunId; gate="$Stem.capture-gate"; capture="$Stem.tracy"; zones="$Stem.zones.csv"; messages="$Stem.messages.csv";
        captureTool=$CaptureTool; exportTool=$ExportTool; port=$Port;
        captureArguments=@('-a','127.0.0.1','-p',"$Port",'-o',"$Stem.tracy");
        zoneArguments=@('--unwrap',"$Stem.tracy"); messageArguments=@('--messages',"$Stem.tracy")}
    foreach ($profilePath in @($profilePlan.gate,$profilePlan.capture,$profilePlan.zones,$profilePlan.messages)) {
        if (Test-Path -LiteralPath $profilePath) { $Errors.Add("Capture evidence already exists: $profilePath") }
    }
    return $profilePlan
}

function Invoke-ProfileCapturedRun([string]$Executable, [string]$Filter, [string]$Stem, [string]$RunId,
                                  [string]$CaptureTool, [string]$ExportTool, [int]$TimeoutSeconds, $Errors) {
    $profileErrorCount = $Errors.Count
    $profilePlan = New-ProfileCapturePlan $Stem $RunId $CaptureTool $ExportTool 8086 $Errors
    if (-not $profilePlan -or $Errors.Count -ne $profileErrorCount) { return $null }
    if (-not (Get-Command Get-NetTCPConnection -ErrorAction SilentlyContinue)) { $Errors.Add('Captured runs require Get-NetTCPConnection to match both owned processes.'); return $null }
    $profilePreviousGate = $env:RADRAY_PROFILE_CAPTURE_GATE
    $env:RADRAY_PROFILE_CAPTURE_GATE = $profilePlan.gate
    $profileCapture = New-ProfileChild $CaptureTool $profilePlan.captureArguments "$Stem.capture" $Errors
    $profileClient = if ($profileCapture) { New-ProfileChild $Executable @($Filter) "$Stem.client" $Errors } else { $null }
    $profileConnected = $false
    $profileConnection = $null
    $profileConnectDeadline = [DateTime]::UtcNow.AddSeconds(30)
    if ($profileClient -and $profileCapture) {
        while (-not $profileClient.process.HasExited -and -not $profileCapture.process.HasExited -and [DateTime]::UtcNow -lt $profileConnectDeadline) {
            $profileConnection = Get-ProfileCaptureConnection $profileClient $profileCapture $profilePlan.port
            if ($profileConnection) { $profileConnected = $true; break }
            Start-Sleep -Milliseconds 50
        }
    }
    if ($profileConnected) {
        # File contents bind this one-shot startup permit to the run, not merely to a path.
        $profileGateFile = [IO.File]::Open($profilePlan.gate, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $profilePermitBytes = [Text.Encoding]::UTF8.GetBytes($RunId)
        $profileGateFile.Write($profilePermitBytes)
        $profileGateFile.Dispose()
        $profileConnection['permitWrittenUtc'] = [DateTime]::UtcNow.ToString('o')
    } else { $Errors.Add('Capture and client did not establish a matching owned TCP connection before the startup deadline.') }
    $profileClientResult = Complete-ProfileChild $profileClient $TimeoutSeconds $Errors -Stop:(-not $profileConnected)
    $profileCaptureResult = Complete-ProfileChild $profileCapture 60 $Errors -Stop:(-not $profileConnected)
    $env:RADRAY_PROFILE_CAPTURE_GATE = $profilePreviousGate
    $profileLog = "$Stem.log"
    if ($profileClientResult) {
        Get-Content -LiteralPath @($profileClientResult.stdout, $profileClientResult.stderr) | Set-Content -LiteralPath $profileLog -Encoding utf8
        if ($profileClientResult.exitCode -ne 0) { $Errors.Add("Captured client failed with exit $($profileClientResult.exitCode).") }
    } else { '' | Set-Content -LiteralPath $profileLog -Encoding utf8 }
    if ($profileCaptureResult -and $profileCaptureResult.exitCode -ne 0) { $Errors.Add("Capture failed with exit $($profileCaptureResult.exitCode).") }
    $profileExports = @()
    if ($profileConnected -and $profileClientResult -and $profileCaptureResult -and
        $profileClientResult.exitCode -eq 0 -and $profileCaptureResult.exitCode -eq 0 -and (Test-Path -LiteralPath $profilePlan.capture -PathType Leaf)) {
        $profileZoneArguments = $profilePlan.zoneArguments
        $profileMessageArguments = $profilePlan.messageArguments
        $profileZoneChild = New-ProfileChild $ExportTool $profileZoneArguments "$Stem.zones-export" $Errors
        $profileZoneProcess = Complete-ProfileChild $profileZoneChild $TimeoutSeconds $Errors
        $profileZoneExit = if ($profileZoneProcess) { $profileZoneProcess.exitCode } else { -1 }
        if ($profileZoneProcess) { Copy-Item -LiteralPath $profileZoneProcess.stdout -Destination $profilePlan.zones }
        $profileMessageChild = New-ProfileChild $ExportTool $profileMessageArguments "$Stem.messages-export" $Errors
        $profileMessageProcess = Complete-ProfileChild $profileMessageChild $TimeoutSeconds $Errors
        $profileMessageExit = if ($profileMessageProcess) { $profileMessageProcess.exitCode } else { -1 }
        if ($profileMessageProcess) { Copy-Item -LiteralPath $profileMessageProcess.stdout -Destination $profilePlan.messages }
        if ($profileZoneExit -ne 0 -or $profileMessageExit -ne 0) { $Errors.Add('Inclusive event or message export failed.') }
        $profileExports = @(
            [ordered]@{arguments=$profilePlan.zoneArguments; exitCode=$profileZoneExit; output=$profilePlan.zones; process=$profileZoneProcess},
            [ordered]@{arguments=$profilePlan.messageArguments; exitCode=$profileMessageExit; output=$profilePlan.messages; process=$profileMessageProcess})
    }
    return [ordered]@{runId=$RunId; connectionMatched=$profileConnected; connection=$profileConnection; gate=$profilePlan.gate; client=$profileClientResult; captureProcess=$profileCaptureResult;
        permit=$(if ($profileConnected) { Get-ProfileFileEvidence $profilePlan.gate $Errors 'This run startup permit' } else { $null });
        capture=(Get-ProfileFileEvidence $profilePlan.capture $Errors 'This run Tracy capture');
        zones=(Get-ProfileFileEvidence $profilePlan.zones $Errors 'This run inclusive event export');
        messages=(Get-ProfileFileEvidence $profilePlan.messages $Errors 'This run message export'); exports=$profileExports;
        provenanceVerified=$false; join=$null}
}
