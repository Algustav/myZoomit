$ErrorActionPreference = 'Stop'
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = $null
if (Test-Path -LiteralPath $vswherePath) {
    $installation = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVersions = @()
$includeRoot = Join-Path $sdkRoot 'Include'
if (Test-Path -LiteralPath $includeRoot) {
    $sdkVersions = @(Get-ChildItem -LiteralPath $includeRoot -Directory | Where-Object {
        (Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h')) -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'um\inkpresenterdesktop.h')) -and
        (Test-Path -LiteralPath (Join-Path $sdkRoot "Lib\$($_.Name)\um\x64\user32.lib"))
    } | Select-Object -ExpandProperty Name)
}
[pscustomobject]@{
    CppBuildTools = if ($installation) { $installation } else { 'Missing' }
    WindowsSDK = if ($sdkVersions.Count) { $sdkVersions -join ', ' } else { 'Missing' }
    Ready = [bool]($installation -and $sdkVersions.Count)
} | Format-List
if (-not ($installation -and $sdkVersions.Count)) { exit 1 }
