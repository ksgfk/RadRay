$ErrorActionPreference = "Stop"

function Fetch-SDK-Artifact {
    param (
        [Parameter(Mandatory=$true)]
        [hashtable]$ArtifactInfo
    )

    $Name = $ArtifactInfo.Name
    $Version = $ArtifactInfo.Version
    $UrlTemplate = $ArtifactInfo.Url
    $ExpectedHash = $ArtifactInfo.Hash
    
    # 处理 URL 占位符
    # 支持 {Key} 形式的占位符，使用 $ArtifactInfo 中的值进行替换
    $DownloadUrl = $UrlTemplate
    foreach ($key in $ArtifactInfo.Keys) {
        $placeholder = "{$key}"
        if ($DownloadUrl.Contains($placeholder)) {
            $DownloadUrl = $DownloadUrl.Replace($placeholder, $ArtifactInfo[$key])
        }
    }
    
    # 推断文件名
    $ArchiveName = $ArtifactInfo.ArchiveName
    if ([string]::IsNullOrEmpty($ArchiveName)) {
        $ArchiveName = Split-Path $DownloadUrl -Leaf
    }

    # 目录设置
    $ScriptRoot = $PSScriptRoot
    $SdksRoot = Join-Path $ScriptRoot "SDKs"
    $SdkRoot = Join-Path $SdksRoot $Name
    $VersionRoot = Join-Path $SdkRoot "v$Version"
    $ArchivePath = Join-Path $VersionRoot $ArchiveName
    $ExtractDir = Join-Path $VersionRoot "extracted"
    $StampFile = Join-Path $VersionRoot ".done"

    Write-Host "正在处理 $Name v$Version ..." -ForegroundColor Cyan

    # 检查是否已经下载并解压
    if (Test-Path $StampFile) {
        Write-Host "$Name v$Version 已经存在，跳过下载" -ForegroundColor Green
        return
    }

    # 检查哈希值要求
    if ($ArtifactInfo.EnforceHash -and [string]::IsNullOrWhiteSpace($ExpectedHash)) {
        Write-Error "[$Name] 启用了哈希校验但未提供期望的哈希值。"
        exit 1
    }

    # 创建目录
    if (-not (Test-Path $VersionRoot)) {
        New-Item -ItemType Directory -Force -Path $VersionRoot | Out-Null
    }

    if (Test-Path $ArchivePath) {
        Write-Host "文件已存在，跳过下载: $ArchivePath" -ForegroundColor Green
    }
    else {
        Write-Host "开始下载 $Name v$Version..." -ForegroundColor Cyan
        Write-Host "URL: $DownloadUrl" -ForegroundColor Gray

        try {
            # 下载文件
            $ProgressPreference = 'Continue'
            Invoke-WebRequest -Uri $DownloadUrl -OutFile $ArchivePath -UseBasicParsing
            Write-Host "下载完成" -ForegroundColor Green
        }
        catch {
            Write-Error "下载失败: $_"
            exit 1
        }
    }

    # 验证哈希值
    if (-not [string]::IsNullOrWhiteSpace($ExpectedHash)) {
        Write-Host "验证文件哈希..." -ForegroundColor Cyan
        $ActualHash = (Get-FileHash -Path $ArchivePath -Algorithm SHA256).Hash
        
        if ($ActualHash -ne $ExpectedHash) {
            Write-Error "哈希值不匹配!`n期望: $ExpectedHash`n实际: $ActualHash`n文件保留: $ArchivePath"
            exit 1
        } else {
            Write-Host "哈希值验证通过" -ForegroundColor Green
        }
    }

    # 解压文件
    Write-Host "解压到 $ExtractDir..." -ForegroundColor Cyan
    if (Test-Path $ExtractDir) {
        Remove-Item $ExtractDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $ExtractDir | Out-Null

    try {
        Expand-Archive -Path $ArchivePath -DestinationPath $ExtractDir -Force
        Write-Host "解压完成" -ForegroundColor Green
    }
    catch {
        Write-Error "解压失败: $_"
        exit 1
    }

    # 验证必需文件
    if ($ArtifactInfo.ContainsKey("ValidationFiles")) {
        foreach ($file in $ArtifactInfo.ValidationFiles) {
            $filePath = Join-Path $ExtractDir $file
            if (-not (Test-Path $filePath)) {
                Write-Error "[$Name] 预编译包缺少必需文件: $file"
                exit 1
            }
        }
    }

    # 创建完成标记
    New-Item -ItemType File -Force -Path $StampFile | Out-Null

    Write-Host "`n$Name v$Version 安装成功!" -ForegroundColor Green
    Write-Host "安装路径: $ExtractDir`n" -ForegroundColor Gray
}

# ==========================================
# 读取 Manifest 并调用
# ==========================================

$ManifestPath = Join-Path $PSScriptRoot "sdk_manifest.json"

if (-not (Test-Path $ManifestPath)) {
    Write-Error "未找到配置文件: $ManifestPath"
    exit 1
}

try {
    $Manifest = Get-Content $ManifestPath -Raw | ConvertFrom-Json
}
catch {
    Write-Error "解析配置文件失败: $_"
    exit 1
}

if ($Manifest.artifacts) {
    $CurrentPlatform = if ($IsWindows) { "windows" } elseif ($IsLinux) { "linux" } elseif ($IsMacOS) { "macos" } else { "unknown" }
    $ProcessedNames = @{}

    foreach ($Artifact in $Manifest.artifacts) {
        $ArtifactInfo = @{}
        $Artifact.PSObject.Properties | ForEach-Object {
            $ArtifactInfo[$_.Name] = $_.Value
        }

        $Name = $ArtifactInfo.Name

        # 检查平台兼容性
        if ($ArtifactInfo.ContainsKey("Platform")) {
            $SupportedPlatforms = $ArtifactInfo.Platform
            if ($SupportedPlatforms -notcontains $CurrentPlatform) {
                Write-Host "[$Name] 跳过: 当前平台 ($CurrentPlatform) 不在支持列表 ($($SupportedPlatforms -join ', ')) 中。" -ForegroundColor Yellow
                continue
            }
        }

        # 检查 Name 是否重复
        if ($ProcessedNames.ContainsKey($Name)) {
            Write-Error "配置错误: 发现重复的 SDK 名称 '$Name' 适用于当前平台 ($CurrentPlatform)。请检查 sdk_manifest.json。"
            exit 1
        }
        $ProcessedNames[$Name] = $true
        
        Fetch-SDK-Artifact -ArtifactInfo $ArtifactInfo
    }
}
