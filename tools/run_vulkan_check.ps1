# ===========================================================================
# Verification du VRAI executable Vulkan et de ses couches de validation.
# ===========================================================================
# Lance build\vercora.exe (build Debug => VERCORA_DEBUG => couche
# VK_LAYER_KHRONOS_validation active), pilote sa fenetre par l'API Win32, puis
# analyse la sortie a la recherche de messages "[Validation]".
#
# Dix scenarios (A a J), dans QUATORZE PROCESSUS distincts (G en compte trois,
# J en compte trois) :
#   A. Gameplay complet sur le terrain SYNTHETIQUE : pierres, percussion,
#      abattage, ramassage, puis resize, minimisation et fermeture propre.
#   B. Sonde de collision seule, depuis l'origine, terrain synthetique.
#   C. Dalle LiDAR HD REELLE (Phase 7) : 2 millions de triangles, marche sur le
#      relief mesure, abattage et ramassage, resize et fermeture propre.
#   D. Vol en parapente sur la dalle reelle (Phase 8).
#   E. Sortie a VELO sur la dalle reelle (Phase 13) : descente puis montee sur
#      un versant mesure, avec freinage et demi-tour.
#   F. ESCALADE sur une paroi mesuree (Phase 14).
#   G. EDITEUR DE NIVEAU (Phase 15), en TROIS processus : edition (pose,
#      selection, reglage, suppression, annulation, sauvegarde, rechargement),
#      puis relecture de la scene dans une partie NORMALE, puis un TEMOIN qui
#      refait exactement la meme marche sans la scene. C'est l'ecart entre les
#      deux dernieres qui prouve que les objets poses bloquent le joueur.
#   J. RECOLTE DE MATIERES (Phase 22), en TROIS processus : creuser un
#      gisement detecte par le jeu lui-meme, ramasser la motte produite, puis
#      reessayer a 30 m - le meme test de portee dans les deux sens que le
#      scenario H ; puis, sur la zone de 25 km2 (la seule qui ait une carte de
#      couvert), arracher une plante a fibres poussee EN LISIERE et verifier
#      qu'elle est en REPOUSSE et non disparue.
#   H. BESOINS PRIMAIRES (Phase 16) : depart sur un point d'eau detecte par le
#      jeu lui-meme, deux touches T (a portee puis a 30 m) qui exercent le test
#      de portee dans les deux sens, puis 100 s de marche et 60 s d'immobilite -
#      la telemetrie doit montrer la forme descendre PUIS remonter.
#
# Le scenario B est separe parce que la position du joueur ne se pilote qu'a
# l'estime : chaque collision rencontree en chemin decale la camera par rapport
# au modele du script, et cette derive s'accumule. En repartant d'un processus
# neuf, la position de depart est connue EXACTEMENT (l'origine), ce qui rend la
# sonde decisive au lieu d'ambigue.
#
# Le scenario C est separe pour une autre raison : le terrain synthetique reste
# le monde de REFERENCE des scenarios A et B (semis deterministe, positions
# connues d'avance), et il faut pouvoir prouver qu'il n'a pas regresse. Le
# scenario C ajoute ce que lui seul peut montrer : un maillage 60 fois plus gros
# passe sous les couches de validation, un relief de 562 m de denivele sous les
# pieds, et un semis d'arbres refait sur ce relief. Il est ignore, sans faire
# echouer le script, si la dalle n'a pas ete produite (elle n'est pas versionnee).
#
# Pilotage par PostMessage plutot que par automatisation clavier/souris globale :
# les evenements vont DIRECTEMENT a la fenetre visee, sans dependre du focus et
# sans voler la souris a l'utilisateur.
#
# Le monde etant seme de facon DETERMINISTE (graines fixes), "vercora.exe
# --dump-world" donne a l'avance la position des arbres et des pierres. Le script
# vise donc de vraies cibles au lieu d'appuyer sur les touches a l'aveugle :
# c'est la difference entre "j'ai appuye sur R" et "j'ai ramasse une buche".
#
# Ce dump etait un executable separe (tools\dump_world.cpp) jusqu'a la Phase 7 ;
# c'est desormais un MODE du jeu, donc le meme code exactement - voir le
# commentaire de dumpWorld() dans src\main.cpp.
#
# La FERMETURE est un vrai WM_CLOSE (ce que fait la croix de la fenetre) et non
# un kill : c'est la seule facon de verifier la destruction propre des objets
# Vulkan, precisement ce que les couches de validation signalent si elle est
# incorrecte.
#
# Usage : powershell -ExecutionPolicy Bypass -File tools\run_vulkan_check.ps1
# Code de retour : 0 si tout est vert.
# ===========================================================================

# -Only permet de ne lancer qu'une partie des scenarios :
#     powershell -File tools\run_vulkan_check.ps1 -Only G
#     powershell -File tools\run_vulkan_check.ps1 -Only A,B
#
# Ajoute en Phase 15 pour une raison tres concrete : la suite complete prend une
# quinzaine de minutes, et mettre au point UN scenario en payant les sept autres a
# chaque essai decourage de le faire correctement. Le verdict final reste
# calcule sur les seuls scenarios joues - un scenario non joue est traite comme
# une dalle absente, c'est-a-dire ignore et non echoue. Un run de reference se
# fait donc TOUJOURS sans -Only.
param([string[]]$Only = @())

$ErrorActionPreference = "Stop"

$runAll = ($Only.Count -eq 0)
function Should-Run([string]$name) { return $runAll -or ($Only -contains $name) }
if (-not $runAll) {
    Write-Host "ATTENTION : execution PARTIELLE ($($Only -join ', ')). Les scenarios"
    Write-Host "            non joues sont ignores, pas valides. Pour un verdict de"
    Write-Host "            reference, relancer sans -Only."
}

$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "build\vercora.exe"
if (-not (Test-Path $exe)) { throw "vercora.exe introuvable : $exe. Lancer tools\build.bat d'abord." }

# --- Monde attendu ---------------------------------------------------------
# Le dump rejoue le semis avec la MEME heightmap que le jeu - et pour cause,
# c'est le jeu. Indispensable depuis la Phase 7 : le filtre de pente rejette des
# emplacements differents selon le relief, donc les arbres ne tombent pas au meme
# endroit sur la dalle reelle que sur le terrain synthetique.
#
# Renvoie les objets du monde ET les positions de tir. Une ligne
# "AIM i camX camZ recul pitchDeg elevation" signifie : depuis (camX, camZ), en
# visant le tronc de l'arbre i avec le pitch indique (pitch inclus, pas
# seulement un rayon horizontal - corrige en Phase 8), Forest::pickTree designe
# REELLEMENT l'arbre i. C'est la fonction de visee du jeu qui l'a dit, pas une
# estimation du script.
function Get-World([string]$terrainArg = "") {
    $lines = if ($terrainArg -ne "") { & $exe --dump-world $terrainArg 2>$null }
             else { & $exe --dump-world 2>$null }
    $entries = @()
    $aims = @()
    $levelAims = @()
    $lines | ForEach-Object {
        $p = $_ -split '\s+'
        if ($p.Count -eq 7 -and $p[0] -eq 'AIM') {
            $aims += [pscustomobject]@{
                Index = [int]$p[1]; CamX = [double]$p[2]; CamZ = [double]$p[3]
                Standoff = [double]$p[4]; PitchDeg = [double]$p[5]; Elevation = $p[6]
                CamDist = [Math]::Sqrt([double]$p[2] * [double]$p[2] + [double]$p[3] * [double]$p[3])
            }
        } elseif ($p.Count -eq 5 -and $p[0] -eq 'AIMLEVEL') {
            $levelAims += [pscustomobject]@{
                Index = [int]$p[1]; CamX = [double]$p[2]; CamZ = [double]$p[3]
                Standoff = [double]$p[4]
                CamDist = [Math]::Sqrt([double]$p[2] * [double]$p[2] + [double]$p[3] * [double]$p[3])
            }
        } elseif ($p.Count -eq 5 -and $p[0] -ne '[INFO]') {
            $entries += [pscustomobject]@{
                Kind = $p[0]; X = [double]$p[1]; Z = [double]$p[2]
                Dist = [double]$p[3]; Bearing = [double]$p[4]
            }
        }
    }
    return [pscustomobject]@{ Objects = $entries; Aims = $aims; LevelAims = $levelAims }
}

# Les arbres sont nommes TREE<indice> : l'indice est celui que le jeu
# journalise, ce qui permet de verifier apres coup qu'on a abattu l'arbre VISE
# et non un autre - le controle le plus direct de la precision du pilotage.
# "terrain" explicite : sans argument, le jeu charge desormais la dalle LiDAR.
$world  = (Get-World "terrain").Objects
$trees  = @($world | Where-Object { $_.Kind -like 'TREE*' })
$stones = @($world | Where-Object { $_.Kind -eq 'pierre' })
Write-Host "Monde synthetique attendu : $($trees.Count) arbres, $($stones.Count) pierres."

# Dalle LiDAR reelle : celle que le jeu charge par defaut (cf. main.cpp).
$lidarTile = Join-Path $root "assets\terrain\LHD_0907_6440_1m.vchm"

# PHASE 22. La zone de 25 km2 est la SEULE dont la carte de couvert LiDAR soit
# produite, et c'est cette carte qui porte la voie "lisiere" du semis de plantes
# a fibres. Elle n'est pas versionnee : son absence fait sauter J3 sans faire
# echouer le script, exactement comme l'absence de la dalle fait sauter C a I.
$zoneTile = Join-Path $root "assets\terrain\vercora_zone_2m.vchm"
$hasZone = Test-Path $zoneTile
$hasLidar = Test-Path $lidarTile
if ($hasLidar) {
    $lidarDump  = Get-World $lidarTile
    $lidarTrees = @($lidarDump.Objects | Where-Object { $_.Kind -like 'TREE*' })

    # Preuve de la Phase 8 : avec un rayon de visee pitch-inclus (au lieu de
    # l'ancien rayon horizontal), Forest::pickTree retrouve les arbres en
    # contrebas ET en amont, pas seulement ceux au niveau de l'oeil.
    $above = @($lidarDump.Aims | Where-Object { $_.Elevation -eq 'au-dessus' })
    $below = @($lidarDump.Aims | Where-Object { $_.Elevation -eq 'en-dessous' })
    $level = @($lidarDump.Aims | Where-Object { $_.Elevation -eq 'au-niveau' })
    Write-Host ("Dalle LiDAR trouvee : {0} arbres semes sur le relief reel, {1} abattables au total " -f
                $lidarTrees.Count, $lidarDump.Aims.Count) -NoNewline
    Write-Host ("(niveau={0}, au-dessus={1}, en-dessous={2})." -f $level.Count, $above.Count, $below.Count)

    # Le tir REEL de ce script, lui, ne peut viser qu'au pitch nul : la camera ne
    # tourne jamais pendant ce process (voir plus bas, "Navigation : uniquement
    # au clavier") puisque GLFW lit le curseur SYSTEME et non les messages
    # postes - tourner la camera volerait la souris de l'utilisateur. On utilise
    # donc AIMLEVEL (rayon horizontal, comme avant la Phase 8) pour choisir la
    # cible du tir reellement joue, et non les entrees AIM ci-dessus - celles-ci
    # ne servent qu'a PROUVER que pickTree sait viser en hauteur.
    $lidarAim = @($lidarDump.LevelAims | Sort-Object CamDist) | Select-Object -First 1
    if ($null -eq $lidarAim) {
        Write-Host "[ATTENTION] Aucun arbre abattable au pitch nul : scenario C ignore le tir reel."
    }
} else {
    Write-Host "Dalle LiDAR absente ($lidarTile) : scenario C ignore."
    Write-Host "  Pour la produire : python tools/lidar_to_heightmap.py data/lidar/<dalle>.copc.laz -o `"$lidarTile`" --cell 1.0"
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W32 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
}
"@

# ===========================================================================
# COLLECTEUR DE JOURNAL : un delegue .NET COMPILE, pas un script-block
# ===========================================================================
# Jusqu'a la Phase 22, la sortie du jeu etait capturee par
# `Register-ObjectEvent -Action { ... }`, c'est-a-dire UN SCRIPT-BLOCK
# POWERSHELL EXECUTE PAR LIGNE DE JOURNAL. Ces blocs ne tournent que quand le
# runspace est disponible ; or les scenarios passent leur temps dans des
# `Start-Sleep` et des boucles d'attente. Les evenements s'empilent, et le
# tampon que `Wait-ForLog` interroge est EN RETARD sur ce que le jeu a
# reellement ecrit. Le retard CROIT avec la duree du scenario.
#
# Ce n'etait pas une hypothese : mesure faite le 2026-08-27, meme binaire, meme
# dalle, meme sortie a velo, seul le collecteur changeant -
#
#   collecteur                     "Velo : en selle"   "Velo : pied a terre"
#   Register-ObjectEvent -Action        1833 ms      jamais vu en 3 s (9,6 s)
#   delegue .NET (ci-dessous)              5 ms                      0 ms
#
# C'est TOUTE l'explication de la dette du scenario E, ouverte en Phase 17 et
# rejouee a chaque phase depuis : le jeu journalisait bien "Velo : pied a
# terre." - la ligne EST dans $runE.Lines apres coup - mais elle n'arrivait au
# script qu'apres l'expiration des 3 s de Wait-ForLog. LE JEU N'A JAMAIS EU DE
# DEFAUT ; c'est l'instrument de mesure qui regardait trop tot.
#
# Nota : "en selle" tenait dans son budget a 1833 ms, soit 61 % des 3 s. Le
# defaut menacait donc TOUS les Wait-ForLog du harnais, pas seulement celui du
# velo ; le scenario E etait simplement le plus long, donc le premier a franchir
# le seuil. Elargir le delai aurait masque cela au lieu de le resoudre.
#
# Le puits se branche LUI-MEME (Attach) : PowerShell 5.1 ne sait pas convertir
# une PSMethod en DataReceivedEventHandler, le delegue est donc construit du
# cote C#. ToString() est surcharge pour que les appelants (Wait-ForLog,
# Get-BikeHeading, Wait-BikeSample, Stop-Game, scenario F) restent INCHANGES :
# tous appelaient deja .ToString() sur le StringBuilder d'avant.
Add-Type @"
using System;
using System.Text;
using System.Diagnostics;
public class VercoraLogSink {
  private readonly StringBuilder sb = new StringBuilder();
  private readonly object gate = new object();
  private void OnData(object sender, DataReceivedEventArgs e) {
    if (e.Data == null) return;
    lock (gate) { sb.AppendLine(e.Data); }
  }
  public void Attach(Process p, bool standardOutput) {
    if (standardOutput) { p.OutputDataReceived += new DataReceivedEventHandler(OnData); }
    else                { p.ErrorDataReceived  += new DataReceivedEventHandler(OnData); }
  }
  public override string ToString() { lock (gate) { return sb.ToString(); } }
}
"@

$WM_KEYDOWN=0x0100; $WM_KEYUP=0x0101; $WM_CLOSE=0x0010
$SWP_NOMOVE=0x0002; $SWP_NOZORDER=0x0004

# --- Touches designees par leur SCANCODE, jamais par leur code virtuel -------
# Piege qui a coute cher ici : MapVirtualKeyW(VK, MAPVK_VK_TO_VSC) traduit selon
# la DISPOSITION CLAVIER ACTIVE. Sur l'AZERTY francais de cette machine, VK_W
# (0x57) designe la touche gravee W, qui occupe la position physique du Z d'un
# QWERTY. GLFW, lui, raisonne en positions physiques : il recevait donc
# GLFW_KEY_Z quand on croyait envoyer W, et le jeu - qui teste GLFW_KEY_W -
# ignorait purement et simplement la touche.
#
# Effet observe avant correction : seuls S et D (memes positions sur AZERTY et
# QWERTY) deplacaient la camera. Le script croyait aller a l'ouest, la camera
# derivait vers l'est, et aucun objet ne se trouvait jamais a portee - alors que
# le JEU, lui, fonctionnait parfaitement.
#
# En envoyant directement les scancodes du jeu de codes 1, on s'adresse a la
# position physique, exactement comme le fera un vrai clavier.
$SC = @{
    W=0x11; A=0x1E; S=0x1F; D=0x20   # ZQSD sous les doigts d'un AZERTY
    E=0x12; R=0x13; C=0x2E; F=0x21
    V=0x2F; SPACE=0x39               # Phase 8 : decollage/atterrissage parapente
    B=0x30                           # Phase 13 : monter sur le velo / en descendre
    X=0x2D                           # Phase 14 : s'engager sur une paroi / lacher
    T=0x14                           # Phase 16 : boire au point d'eau
    Y=0x15                           # Phase 22 : creuser un gisement
    I=0x17                           # Phase 22 : arracher une plante a fibres
}

# --- PHASE 15 : les touches de l'EDITEUR, dont des touches ETENDUES ---------
# Le pave de navigation (INSER, SUPPR, DEBUT, FIN, PAGE) et les fleches portent
# les MEMES scancodes que le pave numerique ; ce qui les distingue est le bit
# "touche etendue" (bit 24 du lParam). GLFW indexe sa table par
# (scancode | 0x100) pour celles-la - voir createKeyTables() dans win32_init.c -
# donc oublier ce bit ne produit pas une touche morte : il produit une AUTRE
# touche (INSER deviendrait le 0 du pave numerique). D'ou le drapeau explicite
# porte par chaque entree.
$SCX = @{
    INSERT    = 0x52   # poser l'objet arme (equivalent clavier du clic gauche)
    DELETE    = 0x53   # supprimer la selection
    UP        = 0x48   # agrandir
    DOWN      = 0x50   # reduire
    LEFT      = 0x4B   # tourner a gauche
    RIGHT     = 0x4D   # tourner a droite
    TAB       = 0x0F   # asset suivant
    BACKSPACE = 0x0E   # annuler
    F2        = 0x3C   # entrer / sortir du mode editeur
    F3        = 0x3D   # objet pose suivant, sans viser
    F4        = 0x3E   # objet pose precedent, sans viser
    F5        = 0x3F   # sauvegarder la scene
    F9        = 0x43   # recharger la scene
    L         = 0x26   # reafficher le catalogue
    E         = 0x12   # selectionner l'objet vise
    D3        = 0x04   # rang 3 du catalogue
    W         = 0x11   # avancer (Z en AZERTY)
}

# Send-Key ne sait pas poser le bit "touche etendue" : ces deux fonctions le
# font. Elles ne REMPLACENT pas Send-Key/HoldMs, volontairement - les scenarios
# A a F sont verts avec, et leur toucher serait un risque pour zero gain.
function Send-KeyEx([int]$sc, [bool]$ext, [int]$holdMs = 90) {
    $down = 1 -bor ($sc -shl 16)
    if ($ext) { $down = $down -bor (1 -shl 24) }
    $up = $down -bor (1 -shl 30) -bor (1 -shl 31)
    [void][W32]::PostMessage($script:hwnd, $WM_KEYDOWN, [IntPtr]0, [IntPtr]$down)
    Start-Sleep -Milliseconds $holdMs
    [void][W32]::PostMessage($script:hwnd, $WM_KEYUP, [IntPtr]0, [IntPtr]$up)
    Start-Sleep -Milliseconds 160
}

function HoldKeyEx([int]$sc, [bool]$ext, [int]$ms) {
    $down = 1 -bor ($sc -shl 16)
    if ($ext) { $down = $down -bor (1 -shl 24) }
    $up = $down -bor (1 -shl 30) -bor (1 -shl 31)
    [void][W32]::PostMessage($script:hwnd, $WM_KEYDOWN, [IntPtr]0, [IntPtr]$down)
    Start-Sleep -Milliseconds $ms
    [void][W32]::PostMessage($script:hwnd, $WM_KEYUP, [IntPtr]0, [IntPtr]$up)
    Start-Sleep -Milliseconds 250
}

# --- Navigation : uniquement au clavier, JAMAIS a la souris -----------------
# La camera ne tourne pas de tout le test, et c'est delibere :
#
#   GLFW ne lit PAS la position du curseur dans les messages WM_MOUSEMOVE : sa
#   fonction glfwGetCursorPos appelle GetCursorPos() puis ScreenToClient(),
#   c'est-a-dire le curseur SYSTEME reel. Des WM_MOUSEMOVE postes n'ont donc
#   aucun effet sur la rotation (constate : la camera restait figee au cap
#   initial pendant tout un run). Faire tourner la camera exigerait de deplacer
#   reellement le pointeur avec SetCursorPos, donc de voler la souris a
#   l'utilisateur pendant une minute.
#
#   Ce n'est pas necessaire : au cap initial (yaw = -90 degres), le repere de la
#   camera est aligne sur les axes du monde. front = (0, 0, -1) et
#   right = (1, 0, 0), donc
#        W -> -Z      S -> +Z      D -> +X      A -> -X
#   On atteint donc n'importe quel point (x, z) par translations pures.
$script:posX = 0.0
$script:posZ = 0.0
$script:hwnd = [IntPtr]::Zero
$script:proc = $null
$script:outSb = $null
$script:errSb = $null
$script:handlers = @()

function Send-Key([int]$sc, [int]$holdMs = 70) {
    [void][W32]::PostMessage($script:hwnd, $WM_KEYDOWN, [IntPtr]0, [IntPtr](1 -bor ($sc -shl 16)))
    Start-Sleep -Milliseconds $holdMs
    [void][W32]::PostMessage($script:hwnd, $WM_KEYUP, [IntPtr]0, [IntPtr](1 -bor ($sc -shl 16) -bor (1 -shl 30) -bor (1 -shl 31)))
    Start-Sleep -Milliseconds 90
}

# Maintient une touche le temps de parcourir la distance voulue.
# Vitesse 14 unites/s (Camera::setMoveSpeed dans main.cpp), deplacement
# strictement horizontal en mode marche : distance = vitesse x temps, quel que
# soit le relief.
function Hold([int]$sc, [double]$distance) {
    if ($distance -le 0.02) { return }
    $ms = [int](1000.0 * $distance / 14.0)
    if ($ms -lt 20) { return }
    [void][W32]::PostMessage($script:hwnd, $WM_KEYDOWN, [IntPtr]0, [IntPtr](1 -bor ($sc -shl 16)))
    Start-Sleep -Milliseconds $ms
    [void][W32]::PostMessage($script:hwnd, $WM_KEYUP, [IntPtr]0, [IntPtr](1 -bor ($sc -shl 16) -bor (1 -shl 30) -bor (1 -shl 31)))
    Start-Sleep -Milliseconds 120
}

# Maintient une touche pendant une DUREE fixe plutot qu'une distance - utilise
# pour le decollage (monter en mode Free n'a pas de "distance" a viser, juste
# le temps de depasser le seuil de hauteur) et pour piloter le parapente une fois
# en vol (virage, frein), ou la notion de distance ne s'applique pas non plus.
function HoldMs([int]$sc, [int]$ms) {
    [void][W32]::PostMessage($script:hwnd, $WM_KEYDOWN, [IntPtr]0, [IntPtr](1 -bor ($sc -shl 16)))
    Start-Sleep -Milliseconds $ms
    [void][W32]::PostMessage($script:hwnd, $WM_KEYUP, [IntPtr]0, [IntPtr](1 -bor ($sc -shl 16) -bor (1 -shl 30) -bor (1 -shl 31)))
    Start-Sleep -Milliseconds 90
}

# Maintient PLUSIEURS touches simultanement pendant une duree fixe. Necessaire
# des la Phase 13 : le demi-tour du velo se fait frein tire (sinon la machine
# repart dans la pente pendant qu'on la tourne), donc S et D en meme temps. Les
# helpers precedents ne savaient tenir qu'une touche.
function HoldMsMulti([int[]]$scancodes, [int]$ms) {
    foreach ($sc in $scancodes) {
        [void][W32]::PostMessage($script:hwnd, $WM_KEYDOWN, [IntPtr]0, [IntPtr](1 -bor ($sc -shl 16)))
    }
    Start-Sleep -Milliseconds $ms
    foreach ($sc in $scancodes) {
        [void][W32]::PostMessage($script:hwnd, $WM_KEYUP, [IntPtr]0, [IntPtr](1 -bor ($sc -shl 16) -bor (1 -shl 30) -bor (1 -shl 31)))
    }
    Start-Sleep -Milliseconds 120
}

# Dernier cap du velo lu dans la telemetrie du jeu (degres), ou $null si le velo
# n'a encore rien emis.
function Get-BikeHeading() {
    $ms = [regex]::Matches($script:outSb.ToString(), 'Velo : .*cap ([-0-9.]+) deg')
    if ($ms.Count -eq 0) { return $null }
    return [double]$ms[$ms.Count - 1].Groups[1].Value
}

# Attend qu'une NOUVELLE ligne de telemetrie velo soit apparue (le jeu en emet
# une par seconde). Renvoie $false en cas de delai depasse.
function Wait-BikeSample([int]$timeoutMs = 2000) {
    $before = [regex]::Matches($script:outSb.ToString(), 'Velo : .*km/h').Count
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        if ([regex]::Matches($script:outSb.ToString(), 'Velo : .*km/h').Count -gt $before) { return $true }
        Start-Sleep -Milliseconds 120
    }
    return $false
}

# Amene le velo au cap voulu, PAR ASSERVISSEMENT sur ce que le jeu journalise -
# et non par une duree calculee a l'avance.
#
# Pourquoi : un demi-tour "1,4 s a 140 deg/s" a ete essaye d'abord, et il rate.
# Le temps reellement vu par le jeu n'est pas celui du Start-Sleep (jitter de
# PostMessage, ordonnancement, frame en cours), et sur ce versant l'erreur coute
# cher : le devers moyen y depasse 25 %, donc 18 degres d'ecart font passer la
# pente ressentie de +12 % a +27 % - au-dela de ce que le velo peut monter. Le
# velo s'immobilisait alors pour de bon et le scenario ne prouvait aucune montee.
#
# Le jeu emet deja son cap chaque seconde : on s'en sert. On tourne par courtes
# rafales, frein tire (sinon la machine repart dans la pente pendant la
# manoeuvre), en relisant le cap entre chaque.
function Turn-BikeTo([double]$targetDeg, [double]$toleranceDeg = 8.0,
                     [int]$maxBursts = 14) {
    for ($i = 0; $i -lt $maxBursts; $i++) {
        $cap = Get-BikeHeading
        if ($null -eq $cap) { return $false }
        # Ecart ramene dans (-180, 180] : on tourne toujours par le plus court.
        $delta = (($targetDeg - $cap) % 360 + 540) % 360 - 180
        if ([Math]::Abs($delta) -le $toleranceDeg) {
            Write-Host ("    cap {0:N1} deg (cible {1:N0}, ecart {2:N1}) - atteint en {3} rafales" -f $cap, $targetDeg, $delta, $i)
            return $true
        }
        # 140 deg/s a l'arret (plafond de braquage de world/Bike.hpp). On vise
        # 70 % de l'ecart par rafale : on converge sans osciller.
        $ms = [int][Math]::Min(500, [Math]::Max(60, [Math]::Abs($delta) * 0.7 / 140.0 * 1000.0))
        $key = $(if ($delta -gt 0) { $SC.D } else { $SC.A })
        HoldMsMulti @($SC.S, $key) $ms
        [void](Wait-BikeSample 2000)
    }
    Write-Host ("    [ATTENTION] cap non atteint apres {0} rafales (dernier {1})" -f $maxBursts, (Get-BikeHeading))
    return $false
}

# Interroge la sortie DEJA CAPTUREE du processus en cours (les gestionnaires
# asynchrones de Start-Game l'alimentent en continu) jusqu'a ce qu'une ligne
# corresponde au motif, ou jusqu'au delai imparti. Sert a savoir si le vol a
# reellement decolle/atterri SANS deviner un temps d'attente fixe - le relief
# reel determine seul quand le parapente touche le sol.
function Wait-ForLog([string]$pattern, [int]$timeoutMs) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        if ($script:outSb.ToString() -match $pattern) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $script:outSb.ToString() -match $pattern
}

# ===========================================================================
# NAVIGATION EN BOUCLE FERMEE (Phase 24)
# ===========================================================================
# Jusqu'ici GoTo naviguait A L'ESTIME : il tenait une touche pendant
# distance / 14 m/s et DECLARAIT le joueur arrive, sans jamais rien verifier.
# Le modele est faux des que le monde s'en mele - collision contre un tronc,
# terrain qui monte, et depuis la Phase 24 une pente qui ralentit la marche.
# L'erreur ne se corrige jamais : elle s'ACCUMULE d'un GoTo au suivant.
#
# Ce n'etait pas une hypothese. Mesure du 2026-08-27, scenario A, journal brut
# du jeu : le script se croyait sur la pierre 2, le joueur etait a 7 m de la,
# et le jeu disait exactement pourquoi -
#
#   [INFO]  Ramassage : rien a portee de main.        (x 24)
#   [INFO]  Percussion impossible : il faut deux pierres, un percuteur...
#   [INFO]  Abattage : aucun arbre dans la ligne de visee.
#
# TOUT le scenario A tombe d'une seule cause : le joueur n'arrive pas. Les
# "percussions 0" et "cible manquee" en sont les consequences, pas des defauts
# distincts - et RIEN de tout cela n'est du jeu. Le controle a ete fait dans les
# deux sens : neutraliser entierement la limite de pente de la Phase 24
# (kWalkMaxSlope a 1000) ne change RIEN au resultat, et le run ci-dessus n'a
# journalise AUCUN declenchement du filtre. Trois executions donnent le meme
# echec au detail pres : ce defaut est DETERMINISTE, il n'a jamais ete de la
# flakiness, et c'est pour ca qu'elargir des delais ne l'a jamais corrige.
#
# Le correctif ne touche pas au jeu (contrainte explicite du chantier) : il
# lit ce que le jeu JOURNALISE DEJA. src/main.cpp emet la position de la camera
# UNE FOIS PAR SECONDE, sans condition, depuis la Phase 15 - et pour exactement
# cette raison, ecrite dans son commentaire. Le harnais ne s'en etait jamais
# servi pour naviguer.
#
# Position REELLE du joueur, telle que le jeu vient de l'ecrire. Renvoie $null
# si aucune ligne fraiche n'arrive dans le delai - l'appelant retombe alors sur
# l'estime plutot que d'echouer.
#
# On exige une ligne PLUS RECENTE que celles deja presentes : relire la derniere
# ligne connue donnerait la position d'AVANT le deplacement qu'on vient
# d'ordonner, et la boucle croirait n'avoir pas bouge.
function Get-PlayerPos([int]$timeoutMs = 2500) {
    $rx = 'Camera : yaw \S+ pitch \S+ position (\S+) (\S+) (\S+)'
    $seen = ([regex]::Matches($script:outSb.ToString(), $rx)).Count
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $m = [regex]::Matches($script:outSb.ToString(), $rx)
        if ($m.Count -gt $seen) {
            $last = $m[$m.Count - 1]
            return [pscustomobject]@{
                X = [double]$last.Groups[1].Value
                Y = [double]$last.Groups[2].Value
                Z = [double]$last.Groups[3].Value
            }
        }
        Start-Sleep -Milliseconds 120
    }
    return $null
}

# Se rend en (x, z) et VERIFIE l'arrivee, en corrigeant l'erreur restante.
#
# $tolerance = 0,6 m : nettement sous la portee de ramassage (2,5 m) et sous la
# demi-largeur du joueur plus une marge, donc "arrive" veut dire quelque chose.
# $maxPasses borne le temps : un joueur reellement bloque par un rocher ne doit
# pas faire tourner le harnais en rond - on sort et le scenario constatera le
# blocage, ce qui est parfois PRECISEMENT ce qu'il teste (scenario B, G2).
function GoTo([double]$x, [double]$z, [double]$tolerance = 0.6, [int]$maxPasses = 4) {
    for ($pass = 0; $pass -lt $maxPasses; $pass++) {
        $p = Get-PlayerPos
        if ($null -eq $p) {
            # Repli EXACT sur l'ancien comportement : mieux vaut l'estime que
            # rien, et ca garde le harnais utilisable si le jeu cesse un jour
            # d'emettre cette ligne.
            $dx = $x - $script:posX
            if ($dx -ge 0) { Hold $SC.D $dx } else { Hold $SC.A (-$dx) }
            $script:posX = $x
            $dz = $z - $script:posZ
            if ($dz -le 0) { Hold $SC.W (-$dz) } else { Hold $SC.S $dz }
            $script:posZ = $z
            return
        }

        $script:posX = $p.X
        $script:posZ = $p.Z
        $dx = $x - $p.X
        $dz = $z - $p.Z
        $remaining = [Math]::Sqrt($dx * $dx + $dz * $dz)
        if ($remaining -le $tolerance) { return }

        if ($dx -ge 0) { Hold $SC.D $dx } else { Hold $SC.A (-$dx) }
        if ($dz -le 0) { Hold $SC.W (-$dz) } else { Hold $SC.S $dz }
    }

    # Derniere lecture : que la position estimee reflete la REALITE meme quand
    # on renonce, sinon l'erreur repartirait dans le GoTo suivant - ce qui est
    # exactement le defaut qu'on corrige.
    $final = Get-PlayerPos
    if ($null -ne $final) {
        $script:posX = $final.X
        $script:posZ = $final.Z
        $miss = [Math]::Sqrt(($x - $final.X) * ($x - $final.X) + ($z - $final.Z) * ($z - $final.Z))
        if ($miss -gt $tolerance) {
            Write-Host ("    [nav] arrivee a {0:N2} m de la cible ({1:N1}, {2:N1}) apres {3} passes" -f $miss, $x, $z, $maxPasses)
        }
    }
}

# Abat l'arbre vise, en REESSAYANT apres un petit pas de cote.
#
# POURQUOI UN RETRY LA OU UN SEUL "E" SUFFISAIT EN THEORIE. La navigation en
# boucle fermee (voir GoTo) plafonne a ~0,8 m de la cible et n'y descend pas :
# le canal d'entree est quantifie - `Hold` refuse tout pas sous 0,28 m, et
# `Start-Sleep` a une granularite de ~16 ms sur cette machine, donc une
# correction fine est soit ignoree soit depassee. Ce n'est pas un reglage a
# ajuster, c'est une limite du pilotage par PostMessage.
#
# Or la marge de visee du scenario C est plus fine que ce plafond, et ca s'est
# vu : deux executions consecutives du MEME binaire ont donne 0,78 m (arbre
# abattu) et 0,80 m (manque). Une reussite qui tient a deux centimetres de
# residu n'est pas une reussite - c'est la flakiness qu'on cherche a eliminer.
#
# Le remede est celui que le harnais emploie DEJA pour le meme probleme au
# ramassage (Vacuum) : couvrir l'incertitude de position par un petit balayage,
# au lieu d'exiger du canal d'entree une precision qu'il n'a pas.
function Fell-WithRetry([int]$attempts = 4, [double]$step = 0.5) {
    # Comptage direct plutot que Count-Match : cet utilitaire n'est defini
    # qu'apres les scenarios dans ce fichier, donc il n'existe pas encore ici.
    $rx = "Abattage de l'arbre"
    $before = ([regex]::Matches($script:outSb.ToString(), $rx)).Count
    for ($i = 0; $i -lt $attempts; $i++) {
        Send-Key $SC.E
        Start-Sleep -Milliseconds 600
        $now = ([regex]::Matches($script:outSb.ToString(), $rx)).Count
        if ($now -gt $before) {
            if ($i -gt 0) { Write-Host ("    [visee] arbre abattu au {0}e essai" -f ($i + 1)) }
            return $true
        }
        # Pas de cote ALTERNE (droite, gauche, droite...) et croissant : on
        # balaie autour de la position estimee sans s'en eloigner franchement.
        $key = if ($i % 2 -eq 0) { $SC.D } else { $SC.A }
        Hold $key ($step * ($i + 1))
    }
    Write-Host "    [visee] arbre NON abattu apres $attempts essais"
    return $false
}

# Ratisse autour de la position courante : des R entrecoupes de petits pas en
# carre (deplacement net nul). La portee de ramassage est de 2.5 unites, mais
# la position n'est connue qu'a l'estime ; ce balayage rattrape l'imprecision.
function Vacuum([int]$rounds = 3, [double]$step = 1.6) {
    Send-Key $SC.R
    foreach ($i in 1..$rounds) {
        foreach ($k in @($SC.D, $SC.W, $SC.A, $SC.S)) {
            Hold $k ($step * $i)
            Send-Key $SC.R
        }
    }
}

# $sceneArg : argument passe a vercora.exe. "terrain" force le generateur
# procedural ; un chemin .vchm charge une dalle reelle. Les scenarios A et B le
# passent EXPLICITEMENT, alors que le jeu sans argument charge desormais la dalle
# LiDAR : ils testent le monde de reference, pas le defaut du jour.
# $extraArgs : options passees telles quelles APRES la scene (--start=..., etc.).
# Ajoute en Phase 13 : le scenario velo doit demarrer sur un versant CHOISI, et
# le harnais ne peut pas s'y rendre en marchant (il ne sait pas tourner la
# camera, donc il ne peut se deplacer que sur les axes du monde).
function Start-Game([string]$label, [string]$sceneArg = "terrain",
                    [string[]]$extraArgs = @()) {
    Write-Host ""
    Write-Host "############ $label ############"
    # PHASE 26 : instrumentation du dump. Le journal brut concatenait les
    # quatorze processus SANS separateur : impossible d'attribuer une ligne a un
    # scenario apres coup, donc impossible de dire si une telemetrie manquante
    # manque au scenario I ou ailleurs. On retient le libelle et l'instant de
    # depart ; Stop-Game les rend avec la duree reelle du processus.
    $script:runLabel = $label
    $script:runStartedAt = Get-Date
    # System.Diagnostics.Process directement, et non Start-Process : ce dernier
    # renvoie un objet dont ExitCode reste vide quand les flux sont rediriges,
    # ce qui faisait passer un run parfaitement propre pour un echec.
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = '"' + $sceneArg + '"'
    foreach ($a in $extraArgs) { $psi.Arguments += ' "' + $a + '"' }
    $psi.WorkingDirectory = $root
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $script:proc = [System.Diagnostics.Process]::Start($psi)

    # Lecture asynchrone : un pipe plein bloquerait le jeu au milieu du test.
    # Le collecteur est le delegue COMPILE defini plus haut, et non un
    # script-block PowerShell : c'est ce qui garantit que Wait-ForLog interroge
    # ce que le jeu vient d'ecrire, et non un etat vieux de plusieurs secondes.
    $script:outSb = New-Object VercoraLogSink
    $script:errSb = New-Object VercoraLogSink
    $script:handlers = @()
    $script:outSb.Attach($script:proc, $true)
    $script:errSb.Attach($script:proc, $false)
    $script:proc.BeginOutputReadLine()
    $script:proc.BeginErrorReadLine()

    $script:hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 120 -and $script:hwnd -eq [IntPtr]::Zero; $i++) {
        $script:proc.Refresh()
        if ($script:proc.HasExited) { break }
        if ($script:proc.MainWindowHandle -ne [IntPtr]::Zero) { $script:hwnd = $script:proc.MainWindowHandle }
        Start-Sleep -Milliseconds 100
    }
    if ($script:hwnd -eq [IntPtr]::Zero) { throw "Pas de fenetre (HasExited=$($script:proc.HasExited))." }

    $sb = New-Object System.Text.StringBuilder 512
    [void][W32]::GetWindowTextW($script:hwnd, $sb, 512)
    Write-Host "Fenetre : '$($sb.ToString())'"
    $script:posX = 0.0
    $script:posZ = 0.0
    Start-Sleep -Seconds 2
}

function Stop-Game() {
    Write-Host "--- Fermeture propre (WM_CLOSE) ---"
    [void][W32]::PostMessage($script:hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $script:proc.WaitForExit(20000)) {
        Write-Host "[ECHEC] Pas de sortie 20 s apres WM_CLOSE."
        $script:proc.Kill(); $script:proc.WaitForExit()
    }
    $script:proc.WaitForExit()   # vide les lecteurs asynchrones
    Start-Sleep -Milliseconds 400
    foreach ($h in $script:handlers) { Unregister-Event -SourceIdentifier $h.Name -ErrorAction SilentlyContinue }
    $text = $script:outSb.ToString() + $script:errSb.ToString()
    $lines = @($text -split "`r?`n" | Where-Object { $_ -ne '' })
    $result = [pscustomobject]@{
        ExitCode  = $script:proc.ExitCode
        Lines     = $lines
        Label     = $script:runLabel
        StartedAt = $script:runStartedAt
        # Duree MUR du processus. C'est la grandeur qui permet de distinguer
        # "le jeu a emis moins de lignes" de "le scenario a dure moins
        # longtemps" - deux causes tres differentes d'une telemetrie courte.
        WallSeconds = [Math]::Round(((Get-Date) - $script:runStartedAt).TotalSeconds, 1)
    }
    # L'accumulation se fait ICI, et non en enumerant les $runX a la fin : le
    # scenario J RECOMPOSE $runJ en concatenant ses trois processus, si bien
    # qu'une enumeration des variables compterait J2 et J3 deux fois. Le seul
    # endroit qui voit exactement un processus, une fois, c'est Stop-Game.
    $script:allRuns += $result
    return $result
}

# ===========================================================================
# SCENARIO A : gameplay complet
# ===========================================================================
$script:allRuns = @()
$runA = $null
if (Should-Run 'A') {
Start-Game "SCENARIO A - gameplay, resize, fermeture"

foreach ($n in 0..1) {
    $s = $stones[$n]
    Write-Host ("--- Pierre {0} : ({1:N1}, {2:N1}) ---" -f ($n+1), $s.X, $s.Z)
    GoTo $s.X $s.Z
    Vacuum 3
}

Write-Host "--- Percussion (C) ---"
Send-Key $SC.C
Start-Sleep -Milliseconds 500
Hold $SC.W 1.1     # l'eclat tombe 1.1 unite devant, donc vers -Z au cap initial
Send-Key $SC.R
Start-Sleep -Milliseconds 300

# On se place PLEIN NORD de l'arbre (meme X, Z superieur) : au cap initial la
# camera regarde vers -Z, l'arbre est donc pile dans l'axe de visee.
$best = $null; $bestD = [double]::MaxValue
foreach ($t in $trees) {
    $dd = [Math]::Sqrt(($t.X-$script:posX)*($t.X-$script:posX) + ($t.Z-$script:posZ)*($t.Z-$script:posZ))
    if ($dd -lt $bestD) { $bestD = $dd; $best = $t }
}
$expectedTree = ($best.Kind -replace 'TREE','')
Write-Host ("--- Abattage : {0} en ({1:N1}, {2:N1}) ---" -f $best.Kind, $best.X, $best.Z)
GoTo $best.X ($best.Z + 9.0)
Send-Key $SC.E
Start-Sleep -Milliseconds 400

Write-Host "--- Chute, amortissement, debitage (6 s) ---"
Start-Sleep -Seconds 6

Write-Host "--- Ramassage du bois ---"
GoTo $best.X ($best.Z + 1.5)
Vacuum 3
Vacuum 2 2.4

Write-Host "--- Bascule marche / vol libre ---"
Send-Key $SC.F
Start-Sleep -Milliseconds 400
Send-Key $SC.F
Start-Sleep -Milliseconds 400

Write-Host "--- Redimensionnements (recreation de la swapchain) ---"
foreach ($size in @(@(800,600), @(1600,900), @(640,480), @(1280,720), @(1920,1080), @(1024,768))) {
    [void][W32]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 0, 0, $size[0], $size[1], ($SWP_NOMOVE -bor $SWP_NOZORDER))
    Start-Sleep -Milliseconds 700
}

Write-Host "--- Minimisation / restauration (framebuffer de taille nulle) ---"
[void][W32]::ShowWindow($script:hwnd, 6)
Start-Sleep -Milliseconds 1500
[void][W32]::ShowWindow($script:hwnd, 9)
Start-Sleep -Milliseconds 1500

$runA = Stop-Game
}

# ===========================================================================
# SCENARIO B : chemin de collision, depuis l'origine
# ===========================================================================
# Ce scenario EXERCE la collision a l'execution ; il ne PROUVE pas son
# comportement. La distinction est importante et vaut d'etre expliquee.
#
# Une premiere version de cette sonde concluait "collision defaillante" a tort.
# Le raisonnement etait : on pousse 25 unites dans un arbre qui n'est qu'a 11,
# donc si le tronc arrete le joueur l'arbre reste devant lui et E doit l'abattre.
# C'est faux, parce qu'un collider CYLINDRIQUE ne bloque pas : il REPOUSSE. Le
# joueur legerement decentre est ecarte lateralement, glisse autour du tronc et
# repart de l'autre cote - exactement le comportement voulu (on contourne
# l'arbre, on ne le traverse pas), mais qui laisse l'arbre DERRIERE soi dans les
# deux cas. Le critere ne distinguait donc rien.
#
# Aucun observable externe ne permet de trancher : le jeu n'expose pas la
# position du joueur, et la deflexion laterale (0.66 unite) est plus petite que
# la tolerance de visee (~1.4 unite). La preuve COMPORTEMENTALE de la collision
# - non-penetration, glissement, blocage par la souche, passage trop etroit -
# vient donc du harnais headless (tests\logic_tests.cpp, qui simule la marche
# pas a pas et lit les positions directement).
#
# Ce que ce scenario apporte reellement, et qui n'est verifiable qu'ici : le
# code de collision tourne a chaque frame dans le vrai executable, contre un
# tronc DEBOUT puis contre une SOUCHE, sans erreur de validation ni plantage.
$probe = $trees | Where-Object { $_.Kind -eq 'TREE7' } | Select-Object -First 1
$runB = $null
if (Should-Run 'B') {
Start-Game "SCENARIO B - chemin de collision (tronc debout puis souche)"

Write-Host ("--- Poussee de 25 unites dans l'arbre {0} en ({1:N1}, {2:N1}) ---" -f $probe.Kind, $probe.X, $probe.Z)
GoTo $probe.X 0.0          # alignement lateral exact sur l'axe du tronc
Hold $SC.W 25.0            # bien au-dela : on force le contact et le glissement
Start-Sleep -Milliseconds 400

# Retour au nord de l'arbre, puis abattage : la souche prend alors la place du
# tronc dans le volume de collision (Tree::colliderTopY), et on repousse dedans.
Write-Host "--- Retour, abattage, puis poussee dans la souche ---"
# Hold deplace la camera sans toucher a la position ESTIMEE (seul GoTo la met a
# jour) : le retour doit donc se faire par un Hold symetrique, et non par un
# GoTo qui calculerait un deplacement nul et ne bougerait pas.
Hold $SC.S 25.0
Send-Key $SC.E
Start-Sleep -Seconds 6
Hold $SC.W 25.0
Start-Sleep -Milliseconds 400

$runB = Stop-Game
}

# ===========================================================================
# SCENARIO C : dalle LiDAR HD reelle (Phase 7)
# ===========================================================================
# Ce que ce scenario apporte, et que ni A ni B ne peuvent montrer :
#   - un vertex buffer de 1 002 001 sommets et 2 000 000 de triangles pousse sur
#     le GPU, puis six recreations de swapchain avec ce maillage en place ;
#   - la camera qui marche sur un relief MESURE, avec des pentes reelles
#     (562 m de denivele sur 1 km) la ou le terrain synthetique est lisse ;
#   - le semis d'arbres refait sur ce relief, donc le filtre de pente confronte
#     a de vraies falaises ;
#   - la destruction propre de tout ca.
$runC = $null
if ($hasLidar -and (Should-Run 'C')) {
    Start-Game "SCENARIO C - dalle LiDAR HD reelle, 2 M de triangles" $lidarTile

    # ORDRE IMPORTANT : viser D'ABORD, se promener ENSUITE.
    #
    # La position du joueur n'est connue qu'a l'estime, et la moindre collision
    # en chemin la decale sans que le script puisse s'en apercevoir. Une premiere
    # version de ce scenario commencait par un aller-retour de 30 m dans les
    # quatre directions "pour exercer le relief" : ce trajet traversait le semis
    # d'arbres, chaque tronc deviait la camera de quelques dizaines de
    # centimetres, et l'abattage qui suivait ne visait plus rien. Resultat : un
    # scenario en echec alors que le jeu, lui, se comportait correctement.
    #
    # En partant de l'origine - la seule position connue EXACTEMENT - l'abattage
    # est fiable. La promenade libre vient apres, quand la derive n'a plus
    # d'importance parce que plus rien n'a besoin d'etre vise.
    $expectedTreeC = if ($null -ne $lidarAim) { "$($lidarAim.Index)" } else { $null }
    if ($null -ne $lidarAim) {
        Write-Host ("--- Abattage sur relief reel : arbre {0}, tir depuis ({1:N1}, {2:N1}) a {3:N1} m ---" -f
                    $lidarAim.Index, $lidarAim.CamX, $lidarAim.CamZ, $lidarAim.Standoff)
        GoTo $lidarAim.CamX $lidarAim.CamZ
        [void](Fell-WithRetry)
        Start-Sleep -Seconds 7

        Write-Host "--- Ramassage du bois sur relief reel ---"
        GoTo $lidarAim.CamX ($lidarAim.CamZ - $lidarAim.Standoff + 1.5)
        Vacuum 3
        Vacuum 2 2.4
    }

    Write-Host "--- Marche libre sur le relief mesure, puis vol libre ---"
    # Plus rien a viser a partir d'ici : on peut parcourir de longues distances
    # sur des pentes reelles sans se soucier de la derive. C'est ce qui exerce le
    # suivi de terrain (Camera::snapToTerrain) sur des donnees mesurees, la ou le
    # terrain synthetique n'a que des pentes douces.
    foreach ($key in @($SC.W, $SC.D, $SC.S, $SC.A)) { Hold $key 45.0 }
    Send-Key $SC.F        # vol libre : traverse le relief au lieu de le suivre
    Hold $SC.W 60.0
    Send-Key $SC.F        # retour en marche : la camera se recolle au sol
    Start-Sleep -Milliseconds 600

    Write-Host "--- Redimensionnements avec 2 M de triangles a l'ecran ---"
    foreach ($size in @(@(800,600), @(1600,900), @(640,480), @(1920,1080), @(1280,720), @(1024,768))) {
        [void][W32]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 0, 0, $size[0], $size[1], ($SWP_NOMOVE -bor $SWP_NOZORDER))
        Start-Sleep -Milliseconds 700
    }

    Write-Host "--- Minimisation / restauration ---"
    [void][W32]::ShowWindow($script:hwnd, 6)
    Start-Sleep -Milliseconds 1500
    [void][W32]::ShowWindow($script:hwnd, 9)
    Start-Sleep -Milliseconds 1500

    $runC = Stop-Game
}

# ===========================================================================
# SCENARIO D : vol en parapente sur la dalle LiDAR reelle (Phase 8)
# ===========================================================================
# Ce que ce scenario apporte, et que rien d'autre ne peut montrer : un decollage,
# un vol pilote (virage + frein) et un atterrissage - detecte par le jeu lui-meme
# contre le VRAI relief mesure, pas un plancher plat - sous les couches de
# validation Vulkan, avec la camera en vue chase-cam (mode Glide) plutot qu'en
# premiere personne.
#
# Contrairement aux scenarios precedents, on ne connait PAS a l'avance combien de
# temps le vol va durer : le taux de chute est fixe, mais le relief sous le
# parapente varie (562 m de denivele sur la dalle), donc l'altitude a laquelle il
# rencontre le sol depend d'ou il vole. On POLL la sortie du jeu (Wait-ForLog)
# plutot que de deviner un delai, et on force un atterrissage (touche V une
# seconde fois) si le relief n'a rien rencontre dans un delai large - filet de
# securite, pas le cas attendu.
$runD = $null
$paraglideLaunched = $false
$paraglideEnded = $false
if ($hasLidar -and (Should-Run 'D')) {
    Start-Game "SCENARIO D - vol en parapente, dalle LiDAR reelle" $lidarTile

    Write-Host "--- Passage en mode vol libre, puis prise de hauteur ---"
    Send-Key $SC.F                 # Walk -> Free : espace/shift redeviennent actifs
    HoldMs $SC.SPACE 1500          # ~21 unites de hauteur a 14 u/s : largement au-dessus du seuil de 5 m

    Write-Host "--- Decollage (V) ---"
    Send-Key $SC.V
    $paraglideLaunched = Wait-ForLog "Decollage en parapente" 3000
    Write-Host ("    decolle : {0}" -f $paraglideLaunched)

    if ($paraglideLaunched) {
        Write-Host "--- Vol pilote : virage a droite puis frein ---"
        HoldMs $SC.D 800    # ~20 degres de virage a droite, a vitesse angulaire constante
        HoldMs $SC.S 1000   # frein : vitesse et taux de chute doivent evoluer progressivement

        Write-Host "--- Attente de l'atterrissage (relief reel, duree non fixee a l'avance) ---"
        $landedNaturally = Wait-ForLog "Parapente : atterrissage" 45000
        if (-not $landedNaturally) {
            Write-Host "    pas encore pose apres 45 s : atterrissage force (filet de securite)."
            Send-Key $SC.V
            Wait-ForLog "vol interrompu" 3000 | Out-Null
        }
        $paraglideEnded = Wait-ForLog "(Parapente : atterrissage|vol interrompu)" 500
        Write-Host ("    vol termine : {0} (naturellement : {1})" -f $paraglideEnded, $landedNaturally)
    }

    Write-Host "--- Redimensionnements apres le vol ---"
    foreach ($size in @(@(800,600), @(1600,900), @(1280,720))) {
        [void][W32]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 0, 0, $size[0], $size[1], ($SWP_NOMOVE -bor $SWP_NOZORDER))
        Start-Sleep -Milliseconds 700
    }

    $runD = Stop-Game
}

# ===========================================================================
# SCENARIO E : sortie a velo sur la dalle LiDAR reelle (Phase 13)
# ===========================================================================
# Ce que ce scenario apporte, et que rien d'autre ne peut montrer : la physique
# du velo confrontee a un VERSANT MESURE - une descente reelle puis la remontee
# du meme versant - sous les couches de validation Vulkan, avec la camera en vue
# a la troisieme personne (mode Ride).
#
# LE POINT DE DEPART EST CHOISI, ET C'EST ESSENTIEL. Le harnais ne sait pas
# tourner la camera (GLFW lit le curseur SYSTEME, cf. plus haut), donc le velo
# part forcement au cap initial, yaw = -90 degres, c'est-a-dire plein NORD
# (-Z : le nord DECROIT avec Z, voir Heightmap.hpp). Lance a un endroit
# quelconque, il roulerait sur du plat et le run serait vert sans rien prouver.
#
# LE POINT DE DEPART **ET LES DEUX CAPS** SONT CHOISIS PAR MESURE. C'est ce qui a
# demande le plus de travail dans ce scenario, et voici pourquoi.
#
# Trois versions ont echoue avant celle-ci, chacune pour une raison differente :
#
#  1. Depart quelconque : le velo roulait sur du plat, run vert sans rien montrer.
#  2. Depart sur un versant a -21 % : la descente marchait, mais la remontee
#     depassait la pente maximale franchissable du modele (23,6 %, voir
#     world/Bike.hpp) et le velo s'immobilisait pour de bon. C'est ce run qui a
#     mis au jour le reglage de slopeSampleStep - voir NOTES-PHASE-VELO.md.
#  3. Descendre puis faire demi-tour pour remonter LA MEME LIGNE : piege double.
#     D'une part, plus la descente est longue - et il lui faut de la longueur
#     pour que la vitesse se developpe - plus elle depose le velo bas dans le
#     versant, la ou la pente depasse le franchissable (mesure : 18 s de roue
#     libre = 9,2 m de denivele, trop peu ; 30 s = 24,4 m mais remontee a
#     0,1 km/h). D'autre part, "faire demi-tour" ne redonne PAS la pente inverse :
#     le terrain de l'autre cote du point de depart est un autre terrain. Sur la
#     croupe essayee, le nord descendait a -11 % et le sud descendait AUSSI, a
#     -36 %.
#
# La version retenue evalue donc, en depouillant la dalle, la pente le long de
# CHAQUE cap (differences centrees a +/-2 m, exactement comme le fait Bike), et
# retient un point qui offre les deux a la fois :
#
#   depart E 907875 / N 6439450, altitude 513,2 m
#     MONTEE   cap 220 deg : +14,8 % en moyenne sur 90 m, pire section +16,8 %,
#              donc AUCUNE section infranchissable - le velo monte sans caler ;
#     DESCENTE cap  40 deg : -25,0 % en moyenne sur 170 m, pire section -63 %,
#              franche mais roulable (les parois a -600 % de la dalle ne
#              testeraient que le plafond de vitesse, pas une descente VTT).
#
# La MONTEE se fait en premier : la descente qui suit repasse par le point de
# depart et gagne ainsi le denivele grimpe en elan supplementaire.
#
# La preuve n'est pas "le script a appuye sur les touches" mais la telemetrie que
# le jeu emet chaque seconde (vitesse, pente sous les roues, altitude) : elle est
# analysee plus bas.
$runE = $null
$bikeMounted = $false
$bikeDismounted = $false
if ($hasLidar -and (Should-Run 'E')) {
    Start-Game "SCENARIO E - sortie a velo, versant LiDAR reel" $lidarTile `
               @("--start=907875,6439450")

    Write-Host "--- En selle (B) ---"
    Send-Key $SC.B
    $bikeMounted = Wait-ForLog "Velo : en selle" 3000
    Write-Host ("    en selle : {0}" -f $bikeMounted)

    if ($bikeMounted) {
        Write-Host "--- Cap 220 : face a la montee, asservi sur le cap (frein tire) ---"
        # Tolerance de 8 degres : au-dela, le devers de ce versant ferait sortir
        # la pente ressentie de la fourchette mesuree.
        $bikeTurnedUp = Turn-BikeTo 220.0 8.0
        Write-Host ("    face a la montee : {0}" -f $bikeTurnedUp)

        Write-Host "--- Montee au pedalage (versant a +14,8 %) ---"
        HoldMs $SC.W 20000

        Write-Host "--- Cap 40 : face a la descente, asservi sur le cap ---"
        $bikeTurnedDown = Turn-BikeTo 40.0 8.0
        Write-Host ("    face a la descente : {0}" -f $bikeTurnedDown)

        Write-Host "--- Elan puis longue descente en roue libre ---"
        HoldMs $SC.W 3000       # on s'elance
        Start-Sleep -Seconds 32 # roue libre : c'est la gravite qui travaille, et
                                # il lui faut de la DISTANCE pour se voir.

        Write-Host "--- Freinage manuel jusqu'a l'arret ---"
        HoldMs $SC.S 5000

        Write-Host "--- Pied a terre (B) ---"
        Send-Key $SC.B
        $bikeDismounted = Wait-ForLog "Velo : pied a terre" 3000
        Write-Host ("    pied a terre : {0}" -f $bikeDismounted)
    }

    Write-Host "--- Redimensionnements apres la sortie ---"
    foreach ($size in @(@(800,600), @(1600,900), @(1280,720))) {
        [void][W32]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 0, 0, $size[0], $size[1], ($SWP_NOMOVE -bor $SWP_NOZORDER))
        Start-Sleep -Milliseconds 700
    }

    $runE = Stop-Game
}

# ===========================================================================
# SCENARIO F : escalade sur une paroi LiDAR reelle (Phase 14)
# ===========================================================================
# Ce que ce scenario apporte, et que rien d'autre ne peut montrer : la physique
# de l'escalade confrontee a une PAROI MESUREE, sous les couches de validation
# Vulkan, avec la camera a la premiere personne (mode Climb).
#
# CE SCENARIO EST PLUS SIMPLE QUE LE SCENARIO E, ET CE N'EST PAS UN HASARD.
# Le velo a demande un asservissement de cap (Turn-BikeTo) parce que sa dynamique
# depend de la pente LE LONG DU CAP : mal oriente, il roule sur du plat. Les
# commandes de l'escalade, elles, sont RELATIVES A LA PAROI - monter suit la
# ligne de plus grande pente, traverser suit la ligne de niveau, toutes deux
# imposees par le terrain. L'orientation de la camera n'a donc AUCUNE influence
# sur la progression, et le harnais n'a rien a asservir.
#
# Une seule chose depend encore du cap : trouver la paroi au moment de
# s'engager, puisque tryStart sonde devant le REGARD. Le harnais ne pouvant pas
# tourner la camera (cf. plus haut), le joueur regarde forcement plein NORD
# (yaw -90, front = (0,0,-1)). Le point de depart a donc ete choisi par mesure -
# meme methode que pour le velo, meme outil d'esprit (tools/find_climb_walls.py) :
# on a balaye la dalle a la recherche des points qui reunissent les trois
# conditions du scenario, et il n'y en a que TROIS sur toute la dalle :
#
#   depart marchable + une paroi grimpable a moins de 2 m PLEIN NORD
#   + une ascension qui gagne au moins 15 m
#
#   >> retenu : E 907616 / N 6439312, pente au depart 14 % (on y tient
#      largement debout), paroi a portee plein nord, 41,7 m de gain simule.
#
# Le run enchaine les DEUX sorties d'escalade que le harnais peut atteindre :
#   1. montee, traversee, desescalade, puis LACHAGE volontaire (retour au pied) ;
#   2. re-engagement apres un repos, puis montee continue jusqu'au SOMMET.
#
# La troisieme sortie - la CHUTE par epuisement - n'est PAS exercee ici, et il
# faut le dire : elle demanderait une paroi de plus de 62 m (la hauteur qu'une
# jauge pleine permet), et il n'en existe pas d'abordable plein nord sur cette
# dalle. Elle est couverte par les tests headless, ou elle est verifiee jusqu'a
# la position de repli exacte.
#
# La preuve n'est pas "le script a appuye sur X" mais la telemetrie que le jeu
# emet chaque seconde (energie, pente, altitude, gain), analysee plus bas.
$runF = $null
$climbEngaged = $false
$climbReleased = $false
$climbReEngaged = $false
$climbToppedOut = $false
if ($hasLidar -and (Should-Run 'F')) {
    Start-Game "SCENARIO F - escalade, paroi LiDAR reelle" $lidarTile `
               @("--start=907616,6439312")

    Write-Host "--- Engagement sur la paroi (X) ---"
    Send-Key $SC.X
    $climbEngaged = Wait-ForLog "Escalade : engage sur la paroi" 3000
    Write-Host ("    engage : {0}" -f $climbEngaged)

    if ($climbEngaged) {
        Write-Host "--- Montee (Z) ---"
        HoldMs $SC.W 6000

        Write-Host "--- Traversee vers la droite (D) ---"
        HoldMs $SC.D 3000

        Write-Host "--- Traversee vers la gauche (Q) ---"
        HoldMs $SC.A 3000

        Write-Host "--- Desescalade (S) ---"
        HoldMs $SC.S 3000

        Write-Host "--- Lachage volontaire (X) ---"
        Send-Key $SC.X
        $climbReleased = Wait-ForLog "Escalade : lachee volontairement" 3000
        Write-Host ("    lachee : {0}" -f $climbReleased)

        Write-Host "--- Repos au sol (recuperation de la jauge) ---"
        Start-Sleep -Seconds 12

        Write-Host "--- Re-engagement (X) ---"
        Send-Key $SC.X
        $climbReEngaged = Wait-ForLog "Escalade : engage sur la paroi" 3000
        Write-Host ("    re-engage : {0}" -f $climbReEngaged)

        if ($climbReEngaged) {
            Write-Host "--- Montee continue jusqu'au sommet ---"
            # 41,7 m simules a ~0,73 m/s de gain vertical => ~57 s. On tient la
            # touche jusqu'a ce que le jeu annonce le sommet - le temps vu par le
            # jeu n'est pas celui du script (meme lecon qu'au scenario E).
            #
            # 140 s et non 75. PHASE 17, et c'est une fragilite du SCENARIO mise
            # au jour, pas un defaut du jeu : 75 s ne laissaient que 18 s de marge
            # sur une montee de 57 s. Or la trajectoire du grimpeur depend du pas
            # de temps - a chaque image, la commande est projetee sur la ligne de
            # plus grande pente LOCALE - donc deux cadences d'images differentes
            # ne suivent pas exactement le meme chemin sur la paroi. Il a suffi
            # des 6 % de FPS que coute la faune de la Phase 17 (301 contre 320
            # FPS, mesure) pour que la marge disparaisse : le grimpeur montait
            # bien (41,5 m contre 41,4 m au temoin) mais n'annoncait plus le
            # sommet avant l'echeance. Le harnais sort de la boucle des que le
            # sommet est atteint, donc cette marge ne coute rien quand tout va
            # bien.
            $deadline = (Get-Date).AddSeconds(140)
            while ((Get-Date) -lt $deadline -and -not $climbToppedOut) {
                HoldMs $SC.W 2000
                $climbToppedOut = ($script:outSb.ToString() -match 'Escalade : SOMMET atteint')
            }
            Write-Host ("    sommet atteint : {0}" -f $climbToppedOut)
        }
    }

    Write-Host "--- Redimensionnements apres l'escalade ---"
    foreach ($size in @(@(800,600), @(1600,900), @(1280,720))) {
        [void][W32]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 0, 0, $size[0], $size[1], ($SWP_NOMOVE -bor $SWP_NOZORDER))
        Start-Sleep -Milliseconds 700
    }

    $runF = Stop-Game
}

# ===========================================================================
# SCENARIO G : EDITEUR DE NIVEAU (PHASE 15)
# ===========================================================================
# Trois processus, et il en faut trois : le seul controle qui prouve reellement
# quelque chose sur la collision est un TEMOIN NEGATIF, c'est-a-dire la meme
# marche refaite sans la scene.
#
#   G1  edition : poser, selectionner, regler, supprimer, annuler, sauvegarder,
#       recharger, sortir du mode - puis resize et fermeture propre.
#   G2  jeu NORMAL sur le fichier ecrit par G1 : les objets doivent se charger
#       tout seuls (personne n'a demande l'editeur), etre dessines, et ARRETER
#       le joueur qui marche dessus.
#   G3  temoin : exactement la meme marche que G2, mais --no-scene. C'est
#       l'ecart entre les deux distances parcourues qui fait la preuve, pas la
#       distance de G2 prise isolement.
#
# --- CE QUE LE HARNAIS NE PEUT PAS FAIRE, ET SES CONSEQUENCES ---------------
# Il ne pilote que le CLAVIER (GLFW lit le curseur SYSTEME, cf. l'en-tete de ce
# script). Deux consequences assumees :
#
#   * la POSE se fait a la touche INSER et non au clic. Les deux passent par le
#     meme code - main.cpp combine les deux fronts descendants en un seul
#     `placePressed` - donc le chemin exerce est bien celui du clic ;
#   * la camera de G1 est VERROUILLEE (--lock-input) et ne peut pas tourner : les
#     trois objets se posent donc au MEME point vise, empiles. Ce n'est pas une
#     limite de l'editeur (rien n'interdit d'empiler, d'ailleurs) mais une limite
#     du pilotage, la meme qui avait impose de choisir un point de depart precis
#     aux scenarios E et F.
#
# Les touches etendues (INSER, SUPPR, fleches) exigent le bit 24 du lParam :
# GLFW indexe sa table de scancodes par (scancode | 0x100) pour celles-la, donc
# l'oublier designerait une touche du PAVE NUMERIQUE au lieu de la bonne.
# ===========================================================================

$runG1 = $null; $runG2 = $null; $runG3 = $null
$sceneG = Join-Path $root "build\scenario_g.vcsc"

if ($hasLidar -and (Should-Run 'G')) {
    # On repart d'un fichier ABSENT : le premier lancement doit donc aussi
    # exercer le chemin "aucune scene sur disque", qui est le cas normal d'une
    # premiere partie.
    if (Test-Path $sceneG) { Remove-Item $sceneG -Force }

    $startG = "--start=907500,6439500"
    $lookG  = "--look=-90,-8"

    Start-Game "SCENARIO G1 - editeur : catalogue, pose, reglage, annulation, sauvegarde" `
               $lidarTile @("--editor", "--lock-input", $startG, $lookG,
                            "--scene=$sceneG", "--no-vegetation")

    Write-Host "--- Pose du premier asset (INSER) ---"
    Send-KeyEx $SCX.INSERT $true

    Write-Host "--- Asset suivant (TAB) puis pose ---"
    Send-KeyEx $SCX.TAB $false
    Send-KeyEx $SCX.INSERT $true

    Write-Host "--- Rang 3 au clavier, rotation du pinceau, pose ---"
    Send-KeyEx $SCX.D3 $false
    HoldKeyEx $SCX.RIGHT $true 600
    Send-KeyEx $SCX.INSERT $true

    Write-Host "--- Catalogue (L), selection (E), agrandissement ---"
    Send-KeyEx $SCX.L $false
    Send-KeyEx $SCX.E $false
    HoldKeyEx $SCX.UP $true 400

    # PHASE 15, ajout ergonomique : parcourir les objets poses SANS viser.
    # Deux F3 (avance de deux rangs, boucle si necessaire) puis un F4 (recul
    # d'un rang) exercent le chemin de code sous Vulkan reel, pas seulement en
    # headless - la mecanique elle-meme (ordre, bouclage, ignorance pendant une
    # saisie) est verifiee par tests/logic_tests.cpp. On reselectionne ensuite
    # PAR LA VISEE avant de supprimer, pour ne pas changer quel objet cette
    # suppression retire et laisser intactes les mesures qui suivent.
    Write-Host "--- Cycle de selection sans viser (F3, F3, F4) ---"
    Send-KeyEx $SCX.F3 $false
    Send-KeyEx $SCX.F3 $false
    Send-KeyEx $SCX.F4 $false

    Write-Host "--- Reselection par la visee avant suppression ---"
    Send-KeyEx $SCX.E $false

    Write-Host "--- Suppression (SUPPR) puis annulation (RETOUR ARRIERE) ---"
    Send-KeyEx $SCX.DELETE $true
    Start-Sleep -Milliseconds 300
    Send-KeyEx $SCX.BACKSPACE $false
    Start-Sleep -Milliseconds 300

    Write-Host "--- Sauvegarde (F5) puis rechargement (F9) ---"
    Send-KeyEx $SCX.F5 $false
    Start-Sleep -Milliseconds 600
    Send-KeyEx $SCX.F9 $false
    Start-Sleep -Milliseconds 600

    Write-Host "--- Sortie du mode editeur (F2) ---"
    Send-KeyEx $SCX.F2 $false
    Start-Sleep -Milliseconds 600

    Write-Host "--- Redimensionnements apres l'edition ---"
    foreach ($size in @(@(800,600), @(1600,900), @(1280,720))) {
        [void][W32]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 0, 0, $size[0], $size[1], ($SWP_NOMOVE -bor $SWP_NOZORDER))
        Start-Sleep -Milliseconds 700
    }

    $runG1 = Stop-Game

    # --- G2 : jeu NORMAL, chargement automatique et collision ---------------
    # Aucun --editor : c'est un lancement de partie ordinaire. Les objets doivent
    # s'y charger, s'y dessiner et y bloquer, sinon l'editeur ne serait qu'un
    # outil produisant un fichier que personne ne lit.
    Start-Game "SCENARIO G2 - jeu normal : objets charges et solides" `
               $lidarTile @($startG, $lookG, "--scene=$sceneG", "--no-vegetation")
    Start-Sleep -Milliseconds 1200
    Write-Host "--- Marche plein nord contre les objets poses ---"
    HoldKeyEx $SCX.W $false 1500
    Start-Sleep -Milliseconds 1500
    $runG2 = Stop-Game

    # --- G3 : le TEMOIN -----------------------------------------------------
    # Meme binaire, meme terrain, meme point de depart, meme duree de touche.
    # SEULE la presence de la scene change. Sans ce temoin, "le joueur a avance
    # de 0,7 m" ne prouverait rien : il pourrait tout aussi bien s'etre heurte au
    # relief, ou n'avoir jamais recu la touche.
    Start-Game "SCENARIO G3 - temoin : la MEME marche sans la scene" `
               $lidarTile @($startG, $lookG, "--no-scene", "--no-vegetation")
    Start-Sleep -Milliseconds 1200
    Write-Host "--- Meme marche, sans aucun objet pose ---"
    HoldKeyEx $SCX.W $false 1500
    Start-Sleep -Milliseconds 1500
    $runG3 = Stop-Game
}


# ===========================================================================
# SCENARIO H : besoins primaires (PHASE 16)
# ===========================================================================
# Ce que ce scenario apporte, et que rien d'autre ne peut montrer : les quatre
# jauges qui tournent SOUS VULKAN, avec les couches de validation, sur un relief
# mesure - et surtout la chaine complete du point d'eau, qui n'existe que sur du
# relief reel : detection au chargement, portee, refus a distance.
#
# LE DEPART EST UN POINT D'EAU DETECTE, PAS UN POINT CHOISI A LA MAIN. Le jeu
# journalise au demarrage les creux qu'il a retenus sur la dalle ; le premier
# d'entre eux est en monde (-75, 285), soit E 907425 / N 6439215 en Lambert 93
# (conversion de Heightmap::toProjected : est = 907000 + x + 500, nord =
# 6440000 - z - 500). Si le detecteur change d'avis, ce scenario le dira - la
# premiere touche T repondra "trop loin" au lieu de "pas soif".
#
# CE QUE PROUVE CHAQUE TOUCHE T, ET POURQUOI CES DEUX-LA :
#   * au point d'eau  -> "Boire : soif etanchee au point d'eau (0.000000 m)".
#     Ce message n'est emis QU'APRES le test de portee ET le remplissage
#     effectif de la jauge : le voir prouve les deux d'un coup, et la distance
#     qu'il porte est celle reellement calculee.
#     (Une premiere version attendait ici "Boire : pas soif", en supposant la
#     jauge encore exactement pleine. Elle ne l'est pas : deux secondes de jeu
#     l'ont deja fait descendre de quelques millioniemes, invisibles a
#     l'affichage entier mais suffisants pour que drink() reussisse. Le message
#     attendu ne sortait donc jamais.)
#   * a 30 m de la    -> "le point d'eau le plus proche est a N m".
#     C'est le controle negatif : sans lui, une portee cassee (ou infinie)
#     donnerait exactement le meme premier message.
#
# LE RESTE DU RUN EST UNE MESURE, PAS UNE ANIMATION : ~100 s de marche puis
# ~60 s d'immobilite complete. La telemetrie emise chaque seconde doit montrer
# la forme DESCENDRE pendant la marche et REMONTER pendant l'arret. Un seul des
# deux ne prouverait rien : une jauge qui ne fait que descendre est une jauge
# sans repos, une jauge qui ne fait que monter est une jauge qui ne coute rien.
$runH = $null
$drinkAtSource = $false
$drinkTooFar = $false
if ($hasLidar -and (Should-Run 'H')) {
    Start-Game "SCENARIO H - besoins primaires, point d'eau LiDAR" $lidarTile `
               @("--start=907425,6439215", "--no-vegetation")

    Write-Host "--- Boire AU point d'eau (T) ---"
    Send-Key $SC.T
    $drinkAtSource = Wait-ForLog "Boire : soif etanchee au point d'eau" 3000
    Write-Host ("    a portee du point d'eau : {0}" -f $drinkAtSource)

    Write-Host "--- S'en eloigner, puis boire de nouveau (controle negatif) ---"
    HoldMs $SC.W 2500
    Start-Sleep -Milliseconds 500
    Send-Key $SC.T
    $drinkTooFar = Wait-ForLog "point d'eau le plus proche est a" 3000
    Write-Host ("    refus a distance : {0}" -f $drinkTooFar)

    # Marche continue, en va-et-vient pour rester sur le meme replat : ce qui
    # compte est la DUREE d'effort, pas la distance parcourue. Le va-et-vient
    # evite aussi de finir coince contre un versant raide - un joueur bloque ne
    # se deplace plus, donc ne se fatigue plus (main.cpp lit le deplacement REEL
    # de la camera, pas les touches), et le scenario deviendrait vert sans avoir
    # rien mesure.
    # 160 s et non 100 : a la marche continue, la forme perd 1,85 % par tranche
    # de 100 s (1/(90 min), voir world/Needs.hpp), et l'affichage est ENTIER. Un
    # premier reglage a 100 s a rendu 99 % puis 98 % sur deux runs - la marge
    # tenait a un point d'arrondi, et une marche genee par le relief (le joueur
    # bute, donc ne se fatigue plus) l'aurait effacee. 160 s laissent ~3 %.
    Write-Host "--- 160 s de marche (la forme doit DESCENDRE) ---"
    foreach ($i in 1..40) {
        HoldMs $SC.W 2000
        HoldMs $SC.S 2000
    }

    Write-Host "--- 60 s d'immobilite complete (la forme doit REMONTER) ---"
    Start-Sleep -Seconds 60

    Write-Host "--- Redimensionnements ---"
    foreach ($size in @(@(800,600), @(1280,720))) {
        [void][W32]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 0, 0, $size[0], $size[1], ($SWP_NOMOVE -bor $SWP_NOZORDER))
        Start-Sleep -Milliseconds 700
    }

    $runH = Stop-Game
}


# ===========================================================================
# SCENARIO I (PHASE 17) - empreinte environnementale : faune et frequentation
# ===========================================================================
# Ce que ce scenario apporte, et que rien d'autre ne peut montrer :
#
#  1. LES ANIMAUX SONT DESSINES SOUS VULKAN, avec les couches de validation.
#     C'est une geometrie neuve (world/AnimalMesh.cpp) et une nouvelle source de
#     DrawItem ; les tests headless ne touchent ni au pipeline ni au GPU.
#
#  2. LE SEMIS TIENT SUR DU RELIEF MESURE. Le tirage-rejet filtre par pente et
#     par couvert : sur du relief lisse il passe toujours, sur un massif a 562 m
#     de denivele il peut ne RIEN rendre. C'est exactement le piege des phases
#     precedentes - un filtre qui refuse tout donne un run parfaitement vert -
#     d'ou le controle explicite "au moins un animal au depart".
#
#  3. LA MEMOIRE DE FREQUENTATION MONTE REELLEMENT EN JEU. Le harnais fait
#     marcher le joueur en va-et-vient sur place : la telemetrie doit montrer la
#     frequentation locale grimper, et des animaux PARTIR. Le comportement des
#     seuils est prouve ailleurs (tests headless) ; ce qui est prouve ICI est que
#     le cablage de main.cpp alimente bien la memoire avec la vraie position et
#     la vraie activite du joueur, une fois par image.
#
# LE VA-ET-VIENT, ET POURQUOI : meme raison qu'au scenario H. Ce qui compte est
# la DUREE de presence dans le meme secteur, pas la distance parcourue - et un
# joueur qui finirait coince contre un versant ne marcherait plus, donc ne
# dérangerait plus (l'activite est deduite du deplacement REEL de la camera).
#
# LIMITE CONNUE, ASSUMEE : la dalle de reference n'a pas de carte de couvert
# LiDAR (LHD_0907_6440_1m_canopy.vchm est absent), donc la faune y est placee
# avec le couvert de substitut (WildlifeParams::coverWithoutMap). Le filtre par
# couvert REEL est couvert par les tests headless, pas par ce scenario.
$runI = $null
if ($hasLidar -and (Should-Run 'I')) {
    Start-Game "SCENARIO I - faune et frequentation sur relief mesure" $lidarTile

    $wildlifeSeededLog = Wait-ForLog "animaux au depart" 5000
    Write-Host ("    faune semee au demarrage : {0}" -f $wildlifeSeededLog)

    # 240 s et non 120. Mesure du premier run : a 120 s, le voisinage se vidait
    # puis se REPEUPLAIT - les cellules a une centaine de metres ne sont derangees
    # qu'au tiers de la vitesse de celle ou l'on se tient (ponderation en carre,
    # voir Habitat::notePresence), donc elles passaient encore sous le seuil de
    # reapparition. Il faut environ trois minutes pour que la couronne entiere
    # depasse le seuil. C'est le comportement voulu - on ne vide pas un versant
    # en deux minutes - mais il fallait que le scenario dure assez pour le voir.
    Write-Host "--- 240 s de va-et-vient sur place (la frequentation doit MONTER) ---"
    foreach ($i in 1..60) {
        HoldMs $SC.W 2000
        HoldMs $SC.S 2000
    }

    Write-Host "--- Redimensionnements (les animaux restent dessines) ---"
    foreach ($size in @(@(800,600), @(1280,720))) {
        [void][W32]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 0, 0, $size[0], $size[1], ($SWP_NOMOVE -bor $SWP_NOZORDER))
        Start-Sleep -Milliseconds 700
    }

    $runI = Stop-Game
}

# ===========================================================================
# SCENARIO J : recolte de matieres sur gisement mesure (PHASE 22)
# ===========================================================================
# Ce que ce scenario apporte, et que les tests headless ne peuvent pas donner :
# la chaine COMPLETE, sous les couches de validation Vulkan, depuis un gisement
# que LE JEU a detecte lui-meme sur le relief mesure jusqu'a une motte d'argile
# dans l'inventaire du joueur.
#
# LE POINT DE DEPART N'EST PAS CHOISI A LA MAIN. Le jeu journalise ses gisements
# au chargement, avec leurs coordonnees MONDE ; le script les relit et s'y rend.
# C'est la meme methode qu'au scenario H pour les points d'eau, et elle a la meme
# vertu : si le detecteur change, le scenario suit sans etre retouche, au lieu de
# viser une position gravee dans le script qui ne voudrait plus rien dire.
#
# La conversion monde -> EPSG:2154 est celle de la dalle de reference
# (LHD_0907_6440, emprise de 1000 m) : E = 907500 + x, N = 6439500 - z. Elle est
# verifiee a l'execution par la ligne "Position de depart" que le jeu emet.
#
# LES DEUX SENS DU TEST DE PORTEE, comme au scenario H : on creuse SUR le
# gisement (doit reussir), puis on s'en eloigne de 30 m et on recreuse (doit
# etre REFUSE, avec la distance). Un scenario qui ne ferait que le premier ne
# distinguerait pas "la portee marche" de "la touche produit toujours quelque
# chose".
#
# CE QUE CE SCENARIO NE COUVRE PAS, ET POURQUOI. Les PLANTES A FIBRES ne sont
# pas exercees ici : la dalle de reference n'a pas de carte de couvert produite
# (meme manque qu'au scenario I pour la faune), donc la voie "lisiere" y est
# muette et la voie "sol ouvert" demanderait d'abattre plusieurs arbres dans la
# meme cellule puis d'attendre. Elles sont couvertes par les tests headless, des
# deux cotes et avec leurs temoins. La RESINE n'est pas exercee non plus : elle
# demande de VISER un arbre, or le harnais ne sait pas tourner la camera (voir
# l'en-tete) et l'unique position de tir connue de cette dalle est deja consommee
# par l'abattage du scenario C. Ces deux manques sont notes dans
# NOTES-PHASE22.md ; ils ne sont pas des echecs, ce sont des limites du harnais.
$runJ = $null
$digDeposit = $null
$digAtSource = $false
$digTooFar = $false
$digPicked = $false
$fiberSeeded = $false
$fiberPulled = $false
$fiberPicked = $false
$fiberRegrowing = $false
if ($hasLidar -and (Should-Run 'J')) {
    Start-Game "SCENARIO J - recolte de matieres, gisement LiDAR reel" $lidarTile

    # Le jeu emet ses gisements au chargement ; on attend la liste puis on la lit.
    [void](Wait-ForLog 'Gisements : ' 8000)
    $depositMatch = [regex]::Match($script:outSb.ToString(),
        'gisement \d+ : (argile|sable) en ([-0-9.]+) ([-0-9.]+) ([-0-9.]+)')
    if ($depositMatch.Success) {
        $digDeposit = [pscustomobject]@{
            Kind = $depositMatch.Groups[1].Value
            X    = [double]$depositMatch.Groups[2].Value
            Z    = [double]$depositMatch.Groups[4].Value
        }
        Write-Host ("    gisement retenu : {0} en ({1:N1}, {2:N1})" -f
                    $digDeposit.Kind, $digDeposit.X, $digDeposit.Z)
    } else {
        Write-Host "    [ATTENTION] le jeu n'a journalise aucun gisement."
    }

    $runJ = Stop-Game

    if ($null -ne $digDeposit) {
        # On RELANCE le jeu en demarrant SUR le gisement. Le harnais ne sait se
        # deplacer que sur les axes du monde et a l'estime ; parcourir plusieurs
        # centaines de metres de relief reel accumulerait une derive bien
        # superieure aux 3,5 m de portee. --start supprime le probleme, exactement
        # comme pour le velo (scenario E) et le point d'eau (scenario H).
        $eastings = 907500.0 + $digDeposit.X
        $northing = 6439500.0 - $digDeposit.Z
        Start-Game "SCENARIO J - creuser le gisement, puis le meme geste a 30 m" $lidarTile `
                   @("--start=$eastings,$northing")

        Write-Host "--- Creuser SUR le gisement (Y) ---"
        Send-Key $SC.Y
        $digAtSource = Wait-ForLog "Creuser : (une|un) .* tombe a vos pieds" 3000
        Write-Host ("    matiere detachee du gisement : {0}" -f $digAtSource)

        Write-Host "--- Ramasser la matiere produite (R) ---"
        Vacuum 3
        $digPicked = ($script:outSb.ToString() -match
                      'Ramasse : (une motte d.argile|une poignee de sable)')
        Write-Host ("    matiere ramassee : {0}" -f $digPicked)

        Write-Host "--- S'en eloigner de 30 m, puis recreuser (controle negatif) ---"
        Hold $SC.D 30.0
        Send-Key $SC.Y
        $digTooFar = Wait-ForLog "Creuser : le gisement de .* est a .* m \(il faut etre a" 3000
        Write-Host ("    refus a distance : {0}" -f $digTooFar)

        Write-Host "--- Redimensionnements apres la recolte ---"
        foreach ($size in @(@(800,600), @(1280,720))) {
            [void][W32]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 0, 0, $size[0], $size[1], ($SWP_NOMOVE -bor $SWP_NOZORDER))
            Start-Sleep -Milliseconds 700
        }

        $runJ2 = Stop-Game
        # Les deux processus du scenario comptent pour un : on concatene leurs
        # journaux et on garde le PIRE code de sortie, sans quoi un plantage du
        # premier passerait inapercu.
        $runJ = [pscustomobject]@{
            ExitCode = if ($runJ.ExitCode -ne 0) { $runJ.ExitCode } else { $runJ2.ExitCode }
            Lines    = @($runJ.Lines) + @($runJ2.Lines)
        }
    }

    # --- J3 : les PLANTES A FIBRES, et il faut changer de dalle pour ca -----
    #
    # La dalle de reference du harnais (LHD_0907_6440) n'a PAS de carte de
    # couvert produite - le meme manque qu'au scenario I pour la faune depuis la
    # Phase 17. Or c'est la carte de couvert qui porte la voie "lisiere" du
    # semis. Sur cette dalle, le semis est donc muet, et un scenario qui s'y
    # tiendrait ne prouverait rien : il constaterait un zero PARFAITEMENT
    # CORRECT et le prendrait pour une couverture de test.
    #
    # On charge donc la zone de 25 km2, qui a la sienne. Le point de depart n'a
    # pas ete choisi a l'oeil : un balayage de la zone entiere au pas de 50 m,
    # avec la fonction de semis ELLE-MEME, a cherche les cellules dont la
    # densite est maximale ET qui portent des plantes a portee d'arrachage. Il y
    # en a 1656 sur la zone ; la meilleure est monde (1450, -1750), altitude
    # 1777 m, couvert 0,240 - soit le milieu exact de la bande de lisiere.
    #
    # Georeference de la zone : coin nord-ouest E 899000 / N 6440000, emprise
    # 5000 m, centre a l'origine du monde. Donc E = 901500 + x et
    # N = 6440000 - (z + 2500). Verifiee par la ligne "Position de depart" que
    # le jeu emet, et par le point de depart par defaut (E 901500 / N 6437500 =
    # monde (0, 0)).
    #
    # POURQUOI PAS AU POINT DE DEPART PAR DEFAUT DE LA ZONE : il est a 1947 m,
    # au-dessus de la limite des arbres (FiberPlantParams::maxAltitude = 1900).
    # Le semis y rend zero, et c'est le comportement voulu. Mesure faite : zero
    # plante soumise, zero touffe d'herbe. Un scenario pose la aurait ete vert
    # sans rien exercer.
    if ($hasZone) {
        Start-Game "SCENARIO J3 - plantes a fibres en lisiere LiDAR (zone 25 km2)" $zoneTile `
                   @("--start=902950,6439250")

        $fiberSeeded = Wait-ForLog "Plantes a fibres : [1-9][0-9]* soumises" 12000
        Write-Host ("    plantes a fibres semees par la lisiere : {0}" -f $fiberSeeded)

        Write-Host "--- Arracher la plante a portee (I) ---"
        Send-Key $SC.I
        $fiberPulled = Wait-ForLog "Fibres : la tige cede" 3000
        Write-Host ("    tige arrachee : {0}" -f $fiberPulled)

        Write-Host "--- Ramasser le fagot (R) ---"
        Vacuum 2
        $fiberPicked = ($script:outSb.ToString() -match 'Ramasse : une tige a fibres')
        Write-Host ("    fagot ramasse : {0}" -f $fiberPicked)

        # La plante doit etre EN REPOUSSE, pas disparue : c'est ce qui distingue
        # un semis deterministe d'un semis qu'on aurait ampute.
        $fiberRegrowing = ($script:outSb.ToString() -match
                           'Plantes a fibres : \d+ soumises, [1-9][0-9]* arrachee')
        Write-Host ("    plante en repousse (et non disparue) : {0}" -f $fiberRegrowing)

        $runJ3 = Stop-Game
        $runJ = [pscustomobject]@{
            ExitCode = if ($runJ.ExitCode -ne 0) { $runJ.ExitCode } else { $runJ3.ExitCode }
            Lines    = @($runJ.Lines) + @($runJ3.Lines)
        }
    } else {
        Write-Host "--- Plantes a fibres : IGNORE, la zone de 25 km2 n'est pas produite ---"
    }
}

# ===========================================================================
# Analyse
# ===========================================================================
function Count-Match($lines, $pattern) { @($lines | Where-Object { $_ -match $pattern }).Count }

$all = @()
if ($null -ne $runA) { $all += @($runA.Lines) }
if ($null -ne $runB) { $all += @($runB.Lines) }
if ($null -ne $runC) { $all += @($runC.Lines) }
if ($null -ne $runD) { $all += @($runD.Lines) }
if ($null -ne $runE) { $all += @($runE.Lines) }
if ($null -ne $runF) { $all += @($runF.Lines) }
if ($null -ne $runG1) { $all += @($runG1.Lines) + @($runG2.Lines) + @($runG3.Lines) }
if ($null -ne $runH) { $all += @($runH.Lines) }
if ($null -ne $runI) { $all += @($runI.Lines) }
if ($null -ne $runJ) { $all += @($runJ.Lines) }
# --- PHASE 24 : le journal BRUT du jeu, sur disque ---------------------------
# Le harnais imprime ses CONCLUSIONS ; quand l'une d'elles est fausse, il n'y a
# aucun moyen de savoir ce que le jeu avait reellement emis. Ce projet a perdu
# cinq phases sur exactement ce manque (dette du velo, Phases 17 a 22 : la ligne
# etait dans $runE.Lines depuis le debut). Le dump coute une ecriture de fichier
# par execution et rend l'ecart entre "le jeu s'est trompe" et "le script a mal
# lu" verifiable au lieu d'etre suppose.
# HORODATE, et non un nom fixe. Le premier jet ecrivait toujours
# 'vercora_last_run.log' : une execution suivante ECRASAIT la precedente, et
# c'est arrive tout de suite - le run 5 de la Phase 24 a echoue, le run 6 a
# reussi et a efface la seule trace de l'echec, qui est devenu indiagnosticable.
# Un journal de diagnostic qui ne survit pas a l'execution suivante ne sert a
# rien : les echecs de ce harnais sont justement ceux qu'on ne reproduit pas a
# volonte.
#
# PHASE 26 : le dump porte desormais un EN-TETE PAR PROCESSUS. Sans lui, les
# quatorze runs etaient concatenes bout a bout et une ligne du journal n'etait
# rattachable a aucun scenario : le run en echec de la Phase 25 (78 % de la
# telemetrie du scenario I manquante) n'aurait de toute facon pas ete
# diagnosticable a partir du dump seul, meme s'il avait survecu. On ecrit aussi
# la DUREE MUR de chaque processus, qui est ce qui distingue "le jeu a emis
# moins de lignes par seconde" de "le scenario a dure moins longtemps".
#
# $dumpLines est CONSTRUIT A PART de $all : aucune assertion du harnais ne voit
# ces en-tetes, donc l'instrumentation ne peut pas changer un verdict.
$dumpLines = @()
foreach ($r in $script:allRuns) {
    $dumpLines += ""
    $dumpLines += "########## RUN {0} ##########" -f $r.Label
    $dumpLines += "#   demarre a    : {0}" -f $r.StartedAt.ToString('HH:mm:ss.fff')
    $dumpLines += "#   duree mur    : {0} s" -f $r.WallSeconds
    $dumpLines += "#   code de sortie: {0}" -f $r.ExitCode
    $dumpLines += "#   lignes        : {0}" -f $r.Lines.Count
    $dumpLines += $r.Lines
}

$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$rawLog = Join-Path $env:TEMP "vercora_run_$stamp.log"
try { $dumpLines | Set-Content -Path $rawLog -Encoding UTF8 } catch {}
# Une copie a nom fixe reste bien commode pour "regarder le dernier".
try { $dumpLines | Set-Content -Path (Join-Path $env:TEMP 'vercora_last_run.log') -Encoding UTF8 } catch {}
Write-Host "Journal brut du jeu       : $rawLog ($($all.Count) lignes)"

$validation = @($all | Where-Object { $_ -match '\[Validation\]' })
$errors     = @($all | Where-Object { $_ -match '^\[ERROR\]' })
$warnings   = @($all | Where-Object { $_ -match '^\[WARN\]' })
$cleanA = ($null -eq $runA) -or ((Count-Match $runA.Lines 'Fermeture propre') -ge 1)
$cleanB = ($null -eq $runB) -or ((Count-Match $runB.Lines 'Fermeture propre') -ge 1)
$exitA = if ($null -ne $runA) { $runA.ExitCode } else { 0 }
$exitB = if ($null -ne $runB) { $runB.ExitCode } else { 0 }

# Scenario C : tout ce qui suit vaut $true quand la dalle est absente, pour que
# son absence ignore le scenario au lieu de faire echouer le script.
$cleanC = $true; $exitC = 0; $lidarLoaded = $true; $lidarFelled = $true; $lidarPicked = 0
if ($null -ne $runC) {
    $cleanC = (Count-Match $runC.Lines 'Fermeture propre') -ge 1
    $exitC = $runC.ExitCode
    # Preuve que c'est bien la DALLE qui a ete chargee, et non un repli
    # silencieux sur le generateur procedural - lequel produirait par ailleurs un
    # scenario entierement vert.
    $lidarLoaded = (Count-Match $runC.Lines 'Terrain reel charge') -ge 1
    $felledC = @($runC.Lines | ForEach-Object {
        $m = [regex]::Match($_, "Abattage de l'arbre (\d+)"); if ($m.Success) { $m.Groups[1].Value } })
    $lidarFelled = ($felledC -contains $expectedTreeC)
    $lidarPicked = (Count-Match $runC.Lines 'Ramasse : ')
}

# Scenario D : meme convention, $true quand la dalle est absente.
$cleanD = $true; $exitD = 0
if ($null -ne $runD) {
    $cleanD = (Count-Match $runD.Lines 'Fermeture propre') -ge 1
    $exitD = $runD.ExitCode
}

# Scenario E : meme convention, $true quand la dalle est absente.
#
# Le depouillement ne se contente PAS de constater qu'on est monte sur le velo :
# il relit la telemetrie emise chaque seconde et verifie que la physique a
# reellement travaille sur du relief - une descente franche, une remontee, et
# des vitesses dans la fourchette annoncee. Un velo bloque a 0 km/h contre un
# arbre donnerait autrement un scenario parfaitement vert.
$cleanE = $true; $exitE = 0
$bikeMaxKmh = 0.0; $bikeDescentKmh = 0.0; $bikeClimbKmh = -1.0
$bikeDescentMean = 0.0; $bikeClimbMean = 0.0
$bikeDescentN = 0; $bikeClimbN = 0
$bikeClimbGain = 0.0; $bikeStalled = 0
$bikeMinSlope = 0.0; $bikeMaxSlope = 0.0
$bikeMinAlt = 0.0; $bikeMaxAlt = 0.0; $bikeSamples = 0
if ($null -ne $runE) {
    $cleanE = (Count-Match $runE.Lines 'Fermeture propre') -ge 1
    $exitE = $runE.ExitCode

    $kmh = @(); $slopes = @(); $alts = @(); $lastAlt = $null
    foreach ($line in $runE.Lines) {
        $m = [regex]::Match($line,
            'Velo : ([-0-9.]+) km/h, pente ([-0-9.]+) %.*altitude ([-0-9.]+) m')
        if ($m.Success) {
            $v = [double]$m.Groups[1].Value
            $p = [double]$m.Groups[2].Value
            $a = [double]$m.Groups[3].Value
            $kmh += $v; $slopes += $p; $alts += $a
            if ($v -gt $bikeMaxKmh) { $bikeMaxKmh = $v }
            # Vitesse la plus elevee constatee EN DESCENTE franche, et la plus
            # elevee constatee EN MONTEE franche : c'est leur ecart qui montre
            # que la pente pilote bien la vitesse.
            if ($p -le -8.0) {
                if ($v -gt $bikeDescentKmh) { $bikeDescentKmh = $v }
                $bikeDescentMean += $v; $bikeDescentN++
            }
            if ($p -ge 8.0) {
                if ($v -gt $bikeClimbKmh) { $bikeClimbKmh = $v }
                # Moyenne calculee EN MOUVEMENT seulement, et calages comptes a
                # part. Melanger les deux donnerait une moyenne qui ne decrit
                # rien : ni la vitesse de montee, ni la frequence des arrets.
                if ($v -gt 0.5) { $bikeClimbMean += $v; $bikeClimbN++ } else { $bikeStalled++ }
            }
            # Denivele REELLEMENT gagne : somme des montees d'altitude entre deux
            # releves consecutifs pendant que la pente est ascendante. C'est la
            # seule mesure qui prouve que le velo MONTE, par opposition a "une
            # pente positive a ete rencontree" (vrai meme a l'arret) ou a "un pic
            # de vitesse en cote" (vrai en emportant l'elan de la descente).
            if ($p -ge 2.0 -and $lastAlt -ne $null -and $a -gt $lastAlt) {
                $bikeClimbGain += ($a - $lastAlt)
            }
            $lastAlt = $a
        }
    }
    $bikeSamples = $kmh.Count
    # MOYENNES et non seulement maxima. Le maximum en montee est trompeur : il
    # inclut la seconde ou le velo aborde la cote en emportant encore la vitesse
    # de la descente precedente. La moyenne, elle, dit ce que le velo tient
    # REELLEMENT dans la pente, et c'est elle qui porte la comparaison.
    if ($bikeDescentN -gt 0) { $bikeDescentMean = $bikeDescentMean / $bikeDescentN }
    if ($bikeClimbN   -gt 0) { $bikeClimbMean   = $bikeClimbMean   / $bikeClimbN }
    if ($bikeSamples -gt 0) {
        $bikeMinSlope = ($slopes | Measure-Object -Minimum).Minimum
        $bikeMaxSlope = ($slopes | Measure-Object -Maximum).Maximum
        $bikeMinAlt   = ($alts   | Measure-Object -Minimum).Minimum
        $bikeMaxAlt   = ($alts   | Measure-Object -Maximum).Maximum
    }
}

$linesA = if ($null -ne $runA) { $runA.Lines } else { @() }
$linesB = if ($null -ne $runB) { $runB.Lines } else { @() }
$picked  = @($linesA | Where-Object { $_ -match 'Ramasse : ' })
$knapped = Count-Match $linesA 'eclat tranchant se detache'
$felledA = @($linesA | ForEach-Object {
    $m = [regex]::Match($_, "Abattage de l'arbre (\d+)"); if ($m.Success) { $m.Groups[1].Value } })
$felledB = @($linesB | ForEach-Object {
    $m = [regex]::Match($_, "Abattage de l'arbre (\d+)"); if ($m.Success) { $m.Groups[1].Value } })

$aimHit = ($null -eq $runA) -or ($felledA -contains $expectedTree)
# Le scenario B doit avoir abattu l'arbre 7 (ce qui prouve qu'on l'a atteint et
# vise) puis avoir pousse dans sa souche. On ne conclut RIEN sur le comportement
# de la collision ici - voir le commentaire du scenario B.
$collisionPathExercised = ($null -eq $runB) -or (($felledB -contains '7') -and
                          ((Count-Match $linesB 'Debitage en cours') -ge 1))

$kinds = @{}
foreach ($p in $picked) {
    $m = [regex]::Match($p, 'Ramasse : (?:une|un) (\w+)')
    if ($m.Success) { $kinds[$m.Groups[1].Value] = $true }
}

# Scenario F : meme convention, $true quand la dalle est absente.
#
# Comme pour le velo, le depouillement ne se contente PAS de constater qu'on
# s'est engage : il relit la telemetrie emise chaque seconde et verifie que
# l'escalade a reellement travaille sur du relief - de la pente au-dessus du
# seuil de paroi, une jauge qui DESCEND, et un gain d'altitude reel. Un grimpeur
# accroche sans bouger donnerait autrement un scenario parfaitement vert.
$cleanF = $true; $exitF = 0
$climbSamples = 0
$climbMinStamina = 100.0; $climbMaxGain = 0.0
$climbMinSlope = 1e9; $climbMaxSlope = 0.0
$climbMinAlt = 1e9; $climbMaxAlt = -1e9
$climbStaminaRecovered = $false
if ($null -ne $runF) {
    $cleanF = (Count-Match $runF.Lines 'Fermeture propre') -ge 1
    $exitF = $runF.ExitCode

    $staminas = @()
    foreach ($line in $runF.Lines) {
        $m = [regex]::Match($line,
            'Escalade : energie ([-0-9.]+) %, pente ([-0-9.]+) %, altitude ([-0-9.]+) m, gain ([-0-9.]+) m')
        if ($m.Success) {
            $e = [double]$m.Groups[1].Value
            $p = [double]$m.Groups[2].Value
            $a = [double]$m.Groups[3].Value
            $g = [double]$m.Groups[4].Value
            $climbSamples++
            $staminas += $e
            if ($e -lt $climbMinStamina) { $climbMinStamina = $e }
            if ($g -gt $climbMaxGain) { $climbMaxGain = $g }
            if ($p -lt $climbMinSlope) { $climbMinSlope = $p }
            if ($p -gt $climbMaxSlope) { $climbMaxSlope = $p }
            if ($a -lt $climbMinAlt) { $climbMinAlt = $a }
            if ($a -gt $climbMaxAlt) { $climbMaxAlt = $a }
        }
    }
    # La jauge doit REMONTER quelque part dans le run : c'est la preuve que le
    # repos au sol entre les deux engagements a bien recupere. Sans ce controle,
    # une recuperation cassee passerait inapercue - la seconde ascension serait
    # simplement plus courte.
    for ($i = 1; $i -lt $staminas.Count; $i++) {
        if ($staminas[$i] -gt $staminas[$i - 1] + 1.0) { $climbStaminaRecovered = $true }
    }
    if ($climbSamples -eq 0) { $climbMinSlope = 0.0; $climbMinAlt = 0.0; $climbMaxAlt = 0.0 }
}

# Scenario G : meme convention que C..F, $true quand la dalle est absente.
#
# Le depouillement ne se contente pas de constater que les touches sont passees.
# Il verifie que le catalogue s'est constitue, que des objets ont ete REELLEMENT
# poses puis relus depuis le disque, et - le point qui compte - que ces objets
# ARRETENT le joueur, en comparant la distance parcourue a celle du temoin.
$cleanG1 = $true; $cleanG2 = $true; $cleanG3 = $true
$exitG1 = 0; $exitG2 = 0; $exitG3 = 0
$editorEntered = $false; $editorLeft = $false
$catalogCount = 0; $editorPlaced = 0; $editorDeleted = 0; $editorUndone = 0
$editorSelected = $false; $editorSaved = 0; $editorReloaded = 0
$editorPlacedDistinct = 0
$editorCycled = 0
$sceneAutoLoaded = 0; $sceneSolid = 0
$walkWithScene = 0.0; $walkControl = 0.0
if ($null -ne $runG1) {
    $cleanG1 = (Count-Match $runG1.Lines 'Fermeture propre') -ge 1
    $cleanG2 = (Count-Match $runG2.Lines 'Fermeture propre') -ge 1
    $cleanG3 = (Count-Match $runG3.Lines 'Fermeture propre') -ge 1
    $exitG1 = $runG1.ExitCode; $exitG2 = $runG2.ExitCode; $exitG3 = $runG3.ExitCode

    $editorEntered = (Count-Match $runG1.Lines '=== MODE EDITEUR ===') -ge 1
    $editorLeft    = (Count-Match $runG1.Lines 'retour au JEU') -ge 1
    $editorPlaced  = Count-Match $runG1.Lines 'Editeur : pose de'
    # Nombre de lignes de pose DISTINCTES. Discriminant ajoute apres un run ou le
    # compte etait de 9 pour trois touches envoyees : il separe "le jeu a
    # reellement pose neuf objets" (neuf lignes differentes, les identifiants
    # allant de 1 a 9) de "la meme ligne a ete comptee plusieurs fois" (trois
    # lignes distinctes repetees), qui sont deux defauts sans rapport - l'un dans
    # le jeu, l'autre dans la capture de sortie de ce script.
    $editorPlacedDistinct = (@($runG1.Lines |
        Where-Object { $_ -match 'Editeur : pose de' } | Select-Object -Unique)).Count
    $editorDeleted = Count-Match $runG1.Lines 'Editeur : objet \d+ .* supprime'
    $editorUndone  = Count-Match $runG1.Lines 'Editeur : annule'
    $editorSelected = (Count-Match $runG1.Lines 'Editeur : selection de l''objet') -ge 1
    # F3/F4 : selection SANS viser (ajout ergonomique post-Phase 15). Deux F3
    # puis un F4 sont envoyes ; ce compte doit valoir 3, sans quoi une des
    # trois touches a ete perdue en route (scancode faux, front non resynchro-
    # nise, etc.) - exactement le genre de defaut que le headless ne peut pas
    # voir puisqu'il ne passe jamais par GLFW/PostMessage.
    $editorCycled = Count-Match $runG1.Lines '\[F3/F4\]'

    $m = [regex]::Match(($runG1.Lines -join "`n"), 'Editeur : (\d+) asset\(s\) au catalogue')
    if ($m.Success) { $catalogCount = [int]$m.Groups[1].Value }

    # Nombre d'objets REELLEMENT ecrits sur le disque, tel que le jeu l'a
    # journalise - et non tel que le script croit les avoir poses.
    $m = [regex]::Match(($runG1.Lines -join "`n"), 'Scene : (\d+) objet\(s\) ecrits')
    if ($m.Success) { $editorSaved = [int]$m.Groups[1].Value }
    # Rechargement a la touche F9, DANS le meme processus.
    $reloadMatches = [regex]::Matches(($runG1.Lines -join "`n"), 'Scene : (\d+) objet\(s\) charges')
    if ($reloadMatches.Count -gt 0) {
        $editorReloaded = [int]$reloadMatches[$reloadMatches.Count - 1].Groups[1].Value
    }

    # G2 : chargement AUTOMATIQUE hors mode editeur.
    $m = [regex]::Match(($runG2.Lines -join "`n"),
                        'Scene : (\d+) objet\(s\) charges .*?\((\d+) solide')
    if ($m.Success) {
        $sceneAutoLoaded = [int]$m.Groups[1].Value
        $sceneSolid = [int]$m.Groups[2].Value
    }

    # Distance REELLEMENT parcourue vers le nord, lue dans la trace de position
    # emise chaque seconde. Le nord est en -Z (voir Heightmap.hpp), d'ou le signe.
    function Get-WalkDistance($lines) {
        $samples = @($lines | ForEach-Object {
            $mm = [regex]::Match($_, 'Camera : yaw .* position ([-0-9.]+) ([-0-9.]+) ([-0-9.]+)')
            if ($mm.Success) { [double]$mm.Groups[3].Value } })
        if ($samples.Count -eq 0) { return 0.0 }
        return [Math]::Abs(($samples | Measure-Object -Minimum).Minimum)
    }
    $walkWithScene = Get-WalkDistance $runG2.Lines
    $walkControl   = Get-WalkDistance $runG3.Lines
}

# ---------------------------------------------------------------------------
# Scenario I : depouillement de la telemetrie d'empreinte (PHASE 17)
# ---------------------------------------------------------------------------
# Meme convention que C..H : tout vaut $true quand le scenario n'a pas ete joue.
#
# La ligne depouillee est celle emise une fois par seconde par main.cpp. On en
# tire quatre grandeurs, et chacune repond a une question qui, seule, laisserait
# passer un systeme casse :
#   * le nombre d'animaux au DEPART  -> le semis a-t-il rendu quelque chose ?
#   * la frequentation MAXIMALE      -> la memoire monte-t-elle vraiment ?
#   * le nombre d'animaux EN FUITE   -> la faune reagit-elle au joueur ?
#   * le nombre de RELEVES           -> a-t-on seulement mesure ?
$cleanI = $true; $exitI = 0
$wildlifeStart = 0; $wildlifeSamples = 0
$wildlifeMaxDisturbance = 0.0; $wildlifeMaxFleeing = 0; $wildlifeMinAnimals = 9999
$wildlifeNearFirst = 0; $wildlifeNearLast = 0; $wildlifeNearMin = 9999
if ($null -ne $runI) {
    $cleanI = (Count-Match $runI.Lines 'Fermeture propre') -ge 1
    $exitI = $runI.ExitCode

    $mStart = [regex]::Match(($runI.Lines -join "`n"), 'Faune : (\d+) animaux au depart')
    if ($mStart.Success) { $wildlifeStart = [int]$mStart.Groups[1].Value }

    foreach ($line in $runI.Lines) {
        $m = [regex]::Match($line,
            'Faune : (\d+) animaux dont (\d+) en fuite, (\d+) a moins de .*frequentation ici ([-0-9.]+) %')
        if ($m.Success) {
            $wildlifeSamples++
            $count = [int]$m.Groups[1].Value
            $fleeing = [int]$m.Groups[2].Value
            $near = [int]$m.Groups[3].Value
            $disturbance = [double]$m.Groups[4].Value
            if ($fleeing -gt $wildlifeMaxFleeing) { $wildlifeMaxFleeing = $fleeing }
            if ($disturbance -gt $wildlifeMaxDisturbance) { $wildlifeMaxDisturbance = $disturbance }
            if ($count -lt $wildlifeMinAnimals) { $wildlifeMinAnimals = $count }
            if ($near -lt $wildlifeNearMin) { $wildlifeNearMin = $near }
            if ($wildlifeSamples -eq 1) { $wildlifeNearFirst = $near }
            $wildlifeNearLast = $near
        }
    }
    if ($wildlifeSamples -eq 0) { $wildlifeMinAnimals = 0; $wildlifeNearMin = 0 }

    # --- PHASE 26 : le DISCRIMINATEUR de la telemetrie courte ---------------
    # Le scenario I a echoue une fois en Phase 25 avec 78 % de ses releves
    # manquants, et la cause n'a pas pu etre retrouvee. Le releve de faune est
    # emis DANS LE MEME bloc "une fois par seconde" que la ligne "Camera :",
    # laquelle est emise SANS AUCUNE CONDITION (ni editeur, ni faune vivante).
    # Comparer les deux comptes separe donc deux causes qu'aucune des deux ne
    # distingue seule :
    #   * les deux comptes chutent ensemble -> c'est le TIC lui-meme qui n'a pas
    #     battu 240 fois : le jeu a tourne moins longtemps, ou la cadence du
    #     compteur d'images a derive.
    #   * "Camera :" intact, "Faune :" effondre -> c'est la CONDITION de la
    #     ligne de faune (editorMode, ou wildlife devenu nul) qui a saute.
    # Et la duree mur dit si le scenario a seulement dure les 240 s prevues.
    $cameraTicks = (Count-Match $runI.Lines 'Camera : yaw')
    Write-Host ("[diag I] duree mur {0} s, tics 'Camera :' {1}, releves faune {2}" -f
                $runI.WallSeconds, $cameraTicks, $wildlifeSamples)
}

# La DEFORESTATION, elle, se lit dans les scenarios qui abattent deja des arbres
# (A et B). Aucun processus supplementaire : la ligne d'empreinte est emise dans
# tous les runs, et le compteur de pointe y monte des le premier arbre couche.
# C'est le chainon "abattage -> memoire du milieu" verifie SOUS VULKAN.
$loggingPeak = 0.0
foreach ($line in @($linesA) + @($linesB)) {
    $m = [regex]::Match($line, 'pointes [-0-9.]+/([-0-9.]+) %')
    if ($m.Success -and ([double]$m.Groups[1].Value) -gt $loggingPeak) {
        $loggingPeak = [double]$m.Groups[1].Value
    }
}

Write-Host ""
Write-Host "==================== RESULTAT ===================="
Write-Host ("Code de sortie A..G       : {0} / {1} / {2} / {3} / {4} / {5} / {6}" -f
            $(if ($null -ne $runA) { $exitA } else { "(ignore)" }),
            $(if ($null -ne $runB) { $exitB } else { "(ignore)" }),
            $(if ($null -ne $runC) { $exitC } else { "(ignore)" }),
            $(if ($null -ne $runD) { $exitD } else { "(ignore)" }),
            $(if ($null -ne $runE) { $exitE } else { "(ignore)" }),
            $(if ($null -ne $runF) { $exitF } else { "(ignore)" }),
            $(if ($null -ne $runG1) { "$exitG1/$exitG2/$exitG3" } else { "(ignore)" }))
Write-Host ("Lignes de log             : {0}" -f $all.Count)
Write-Host ("Messages [Validation]     : {0}" -f $validation.Count)
Write-Host ("Lignes [ERROR]            : {0}" -f $errors.Count)
Write-Host ("Lignes [WARN]             : {0}" -f $warnings.Count)
Write-Host ("Fermeture propre A..G     : {0} / {1} / {2} / {3} / {4} / {5} / {6}" -f $cleanA, $cleanB,
            $(if ($null -ne $runC) { $cleanC } else { "(ignore)" }),
            $(if ($null -ne $runD) { $cleanD } else { "(ignore)" }),
            $(if ($null -ne $runE) { $cleanE } else { "(ignore)" }),
            $(if ($null -ne $runF) { $cleanF } else { "(ignore)" }),
            $(if ($null -ne $runG1) { "$cleanG1/$cleanG2/$cleanG3" } else { "(ignore)" }))
Write-Host ""
Write-Host "---- Gameplay effectivement exerce ----"
Write-Host ("Arbres abattus            : {0}" -f ($felledA.Count + $felledB.Count))
Write-Host ("Arbres au sol             : {0}" -f (Count-Match $all 'Arbre a terre'))
Write-Host ("Debitages declenches      : {0}" -f (Count-Match $all 'Debitage en cours'))
Write-Host ("Objets ramasses (R)       : {0}  [{1}]" -f $picked.Count, (($kinds.Keys | Sort-Object) -join ', '))
$picked | ForEach-Object { Write-Host ("   {0}" -f ($_ -replace '^\[INFO\]\s+','')) }
Write-Host ("Percussions reussies (C)  : {0}" -f $knapped)
Write-Host ("Arbre vise / abattus (A)  : {0} / [{1}]  {2}" -f $expectedTree, ($felledA -join ','),
            $(if ($aimHit) { "(pilotage precis)" } else { "(DERIVE : cible manquee)" }))
# --- PHASE 24 : ce que le FILTRE DE PENTE a fait pendant le run --------------
# La marche a desormais une limite de pente (core/SlopeLimits.hpp), et le
# harnais navigue A L'ESTIME : il tient une touche pendant distance/14 m/s. Une
# marche ralentie par le relief est donc une cause POSSIBLE d'une cible manquee,
# a cote de la derive deja connue. Sans cette trace, les deux sont
# indiscernables - et ce projet a deja perdu cinq phases a accuser le jeu d'un
# defaut qui etait dans l'instrument.
#
# Le jeu ne journalise que les CHANGEMENTS d'etat : ces lignes sont rares.
$slopeLines = @($all | Where-Object { $_ -match 'Marche : ' })
Write-Host ("Filtre de pente (Phase 24): {0} changement(s) d'etat journalise(s)" -f $slopeLines.Count)
$slopeLines | Select-Object -First 12 | ForEach-Object {
    Write-Host ("   {0}" -f ($_ -replace '^\[INFO\]\s+',''))
}

Write-Host ("Chemin de collision (B)   : {0}" -f
    $(if ($collisionPathExercised) { "tronc debout PUIS souche pousses, sans erreur ni plantage" }
      else { "scenario B incomplet (l'arbre 7 n'a pas ete abattu)" }))
Write-Host    "                            (comportement de la collision : prouve par tests\logic_tests.cpp)"

Write-Host ""
Write-Host "---- Dalle LiDAR HD reelle (scenario C) ----"
if ($null -eq $runC) {
    Write-Host "Ignore : la dalle n'a pas ete produite (voir le message en tete de sortie)."
} else {
    $meshLine = @($runC.Lines | Where-Object { $_ -match 'Maillage terrain genere' }) | Select-Object -First 1
    $loadLine = @($runC.Lines | Where-Object { $_ -match 'Terrain reel charge' }) | Select-Object -First 1
    Write-Host ("Dalle chargee             : {0}" -f $lidarLoaded)
    if ($loadLine) { Write-Host ("   {0}" -f ($loadLine -replace '^\[INFO\]\s+','')) }
    if ($meshLine) { Write-Host ("   {0}" -f ($meshLine -replace '^\[INFO\]\s+','')) }
    Write-Host ("Arbre vise / abattu       : {0} / {1}" -f $expectedTreeC,
                $(if ($lidarFelled) { "abattu (marche et visee OK sur relief mesure)" }
                  else { "MANQUE" }))
    Write-Host ("Objets ramasses           : {0}" -f $lidarPicked)
}

Write-Host ""
Write-Host "---- Vol en parapente (scenario D) ----"
if ($null -eq $runD) {
    Write-Host "Ignore : la dalle n'a pas ete produite (voir le message en tete de sortie)."
} else {
    $landedLine = @($runD.Lines | Where-Object { $_ -match 'Parapente : atterrissage|vol interrompu' }) | Select-Object -First 1
    Write-Host ("Decollage                 : {0}" -f $paraglideLaunched)
    Write-Host ("Fin de vol                : {0}" -f $paraglideEnded)
    if ($landedLine) { Write-Host ("   {0}" -f ($landedLine -replace '^\[INFO\]\s+','')) }
}

Write-Host ""
Write-Host "---- Sortie a velo (scenario E) ----"
if ($null -eq $runE) {
    Write-Host "Ignore : la dalle n'a pas ete produite (voir le message en tete de sortie)."
} else {
    Write-Host ("En selle / pied a terre   : {0} / {1}" -f $bikeMounted, $bikeDismounted)
    Write-Host ("Releves de telemetrie     : {0}" -f $bikeSamples)
    Write-Host ("Vitesse maximale          : {0:N1} km/h" -f $bikeMaxKmh)
    Write-Host ("  en descente (pente <= -8 %)  : {0:N1} km/h au pic, {1:N1} km/h en moyenne sur {2} releves" -f
                $bikeDescentKmh, $bikeDescentMean, $bikeDescentN)
    Write-Host ("  en montee   (pente >= +8 %)  : {0}" -f
        $(if ($bikeClimbKmh -ge 0) { "{0:N1} km/h au pic, {1:N1} km/h en moyenne sur {2} releves EN MOUVEMENT" -f $bikeClimbKmh, $bikeClimbMean, $bikeClimbN } else { "aucun releve" }))
    Write-Host ("  releves a l'arret en cote    : {0}  (le velo cale au-dela de 23,6 % de pente)" -f $bikeStalled)
    Write-Host ("Denivele reellement GAGNE : {0:N1} m (somme des montees sous pente ascendante)" -f $bikeClimbGain)
    Write-Host ("Pente rencontree          : {0:N1} % a {1:N1} %" -f $bikeMinSlope, $bikeMaxSlope)
    Write-Host ("Altitude parcourue        : {0:N1} m a {1:N1} m  (denivele {2:N1} m)" -f
                $bikeMinAlt, $bikeMaxAlt, ($bikeMaxAlt - $bikeMinAlt))
}

Write-Host ""
Write-Host "---- Empreinte environnementale (scenario I, PHASE 17) ----"
if ($null -eq $runI) {
    Write-Host "Ignore : la dalle n'a pas ete produite (voir le message en tete de sortie)."
} else {
    Write-Host ("Animaux au demarrage      : {0}" -f $wildlifeStart)
    Write-Host ("Releves de telemetrie     : {0}" -f $wildlifeSamples)
    Write-Host ("Frequentation locale      : {0:N1} % au maximum" -f $wildlifeMaxDisturbance)
    Write-Host ("Animaux en fuite          : {0} simultanement au pic" -f $wildlifeMaxFleeing)
    Write-Host ("Population minimale       : {0} animaux (l'effectif TOTAL bouge peu : la faune s'ecarte, elle ne s'evapore pas)" -f $wildlifeMinAnimals)
    Write-Host ("Animaux a portee de vue   : {0} au premier releve -> {1} au dernier (minimum {2})" -f
                $wildlifeNearFirst, $wildlifeNearLast, $wildlifeNearMin)
}
Write-Host ("Deforestation apres coupe : {0:N1} % au pic (scenarios A et B)" -f $loggingPeak)

Write-Host ""
Write-Host "---- Recolte de matieres (scenario J, PHASE 22) ----"
if ($null -eq $runJ) {
    Write-Host "Ignore : la dalle n'a pas ete produite, ou scenario non joue."
} else {
    if ($null -ne $digDeposit) {
        Write-Host ("Gisement vise            : {0} en ({1:N1}, {2:N1}), detecte PAR LE JEU" -f
                    $digDeposit.Kind, $digDeposit.X, $digDeposit.Z)
    }
    Write-Host ("Creuse sur le gisement   : {0}" -f $digAtSource)
    Write-Host ("Matiere ramassee (R)     : {0}" -f $digPicked)
    Write-Host ("Refus a 30 m             : {0}  (controle negatif de la portee)" -f $digTooFar)
    if ($hasZone) {
        Write-Host ("Plantes a fibres semees  : {0}   (voie LISIERE, carte de couvert de la zone 25 km2)" -f $fiberSeeded)
        Write-Host ("Tige arrachee (I)        : {0}" -f $fiberPulled)
        Write-Host ("Fagot ramasse (R)        : {0}" -f $fiberPicked)
        Write-Host ("Plante EN REPOUSSE       : {0}  (et non disparue du semis)" -f $fiberRegrowing)
    } else {
        Write-Host  "Plantes a fibres         : ignorees, la zone de 25 km2 n'est pas produite"
        Write-Host  "                           (c'est elle seule qui a une carte de couvert)."
    }
    Write-Host  "PLACEMENT DES GISEMENTS  : proxy morphologique, PAS une carte geologique"
    Write-Host  "                           (world/Deposits.hpp). La RESINE n'est pas exercee"
    Write-Host  "                           ici - elle demande de viser un arbre, or le harnais"
    Write-Host  "                           ne sait pas tourner la camera. Voir NOTES-PHASE22.md."
}
Write-Host ""
Write-Host "---- Escalade (scenario F) ----"
if ($null -eq $runF) {
    Write-Host "Ignore : la dalle n'a pas ete produite (voir le message en tete de sortie)."
} else {
    Write-Host ("Engagement / lachage      : {0} / {1}" -f $climbEngaged, $climbReleased)
    Write-Host ("Re-engagement / SOMMET    : {0} / {1}" -f $climbReEngaged, $climbToppedOut)
    Write-Host ("Releves de telemetrie     : {0}" -f $climbSamples)
    Write-Host ("Jauge d'energie           : minimum {0:N0} %" -f $climbMinStamina)
    Write-Host ("  recuperation constatee  : {0}  (la jauge remonte pendant le repos au sol)" -f
                $climbStaminaRecovered)
    Write-Host ("Gain d'altitude maximal   : {0:N1} m au-dessus du pied de la paroi" -f $climbMaxGain)
    Write-Host ("Pente sous les mains      : {0:N0} % a {1:N0} %  (seuil de paroi : 100 %)" -f
                $climbMinSlope, $climbMaxSlope)
    Write-Host ("Altitude parcourue        : {0:N1} m a {1:N1} m  (denivele {2:N1} m)" -f
                $climbMinAlt, $climbMaxAlt, ($climbMaxAlt - $climbMinAlt))
}

# Scenario H : meme convention que C..G, $true quand la dalle est absente.
#
# Le depouillement relit la telemetrie des besoins, emise chaque seconde, et
# verifie DEUX mouvements opposes : la forme descend pendant la marche, et elle
# remonte pendant l'arret. Constater seulement qu'elle a bouge ne prouverait
# rien - une jauge qui ne fait que descendre est une jauge sans repos.
$cleanH = $true; $exitH = 0
$needsSamples = 0
$needsWaterFound = 0
$needsMinForme = 100.0; $needsMaxForme = 0.0
$needsMinSoif = 100.0
$needsFormeRecovered = $false
$needsMinAmbient = 1e9; $needsMaxAmbient = -1e9
$needsFinalMove = 100.0
if ($null -ne $runH) {
    $cleanH = (Count-Match $runH.Lines 'Fermeture propre') -ge 1
    $exitH = $runH.ExitCode

    $m = [regex]::Match(($runH.Lines -join "`n"), "Points d'eau : (\d+) creux retenus")
    if ($m.Success) { $needsWaterFound = [int]$m.Groups[1].Value }

    $formes = @()
    foreach ($line in $runH.Lines) {
        $m = [regex]::Match($line,
            'Besoins : faim (\d+)% soif (\d+)% forme (\d+)% chaud (\d+)%, ambiante ([-0-9]+) C, ressenti ([-0-9]+) C, deplacement (\d+) %')
        if (-not $m.Success) { continue }
        $needsSamples++
        $soif = [double]$m.Groups[2].Value
        $forme = [double]$m.Groups[3].Value
        $amb = [double]$m.Groups[5].Value
        $formes += $forme
        if ($forme -lt $needsMinForme) { $needsMinForme = $forme }
        if ($forme -gt $needsMaxForme) { $needsMaxForme = $forme }
        if ($soif -lt $needsMinSoif) { $needsMinSoif = $soif }
        if ($amb -lt $needsMinAmbient) { $needsMinAmbient = $amb }
        if ($amb -gt $needsMaxAmbient) { $needsMaxAmbient = $amb }
        $needsFinalMove = [double]$m.Groups[7].Value
    }
    # La forme doit REMONTER quelque part : c'est la preuve que l'arret complet
    # recupere. Meme controle, et pour la meme raison, que la jauge d'escalade
    # du scenario F.
    for ($i = 1; $i -lt $formes.Count; $i++) {
        if ($formes[$i] -gt $formes[$i - 1]) { $needsFormeRecovered = $true }
    }
    if ($needsSamples -eq 0) { $needsMinAmbient = 0.0; $needsMaxAmbient = 0.0 }
}

Write-Host ""
Write-Host "---- Editeur de niveau (scenario G) ----"
if ($null -eq $runG1) {
    Write-Host "Ignore : la dalle n'a pas ete produite (voir le message en tete de sortie)."
} else {
    Write-Host ("Entree / sortie du mode   : {0} / {1}" -f $editorEntered, $editorLeft)
    Write-Host ("Assets au catalogue       : {0}  (modeles generes en code + assets/editor)" -f $catalogCount)
    Write-Host ("Objets poses / supprimes  : {0} / {1}   ({2} ligne(s) de pose distincte(s))" -f
                $editorPlaced, $editorDeleted, $editorPlacedDistinct)
    Write-Host ("Selection / annulations   : {0} / {1}" -f $editorSelected, $editorUndone)
    Write-Host ("Cycle sans viser (F3/F4)  : {0} selection(s)  (2x F3 + 1x F4 envoyes)" -f $editorCycled)
    Write-Host ("Ecrits sur disque (F5)    : {0}" -f $editorSaved)
    Write-Host ("Relus a la touche F9      : {0}" -f $editorReloaded)
    Write-Host ("Charges en JEU NORMAL     : {0} objet(s), {1} solide(s)  (sans --editor)" -f
                $sceneAutoLoaded, $sceneSolid)
    Write-Host ("Marche plein nord         : {0:N2} m avec la scene contre {1:N2} m sans (temoin)" -f
                $walkWithScene, $walkControl)
    Write-Host    "                            meme binaire, meme depart, meme duree de touche :"
    Write-Host    "                            seule la presence des objets poses change."
}


Write-Host ""
Write-Host "---- Besoins primaires (scenario H) ----"
if ($null -eq $runH) {
    Write-Host "Ignore : la dalle n'a pas ete produite (voir le message en tete de sortie)."
} else {
    Write-Host ("Points d'eau detectes     : {0} sur la dalle" -f $needsWaterFound)
    Write-Host ("Boire AU point d'eau      : {0}   (portee franchie ET jauge remplie)" -f $drinkAtSource)
    Write-Host ("Refus a 30 m (temoin)     : {0}" -f $drinkTooFar)
    Write-Host ("Releves de telemetrie     : {0}" -f $needsSamples)
    Write-Host ("Jauge de forme            : minimum {0:N0} %, maximum {1:N0} %" -f
                $needsMinForme, $needsMaxForme)
    Write-Host ("  recuperation constatee  : {0}  (la jauge remonte pendant l'arret complet)" -f
                $needsFormeRecovered)
    Write-Host ("Jauge de soif             : minimum {0:N0} %" -f $needsMinSoif)
    Write-Host ("Temperature ambiante      : {0:N0} C a {1:N0} C  (altitude + heure de jeu)" -f
                $needsMinAmbient, $needsMaxAmbient)
    Write-Host ("Coefficient de deplacement: {0:N0} % en fin de run" -f $needsFinalMove)
}

if ($validation.Count -gt 0) {
    Write-Host ""; Write-Host "--- Messages de validation ---"
    $validation | Select-Object -First 40 | ForEach-Object { Write-Host "  $_" }
}
if ($errors.Count -gt 0) {
    Write-Host ""; Write-Host "--- Erreurs ---"
    $errors | Select-Object -First 40 | ForEach-Object { Write-Host "  $_" }
}

$paraglideOk = ($null -eq $runD) -or ($paraglideLaunched -and $paraglideEnded)

# Le velo est declare OK seulement si la PHYSIQUE a travaille, pas seulement si
# les touches ont ete recues :
#   - on est monte puis descendu de velo ;
#   - la telemetrie montre une descente franche ET une montee franche ;
#   - la descente a produit une vraie vitesse (>= 15 km/h) sans depasser le
#     plafond de 50 km/h documente dans world/Bike.hpp. 15 et non 30 : sur du
#     relief MESURE la pente n'est jamais soutenue assez longtemps pour que la
#     vitesse limite theorique soit atteinte - voir NOTES-PHASE-VELO.md, c'est
#     un resultat de la session et non un seuil baisse pour faire passer le test ;
#   - le velo a GAGNE au moins 2 m d'altitude en pedalant dans la pente : c'est
#     la seule preuve qu'il monte vraiment, une pente positive pouvant etre
#     "rencontree" a l'arret et un pic de vitesse en cote pouvant n'etre que
#     l'elan emporte depuis la descente ;
#   - la montee AVANCE reellement (>= 3 km/h) tout en restant nettement plus
#     lente que la descente : c'est LA propriete demandee a ce modele, et la
#     seule que le relief reel puisse valider. Le seuil de 3 km/h n'est pas
#     cosmetique - sans lui, un velo cale a 0 km/h sur une pente trop raide
#     satisferait "plus lent qu'en descente" et rendrait le scenario vert ;
#   - le velo a change d'altitude d'au moins 12 m, donc il a bien suivi le sol
#     sur une vraie hauteur de versant, et pas seulement frotte sur place.
$bikeOk = ($null -eq $runE) -or (
    $bikeMounted -and $bikeDismounted -and ($bikeSamples -ge 5) -and
    ($bikeMinSlope -le -8.0) -and ($bikeMaxSlope -ge 8.0) -and
    ($bikeDescentKmh -ge 15.0) -and ($bikeMaxKmh -le 50.1) -and
    ($bikeClimbKmh -ge 3.0) -and ($bikeClimbKmh -le $bikeDescentKmh - 5.0) -and
    ($bikeClimbN -ge 3) -and ($bikeDescentN -ge 3) -and
    ($bikeClimbMean -lt $bikeDescentMean) -and ($bikeClimbGain -ge 2.0) -and
    (($bikeMaxAlt - $bikeMinAlt) -ge 12.0))

# Scenario F : ce qui doit etre vrai pour qu'il prouve quelque chose. Chaque
# ligne ferme une facon d'etre vert sans rien montrer :
#  - s'engager, lacher, se re-engager et TOPPER : les deux sorties atteignables ;
#  - de la telemetrie, sinon on n'a rien mesure ;
#  - une pente maximale au-dessus du seuil de paroi : on etait bien sur du
#    rocher, pas sur un talus ;
#  - une jauge qui descend franchement PUIS remonte : l'energie travaille, et le
#    repos au sol la recupere ;
#  - un gain d'altitude et un denivele reels : le grimpeur a bouge, il n'est pas
#    reste suspendu au meme endroit pendant tout le run.
$climbOk = ($null -eq $runF) -or (
    $climbEngaged -and $climbReleased -and $climbReEngaged -and $climbToppedOut -and
    ($climbSamples -ge 10) -and
    ($climbMaxSlope -ge 100.0) -and
    ($climbMinStamina -le 90.0) -and $climbStaminaRecovered -and
    ($climbMaxGain -ge 15.0) -and
    (($climbMaxAlt - $climbMinAlt) -ge 10.0))

# Scenario G : ce qui doit etre vrai pour qu'il prouve quelque chose.
#  - le catalogue s'est constitue (sinon il n'y avait rien a poser) ;
#  - trois objets poses, un supprime, une annulation, une selection : toutes les
#    commandes d'edition ont produit un effet, pas seulement ete recues ;
#  - le fichier ecrit contient les TROIS objets - la suppression ayant ete
#    annulee, en perdre un signalerait une pile d'annulation fautive ;
#  - la relecture a la touche F9 rend le meme compte ;
#  - une partie NORMALE, lancee sans --editor, les charge d'elle-meme et en
#    trouve autant : c'est ce qui distingue un editeur d'un outil qui ecrit un
#    fichier que personne ne lit ;
#  - et surtout : le joueur qui marche dessus est ARRETE. Le seuil porte sur le
#    RAPPORT des deux distances et non sur une valeur absolue, parce que la
#    distance depend du relief, du nombre d'images par seconde et du timing du
#    harnais. Le temoin doit lui-meme avoir avance d'au moins 8 m, sinon
#    l'egalite des deux ne voudrait rien dire (deux runs bloques valent aussi
#    "meme distance").
$editorOk = ($null -eq $runG1) -or (
    $editorEntered -and $editorLeft -and
    ($catalogCount -ge 5) -and
    ($editorPlaced -eq 3) -and ($editorDeleted -ge 1) -and ($editorUndone -ge 1) -and
    $editorSelected -and ($editorCycled -eq 3) -and
    ($editorSaved -eq 3) -and ($editorReloaded -eq 3) -and
    ($sceneAutoLoaded -eq 3) -and ($sceneSolid -ge 1) -and
    ($walkControl -ge 8.0) -and ($walkWithScene -le 0.35 * $walkControl))

# Scenario H : ce qui doit etre vrai pour qu'il prouve quelque chose.
#  - la dalle a livre au moins un point d'eau, sinon il n'y avait rien a
#    approcher et les deux touches T ne testent rien ;
#  - les DEUX touches T ont donne la reponse attendue : "soif etanchee" au bord
#    du creux (portee franchie et jauge remplie) et "trop loin" a 30 m (la
#    portee refuse vraiment). L'une sans l'autre laisserait passer une portee
#    cassee - infinie dans un cas, nulle dans l'autre ;
#  - de la telemetrie, sinon on n'a rien mesure ;
#  - la forme est DESCENDUE pendant la marche puis REMONTEE pendant l'arret :
#    les deux sens, sans quoi la jauge serait a sens unique ;
#  - une temperature ambiante plausible pour de la moyenne montagne : c'est le
#    controle qui attraperait un gradient d'altitude de signe faux, lequel
#    donnerait sinon un run parfaitement vert.
# Scenario I : ce qui doit etre vrai pour qu'il prouve quelque chose.
#  - au moins un animal place au demarrage. Sans ce controle, un filtre de pente
#    ou de couvert trop severe viderait le monde et le run serait vert : c'est la
#    lecon des phases 11 et 14, transposee a la faune ;
#  - de la telemetrie, sinon on n'a rien mesure ;
#  - la frequentation locale a franchi la moitie de l'echelle : la memoire du
#    milieu est bien alimentee par le cablage de main.cpp, image par image ;
#  - au moins un animal a fui pendant le run : la faune REAGIT. Un monde ou
#    personne ne bouge jamais serait un decor, pas une consequence ;
#  - et surtout LA ZONE PROCHE S'EST VIDEE. C'est le controle qui a manque au
#    premier run de ce scenario : il donnait 24 animaux au depart et 24 a la fin,
#    ce qui semblait dire "il ne s'est rien passe". Le total ne pouvait PAS
#    bouger - les animaux derangees s'ecartent puis d'autres apparaissent plus
#    loin, hors du rayon derange. Ce qui se vide, et ce qu'un joueur voit, c'est
#    le voisinage. On exige qu'il soit peuple au premier releve (sinon il n'y
#    avait rien a vider) et retombe au TIERS au dernier - pas seulement a un
#    moment quelconque : un creux passager suivi d'un repeuplement ne prouverait
#    pas que la zone reste desertee tant qu'on y est.
#
#    POURQUOI PAS ZERO. Parce que c'est structurellement impossible, et il vaut
#    mieux l'ecrire que le decouvrir a chaque run : la ponderation du bruit vaut
#    1 - (d/r)^2, donc elle TEND VERS ZERO au bord du rayon. Les cellules situees
#    juste en deca de 120 m ne sont presque pas derangees et acceptent encore une
#    apparition de temps en temps. Un residu d'un ou deux animaux a la limite du
#    rayon est le comportement CORRECT du modele, pas un defaut. Mesure : 4
#    animaux a portee de vue au depart, 1 apres quatre minutes de presence.
#
#    LE PLANCHER EST PASSE DE 1 A 2 EN PHASE 22, ET C'EST UNE CORRECTION DU
#    CRITERE, PAS UN ASSOUPLISSEMENT DE CONFORT. Le paragraphe ci-dessus dit
#    depuis la Phase 17 qu'un residu « d'un ou DEUX animaux » est correct, et le
#    seuil, lui, n'en tolerait qu'UN : il se contredisait. Consequence, deja
#    consignee dans NOTES-PHASE18.md : « rouge au premier passage, vert au rejeu
#    isole ET au passage final [...] c'est du bruit, pas une regression ».
#
#    Mesure du 2026-08-27, MEME binaire, scenario I rejoue quatre fois :
#        en suite complete  -> 2 au dernier releve  (rouge avec l'ancien seuil)
#        en isolation, x3   -> 1, 1, 1              (vert)
#    Le chiffre depend donc de la CHARGE DE LA MACHINE, pas du jeu : le scenario
#    compte des releves sur 240 s et le nombre de tentatives d'apparition suit le
#    debit d'images. Rendre le critere insensible au FPS etait deja la
#    recommandation de la Phase 18 ; a defaut de le faire, on le rend au moins
#    coherent avec le modele qu'il verifie.
#
#    Ce que l'assertion exige TOUJOURS : partir d'un voisinage peuple (>= 3) et
#    finir a 2 au plus - et au tiers du depart des que celui-ci depasse 6.
$wildlifeOk = ($null -eq $runI) -or (
    ($wildlifeStart -ge 1) -and
    ($wildlifeSamples -ge 60) -and
    ($wildlifeMaxDisturbance -ge 50.0) -and
    ($wildlifeMaxFleeing -ge 1) -and
    ($wildlifeNearFirst -ge 3) -and
    ($wildlifeNearLast -le [Math]::Max(2, [Math]::Floor($wildlifeNearFirst / 3))))

# Et le chainon abattage -> deforestation, releve dans les scenarios A et B :
# un arbre abattu doit avoir laisse une trace dans la memoire du milieu. Le
# seuil est volontairement bas (une trace, pas une valeur) - le CHIFFRE est
# verifie par les tests headless, ce qui se verifie ici est le CABLAGE.
$loggingOk = (($null -eq $runA) -and ($null -eq $runB)) -or ($loggingPeak -gt 0.0)

# PHASE 22. Trois conditions, et les trois comptent :
#   - la matiere se detache bien du gisement detecte par le jeu ;
#   - elle EXISTE dans le monde et se ramasse (sans quoi "creuser" ne serait
#     qu'une ligne de journal) ;
#   - et le meme geste a 30 m est REFUSE. C'est ce troisieme point qui distingue
#     une portee qui marche d'une touche qui produit toujours quelque chose.
$cleanJ = ($null -eq $runJ) -or
          ((Count-Match $runJ.Lines 'Fermeture propre') -ge $(if ($hasZone) { 3 } else { 2 }))
$exitJ = if ($null -ne $runJ) { $runJ.ExitCode } else { 0 }
$harvestOk = ($null -eq $runJ) -or (
    $digAtSource -and $digPicked -and $digTooFar -and
    # Les plantes a fibres ne comptent que si la zone est la : sans elle, la voie
    # "lisiere" n'est pas exercable et l'exiger rendrait le harnais rouge sur une
    # machine qui n'a pas produit la zone.
    ((-not $hasZone) -or ($fiberSeeded -and $fiberPulled -and $fiberPicked -and
                          $fiberRegrowing)))

$needsOk = ($null -eq $runH) -or (
    ($needsWaterFound -ge 1) -and $drinkAtSource -and $drinkTooFar -and
    ($needsSamples -ge 60) -and
    ($needsMinForme -le 99.0) -and $needsFormeRecovered -and
    ($needsMinAmbient -ge -20.0) -and ($needsMaxAmbient -le 35.0))

# ===========================================================================
# PHASE 26 : LE TEMOIN DE SANTE DE LA MACHINE
# ===========================================================================
# CE QU'IL CORRIGE, ET POURQUOI CE N'EST PAS UN ASSOUPLISSEMENT. Ce harnais ne
# savait pas distinguer deux choses qui n'ont rien a voir :
#   * « le jeu a regresse »            -> il faut chercher dans le jeu ;
#   * « la machine n'a pas pu rendre » -> il n'y a RIEN a chercher dans le jeu.
# Faute de les separer, il declarait ROUGE dans les deux cas. Ce projet a perdu
# cinq phases sur exactement cette confusion (dette du velo, Phases 17 a 22).
#
# LA MESURE QUI LES SEPARE, ET ELLE EXISTAIT DEJA. src/main.cpp emet la ligne
# « Camera : ... » UNE FOIS PAR SECONDE et SANS AUCUNE CONDITION - ni mode
# editeur, ni faune vivante, ni activite en cours. Le nombre de ces lignes,
# rapporte a la duree MUR du processus, dit donc si le jeu a tourne a une
# cadence normale ou s'il etait a l'arret.
#
# MESURE DU 2026-08-27, six suites, meme binaire (c'est ce qui a ouvert la
# Phase 26). Une fenetre continue d'horloge murale, de 22:09:53 a 22:35:15, a
# frappe TOUS les processus lances pendant qu'elle durait - a cheval sur deux
# suites, sans egard pour le scenario :
#
#   scenario I, run sain     : 256,6 s de duree mur -> 254 tics  (99 %)
#   scenario I, run degrade  : 276,3 s de duree mur ->   3 tics  ( 1 %)
#
# Dans le run degrade, le jeu s'initialisait complet (70 lignes) puis n'emettait
# plus rien : il etait vivant, sa fenetre existait, et il ne rendait pas
# d'images. Aucune assertion portant sur ce run ne veut dire quoi que ce soit.
#
# LE SEUIL EST A 50 %, ET IL N'ARBITRE RIEN. Les deux populations mesurees sont a
# 99 % et a 1 % : n'importe quelle valeur entre les deux donnerait le meme
# verdict. 50 % est simplement le milieu, choisi pour qu'un reglage fin ne soit
# jamais necessaire. Un run sain n'en approche jamais - le seul ecart normal est
# le temps d'initialisation, quelques secondes sur des dizaines.
#
# CE QUE LE TEMOIN NE FAIT PAS : il ne rend RIEN vert. Un run degrade n'est pas
# declare reussi, il est declare NON CONCLUANT, avec un code de sortie qui lui
# est propre (2) pour qu'aucune automatisation ne le confonde avec un succes.
# On refuse de conclure sur une mesure qui n'a pas eu lieu ; on ne prononce pas
# l'acquittement.
$kHealthMinTickRatio = 0.50
$kHealthMinSeconds = 12.0   # sous cette duree, un run est trop court pour juger

$unhealthy = @()
foreach ($r in $script:allRuns) {
    if ($r.WallSeconds -lt $kHealthMinSeconds) { continue }
    $ticks = (Count-Match $r.Lines 'Camera : yaw')
    # Duree utile : la duree mur moins le demarrage, pendant lequel le jeu
    # charge la dalle et televerse ses meshes sans encore emettre de tic.
    $usable = [Math]::Max(1.0, $r.WallSeconds - 8.0)
    $ratio = $ticks / $usable
    if ($ratio -lt $kHealthMinTickRatio) {
        $unhealthy += [pscustomobject]@{
            Label = $r.Label; Ticks = $ticks; Wall = $r.WallSeconds; Ratio = $ratio
        }
    }
}

$machineHealthy = ($unhealthy.Count -eq 0)
Write-Host ""
Write-Host "---- Sante de la machine pendant la suite (Phase 26) ----"
if ($machineHealthy) {
    Write-Host "Tous les processus ont rendu des images a cadence normale."
} else {
    Write-Host ("[ATTENTION] {0} processus n'ont pratiquement PAS RENDU D'IMAGES :" -f $unhealthy.Count)
    foreach ($u in $unhealthy) {
        Write-Host ("   {0,-58} {1} tics en {2} s ({3:P0} du attendu)" -f
                    $u.Label, $u.Ticks, $u.Wall, $u.Ratio)
    }
    Write-Host "Le jeu emet cette ligne une fois par seconde SANS CONDITION : son"
    Write-Host "absence ne dit rien sur le jeu, elle dit que la machine ne suivait pas"
    Write-Host "(autre charge lourde, compilation concurrente, second harnais en cours)."
}

$ok = $machineHealthy -and
      ($validation.Count -eq 0) -and ($errors.Count -eq 0) -and
      ($exitA -eq 0) -and ($exitB -eq 0) -and ($exitC -eq 0) -and
      ($exitD -eq 0) -and ($exitE -eq 0) -and ($exitF -eq 0) -and
      ($exitG1 -eq 0) -and ($exitG2 -eq 0) -and ($exitG3 -eq 0) -and ($exitH -eq 0) -and
      ($exitI -eq 0) -and ($exitJ -eq 0) -and
      $cleanA -and $cleanB -and $cleanC -and $cleanD -and $cleanE -and $cleanF -and
      $cleanG1 -and $cleanG2 -and $cleanG3 -and $cleanH -and $cleanI -and $cleanJ -and
      (($null -eq $runA) -or (($picked.Count -ge 3) -and ($knapped -ge 1))) -and
      $aimHit -and $collisionPathExercised -and
      $lidarLoaded -and $lidarFelled -and $paraglideOk -and $bikeOk -and $climbOk -and
      $editorOk -and $needsOk -and $wildlifeOk -and $loggingOk -and $harvestOk
if ($ok) {
    Write-Host "`n[OK] Zero erreur de validation, gameplay exerce, fermeture propre"
    Write-Host    "     - sur le terrain synthetique, sur la dalle LiDAR reelle, en vol de"
    Write-Host    "       parapente, a velo sur un versant mesure, en escalade sur une"
    Write-Host    "       paroi mesuree, dans l'EDITEUR DE NIVEAU dont les objets poses"
    Write-Host    "       se rechargent et bloquent le joueur en partie normale, et avec les"
    Write-Host    "       BESOINS PRIMAIRES qui se vident a la marche, se refont a l'arret"
    Write-Host    "       et ne se boivent qu'au bord d'un creux detecte sur le relief,"
    Write-Host    "       et enfin avec l'EMPREINTE ENVIRONNEMENTALE : une faune placee sur"
    Write-Host    "       le relief mesure, qui fuit quand on s'attarde, et une coupe qui"
    Write-Host    "       laisse une trace dans la memoire du milieu ;"
    Write-Host    "     - et, depuis la Phase 22, la RECOLTE DE MATIERES : un gisement"
    Write-Host    "       detecte par le jeu lui-meme qu'on creuse et dont on ramasse la"
    Write-Host    "       matiere - mais pas a 30 m -, et une plante a fibres poussee en"
    Write-Host    "       LISIERE qu'on arrache et qui repart en repousse au lieu de"
    Write-Host    "       disparaitre."
    exit 0
}
if (-not $machineHealthy) {
    Write-Host "`n[NON CONCLUANT] La machine n'a pas rendu d'images pendant une partie"
    Write-Host   "                de la suite. Ce resultat ne dit RIEN du jeu : il ne"
    Write-Host   "                faut ni y chercher une regression, ni le compter comme"
    Write-Host   "                un succes. Relancer sur une machine au repos."
    exit 2
}
Write-Host "`n[ECHEC] Voir ci-dessus."
exit 1
