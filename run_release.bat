@echo off
cd /d "%~dp0"
echo Lancement de Vercora Release (zone 25 km2, activites completes : escalade, velo, parapente, faune, meteo)...
start "" "build-release\vercora.exe" assets\terrain\vercora_zone_1m.vchm
