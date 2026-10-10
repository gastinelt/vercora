# NOTES-PHASE18.md — Un vrai tapis d'herbe, et où passe vraiment le temps d'une tuile

Session du 2026-08-25, à la suite de la Phase 17. Deux chantiers indépendants :

1. **l'herbe** — la Phase 11 laissait un semis de touffes opaques ; il fallait un
   tapis et des brins découpés ;
2. **la construction du maillage CPU d'une tuile** — la Phase 12 l'avait
   désignée comme le goulot suivant, sans jamais la mesurer poste par poste.

Toutes les mesures : Intel UHD 770, build **Debug avec couches de validation**,
1280x720, `tools\measure_terrain.ps1`, caméra **verrouillée** (`--lock-input`),
position et orientation fixées par `--start` / `--look`. **Zéro message
`[Validation]` sur la totalité des mesures de ce document.**

---

## 0. Les deux vues de référence, et pourquoi il en fallait une seconde

La Phase 11 mesurait l'herbe sur la vue **E 901500 N 6436600, yaw 100, pitch -7**
(271 FPS / 332 Mo). Cette vue a été reprise telle quelle pour la non-régression —
et le premier relevé de la session l'a reproduite **exactement** : 271 FPS,
332,4 Mo.

Mais cette vue ne contient que **124 touffes**. Elle ne peut donc rien dire d'une
amélioration de l'herbe : n'importe quel changement du semis s'y perd dans le
bruit. Cinq positions ont été sondées pour trouver un **alpage réellement
herbeux** ; la retenue est **E 900500 N 6439000, yaw 100, pitch -7** (8 867
touffes avant la phase). Les deux vues sont mesurées à chaque étape : l'une dit
si quelque chose a régressé, l'autre dit si quelque chose a changé.

> C'est la même leçon que « savoir où il y a de la pente ne dit pas par où
> l'aborder » (Phase vélo) : une vue de référence n'est valable que pour ce
> qu'elle contient réellement.

---

## 1. TÂCHE 1 — l'herbe

### 1.1 Ce que la Phase 11 laissait, précisément

Trois quads croisés, **opaques**, effilés en pointe, semés à ~2 touffes/m². Vue
de près, une touffe est un éventail de rubans verts ; vue de loin, le sol reste
majoritairement nu entre les touffes. La capture `build\p18_avant_rasant.png` le
montre sans discussion : des cônes de papier posés sur un sol gris.

### 1.2 Ce qui a été changé — et ce qui ne l'a PAS été

| Fichier | Changement |
|---|---|
| `shaders/foliage.frag` | **masque de brins calculé**, `discard` — la découpe alpha |
| `shaders/foliage.vert` | transmet UV, aléa d'instance et **force de découpe** (qui décroît avec la distance) |
| `src/world/GrassMesh.cpp` | le quad devient une **planche** large et peu effilée : 0,26 → 0,42 au pied, 0,06 → 0,30 à la pointe |
| `src/world/Vegetation.hpp` | `grassSlots` 24 → **56** ; nouveau `grassCutout` |
| `src/world/Vegetation.cpp` | hauteur du sol échantillonnée **par cellule** et non plus par touffe |
| `src/main.cpp` | câblage de `params.w`, option **`--no-grass-cutout`** |

**Rien d'autre n'a bougé.** En particulier :

- **aucun pipeline, aucun descriptor, aucun binding** n'a été ajouté ou modifié.
  La découpe passe par `params.w`, qui était **réservé** dans le bloc de push
  constants depuis la Phase 11 (voir `renderer/DrawItem.hpp`). Un arbre y pousse
  0 et traverse le même pipeline, inchangé ;
- **aucun seuil de répartition** n'a été touché : ni `grassClusterThreshold`, ni
  `grassMaxSlope`, ni les altitudes, ni `grassUnderCanopy`. La carte de
  répartition établie en Phase 11 — celle qui garde les lapiaz nus et suit la
  donnée LiDAR — est **littéralement** la même. Seul son remplissage change.

### 1.3 Pourquoi un masque calculé et non une texture

Une planche de brins en RGBA aurait demandé un second échantillonneur, donc un
binding de descriptor sur ce pipeline seulement, un asset à produire et à
charger — et surtout son filtrage mipmap aurait **fait fondre** les brins fins en
une bouillie grise à quelques mètres. Le masque analytique n'a aucun de ces
coûts, tient en quinze lignes, et sa finesse ne dépend d'aucune résolution.

Le masque découpe **4 brins par quad**, soit **12 brins par touffe** pour les
mêmes 6 triangles. Chaque brin a sa propre hauteur de pointe, sa propre largeur
décroissante et son propre écartement, tirés d'un haché de son indice et de
l'aléa de l'instance : deux touffes voisines n'ont donc pas le même découpage.

### 1.4 Pourquoi `discard` et non du mélange alpha

Le mélange alpha imposerait de **trier les touffes en profondeur à chaque
image** — vingt mille objets, depuis une caméra qui bouge — et une seconde passe
de rendu. Le test alpha laisse le tampon de profondeur faire son travail :
l'ordre de dessin reste sans importance, exactement comme avant. C'est ce qui
rend cette amélioration presque gratuite (§1.6).

Sa contrepartie est un bord **franc**, sans demi-teinte, et il n'y a pas de MSAA
dans ce moteur (`rasterizationSamples = 1`) pour l'absorber. D'où le troisième
mécanisme : **la découpe s'efface avec la distance**. Les brins s'élargissent
jusqu'à refermer le quad entre 7 m et 17 m. De près on voit des brins, de loin
une silhouette pleine, et la transition est continue. Sans cela, l'herbe
lointaine scintillerait à chaque déplacement de la caméra.

### 1.5 La densité : agir sur le remplissage, pas sur les frontières

La Phase 11 concluait « il faudrait 8 à 10 touffes/m² là où il y en a 2 ».
Deux chemins menaient à plus de touffes :

- **baisser les seuils de sélection** (bruit de groupement, pente, canopée) —
  cela aurait mis de l'herbe **là où il n'y en a pas** : sur le rocher, sous la
  futaie dense, sur les versants que la donnée LiDAR déclare nus ;
- **augmenter le nombre d'emplacements par cellule** — cela densifie **là où
  l'herbe pousse déjà**, sans déplacer une seule frontière.

Le reproche de la Phase 11 n'était pas que l'herbe manquait d'emprise, mais
qu'elle formait un semis clairsemé **là où elle pousse**. C'est donc
`grassSlots` qui passe de 24 à **56**. Le compte retenu vaut `densité × slots` :
sur l'alpage mesuré, cela fait passer la vue de **8 867 à 20 098 touffes**
(×2,27), soit environ 5 touffes/m² au lieu de 2.

> Repère honnête vis-à-vis de la consigne : la Phase 11 citait 2 158 touffes et
> suggérait « 4 à 5 fois plus ». Ce chiffre de 2 158 date d'avant les réglages
> des phases suivantes ; **la même vue de prairie en rendait déjà 8 867 au début
> de cette session**. Le facteur 2,27 obtenu ici s'ajoute donc à un gain déjà
> acquis : par rapport au repère écrit de la Phase 11, on est à **×9**. Multiplier
> encore par deux était possible et a été écarté — au-delà, le coût par image
> (§1.6) n'est plus payé par une différence visible.

### 1.6 Coût mesuré — et une surprise

Vue d'alpage, **E 900500 N 6439000, yaw 100, pitch -7**, même binaire, seules
les options changent :

| État | Touffes | FPS médian | Pic RAM |
|---|---:|---:|---:|
| Avant la Phase 18 | 8 867 | **137** | 338,5 Mo |
| Avant la Phase 18, `--no-grass` (témoin) | 0 | 304 | 337,6 Mo |
| Densité ×2,27 + découpe alpha | 20 098 | **95** | 340,2 Mo |
| Idem, `--no-grass-cutout` (témoin) | 20 098 | **95** | 340,1 Mo |
| + hauteur du sol par cellule (§1.7) | 20 098 | **118** | 340,1 Mo |

**La découpe alpha coûte zéro image par seconde.** 95 FPS avec, 95 FPS sans, sur
le même binaire et la même vue — c'est précisément ce que le témoin
`--no-grass-cutout` a été ajouté pour pouvoir affirmer. Le fragment shader
n'était pas le facteur limitant, et un `discard` précoce est moins cher que
l'éclairage Blinn-Phong qu'il évite.

**Tout le coût vient du nombre d'instances**, et il est majoritairement **CPU** :
c'est ce que prouve le §1.7, où une optimisation qui ne change pas un pixel rend
23 FPS.

### 1.7 L'optimisation qui a rendu la densité payable

`gatherGrass` appelait `terrain_.heightAt()` **une fois par touffe**. À 56
emplacements par cellule et un millier de cellules par image, ce poste grandit
exactement au rythme de la densité : c'est lui qui plafonnait le tapis.

Il échantillonne désormais les **quatre coins de la cellule**, puis interpole à
l'intérieur — et ce n'est **pas une approximation** quand la cellule d'herbe
coïncide avec une cellule du terrain (1 m et 1 m sur cette zone) : le découpage
en deux triangles le long de la diagonale h00-h11 de `Heightmap::heightAt` est
reproduit à l'identique, donc la touffe est posée au pixel près sur la surface
affichée. Un test headless le vérifie sur un versant à 35 % : écart maximal
**nul** à 10⁻³ près.

**95 → 118 FPS (+24 %), image strictement identique.**

C'est la suite directe d'une leçon de la Phase 11, qui avait déjà ramené ce
poste de deux appels à un par touffe. La même leçon, poussée un cran plus haut :
au niveau de la cellule.

### 1.8 Résultat visuel

| Capture | Vue |
|---|---|
| `build\p18_avant_rasant.png` / `build\p18_apres_rasant.png` | alpage, pitch -25 — avant / après |
| `build\p18_avant_prairie.png` / `build\p18_apres_prairie.png` | alpage, pitch -7 — avant / après |
| `build\p18_apres_proche.png` | pitch -45, brins visibles individuellement |

Ce qui se voit : le sol nu entre les touffes a disparu là où la donnée LiDAR
déclare de l'herbe, et une touffe se lit désormais comme une gerbe de brins et
non comme un éventail de rubans. Ce qui **ne** se voit pas et n'est pas censé se
voir : les zones que la carte de couvert déclare nues sont restées nues — la
partie gauche de la capture, en particulier, est aussi clairsemée qu'avant, et
c'est correct.

**Ce qui ne convainc toujours pas**, honnêtement : à moins d'un mètre, le sol
reste visible entre les touffes (5 touffes/m² de 30 cm ne ferment pas une
pelouse), et le sol lui-même reste l'orthophoto grise — un tapis totalement
crédible demanderait aussi de teinter le terrain sous l'herbe. Ce n'est pas fait.

---

## 2. TÂCHE 2 — la construction du maillage CPU d'une tuile

### 2.1 Mesurer d'abord : l'instrumentation

La Phase 12 concluait que le maillage CPU était devenu le goulot, mais sans
jamais le décomposer. `TerrainStreamer::buildProfile()` chronomètre donc
maintenant **trois postes séparés**, en microsecondes cumulées, journalisés à la
fermeture :

- **géométrie** — `terrainNodeMeshData` : échantillonnage de la heightmap,
  normales, cibles de morphing ;
- **indices** — la topologie de la tuile ;
- **téléversement** — `Renderer::addMesh`, le chemin GPU traité en Phase 12.

Scénario : `--flythrough=900 --no-grass` sur la zone de 25 km², 45 s, caméra
scriptée — le survol à haute vitesse de la Phase 12, celui qui sature le
streaming. `--no-grass` pour que l'herbe de la Tâche 1 ne pollue pas la mesure.

### 2.2 Ce que le profilage a dit — et ce à quoi personne ne s'attendait

Premier relevé, avant toute optimisation, ~5 590 tuiles construites :

| Poste | µs/tuile | Part |
|---|---:|---:|
| Géométrie | 1 858 | 66 % |
| **Indices** | **636** | **23 %** |
| Téléversement GPU | 307 | 11 % |

**23 % du coût d'une tuile partait dans la reconstruction de sa topologie** —
24 576 indices, **identiques pour toutes les tuiles de tous les niveaux**.
L'en-tête de `TerrainNodeMesh.hpp` l'écrivait noir sur blanc depuis la Phase 10
(« calculés une fois et recopiés ») ; le code, lui, les reconstruisait à chaque
tuile. Personne n'aurait parié sur la boucle d'indices, et c'était le deuxième
poste, **deux fois plus cher que le téléversement GPU** qui avait mobilisé toute
la Phase 12.

C'est exactement la raison pour laquelle la consigne disait « mesure d'abord ».

### 2.3 Les deux optimisations retenues

Toutes deux sont **exactes** : le maillage produit est identique, seul le
travail dépensé change.

1. **La topologie est construite une fois** (`TerrainStreamer::indexTemplate_`)
   puis recopiée dans le `MeshData`. `terrainNodeMeshData` reçoit un paramètre
   `withIndices` pour laisser la topologie à l'appelant ; les autres appelants,
   dont les tests headless, gardent le comportement complet par défaut.

2. **Les cibles de morphing sont regroupées.** Chaque sommet a besoin du point
   que le nœud PARENT produirait au même endroit ; ce point ne dépend que des
   indices locaux **arrondis à un multiple de deux**, donc les quatre sommets
   d'un bloc 2×2 partagent exactement la même cible. Le code la recalculait pour
   chacun des quatre — trois échantillonnages sur quatre jetés, chacun coûtant
   cinq lectures de heightmap et une normalisation. Une grille grossière 33×33
   est désormais calculée une fois par tuile.
   Un test headless vérifie l'hypothèse plutôt que de la croire : les 4 sommets
   de chaque bloc 2×2 visent bien la même cible.

Un **témoin**, `--no-tile-cache`, remet les deux dans l'état d'avant la phase.
Toutes les comparaisons ci-dessous sont donc faites **sur le même binaire et la
même trajectoire** — la seule comparaison qui ait du sens.

### 2.4 Avant / après, survol à 900 m/s, deux passages chacun

| | `--no-tile-cache` (témoin) | défaut | écart |
|---|---:|---:|---:|
| Géométrie | 1 723 / 1 730 µs | 1 248 / 1 245 µs | **−28 %** |
| Indices | 629 / 631 µs | 7,5 / 7,4 µs | **−99 %** |
| Téléversement | 254 / 249 µs | 262 / 254 µs | inchangé |
| **Total par tuile** | **~2 609 µs** | **~1 512 µs** | **−42 %** |
| Frames en 45 s | 9 471 / 9 426 | 11 435 / 11 320 | **+20 %** |
| Frames à trou | **0** | **0** | — |

Les deux passages de chaque colonne sont à moins de 0,5 % l'un de l'autre : ce
chronométrage cumulé est **beaucoup plus stable** que le FPS lu dans le titre de
la fenêtre (≈15 % d'écart entre deux runs identiques, cf. MEMORY.md). C'est un
sous-produit utile de la tâche : le projet dispose maintenant d'une mesure de
performance reproductible.

Le téléversement GPU, lui, ne bouge pas — et c'est le résultat attendu : la
Phase 12 l'avait déjà traité, et il ne représente plus que **17 %** du coût
d'une tuile après cette phase.

### 2.5 Ce qui n'a PAS été fait, et pourquoi

- **Parallélisation multi-thread de la génération de tuiles.** C'est le gain
  suivant en importance — la géométrie reste 83 % du coût — mais
  `Renderer::addMesh` n'est pas conçu pour être appelé depuis plusieurs threads,
  et la Phase 12 a déjà montré ce que coûte une erreur sur ce chemin. Le travail
  se découpe pourtant proprement : `terrainNodeMeshData` est **pur** (il ne lit
  que la heightmap, en lecture seule) et pourrait être lancé sur un pool, le
  `addMesh` restant sur le thread de rendu. Non fait ici : la consigne demandait
  explicitement de s'arrêter à un profilage clair plutôt que de forcer une
  optimisation risquée, et ce chantier ne tenait pas dans la session.
- **Partager UN buffer d'indices GPU** entre toutes les tuiles. Les 7,5 µs
  restants sont la recopie ; la supprimer économiserait surtout **96 Ko par
  tuile résidente**, soit ~48 Mo sur 512 tuiles. Cela demande de dissocier
  sommets et indices dans `Renderer::addMesh` : c'est un chantier de renderer,
  pas de streaming.
- **Réduire le coût de `sampleGrid`** (5 lectures + une `normalize` par
  échantillon). Piste réelle, mais qui touche la forme des normales : à ne
  tenter qu'avec une comparaison d'images.

---

## 3. Résumé des chiffres

### Vue de référence de la Phase 11 — E 901500 N 6436600, yaw 100, pitch -7

| État | FPS médian | Pic RAM |
|---|---:|---:|
| Repère Phase 11 (reproduit au début de la session) | 271 | 332,4 Mo |
| **Après la Phase 18** | **293** | **332,7 Mo** |

Cette vue ne contient que 124 touffes : elle ne mesure pas l'herbe, elle mesure
la **non-régression** — et le gain de 22 FPS vient de la Tâche 2.

### Vue d'alpage — E 900500 N 6439000, yaw 100, pitch -7

| État | Touffes | FPS médian | Pic RAM |
|---|---:|---:|---:|
| Avant la Phase 18 | 8 867 | 137 | 338,5 Mo |
| **Après la Phase 18** | **20 098** | **122** | **340,2 Mo** |

**2,27× plus de touffes, des brins découpés, pour 11 % de FPS et 1,7 Mo.**

### Construction d'une tuile (survol 900 m/s)

**2 609 → 1 512 µs par tuile (−42 %)**, +20 % de frames sur la même trajectoire,
0 frame à trou avant comme après.

---

## 4. Vérification

- **Tests headless : 896 assertions** (885 avant la phase, **+11**), toutes
  passent. Nouvelles sections : « Phase 18 : herbe plus dense et brins
  découpés » (quad devenu planche, densité effectivement plus que doublée,
  `grassCutout` sans effet sur le semis, pied des touffes posé au sol malgré
  l'interpolation par cellule) et, dans la section tuile, l'invariant du bloc
  2×2 et l'équivalence `withIndices` vrai/faux.
- **Zéro message `[Validation]`** sur la totalité des mesures et des scénarios.
- **Harnais `run_vulkan_check.ps1`** (11 processus, scénarios A à I), joué deux
  fois en entier — une fois après la Tâche 1, une fois sur le binaire final :
  **0 `[Validation]`, 0 `[ERROR]`, fermeture propre partout, code de sortie 0
  pour chacun des onze processus**, gameplay effectivement exercé (2 arbres
  abattus, 9 objets ramassés, percussion, vol de parapente complet, escalade
  jusqu'au sommet, éditeur : 3 objets posés/sauvés/rechargés, boisson au point
  d'eau, faune qui fuit).

### Le point rouge du harnais, honnêtement

Le harnais sort tout de même en `[ECHEC]`, sur **un seul critère** :
**scénario E (vélo), « pied à terre » final**. C'est un échec **déjà documenté
avant cette phase** (MEMORY.md, bilan de la Phase 17 : « rejoué deux fois avec
`--no-wildlife`, il échoue pareil », laissé non corrigé pour cause de
périmètre). La Phase 18 ne l'a ni causé ni aggravé.

Un second critère a été rouge **au premier passage seulement** : scénario I
(faune), « animaux à portée de vue », 4 → 2 pour un seuil à 1. Rejoué isolément
il donne 4 → 0, et le passage final donne 4 → 1 : les deux passent. Ce critère
compte des relevés pendant 240 s, donc dépend du FPS — c'est un critère
**bruité**, pas une régression.

Ces deux critères sont sensibles au débit d'images, et l'herbe plus dense en
coûte. Le harnais gagnerait à les rendre insensibles au FPS plutôt qu'à compter
des frames. Ce n'est pas fait dans cette phase.

---

## 5. Options de jeu ajoutées

| Option | Effet |
|---|---|
| `--no-grass-cutout` | rend les quads d'herbe **opaques** (état Phase 11), densité inchangée |
| `--no-tile-cache` | reconstruit topologie et cibles de morphing à chaque tuile (état pré-Phase 18) |

Ce sont des **témoins de mesure**, au même titre que `--no-vegetation`,
`--no-grass` et `--no-wildlife` : ils permettent d'attribuer un écart à ce qui a
été ajouté, sur le même binaire.

---

## 6. Prochaines étapes suggérées

1. **Paralléliser `terrainNodeMeshData`** sur un pool de threads (§2.5) : c'est
   désormais 83 % du coût d'une tuile, et la fonction est déjà pure.
2. **Teinter le terrain sous l'herbe** : le tapis est convaincant en volume,
   mais le sol reste l'orthophoto grise entre les touffes (§1.8).
3. **Buffer d'indices GPU partagé** : ~48 Mo de mémoire GPU récupérables.
4. **Rendre les scénarios E et I du harnais insensibles au FPS** (§4).
