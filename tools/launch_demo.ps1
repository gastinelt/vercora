# Script pour lancer Vercora et amener la fenetre au premier plan
$buildDir = "$PSScriptRoot\..\build"
$exePath = "$buildDir\vercora.exe"

# Arreter toute instance preexistante
Get-Process -Name vercora -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

# Demarrer le processus
$p = Start-Process -FilePath $exePath -ArgumentList "--demo" -WorkingDirectory "$PSScriptRoot\.." -PassThru

Write-Host "Vercora lance avec le PID $($p.Id)"
