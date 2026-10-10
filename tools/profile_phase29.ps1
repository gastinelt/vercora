# ===========================================================================
# PHASE 29 (Agent 2) - cout des rochers : A = jeu normal, B = --no-rocks.
# Derive de profile_phase27.ps1 (memes lecons : A/B ENTRELACES, sante machine),
# avec deux differences :
#   - arret UNIQUEMENT par PID exact (WM_CLOSE puis Kill du PID lance) ; aucun
#     Stop-Process par nom, et refus de demarrer si un vercora tourne deja ;
#   - RAM : working set et pic de working set du processus, lus juste avant la
#     fermeture.
# Usage : powershell -File tools\profile_phase29.ps1 -View alpage -Rounds 3
# Vues : alpage (zone de reference de la Phase 27, 900500,6439000, regard -8)
#        pierrier (903452,6439601 : echantillon 4 de NOTES-PHASE29-DESIGN.md)
# ===========================================================================
param(
    [ValidateSet("alpage", "pierrier", "buissons")][string]$View = "alpage",
    # Agent 3 : option temoin de la condition B (--no-rocks par defaut, --no-bushes pour les buissons).
    [string]$Toggle = "--no-rocks",
    [int]$Rounds = 3,
    [int]$Seconds = 25,
    [string]$OutCsv = "build\phase29\profil.csv"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\vercora.exe"
$scenePath = Join-Path $root "assets\terrain\vercora_zone_1m.vchm"
$csv = Join-Path $root $OutCsv
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $csv) | Out-Null
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W32P29 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$start = switch ($View) { "alpage" { "--start=900500,6439000" } "pierrier" { "--start=903452,6439601" } "buissons" { "--start=901160,6436275" } }
$common = @($start, "--look=-90,-8", "--day=12", "--hour=13", "--time-scale=0", "--lock-input")
$cores = [Environment]::ProcessorCount

function Invoke-Run([string]$label, [string[]]$extra) {
    if (Get-Process -Name "vercora" -ErrorAction SilentlyContinue) { throw "un vercora tourne deja : refus de mesurer" }
    $log = Join-Path (Split-Path -Parent $csv) ("run_{0}_{1}_{2}.log" -f $View, $label, [DateTime]::Now.ToString("HHmmss"))
    $gameArgs = @("`"$scenePath`"") + $common + @($extra | Where-Object { $_ })
    $proc = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $root `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 900 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        $proc.Refresh(); if ($proc.HasExited) { break }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle }
        Start-Sleep -Milliseconds 100
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw "pas de fenetre ($label)" }
    $proc.Refresh(); $cpu0 = $proc.TotalProcessorTime.TotalSeconds; $s0 = [DateTime]::Now
    $loads = @()
    while (([DateTime]::Now - $s0).TotalSeconds -lt $Seconds) {
        Start-Sleep -Milliseconds 1500
        $loads += [double](Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
    }
    $proc.Refresh()
    $wall = ([DateTime]::Now - $s0).TotalSeconds
    $others = [math]::Max(0.0, ($loads | Measure-Object -Average).Average * $cores - 100.0 * ($proc.TotalProcessorTime.TotalSeconds - $cpu0) / $wall)
    $wsMb = $proc.WorkingSet64 / 1MB; $peakMb = $proc.PeakWorkingSet64 / 1MB; $privMb = $proc.PrivateMemorySize64 / 1MB
    [void][W32P29]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $proc.WaitForExit(30000)) { Stop-Process -Id $proc.Id -Force; $proc.WaitForExit() }
    $text = Get-Content $log -Raw
    $cad = [regex]::Match($text, "Cadence de boucle \(PHASE 27\) : (\d+) images en ([\d.]+) s, soit ([\d.]+) images/s, ([\d.]+) us par image")
    $rk = [regex]::Match($text, "semis ([\d.]+) us par image ; derniere image (\d+) gros blocs, (\d+) rochers, (\d+) plaques")
    $bu = [regex]::Match($text, "Buissons \(PHASE 29\) : semis ([\d.]+) us par image ; derniere image (\d+) buissons, (\d+) petits arbres")
    $validation = ([regex]::Matches($text, "\[Validation\]")).Count
    if (-not $cad.Success) { throw "bilan absent ($label) : $log" }
    $fps = [double]$cad.Groups[3].Value
    $verdict = if ($others -gt 100.0) { "NON CONCLUANT (autres {0:N0} %)" -f $others } else { "OK" }
    $row = [pscustomobject]@{
        View = $View; Condition = $label; ImgParS = $fps; UsParImage = [double]$cad.Groups[4].Value
        RochersUs = $(if ($rk.Success) { [double]$rk.Groups[1].Value } else { 0 })
        Blocs = $(if ($rk.Success) { [int]$rk.Groups[2].Value } else { 0 })
        Rochers = $(if ($rk.Success) { [int]$rk.Groups[3].Value } else { 0 })
        Plaques = $(if ($rk.Success) { [int]$rk.Groups[4].Value } else { 0 })
        WsMo = [math]::Round($wsMb, 1); PicMo = [math]::Round($peakMb, 1); PriveMo = [math]::Round($privMb, 1)
        BuissonsUs = $(if ($bu.Success) { [double]$bu.Groups[1].Value } else { 0 })
        Buissons = $(if ($bu.Success) { [int]$bu.Groups[2].Value } else { 0 })
        PetitsArbres = $(if ($bu.Success) { [int]$bu.Groups[3].Value } else { 0 })
        AutresPctCoeur = [math]::Round($others, 1); Validation = $validation; Verdict = $verdict
    }
    $row | Export-Csv -Path $csv -Append -NoTypeInformation -Encoding UTF8
    if ($bu.Success) { Write-Host ("          buissons {0:N1} us, {1} buissons, {2} petits arbres" -f $row.BuissonsUs, $row.Buissons, $row.PetitsArbres) }
    Write-Host ("{0,-8} {1} {2,7:N1} img/s {3,8:N1} us/img  rochers {4,6:N1} us ({5}/{6}/{7})  WS {8:N0} Mo pic {9:N0}  autres {10:N0}%  valid {11}  {12}" -f `
        $View, $label, $fps, $row.UsParImage, $row.RochersUs, $row.Blocs, $row.Rochers, $row.Plaques, $wsMb, $peakMb, $others, $validation, $verdict)
    return $row
}

$rows = @()
for ($r = 1; $r -le $Rounds; $r++) {
    $order = if ($r % 2 -eq 1) { @("A", "B") } else { @("B", "A") }
    foreach ($c in $order) { $rows += Invoke-Run $c $(if ($c -eq "A") { @() } else { @($Toggle) }) }
}
function Med($xs) { $s = @($xs | Sort-Object); if ($s.Count -eq 0) { return [double]::NaN }; return $s[[int][math]::Floor(($s.Count - 1) / 2)] }
$ok = $rows | Where-Object { $_.Verdict -eq "OK" }
foreach ($c in "A", "B") {
    $x = $ok | Where-Object { $_.Condition -eq $c }
    Write-Host ("MEDIANE {0} : {1:N1} img/s, {2:N1} us/img, WS {3:N0} Mo, pic {4:N0} Mo  (min/max img/s {5:N1}/{6:N1})" -f $c,
        (Med ($x | % { $_.ImgParS })), (Med ($x | % { $_.UsParImage })), (Med ($x | % { $_.WsMo })), (Med ($x | % { $_.PicMo })),
        (($x | Measure-Object ImgParS -Minimum).Minimum), (($x | Measure-Object ImgParS -Maximum).Maximum))
}
if (($rows | Measure-Object -Property Validation -Sum).Sum -gt 0) { Write-Host "[ECHEC] messages de validation Vulkan"; exit 3 }
