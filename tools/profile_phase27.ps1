# ===========================================================================
# PHASE 27 - Mesure de cout ENTRELACEE, avec temoin de sante machine.
# ===========================================================================
# Ce script applique les trois lecons de mesure du projet, et n'existe que pour
# ne plus avoir a s'en souvenir a la main :
#
#   1. Conditions ENTRELACEES, jamais en blocs (MEMORY : "Mesurer en blocs
#      fabrique un effet"). Chaque tour joue A puis B, le tour suivant B puis A :
#      une derive lente de la machine (thermique, tache de fond) se repartit sur
#      les deux conditions au lieu de s'imputer a l'une.
#   2. Un POSTE TEMOIN qui ne peut pas avoir bouge : le semis de l'herbe
#      (gatherGrass, CPU). Aucune des deux conditions ne touche au CPU de
#      l'herbe ; s'il varie de plus de 15 % entre A et B, c'est la machine qui a
#      change, et la comparaison est declaree SUSPECTE.
#   3. SANTE MACHINE par run : le temps CPU consomme par TOUS LES AUTRES
#      processus pendant le run, et le nombre d'images rendues. Un run ou les
#      autres processus ont pris plus d'UN coeur en moyenne, ou qui a
#      rendu moins de 20 images/s, est NON CONCLUANT - jamais vert (MEMORY :
#      "Temoin de sante machine dans le harnais").
#
# Les conditions sont des PAIRES A/B dont B est le temoin ETROIT de ce que la
# Phase 27 a ajoute (--no-weather-visuals, --no-ground-detail) : la difference
# A - B est le cout de cet ajout et de rien d'autre.
#
# Usage :
#   powershell -File tools\profile_phase27.ps1 -Scenario pluie -Rounds 3 -Config Release
# Scenarios : survol, pluie, sol, beau.
# ===========================================================================

param(
    [Parameter(Mandatory = $true)][ValidateSet("survol", "survolbeau", "pluie", "sol", "marche", "beau")][string]$Scenario,
    [int]$Rounds = 3,
    [int]$Seconds = 25,
    [string]$Config = "?",
    [string]$Scene = "assets\terrain\vercora_zone_1m.vchm",
    [string]$OutCsv = "build\phase27\profil.csv"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\vercora.exe"
$scenePath = Join-Path $root $Scene
$csv = Join-Path $root $OutCsv
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $csv) | Out-Null

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W32M {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@

$alpage = @("--start=900500,6439000", "--time-scale=0", "--lock-input")
switch ($Scenario) {
    # Survol de streaming, comme l'audit (900 m/s, 120 m), un jour de PLUIE :
    # tout le contenu de la Phase 27 qui peut couter y est actif. B = Phase 26.
    "survol" { $common = @("--flythrough=900", "--day=8", "--hour=13", "--time-scale=0", "--lock-input")
               $A = @(); $B = @("--no-weather-visuals", "--no-ground-detail") }
    # Meme survol par BEAU temps : separe le cout de la pluie (absente ici) de
    # celui des shaders modifies (brouillard nul, micro-relief hors portee).
    "survolbeau" { $common = @("--flythrough=900", "--day=12", "--hour=13", "--time-scale=0", "--lock-input")
               $A = @(); $B = @("--no-weather-visuals", "--no-ground-detail") }
    # Vue fixe sous la pluie : cout de la meteo visible (passe de pluie plein
    # ecran + brouillard + sol mouille).
    "pluie"  { $common = $alpage + @("--look=-90,-8", "--day=8", "--hour=13")
               $A = @(); $B = @("--no-weather-visuals") }
    # Vue fixe au ras du sol : cout du micro-relief, la ou il couvre le plus
    # de pixels.
    "sol"    { $common = $alpage + @("--look=-90,-50", "--day=12", "--hour=13")
               $A = @(); $B = @("--no-ground-detail") }
    # Vue de MARCHE (regard a l'horizon, -8 deg) : cout REALISTE du
    # micro-relief, qui n'y couvre que le bas de l'ecran.
    "marche" { $common = $alpage + @("--look=-90,-8", "--day=12", "--hour=13")
               $A = @(); $B = @("--no-ground-detail") }
    # Vue fixe par beau temps : la meteo visible doit y couter ~0 (pas de
    # pluie, pas de brouillard). Controle negatif de la mesure "pluie".
    "beau"   { $common = $alpage + @("--look=-90,-8", "--day=12", "--hour=13")
               $A = @(); $B = @("--no-weather-visuals") }
}

$cores = [Environment]::ProcessorCount

# Charge totale de la machine, en % de TOUS les coeurs (compteur formate de
# Windows, deja moyenne sur l'intervalle d'echantillonnage).
#
# PREMIERE VERSION ABANDONNEE, et la raison vaut d'etre ecrite : elle sommait
# TotalProcessorTime sur Get-Process au debut et a la fin du run. Un processus
# NE pendant le run y entre avec toute sa vie, un processus qui refuse l'acces
# au debut et l'accorde a la fin aussi : l'essai a rendu "318 910 % d'un
# coeur". Un temoin de sante qui ment est pire que pas de temoin.
function Get-TotalLoadPercent {
    return [double](Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
}

function Invoke-Run([string]$label, [string[]]$extra) {
    $stale = Get-Process -Name "vercora" -ErrorAction SilentlyContinue
    if ($stale) { $stale | Stop-Process -Force; Start-Sleep -Milliseconds 800 }

    $log = Join-Path (Split-Path -Parent $csv) ("run_{0}_{1}_{2}.log" -f $Scenario, $label, [DateTime]::Now.ToString("HHmmss"))
    $gameArgs = @("`"$scenePath`"") + $common + @($extra | Where-Object { $_ })
    $t0 = [DateTime]::Now
    $proc = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $root `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 900 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        $proc.Refresh()
        if ($proc.HasExited) { break }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle }
        Start-Sleep -Milliseconds 100
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw "pas de fenetre ($label)" }
    # Sante machine : charge totale echantillonnee pendant le run, moins ce que
    # vercora lui-meme a consomme sur la meme fenetre de temps.
    $proc.Refresh()
    $gameCpu0 = $proc.TotalProcessorTime.TotalSeconds
    $s0 = [DateTime]::Now
    $loads = @()
    while (([DateTime]::Now - $s0).TotalSeconds -lt $Seconds) {
        Start-Sleep -Milliseconds 1500
        $loads += Get-TotalLoadPercent
    }
    $proc.Refresh()
    $sampleWall = ([DateTime]::Now - $s0).TotalSeconds
    $gameCorePct = 100.0 * ($proc.TotalProcessorTime.TotalSeconds - $gameCpu0) / $sampleWall
    $totalCorePct = ($loads | Measure-Object -Average).Average * $cores
    $others = [math]::Max(0.0, $totalCorePct - $gameCorePct)   # en % d'UN coeur
    [void][W32M]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $proc.WaitForExit(30000)) { $proc.Kill(); $proc.WaitForExit() }

    $text = Get-Content $log -Raw
    $cost = [regex]::Match($text, "Cout par image \(PHASE 19\) : (\d+) images - streaming terrain ([\d.]+) us, arbres ([\d.]+) us, herbe ([\d.]+) us, plantes a fibres ([\d.]+) us, tronc de vision ([\d.]+) us, drawFrame ([\d.]+) us, visuels meteo ([\d.]+) us")
    $cad = [regex]::Match($text, "Cadence de boucle \(PHASE 27\) : (\d+) images en ([\d.]+) s, soit ([\d.]+) images/s, ([\d.]+) us par image")
    $validation = ([regex]::Matches($text, "\[Validation\]")).Count
    if (-not $cost.Success -or -not $cad.Success) { throw "bilan absent du journal ($label) : $log" }

    $fps = [double]$cad.Groups[3].Value
    $verdict = "OK"
    # Seuil : un coeur entier (100 %) sur les 20 de la machine. Un bureau vivant
    # (messagerie, navigateur integre, pilotes) tourne a 25-35 % d'un coeur au
    # repos sur ce poste - mesure au premier essai de ce script. Un seuil plus
    # bas rejetterait tous les runs sans rien proteger ; au-dela d'un coeur, une
    # vraie tache concurrente (compilation, indexation, un vercora orphelin)
    # est en cours et le run n'est pas comparable.
    if ($others -gt 100.0) { $verdict = "NON CONCLUANT (autres processus {0:N0} % d'un coeur)" -f $others }
    elseif ($fps -lt 20.0) { $verdict = "NON CONCLUANT (n'a pas pu rendre : {0:N1} img/s)" -f $fps }

    $row = [pscustomobject]@{
        Config = $Config; Scenario = $Scenario; Condition = $label
        ImgParS = $fps; UsParImage = [double]$cad.Groups[4].Value
        Streaming = [double]$cost.Groups[2].Value; Arbres = [double]$cost.Groups[3].Value
        Herbe = [double]$cost.Groups[4].Value; Fibres = [double]$cost.Groups[5].Value
        Frustum = [double]$cost.Groups[6].Value; DrawFrame = [double]$cost.Groups[7].Value
        MeteoCpu = [double]$cost.Groups[8].Value
        AutresPctCoeur = [math]::Round($others, 1); Validation = $validation; Verdict = $verdict
    }
    $row | Export-Csv -Path $csv -Append -NoTypeInformation -Encoding UTF8
    Write-Host ("{0,-8} {1,-2} {2,7:N1} img/s  {3,8:N1} us/img  drawFrame {4,8:N1}  herbe {5,6:N1}  meteoCPU {6,5:N2}  autres {7,5:N1}%  valid {8}  {9}" -f `
        $Scenario, $label, $fps, $row.UsParImage, $row.DrawFrame, $row.Herbe, $row.MeteoCpu, $others, $validation, $verdict)
    return $row
}

Write-Host "=== $Scenario ($Config) : $Rounds tours A/B entrelaces, $Seconds s par run, $cores coeurs logiques ==="
$rows = @()
for ($r = 1; $r -le $Rounds; $r++) {
    $order = if ($r % 2 -eq 1) { @("A", "B") } else { @("B", "A") }
    foreach ($c in $order) {
        $extra = if ($c -eq "A") { $A } else { $B }
        $rows += Invoke-Run $c $extra
    }
}

$ok = $rows | Where-Object { $_.Verdict -eq "OK" }
$a = $ok | Where-Object { $_.Condition -eq "A" }
$b = $ok | Where-Object { $_.Condition -eq "B" }
function Med($xs) { $s = @($xs | Sort-Object); if ($s.Count -eq 0) { return [double]::NaN }; return $s[[int][math]::Floor(($s.Count - 1) / 2)] }
if ($a.Count -gt 0 -and $b.Count -gt 0) {
    $usA = Med ($a | ForEach-Object { $_.UsParImage }); $usB = Med ($b | ForEach-Object { $_.UsParImage })
    $hA = Med ($a | ForEach-Object { $_.Herbe }); $hB = Med ($b | ForEach-Object { $_.Herbe })
    $temoin = if ($hB -gt 0) { [math]::Abs($hA - $hB) / $hB } else { 0 }
    Write-Host ("MEDIANES  A {0:N1} us/img   B {1:N1} us/img   A-B = {2:N1} us/img ({3:+0.0;-0.0} %)" -f $usA, $usB, ($usA - $usB), (100 * ($usA - $usB) / $usB))
    Write-Host ("POSTE TEMOIN (herbe, CPU)  A {0:N1} us  B {1:N1} us  ecart {2:N1} %  -> {3}" -f $hA, $hB, (100 * $temoin), $(if ($temoin -gt 0.15) { "SUSPECT : la machine a change entre A et B" } else { "stable" }))
} else {
    Write-Host "PAS DE CONCLUSION : pas assez de runs OK dans les deux conditions."
}
if (($rows | Measure-Object -Property Validation -Sum).Sum -gt 0) { Write-Host "[ECHEC] messages de validation Vulkan presents"; exit 3 }
