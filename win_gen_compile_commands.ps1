param(
	[Parameter(Mandatory = $true)] [string]$BuildDir,
	[string]$Output = '.vscode/compile_commands.json',
	[string]$Configuration = 'Debug',
	[string]$Solution
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Write-Info([string]$msg){ Write-Host "[INFO] $msg" -ForegroundColor Cyan }
function Write-Warn([string]$msg){ Write-Host "[WARN] $msg" -ForegroundColor Yellow }
function Write-Err ([string]$msg){ Write-Host "[ERR ] $msg" -ForegroundColor Red }
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
try {
	chcp 65001 > $null 2>&1
} catch {}
try {
	[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
} catch {}
$env:MSBUILDDISABLENODEREUSE = '1'
$BuildDir = Resolve-Path -LiteralPath $BuildDir -ErrorAction Stop | Select-Object -ExpandProperty Path
if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) { Write-Err "BuildDir 不存在: $BuildDir"; exit 1 }
$OutputFull = if ([System.IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path (Get-Location) $Output }
$OutputDir = Split-Path -Parent $OutputFull
if (-not (Test-Path -LiteralPath $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir | Out-Null }
$loggerProjDir = Join-Path $ScriptRoot 'cmake/MsBuildCompileCommandsJson'
if (-not (Test-Path -LiteralPath $loggerProjDir)) { Write-Err "未找到 logger 工程目录: $loggerProjDir"; exit 1 }
Write-Info "构建 Logger 工程 (Release)"
dotnet build "$loggerProjDir" -c Release --nologo | Write-Host
$loggerDll = Get-ChildItem -Path (Join-Path $loggerProjDir 'bin/Release') -Recurse -Filter 'CompileCommandsJson.dll' | Select-Object -First 1
if (-not $loggerDll) { Write-Err '未找到 CompileCommandsJson.dll'; exit 1 }
Write-Info "Logger DLL: $($loggerDll.FullName)"
if ($Solution) {
	if (-not (Test-Path -LiteralPath $Solution)) { Write-Err "指定的 Solution 不存在: $Solution"; exit 1 }
	$slnPath = Resolve-Path -LiteralPath $Solution | Select-Object -ExpandProperty Path
} else {
	$slnCandidates = @(Get-ChildItem -LiteralPath $BuildDir -Filter '*.slnx' -File | Sort-Object FullName)
	if ($slnCandidates.Count -eq 0) { Write-Err "在 $BuildDir 顶层未找到 .slnx"; exit 1 }
	if ($slnCandidates.Count -gt 1) {
		Write-Warn "顶层找到多个 .sln，使用第一个: $($slnCandidates[0].FullName)"
	}
	$slnPath = $slnCandidates[0].FullName
}
Write-Info "使用解决方案: $slnPath"
function Get-MsBuild {
	$vswhereList = @(
		"$Env:ProgramFiles (x86)\Microsoft Visual Studio\Installer\vswhere.exe",
		"$Env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe"
	) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
	foreach ($vw in $vswhereList) {
		$found = (& $vw -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe 2>$null | Select-Object -First 1)
        Write-Warn "vswhere 查找 MSBuild: $found"
		if ($found) { return ($found -as [string]).Trim() }
	}
	return $null
}
$msbuildExe = Get-MsBuild
if (-not $msbuildExe) { Write-Err '未找到 msbuild.exe，请安装 VS Build Tools / VS 或用 -MsBuild 指定完整路径。'; exit 1 }
Write-Info "MSBuild: $msbuildExe"
$loggerArg = "/logger:$($loggerDll.FullName);$OutputFull"
$commonArgs = @("$slnPath", '/t:Rebuild', "/p:Configuration=$Configuration", '/m', $loggerArg)
Write-Info "开始 Rebuild (生成 compile_commands.json)" 
& $msbuildExe @commonArgs
if (-not (Test-Path -LiteralPath $OutputFull)) {
	Write-Err "Rebuild 结束但未生成: $OutputFull"; exit 1
}
Write-Host "生成成功: $OutputFull" -ForegroundColor Green
