# 为当前 PowerShell 会话设置 RadRay 开发运行库搜索路径：DXC fork 与 /MD[d] 的 mimalloc DLL。
# 需要 dot-source 才会作用于当前终端，例如：
#   . tools/dev_env.ps1 -Triplet windows-x64-clangcl -Config Debug
# 重复执行会先移除上次加入的 RadRay SDK 路径，切换三元组或配置不会叠加。
param(
    [Parameter(Mandatory)]
    [ValidateSet('windows-x64-msvc', 'windows-x64-clangcl', 'windows-x64-msvc-staticcrt', 'windows-x64-clangcl-staticcrt')]
    [string]$Triplet,
    [Parameter(Mandatory)]
    [ValidateSet('Debug', 'Release')]
    [string]$Config
)

$sdkRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\SDKs'))
$entries = @(
    (Join-Path $sdkRoot 'radray_dxc\extracted\bin'),
    (Join-Path $sdkRoot "mimalloc\$Triplet\$Config\bin")
)

$kept = $env:PATH -split ';' | Where-Object {
    $_ -and -not ($_.StartsWith((Join-Path $sdkRoot 'radray_dxc'), [StringComparison]::OrdinalIgnoreCase) -or
                  $_.StartsWith((Join-Path $sdkRoot 'mimalloc'), [StringComparison]::OrdinalIgnoreCase))
}
$present = $entries | Where-Object { Test-Path $_ }
foreach ($missing in $entries | Where-Object { -not (Test-Path $_) }) {
    if ($missing -notlike '*staticcrt*') { Write-Warning "Runtime directory not found: $missing" }
}
$env:PATH = (@($present) + @($kept)) -join ';'
Write-Host "RadRay runtime paths ($Triplet/$Config):"
$present | ForEach-Object { Write-Host "  $_" }
