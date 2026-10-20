param(
    [Parameter(Mandatory = $true)][string]$ArgsString,
    [Parameter(Mandatory = $true)][string]$LogFile,
    [string]$TargetDesktop = "Bench Vercora"
)

$root = "C:\Users\theog\Documents\_CODE\4-vercors\vercora"
$exe = Join-Path $root "build\vercora.exe"
$vd = Join-Path $root "tools\VirtualDesktop.exe"
$logPath = Join-Path $root $LogFile
$errPath = "$logPath.err"

# Verifier ou recuperer l'indice du bureau actuel
# Par defaut, le bureau 0 est le bureau principal
$origDesktop = 0

Write-Host "1. Bascule vers le bureau virtuel '$TargetDesktop'..."
& $vd /Switch:$TargetDesktop
Start-Sleep -Milliseconds 150

Write-Host "2. Demarrage de Vercora..."
$proc = Start-Process -FilePath $exe -ArgumentList $ArgsString -WorkingDirectory $root `
    -RedirectStandardOutput $logPath -RedirectStandardError $errPath -PassThru

# Attendre que la fenetre soit initialisee (environ 800ms) pour qu'elle s'ancre sur ce bureau
Start-Sleep -Milliseconds 900

Write-Host "3. Retour immediat vers le bureau d'origine ($origDesktop)..."
& $vd /Switch:$origDesktop

Write-Host "4. Benchmark en cours sur '$TargetDesktop' (PID $($proc.Id))..."
$proc.WaitForExit()
Write-Host "Benchmark termine avec le code de sortie : $($proc.ExitCode)."
