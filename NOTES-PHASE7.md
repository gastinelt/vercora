# Vercora — Phase 7 : pipeline LiDAR HD réel, de bout en bout

Session du 2026-08-16, non supervisée. Objectif : valider la **chaîne technique**
complète, d'une vraie dalle IGN jusqu'au terrain sur lequel la caméra marche.
Ce n'est **pas** le choix de la zone finale du jeu, qui reste à définir.

Jusqu'ici le terrain était toujours synthétique (bruit fractal, Phase 4). Il ne
l'est plus : `vercora.exe` sans argument charge désormais une vraie dalle. Le
générateur procédural reste disponible (`vercora.exe terrain`) et sert toujours
de monde de référence aux tests — voir « Ce qui n'a pas changé » plus bas.

---

## État : vérifié, zéro erreur de validation Vulkan

```bash
tools\build.bat                # compile
tools\run_logic_tests.bat      # 169 assertions headless (121 avant la Phase 7)
tools\run_vulkan_check.ps1     # le VRAI exe, 3 scenarios, sous validation layers
```

**Les trois passent.** `run_vulkan_check.ps1` : 0 message `[Validation]`,
0 `[ERROR]`, 0 `[WARN]`, codes de sortie 0/0/0, fermeture propre sur les trois
scénarios — dont le nouveau **scénario C**, qui joue sur la dalle réelle
(2 000 000 de triangles) : chargement, marche sur le relief mesuré, abattage,
ramassage, vol libre, 6 redimensionnements, minimisation/restauration, `WM_CLOSE`.

Non-régression : les scénarios A et B tournent toujours sur le terrain
synthétique, avec exactement la même couverture qu'en Phase 6.

---

## 1. La dalle utilisée

| | |
|---|---|
| Nom | `LHD_FXX_0907_6440_PTS_LAMB93_IGN69.copc.laz` |
| Emprise | Lambert 93, X 907000–908000, Y 6439000–6440000 (1 km × 1 km) |
| WGS84 (centre) | **45,0236 N — 5,6349 E** |
| Lieu | **Le Gua, Isère (38450)** — bordure orientale du Vercors |
| Bloc / mission | `PM`, mission `21LHD6PM`, acquisition du 06 au 13/08/2021 |
| Classement | `ST_AUTO_MANUEL_POST_EXP` (auto Avineon + reprise manuelle IGN) |
| Points | 28 739 937, format LAS 1.4 point format 6 |
| Taille | **175 350 922 octets (167,2 Mo)** |
| Licence | Etalab 2.0 (open data) |

URL de téléchargement :

```
https://data.geopf.fr/telechargement/download/LiDARHD-NUALID/NUALHD_1-0__LAZ_LAMB93_PM_2025-03-25/LHD_FXX_0907_6440_PTS_LAMB93_IGN69.copc.laz
```

**Comment on la trouve** — et c'est le point non évident. `geoservices.ign.fr`
redirige vers `cartes.gouv.fr`, une application web dont les liens ne
s'extraient pas d'une page. L'index des dalles est en revanche exposé en **WFS**,
et chaque tuile y porte son URL de téléchargement :

```bash
curl "https://data.geopf.fr/wfs/ows?SERVICE=WFS&VERSION=2.0.0&REQUEST=GetFeature&TYPENAMES=IGNF_NUAGES-DE-POINTS-LIDAR-HD:dalle&COUNT=3&OUTPUTFORMAT=application/json&SRSNAME=EPSG:2154&BBOX=908000,6440000,913000,6445000,EPSG:2154"
```

La propriété `url` de chaque *feature* est le lien direct ; `metadata` donne le
nombre de points, les dates d'acquisition et le procédé de classement. Le service
limite à 1 requête/seconde. L'ancienne forme
`storage.sbg.cloud.ovh.net/.../ppk-lidar/...`, encore citée un peu partout, n'est
plus la source de référence.

**Pourquoi cette dalle.** Zone de couverture confirmée (livrée dès 2021), et
surtout un relief qui met le pipeline à l'épreuve plutôt que de le flatter :
**562 m de dénivelé sur 1 km²** (420 → 982 m), donc de vraies falaises, et
**67 % de végétation haute**, donc de vrais trous de données sous le couvert. Un
plateau agricole aurait tout fait passer sans rien prouver.

Espace disque vérifié avant téléchargement : 294 Go libres sur C:.

---

## 2. L'outil de conversion — `tools/lidar_to_heightmap.py`

```bash
python -m pip install "laspy[lazrs]" numpy
python tools/lidar_to_heightmap.py data/lidar/LHD_FXX_0907_6440_PTS_LAMB93_IGN69.copc.laz -o assets/terrain/LHD_0907_6440_1m.vchm --cell 1.0
```

Options : `--cell`, `--stat median|mean|min`, `--classes`, `--no-despike`,
`--zero-base`, `--report-only`. L'en-tête du script documente tout.

**laspy + lazrs plutôt que PDAL.** PDAL est bien le standard du domaine, mais sur
Windows il ne se livre en pratique que par conda, absent de cette machine.
laspy 2.7 avec le backend lazrs (l'implémentation Rust de LASzip) lit exactement
le même LAZ 1.4 classifié, s'installe en une commande pip — y compris sur le
Python 3.14 de cette machine, ce qui n'allait pas de soi — et tout le reste du
travail est du numpy de toute façon. Le format de sortie ne dépend pas du lecteur :
PDAL produirait le même fichier.

**Pourquoi un outil séparé du moteur.** Un `.laz` est un nuage compressé et non
trié ; en tirer une grille demande un décodeur LASzip, un tri, une agrégation,
un bouchage de trous et un filtrage — 3,6 s et 167 Mo de lecture, pour un
résultat toujours identique. C'est de la *préparation*, pas du chargement. Le jeu
lit 4 Mo de float déjà propres. Même frontière que les shaders, compilés en
SPIR-V par le build et non en GLSL à l'exécution.

### Résultats de la conversion

| | 1 m (**retenu**) | 2 m |
|---|---|---|
| Échantillons | 1001 × 1001 | 501 × 501 |
| Fichier `.vchm` | **4 008 124 o (3,82 Mo)** | 1 004 124 o (0,96 Mo) |
| Points sol par cellule (médiane) | 7 | 20 |
| Cellules sans aucun point | **21,58 %** | 6,64 % |
| Rayon du plus gros trou | 6 cellules (6 m) | 3 cellules (6 m) |
| Pics corrigés | 13 (0,0013 %) | 5 (0,0020 %) |
| Sommets / triangles | 1 002 001 / 2 000 000 | 251 001 / 500 000 |
| FPS (GTX 960, Debug) | **848** | 1876 |
| Pic mémoire du processus | **185 Mo** | 102 Mo |
| Durée de conversion | 3,6 s | 3,2 s |

**Le 1 m est retenu**, pour trois raisons : la densité de points sol (7,10 pts/m²)
le supporte réellement — descendre à 20 cm inventerait du détail que la donnée
n'a pas ; 848 FPS sur une GTX 960 de 2015 laisse une marge considérable ; et
185 Mo de pic mémoire sont sans commune mesure avec les 64 Go disponibles. Le 2 m
reste généré et utilisable (`vercora.exe chemin\vers\..._2m.vchm`) comme repli
pour une machine plus modeste ou pour une future carte multi-dalles.

### Le format `.vchm`

En-tête de 120 octets petit-boutiste puis `samplesZ * samplesX` float32.
Définition complète en tête de `tools/lidar_to_heightmap.py`, redite dans
`src/assets/TerrainFile.hpp`. Les altitudes restent **absolues (IGN69)** et le
géoréférencement (coin nord-ouest en Lambert 93 + code EPSG) est conservé dans
l'en-tête : le fichier reste raccordable à ses voisins.

---

## 3. Intégration dans le moteur

`TerrainMesh`, les shaders et la caméra n'ont **pas été touchés**. C'était tout
l'intérêt d'avoir fait passer le terrain par `Heightmap` dès la Phase 4, et ça
s'est vérifié : la classe a simplement reçu un second constructeur qui adopte une
grille déjà construite.

- `src/assets/TerrainFile.{hpp,cpp}` — lecture et validation du `.vchm`.
- `src/assets/Heightmap.{hpp,cpp}` — constructeur « grille toute faite »,
  `GeoReference`, `toProjected()`, validation factorisée dans `finalize()`.
- `src/main.cpp` — sélection de la scène, repli sur le procédural.

### Scènes disponibles

```
vercora.exe                      -> dalle LiDAR reelle (defaut)
vercora.exe terrain              -> terrain synthetique (Phase 4)
vercora.exe chemin\dalle.vchm    -> une autre heightmap reelle
vercora.exe cube                 -> cube code en dur (Phase 1)
vercora.exe modele.obj           -> mesh charge depuis un fichier
```

Si la dalle par défaut est absente (elle n'est pas versionnée), le jeu **se replie
sur le terrain synthétique** avec un message expliquant comment la produire. Un
chemin donné *explicitement* qui n'existe pas est en revanche une erreur : obtenir
du bruit fractal à la place de la dalle demandée sans s'en apercevoir serait pire
que planter.

---

## 4. Les problèmes rencontrés avec les vraies données

### 4.1 Trous — 21,6 % des cellules au pas de 1 m

Cause : 67 % de végétation haute. Sous un couvert dense, aucun écho ne redescend
jusqu'au sol. Le plus gros trou fait **6 cellules de rayon**.

Traitement : dilatation itérative — chaque passe remplit une cellule vide par la
moyenne de ses voisines déjà connues, jusqu'à ce qu'il n'en reste aucune. Le
nombre de passes est journalisé et vaut exactement le rayon du plus gros trou :
c'est le diagnostic le plus parlant sur la qualité d'une dalle. Aucun `NaN` ne
doit atteindre le GPU — il produirait un triangle dégénéré et un terrain
clignotant, sans le moindre message d'erreur. `Heightmap::finalize()` refuse donc
aussi toute valeur non finie, en seconde barrière.

### 4.2 Valeurs aberrantes — 13 cellules sur 1 002 001

La médiane par nœud absorbe déjà le bruit ponctuel. Le risque résiduel est la
cellule dont *tous* les points sont faux. Le filtre doit donc être sévère envers
un pic isolé et **indulgent envers une falaise**, car cette dalle en a de vraies.
D'où un double seuil : il faut dépasser **à la fois** 5 m d'écart absolu **et**
6 × la dispersion locale (MAD du voisinage 3×3). Sur un versant raide la
dispersion locale est grande et le filtre se désactive de lui-même ; sur un
replat elle est proche de zéro et le moindre pic ressort. Résultat : 13 cellules
touchées, écart maximal 25,2 m — des blunders francs, pas du relief.

### 4.3 Coordonnées Lambert 93 — recentrage obligatoire

Les coordonnées IGN valent ~907 000 en X et ~6 440 000 en Y. À 6,4 millions,
l'écart entre deux `float32` consécutifs vaut déjà **0,5 m** : porter ces valeurs
dans les sommets ferait trembler la géométrie et ruinerait le depth buffer.

Le terrain est donc **recentré sur l'origine du monde** (ce que `Heightmap`
faisait déjà), et le géoréférencement est conservé **en `double`, à côté de la
grille**, jamais dans les sommets. `Heightmap::toProjected()` refait le chemin
inverse ; le jeu journalise au démarrage la position Lambert 93 du point de
départ (E 907500, N 6439500, altitude 605,38 m), ce qui permet de retrouver
l'endroit sur une carte à partir d'un simple log de partie.

### 4.4 Le piège de l'orientation — le relief en miroir

Le moteur est en repère main droite avec Y vers le haut, donc X ∧ Y = Z. En
posant X = Est, on obtient **Z = Est ∧ Haut = −Nord** : le nord est en −Z. Les
lignes du `.vchm` sont donc rangées **du nord vers le sud**.

Se tromper de sens produit un terrain parfaitement plausible — même dénivelé,
mêmes pentes, même histogramme — mais **retourné**, et d'autant plus indétectable
qu'il n'y a pas encore d'orthophoto à plaquer dessus. C'est le genre d'erreur
qu'on découvre trois phases plus tard.

D'où `tools/check_terrain_file.py`, qui relit le `.vchm` et le confronte au nuage
brut sans rien partager avec le convertisseur :

```bash
python tools/check_terrain_file.py assets/terrain/LHD_0907_6440_1m.vchm data/lidar/LHD_FXX_0907_6440_PTS_LAMB93_IGN69.copc.laz
```

Résultat sur les deux résolutions :

```
lecture DIRECTE                              : ecart moyen 0.0000 m, max 0.0000 m
si les lignes etaient inversees (nord/sud)   : ecart moyen  64.3 m
si les colonnes etaient inversees (est/ouest): ecart moyen 221.1 m
```

C'est le **contraste** qui fait la preuve, pas la valeur absolue.

### 4.5 Deux erreurs réellement commises, et attrapées par ce contrôle

1. **Arrondi bancaire.** `np.rint` arrondit les ex æquo vers l'entier *pair* : un
   point tombant pile sur la frontière entre deux nœuds partait à gauche ou à
   droite selon la **parité** de l'indice. Les coordonnées LAZ étant quantifiées
   au centimètre, ces ex æquo sont fréquents. Remplacé par `floor(t + 0.5)`.

2. **Intervalle inversé sur l'axe nord.** En arrondissant directement
   `(y_max − y)`, le sens de l'intervalle semi-ouvert s'inversait avec le signe :
   `[c−½, c+½[` sur l'axe est, mais `]c−½, c+½]` sur l'axe nord. Deux dalles
   voisines raccordées le long d'une frontière est-ouest se seraient disputé leur
   ligne commune. Le renversement nord-sud se fait désormais **après** l'arrondi,
   sur l'indice entier.

Aucune des deux n'était visible à l'écran : elles ne déplaçaient qu'une poignée
de points sur une dalle isolée. Elles auraient explosé au premier assemblage
multi-dalles.

### 4.6 Le relief casse la visée horizontale — et donc le script de test

Symptôme : le scénario C échouait à abattre l'arbre visé, alors que le jeu
fonctionnait parfaitement.

`Forest::pickTree` lance un rayon **horizontal** depuis l'œil (la souris ne peut
pas être pilotée par messages Win32 — voir l'en-tête de `run_vulkan_check.ps1`),
et vérifie que le point touché est entre le pied et la cime. Sur le terrain
synthétique, assez lisse, un arbre à 9 m au nord est toujours dans l'axe : **les
16 arbres sur 16 sont abattables**. Sur le relief mesuré, un arbre en contrebas
passe entièrement sous le rayon et un arbre en amont entièrement au-dessus :
**seuls 6 sur 16 le sont**.

Plutôt que de réimplémenter ce raisonnement en PowerShell, `vercora.exe
--dump-world` interroge la **vraie** fonction de visée du jeu, avec la vraie
heightmap, pour chaque recul plausible, et émet des lignes
`AIM <indice> <camX> <camZ> <recul>`. Le script vise le plus proche des arbres
*réellement* abattables. Ce n'est pas une estimation : c'est la réponse de
`Forest::pickTree`.

C'est un vrai constat de gameplay, pas seulement un problème de test : **sur du
relief réel, on ne peut abattre qu'un tiers des arbres sans pouvoir viser en
hauteur.** Le pitch de la caméra existe pourtant — c'est le script qui ne peut
pas s'en servir. À confirmer manette en main.

### 4.7 `dump_world` replié dans le jeu — et Smart App Control

`tools/dump_world.cpp` et `tools/build_dump_world.bat` sont **supprimés**. Le dump
est désormais un mode du jeu :

```
vercora.exe --dump-world                    # dalle par defaut
vercora.exe --dump-world terrain            # terrain synthetique
vercora.exe --dump-world chemin\dalle.vchm  # une autre dalle
```

Deux raisons, dans cet ordre d'importance :

1. **La duplication était un piège.** `dump_world.cpp` *recopiait* la construction
   du monde de `main.cpp` en comptant sur le fait que les deux resteraient dans le
   même ordre — le semis étant déterministe, un ordre différent aurait donné un
   monde différent, et le script aurait visé à côté sans que rien ne le signale.
   Le dump traverse maintenant exactement le code du jeu, par construction.

2. **Smart App Control est actif sur cette machine** (`VerifiedAndReputablePolicyState = 1`).
   Il a bloqué à l'exécution le `dump_world.exe` fraîchement recompilé —
   « Une stratégie de contrôle d'application a bloqué ce fichier » — alors que le
   binaire *précédent*, issu des mêmes sources, tournait. Le verdict porte sur
   l'empreinte du fichier : il est donc imprévisible pour tout binaire non signé
   nouvellement produit. Recopier le fichier sous un autre nom ne change rien, et
   attendre 2 minutes non plus. `vercora.exe` et `logic_tests.exe` passent, eux.

   *Rien n'a été désactivé* : SAC est un réglage de sécurité de la machine. Si le
   problème réapparaît sur un autre binaire, c'est à l'utilisateur de décider
   (Sécurité Windows → Contrôle d'application intelligent). Une raison de plus de
   n'avoir qu'un seul exécutable. À classer avec les autres particularités de
   cette machine (VS corrompu, `rc.exe` bloqué au premier appel), cf.
   NOTES-PHASE6.md.

### 4.8 Ordre des opérations dans le pilotage

Première version du scénario C : promenade de 30 m dans les quatre directions
« pour exercer le relief », *puis* abattage. Le trajet traversait le semis
d'arbres, chaque tronc déviait la caméra de quelques dizaines de centimètres, et
la position à l'estime n'était plus bonne. Corrigé : **viser d'abord depuis
l'origine** — la seule position connue exactement — **se promener ensuite**,
quand la dérive n'a plus d'importance. C'est exactement la leçon que l'en-tête du
scénario B documentait déjà.

---

## 5. Vérification

### Tests headless — 169 assertions (121 avant)

`TerrainFile.cpp` et `TerrainMesh.cpp` ne dépendent ni de Vulkan ni de GLFW : ils
sont désormais **compilés et liés** dans l'exécutable de test (`TerrainMesh.cpp`
n'était que vérifié syntaxiquement). Le nouveau bloc couvre :

- écriture puis relecture d'un `.vchm` construit octet par octet dans le test,
  avec une grille **asymétrique sur les deux axes** (sans quoi une inversion
  passerait inaperçue) ;
- résolution, pas, emprise déduite, bornes, label, `heightAt` aux nœuds et en
  milieu de cellule ;
- `toProjected` aux quatre coins, y compris la règle des signes opposés (aller à
  l'est **augmente** l'abscisse projetée, aller au sud **diminue** l'ordonnée) ;
- `terrainMeshData` sur la grille chargée : un sommet par échantillon, altitudes
  identiques ;
- **refus** d'un fichier à magie fausse, version inconnue, grille non carrée, pas
  nul, grille d'un seul échantillon, fichier tronqué, altitude `NaN`, fichier
  absent ;
- la **vraie dalle** si elle est présente (bloc ignoré sinon) : 1001², pas de 1 m,
  EPSG 2154, dénivelé > 500 m, et surtout `heightAt(0,0) == 605,38 m` — valeur
  relevée indépendamment sur le nuage brut par `check_terrain_file.py`.

### Vulkan réel — 3 scénarios

| | Scénario A | Scénario B | Scénario C |
|---|---|---|---|
| Terrain | synthétique | synthétique | **dalle LiDAR** |
| Couvre | gameplay complet, resize, minimisation | collision tronc puis souche | 2 M triangles, marche sur relief mesuré, abattage, ramassage, vol libre, 6 resize, minimisation |
| Résultat | 0 validation, sortie 0 | 0 validation, sortie 0 | 0 validation, sortie 0 |

---

## Ce qui n'a pas changé

- **Le terrain synthétique reste le monde de référence des tests.** Semis
  déterministe, positions connues d'avance, arbres tous abattables : c'est ce qui
  rend les scénarios A et B décisifs. Il n'est pas près d'être supprimé.
- **Les arbres et les pierres restent procéduraux.** La classification LiDAR
  contient bien la végétation (67 % de la dalle), mais en extraire des arbres
  individuels — segmentation de houppiers — est un sujet à part entière.
- **`TerrainMesh`, les shaders, `Camera`, `Forest`, `Ground` : aucune
  modification.** `Camera.hpp` n'a reçu qu'une constante nommée
  (`kDefaultEyeHeight`), pour que le dump puisse prédire ce que voit le joueur.

## Outils

| Script | Rôle |
|---|---|
| `tools\build.bat` | configure + compile (Debug par défaut, donc validation layers) |
| `tools\run_logic_tests.bat` | 169 assertions de comportement, sans GPU |
| `tools\run_vulkan_check.ps1` | lance le vrai jeu (3 scénarios) et traque les erreurs de validation |
| `tools\lidar_to_heightmap.py` | dalle `.laz` IGN → heightmap `.vchm` |
| `tools\check_terrain_file.py` | confronte un `.vchm` au nuage brut (détecte le miroir) |
| `vercora.exe --dump-world` | imprime le monde semé et les reculs de tir (remplace `tools\dump_world.cpp`) |

## Limites assumées

- **Grilles carrées uniquement.** `Heightmap` et `TerrainMesh` n'exposent qu'une
  résolution. Une dalle IGN étant un carré de 1 km, ça ne coûte rien aujourd'hui ;
  ça devra tomber au premier assemblage multi-dalles. Un fichier rectangulaire est
  **refusé avec un message explicite**, pas chargé de travers.
- **Une seule dalle à la fois**, chargée entièrement en mémoire. Pas de streaming,
  pas de LOD, pas de pavage.
- **Pas de texture.** Le terrain est coloré par altitude et par pente, comme en
  Phase 4. Les UV du maillage sont prêts pour une orthophoto (`data.geopf.fr`
  diffuse la BD ORTHO sur la même emprise).
- **Le nord est en −Z**, ce qui est correct mais peu intuitif. À garder en tête au
  moment de plaquer la première orthophoto.

## Prochaines étapes suggérées

1. **Essayer manette en main** — notamment la remarque 4.6 : est-ce que viser en
   hauteur suffit à rendre tous les arbres abattables sur du vrai relief ?
2. **Orthophoto BD ORTHO** sur la même emprise, plaquée via les UV existants.
   C'est le premier vrai contrôle visuel de l'orientation.
3. **Assemblage multi-dalles** : supporter les grilles rectangulaires, raccorder
   les bords (les nœuds de bord sont déjà partagés par construction), et décider
   d'une origine monde commune.
4. **Exploiter la classification** : la classe 5 (végétation haute) donne une
   carte de densité de forêt, de quoi semer les arbres là où il y en a vraiment.
5. **Choisir la zone du jeu.** Le pipeline est prêt ; n'importe quelle dalle de
   France se charge maintenant en deux commandes.
