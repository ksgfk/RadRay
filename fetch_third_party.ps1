function Fetch-Git {
    param (
        [Parameter(Mandatory = $true)]
        [hashtable]$SrcInfo
    )

    $ThirdPartyDir = Join-Path $PSScriptRoot "third_party"
    if (-not (Test-Path $ThirdPartyDir)) {
        New-Item -ItemType Directory -Path $ThirdPartyDir | Out-Null
    }

    $Name = $SrcInfo.Name
    $RepoUrl = $SrcInfo.Git
    $TargetDir = Join-Path $ThirdPartyDir $Name

    Write-Host "正在处理 $Name..." -ForegroundColor Cyan

    if (-not (Test-Path $TargetDir)) {
        Write-Host "  正在从 $RepoUrl 克隆..."
        git clone $RepoUrl $TargetDir
        if ($LASTEXITCODE -ne 0) {
            Write-Error "  克隆 $Name 失败"
            return
        }
    }

    Push-Location $TargetDir
    try {
        Write-Host "  正在获取最新更改..."
        git fetch --all --tags --prune
        git reset --hard

        if ($SrcInfo.ContainsKey("Commit")) {
            $Commit = $SrcInfo.Commit
            Write-Host "  正在检出提交 $Commit..."
            git checkout $Commit
        }
        elseif ($SrcInfo.ContainsKey("Tag")) {
            $Tag = $SrcInfo.Tag
            Write-Host "  正在检出标签 $Tag..."
            git checkout "tags/$Tag"
        }
        elseif ($SrcInfo.ContainsKey("Branch")) {
            $Branch = $SrcInfo.Branch
            Write-Host "  正在检出分支 $Branch..."
            git checkout $Branch
            git reset --hard "origin/$Branch"
        }
        else {
            Write-Host "  正在拉取最新更改..."
            git pull
        }

        if ($SrcInfo.ContainsKey("Patch")) {
            $PatchFile = Join-Path $PSScriptRoot $SrcInfo.Patch
            if (Test-Path $PatchFile) {
                Write-Host "  正在应用补丁 $PatchFile..."
                git apply $PatchFile
            } else {
                Write-Warning "  未找到补丁文件: $PatchFile"
            }
        }
    }
    catch {
        Write-Error "  处理 $Name 时发生错误: $_"
    }
    finally {
        Pop-Location
    }
}

$ManifestPath = Join-Path $PSScriptRoot "project_manifest.json"
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
if ($Manifest.ThirdParties) {
    foreach ($ThirdParty in $Manifest.ThirdParties) {
        $ThirdPartyInfo = @{}
        $ThirdParty.PSObject.Properties | ForEach-Object {
            $ThirdPartyInfo[$_.Name] = $_.Value
        }
        if ($ThirdPartyInfo.Type -eq "git") {
            Fetch-Git -SrcInfo $ThirdPartyInfo
        } else {
            Write-Warning "未知的类型: $($ThirdPartyInfo.Type)"
        }
    }
}
