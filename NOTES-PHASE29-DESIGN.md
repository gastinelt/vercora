# Phase 29 - Rochers, pierriers, buissons : donnees reelles (Agent 1/4)

Session sans supervision du 2026-09-27. Aucun fichier de `src\`, `shaders\`, `tests\`
ni de donnee existante modifie ; aucun build. Tout le derive est dans `data\phase29\`.
Scripts executes avec `py -3.14` (seul Python de la machine qui a laspy 2.7 + lazrs ;
le `python` par defaut, 3.10, ne l'a pas).

## 1. Donnees disponibles (VERIFIE)

| fichier | contenu |
|---|---|
| `data\lidar\*.copc.laz` (26) | nuages classes ; 25 dalles de la zone + `0907_6440` (hors zone) |
| `assets\terrain\vercora_zone_1m.vchm` | DTM 5001x5001, pas 1 m, origine E 899000 / N 6440000 (coin NO), 1029-2285 m |
| `assets\terrain\vercora_zone_canopy.vchm` | couvert 5 m 1001x1001 = (c3+c4+c5)/valides (tools/lidar_to_canopy.py) |
| `assets\terrain\vercora_zone_canopy_height.vchm` | z max classe 5 - z min sol, 5 m |
| `data\ortho\*.jpg` | ortho 0,5 m, dalles nommees par coin SO (validation independante) |
| **nouveau** `data\phase29\vercora_zone_lowveg.vchm` | **F3 = c3 / retours valides**, 5 m, meme emprise/convention que le couvert |
| **nouveau** `data\phase29\vercora_zone_lowveg_height.vchm` | z max classe 3 - z min sol (m), ecrete 0..10, 0 sans classe 3 |
| `data\phase29\class_counts_5m.npz` | comptes par classe (1,2,3,4,5,6,9,17,64...) par cellule 5 m, z3max, z3sum, gmin |
| `data\phase29\class_totals.csv` | comptes par dalle et par classe |

Scripts (lecture seule) : `data\phase29\rasterize_classes.py` (LAZ -> npz, ~2 min),
`probe_rocks.py [SMIN SMAX CMAX RMIN]` (sondage + PNG), `export_vchm.py`.
Coordonnees monde : `worldX = E - 901500`, `worldZ = 6437500 - N` (Heightmap.cpp l.240-255).

## 2. Classe 3 : EXISTE, DISTINCTE, EXPLOITABLE

Mesure (25 dalles, cf. class_totals.csv) : la classe 3 est presente dans 24 dalles sur 25,
de 0,5 M a 6,1 M points par dalle, avec des comptes indépendants des classes 4 et 5.
- Fraction F3 par cellule 5 m : p50 0,016 ; p75 0,065 ; p90 0,136 ; p99 0,31.
- corr(F3, F45) = **-0,19** : ce n'est PAS un doublon de la canopee (elle vit a cote).
- Hauteur z max c3 - sol : p10 1,1 / p50 2,5 / p90 4,5 m ; hauteur MOYENNE p50 1,5 m -> buissons.
- Densite classe 3 : moyenne 1,7 pt/m2 (p50 0,6 ; p90 4,9) sur ~33 pts/m2 totaux.
  **Resolution utile : 5 m** (~40 pts c3/cellule la ou il y a du buisson). 2 m possible
  mais bruite (≈3 pts/cellule en mediane) ; 1 m non.
- Deja rasterisee ? **Non separement** : elle est fondue dans le couvert (c3+c4+c5). Elle
  l'est maintenant dans `vercora_zone_lowveg.vchm` (nouveau fichier).
- Limite mesuree : **dalle `0902_6438` quasi sans vegetation classee** (c3 = 988 pts,
  c1 = 1,85 M) et c1 eleve dans 0901_6438, 0902_6439, 0903_6438/39/40 (~4-6 %). A l'ortho
  ces dalles sont le plateau karstique clair, donc plausible, mais une sous-classification
  n'est pas exclue (supposition). La classe 3 n'y creera simplement pas de buissons.
- F3 ne correle pas a la luminance ortho (0,03 ; F45 : -0,69) : a l'ortho un buisson ne se
  distingue pas d'une pelouse. Validation par la hauteur, pas par la couleur.

Le proxy « lisiere » (F45 < 0,3 a <= 10 m d'une cellule F45 > 0,5, 11,5 % de la zone) a un
F3 moyen de 0,109 contre 0,046 sur la zone : il est coherent, mais **inutile** puisque la
classe 3 est la ; a garder seulement comme repli sur terrain sans carte.

### Critere BUISSON retenu
`F3 >= 0,10  ET  F45 < 0,20` (F45 = couvert - F3, ou directement c4+c5).
= 10,5 % de la zone (2,62 km2). Variantes mesurees : F3 >= 0,05 -> 16,4 % ; >= 0,20 -> 3,5 %.
Densite recommandee : lineaire de F3 = 0,10 (rare) a 0,30 (~p99, plein). Taille : tirer
sous `lowveg_height` (p50 2,5 m), ecretee a 3 m pour rester des buissons. `F45 < 0,20`
evite de doubler les arbres deja semes par Vegetation (coverMin = 0,20).
Ce qui est suppose : l'espece (pin a crochets rampant / genevrier / aulne) n'est pas dans la donnee.

## 3. Rochers et pierriers

Seuils de reference du code : `kWalkComfortSlope = 0.60`, `kWalkMaxSlope = 0.84` =
`ClimberParams::wallSlope` (`src\core\SlopeLimits.hpp`) ; pente = `Climber::slopeAt`
(`src\world\Climber.cpp`), differences centrees demi-base `slopeSampleStep = 2 m`.
La sonde reproduit exactement cette pente sur le DTM 1 m.

### Critere PIERRIER retenu
`0,60 <= pente(base 4 m) < 0,84  ET  couvert < 0,10  ET  hors bati/eau`
- 0,84 : sous le seuil de paroi, par contrat (au-dessus c'est Climber).
- 0,60 : pente de marche confortable ; c'est aussi 31°, bas de l'angle de repos des eboulis
  (31-40°). Sous 0,60, les zones nues sont majoritairement pelouses/pistes (mesure ci-dessous).
- couvert < 0,10 : sol nu au LiDAR (les parois de reference ont couvert ≈ 0).
- bati/eau : cellule 5 m avec >= 1 point classe 6, dilatee 10 m, ou classe 9 dilatee 5 m.
  Effet mesure : **2 272 m2 seulement** retires (le bati est hors des pentes 0,6-0,84) ;
  utile quand meme comme garde-fou (classe 6 : 0 a 266 k pts/dalle). Classe 9 : 230 pts, 1 dalle.
- **Rugosite ecartee comme filtre** : RMS du residu DTM - moyenne 5x5. Elle retirait 37 %
  de la surface mais les rejetes sont AUSSI clairs a l'ortho (lum 166 vs 158) : un eboulis
  fin est lisse a 1 m. A employer pour CHOISIR le mesh : rugosite >= 0,10 m -> gros blocs
  (StoneMesh a l'echelle), < 0,10 -> pierraille fine (petits cailloux denses).

### Chiffres du sondage (probe_default.log)
Surface : **2,226 km2 (8,9 % de la zone)** ; dont rugueuse (>= 0,10 m) 1,395 km2.
Validation ortho (independante) : luminance moyenne **161** dans le masque contre 116 sur la
zone ; pixels clairs (> 140) **64 %** contre 32 %. Pelouse nue de reference : 113 ; paroi : 190.

Par tranche de pente, part NUE (couvert < 0,1) et luminance :
| pente | % zone | nu | lum nu |
|---|---|---|---|
| 0-0,30 | 20,4 | 6,9 | 152 |
| 0,30-0,45 | 25,2 | 6,8 | 150 |
| 0,45-0,60 | 22,4 | 7,6 | 154 |
| **0,60-0,70** | 11,3 | 5,0 | 164 |
| **0,70-0,84** | 8,2 | 3,9 | 157 |
| 0,84-1,00 | 4,2 | 2,1 | 156 |
| >= 1,00 | 8,4 | 5,9 | 161-181 |

Histogramme du masque rugueux (m2), altitude x pente [0,60 ;0,66 ;0,72 ;0,78 ;0,84[ :
| altitude | 0,60 | 0,66 | 0,72 | 0,78 | total | part de l'etage |
|---|---|---|---|---|---|---|
| 1000-1200 | 289 | 145 | 107 | 58 | 599 | 0,1 % |
| 1200-1400 | 907 | 657 | 528 | 362 | 2 454 | 0,1 % |
| 1400-1600 | 20 325 | 18 881 | 15 487 | 11 815 | 66 508 | 1,1 % |
| 1600-1800 | 100 764 | 117 042 | 109 416 | 90 904 | 418 126 | 6,5 % |
| 1800-2000 | 184 609 | 181 903 | 164 057 | 134 488 | 665 057 | 12,1 % |
| 2000-2300 | 68 000 | 65 836 | 58 462 | 50 084 | 242 382 | 14,4 % |

Coherence geographique (masque `data\phase29\mask_on_ortho_phase29.png`, 2 m/px) : rouge
sur les bandes calcaires claires et les pieds de barres ; **pas** sur le village (NO), les
pistes de ski en foret, le lac, ni les fonds de vallee boises. Le quart SE boise est vide.

12 echantillons (graine 29, masque rugueux) :
| E | N | worldX | worldZ | alt | pente | couvert | F3 | rug | RGB ortho | dalle |
|---|---|---|---|---|---|---|---|---|---|---|
| 903578 | 6438125 | 2078 | -625 | 1704 | 0,68 | 0,01 | 0,01 | 0,10 | 212,213,208 | 0903_6439 |
| 902390 | 6437021 | 890 | 479 | 1858 | 0,79 | 0 | 0 | 0,14 | 191,192,178 | 0902_6438 |
| 901790 | 6436949 | 290 | 551 | 1825 | 0,79 | 0 | 0 | 0,13 | 227,228,214 | 0901_6437 |
| 903452 | 6439601 | 1952 | -2101 | 1785 | 0,64 | 0 | 0 | 0,21 | 194,195,197 | 0903_6440 |
| 903491 | 6439343 | 1991 | -1843 | 1812 | 0,71 | 0 | 0 | 0,30 | 132,131,134 | 0903_6440 |
| 901751 | 6436832 | 251 | 668 | 1751 | 0,63 | 0 | 0 | 0,15 | 217,216,212 | 0901_6437 |
| 903608 | 6438770 | 2108 | -1270 | 1972 | 0,65 | 0 | 0 | 0,17 | 107,107,98 | 0903_6439 |
| 900086 | 6435551 | -1414 | 1949 | 1817 | 0,66 | 0 | 0 | 0,31 | 236,232,223 | 0900_6436 |
| 902588 | 6437228 | 1088 | 272 | 2050 | 0,71 | 0 | 0 | 0,58 | 85,83,82 | 0902_6438 |
| 900302 | 6435527 | -1198 | 1973 | 1681 | 0,83 | 0,01 | 0,01 | 0,18 | 186,181,165 | 0900_6436 |
| 903602 | 6438269 | 2102 | -769 | 1805 | 0,63 | 0 | 0 | 3,38 | 194,194,183 | 0903_6439 |
| 901802 | 6437189 | 302 | 311 | 2156 | 0,78 | 0 | 0 | 0,22 | 157,150,142 | 0901_6438 |
9/12 gris clair (calcaire), 3 sombres (ombre portee ou paroi : rug 3,38 = pied de barre).

## 4. Source exacte pour les agents 2 et 3

- Pente : `Climber::slopeAt`/`gradientAt` (`src\world\Climber.cpp` l.31-46), demi-base 2 m.
  **Ne pas** reprendre `Vegetation::slopeAt` (`Vegetation.cpp` l.122, differences avant sur 1 m,
  plus bruitee) : les seuils ci-dessus ont ete mesures avec la base de 4 m. Seuils :
  `kWalkComfortSlope`, `kWalkMaxSlope` de `src\core\SlopeLimits.hpp`.
- Couvert : `Vegetation::coverAt` (`Vegetation.cpp` l.131) sur `vercora_zone_canopy.vchm`,
  chargement dans `main.cpp` (`canopyPathFor`, l.498 ; chargement l.1329-1342).
- Classe 3 : `data\phase29\vercora_zone_lowveg.vchm` (+ `_height`), lisible par
  `loadTerrainFile()` sans code neuf. A COPIER dans `assets\terrain\` sous le nom
  `vercora_zone_canopy_lowveg.vchm` si l'on veut reutiliser `canopyPathFor(path, "_lowveg")`
  (decision a prendre par l'agent 2 ; je ne touche pas a assets).
- Bati : non expose au jeu. Exclusion pratiquement sans effet (2 272 m2) : on peut s'en passer
  en jeu ; sinon exporter `c6 > 0` depuis `class_counts_5m.npz`.
- Semis : meme schema que Vegetation (hachage par cellule, blocs 8x8, InstanceData 28 o) ;
  meshes `stoneMeshData()` (`src\world\StoneMesh.hpp`), `treeMeshData/broadleaf` (`TreeMesh.hpp`).

## 5. Limites

- Mesure : pente sur DTM 1 m (le jeu lit aussi la 2 m au loin : pentes lissees, masque plus petit).
- Couvert 5 m echantillonne au plus proche dans la sonde ; le jeu interpole en bilineaire
  (bords de masque decales de <= 2,5 m).
- Pierrier vs pelouse raide rase : non separables par le LiDAR seul (les deux sont c2). La
  luminance ortho confirme la moyenne, pas chaque pixel (25 % de pixels verts dans le masque,
  comme la zone). Supposition : une part des 0,60-0,70 est de la pelouse caillouteuse.
- Les rochers AU-DESSUS de 0,84 (pieds de parois, >= 1,0 : 5,9 % nu) sont hors critere par
  consigne ; des blocs isoles y seraient aussi realistes.
- Dalles a classe 1 elevee (§2) : vegetation possiblement non classee.
- Rugosite : RMS 5x5 d'un DTM median 1 m, sensible aux artefacts de jointure (p99 2,8 m, Phase 7).
