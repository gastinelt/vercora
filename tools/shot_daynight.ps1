# ===========================================================================
# PHASE 23 - Captures du cycle jour/nuit et du contraste adret/ubac.
# ===========================================================================
# Produit une SERIE d'images de la MEME vue a des heures de jeu differentes.
# C'est la verification visuelle du cycle : la direction et l'intensite de la
# lumiere doivent suivre l'heure, et rien d'autre ne doit bouger.
#
# CE QUI REND CES CAPTURES COMPARABLES, et c'est tout l'interet du script :
#
#   --lock-input    (pose par screenshot_scene.ps1) fige la camera. Sans lui,
#                   deux executions du meme script cadrent deux choses
#                   differentes - constate en Phase 11, 65 % des pixels.
#   --look=         epingle le cadrage. --lock-input SEUL NE SUFFIT PAS : trois
#                   captures de la meme scene ont donne 38,2 / 38,3 / 32,9 % de
#                   ciel, donc trois vues. Lecon de la Phase 12.
#   --start=        epingle la position.
#   --hour=         epingle l'instant de jeu (Phase 23).
#   --time-scale=0  GELE l'horloge, pour que les secondes de mise en place de la
#                   capture ne deplacent pas le soleil. C'est le pendant exact
#                   de --lock-input pour le TEMPS, et sans lui le cycle
#                   jour/nuit rendait toute capture non reproductible.
#
# Le systeme meteo est deterministe a seed fixe (voir world/Weather.hpp), donc
# deux executions de ce script a la meme heure doivent rendre la MEME image, au
# bruit de rasterisation pres.
#
# Usage :
#   powershell -File tools\shot_daynight.ps1
#   powershell -File tools\shot_daynight.ps1 -Hours 6,12,18,22 -OutDir build\cycle
# ===========================================================================

param(
    # Heures de jeu a capturer. Par defaut : nuit, aube, matin, midi, soir,
    # crepuscule - les six moments ou la lumiere est franchement differente.
    [double[]]$Hours = @(3, 6.5, 9, 13, 18, 21),

    [string]$OutDir = "build\daynight",

    # Vue de reference. Position et cadrage EPINGLES, voir l'en-tete.
    # Par defaut : l'alpage de la Phase 18 (E 900500 / N 6439000), qui est la
    # seule vue du projet dont on sache ce qu'elle contient (124 touffes
    # d'herbe comptees), donc la seule ou une difference se lise.
    [string]$Start = "900500,6439000",
    [string]$Look = "-90,-8",

    [string]$Scene = "assets\terrain\vercora_zone_1m.vchm",

    [int]$SettleSeconds = 8
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$shot = Join-Path $PSScriptRoot "screenshot_scene.ps1"
$scenePath = Join-Path $root $Scene

if (-not (Test-Path $scenePath)) {
    Write-Host "[ERREUR] Scene introuvable : $scenePath"
    Write-Host "         Produire la zone assemblee, ou passer -Scene."
    exit 1
}

$outRoot = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

# Un vercora.exe orphelin fabrique de faux plantages ET de faux ralentissements
# (lecon de la Phase 19). On nettoie avant, pas apres.
$stale = Get-Process -Name "vercora" -ErrorAction SilentlyContinue
if ($stale) {
    Write-Host "[ATTENTION] $($stale.Count) vercora.exe orphelin(s) : arret avant capture."
    $stale | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

Write-Host ""
Write-Host "Cycle jour/nuit : $($Hours.Count) captures de la meme vue"
Write-Host "  position $Start, cadrage $Look, horloge GELEE"
Write-Host ""

foreach ($hour in $Hours) {
    # Nom de fichier trie a l'oeil ET par le systeme : "h06.5" plutot que "6.5".
    $label = ("h{0:00.0}" -f $hour).Replace(",", ".")
    $out = Join-Path $outRoot "$label.png"

    Write-Host "--- $label ---"
    # Appel EN PROCESSUS (& $shot) et non `powershell -File $shot`. La raison
    # n'est pas cosmetique : `-File` aplatit les arguments en ligne de commande,
    # et PowerShell relit alors "--look=-90,-8" comme un NOM DE PARAMETRE a cause
    # de son tiret initial. En processus, le tableau reste un tableau et chaque
    # element reste une chaine.
    & $shot `
        -Scene $scenePath `
        -Out $out `
        -SettleSeconds $SettleSeconds `
        -GameArgs @("--start=$Start", "--look=$Look",
                    ("--hour={0}" -f $hour.ToString([System.Globalization.CultureInfo]::InvariantCulture)),
                    "--time-scale=0")

    if (-not (Test-Path $out)) {
        Write-Host "[ECHEC] pas d'image produite pour $label"
        exit 1
    }
}

Write-Host ""
Write-Host "[OK] $($Hours.Count) captures dans $outRoot"
Write-Host "     Les comparer a l'oeil : seule la LUMIERE doit changer d'une"
Write-Host "     image a l'autre. Un relief, une vegetation ou un cadrage qui"
Write-Host "     bougent signalent une capture non epinglee, pas un cycle."
