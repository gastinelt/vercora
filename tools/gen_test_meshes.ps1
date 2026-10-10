# ---------------------------------------------------------------------------
# Generateur d'assets .obj de test pour la Phase 3 (loader de mesh).
#
# Produit deux fichiers dans assets/ , volontairement minuscules (quelques Ko) :
#
#   icosphere.obj  - icosphere subdivisee 1 fois : 42 sommets / 80 triangles,
#                    normales LISSES (normale = position normalisee) + UV
#                    spheriques. Teste le cas "beaucoup de faces, normales et
#                    UV presents, indices v/vt/vn identiques".
#
#   octahedron.obj - octaedre a facettes PLATES : 6 positions seulement mais 8
#                    normales distinctes. Chaque position est donc reutilisee
#                    par plusieurs faces avec des normales differentes -> teste
#                    la deduplication du loader sur le triplet (v, vt, vn),
#                    cas que l'icosphere ne couvre pas.
#
# Usage :  powershell -ExecutionPolicy Bypass -File tools\gen_test_meshes.ps1
# ---------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'

$assetsDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'assets'
if (-not (Test-Path $assetsDir)) {
    New-Item -ItemType Directory -Path $assetsDir | Out-Null
}

# Culture invariante : sur une machine en locale FR, "{0:F6}" produirait des
# virgules decimales, illisibles pour un parseur .obj.
$inv = [System.Globalization.CultureInfo]::InvariantCulture
function Fmt([double]$v) { return $v.ToString('F6', $inv) }

function Write-ObjFile([string]$path, [string[]]$lines) {
    # UTF8 sans BOM : un BOM en tete de fichier n'est pas garanti d'etre tolere
    # par tous les parseurs .obj.
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllLines($path, $lines, $enc)
    Write-Host ("Ecrit : {0} ({1} lignes)" -f $path, $lines.Count)
}

# ---------------------------------------------------------------------------
# 1. Icosphere (icosaedre + 1 subdivision, projetee sur la sphere unite)
# ---------------------------------------------------------------------------
$positions = New-Object System.Collections.ArrayList
$midCache  = @{}   # cle "i_j" -> index du milieu deja cree

function Add-Normalized([double]$x, [double]$y, [double]$z) {
    $len = [math]::Sqrt($x * $x + $y * $y + $z * $z)
    # Parentheses obligatoires autour de chaque element : dans un @(...), la
    # virgule de PowerShell est PLUS prioritaire que les operateurs arithmetiques,
    # donc "@($x / $len, $y / $len)" se lirait "$x / ($len, $y) / $len".
    [void]$positions.Add(@(($x / $len), ($y / $len), ($z / $len)))
    return $positions.Count - 1
}

# Milieu de l'arete (a,b), projete sur la sphere. Mis en cache pour que les
# deux triangles partageant l'arete reutilisent le meme sommet (mesh soude).
function Get-Midpoint([int]$a, [int]$b) {
    $key = if ($a -lt $b) { "$($a)_$($b)" } else { "$($b)_$($a)" }
    if ($midCache.ContainsKey($key)) { return $midCache[$key] }

    $pa = $positions[$a]
    $pb = $positions[$b]
    $idx = Add-Normalized (($pa[0] + $pb[0]) / 2) (($pa[1] + $pb[1]) / 2) (($pa[2] + $pb[2]) / 2)
    $midCache[$key] = $idx
    return $idx
}

# Nombre d'or : les 12 sommets de l'icosaedre sont 3 rectangles d'or orthogonaux.
$t = (1.0 + [math]::Sqrt(5.0)) / 2.0

$icoVerts = @(
    @(-1,  $t,  0), @( 1,  $t,  0), @(-1, -$t,  0), @( 1, -$t,  0),
    @( 0, -1,  $t), @( 0,  1,  $t), @( 0, -1, -$t), @( 0,  1, -$t),
    @( $t,  0, -1), @( $t,  0,  1), @(-$t,  0, -1), @(-$t,  0,  1)
)
foreach ($v in $icoVerts) {
    # Variables intermediaires : "[double]$v[0]" se lit "([double]$v)[0]" en
    # PowerShell, ce qui casserait le cast.
    $vx = [double]$v[0]
    $vy = [double]$v[1]
    $vz = [double]$v[2]
    [void](Add-Normalized $vx $vy $vz)
}

# 20 faces de l'icosaedre, enroulement anti-horaire vu de l'exterieur.
$faces = @(
    @(0, 11, 5), @(0, 5, 1),  @(0, 1, 7),   @(0, 7, 10), @(0, 10, 11),
    @(1, 5, 9),  @(5, 11, 4), @(11, 10, 2), @(10, 7, 6), @(7, 1, 8),
    @(3, 9, 4),  @(3, 4, 2),  @(3, 2, 6),   @(3, 6, 8),  @(3, 8, 9),
    @(4, 9, 5),  @(2, 4, 11), @(6, 2, 10),  @(8, 6, 7),  @(9, 8, 1)
)

# Une passe de subdivision : chaque triangle -> 4 triangles.
$subdivided = New-Object System.Collections.ArrayList
foreach ($f in $faces) {
    $a = Get-Midpoint $f[0] $f[1]
    $b = Get-Midpoint $f[1] $f[2]
    $c = Get-Midpoint $f[2] $f[0]
    [void]$subdivided.Add(@($f[0], $a, $c))
    [void]$subdivided.Add(@($f[1], $b, $a))
    [void]$subdivided.Add(@($f[2], $c, $b))
    [void]$subdivided.Add(@($a, $b, $c))
}

$lines = New-Object System.Collections.ArrayList
[void]$lines.Add('# Vercora - asset de test Phase 3 : icosphere (1 subdivision)')
[void]$lines.Add(('# {0} sommets, {1} triangles, normales lissees + UV spheriques' -f $positions.Count, $subdivided.Count))
[void]$lines.Add('# Genere par tools/gen_test_meshes.ps1 - ne pas editer a la main')
[void]$lines.Add('o Icosphere')

# Note : la chaine est construite dans une variable avant l'appel a .Add().
# Dans les parentheses d'un appel de methode .NET, la virgule separe les
# ARGUMENTS de la methode : ecrire .Add('{0} {1}' -f $a, $b) appellerait
# Add() avec 2 arguments au lieu de formater 2 valeurs.
foreach ($p in $positions) {
    $line = 'v {0} {1} {2}' -f (Fmt $p[0]), (Fmt $p[1]), (Fmt $p[2])
    [void]$lines.Add($line)
}
# Sphere unite centree sur l'origine : la normale d'un sommet EST sa position.
foreach ($p in $positions) {
    $line = 'vn {0} {1} {2}' -f (Fmt $p[0]), (Fmt $p[1]), (Fmt $p[2])
    [void]$lines.Add($line)
}
# Mapping equirectangulaire (longitude/latitude). Pas encore utilise par le
# shader, mais present pour verifier que le loader lit bien les UV.
foreach ($p in $positions) {
    $u = 0.5 + [math]::Atan2($p[2], $p[0]) / (2.0 * [math]::PI)
    $vv = 0.5 - [math]::Asin([math]::Max(-1.0, [math]::Min(1.0, $p[1]))) / [math]::PI
    $line = 'vt {0} {1}' -f (Fmt $u), (Fmt $vv)
    [void]$lines.Add($line)
}
foreach ($f in $subdivided) {
    $i = $f[0] + 1; $j = $f[1] + 1; $k = $f[2] + 1
    [void]$lines.Add("f $i/$i/$i $j/$j/$j $k/$k/$k")
}

Write-ObjFile (Join-Path $assetsDir 'icosphere.obj') $lines.ToArray()

# ---------------------------------------------------------------------------
# 2. Octaedre a facettes plates
# ---------------------------------------------------------------------------
# 6 positions (les pointes sur les axes), 8 faces triangulaires. La normale de
# chaque face est (+/-1, +/-1, +/-1) normalisee : les 3 sommets d'une face
# partagent SA normale, et chaque position apparait dans 4 faces avec 4
# normales differentes.
$octPos = @(
    @( 1, 0, 0), @(-1, 0, 0),   # 1, 2 : +X, -X
    @( 0, 1, 0), @( 0,-1, 0),   # 3, 4 : +Y, -Y
    @( 0, 0, 1), @( 0, 0,-1)    # 5, 6 : +Z, -Z
)
# Chaque face : les 3 positions (indices .obj 1-based) + le signe de sa normale.
$octFaces = @(
    @(1, 3, 5,  1,  1,  1), @(3, 2, 5, -1,  1,  1),
    @(2, 4, 5, -1, -1,  1), @(4, 1, 5,  1, -1,  1),
    @(3, 1, 6,  1,  1, -1), @(2, 3, 6, -1,  1, -1),
    @(4, 2, 6, -1, -1, -1), @(1, 4, 6,  1, -1, -1)
)

$lines = New-Object System.Collections.ArrayList
[void]$lines.Add('# Vercora - asset de test Phase 3 : octaedre a facettes plates')
[void]$lines.Add('# 6 positions / 8 normales distinctes / 8 triangles')
[void]$lines.Add('# Chaque position est reutilisee avec plusieurs normales : teste la')
[void]$lines.Add('# deduplication du loader sur le triplet (position, uv, normale).')
[void]$lines.Add('# Genere par tools/gen_test_meshes.ps1 - ne pas editer a la main')
[void]$lines.Add('o OctahedronFlat')

foreach ($p in $octPos) {
    $line = 'v {0} {1} {2}' -f (Fmt $p[0]), (Fmt $p[1]), (Fmt $p[2])
    [void]$lines.Add($line)
}
$invSqrt3 = 1.0 / [math]::Sqrt(3.0)
foreach ($f in $octFaces) {
    $line = 'vn {0} {1} {2}' -f (Fmt ($f[3] * $invSqrt3)), (Fmt ($f[4] * $invSqrt3)), (Fmt ($f[5] * $invSqrt3))
    [void]$lines.Add($line)
}
# Pas de 'vt' ici : verifie aussi que le loader accepte un .obj SANS UV
# (format "f v//vn").
$n = 0
foreach ($f in $octFaces) {
    $n++
    $line = 'f {0}//{3} {1}//{3} {2}//{3}' -f $f[0], $f[1], $f[2], $n
    [void]$lines.Add($line)
}

Write-ObjFile (Join-Path $assetsDir 'octahedron.obj') $lines.ToArray()
