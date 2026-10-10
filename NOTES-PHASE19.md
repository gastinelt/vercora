# NOTES-PHASE19.md — Le profil de la Phase 18 était un profil de Debug

Session du 2026-08-26, à la suite de la Phase 18. Deux tâches étaient au
programme :

1. rendre `Renderer::addMesh` thread-safe pour paralléliser la construction de
   mesh de tuile — la piste que la Phase 18 avait désignée comme le gain
   suivant (83 % du coût restant) ;
2. profiler les autres postes du frame (terrain, végétation, faune) pour
   trouver des gains sûrs.

**Le résultat principal de la phase n'est aucun des deux, et il les annule tous
les deux** : la mesure qui fondait la Tâche 1 avait été relevée en **Debug**, et
elle ne tient pas en **Release**. En Release, le maillage d'une tuile n'est plus
le poste dominant, et le frame entier n'est pas limité par le CPU.

C'est la leçon de la Phase 18 (« un commentaire n'est pas une mesure »)
appliquée un cran plus haut : **un profil de Debug n'est pas un profil de
Release.**

Configurations et matériel : Intel UHD 770, 20 cœurs logiques, 1280x720,
caméra **verrouillée** (`--lock-input`), cadrage fixé par `--start` / `--look`
(sans `--look`, tout diff d'image est un artefact — cf. MEMORY.md). Sauf mention
explicite de « Release », les mesures sont en **Debug avec couches de
validation**, la seule configuration où le projet valide. **Zéro message
`[Validation]` sur la totalité des mesures de ce document.**

---

## 1. TÂCHE 1 — la parallélisation

### 1.1 Ce qui empêche réellement `Renderer::addMesh` d'être appelé depuis plusieurs threads

La question posée était précise, la réponse l'est aussi. `addMesh` touche
**quatre** ensembles d'état partagé, de dangerosité très inégale :

| # | État | Nature du problème |
|---|---|---|
| 1 | `meshes_`, `freeMeshSlots_`, `liveMeshCount_` | `meshes_.push_back` peut **réallouer** le vecteur pendant que `recordCommandBuffer` l'indexe depuis le thread de rendu. Course franche, sur un pointeur. |
| 2 | `pendingCopies_`, `stagingBytesInFlight_` | `enqueueCopy` fait `push_back` + `+=` sans verrou ; vidés à chaque `recordCommandBuffer`. |
| 3 | `commandPool_` | `flushCopiesNow` y alloue un command buffer. Un `VkCommandPool` et **tous** ses command buffers exigent une synchronisation externe : allouer dedans pendant que le thread de rendu réinitialise et enregistre le sien est une violation directe de la spécification. |
| 4 | `graphicsQueue_` | `flushCopiesNow` fait `vkQueueSubmit` + `vkQueueWaitIdle`. Un `VkQueue` exige lui aussi une synchronisation externe ; en concurrence avec le `vkQueueSubmit` de `drawFrame`, c'est une corruption de driver, pas une erreur de validation. |

Les points 1 et 2 se règlent avec un mutex, moyennant un coût sur le chemin de
rendu (le point 1 impose de verrouiller aussi la **lecture** de `meshes_` à
chaque draw, ou de figer la capacité du vecteur).

**Ce sont les points 3 et 4 qui condamnent l'exercice**, et pour une raison qui
n'est pas la difficulté : `flushCopiesNow` n'est atteint que lorsque le staging
en vol dépasse **128 Mo** (`kMaxPendingStagingBytes`). Une tuile pèse ~385 Ko et
le budget est de 64 tuiles par frame, soit ~25 Mo : **ce chemin ne se déclenche
jamais pendant le streaming**. Il ne se déclenche que sur `--no-lod`, qui
téléverse la zone entière en un bloc — avant la première frame, donc sans
concurrence.

Autrement dit : la partie dangereuse du code serait **exactement celle qu'aucun
test de cette session n'aurait pu exercer**. Un verrou oublié là ne se
manifesterait ni dans le harnais, ni sous `VK_LAYER_VALIDATE_SYNC`, ni dans
aucune des mesures ci-dessous — mais un jour, chez l'utilisateur, sur une scène
plus grosse. C'est la définition même du bug de concurrence indétectable que la
consigne demandait d'éviter.

**`Renderer::addMesh` n'a donc pas été modifié. Aucun appel Vulkan ne change de
thread dans cette phase.**

### 1.2 Ce qui a été fait à la place, et pourquoi c'était le bon découpage

La Phase 18 avait déjà noté que `terrainNodeMeshData` est **pure**. Vérifié
ligne à ligne : elle ne lit que `Heightmap::at/spacing/worldCoord` et
`TerrainQuadtree::node` — tous `const`, sur des données immuables pour la durée
de la session — et écrit dans un `MeshData` qui lui appartient. Aucun `static`,
aucun `mutable`, aucun cache interne.

Le travail se découpe donc sans toucher au Renderer :

- **`TerrainStreamer::meshBatch`** maille tout le lot de la frame, en parallèle.
  Aucun appel Vulkan. Chaque thread écrit dans des **éléments disjoints** de
  vecteurs déjà dimensionnés : ni verrou, ni file, ni compteur partagé — donc
  aucun état sur lequel une course serait possible ;
- **`TerrainStreamer::uploadNode`** pose la topologie et appelle `addMesh`, sur
  le thread de rendu, en séquence, dans le même ordre qu'avant.

Trois précautions, chacune pour une raison précise :

- une exception ne traverse **jamais** la frontière d'un `std::thread` (elle
  appellerait `std::terminate`) : elle est capturée et rendue comme une donnée
  dans `batchErrors_[k]` ;
- rien n'est **journalisé** depuis un thread : les fonctions de journalisation
  ne sont pas prévues pour ça. C'est l'appelant qui journalise, après jonction ;
- le chronomètre par tuile dépose sa durée dans un élément propre puis est sommé
  après jonction. Une course sur un compteur de **mesure** serait la pire de
  toutes : elle fausserait le chiffre censé prouver que le changement est bon.

Découpage **statique** en tranches contiguës, et non file de travail : une tuile
coûte toujours la même chose (65x65 sommets quel que soit son niveau), donc
l'équilibrage est naturel — et une file demanderait un verrou.

### 1.3 En Debug, le gain est réel

Survol scripté `--flythrough=900 --no-grass`, 45 s, **même binaire**, seule
l'option `--tile-threads` change (témoin de mesure, au même titre que
`--no-tile-cache`) :

| | `--tile-threads=1` (témoin) | 4 threads | écart |
|---|---:|---:|---:|
| Travail géométrie / tuile | 1278 / 1294 µs | 1542 / 1457 µs | **+16 %** |
| Temps de MUR du maillage | 7255 / 7341 ms | 3620 / 3322 ms | **−52 %** |
| Facteur de parallélisme | 1,00 | **2,42 / 2,49** | — |
| Images en 45 s | 11 046 / 11 253 | 13 320 / 12 081 | **+14 %** |
| Tuiles non couvertes | **0** | **0** | — |

Deux chiffres sont publiés, et il le faut : le **travail** par tuile ne doit
*pas* baisser — c'est la même fonction qui fait le même calcul — et il monte même
de 16 %, parce que quatre threads se disputent la bande passante mémoire en
parcourant une heightmap de 5001x5001. Ce qui doit baisser, c'est le temps que le
thread de rendu passe **bloqué** dessus. Publier un seul des deux laisserait
croire soit qu'il n'y a pas de gain, soit que le travail a diminué.

### 1.4 En Release, le gain disparaît — et c'est le résultat de la phase

Même survol, même méthode, **build Release** :

| | `--tile-threads=1` (témoin) | 4 threads |
|---|---:|---:|
| Travail géométrie / tuile | 141 / 139 µs | 154 / 146 µs |
| Temps de MUR du maillage | 819 / 811 ms | 1227 / 870 ms |
| Facteur de parallélisme | 1,00 | **0,73 / 0,97** |
| Images en 45 s | **32 206 / 32 327** | 29 776 / 32 104 |

**Le facteur est inférieur à 1 : paralléliser coûte plus que ça ne rapporte.**
La création des threads (~3 900 sur le survol, en rafales de 3) dépasse le
travail qu'elle répartit, parce qu'une tuile Release ne dure que 140 µs.

Une variante a été essayée avant de conclure — ne paralléliser qu'à partir de
**16 tuiles** par lot, pour ne viser que les grosses rafales. Résultat : 3 lots
parallélisés en 45 s, facteur 1,01, aucun effet mesurable. L'option devient
inerte, elle ne devient pas gagnante.

### 1.5 Pourquoi les deux mesures divergent : le coût d'une tuile, Debug vs Release

| Poste d'une tuile | Debug | Release |
|---|---:|---:|
| Géométrie | 1 344 µs (**79 %**) | 140 µs (**33 %**) |
| Indices | 16 µs (1 %) | 15 µs (4 %) |
| Téléversement GPU | 250 µs (15 %) | 262 µs (**63 %**) |

La géométrie est **9,6 fois moins chère** une fois optimisée ; le téléversement,
lui, ne bouge pas (c'est du memcpy et du pilote, pas du code compilé par nous).
Le poste dominant d'une tuile en Release n'est donc **pas** la géométrie, c'est
le **téléversement** — celui-là même que la Phase 12 avait traité et que la
Phase 18 avait rangé au rang de résidu à 11 %.

La Phase 18 n'a rien mesuré de faux : ses chiffres sont exacts, en Debug. Elle en
a tiré une priorité qui ne vaut que là. Et comme toutes les mesures du projet
sont en Debug (c'est la seule configuration avec les couches de validation), rien
ne pouvait le signaler.

### 1.6 Décision : défaut séquentiel, chemin parallèle conservé

`Params::meshThreads` vaut **1** par défaut — le réglage que la mesure désigne
en Release. Le chemin parallèle reste disponible par `--tile-threads=N`, testé
et documenté, pour trois raisons :

1. il est un **gain net de 2,4x et +14 % d'images en Debug**, c'est-à-dire dans
   la configuration où tournent toutes les campagnes de mesure et de validation
   du projet — ça a une valeur d'outillage réelle ;
2. il redeviendra intéressant en Release le jour où une tuile coûtera plus cher
   (format de sommet plus lourd, pas LiDAR plus fin) ;
3. il sert de **témoin** : sans lui, l'écart Debug/Release ci-dessus ne serait
   pas reproductible sur le même binaire.

Un troisième argument, mesuré incidemment, va dans le même sens. Sur la vue de
référence en Debug, `--tile-threads=4` donne une médiane identique (312 contre
311 FPS) mais un **minimum de 89 FPS contre 126** : les créations de threads
introduisent des à-coups que la médiane cache. Le défaut séquentiel est aussi le
plus régulier.

### 1.7 Un coût mémoire introduit puis retiré

Première version : tout passait par le lot, donc jusqu'à 53 `MeshData`
simultanés (~287 Ko chacun, ~15 Mo). Mesuré : **pic mémoire du processus
+5,6 Mo** sur la vue de référence, 338,3 Mo contre 332,7 en Phase 18.

Le chemin séquentiel (le défaut) maille et téléverse donc **une tuile à la
fois**, sans jamais garder plus d'un `MeshData` en vie, et le lot est relâché
(`shrink_to_fit`) à la fin de la frame sur le chemin parallèle. Pic revenu à
**331,7 Mo**, soit 1 Mo sous la Phase 18.

C'est le genre de régression qu'on n'aurait pas vue en ne regardant que le FPS.

### 1.8 Comment la correction a été vérifiée

**La comparaison d'images ne peut pas trancher ici, et c'est une conclusion en
soi.** Deux captures du jeu diffèrent déjà entre deux exécutions **identiques** :
l'herbe et la faune s'animent avec le temps écoulé, et le temps écoulé dépend du
débit d'images — que la phase change justement. Mesuré :

| Comparaison | Octets différents |
|---|---:|
| Témoin : parallèle vs parallèle, mêmes options | 0,65 % / 4,40 % |
| Témoin : séquentiel vs séquentiel, sans faune | 1,54 % |
| Séquentiel vs parallèle | 0,16 % / 6,67 % |

Le plancher de bruit est du même ordre que le signal cherché. Sur la vue de
référence, l'écart séquentiel/parallèle (0,16 %) est même **plus petit** que
l'écart entre deux runs parallèles (0,65 %). Ce test ne prouve rien — ni dans un
sens ni dans l'autre.

Le contrôle qui tranche est **headless et déterministe** (nouveau dans
`tests\logic_tests.cpp`) : 40 tuiles de niveaux mélangés sont maillées en
séquence puis sur 4 threads, et les sommets sont comparés **par `memcmp`**, pas à
une tolérance près — une course ne produirait pas un petit écart numérique, elle
produirait un sommet pris à la mauvaise tuile. Le test comprend son propre
témoin (vérifier que les tuiles comparées sont bien distinctes entre elles, sans
quoi la comparaison passerait sans rien prouver) et **12 passages
supplémentaires**, parce qu'une course se manifeste rarement et qu'un passage
unique ne dit presque rien.

### 1.9 Validation de synchronisation, avec son contrôle négatif

`VK_LAYER_VALIDATE_SYNC=1` sur le survol, chemin parallèle **et** chemin
séquentiel : **0 message**.

Un journal propre ne prouve rien tant qu'on n'a pas montré que l'outil regarde au
bon endroit (MEMORY.md, leçon de la Phase 12). **Contrôle négatif** : la barrière
`TRANSFER_WRITE -> VERTEX_ATTRIBUTE_READ | INDEX_READ` de
`Renderer::recordPendingCopies` a été retirée, le projet rebuildé, et la couche a
immédiatement crié :

```
vkCmdDrawIndexed(): READ_AFTER_WRITE hazard detected. vkCmdDrawIndexed reads
index VkBuffer 0xaf00000000af, which was previously written by vkCmdCopyBuffer.
```

10 messages, plafond de déduplication atteint. La barrière a été remise et le
journal est redevenu vide. L'outil regarde bien le chemin de téléversement des
tuiles.

---

## 2. TÂCHE 2 — où passe vraiment le temps d'une image

### 2.1 L'instrumentation

`src/main.cpp` chronomètre désormais cinq postes CPU par image, journalisés à la
fermeture (`Cout par image`) : streaming terrain, `gatherTrees`, `gatherGrass`,
construction du tronc de vision, et `drawFrame`. Même démarche que
`buildProfile` en Phase 18, appliquée à l'image entière.

Le chronomètre est pris **sans condition** : un profilage qu'on active à la
demande finit par mesurer un binaire différent de celui qui tourne. Coût mesuré :
« tronc de vision » vaut 0,3 à 0,9 µs en Release, ce qui borne le coût des
bornes elles-mêmes.

### 2.2 Ce que le Debug faisait croire

Vue d'alpage, Debug :

| Poste | µs/image |
|---|---:|
| **`gatherTrees`** | **1 747** |
| `gatherGrass` | 4 944 |
| `drawFrame` | 1 373 |
| Streaming terrain | 97 |
| Tronc de vision | 5 |

`gatherTrees` coûtait 1 630 à 1 936 µs **dans les trois scénarios testés**, y
compris le survol à 120 m d'altitude où les arbres sont à peine visibles. Coût
strictement constant, indépendant de ce qui est à l'écran : la signature d'un
coût de **balayage**, pas d'un coût de contenu. Deux redondances par image
étaient identifiées dans le code et prêtes à être corrigées :

- 5 appels à `terrain_.heightAt()` par **bloc** de 8x8 cellules pour estimer ses
  bornes verticales, à chaque image, alors que ces bornes ne changent jamais
  (~720 appels par image) ;
- la purge du cache parcourt les **~1 800 cellules** du cache à chaque image
  pour y chercher des cellules à oublier, alors que la caméra bouge de quelques
  centimètres par image.

### 2.3 Ce que le Release dit, et qui annule tout ce qui précède

Mêmes vues, build Release :

| Poste | Alpage | Vue de référence |
|---|---:|---:|
| **`drawFrame`** | **3 537 µs (81 %)** | **1 424 µs (83 %)** |
| `gatherGrass` | 515 µs | 128 µs |
| `gatherTrees` | 254 µs | 153 µs |
| Streaming terrain | 22 µs | 15 µs |
| Tronc de vision | 0,9 µs | 0,6 µs |

`gatherTrees` passe de 1 747 à 254 µs. La somme des postes (4 328 µs) rend
compte de la quasi-totalité de l'image mesurée (4,39 ms), donc rien d'important
n'échappe au profil.

### 2.4 Le frame est limité par le GPU — vérifié par ablation, pas affirmé

`drawFrame` inclut l'attente de la barrière de la frame en vol. Pour distinguer
« le CPU travaille » de « le CPU attend », on retire du travail **GPU** et on
regarde si les postes CPU bougent. Alpage, Release, 20 s :

| | Images | `drawFrame` | Arbres CPU | Herbe CPU | Temps/image |
|---|---:|---:|---:|---:|---:|
| Tout | 4 561 | 3 537 µs | 254 | 515 | 4,39 ms |
| `--no-grass` | 7 184 | 2 537 µs | 185 | 0 | 2,78 ms |
| `--no-vegetation` | 10 631 | 1 830 µs | 0 | 0 | 1,88 ms |

Retirer l'herbe économise 515 µs de CPU mais fait tomber le temps d'image de
1 610 µs : **le gain vient du GPU, pas du CPU.** Le CPU passe 81 % de l'image à
attendre dans `drawFrame`.

**Conséquence directe : aucune optimisation CPU ne vaut la peine d'être faite
ici.** Supprimer entièrement `gatherTrees` et `gatherGrass` — 769 µs sur 4 390 —
ne rendrait rien, puisque ce temps est déjà pris sur une attente. Les deux
redondances du §2.2 sont **réelles** et restent notées pour le jour où le
rapport de forces changera (GPU plus rapide, ou résolution plus basse) ; les
corriger aujourd'hui serait optimiser un artefact du build de test.

Conformément à la consigne — « n'implémente que ce qui est mesuré comme un vrai
gain » — **rien n'a été optimisé dans la Tâche 2.** Ce qu'elle livre est le
profileur permanent et la mesure qui réoriente le travail futur.

### 2.5 Une redondance réelle mais négligeable, signalée et non corrigée

`renderer.projectionMatrix() * camera.viewMatrix()` puis
`Frustum::fromViewProjection` sont calculés **deux fois par image** — une fois
pour le streaming, une fois pour la végétation. C'est bien une redondance, et
elle est maintenant mesurée : **0,3 à 0,9 µs par image en Release**. La
supprimer n'est pas une optimisation, c'est du ménage ; elle n'a pas été faite
dans cette phase pour ne pas mélanger un changement non mesurable avec des
changements mesurés.

---

## 3. Résumé des chiffres

### Vue de référence Phase 11 — E 901500 N 6436600, yaw 100, pitch -7 (Debug)

| État | FPS médian | Pic RAM |
|---|---:|---:|
| Phase 18 | 293 | 332,7 Mo |
| **Après la Phase 19** | **311** | **331,7 Mo** |

### Vue d'alpage — E 900500 N 6439000, yaw 100, pitch -7 (Debug)

| État | FPS médian | Pic RAM |
|---|---:|---:|
| Phase 18 | 122 | 340,2 Mo |
| **Après la Phase 19** | **131** | **339,1 Mo** |

Ces écarts sont **dans le bruit** de la mesure de FPS (~15 % entre deux runs
identiques, cf. MEMORY.md) : la phase ne prétend pas à un gain de FPS sur ces
vues. Ce qui est solide, c'est le **pic mémoire**, qui est plus stable et qui est
descendu de 1 Mo sur les deux vues.

### Coût d'une tuile — survol 900 m/s

| Poste | Debug | Release |
|---|---:|---:|
| Géométrie | 1 344 µs (79 %) | 140 µs (33 %) |
| Indices | 16 µs | 15 µs |
| Téléversement | 250 µs (15 %) | 262 µs (**63 %**) |

0 tuile non couverte sur toutes les mesures, Debug comme Release, séquentiel
comme parallèle.

---

## 4. Vérification

- **Tests headless : 902 assertions** (896 avant la phase, **+6**), toutes
  passent. Nouvelle section « Phase 19 : maillage de tuiles en parallèle »
  (§1.8).
- **Zéro message `[Validation]`** sur la totalité des mesures et des scénarios,
  Debug et Release.
- **`VK_LAYER_VALIDATE_SYNC=1`** sur les deux chemins : 0 message, **avec son
  contrôle négatif** (§1.9).
- **Harnais `run_vulkan_check.ps1`** : voir §4.1.

### 4.1 Résultat du harnais

Joué **deux fois en entier** (11 processus, scénarios A à I) : une fois après la
Tâche 1, une fois sur le binaire final.

- **0 message `[Validation]`, 0 ligne `[ERROR]`**, code de sortie 0 pour chacun
  des onze processus, fermeture propre partout ;
- gameplay effectivement exercé : 2 arbres abattus (3 au sol), 3 débitages,
  4 objets ramassés, percussion, vol de parapente complet, escalade avec
  sommet atteint (+41,5 m, pente 65 à 155 %), éditeur (3 objets posés, un
  supprimé, une annulation, sauvés/rechargés, **et rechargés en partie normale**
  où ils arrêtent le joueur : 0,28 m parcourus contre 21,05 m pour le témoin
  sans scène), boisson au point d'eau avec son contrôle négatif à 30 m, faune
  qui se retire du voisinage (4 animaux à portée au premier relevé → 0 au
  dernier).

Les 4 lignes `[WARN]` sont les avertissements bénins habituels (pas de point
d'eau ni de faune sur certaines dalles) ; le harnais ne les compte pas comme des
erreurs.

### Le point rouge, inchangé depuis la Phase 17

Le harnais sort en `[ECHEC]` sur **un seul critère** : scénario E (vélo),
« pied à terre » (`False`). C'est un échec **antérieur à cette phase**, documenté
dans NOTES-PHASE17.md puis dans NOTES-PHASE18.md §4, laissé non corrigé pour
cause de périmètre. La Phase 19 ne l'a ni causé ni aggravé — elle ne touche que
le maillage des tuiles de terrain, et le reste du scénario E est vert (en selle
`True`, 13 relevés à l'arrêt en côte au-delà de 23,6 % de pente).

Le scénario I, bruité en Phase 18, est **vert** cette fois (4 → 0).

---

## 5. Options et outils ajoutés

| Ajout | Rôle |
|---|---|
| `--tile-threads=N` | Degré de parallélisme du maillage CPU des tuiles, thread de rendu compris. Défaut **1** (séquentiel). **Témoin de mesure** : aucun appel Vulkan ne change de thread quelle que soit la valeur. |
| `tools\profile_tiles.ps1` | Relève le profil de construction des tuiles et le coût par image sur un scénario donné. La Phase 18 relevait ces lignes à la main ; ce script rend le relevé reproductible — ce que `measure_terrain.ps1` avait fait pour le FPS en Phase 9. |
| Journal `Cout par image` | Cinq postes CPU du frame, à la fermeture. Permanent, non conditionnel. |
| Journal `Maillage parallele` | Temps de mur **et** travail cumulé du maillage, dans `buildProfile`. Les deux, jamais un seul (§1.3). |

---

## 6. Prochaines étapes suggérées

1. **Refaire les mesures de performance des phases précédentes en Release.**
   C'est la conséquence la plus large de cette phase : les priorités
   d'optimisation du projet sont établies sur des profils de Debug, et on vient
   de voir qu'au moins l'une d'elles ne survit pas au passage en Release. Les
   conclusions des Phases 10, 11, 12 et 18 méritent une relecture sous cet
   angle.
2. **Le vrai poste d'une tuile en Release est le TÉLÉVERSEMENT (63 %).** La
   piste chiffrée existe déjà et n'a pas bougé : **partager un unique buffer
   d'indices GPU** entre toutes les tuiles (Phase 18 §2.5) supprimerait 96 Ko de
   copie par tuile et ~48 Mo de mémoire GPU. C'est désormais le chantier de
   performance le mieux ciblé.
3. **Le frame est limité par le GPU, et le premier consommateur est la
   végétation** : sur l'alpage, l'herbe pèse 1,0 ms et les arbres 0,7 ms des
   3,5 ms de `drawFrame`. Le levier est le **nombre d'instances**, pas le
   shader (la Phase 18 avait déjà montré que la découpe alpha coûte zéro). Un
   LOD sur l'herbe — moins de touffes au-delà de 20 m — serait le gain le plus
   important disponible aujourd'hui, mais il **touche la qualité visuelle** :
   c'est un compromis à valider, pas une optimisation à décider seul.
4. **Les deux redondances de `gatherTrees`** (§2.2), si le rapport CPU/GPU
   change un jour.
5. Rendre les scénarios E et I du harnais insensibles au FPS (hérité de la
   Phase 18, toujours pas fait).
