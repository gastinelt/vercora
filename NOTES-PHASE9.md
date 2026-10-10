# Vercora — Phase 9 : la vraie zone de jeu, 25 dalles assemblées en un terrain

Session du 2026-08-20/21. Objectif : passer de la dalle unique de validation
(Phase 7) à la **zone de jeu réelle** choisie avec l'utilisateur et son père —
25 dalles LiDAR HD, grille 5 × 5 km, Lambert 93 X 899–903 / Y 6436–6440.

C'est la session où se jouait ce que les deux bugs d'arrondi de la Phase 7
(arrondi bancaire, intervalle semi-ouvert inversé sur l'axe nord) auraient cassé
**en silence**. Ils avaient été corrigés mais jamais éprouvés en conditions
réelles. Ils le sont maintenant : **les 8 frontières intérieures sont vérifiées
contre les nuages bruts, alignement prouvé par contraste ×14,5.**

> **Numérotation.** La consigne de session parlait de « Phase 8 ». La Phase 8
> existe déjà dans `MEMORY.md` (visée sur relief + base du parapente, même
> journée) : cette session est donc numérotée **Phase 9** partout, y compris dans
> l'en-tête des nouveaux outils.

---

## Résultat en une page

| | |
|---|---|
| Dalles téléchargées | **25 / 25**, 4,99 Go, **73 s** (~85 Mo/s) |
| Conversion 1 m | 25 × 1001×1001, ~6 s/dalle, ~2,5 min au total |
| Terrain assemblé 1 m | **5001 × 5001**, 95,4 Mo, 5000 × 5000 m, dénivelé **1256 m** (1028,8 → 2285,2 m) |
| Continuité aux frontières | **conforme sur les 8 frontières** (détail plus bas) |
| Chargé dans le moteur | oui — 25 M sommets, 50 M triangles, **0 erreur de validation** |
| Jouable à 1 m ? | **non — 12 FPS.** Le 2 m (46 FPS) est le repli utilisable |
| Verdict | **le streaming par tuiles devient nécessaire maintenant**, cf. §6 |

---

## 1. Téléchargement — `tools/fetch_dalles.py` (nouveau)

Le manifeste annoncé à la racine sous le nom `vercora_dalles.json` n'y était
pas : il se trouvait dans `Downloads` sous le nom **`metadonnees.json`** (avec un
doublon `metadonnees(1).json`, strictement identique — même MD5). Contenu
conforme à l'annonce : 25 entrées, grille 5 × 5 complète, 925 974 185 points.
Copié à la racine sous le nom attendu.

```bash
python tools/fetch_dalles.py vercora_dalles.json -o data/lidar --dry-run
python tools/fetch_dalles.py vercora_dalles.json -o data/lidar
```

Le script fait d'abord **25 requêtes HEAD** pour connaître la taille réelle du
lot *avant* d'écrire quoi que ce soit (4,99 Go), la compare à l'espace libre
(281,6 Go) avec une marge, puis télécharge. Reprise par `Range` sur fichier
partiel, 4 tentatives avec attente croissante, vérification de la taille finale
**et** de la signature `LASF` — un HTML d'erreur enregistré sous un nom en `.laz`
passerait sinon inaperçu jusqu'à la conversion. Bilan final nommant les échecs.

**Résultat : 25/25, aucune reprise, aucun échec.** 73 s en deux lots (4 puis 21).

> **À noter pour l'utilisateur** : le téléchargement par navigateur lancé en
> parallèle tournait à ~1,5 Mo/s, contre ~85 Mo/s pour le script — soit plus
> d'une heure contre une minute pour le même lot.

## 2. Conversion — `tools/lidar_to_heightmap.py` (une option ajoutée)

Résolution **1 m, inchangée depuis la Phase 7**, et pour les mêmes raisons (la
densité de points sol la supporte). Un jeu 2 m est produit **en plus**, pas à la
place — voir §5.

Une seule modification, mais elle est structurante pour l'assemblage :

**`--bounds-from-name`.** L'emprise de la grille était déduite de
`header.mins/maxs`, c'est-à-dire de l'étendue des **points**, arrondie au mètre.
Sur les 25 dalles ça tombe juste — mais ce n'est pas garanti : une dalle dont les
échos s'arrêtent 30 cm avant le bord (bord de mission, plan d'eau, trou de vol)
aurait donné 1000 échantillons au lieu de 1001. Elle aurait alors été **refusée
comme non carrée**, ou pire, se serait raccordée **décalée d'un mètre**. L'option
impose l'emprise **nominale** lue dans le nom de la dalle, qui est celle que
l'IGN garantit. Utilisée pour les 25.

Résultats : **25 dalles, toutes en 1001 × 1001**, ~6 s chacune. Les trous varient
énormément d'une dalle à l'autre — de **0,26 %** (0901_6438) à **47,3 %**
(0901_6436, sous couvert dense) — et sont bouchés par dilatation comme en Phase 7.

> **Un piège rencontré, et qui vaut d'être noté.** La première passe de
> conversion en a silencieusement sauté 3 : le motif shell `LHD_FXX_09*` ne
> correspond pas aux dalles de la colonne **X = 899**, dont le nom commence par
> `LHD_FXX_0899` — donc par `08`, pas `09`. Erreur de ma part, pas de l'outil.
> Elle n'a pas pu aller plus loin parce que **`assemble_terrain.py` refuse une
> grille incomplète** (`--allow-gaps` est explicite) : le filet a joué son rôle.

## 3. Assemblage — `tools/assemble_terrain.py` (nouveau)

```bash
python tools/assemble_terrain.py assets/terrain/LHD_09*_1m.vchm \
    -o assets/terrain/vercora_zone_1m.vchm --label vercora_zone_5x5_1m
```

Chaque dalle est placée par son **géoréférencement** (`originEast`/`originNorth`
de son en-tête), jamais par son nom ni son ordre sur la ligne de commande. Une
origine qui ne tombe pas sur le réseau au pas courant est refusée.

### Le nœud partagé — le point non évident

Les échantillons sont sur les **nœuds** : une dalle de 1 km au pas de 1 m fait
1001 échantillons, de X=899000 à X=900000 **inclus**. La voisine commence à
X=900000, inclus elle aussi. **La colonne est donc dans les deux fichiers.**

5 dalles alignées font **5 × 1001 − 4 = 5001** échantillons, pas 5005. Une
concaténation naïve aurait produit 5005 colonnes : terrain 4 m trop large et tout
ce qui est à l'est d'une jointure décalé — la « duplication de ligne/colonne à la
jointure » à ne pas commettre. Le script le signale explicitement dans sa sortie.

Mais les deux valeurs partagées **ne sont pas identiques**, et c'est normal : le
nœud X=900000 agrège les points de [899999,5 ; 900000,5[. La dalle ouest n'en
voit que la moitié gauche, la dalle est que la moitié droite. **L'écart entre les
deux mesure directement la cohérence du raccord.** La valeur retenue est leur
moyenne.

### Écart entre les deux versions d'un nœud partagé (40 040 nœuds, 1 m)

| | |
|---|---|
| Médiane | **0,170 m** |
| Moyenne | 0,359 m |
| p99 | 2,78 m |
| > 10 m | 73 nœuds (0,18 %) |
| > 50 m | 15 nœuds (0,037 %) |
| Maximum | **79,23 m** |

Les 79 m ont été **cherchés et expliqués**, pas laissés de côté. Le pire nœud est
à E 903844 / N 6439000 : 1728,66 m selon la dalle nord, 1807,89 m selon la dalle
sud. Vérification dans les nuages bruts : les deux dalles **ont** des points là
(28 et 22 dans la cellule), et dans une fenêtre de 10 × 10 m autour, les
altitudes sol vont de **1682 à 1827 m** — une paroi quasi verticale.

La corrélation est nette sur l'ensemble :

| Écart au nœud partagé | Nœuds | Relief local médian (amplitude sur 5 m) |
|---|---|---|
| ≤ 0,5 m | 34 569 | **1,62 m** |
| 0,5 – 2 m | 4 742 | 2,31 m |
| 2 – 10 m | 656 | 2,41 m |
| 10 – 50 m | 58 | **34,13 m** |
| > 50 m | 15 | **59,71 m** |

**Ce n'est pas un défaut d'assemblage mais la limite d'une heightmap 2,5D sur une
falaise** : sur une paroi verticale, « l'altitude du sol en (x, y) » n'a pas de
valeur unique, et la médiane d'une cellule de 1 m² y est instable par nature. 79 %
des nœuds à plus de 10 m d'écart sont sur du relief à plus de 20 m d'amplitude
sur 5 m. À garder en tête le jour où la collision devra gérer ces parois.

## 4. Vérification des frontières — `tools/check_seams.py` (nouveau)

`tools/check_terrain_file.py` est resté **inchangé** : deux arguments positionnels
et des sondes nommées en dur, taillés pour une dalle isolée. Le pendant
multi-dalles est un outil frère, qui applique le **même principe** — relire le
`.vchm` produit et le confronter aux points sol **bruts**, sans rien partager avec
le producteur, pour qu'une erreur d'indexation ne puisse pas se compenser.

```bash
python tools/check_seams.py assets/terrain/vercora_zone_1m.vchm data/lidar/*.laz
```

1 676 sondes (1 592 réparties tous les 25 m le long des 8 frontières, 100 témoins
loin de toute jointure), dont **1 360 exploitables** — les 316 autres sont
rejetées faute de points sol, sous couvert dense.

### Résultat (terrain 1 m, 25 dalles)

```
lecture       jointures: median       max    | temoins: median       max
direct                   0.020m   80.840m               0.000m    0.080m   <- lecture reelle
est +1                   0.300m   42.605m               0.290m    3.430m
ouest -1                 0.300m   85.420m               0.305m    2.105m
sud +1                   0.290m   77.540m               0.260m    1.885m
nord -1                  0.295m   86.930m               0.280m    1.595m
```

**C'est le contraste qui fait la preuve**, comme pour le test nord/sud de la
Phase 7 : la lecture directe vaut **0,020 m** contre **0,290 m** pour le meilleur
décalage d'un nœud, soit **×14,5**. Un assemblage décalé d'une colonne resterait
plausible sur du relief doux ; il ne peut pas produire ce contraste.

Détail des 8 frontières — aucune ne se distingue :

| Frontière | Sondes | Médiane | Max |
|---|---|---|---|
| H N6436000 | 109 | 0,030 m | 12,49 m |
| H N6437000 | 167 | 0,025 m | 11,66 m |
| H N6438000 | 171 | 0,020 m | 0,53 m |
| H N6439000 | 175 | 0,017 m | 12,72 m |
| V E900000 | 175 | 0,022 m | 1,87 m |
| V E901000 | 164 | 0,015 m | 0,27 m |
| V E902000 | 152 | 0,017 m | **80,84 m** |
| V E903000 | 160 | 0,020 m | 0,39 m |

Les maxima isolés sont les falaises du §3 (mêmes nœuds, même cause). **Les
médianes, elles, sont toutes ≤ 0,030 m**, et les témoins en plein milieu de dalle
sont à 0,000 m : le raccord n'est pas moins fidèle que le reste du terrain.

### Deuxième contrôle, indépendant, dans `assemble_terrain.py`

Mesure faite sur le **produit fini**, sans rien savoir des dalles d'origine : le
long d'une jointure, la différence entre deux colonnes voisines doit ressembler à
celle observée partout ailleurs. Un décalage la rendrait anormalement **grande**,
une colonne dupliquée anormalement **petite** (deux colonnes identiques = pente
nulle). Les deux défauts sont donc attrapés.

```
reference : pente mediane est-ouest 0.300 m/m, nord-sud 0.265 m/m
jointure verticale   x=1000 : 0.393 / 0.397    x=2000 : 0.253 / 0.250
                     x=3000 : 0.270 / 0.267    x=4000 : 0.318 / 0.315
jointure horizontale z=1000 : 0.235 / 0.235    z=2000 : 0.250 / 0.248
                     z=3000 : 0.388 / 0.390    z=4000 : 0.403 / 0.398
OK - aucune jointure ne se distingue du relief environnant.
```

### Système de coordonnées

Inchangé et conforme à la Phase 7 : le `.vchm` garde son géoréférencement en
`double` dans l'en-tête (coin nord-ouest **E 899000 / N 6440000**, EPSG:2154) et
des altitudes **absolues IGN69** ; c'est le moteur qui **recentre le terrain sur
l'origine du monde** au chargement. Les sommets ne portent donc jamais de
coordonnées Lambert 93 — à 6,4 millions, l'écart entre deux `float32` vaut déjà
0,5 m. Sur 5 km, le monde va de −2500 à +2500 en X et en Z, et les anciennes
frontières de dalles tombent à ±500 et ±1500 m.

## 5. Chargement dans le moteur et performances

Mesures reproductibles via **`tools/measure_terrain.ps1`** (nouveau) : FPS lu dans
le titre de la fenêtre, mémoire par `PeakWorkingSet64`. **Rien n'a été ajouté au
moteur pour mesurer** — c'est exactement le binaire qui tourne en vrai.

> **Le GPU a changé depuis la Phase 7.** Cette machine n'a plus de GTX 960 : elle
> tourne sur l'**Intel UHD 770** intégrée (confirmé par l'utilisateur, ce n'est
> pas un défaut de détection). Les 848 FPS de la Phase 7 ne sont donc **pas**
> comparables aux chiffres ci-dessous. La dalle de Phase 7 a été **re-mesurée**
> sur ce GPU pour servir de repère honnête. i5-14600K, 64 Go de RAM, build
> **Debug avec couches de validation**, comme en Phase 7.

| Scène | Sommets | Triangles | FPS | Pic RAM | Chargement |
|---|---:|---:|---:|---:|---:|
| Procédural (Phase 4) | 16 384 | 32 258 | **1381** | 108 Mo | 0,2 s |
| 1 dalle 1 m (Phase 7) | 1 002 001 | 2 000 000 | **247** | 302 Mo | 0,6 s |
| 2 × 2 dalles 1 m (4 km²) | 4 004 001 | 8 000 000 | **67** | 805 Mo | 1,8 s |
| **5 × 5 dalles 2 m (25 km²)** | 6 255 001 | 12 500 000 | **46** | 1 083 Mo | 2,8 s |
| **5 × 5 dalles 1 m (25 km²)** | 25 010 001 | 50 000 000 | **12** | **4 014 Mo** | 10,2 s |

**Zéro message `[Validation]`, zéro `[ERROR]`/`[WARN]`, code de sortie 0 sur les
cinq scènes.** Le terrain de 25 km² à 1 m *charge et fonctionne* — il n'est
simplement pas jouable.

### Déplacement sur toute la zone — `tools/traverse_terrain.ps1` (nouveau)

La caméra marche 5,6 km en franchissant les anciennes frontières à ±500 m et
±1500 m sur les deux axes, avec relevé du FPS **en cours de route** : un accroc à
une jointure se verrait comme une chute localisée, pas dans une moyenne.

| Terrain | FPS moyen | min | max | Validation | Sortie |
|---|---|---|---|---|---|
| 5 × 5 à 2 m | 46–47 | 43 | 47 | 0 | propre |
| 5 × 5 à 1 m | 12 | 9 | 12 | 0 | propre |

**Aucune chute localisée au franchissement d'une frontière**, aucune chute à
travers le sol, aucun blocage, fermeture propre. Le FPS est le même au milieu
d'une ancienne dalle et à cheval sur une jointure.

*Limite assumée, la même qu'en Phase 7/8 : la souris ne peut pas être pilotée
(GLFW lit le curseur système), donc la caméra ne tourne pas de tout le test et la
position n'est connue qu'à l'estime. Le test prouve qu'on traverse sans incident ;
il ne mesure pas où l'on arrive.*

### Non-régression

- **Terrain procédural** (`vercora.exe terrain`) : 1381 FPS, intact.
- **Dalle unique de la Phase 7** (`vercora.exe assets\terrain\LHD_0907_6440_1m.vchm`) :
  247 FPS, intacte. Elle **reste la scène par défaut** — voir §6.
- **185 assertions headless** (`tools\run_logic_tests.bat`) : toutes passent.
- **`tools\run_vulkan_check.ps1`**, 4 scénarios : voir le tableau de `MEMORY.md`.

## 6. Le verdict qu'il faut entendre : le streaming devient nécessaire

Le facteur limitant n'est **pas** la mémoire (4 Go sur une machine qui en a 64,
et le GPU intégré puise dans la RAM système) ni le disque (5,2 Go sur 281 Go
libres) ni la conversion (2,5 min pour 25 dalles). C'est le **débit géométrique**.

Le moteur envoie **un seul maillage, en entier, à chaque image**, sans LOD ni
culling par morceaux. Le coût est donc proportionnel à la surface, et il l'est
très proprement : 247 → 67 → 12 FPS pour 1 → 4 → 25 km² à 1 m. Or **le joueur ne
voit à aucun moment plus d'un très petit pourcentage** de ces 50 M de triangles.

**Réponse à la question posée : oui, un système de streaming par tuiles devient
nécessaire maintenant, et non plus tard.** À l'échelle de la Phase 7 tout passait
en force ; à 25 km² ce n'est plus vrai, et la donnée finale est là pour rester.
Sans découpage, chaque agrandissement futur de la zone divisera encore le FPS.

Ce qui est *déjà* acquis pour ça : les 25 heightmaps individuelles sont
conservées (`assets/terrain/LHD_<X>_<Y>_1m.vchm`), le géoréférencement de chacune
est exact, et l'assemblage est prouvé cohérent. **Le découpage en tuiles n'aura
donc rien à redécouper** — il consommera directement les dalles existantes.

Ce qui manque côté moteur, par ordre d'importance : charger/décharger des
morceaux de terrain selon la position de la caméra ; un LOD par distance (une
tuile lointaine n'a pas besoin du pas de 1 m) ; un frustum culling par tuile. La
contrainte « grilles carrées uniquement » de `TerrainFile` devra aussi tomber,
puisqu'elle n'a survécu ici que parce que 5 × 5 est carré.

### En attendant : le 2 m est le repli utilisable

`assets/terrain/vercora_zone_2m.vchm` (2501 × 2501, 25 Mo) couvre la **même zone
de 25 km²** à **46 FPS** et 1 Go — soit 4× moins de triangles pour un pas qui
reste très fin à l'échelle d'une montagne. La Phase 7 avait déjà généré le 2 m en
prévoyant exactement ce cas (« pour une future carte multi-dalles »).

**Ce n'est pas un substitut au 1 m** : le fichier 1 m est produit, vérifié et
livré, et c'est lui qui alimentera le streaming. C'est un repli pour continuer à
travailler sur la vraie zone d'ici là.

## 7. Ce qui n'a pas été fait / vérifié — à dire franchement

- **La scène par défaut n'a pas été changée.** `vercora.exe` sans argument charge
  toujours la dalle de Phase 7. Faire de la zone 25 km² le défaut alors qu'elle
  tourne à 12 FPS aurait donné l'impression d'une régression du moteur ; le choix
  du défaut revient à l'utilisateur, une fois le streaming décidé. Les deux
  terrains assemblés se chargent par chemin explicite.
- **Aucun test headless n'a été ajouté** pour l'assemblage : la vérification est
  entièrement en Python (`assemble_terrain.py` + `check_seams.py`), pas dans les
  185 assertions C++. Un test C++ qui chargerait `vercora_zone_1m.vchm` supposerait
  un fichier de 95 Mo non versionné.
- **Aucun scénario n'a été ajouté à `run_vulkan_check.ps1`** pour la zone
  assemblée : le pilotage par PostMessage est calibré en distance/temps à
  vitesse 14 m/s, et à 12 FPS le jeu consomme les touches trop grossièrement pour
  que le tir sur cible reste fiable. `traverse_terrain.ps1` couvre le
  déplacement ; l'abattage/ramassage sur la nouvelle zone reste à vérifier.
- **Rien n'a été vérifié à la manette / à l'œil.** L'orientation du relief est
  prouvée par la donnée (contraste ×14,5), pas par un contrôle visuel : il n'y a
  toujours pas d'orthophoto plaquée. C'est toujours la recommandation n°2 de la
  Phase 7.
- **Les falaises du §3** ne sont pas traitées, seulement caractérisées.
- **`--bounds-from-name` n'a pas été éprouvée sur une dalle réellement tronquée**,
  puisque les 25 tombaient juste. C'est un filet, pas une correction constatée.

## 8. Où sont les choses

| Chemin | Quoi |
|---|---|
| `vercora_dalles.json` | manifeste des 25 dalles (copié depuis `Downloads/metadonnees.json`) |
| `data/lidar/*.copc.laz` | 25 dalles brutes, **5,2 Go** (avec celle de Phase 7) — non versionnées |
| `assets/terrain/LHD_<X>_<Y>_1m.vchm` | 25 heightmaps individuelles, 3,82 Mo chacune |
| `assets/terrain/LHD_<X>_<Y>_2m.vchm` | idem au pas de 2 m |
| `assets/terrain/vercora_zone_1m.vchm` | **terrain assemblé 5001², 95,4 Mo** |
| `assets/terrain/vercora_zone_2m.vchm` | terrain assemblé 2501², 25 Mo (repli jouable) |
| `assets/terrain/test_2x2_1m.vchm` | bloc de validation 2 × 2, gardé comme cas de test |

Nouveaux outils : `tools/fetch_dalles.py`, `tools/assemble_terrain.py`,
`tools/check_seams.py`, `tools/measure_terrain.ps1`, `tools/traverse_terrain.ps1`.
Modifié : `tools/lidar_to_heightmap.py` (option `--bounds-from-name`).
**Aucun fichier de `src/` n'a été modifié dans cette session.**

## Prochaines étapes suggérées

1. **Streaming par tuiles + LOD** (§6) — le vrai sujet de la prochaine session.
2. **Orthophoto BD ORTHO** sur l'emprise E 899000–904000 / N 6435000–6440000 :
   premier contrôle *visuel* de l'orientation, toujours en attente depuis la
   Phase 7.
3. Décider de la **scène par défaut** une fois le streaming en place.
4. Exploiter la classification (classe 5) pour semer les arbres là où il y en a
   vraiment — la variation de couvert entre dalles (0,26 % à 47,3 % de trous) est
   une carte de forêt gratuite.
