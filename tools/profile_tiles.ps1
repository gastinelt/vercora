# profile_tiles.ps1 - releve le PROFIL DE CONSTRUCTION DES TUILES sur un survol.
#
# PHASE 19. La Phase 18 a montre que le chronometrage cumule de
# TerrainStreamer::buildProfile() est BEAUCOUP plus stable que le FPS lu dans le
# titre de la fenetre (moins de 0,5 % d'ecart entre deux passages, contre ~15 %
# pour le FPS - voir MEMORY.md). C'est donc lui, et non le FPS, qui doit servir a
# juger une optimisation du chemin de streaming.
#
# La Phase 18 relevait ces lignes a la main dans la sortie du jeu. Ce script les
# extrait, ce qui rend le releve reproductible et comparable d'une phase a
# l'autre - exactement ce que measure_terrain.ps1 a fait pour le FPS en Phase 9.
#
# Usage :
#   powershell -File tools\profile_tiles.ps1 -Scene terrain -Seconds 45 `
#              -GameArgs @('--flythrough=900','--no-grass')
#
# Le jeu est ferme par WM_CLOSE, ce qui est indispensable : le bilan est
# journalise A LA DESTRUCTION du TerrainStreamer. Un processus tue n'ecrit rien.

param(
    [Parameter(Mandatory = $true)][string]$Scene,
    [int]$Seconds = 45,
    [string[]]$GameArgs = @(),
    # Repetitions du meme run. La Phase 18 en faisait deux systematiquement pour
    # pouvoir affirmer la stabilite du chiffre plutot que de la supposer.
    [int]$Runs = 1
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\vercora.exe"
if (-not (Test-Path $exe)) { throw "vercora.exe introuvable : $exe (lancer tools\build.bat)" }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W32P {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$WM_CLOSE = 0x0010

for ($run = 1; $run -le $Runs; $run++) {
    Write-Host ("=== Profil tuiles : {0} {1}(passage {2}/{3}) ===" -f `
        $Scene, $(if ($GameArgs.Count) { ($GameArgs -join ' ') + ' ' } else { '' }), $run, $Runs)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = (@('"' + $Scene + '"') + $GameArgs) -join ' '
    $psi.WorkingDirectory = $root
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $proc = [System.Diagnostics.Process]::Start($psi)
    $outSb = New-Object System.Text.StringBuilder
    $handlers = @(
        (Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
            if ($null -ne $EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) }
        } -MessageData $outSb),
        (Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
            if ($null -ne $EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) }
        } -MessageData $outSb)
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
        Write-Host $outSb.ToString()
        Write-Host "[ECHEC] Pas de fenetre."
        exit 1
    }

    Start-Sleep -Seconds $Seconds

    $proc.Refresh()
    $peakMb = [math]::Round($proc.PeakWorkingSet64 / 1MB, 1)
    [void][W32P]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $proc.WaitForExit(60000)) {
        Write-Host "[ECHEC] Pas de sortie 60 s apres WM_CLOSE."
        $proc.Kill(); $proc.WaitForExit()
        $code = 1
    } else {
        $code = $proc.ExitCode
    }
    Start-Sleep -Milliseconds 400
    foreach ($h in $handlers) { Unregister-Event -SourceIdentifier $h.Name -ErrorAction SilentlyContinue }

    $text = $outSb.ToString()
    $lines = @($text -split "`r?`n" | Where-Object { $_ -ne '' })

    # Une meme ligne peut satisfaire plusieurs motifs (le bilan de construction
    # porte aussi le bilan de parallelisme) : on deduplique, sinon le releve
    # affiche deux fois le meme chiffre, ce qui invite a le lire deux fois.
    $shown = @{}
    foreach ($pattern in @('Construction de tuiles',
                           'Trous de streaming',
                           'Streaming terrain : pics',
                           'Cout par image')) {
        foreach ($l in ($lines | Where-Object { $_ -match $pattern })) {
            if (-not $shown.ContainsKey($l)) { $shown[$l] = $true; Write-Host "   | $l" }
        }
    }

    $validation = @($lines | Where-Object { $_ -match '\[Validation\]' })
    $errors     = @($lines | Where-Object { $_ -match '\[ERROR\]|\[WARN\]' })
    Write-Host ("   validation Vulkan : {0} message(s) [Validation], {1} [ERROR]/[WARN]" -f `
        $validation.Count, $errors.Count)
    foreach ($l in ($validation + $errors) | Select-Object -First 12) { Write-Host "      $l" }
    Write-Host ("   pic memoire : {0} Mo, code de sortie : {1}" -f $peakMb, $code)

    if ($validation.Count -gt 0 -or $code -ne 0) { exit 1 }
}
exit 0
