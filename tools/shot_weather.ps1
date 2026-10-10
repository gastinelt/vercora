# ===========================================================================
# PHASE 27 - Captures epinglees de la meteo visible et du micro-relief.
# ===========================================================================
# Meme discipline que shot_daynight.ps1 (--lock-input + --look + --start +
# --hour + --time-scale=0), plus --day : sans lui, --hour ne choisit que l'heure
# du JOUR 0, qui ne contient aucune perturbation - donc aucune pluie a montrer.
#
# Les instants par defaut ont ete CHOISIS dans le calendrier deterministe
# (seed fixe), pas au hasard ; voir NOTES-PHASE27.md, section "Instants de
# reference" :
#   j12 13 h : nebulosite 0,14, pas de pluie   (grand beau)
#   j0  13 h : nebulosite 0,54, pas de pluie   (mi-couvert)
#   j8  13 h : nebulosite 0,95, 4,9 mm/h       (pluie franche)
#   j8  22 h : pluie de nuit
#   j169 9 h 15 : neige, 7 mm/h equivalents, -3 C a l'altitude de l'alpage
#
# Difference avec screenshot_scene.ps1 : la sortie du jeu est JOURNALISEE dans
# un fichier a cote de l'image (Start-Process -RedirectStandardOutput). C'est ce
# qui permet de compter les messages [Validation] et de relire la ligne meteo
# de chaque capture - une capture dont on ne sait pas quel temps il faisait ne
# prouve rien.
#
# Usage :
#   powershell -File tools\shot_weather.ps1 -OutDir build\phase27\avant
#   powershell -File tools\shot_weather.ps1 -OutDir build\phase27\apres -Only meteo
# ===========================================================================

param(
    [string]$OutDir = "build\phase27\captures",
    [string]$Scene = "assets\terrain\vercora_zone_1m.vchm",
    # "tout", "meteo" ou "sol".
    [string]$Only = "tout",
    [int]$SettleSeconds = 10,
    # Arguments supplementaires passes a TOUTES les captures (ex. un temoin
    # --no-weather-visuals).
    [string[]]$Extra = @()
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\vercora.exe"
$scenePath = Join-Path $root $Scene
$outRoot = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W32W {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

# Vue de reference de l'alpage (Phase 18, 124 touffes comptees), et deux
# variantes du MEME point pour le sol : au ras (regard plonge) et au loin.
$alpage = "900500,6439000"
$shots = @(
    @{ n = "meteo_j12_13h_beau";    d = 12; h = 13; s = $alpage; l = "-90,-8";  k = "meteo" },
    @{ n = "meteo_j00_13h_micouv";  d = 0;  h = 13; s = $alpage; l = "-90,-8";  k = "meteo" },
    @{ n = "meteo_j08_13h_pluie";   d = 8;  h = 13; s = $alpage; l = "-90,-8";  k = "meteo" },
    @{ n = "meteo_j08_22h_pluienuit"; d = 8; h = 22; s = $alpage; l = "-90,-8"; k = "meteo" },
    @{ n = "meteo_j169_0915_neige"; d = 169; h = 9.25; s = $alpage; l = "-90,-8"; k = "meteo" },
    @{ n = "sol_ras_j12_13h";       d = 12; h = 13; s = $alpage; l = "-90,-50"; k = "sol" },
    @{ n = "sol_loin_j12_13h";      d = 12; h = 13; s = $alpage; l = "-90,-2";  k = "sol" }
)

$validationTotal = 0
foreach ($shot in $shots) {
    if ($Only -ne "tout" -and $Only -ne $shot.k) { continue }

    $stale = Get-Process -Name "vercora" -ErrorAction SilentlyContinue
    if ($stale) { $stale | Stop-Process -Force; Start-Sleep -Milliseconds 500 }

    $png = Join-Path $outRoot "$($shot.n).png"
    $log = Join-Path $outRoot "$($shot.n).log"
    $gameArgs = @("`"$scenePath`"", "--start=$($shot.s)", "--look=$($shot.l)",
                  "--day=$($shot.d)", ("--hour=" + ([double]$shot.h).ToString([System.Globalization.CultureInfo]::InvariantCulture)), "--time-scale=0",
                  "--lock-input") + $Extra

    $proc = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $root `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru

    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 900 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        $proc.Refresh()
        if ($proc.HasExited) { break }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle }
        Start-Sleep -Milliseconds 100
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        Write-Host "[ECHEC] $($shot.n) : pas de fenetre."
        if (-not $proc.HasExited) { $proc.Kill() }
        exit 1
    }
    Start-Sleep -Seconds $SettleSeconds

    $rect = New-Object W32W+RECT
    [void][W32W]::GetClientRect($hwnd, [ref]$rect)
    $w = $rect.R - $rect.L; $h = $rect.B - $rect.T
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $gfx.GetHdc()
    [void][W32W]::PrintWindow($hwnd, $dc, 2)
    $gfx.ReleaseHdc($dc); $gfx.Dispose()
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()

    [void][W32W]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $proc.WaitForExit(30000)) { $proc.Kill(); $proc.WaitForExit() }

    $text = (Get-Content $log -Raw) + (Get-Content "$log.err" -Raw -ErrorAction SilentlyContinue)
    $validation = ([regex]::Matches($text, "\[Validation\]")).Count
    $validationTotal += $validation
    $meteo = ($text -split "`n" | Select-String "Visuels meteo" | Select-Object -Last 1)
    Write-Host ("{0,-26} validation={1}  {2}" -f $shot.n, $validation, $meteo)
}

Write-Host ""
Write-Host "[FIN] messages [Validation] cumules : $validationTotal"
if ($validationTotal -gt 0) { exit 3 }
exit 0
