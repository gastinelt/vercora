# measure_terrain.ps1 - mesure FPS et memoire d'une scene, sans toucher au moteur.
#
# PHASE 9. La Phase 7 relevait ses chiffres a la main (848 FPS, 185 Mo). A
# l'echelle de 25 km2 il faut comparer plusieurs scenes entre elles, donc une
# mesure reproductible :
#
#   powershell -File tools\measure_terrain.ps1 -Scene assets\terrain\LHD_0907_6440_1m.vchm
#   powershell -File tools\measure_terrain.ps1 -Scene terrain -Seconds 20
#
# Le FPS est lu dans le TITRE de la fenetre (le moteur l'y ecrit deja) et la
# memoire via PeakWorkingSet64 : rien n'est ajoute au jeu pour l'occasion, donc
# la mesure porte sur exactement le binaire qui tourne en vrai.
#
# Les premieres secondes sont IGNOREES (chargement, premiers frames, montee en
# regime du pilote) : sans cela la moyenne d'une grosse scene est tiree vers le
# bas par son propre chargement, et la comparaison entre scenes n'a plus de sens.
#
# Sortie : lignes "MESURE ..." faciles a relire, et code de sortie non nul si le
# jeu n'a pas demarre ou n'est pas sorti proprement.

param(
    [Parameter(Mandatory = $true)][string]$Scene,
    [int]$Seconds = 15,
    [int]$WarmupSeconds = 5,
    # PHASE 10 : options supplementaires passees telles quelles au jeu, pour
    # mesurer la MEME scene avec et sans le systeme de LOD (--no-lod). Sans ce
    # parametre il faudrait deux scripts, ou recompiler entre les deux mesures -
    # et la comparaison ne porterait plus sur le meme binaire.
    [string[]]$GameArgs = @()
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\vercora.exe"
if (-not (Test-Path $exe)) { throw "vercora.exe introuvable : $exe (lancer tools\build.bat)" }

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W32M {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
}
"@
$WM_CLOSE = 0x0010

Write-Host ("=== Mesure : {0} {1}===" -f $Scene, ($(if ($GameArgs.Count) { ($GameArgs -join ' ') + ' ' } else { '' })))

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = (@('"' + $Scene + '"') + $GameArgs) -join ' '
$psi.WorkingDirectory = $root
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true

$loadStart = Get-Date
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
for ($i = 0; $i -lt 600 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    $proc.Refresh()
    if ($proc.HasExited) { break }
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle }
    Start-Sleep -Milliseconds 100
}
if ($hwnd -eq [IntPtr]::Zero) {
    $proc.WaitForExit()
    Start-Sleep -Milliseconds 300
    foreach ($h in $handlers) { Unregister-Event -SourceIdentifier $h.Name -ErrorAction SilentlyContinue }
    Write-Host ($outSb.ToString() + $errSb.ToString())
    Write-Host "[ECHEC] Pas de fenetre (HasExited=$($proc.HasExited), code=$($proc.ExitCode))."
    exit 1
}
$loadSeconds = ((Get-Date) - $loadStart).TotalSeconds
Write-Host ("MESURE chargement       : {0:N1} s (lancement -> fenetre affichee)" -f $loadSeconds)

Start-Sleep -Seconds $WarmupSeconds

$samples = New-Object System.Collections.ArrayList
$sb = New-Object System.Text.StringBuilder 512
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    $proc.Refresh()
    if ($proc.HasExited) { break }
    [void]$sb.Clear()
    [void][W32M]::GetWindowTextW($hwnd, $sb, 512)
    $title = $sb.ToString()
    if ($title -match '(\d+)\s*FPS') { [void]$samples.Add([int]$Matches[1]) }
    Start-Sleep -Milliseconds 250
}

$proc.Refresh()
$peakMb = [math]::Round($proc.PeakWorkingSet64 / 1MB, 1)
$workingMb = [math]::Round($proc.WorkingSet64 / 1MB, 1)

if ($samples.Count -gt 0) {
    $sorted = $samples | Sort-Object
    $median = $sorted[[int]($sorted.Count / 2)]
    $stats = $samples | Measure-Object -Average -Minimum -Maximum
    Write-Host ("MESURE FPS              : median {0}, moyen {1:N0}, min {2}, max {3}  ({4} releves)" -f `
        $median, $stats.Average, $stats.Minimum, $stats.Maximum, $samples.Count)
} else {
    Write-Host "MESURE FPS              : aucun releve (titre sans FPS ?)"
}
Write-Host ("MESURE memoire          : pic {0} Mo, courant {1} Mo (processus, RAM systeme)" -f $peakMb, $workingMb)

[void][W32M]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
if (-not $proc.WaitForExit(30000)) {
    Write-Host "[ECHEC] Pas de sortie 30 s apres WM_CLOSE."
    $proc.Kill(); $proc.WaitForExit()
    $code = 1
} else {
    $code = $proc.ExitCode
}
$proc.WaitForExit()
Start-Sleep -Milliseconds 400
foreach ($h in $handlers) { Unregister-Event -SourceIdentifier $h.Name -ErrorAction SilentlyContinue }

$text = $outSb.ToString() + $errSb.ToString()
$lines = @($text -split "`r?`n" | Where-Object { $_ -ne '' })

# Zero erreur de validation Vulkan est une contrainte de chaque etape, pas
# seulement du harnais complet : on la verifie donc aussi ici.
$validation = @($lines | Where-Object { $_ -match '\[Validation\]' })
$errors     = @($lines | Where-Object { $_ -match '\[ERROR\]|\[WARN\]' })
Write-Host ("MESURE validation Vulkan: {0} message(s) [Validation], {1} [ERROR]/[WARN]" -f `
    $validation.Count, $errors.Count)
foreach ($l in ($validation + $errors) | Select-Object -First 12) { Write-Host "   $l" }

$interesting = @($lines | Where-Object { $_ -match 'Terrain|echantillon|sommets|triangles|Lambert|denivele|Quadtree|tuiles' })
foreach ($l in $interesting | Select-Object -First 12) { Write-Host "   | $l" }

Write-Host ("MESURE code de sortie   : {0}" -f $code)
if ($validation.Count -gt 0 -or $errors.Count -gt 0 -or $code -ne 0) { exit 1 }
exit 0
