<#
.SYNOPSIS
    Undoes Apply-LiquidGlass.ps1 using the backup it saved.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Restore-Windows.ps1
#>
$ErrorActionPreference = 'Stop'
$backupFile = Join-Path $env:LOCALAPPDATA 'LiquidGlass\backup.xml'

if (-not (Test-Path $backupFile)) {
    Write-Host "No backup found at $backupFile - nothing to restore." -ForegroundColor Yellow
    exit 1
}

$entries = @(Import-Clixml -Path $backupFile)
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$skipped = 0

foreach ($e in $entries) {
    if ($e.Path -like 'HKLM:*' -and -not $isAdmin) { $skipped++; continue }
    if ($e.Existed) {
        $value = if ($e.Kind -eq 'Binary') { [Convert]::FromBase64String($e.Value) } else { $e.Value }
        New-ItemProperty -Path $e.Path -Name $e.Name -Value $value -PropertyType $e.Kind -Force | Out-Null
    } else {
        Remove-ItemProperty -Path $e.Path -Name $e.Name -ErrorAction SilentlyContinue
    }
}

# Re-apply the original wallpaper
$wall = (Get-ItemProperty 'HKCU:\Control Panel\Desktop').Wallpaper
if (-not ('LiquidGlass.Native' -as [type])) {
    Add-Type -Namespace LiquidGlass -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern int SystemParametersInfo(int uAction, int uParam, string lpvParam, int fuWinIni);
'@
}
[LiquidGlass.Native]::SystemParametersInfo(0x14, 0, $wall, 3) | Out-Null

if ($skipped -gt 0) {
    Write-Host "Skipped $skipped lock-screen setting(s): re-run this as Administrator to restore those too." -ForegroundColor Yellow
    Write-Host 'Your backup file was kept so you can do that.' -ForegroundColor Yellow
} else {
    Remove-Item $backupFile -Force
}

Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }
Write-Host 'Restored your previous Windows look.' -ForegroundColor Cyan
