# NOTES — Phase 10 : quadtree, LOD continu et frustum culling

Session du 2026-08-21. Suite directe du verdict de la Phase 9 (`NOTES-PHASE9.md`
§6) : « le moteur envoie un seul maillage entier à chaque image, le coût est
proportionnel à la surface, le streaming par tuiles devient nécessaire
maintenant ».

Cette session pose les fondations demandées : structure de quadtree, sélection de
LOD par distance avec geomorphing continu (CDLOD), frustum culling par nœud, et
un cache de tuiles GPU. Le découplage rendu/collision est vérifié.

---

## 1. Le résultat, d'abord

Même zone, même position de caméra (le centre du terrain, E 901500 N 6437500,
altitude 1947 m), même binaire, `tools\measure_terrain.ps1`, Debug avec couches
de validation, Intel UHD 770.

| Scène | Phase 9 (bloc unique) | **Phase 10 (quadtree + LOD + culling)** | Gain |
|---|---:|---:|---:|
| **25 km² au pas de 1 m** | 12 FPS / 4 014 Mo | **487 FPS / 246 Mo** | **×40 FPS, ÷16 RAM** |
| **25 km² au pas de 2 m** | 46 FPS / 1 083 Mo | **601 FPS / 166 Mo** | ×13 FPS, ÷6,5 RAM |
| 1 dalle 1 m (Phase 7) | 247 FPS / 302 Mo | **847 FPS / 141 Mo** | ×3,4 FPS, ÷2,1 RAM |
| Procédural (Phase 4) | 1 381 FPS / 108 Mo | 1 244 FPS / 106 Mo | ×0,90 (voir §6) |

**Zéro message `[Validation]`, zéro `[ERROR]`/`[WARN]` sur les quatre scènes.**

Le chiffre à retenir n'est pas seulement le FPS : sur la zone au pas de 1 m, la
géométrie réellement soumise au GPU passe de **50 000 000 de triangles à 548 864**
(67 tuiles × 8 192), soit **1,1 %**. Le temps de chargement tombe de **10,4 s à
0,5 s**, parce qu'on ne construit plus 25 M de sommets pour les jeter dans un
seul buffer.

### Le contrôle qui compte : ce n'est pas un culling cassé

Un bug de culling produit d'excellents FPS — on ne dessine plus rien. Le chiffre
seul ne prouve donc rien. `tools\screenshot_scene.ps1` (nouveau) capture la même
vue avec et sans LOD : `build\shots\zone_lod.png` et `build\shots\zone_nolod.png`
sont visuellement **identiques** (relief, falaise, arbres, pierre au sol), à la
finesse du maillage lointain près, qui est précisément l'effet recherché.

---

## 2. Architecture

Quatre briques neuves, séparées selon la ligne de fracture habituelle du projet
(ce qui dépend de Vulkan / ce qui n'en dépend pas et se teste sans GPU).

| Fichier | Rôle | Dépend de Vulkan |
|---|---|---|
| `src/core/Frustum.hpp` | 6 plans du tronc de vision, test boîte, distance à une boîte | non |
| `src/world/TerrainQuadtree.{hpp,cpp}` | arbre, bornes, sélection de LOD, culling | non |
| `src/renderer/TerrainNodeMesh.{hpp,cpp}` | maillage d'une tuile + cible de geomorphing | non |
| `src/renderer/TerrainStreamer.{hpp,cpp}` | cache GPU, budget, éviction LRU, `DrawItem` | oui |

Les trois premières sont compilées **et exécutées** dans `tests/logic_tests.cpp` ;
la quatrième est vérifiée en syntaxe seulement. C'est la même séparation qui
avait permis de tester la Phase 7 sans GPU.

### 2.1 Le quadtree

La grille d'altitudes est découpée en nœuds carrés. **Chaque nœud est toujours
maillé avec le même nombre de sommets** — 65 × 65, soit 8 192 triangles. Ce qui
change d'un niveau à l'autre, c'est le **pas d'échantillonnage** :

```
niveau 0 (feuille) : pas 1     -> couvre 64 cellules   (64 m au pas de 1 m)
niveau L           : pas 2^L   -> couvre 64 * 2^L cellules
```

Un nœud de niveau L coûte donc exactement le même prix GPU qu'une feuille tout en
couvrant 4^L fois plus de terrain. La racine est le plus petit niveau dont
l'emprise couvre la grille entière.

| Terrain | Cellules | Niveaux | Nœuds | Tuile L0 |
|---|---:|---:|---:|---:|
| Procédural | 127 | 0..1 | 5 | 50 m |
| Dalle Phase 7 | 1 000 | 0..4 | 341 | 64 m |
| Zone 25 km² 2 m | 2 500 | 0..6 | 2 139 | 128 m |
| Zone 25 km² 1 m | 5 000 | 0..7 | 8 380 | 64 m |

Deux détails de construction qui ont leur importance :

- **Les bornes verticales d'une feuille sont calculées à la PLEINE résolution**,
  pas au pas du nœud. Une boîte trop petite ferait disparaître du terrain au
  culling ; les bornes des nœuds internes remontent ensuite des fils, ce qui est
  exact et gratuit.
- **Les indices sont écrêtés sur le bord du terrain.** 5 000 n'est pas un
  multiple de 64 : la racine couvre 8 192 cellules, dont 3 192 hors données. Sans
  écrêtage on obtiendrait une « étagère » plate de 3,2 km au-delà du terrain ;
  avec, les sommets excédentaires se superposent au bord et produisent des
  triangles dégénérés, invisibles.

### 2.2 Sélection de LOD

Une portée par niveau, qui **double** à chaque niveau :

```
range[L] = rangeFactor * tailleMonde(tuile de niveau L)     (rangeFactor = 3)
```

Au pas de 1 m : `L0 ≤ 192 m, L1 ≤ 384 m, ... L7 ≤ 24 576 m`.

La descente part de la racine :

```
si le nœud est hors du frustum          -> abandon (tout le sous-arbre tombe)
si niveau 0, ou distance >= range[L-1]  -> le nœud est DESSINÉ à son niveau
sinon                                   -> on descend dans les 4 fils
```

La distance est celle de la caméra à la **boîte** du nœud, pas à son centre : sur
une grande tuile dont on rase le bord, la distance au centre donnerait un niveau
trop grossier.

Le résultat est un anneau concentrique de niveaux autour de la caméra, avec la
pleine résolution (pas de 1 m) au contact immédiat du joueur. Un test headless
vérifie que **les tuiles retenues pavent le terrain exactement** — somme des
emprises = surface du terrain, donc ni trou ni recouvrement.

### 2.3 Geomorphing continu — et pourquoi il ne sert pas qu'à l'esthétique

Chaque sommet porte, en plus de sa position et de sa normale, **sa position et sa
normale dans la grille du niveau supérieur** (deux attributs de sommet neufs,
locations 4 et 5). La cible s'obtient en arrondissant les indices **locaux** vers
le bas à un multiple de deux : un sommet d'indice pair est déjà sur la grille du
parent et ne bouge pas, un sommet impair vient se confondre avec son voisin pair.
À morph = 1, la tuile a donc **exactement** la forme de son parent.

Le vertex shader interpole selon la distance du sommet à la caméra, dans une
bande `[0,70 × range[L], 0,95 × range[L]]` poussée par push constant.

Deux effets, et le second est le plus important :

1. **Pas de popping** au changement de niveau : la géométrie fine a déjà pris la
   forme de la grossière au moment de la bascule.
2. **Pas de fissure entre tuiles de niveaux différents.** Au-delà de `range[L]`,
   une tuile de niveau L est entièrement morphée sur la grille L+1 ; sa voisine
   de niveau L+1 y est encore à morph = 0, donc sur cette même grille. Les deux
   bords coïncident au sommet près. C'est ce qui évite d'avoir à coudre les
   bords ou à poser des jupes.

C'est pour cette raison que `morphStartRatio` **doit** rester > 0,5 (sinon la
bande commence avant `range[L-1]` et le raccord se rouvre). Le constructeur
refuse une valeur invalide, et un test headless le vérifie.

La distance est mesurée sur la position **non morphée** : c'est ce qui garantit
qu'un sommet partagé par deux tuiles voisines obtient le même facteur des deux
côtés.

### 2.4 Frustum culling

`Frustum::fromViewProjection` extrait les six plans par la méthode
Gribb-Hartmann. Un piège évité : le plan **proche** est `row2` et non
`row3 + row2`, parce que `GLM_FORCE_DEPTH_ZERO_TO_ONE` est actif (convention
Vulkan, z ∈ [0,1]) — prendre la convention OpenGL donnerait un plan proche deux
fois trop loin devant la caméra.

Le test boîte/frustum est **conservateur dans le bon sens** : une boîte hors
champ peut être déclarée visible (on dessine pour rien, c'est bénin), mais une
boîte visible ne l'est jamais déclarée invisible — ce serait un trou.

La matrice de projection vient de `Renderer::projectionMatrix()`, exposée pour
l'occasion, et `updateUniformBuffer` l'appelle désormais aussi : **une seule
définition**, donc aucun risque que le culling et le rendu ne voient pas le même
tronc de vision.

### 2.5 Résidence des tuiles (`TerrainStreamer`)

Le quadtree de la zone 1 m compte 8 380 nœuds ; les mailler tous coûterait plus
cher que le bloc unique de la Phase 9. Seules les tuiles **sélectionnées** sont
construites, et gardées tant que la place le permet. Trois garde-fous :

1. **Budget par frame** (4 tuiles). Chaque upload passe aujourd'hui par un
   staging + `vkQueueWaitIdle` (`VulkanBuffer::copyFrom`), c'est-à-dire une
   synchronisation complète avec le GPU. Les tuiles manquantes sont servies aux
   frames suivantes, **les plus proches d'abord** — la sélection est triée par
   distance exprès. La toute première frame est exemptée, pour que la vue
   initiale s'affiche d'un coup.
2. **Plafond de résidence** (512 tuiles ≈ 188 Mo). Chaque tuile consomme deux
   allocations Vulkan, et `maxMemoryAllocationCount` est typiquement de 4 096.
3. **Éviction LRU**, jamais sur une tuile utilisée dans la frame courante (ce
   serait du thrashing pur).

En pratique, sur la zone 1 m à l'arrêt : **67 tuiles sélectionnées, 67 dessinées,
67 résidentes, 24 Mo de géométrie GPU**. Le plafond n'est jamais approché.

---

## 3. Ce qu'il a fallu changer dans l'existant

| Fichier | Changement | Pourquoi |
|---|---|---|
| `Vertex.hpp` | +`morphPos`, +`morphNormal` (44 → 68 octets) | cibles de geomorphing |
| `VertexFormat.hpp` | 4 → 6 attributs décrits | idem |
| `Pipeline.{hpp,cpp}` | `ShaderSet::usesMorphAttributes` | voir ci-dessous |
| `DrawItem.hpp` | +`params` (vec4), +`DrawItemPush` | bande de morphing par tuile |
| `Renderer.{hpp,cpp}` | `removeMesh`, réemploi de poignées, `projectionMatrix()`, `addMesh(…, log)` | le streaming crée et détruit des maillages en permanence |
| `Mesh.{hpp,cpp}` | paramètre `log` | une ligne de journal par tuile noierait tout |
| `shaders/terrain.vert` | geomorphing | — |
| `shaders/cube.vert` | déclare le bloc push constant complet | plage identique pour les deux pipelines |
| `main.cpp` | quadtree par défaut, option `--no-lod` | mesure avant/après sur le même binaire |

Deux points valent d'être notés.

**`ShaderSet::usesMorphAttributes` n'est pas une optimisation, c'est une
correction.** Décrire au pipeline un attribut qu'aucun shader ne lit fait émettre
aux couches de validation `Vertex attribute at location 4 not consumed by vertex
shader`, et le contrat du projet est zéro message. Le format de sommet reste
unique côté C++ ; c'est seulement sa *description* au pipeline qui est tronquée
à 4 attributs pour le matériau `Object`. Le stride reste `sizeof(Vertex)`.

**La destruction d'un mesh est différée de `kMaxFramesInFlight + 1` frames.** Le
GPU peut encore lire les buffers d'une frame déjà soumise ; les détruire tout de
suite serait une erreur de validation immédiate. Le vieillissement se fait juste
après l'attente de la fence, c'est-à-dire à un instant où l'on sait qu'une frame
de plus est réellement terminée.

---

## 4. Découplage rendu / collision — vérifié

**Rien n'a changé côté physique, et c'est le résultat attendu.** La `Heightmap`
pleine résolution reste en mémoire et reste la source **unique** de
`heightAt()` : c'est elle que consomment `Camera` (marche), `Forest` (abattage,
collision troncs/souches), `Ground` (objets au sol) et `Paraglider`
(atterrissage). Le quadtree ne détient que des **bornes** ; il n'a ni copie ni
version simplifiée des altitudes, et ne peut donc pas influencer la physique.

Trois vérifications :

1. Test headless dédié : `heightAt()` donne la même valeur au bit près, que le
   quadtree vienne d'être sélectionné depuis 5 000 m d'altitude (tout au niveau
   le plus grossier) ou au contact du point mesuré (tout au niveau 0).
2. Test headless : au nœud de grille, `heightAt()` vaut exactement `at()` — la
   surface de collision est bien celle de la donnée, pas celle d'une tuile
   grossière.
3. `run_vulkan_check.ps1`, 4 scénarios sur 4 **du premier coup** : 2 arbres
   abattus, 3 débitages, 8 objets ramassés (bûche, branche, pierre, éclat), 1
   percussion réussie, chemin de collision tronc → souche, vol de parapente avec
   atterrissage naturel sur le relief LiDAR. Zéro validation.

Au contact du joueur, les tuiles sont de niveau 0 (pas de 1 m) : la surface
**affichée** y est donc également celle de la donnée. L'écart rendu/collision
n'existe qu'au loin, là où personne ne marche.

---

## 4bis. Traversée en mouvement — le vrai test du streaming

`measure_terrain.ps1` mesure à l'arrêt : le cache de tuiles y est figé après la
première frame, et rien n'est jamais évincé. C'est `traverse_terrain.ps1` qui
exerce réellement le streaming.

**20 km parcourus sur la zone 25 km² au pas de 1 m** (2,5 km par branche, aller-
retour sur les deux axes, ce qui sort deux fois du terrain et y revient) :

| Branche | Distance | FPS moyen | FPS min |
|---|---:|---:|---:|
| nord | 2 500 m | 725 | 210 |
| sud (traverse et sort au sud) | 5 000 m | 608 | 151 |
| nord (retour au centre) | 2 500 m | 459 | 146 |
| est | 2 500 m | 550 | 286 |
| ouest (traverse et sort à l'ouest) | 5 000 m | 564 | 370 |
| est (retour au centre) | 2 500 m | 555 | 339 |

**Zéro message `[Validation]`, zéro `[ERROR]`/`[WARN]` sur les 20 km.** Aucune
chute localisée aux anciennes frontières de dalles, aucun blocage, aucune chute à
travers le sol. Le **minimum instantané reste à 146 FPS** — soit douze fois le
chiffre *moyen* de la Phase 9 à l'arrêt.

Le maximum de 3 209 FPS sur les branches de 5 000 m n'est pas une anomalie : ces
branches sortent de l'emprise (le terrain fait ±2 500 m depuis le centre), la
caméra regarde alors hors du terrain et le culling ne retient presque rien. C'est
le comportement attendu.

### Le seul incident, et ce qu'il vaut

Sur ce run de 20 km, **le jeu n'est pas sorti dans les 30 s après `WM_CLOSE`** et
le script l'a tué (code -1). C'est le seul échec de la session. Enquête menée
plutôt que rangée sous le tapis :

- **Traversée de 3,2 km, mêmes conditions : sortie propre, code 0.**
- **Sonde dédiée chronométrant la fermeture** après 0 s, 60 s et 180 s de marche
  continue (jusqu'à **130 tuiles résidentes**, 47 Mo de géométrie GPU) :
  **0,1 s de fermeture dans les trois cas.**

Le temps de sortie **ne dépend donc pas** du nombre de tuiles résidentes ni de la
durée de la session. Rien ne rattache l'incident au streaming. Il correspond à la
flakiness de pilotage par `PostMessage` déjà documentée depuis la Phase 7 (voir
`MEMORY.md`) — mais **30 s reste beaucoup pour une simple lenteur, et l'incident
n'a pas été reproduit : il est signalé comme non élucidé, pas comme classé.** À
re-tenter au prochain run long.

---

## 5. Tests

- **`tests/logic_tests.cpp` : 185 → 223 assertions**, toutes passent. 38 neuves,
  réparties en : frustum (5 cas dont derrière-la-caméra et au-delà du far),
  `distanceToAABB`, structure de l'arbre, bornes de la racine, sélection par
  distance, tri, bande de morphing, **pavage exact du terrain**, culling effectif,
  maillage d'une tuile (sommets sur la grille au niveau 0), **propriétés du
  geomorphing** (pair immobile / impair confondu avec son voisin), topologie
  partagée, sommets dans les bornes, **découplage collision**, grille non multiple
  de 64, refus des paramètres invalides.
- **`run_vulkan_check.ps1` : 4/4, zéro validation, du premier coup.**
- **`traverse_terrain.ps1` : 20 km sur la zone 25 km², zéro validation** (§4bis),
  puis 3,2 km avec sortie propre.
- **Scènes non-terrain intactes** : `vercora.exe cube` tourne à 3 403 FPS sans
  message de validation (le matériau `Object` n'a que 4 attributs décrits, cf.
  §3), et `--dump-world` retrouve toujours **16 arbres visables sur 16**.
- **`measure_terrain.ps1`** : nouvelle option `-GameArgs` pour mesurer la même
  scène avec et sans LOD sans recompiler.
- **`screenshot_scene.ps1`** (nouveau) : capture la fenêtre via `PrintWindow` +
  `PW_RENDERFULLCONTENT` (sans ce drapeau une fenêtre Vulkan ressort noire) et
  estime la proportion de ciel, garde-fou automatique contre un terrain disparu.

---

## 6. Compromis et limites — à dire franchement

### Le chemin `--no-lod` a RÉGRESSÉ, et c'est le prix des attributs de morphing

Mesuré avec le binaire de la Phase 10 sur la zone 1 m :

| | Phase 9 | Phase 10 `--no-lod` |
|---|---:|---:|
| FPS | 12 | **9** |
| RAM | 4 014 Mo | **5 636 Mo** |

Le sommet est passé de 44 à 68 octets (+55 %) pour porter `morphPos` et
`morphNormal`. Sur 25 M de sommets, cela coûte 600 Mo de plus côté CPU et autant
côté GPU. **Ce chemin n'existe que pour la comparaison** ; le chemin réel, lui,
n'a jamais 25 M de sommets en vol. Deux façons de récupérer ça plus tard :
n'encoder que le *delta* de morphing (souvent nul), ou reprendre `color` et `uv`,
inutilisés par le terrain.

### Le terrain procédural perd 10 % de FPS (1 381 → 1 244)

Deux causes : les 127 cellules sont couvertes par 4 tuiles de 64 (donc 32 768
triangles au lieu de 32 258, plus un peu d'écrêtage), et le sommet est plus gros.
À 1 244 FPS c'est sans conséquence, mais c'est une vraie petite régression, pas
du bruit de mesure.

### Ce qui n'est PAS fait

- **Pas de transfert asynchrone.** `VulkanBuffer::copyFrom` fait un
  `vkQueueWaitIdle` : chaque tuile construite synchronise complètement le GPU.
  C'est ce qui impose le budget de 4 tuiles par frame. Le vrai correctif est une
  queue de transfert dédiée avec fence, pas un budget plus gros. **C'est le
  chantier n°1 de la suite.**
- **Pas de repli sur l'ancêtre.** Une tuile sélectionnée mais pas encore
  construite n'est simplement pas dessinée. À la marche (14 m/s) le budget suit
  largement ; en parapente à grande vitesse, des trous peuvent apparaître
  brièvement à l'horizon.
- **Pas d'index buffer partagé.** La topologie est identique pour toutes les
  tuiles (le test le vérifie) mais chaque tuile a encore son propre index buffer :
  98 Ko × nombre de tuiles gaspillés, et deux allocations Vulkan par tuile au lieu
  d'une. Économie facile, non faite.
- **Pas de streaming disque.** La `Heightmap` entière (95 Mo au pas de 1 m) est
  toujours chargée en RAM d'un bloc. C'est ce qui rend le découplage collision
  simple, et ce n'est pas le facteur limitant aujourd'hui — mais ça le deviendra
  quand la zone dépassera 25 km².
- **La contrainte « grilles carrées uniquement » de `TerrainFile` n'est pas
  tombée.** Elle n'était pas sur le chemin critique de cette session.
- **La scène par défaut n'a pas changé** : `vercora.exe` sans argument charge
  toujours la dalle de la Phase 7. La zone 25 km² tourne maintenant à 487 FPS et
  pourrait devenir le défaut ; c'est un choix de jeu, pas de moteur, et il revient
  à l'utilisateur.
- **Les paramètres du quadtree ne sont pas réglables en ligne de commande**
  (`nodeCells`, `rangeFactor`, budgets). Ils sont dans `TerrainStreamer::Params`
  avec des valeurs par défaut mesurées, mais les faire varier demande de
  recompiler.
- **Aucune mesure en mouvement rapide n'a été faite en parapente** sur la zone
  25 km² : le pilotage par PostMessage ne peut pas orienter la caméra, donc un vol
  scripté ne balaierait pas vraiment le terrain.
- **La sortie bloquée du run de 20 km n'est pas expliquée** (§4bis). Non
  reproduite, non corrélée à la résidence des tuiles, mais non classée.

### Ce qui reste incertain

Le raccord entre niveaux repose sur l'argument du §2.3, vérifié par construction
et par les tests headless sur les propriétés du morphing — mais **aucun test
automatique ne cherche une fissure dans l'image rendue**. Les captures ne
montrent rien de visible ; ce n'est pas une preuve.

---

## 7. Où sont les choses

| Chemin | Quoi |
|---|---|
| `src/core/Frustum.hpp` | plans du tronc de vision, test boîte, distance à une boîte |
| `src/world/TerrainQuadtree.{hpp,cpp}` | arbre, bornes, sélection de LOD, culling |
| `src/renderer/TerrainNodeMesh.{hpp,cpp}` | maillage d'une tuile + cible de geomorphing |
| `src/renderer/TerrainStreamer.{hpp,cpp}` | cache GPU, budget, LRU, `DrawItem` |
| `tools/screenshot_scene.ps1` | capture d'une scène (contrôle visuel automatisable) |
| `build/shots/zone_lod.png` | zone 25 km² 1 m avec LOD, 482 FPS |
| `build/shots/zone_nolod.png` | même vue sans LOD, 9 FPS — comparaison |

Commandes utiles :

```bash
tools\build.bat
tools\run_logic_tests.bat
powershell -File tools\run_vulkan_check.ps1
powershell -File tools\measure_terrain.ps1 -Scene assets\terrain\vercora_zone_1m.vchm
powershell -File tools\measure_terrain.ps1 -Scene assets\terrain\vercora_zone_1m.vchm -GameArgs --no-lod
powershell -File tools\screenshot_scene.ps1 -Scene assets\terrain\vercora_zone_1m.vchm -Out build\shots\zone_lod.png
```

---

## 8. Prochaines étapes suggérées

0. **Reproduire ou écarter la sortie bloquée du run de 20 km** (§4bis) : relancer
   une traversée longue et, si ça se reproduit, attacher un débogueur au moment du
   `WM_CLOSE`. C'est la seule inconnue de la session.
1. **Transfert asynchrone des tuiles** (queue dédiée + fence) — supprime le
   `vkQueueWaitIdle` par upload, donc le budget de 4 tuiles par frame, donc les
   trous en déplacement rapide. C'est le vrai verrou restant.
2. **Index buffer partagé** entre toutes les tuiles : une allocation par tuile au
   lieu de deux, et 98 Ko économisés par tuile. Facile.
3. **Alléger le sommet** : n'encoder que le delta de morphing, ou reprendre
   `color`/`uv` inutilisés par le terrain.
4. **Orthophoto BD ORTHO** sur E 899000–904000 / N 6435000–6440000 : premier
   contrôle *visuel* de l'orientation du relief, en attente depuis la Phase 7. Les
   UV sont générés par tuile en coordonnées du terrain ENTIER, donc continus :
   le placage n'aura rien à recalculer.
5. **Décider de la scène par défaut** — la zone 25 km² est maintenant jouable.
6. Exploiter la classification LiDAR (classe 5) pour semer les arbres là où il y
   en a vraiment.
