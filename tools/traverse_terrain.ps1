# traverse_terrain.ps1 - traverse un terrain assemble en marchant, et surveille
# ce qui se passe AUX FRONTIERES entre anciennes dalles.
#
# PHASE 9. Complement de tools\measure_terrain.ps1 (qui mesure a l'arret) : ici
# la camera MARCHE, pour repondre a la question " la camera peut-elle se deplacer
# sur toute la zone sans probleme aux frontieres ". Un raccord defectueux se
# manifesterait par une chute de FPS localisee, un message de validation, une
# chute a travers le sol ou un plantage - donc par autre chose qu'un chiffre
# moyen.
#
#   powershell -File tools\traverse_terrain.ps1 -Scene assets\terrain\vercora_zone_2m.vchm
#   powershell -File tools\traverse_terrain.ps1 -Scene ... -Distance 1200
#
# Le terrain est CENTRE sur l'origine : sur une grille de 5 km, la camera demarre
# au milieu de la dalle centrale et les frontieres d'anciennes dalles tombent a
# 500 m et 1500 m de part et d'autre, sur les deux axes. Marcher 700 m dans une
# direction en franchit donc au moins une, quelle que soit la direction.
#
# LIMITE ASSUMEE, la meme qu'en Phase 7/8 : la souris ne peut pas etre pilotee
# (GLFW lit le curseur SYSTEME, pas les messages postes), donc la camera ne
# tourne jamais et la position n'est connue qu'A L'ESTIME (vitesse x temps).
# A bas FPS le jeu consomme les touches plus grossierement, donc la distance
# reellement parcourue est un ORDRE DE GRANDEUR, pas une mesure. Ce test prouve
# qu'on traverse sans incident, il ne mesure pas ou l'on arrive.

param(
    [Parameter(Mandatory = $true)][string]$Scene,
    [double]$Distance = 700.0,
    [double]$Speed = 14.0
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\vercora.exe"
if (-not (Test-Path $exe)) { throw "vercora.exe introuvable : $exe" }

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W32T {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
}
"@
$WM_KEYDOWN = 0x0100; $WM_KEYUP = 0x0101; $WM_CLOSE = 0x0010
$SC = @{ W = 0x11; A = 0x1E; S = 0x1F; D = 0x20 }

Write-Host "=== Traversee : $Scene ($Distance m par direction) ==="

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = '"' + $Scene + '"'
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
for ($i = 0; $i -lt 600 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    $proc.Refresh()
    if ($proc.HasExited) { break }
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle }
    Start-Sleep -Milliseconds 100
}
if ($hwnd -eq [IntPtr]::Zero) {
    $proc.WaitForExit(); Start-Sleep -Milliseconds 300
    foreach ($h in $handlers) { Unregister-Event -SourceIdentifier $h.Name -ErrorAction SilentlyContinue }
    Write-Host ($outSb.ToString() + $errSb.ToString())
    Write-Host "[ECHEC] Pas de fenetre."
    exit 1
}
Start-Sleep -Seconds 3

$sb = New-Object System.Text.StringBuilder 512
function Read-Fps() {
    [void]$sb.Clear()
    [void][W32T]::GetWindowTextW($hwnd, $sb, 512)
    if ($sb.ToString() -match '(\d+)\s*FPS') { return [int]$Matches[1] }
    return -1
}

# Marche en maintenant la touche, en relevant le FPS EN COURS DE ROUTE : c'est
# le minimum instantane qui trahirait un accroc a une frontiere, pas la moyenne.
function Walk([string]$name, [int]$sc, [double]$distance) {
    $ms = [int](1000.0 * $distance / $Speed)
    [void][W32T]::PostMessage($hwnd, $WM_KEYDOWN, [IntPtr]0, [IntPtr](1 -bor ($sc -shl 16)))
    $samples = New-Object System.Collections.ArrayList
    $deadline = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $deadline) {
        if ($proc.HasExited) { break }
        $f = Read-Fps
        if ($f -ge 0) { [void]$samples.Add($f) }
        Start-Sleep -Milliseconds 250
    }
    [void][W32T]::PostMessage($hwnd, $WM_KEYUP, [IntPtr]0, [IntPtr](1 -bor ($sc -shl 16) -bor (1 -shl 30) -bor (1 -shl 31)))
    Start-Sleep -Milliseconds 200

    if ($proc.HasExited) {
        Write-Host ("  {0,-22} LE JEU S'EST ARRETE PENDANT LA MARCHE (code {1})" -f $name, $proc.ExitCode)
        return $false
    }
    if ($samples.Count -eq 0) {
        Write-Host ("  {0,-22} aucun releve de FPS" -f $name)
        return $true
    }
    $stats = $samples | Measure-Object -Average -Minimum -Maximum
    Write-Host ("  {0,-22} {1,4:N0} m parcourus  |  FPS moyen {2,4:N0}, min {3,4}, max {4,4}  ({5} releves)" -f `
        $name, $distance, $stats.Average, $stats.Minimum, $stats.Maximum, $samples.Count)
    return $true
}

$ok = $true
Write-Host "Marche (la camera ne tourne pas : W=nord, S=sud, D=est, A=ouest)"
$ok = (Walk "nord (franchit -Z)" $SC.W $Distance) -and $ok
$ok = (Walk "sud  (retour + au-dela)" $SC.S (2 * $Distance)) -and $ok
$ok = (Walk "nord (retour au centre)" $SC.W $Distance) -and $ok
$ok = (Walk "est  (franchit +X)" $SC.D $Distance) -and $ok
$ok = (Walk "ouest(retour + au-dela)" $SC.A (2 * $Distance)) -and $ok
$ok = (Walk "est  (retour au centre)" $SC.D $Distance) -and $ok

if (-not $proc.HasExited) {
    [void][W32T]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $proc.WaitForExit(30000)) {
        Write-Host "[ECHEC] Pas de sortie 30 s apres WM_CLOSE."
        $proc.Kill(); $proc.WaitForExit(); $ok = $false
    }
}
$proc.WaitForExit()
$code = $proc.ExitCode
Start-Sleep -Milliseconds 400
foreach ($h in $handlers) { Unregister-Event -SourceIdentifier $h.Name -ErrorAction SilentlyContinue }

$text = $outSb.ToString() + $errSb.ToString()
$lines = @($text -split "`r?`n" | Where-Object { $_ -ne '' })
$validation = @($lines | Where-Object { $_ -match '\[Validation\]' })
$errors     = @($lines | Where-Object { $_ -match '\[ERROR\]|\[WARN\]' })

Write-Host ("TRAVERSEE validation    : {0} message(s) [Validation], {1} [ERROR]/[WARN]" -f `
    $validation.Count, $errors.Count)
foreach ($l in ($validation + $errors) | Select-Object -First 10) { Write-Host "   $l" }
Write-Host ("TRAVERSEE code de sortie: {0}" -f $code)

if (-not $ok -or $validation.Count -gt 0 -or $errors.Count -gt 0 -or $code -ne 0) {
    Write-Host "TRAVERSEE : ECHEC"
    exit 1
}
Write-Host "TRAVERSEE : OK - aucune anomalie en franchissant les frontieres."
exit 0
