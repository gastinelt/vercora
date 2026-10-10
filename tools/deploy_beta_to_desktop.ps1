$dest = "C:\Users\theog\Desktop\Vercora.Beta"
if (-not (Test-Path $dest)) {
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
}

Copy-Item -Path "build\vercora.exe" -Destination $dest -Force
Copy-Item -Path "dist\VercoraDemo\*.dll" -Destination $dest -Force
Copy-Item -Path "build\shaders" -Destination $dest -Recurse -Force
Copy-Item -Path "dist\VercoraDemo\assets" -Destination $dest -Recurse -Force

$batContent = @"
@echo off
cd /d "%~dp0"
start "" "vercora.exe" --demo
"@
Set-Content -Path "$dest\Lancer_Demo.bat" -Value $batContent -Encoding ASCII

$readmeContent = @"
Vercora - DEMO (Exploration & Vol libre)
========================================

Pour lancer la demo : double-cliquez sur Lancer_Demo.bat ou vercora.exe.

Commandes :
- Clic droit maintenu + Souris : Regarder autour de soi / orienter la vue
- Touches Z, Q, S, D (ou W, A, S, D) : Se deplacer
- Touche F : Basculer entre Marche et Vol libre
- Touche O : Basculer entre l'orthophoto aerienne et la coloration procedurale
- Fermer : Croix de la fenetre ou Alt+F4

Cette version demo se concentre sur l'exploration du massif (25 km2 LiDAR)
en marche et vol libre, sans les activites complexes ni survie.
"@
Set-Content -Path "$dest\LisezMoi.txt" -Value $readmeContent -Encoding UTF8

Write-Host "Deploiement termine dans $dest :"
Get-ChildItem -Path $dest
