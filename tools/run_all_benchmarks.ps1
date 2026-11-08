$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$runner = Join-Path $root "tools\bench_runner.ps1"

$scenarios = @(
    @{
        Name = "Survol 900 m/s (Stress Streaming 25 km2)"
        Log = "build\bench_survol.log"
        Args = "assets/terrain/vercora_zone_1m.vchm --flythrough=900,120 --time-scale=0 --freeze-anim --lock-input --hour=13 --quit-after=30"
    },
    @{
        Name = "Panorama Sommet (Vue Longue Portee)"
        Log = "build\bench_panorama.log"
        Args = "assets/terrain/vercora_zone_1m.vchm --start=900175,6438524 --look=45,-6 --time-scale=0 --freeze-anim --lock-input --hour=13 --quit-after=30"
    },
    @{
        Name = "Foret Dense au Sol (Vegetation, Rochers & Faune)"
        Log = "build\bench_foret.log"
        Args = "assets/terrain/vercora_zone_1m.vchm --start=901800,6435800 --look=0,-8 --time-scale=0 --freeze-anim --lock-input --hour=13 --quit-after=30"
    }
)

foreach ($sc in $scenarios) {
    Write-Host "=========================================================="
    Write-Host "Lancement du scenario : $($sc.Name)"
    Write-Host "=========================================================="
    & powershell -ExecutionPolicy Bypass -File $runner -ArgsString $sc.Args -LogFile $sc.Log
    Start-Sleep -Seconds 1
}

Write-Host "Tous les benchmarks sont termines avec succes !"
