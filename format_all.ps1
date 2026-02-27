param(
    [string[]]$Paths = @('benchmarks', 'examples', 'modules', 'tests', 'tools'),
    [string[]]$Extensions = @('.h', '.hh', '.hpp', '.hxx', '.c', '.cc', '.cpp', '.cxx', '.m', '.mm', '.inl'),
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-Info([string]$Message) { Write-Host "[INFO] $Message" -ForegroundColor Cyan }
function Write-Warn([string]$Message) { Write-Host "[WARN] $Message" -ForegroundColor Yellow }
function Write-Err([string]$Message) { Write-Host "[ERR ] $Message" -ForegroundColor Red }

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $scriptRoot

try {
    $clangFormat = Get-Command clang-format -ErrorAction SilentlyContinue
    if (-not $clangFormat) {
        Write-Err '未找到 clang-format，请先安装并加入 PATH。'
        exit 1
    }

    $validPaths = @()
    foreach ($path in $Paths) {
        if (Test-Path -LiteralPath $path -PathType Container) {
            $validPaths += $path
        } else {
            Write-Warn "目录不存在，已跳过: $path"
        }
    }

    if ($validPaths.Count -eq 0) {
        Write-Err '没有可用的格式化目录。'
        exit 1
    }

    $extensionSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($ext in $Extensions) {
        if ([string]::IsNullOrWhiteSpace($ext)) { continue }
        $normalized = if ($ext.StartsWith('.')) { $ext } else { ".$ext" }
        $extensionSet.Add($normalized) | Out-Null
    }

    $targetFiles = @(
        foreach ($path in $validPaths) {
            Get-ChildItem -LiteralPath $path -File -Recurse | Where-Object {
                $ext = [System.IO.Path]::GetExtension($_.FullName)
                $extensionSet.Contains($ext)
            } | ForEach-Object {
                $_.FullName
            }
        }
    )

    if ($targetFiles.Count -eq 0) {
        Write-Warn '没有找到匹配扩展名的已跟踪文件。'
        exit 0
    }

    Write-Info ("将处理 {0} 个文件（目录: {1}）" -f $targetFiles.Count, ($validPaths -join ', '))

    if ($DryRun) {
        Write-Info 'DryRun 模式：仅展示将被格式化的文件，不执行 clang-format。'
        $targetFiles | ForEach-Object { Write-Host $_ }
        exit 0
    }

    $formattedCount = 0
    foreach ($file in $targetFiles) {
        & clang-format -i --style=file -- "$file"
        if ($LASTEXITCODE -ne 0) {
            Write-Err "格式化失败: $file"
            exit 1
        }
        $formattedCount++
    }

    Write-Host ("格式化完成，共处理 {0} 个文件。" -f $formattedCount) -ForegroundColor Green
} finally {
    Pop-Location
}
