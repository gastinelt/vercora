# ===========================================================================
# shot_editor.ps1 - capture d'ecran du MODE EDITEUR (Phase 15).
# ===========================================================================
# Pourquoi ce script existe a cote de screenshot_scene.ps1 : l'editeur a trois
# etats VISUELS qu'aucune autre trace ne peut prouver.
#
#   * l'objet POSE, rendu comme n'importe quel objet du monde ;
#   * le FANTOME, teinte en cyan, qui suit la visee et n'est pas encore pose ;
#   * l'objet SELECTIONNE, teinte en ambre.
#
# Les trois passent par le MEME mesh et le meme pipeline : seule la teinte de
# temoin poussee en push constant les distingue (voir shaders/cube.frag). Un
# journal ne peut donc pas dire si elle s'applique - il faut regarder l'image.
# C'est la meme raison qui avait motive screenshot_scene.ps1 en Phase 10 : un
# culling casse produit d'excellents FPS, et seule une capture le montre.
#
# screenshot_scene.ps1 ne suffit pas parce qu'il ne PILOTE rien : il lance,
# attend, capture. Or selectionner un objet demande d'appuyer sur une touche.
#
# Comme partout dans ce projet, les touches sont envoyees par PostMessage avec
# leur SCANCODE (jeu de codes 1) et non leur code virtuel : MapVirtualKey traduit
# selon la disposition clavier active, alors que GLFW raisonne en positions
# physiques - voir l'en-tete de run_vulkan_check.ps1 pour le detail du piege.
#
#   powershell -File tools\shot_editor.ps1
#   powershell -File tools\shot_editor.ps1 -Out build\editeur.png -Select
#
# ATTENTION : la camera est verrouillee (--lock-input). Sans ce verrou, la souris
# SYSTEME reoriente la vue pendant la pose et deux captures du meme script ne
# cadrent pas la meme chose (constate en Phase 11). --start et --look determinent
# donc entierement l'image.
# ===========================================================================

param(
    [string]$Scene = "assets\terrain\LHD_0907_6440_1m.vchm",
    [string]$SceneFile = "assets\scenes\demo_phase15.vcsc",
    [string]$Start = "--start=907500,6439500",
    [string]$Look = "--look=-90,-8",
    [string]$Out = "build\p15_editeur.png",
    # Selectionne l'objet vise avant de capturer : montre la teinte AMBRE en plus
    # de la teinte cyan du fantome.
    [switch]$Select,
    [int]$SettleSeconds = 6
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\vercora.exe"
if (-not (Test-Path $exe)) { throw "vercora.exe introuvable : $exe (lancer tools\build.bat)" }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W32E {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$WM_KEYDOWN = 0x0100
$WM_KEYUP   = 0x0101
$WM_CLOSE   = 0x0010
# PW_RENDERFULLCONTENT : sans lui, une fenetre rendue par le GPU ressort NOIRE.
$PW_RENDERFULLCONTENT = 2

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = (@('"' + $Scene + '"',
                    '"--editor"',
                    '"--lock-input"',
                    '"--no-vegetation"',
                    '"' + $Start + '"',
                    '"' + $Look + '"',
                    '"--scene=' + $SceneFile + '"') -join ' ')
$psi.WorkingDirectory = $root
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$proc = [System.Diagnostics.Process]::Start($psi)

$outSb = New-Object System.Text.StringBuilder
$errSb = New-Object System.Text.StringBuilder
$handlers = @(
    (Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
        if ($null -ne $EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) }
    } -MessageData $outSb),
    (Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
        if ($null -ne $EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) }
    } -MessageData $errSb)
)
$proc.BeginOutputReadLine()
$proc.BeginErrorReadLine()

$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 150 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    $proc.Refresh()
    if ($proc.HasExited) { break }
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle }
    Start-Sleep -Milliseconds 100
}
if ($hwnd -eq [IntPtr]::Zero) { throw "Pas de fenetre (HasExited=$($proc.HasExited))." }

Start-Sleep -Seconds $SettleSeconds

function Tap([int]$sc, [bool]$ext = $false, [int]$holdMs = 90) {
    # Le bit 24 du lParam est le drapeau "touche etendue" : GLFW indexe sa table
    # de scancodes par (scancode | 0x100) pour ces touches-la, donc l'oublier
    # designerait une touche du pave numerique au lieu de SUPPR, INSER ou d'une
    # fleche.
    $down = 1 -bor ($sc -shl 16)
    if ($ext) { $down = $down -bor (1 -shl 24) }
    $up = $down -bor (1 -shl 30) -bor (1 -shl 31)
    [void][W32E]::PostMessage($hwnd, $WM_KEYDOWN, [IntPtr]0, [IntPtr]$down)
    Start-Sleep -Milliseconds $holdMs
    [void][W32E]::PostMessage($hwnd, $WM_KEYUP, [IntPtr]0, [IntPtr]$up)
    Start-Sleep -Milliseconds 250
}

if ($Select) {
    # Pour montrer la teinte AMBRE il faut un objet selectionne, donc un objet
    # sous le reticule. Plutot que de dependre du cadrage - un objet deja pose
    # n'est pas forcement dans l'axe - on en POSE un a la visee, puis on le
    # selectionne : ca marche depuis n'importe quel point de vue.
    #
    # Entre les deux, on arme un asset MINCE (le poteau, rang 5). Sinon le
    # fantome cyan, pose exactement au meme endroit, recouvrirait entierement
    # l'objet ambre qu'on cherche justement a montrer.
    Tap 0x52 $true   # INSER : poser au point vise
    Start-Sleep -Milliseconds 300
    Tap 0x06         # 5 : armer le poteau
    Tap 0x12         # E : selectionner l'objet vise
    Start-Sleep -Milliseconds 500
}

# --- Capture ---------------------------------------------------------------
$rect = New-Object W32E+RECT
[void][W32E]::GetClientRect($hwnd, [ref]$rect)
$width = $rect.R - $rect.L
$height = $rect.B - $rect.T
$bitmap = New-Object System.Drawing.Bitmap($width, $height)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$dc = $graphics.GetHdc()
[void][W32E]::PrintWindow($hwnd, $dc, $PW_RENDERFULLCONTENT)
$graphics.ReleaseHdc($dc)
$graphics.Dispose()

$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
$outDir = Split-Path -Parent $outPath
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$bitmap.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
Write-Host "CAPTURE $outPath ($width x $height)"

[void][W32E]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
if (-not $proc.WaitForExit(20000)) { $proc.Kill(); $proc.WaitForExit() }
$proc.WaitForExit()
Start-Sleep -Milliseconds 400
foreach ($h in $handlers) { Unregister-Event -SourceIdentifier $h.Name -ErrorAction SilentlyContinue }

$text = $outSb.ToString() + $errSb.ToString()
$lines = @($text -split "`r?`n" | Where-Object { $_ -ne '' })
$validation = @($lines | Where-Object { $_ -match '\[Validation\]' }).Count
$lines | Where-Object { $_ -match 'objet\(s\) charges|asset\(s\) au catalogue|selection de|Editeur : pinceau' } |
    Select-Object -Last 4 | ForEach-Object { Write-Host $_ }
Write-Host "Messages de validation : $validation ; code de sortie : $($proc.ExitCode)"
if ($validation -ne 0 -or $proc.ExitCode -ne 0) { exit 1 }
