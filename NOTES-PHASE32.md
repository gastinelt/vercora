# Phase 32 - Optimisation des ressources (memoire GPU, RAM, allocations, CPU)

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
| ligne `Ressources ... (PHASE 32)` | au chargement, toutes les 10 s, et en fin de partie (pics de session) |
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

---------------------------------------------------------------------------

## 5. Execution : ce qui a ete fait

Cinq commits sur la branche, chacun valide avant le suivant (tests headless,
couches de validation Khronos, comparaison d'images au pixel pres).

### P6 - ordre de dessin et culling (Renderer, Pipeline)
- Ordre objets -> vegetation -> TERRAIN (`kDrawOrder`). Le shader de fragment
  du terrain est le plus lourd du jeu ; dessine en dernier, ses fragments
  caches par les troncs et l'herbe sont ecartes par le test de profondeur
  precoce. Foret : -26 % de temps de rendu a lui seul.
- `ShaderSet::cullBackFaces`, terrain seulement (champ de hauteurs : un oeil
  au-dessus du relief rencontre toujours une face avant d'abord).
  `frontFace = COUNTER_CLOCKWISE` une fois l'axe Y de la projection retourne,
  verifie par le calcul d'aire de la spec ET a l'image.

### P1 a P4 - geometrie compacte et pool (GpuVertex, GpuBufferPool, Mesh)
- `renderer/GpuVertex.hpp` : `ObjectVertex` (44 o, conversion exacte de
  `Vertex`) et `TerrainVertex` (16 o : altitude, altitude cible, normales x/z
  en snorm16). `Vertex` (68 o) reste le format de TRAVAIL cote CPU : aucun
  generateur ni aucun test n'a change de format.
- `terrain.vert` recalcule x/z, la cible de morphing et l'UV depuis
  `gl_VertexIndex` et `DrawItem::grid` (ix0, iz0, pas, sommets par rangee) +
  `UniformBufferObject::terrainGrid` (origine, pas, cellules). `precise`
  interdit la fusion en FMA : les positions sont celles de
  `Heightmap::worldCoord` au bit pres (verifie en C++ par les tests).
- Topologie des tuiles televersee UNE fois en uint16 (`addSharedIndices`) ;
  indices des objets en uint16 ; une seule liaison d'indices par image pour
  tout le terrain (`bindMesh` saute les liaisons redondantes).
- `GpuBufferPool` (blocs de 4 Mo, `RangeAllocator` premier ajustement, bloc
  dedie au-dela d'un demi-bloc, un bloc vide garde en reserve) : un `Mesh` ne
  possede plus de buffer mais des PLAGES, rendues au pool par le meme retrait
  differe qu'avant (kMaxFramesInFlight + 1 images).
- Memoire unifiee + GPU integre ou logiciel (`preferDirectGeometryWrites`) :
  blocs mappes, la geometrie est ECRITE la ou le GPU la lit - ni staging, ni
  copie, ni barriere. GPU discret : staging de la Phase 12, avec une barriere
  d'execution VERTEX_INPUT -> TRANSFER avant les copies (une plage recyclee a
  ete lue par des images precedentes). `--force-staging` eprouve ce chemin.
- `writeTerrainNodeVertices` : les sommets d'une tuile ecrits directement au
  format compact (plus de `MeshData` de 287 Ko par tuile, plus de copie).
  `terrainNodeMeshData` reste la reference testee : les deux partagent le meme
  parcours (`forEachNodeVertex`), ils ne peuvent pas diverger.
- `--no-lod` : `terrainGpuMeshData` produit directement le format compact
  (1,7 Go de `MeshData` en moins au pic sur la zone au pas de 1 m).

### P5 - orthophoto (TextureFile, Texture)
- `loadTextureFileHeader` + `readTexturePayload` (entier ou par tranche) ;
  `loadTextureFile` en est la composition.
- `Texture` remplit son staging par une source et le REUTILISE par bandes de
  rangees de blocs (4 Mo au plus) au lieu d'un staging de toute la chaine.
  L'orthophoto n'est plus jamais tenue en RAM.

### P7 - construction des tuiles interieures (TerrainNodeMesh)
- Loin du bord, les altitudes sont lues une fois dans une grille bordee de
  deux rangs, puis les MEMES operations flottantes que `sampleGrid`. Bords et
  temoin `--no-tile-cache` : chemin de reference.

### En plus du plan
- `--vsync` : presentation FIFO. Le plus gros levier de CONSOMMATION (378
  images/s sur l'UHD 770 = GPU a 100 % pour un ecran a 60 Hz). Non active
  par defaut : les mesures du projet comptent des images non plafonnees.
- Ligne `Ressources (PHASE 32)` completee par le bilan du pool de geometrie.
- Televersements surs en cas d'echec d'allocation : le `Mesh` possede ses
  plages des leur reservation, et aucune copie n'est confiee a l'image
  suivante avant que toutes les ecritures du maillage aient reussi.
- Le C4244 connu de main.cpp (Phase 27, `groundDetailPattern`) : conversion
  rendue explicite, meme valeur.

## 6. Bilan chiffre (meme banc, memes scenarios, moyenne de 2 passages)

Release, lavapipe, 1280 x 720, zone proxy. "RAM" inclut ici la memoire GPU
(lavapipe l'alloue dans le processus) : sur l'UHD 770 elle n'y figure pas.

| | foret avant | foret apres | panorama avant | panorama apres | survol avant | survol apres |
|---|---:|---:|---:|---:|---:|---:|
| memoire GPU, fin de partie | 75,0 Mo | **55,5 Mo** | 71,6 Mo | **54,6 Mo** | 254 Mo | **86,7 Mo** |
| memoire GPU, pic de session | 102,6 Mo | **55,5 Mo** | 96,6 Mo | **54,6 Mo** | 285 Mo | **86,7 Mo** |
| dont geometrie de terrain (pic) | 27 Mo | **4 Mo** | 24 Mo | **4 Mo** | 188 Mo | **33 Mo** |
| blocs vkAllocateMemory vivants (pic) | 436 | **10** | 408 | **10** | 1 366 | **18** |
| appels vkAllocateMemory (session) | 439 | **13** | 411 | **13** | ~16 950 | **22** |
| pic RAM au chargement | 302 Mo | **233 Mo** | 302 Mo | **234 Mo** | 302 Mo | **234 Mo** |
| pic memoire GPU au chargement | 89,2 Mo | **50,5 Mo** | 89,2 Mo | **50,5 Mo** | 89,2 Mo | **50,5 Mo** |
| temps de chargement | 0,48 s | **0,38 s** | 0,47 s | **0,38 s** | 0,47 s | **0,38 s** |
| pic RAM de session | 463 Mo | **409 Mo** | 353 Mo | **312 Mo** | 605 Mo | **374 Mo** |
| tuile : geometrie | 167 us | **142 us** | 194 us | **139 us** | 202 us | **105 us** |
| tuile : televersement | 210 us | **0,5 us** | 207 us | **0,6 us** | 219 us | **0,7 us** |
| streaming terrain, CPU par image | 119 us | **45 us** | 58 us | **35 us** | 4,3 ms | **0,93 ms** |
| temps de rendu (drawFrame) | 120 ms | **83 ms** | 51 ms | **43 ms** | 62 ms | **52 ms** |
| images/s (lavapipe) | 8,2 | **11,8 (+44 %)** | 19,4 | **22,7 (+17 %)** | 14,6 | **18,0 (+24 %)** |

Les images/s et le temps de rendu sont ceux d'un GPU LOGICIEL : ils disent le
sens et l'ordre de grandeur du gain (moins de fragments de terrain ombres,
moins d'octets lus par sommet), pas le chiffre de l'UHD 770. Les octets, les
allocations et les couts CPU de construction, eux, ne dependent pas du GPU.

Maillages d'objets (27 maillages, 4 070 sommets, 1 504 triangles) : 295 Ko ->
188 Ko (-36 %), en 1 bloc au lieu de 54 allocations (et 54 de staging).

## 7. Verification

- **Tests headless** : 3 963 / 3 963 (3 943 avant : +20 verifications),
  compiles avec GCC 13 (0 avertissement en -Wall -Wextra) et Clang 18. Les
  25 verifications qui ont besoin des vraies dalles LiDAR sont ignorees ici,
  comme toujours sans elles.
- **Validation Khronos** (Debug) : 0 message sur foret, panorama et survol ;
  **validation de synchronisation** : 0 message, chemins direct ET staging.
  Controle negatif : sans la barriere VERTEX_INPUT -> TRANSFER, 414 messages
  SYNC-HAZARD-WRITE-AFTER-READ en 30 s de survol avec `--force-staging`.
- **Images** (sans faune : les animaux bougent en temps reel, leur position
  depend du nombre d'images rendues) :
  - foret : 353 pixels sur 921 600 (0,038 %) ; 329 a +/- 1 niveau
    (normales quantifiees) et 24 aux pieds des brins et des troncs, la ou leur
    profondeur EGALE celle du sol. Preuve : une variante qui rend ces egalites
    au terrain redonne l'image de reference au bit pres ;
  - panorama : 1 149 pixels (0,125 %), dont ~250 isoles sur les parois quasi
    verticales vues en rasant (culling, voir ShaderSet::cullBackFaces) ;
  - chemin staging (`--force-staging`), maillage parallele
    (`--tile-threads=4`), orthophoto par tranches, chemin rapide des tuiles :
    chacun IDENTIQUE au bit pres au chemin qu'il remplace ;
  - `--no-lod` et LOD : identiques entre eux sur le terrain synthetique.

## 8. Ce qui reste, et pour qui

- **A re-mesurer sur l'UHD 770** : les images/s Release (`profile_phase27.ps1`,
  `profile_phase29.ps1`) et `run_vulkan_check.ps1` complet. Les lignes
  `Ressources (PHASE 32)` donnent desormais la memoire GPU exacte dans le
  journal de chaque scenario.
- **Staging sur GPU discret** : un staging (une allocation) par
  televersement, comme avant la phase. Un anneau de staging persistant
  ramenerait ce chemin au niveau du chemin direct ; non ecrit faute de GPU
  discret pour l'eprouver.
- **Vertex pulling du terrain** (16 -> ~4,5 o par sommet) : 25 Mo de mieux au
  pic du survol. Voir section 4.
- **Cache de pipelines** (`VkPipelineCache` sur disque) : demarrage plus
  rapide sur un pilote sans cache propre ; le pilote Intel en a un.
- **CPU par image** : le profil (perf) ne montre plus que le semis de l'herbe
  et des arbres (Phase 18) ; rien d'autre au-dessus du bruit.

## 9. Reproduire

    # Linux : dependances (Ubuntu 24.04)
    apt-get install mesa-vulkan-drivers libvulkan-dev glslc vulkan-validationlayers \
        ninja-build libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
        libxkbcommon-dev xvfb imagemagick python3-numpy
    cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DGLFW_BUILD_WAYLAND=OFF
    ninja -C build-rel
    python3 tools/make_bench_zone.py          # sans les vraies donnees seulement
    Xvfb :99 -screen 0 1280x720x24 &
    DISPLAY=:99 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
        build-rel/vercora assets/terrain/vercora_zone_1m.vchm --lock-input \
        --hour=13 --time-scale=0 --freeze-anim --start=901800,6435800 \
        --look=0,-8 --quit-after=30
    # puis lire les lignes "Ressources", "Cout par image", "Construction de tuiles"
    # validation de synchronisation : Debug + VK_LAYER_ENABLES=
    #   VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
