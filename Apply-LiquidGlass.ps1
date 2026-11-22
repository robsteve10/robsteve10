<#
.SYNOPSIS
    Applies the "Liquid Glass" desktop theme (Perplexity-inspired teal glass look).

.DESCRIPTION
    - Dark mode (or light "Paper" mode with -Theme Light)
    - Transparency effects on, turquoise accent color palette
    - Liquid glass wallpaper (4K, Fill)
    - Lock screen image (only when run as Administrator)
    - Optional: installs TranslucentTB, Windhawk and Mica For Everyone via winget

    Every registry value it touches is backed up first to
    %LOCALAPPDATA%\LiquidGlass\backup.xml so Restore-Windows.ps1 can undo it.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Apply-LiquidGlass.ps1
    powershell -ExecutionPolicy Bypass -File .\Apply-LiquidGlass.ps1 -Wallpaper panels
    powershell -ExecutionPolicy Bypass -File .\Apply-LiquidGlass.ps1 -Theme Light
    powershell -ExecutionPolicy Bypass -File .\Apply-LiquidGlass.ps1 -InstallGlassApps
#>
[CmdletBinding()]
param(
    [ValidateSet('Dark', 'Light')]
    [string]$Theme = 'Dark',

    [ValidateSet('depth', 'panels', 'paper')]
    [string]$Wallpaper,

    [switch]$InstallGlassApps,

    [switch]$SkipLockScreen
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $Wallpaper) { $Wallpaper = if ($Theme -eq 'Light') { 'paper' } else { 'depth' } }

# Paths
$stateDir  = Join-Path $env:LOCALAPPDATA 'LiquidGlass'
$wallDir   = Join-Path ([Environment]::GetFolderPath('MyPictures')) 'LiquidGlass'
$backupFile = Join-Path $stateDir 'backup.xml'
New-Item -ItemType Directory -Force -Path $stateDir, $wallDir | Out-Null

Write-Host ''
Write-Host '  Liquid Glass for Windows' -ForegroundColor Cyan
Write-Host "  Theme: $Theme   Wallpaper: $Wallpaper" -ForegroundColor DarkCyan
Write-Host ''

# Backup-aware registry writer
$backup = New-Object System.Collections.Generic.List[object]
if (Test-Path $backupFile) {
    # Re-run: keep the ORIGINAL values, only add settings that were never backed up.
    Write-Host '  Existing backup found - keeping your original settings.' -ForegroundColor DarkGray
    foreach ($e in @(Import-Clixml -Path $backupFile)) { $backup.Add($e) }
}

function Set-Reg {
    param([string]$Path, [string]$Name, $Value, [string]$Kind = 'DWord')
    if (-not (Test-Path $Path)) { New-Item -Path $Path -Force | Out-Null }
    $existing = Get-ItemProperty -Path $Path -Name $Name -ErrorAction SilentlyContinue
    $oldKind = $null
    if ($existing) {
        $oldKind = (Get-Item -LiteralPath $Path).GetValueKind($Name).ToString()
        # Keep the value's original registry type (e.g. REG_SZ vs REG_DWORD) unless it's binary data.
        if ($Kind -ne 'Binary' -and $oldKind -in 'DWord', 'String') { $Kind = $oldKind }
    }
    $known = $script:backup | Where-Object { $_.Path -eq $Path -and $_.Name -eq $Name }
    if (-not $known) {
        $entry = [ordered]@{ Path = $Path; Name = $Name; Existed = [bool]$existing; Kind = $Kind; Value = $null }
        if ($existing) {
            $v = $existing.$Name
            $entry.Kind = $oldKind
            if ($v -is [byte[]]) { $entry.Value = [Convert]::ToBase64String($v) } else { $entry.Value = $v }
        }
        $script:backup.Add([pscustomobject]$entry)
    }
    New-ItemProperty -Path $Path -Name $Name -Value $Value -PropertyType $Kind -Force | Out-Null
}

# ABGR / ARGB hex string -> signed Int32 (works in Windows PowerShell 5.1 and PowerShell 7)
function Hex32([string]$hex) { [Convert]::ToInt32($hex, 16) }

# 1. Copy wallpapers
Write-Host '  [1/5] Copying wallpapers to' $wallDir
Copy-Item -Path (Join-Path $here 'wallpapers\*.jpg') -Destination $wallDir -Force
$wallPath = Join-Path $wallDir "liquid-glass-$Wallpaper.jpg"
$lockPath = Join-Path $wallDir 'liquid-glass-lockscreen.jpg'

# 2. Light/dark + transparency
Write-Host '  [2/5] Setting color mode and transparency'
$personalize = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$light = [int]($Theme -eq 'Light')
Set-Reg $personalize 'AppsUseLightTheme'   $light
Set-Reg $personalize 'SystemUsesLightTheme' 0          # keep taskbar/Start dark: glass reads best on dark
Set-Reg $personalize 'EnableTransparency'  1
Set-Reg $personalize 'ColorPrevalence'     0          # no solid accent fill on Start/taskbar: stay translucent

# 3. Turquoise accent palette
Write-Host '  [3/5] Applying turquoise accent palette'
Set-Reg 'HKCU:\Control Panel\Desktop' 'AutoColorization' '0' 'String'

# 8 swatches, RGBA bytes each: Light3, Light2, Light1, Accent, Dark1, Dark2, Dark3, (extra)
$palette = [byte[]]@(
    0xBA,0xDE,0xDD,0x00,   # #BADEDD  sky
    0x7C,0xD2,0xDE,0x00,   # #7CD2DE
    0x1F,0xB8,0xCD,0x00,   # #1FB8CD  plex blue
    0x20,0x80,0x8D,0x00,   # #20808D  true turquoise (accent)
    0x1A,0x68,0x73,0x00,   # #1A6873
    0x16,0x4E,0x57,0x00,   # #164E57
    0x13,0x34,0x3B,0x00,   # #13343B  inky blue
    0x69,0x79,0x7E,0x00    # #69797E
)
$accent = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Accent'
Set-Reg $accent 'AccentPalette'   $palette 'Binary'
Set-Reg $accent 'AccentColorMenu' (Hex32 'FF8D8020')   # ABGR of #20808D
Set-Reg $accent 'StartColorMenu'  (Hex32 'FF574E16')   # ABGR of #164E57

$dwm = 'HKCU:\Software\Microsoft\Windows\DWM'
Set-Reg $dwm 'AccentColor'            (Hex32 'FF8D8020')   # ABGR
Set-Reg $dwm 'ColorizationColor'      (Hex32 'C420808D')   # ARGB
Set-Reg $dwm 'ColorizationAfterglow'  (Hex32 'C420808D')
Set-Reg $dwm 'ColorPrevalence'        0                   # title bars use Mica, not a solid color
Set-Reg $dwm 'EnableWindowColorization' 0

# 4. Wallpaper
Write-Host '  [4/5] Setting wallpaper'
$desktop = 'HKCU:\Control Panel\Desktop'
Set-Reg $desktop 'Wallpaper'      $wallPath 'String'
Set-Reg $desktop 'WallpaperStyle' '10' 'String'   # Fill
Set-Reg $desktop 'TileWallpaper'  '0'  'String'

if (-not ('LiquidGlass.Native' -as [type])) {
    Add-Type -Namespace LiquidGlass -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern int SystemParametersInfo(int uAction, int uParam, string lpvParam, int fuWinIni);
'@
}
# SPI_SETDESKWALLPAPER = 0x14, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE = 3
[LiquidGlass.Native]::SystemParametersInfo(0x14, 0, $wallPath, 3) | Out-Null

# 5. Lock screen (needs admin)
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if ($SkipLockScreen) {
    Write-Host '  [5/5] Lock screen skipped (-SkipLockScreen)'
} elseif ($isAdmin) {
    Write-Host '  [5/5] Setting lock screen image'
    $csp = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\PersonalizationCSP'
    Set-Reg $csp 'LockScreenImagePath'   $lockPath 'String'
    Set-Reg $csp 'LockScreenImageUrl'    $lockPath 'String'
    Set-Reg $csp 'LockScreenImageStatus' 1
} else {
    Write-Host '  [5/5] Lock screen: not admin, so set it by hand:' -ForegroundColor Yellow
    Write-Host "        Settings > Personalization > Lock screen > Picture > Browse > $lockPath" -ForegroundColor Yellow
}

# Save backup
# Export-Clixml, not ConvertTo-Json: Windows PowerShell 5.1's JSON writer throws
# "Argument types do not match" on these objects.
$backup.ToArray() | Export-Clixml -Path $backupFile
Write-Host "  Backup of your previous settings: $backupFile" -ForegroundColor DarkGray

# Optional glass apps
if ($InstallGlassApps) {
    Write-Host ''
    Write-Host '  Installing glass helper apps with winget...' -ForegroundColor Cyan
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Write-Host '  winget not found. Install "App Installer" from the Microsoft Store, then re-run.' -ForegroundColor Yellow
    } else {
        foreach ($id in 'CharlesMilette.TranslucentTB', 'RamenSoftware.Windhawk', 'MicaForEveryone.MicaForEveryone') {
            Write-Host "   - $id"
            winget install --id $id -e --accept-source-agreements --accept-package-agreements --silent
        }
        Write-Host '  See README.md for the recommended settings in each app.' -ForegroundColor DarkCyan
    }
}

# Restart Explorer so the accent palette and taskbar pick up the change
Write-Host ''
Write-Host '  Restarting Explorer to apply...' -ForegroundColor DarkGray
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }

Write-Host ''
Write-Host '  Done. Sign out and back in if any color looks stale.' -ForegroundColor Cyan
Write-Host '  To undo: powershell -ExecutionPolicy Bypass -File .\Restore-Windows.ps1' -ForegroundColor DarkGray
Write-Host ''
