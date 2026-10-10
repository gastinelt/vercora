# screenshot_scene.ps1 - lance une scene, capture la fenetre, ferme proprement.
#
# PHASE 10. Raison d'etre : un bug de frustum culling ou de selection de LOD se
# traduit par un EXCELLENT chiffre de FPS - on ne dessine plus rien. Les mesures
# de measure_terrain.ps1 ne peuvent donc pas, a elles seules, valider le systeme
# de tuiles ; il faut regarder l'image. Ce script produit ce regard sans voler la
# souris ni le clavier de l'utilisateur.
#
#   powershell -File tools\screenshot_scene.ps1 -Scene assets\terrain\vercora_zone_1m.vchm -Out build\zone.png
#   powershell -File tools\screenshot_scene.ps1 -Scene terrain -GameArgs --no-lod -Out build\ref.png
#
# La capture passe par PrintWindow avec PW_RENDERFULLCONTENT (drapeau 2) : sans
# lui, une fenetre rendue par le GPU (Vulkan, DirectX) ressort NOIRE, la fenetre
# ne possedant pas de contenu GDI a recopier.

param(
    [Parameter(Mandatory = $true)][string]$Scene,
    [Parameter(Mandatory = $true)][string]$Out,
    [string[]]$GameArgs = @(),
    # Secondes d'attente avant la capture : le temps que la scene charge et que
    # le streaming ait servi les premieres tuiles.
    [int]$SettleSeconds = 6,
    # PHASE 11. Par defaut le jeu est lance avec --lock-input : la camera ignore
    # alors le clavier et la souris SYSTEME.
    #
    # Sans ce verrou, deux executions du MEME script ne cadrent pas la meme
    # chose : GLFW lit le vrai curseur (il ne voit pas les messages postes, cf.
    # l'en-tete de run_vulkan_check.ps1), donc toute manipulation de la machine
    # pendant la pose reoriente la camera. Constate directement : deux
    # executions consecutives ont donne yaw -90 et yaw -50, soit deux images qui
    # different sur 65 % de leurs pixels - ce qui prive de toute valeur une
    # comparaison avant/apres.
    #
    # -FreeInput redonne la main a l'utilisateur, pour une capture pilotee.
    [switch]$FreeInput
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\vercora.exe"
if (-not (Test-Path $exe)) { throw "vercora.exe introuvable : $exe (lancer tools\build.bat)" }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W32S {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$WM_CLOSE = 0x0010
$PW_RENDERFULLCONTENT = 2

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$lockArgs = if ($FreeInput) { @() } else { @('--lock-input') }
$psi.Arguments = (@('"' + $Scene + '"') + $GameArgs + $lockArgs) -join ' '
$psi.WorkingDirectory = $root
$psi.UseShellExecute = $false

$proc = [System.Diagnostics.Process]::Start($psi)

$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 900 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    $proc.Refresh()
    if ($proc.HasExited) { break }
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle }
    Start-Sleep -Milliseconds 100
}
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Host "[ECHEC] Pas de fenetre."
    if (-not $proc.HasExited) { $proc.Kill() }
    exit 1
}

Start-Sleep -Seconds $SettleSeconds

$rect = New-Object W32S+RECT
[void][W32S]::GetClientRect($hwnd, [ref]$rect)
$w = $rect.R - $rect.L
$h = $rect.B - $rect.T

$bmp = New-Object System.Drawing.Bitmap $w, $h
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $gfx.GetHdc()
[void][W32S]::PrintWindow($hwnd, $dc, $PW_RENDERFULLCONTENT)
$gfx.ReleaseHdc($dc)
$gfx.Dispose()

$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outPath) | Out-Null
$bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)

# Proportion de pixels "ciel" : un terrain qui aurait disparu (culling casse,
# tuiles jamais construites) donnerait une image quasi entierement de la couleur
# d'effacement. C'est un controle grossier mais automatique, qui complete l'oeil.
$sky = 0
for ($y = 0; $y -lt $h; $y += 8) {
    for ($x = 0; $x -lt $w; $x += 8) {
        $p = $bmp.GetPixel($x, $y)
        if ($p.B -gt $p.G -and $p.B -gt 150) { $sky++ }
    }
}
$total = [math]::Ceiling($h / 8) * [math]::Ceiling($w / 8)
$bmp.Dispose()

Write-Host ("CAPTURE {0} ({1}x{2}), ciel estime {3:N1} %" -f $outPath, $w, $h, (100.0 * $sky / $total))

[void][W32S]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
if (-not $proc.WaitForExit(30000)) { $proc.Kill(); $proc.WaitForExit(); exit 1 }
exit $proc.ExitCode
