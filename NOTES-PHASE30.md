# Phase 30 - Optimisation des ressources (memoire GPU, RAM, allocations, CPU)

Session du 2026-10-13, en environnement cloud (Linux, sans GPU). Demande : un
plan d'optimisation des ressources "a l'octet pres", une revue du noyau moteur
et Vulkan, un banc de mesure, l'execution du plan, puis une re-mesure.

Ce fichier a deux parties : le PLAN (ecrit avant de toucher au moteur, avec la
mesure de reference) et le BILAN (apres execution, meme banc). Detail des
chiffres bruts : section "Banc de mesure".

---------------------------------------------------------------------------

## 1. Comment mesurer sans la machine de developpement

Pas de GPU, pas de Windows, pas de donnees LiDAR (le proxy reseau refuse
data.geopf.fr). Il a donc fallu :

- **Builder sous Linux.** Deux defauts de portabilite l'empechaient :
  - `Heightmap(const Params& = {})`, `TerrainQuadtree(..., const Params& = {})`
    et `TerrainStreamer(..., const Params& = {})` : un argument par defaut qui
    construit une struct IMBRIQUEE a initialiseurs de membres est refuse par GCC
    et Clang (CWG 1397), accepte par MSVC. Remplace par une surcharge qui
    delegue (meme semantique, `explicit` conserve).
  - `<GLFW/glfw3.h>` incluait `<GL/gl.h>` (present avec le SDK Windows, absent
    sans libgl-dev sous Linux). `GLFW_INCLUDE_NONE` ajoute a cote de
    `GLFW_INCLUDE_VULKAN`.
  Le projet compile desormais sans avertissement nouveau avec GCC 13 (Release
  et Debug), et les tests headless passent avec Clang 18.
- **Un GPU logiciel** : lavapipe (Mesa 25.2, Vulkan 1.4) sous Xvfb 1280x720.
  Les couches de validation Khronos tournent dessus. Le rendu y est
  DETERMINISTE : deux executions identiques donnent 0 pixel different, ce qui
  permet de prouver au bit pres qu'une optimisation ne change pas l'image.
- **Une zone proxy a la vraie echelle** : `tools/make_bench_zone.py` ecrit un
  relief fBm de 5001 x 5001 au pas de 1 m avec la georeference et la plage
  d'altitudes de la vraie zone (l'orthophoto versionnee s'y applique), et des
  cartes de couvert tirees de l'orthophoto elle-meme. Le quadtree (8 380
  noeuds), le streaming, la vegetation et tous les budgets memoire y
  travaillent a la vraie echelle. Le relief n'est pas le Vercors : les
  chiffres sont comparables ENTRE EUX (avant/apres), pas a ceux de l'UHD 770.

Outillage ajoute au jeu pour ce banc (et utilisable sur la machine de dev) :

| ajout | role |
|---|---|
| `VulkanContext::allocateMemory / freeMemory` | TOUTE la memoire GPU passe par le contexte, qui la compte : octets vivants, pics, blocs vivants, appels `vkAllocateMemory` |
| `core/ProcessMemory` | RAM residente et pic du processus, lus au systeme (`/proc/self/status` sous Linux, `GetProcessMemoryInfo` sous Windows) |
| ligne `Ressources ... (PHASE 30)` | au chargement, toutes les 10 s, et en fin de partie (pics de session) |
| `--quit-after=S` | fermeture propre apres S s de boucle : tous les bilans sont ecrits (remplace le WM_CLOSE poste de l'exterieur) |
| `--freeze-anim` | fige l'horloge d'animation des shaders (herbe, pluie) : images comparables au pixel |

Scenarios (Release, `--lock-input --hour=13 --time-scale=0 --freeze-anim`,
30 s de boucle, deux passages chacun) :

- **foret** : a pied dans la foret sud (`--start=901800,6435800 --look=0,-8`) ;
- **panorama** : depuis le point culminant, vue lointaine sur la zone
  (`--start=900175,6438524 --look=45,-6`) ;
- **survol** : `--flythrough=900,120`, le cas de stress du streaming.

## 2. Mesure de reference (avant toute optimisation)

| | foret | panorama | survol |
|---|---:|---:|---:|
| memoire GPU en fin de partie | 75,0 Mo | 71,6 Mo | 251-256 Mo |
| pic memoire GPU | 102,6 Mo | 96,6 Mo | **283-286 Mo** |
| dont geometrie de terrain (pic) | 27 Mo / 74 tuiles | 24 Mo / 67 tuiles | **188 Mo / 512 tuiles** |
| blocs `vkAllocateMemory` vivants (pic) | 436 | 408 | 1 374 |
| appels `vkAllocateMemory` sur la session | 439 | 411 | **~17 000** |
| construction d'une tuile (geometrie + televersement) | 167 + 210 us | 194 + 207 us | 202 + 219 us |
| streaming terrain, CPU par image | 119 us | 58 us | **4,1-4,5 ms** |
| RAM (pic, lavapipe compris) | 463 Mo | 353 Mo | 606 Mo |
| images/s (lavapipe, CPU) | 8,2 | 19,5 | 14,5 |

Validation : 0 message sur les trois scenarios (Debug + couches Khronos).

Repartition de la RAM du moteur (heaptrack, scenario foret) : la heightmap
1 m (100 Mo de float) et les quatre cartes de vegetation (16 Mo) font
l'essentiel ; tout le reste du moteur (quadtree 1 Mo, caches de vegetation
< 1 Mo, maillages CPU transitoires) pese moins de 3 Mo. Le reste du processus
est lavapipe lui-meme (tramage logiciel, JIT LLVM) et n'existe pas sur l'UHD 770.

## 3. Revue du noyau moteur et Vulkan

Ce qui est BIEN et ne bouge pas :
- synchronisation : 2 images en vol, semaphore de fin de rendu par image de
  swapchain, fences, dependance de subpass corrigee en Phase 12 - correct ;
- une seule soumission par image, copies enregistrees en tete du command
  buffer avec UNE barriere (Phase 12) - le bon choix sur un GPU sans file de
  transfert dediee ;
- descriptor sets alloues une fois, aucun `vkUpdateDescriptorSets` par image ;
- push constants pour le par-objet, instanciation pour la vegetation,
  orthophoto deja en BC1 avec mipmaps precalculees ;
- aucune allocation de tas notable par image cote moteur (heaptrack).

Ce qui COUTE, par ordre d'importance mesuree :

1. **Le format de sommet unique de 68 octets.** Tout passe par `Vertex`
   (position, couleur, normale, UV, et 24 octets de cible de geomorphing).
   Une tuile de terrain = 4 225 sommets x 68 o = 287 Ko, alors que le terrain
   n'a besoin que de DEUX altitudes et DEUX normales par sommet : x, z et UV
   se deduisent de l'indice du sommet, la couleur est constante (blanc).
   Les maillages d'objets, eux, portent 24 octets de morphing qu'aucun de
   leurs shaders ne lit.
2. **Une topologie recopiee par tuile.** Les 24 576 indices sont identiques
   pour toutes les tuiles (l'en-tete de TerrainNodeMesh le dit depuis la
   Phase 10) mais chaque tuile en televerse sa copie : 96 Ko par tuile, en
   uint32 alors que 4 225 sommets tiennent en uint16.
3. **Une allocation Vulkan par buffer.** 2 par maillage, plus 2 de staging par
   televersement : ~17 000 `vkAllocateMemory` en 30 s de survol. C'est un
   appel noyau sous Windows (WDDM), arrondi a sa granularite d'allocation, et
   plafonne par `maxMemoryAllocationCount` (souvent 4 096) - le plafond de 512
   tuiles residentes n'existait que pour lui.
4. **Un staging inutile en memoire unifiee.** Sur l'UHD 770 (et lavapipe), la
   memoire device-local EST mappable : ecrire la tuile directement la ou le
   GPU la lit evite un buffer staging, une copie GPU et une barriere par tuile.
5. **Le chargement de l'orthophoto** tient trois copies de 42 Mo en meme
   temps (fichier lu en RAM, staging, image GPU).
6. **Le terrain est dessine AVANT la vegetation**, sans culling des faces
   arriere : en foret, le shader de terrain (le plus lourd du jeu) est execute
   pour des pixels que les troncs et houppiers recouvrent ensuite.
7. **La construction d'une tuile lit chaque altitude ~6 fois** (cinq lectures
   ecretees par echantillon, plus la grille du parent).

## 4. Le plan

Chaque point dit ce qu'il gagne, ce qu'il risque, et COMMENT on prouve qu'il ne
casse rien. Regle du projet appliquee : ce qui doit etre invisible est verifie
AU PIXEL PRES contre la reference (lavapipe est deterministe) ; ce qui change
l'image est chiffre en pixels touches et en ecart maximal.

| # | chantier | gain attendu | image | preuve |
|---|---|---|---|---|
| P1 | Terrain : sommet compact 16 o (2 altitudes f32 + 2 normales snorm16), x/z/UV deduits de l'indice du sommet, indices PARTAGES en uint16 | geometrie terrain -82 % (385 Ko -> 68 Ko par tuile) | normales quantifiees a 3e-5 pres : ecart d'arrondi | tests : altitudes au bit pres, normales a 1/32767 pres ; captures |
| P2 | Pool de sous-allocation (blocs de 4 Mo, premier ajustement) pour TOUTE la geometrie, liberation differee comme avant | ~17 000 -> quelques dizaines d'allocations ; fin des petits blocs | aucun | captures au bit pres ; validation |
| P3 | Ecriture DIRECTE en memoire unifiee (GPU integre/CPU), staging conserve sur GPU discret (`--force-staging` pour l'eprouver) | plus de staging ni de copie par tuile | aucun | captures identiques dans les deux modes |
| P4 | Maillages d'objets : format GPU de 44 o (sans les 24 o de morphing), exact | -35 % sur ces maillages | aucun | au bit pres |
| P5 | Orthophoto lue directement dans le staging | -42 Mo de pic RAM au chargement, une copie de 42 Mo en moins | aucun | au bit pres |
| P6 | Ordre de dessin objets -> vegetation -> terrain (early-Z) + culling des faces arriere du terrain | moins de fragments de terrain ombres | aucun (opaque, test LESS) | au bit pres |
| P7 | Tuile : altitudes lues UNE fois (grille bordee), chemin de reference garde pour les bords | geometrie de tuile ~-50 % CPU | aucun | test : bit a bit identique a la reference sur toutes les tuiles |

NON RETENU, et pourquoi :
- **Quantifier la heightmap** (100 Mo -> 50 Mo en uint16) : elle est la verite
  PHYSIQUE (collision, marche, velo, escalade, semis). Les altitudes ne sont
  pas des centimetres ronds (mediane paire, bouchage, raccords de dalles) : il
  n'existe pas de codage 16 bits sans perte. 100 Mo est le prix de l'exactitude.
- **Cartes de vegetation en uint8** (16 -> 4 Mo) : le semis est deterministe
  et compare la densite a des seuils ; quantifier deplacerait des arbres.
- **InstanceData 28 -> 20 o** : `variation` porte un entier code (gradient des
  rochers, Phase 29) qui ne survit pas a un demi-flottant ; le gain (~100 Ko
  par image) ne vaut pas le risque.
- **Abandonner le niveau 0 de l'orthophoto** (-32 Mo) : 0,61 -> 1,22 m/texel,
  une perte de qualite visible a pied.
- **Tirer les altitudes d'un SSBO** (vertex pulling, 16 -> ~4,5 o/sommet) :
  25 Mo de mieux au pic du survol, pour un changement de modele d'entree, un
  descripteur de plus et un shader a bords exacts. A reprendre si le pas
  LiDAR s'affine.
