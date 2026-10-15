# Phase 29 - Rochers, pierriers, buissons

Conception et donnees : `NOTES-PHASE29-DESIGN.md` (Agent 1).

# Agent 2/4 - Rochers et pierriers proceduraux (2026-09-27)

Session sans supervision. Sauvegarde des sources avant la phase :
`..\_Vercora_backups\avant-phase29-20260927-0513`.

## Fichiers

| fichier | role |
|---|---|
| `src/world/RockMesh.{hpp,cpp}` (nouveaux) | 3 meshes generes en code, echelle unite : `boulderMeshData` (gros bloc, 128 tri), `rockMeshData` (rocher moyen, 32 tri), `gravelMeshData` (plaque de 11 cailloux, 88 tri). Sphere subdivisee bosselee puis tronquee par des plans aleatoires (faces de fracture). En partie enterres sous l'origine (0,13-0,25 x echelle). |
| `src/world/Rocks.{hpp,cpp}` (nouveaux) | semis deterministe + LOD + collision (`CollisionSolver`) |
| `shaders/foliage.vert` | mode ROCHER quand `params.w < 0` : inclinaison selon la pente. Arbres/herbe/fibres (w >= 0) : chemin inchange. |
| `src/main.cpp` | creation apres les plantes a fibres, 3 meshes, 3 appels instancies, ajout au `worldSolver`, options `--no-rocks` (temoin) et `--no-rock-collision`, lignes de journal `Rochers (PHASE 29)`. La ligne `Cout par image (PHASE 19)` n'est PAS modifiee (les scripts la parsent). |
| `CMakeLists.txt`, `tools/run_logic_tests.bat` | ajout des 2 .cpp |
| `tests/logic_tests.cpp` | `testRockMeshes`, `testRockPlacement` (+53 verifications) |
| `tools/profile_phase29.ps1` (nouveau) | A/B entrelace `--no-rocks`, arret par PID exact, RAM |

Donnees : AUCUNE modifiee ni copiee. Le masque n'a besoin que du DTM et de
`vercora_zone_canopy.vchm`, deja charges par le jeu (`coverMap`). Les cartes
`lowveg` de l'Agent 1 ne servent qu'aux buissons (Agent 3).

## Critere (strictement celui de la conception)

- `kWalkComfortSlope (0,60) <= Climber::slopeAt < kWalkMaxSlope (0,84)` - appel
  LITTERAL a `Climber::slopeAt` (base 4 m), jamais `Vegetation::slopeAt`.
- couvert LiDAR (`coverMap`, bilineaire) `< 0,10`.
- rugosite (definition exacte de `probe_rocks.py` : RMS 5x5 du residu DTM -
  moyenne 5x5, 81 sondes) `>= 0,10 m` -> gros blocs + rochers ; `< 0,10` ->
  pierraille. Blocs : rugosite au point ; pierraille : au centre de la cellule de 8 m.
- **Masque = filtre dur** ; le bruit ne fait que moduler la densite dedans.
- **Sans carte de couvert : zero rocher** (pas de repli pente seule).
- Bati/eau : NON applique (pas de carte en jeu ; effet mesure par l'Agent 1 :
  2 272 m2 sur 25 km2). Chemins/activites : il n'existe AUCUN masque de
  chemin dans le projet ; les parois d'escalade (>= 0,84) sont exclues par
  construction. Rien d'autre a exclure.
- Mesure en jeu (ligne de journal) : **masque 8,4 % de la zone** (conception :
  8,9 % sur DTM 1 m ; ecart = echantillonnage 1/9 des cellules + couvert bilineaire).

## Parametres (`RockParams`)

| | cellule | emplacements | taille (echelle, m) | portee far -> near |
|---|---|---|---|---|
| gros blocs | 32 m | 8 (1 / 128 m2 max) | 1,1-2,6 | 700 -> 350 m |
| rochers moyens | 32 m | 20 (1 / 51 m2 max) | 0,35-0,95 | 320 -> 140 m |
| pierraille (plaques) | 8 m | 12 (3 / 16 m2 max) | 0,7-1,5 | 70 -> 30 m |

- Taille tiree en loi carree (petits sujets majoritaires). Graine `20260927`.
- **Trainees** : bruit de valeur 2 octaves, anisotrope : 1/110 m le long de la
  pente, 1/28 m en travers ; densite = rampe(bruit, 0,35, 0,65). Le repere de
  pente est fixe **par tuile de 96 m** (direction lue au centre, base 32 m),
  4 tuiles melangees. Premiere version (repere au point) REJETEE par le test
  d'anisotropie : loin de l'origine, quelques degres d'ecart de direction
  deplacaient le bruit de plusieurs periodes -> bruit blanc (mesure : variation
  le long 64,7 vs en travers 23,0, soit l'inverse du but ; apres : 5,1 vs 20,3).
- Estimation zone (journal) : 1,85 gros blocs, 4,75 rochers, 46 plaques / ha de zone.
- LOD de densite : portee PAR EMPLACEMENT (comme les arbres) + retrecissement
  sur les 18 derniers %. Semis par cellules, cache PAR BLOC de 8x8 cellules,
  budget 3 blocs (4 pour la pierraille) par image, purge a 1,5 x portee.
- Inclinaison : `InstanceData` est plein (28 o) ; le gradient (x 0,85) est
  quantifie 64x64 dans la partie ENTIERE de `variation`, la teinte dans sa
  partie fractionnaire (`packRockVariation`). Rodrigues dans le shader.

## Collision : decision

**Gros blocs d'echelle >= 1,3 m seulement** bloquent (cylindre de rayon
0,425 x echelle), via le `CompositeCollisionSolver` existant, comme Forest et
la faune. Rochers moyens et pierraille : traversables (sinon un pierrier
deviendrait un labyrinthe impraticable a pied et a velo). Un corps dont les
pieds sont au-dessus du sommet - 0,40 m (parapente, joueur sur le bloc) passe.
Juge sur et simple : meme repoussement que `Forest::resolve`, aucun bloc sur
paroi (>= 0,84) par construction. Temoin : `--no-rock-collision`.
Verifie : scenarios Vulkan **E (velo) et F (escalade)** joues seuls
(`-Only 'E','F'` via `-Command` - via `-File` la liste arrive en une seule chaine
et RIEN n'est joue) : code 0, 0 `[Validation]`, fermeture propre. Leurs
controles detailles dependent du scenario C (non joue) : suite complete = Agent 4.

## Tests headless

`tools\run_logic_tests.bat` : **3930/3930** (3877 + 53). Couvre : meshes valides
et bas-poly, chaque critere du masque isole (plat, 0,50, 0,95, couvert 0,30,
bornes), sans carte -> vide, **0 rocher hors masque sur 10 819 semes** (relief a
bande de pente + couvert coupe en deux), pose au sol, blocs seulement sur
rugueux / pierraille seulement sur lisse, determinisme au bit, graine,
densite bornee, cellules vides ET pleines (trainees), anisotropie, LOD (portees,
eclaircissement avec la distance), pack/unpack, collision (detectee, resolue,
temoin, survol, composite, rien hors masque).

## Performance (Debug + validation, `tools\profile_phase29.ps1`, 3 tours A/B entrelaces, 20 s)

Charge verifiee avant (0-43 %) ; runs a > 1 coeur d'autres processus ecartes.

| vue | A (rochers) | B (`--no-rocks`) | A-B | RAM WS / pic |
|---|---|---|---|---|
| pierrier (903452, 6439601) | 334,8 img/s, 2 974 us | 346,6 img/s, 2 885 us | **+89 us/img (+3,1 %)** | 296/339 vs 295/338 Mo |
| alpage ref. Phase 27 (900500, 6439000) | 150,3 img/s, 6 653 us | 152,3 img/s, 6 568 us | **+85 us/img (+1,3 %)** | 291/358 vs 291/358 Mo |

Bruit : ecart min-max intra-condition 1-2 img/s. Semis rochers ~62-72 us/image.
Historique de l'optimisation (mesures) : cellule 16 m / 900 m -> **1 625 us** de
semis ; cellule 32 m / 700 m -> 402 us ; cache par bloc -> **~70 us**.
0 `[Validation]` sur les 12 runs.

## Limites

- Premier passage dans une zone : un bloc de 8x8 cellules est seme d'un coup
  (jusqu'a ~1 800 tests de masque + rugosite) ; borne a 3+4 blocs par image,
  mais a-coup possible, non mesure isolement.
- Pierraille : rugosite au centre de la cellule de 8 m (au point pour les blocs).
- Pas de bati/eau en jeu ; pas de rochers au-dessus de 0,84 (pieds de parois),
  par consigne. Pierrier vs pelouse raide rase non separables (limite Agent 1).
- Le terrain lointain lu en 2 m peut lisser la pente : non concerne, le semis lit
  le `heightmap` CPU 1 m.
- Collision : les blocs empiles/inclines sont des cylindres verticaux ; on ne
  peut pas monter sur un bloc (pas de hauteur de sol modifiee).
- Rendu : les rochers suivent la touche G (vegetation) ; pas d'ombre portee.


# Agent 3/4 - Buissons et petits arbres (classe 3 LiDAR) (2026-09-27)

Session sans supervision ; toutes les decisions ci-dessous sont prises et justifiees ici.

## Fichiers

| fichier | role |
|---|---|
| `assets/terrain/vercora_zone_canopy_lowveg.vchm` (COPIE) | = `data/phase29/vercora_zone_lowveg.vchm` (F3), SHA-256 identique |
| `assets/terrain/vercora_zone_canopy_lowveg_height.vchm` (COPIE) | = `data/phase29/vercora_zone_lowveg_height.vchm` (h max c3 - sol) |
| `src/world/BushMesh.{hpp,cpp}` (nouveaux) | `shrubMeshData` (buisson 3 lobes, 106 tri), `smallTreeMeshData` (tronc + 2 cones, 44 tri) ; echelle unite = 1 m de HAUT, pied enterre de 0,08 |
| `src/world/Bushes.{hpp,cpp}` (nouveaux) | critere, semis deterministe, LOD, emprises d'arbres |
| `src/main.cpp` | chargement des 2 cartes, `--no-bushes` (temoin), 2 appels instancies, lignes `Buissons (PHASE 29)` |
| `CMakeLists.txt`, `tools/run_logic_tests.bat` | ajout des 2 .cpp |
| `tests/logic_tests.cpp` | `testBushMeshes`, `testBushPlacement` (+38) |
| `tools/profile_phase29.ps1` | + vue `buissons` (901160, 6436275) et `-Toggle` (option temoin de B, defaut `--no-rocks` : usage Agent 2 inchange) ; colonnes buissons |
| `shaders/foliage.vert` | **NON modifie** : les buissons prennent le chemin ARBRE (params.w = 0) |

**Donnees : schema du projet respecte.** Les cartes de terrain vivent dans
`assets\terrain\` et se trouvent par `canopyPathFor(terrain, suffixe)` ; les copies
portent donc les noms `_canopy_lowveg` / `_canopy_lowveg_height` (proposition de
l'Agent 1). Sources de `data\phase29\` non modifiees. `.gitignore` ignore deja
`assets/terrain/*.vchm`. Verifie avant : meme en-tete (emprise, pas 5 m) que le
couvert ; couvert >= F3 sauf 6 cellules sur 1 M (ecart < 0,03).

## Critere (celui de la conception, section 2)

`F3 >= 0,10  ET  F45 = couvert - F3 < 0,20`, plus `couvert >= 0,10`.
- Pilote **uniquement** par F3 : la densite, la hauteur et le choix du mesh viennent
  de la classe 3. Le couvert total ne sert qu'a EXCLURE (foret, et domaine rocher).
- `couvert >= 0,10` : le masque des Rochers exige couvert < 0,10, donc les deux semis
  sont **disjoints par construction** (et les 6 cellules F3 > couvert sont ecartees).
  Plus simple et plus sur que d'interroger Rocks.
- Arbres abattables (Forest) : aucun buisson dans le rayon de HOUPPIER
  (`Tree::crownRadius`), emprises passees une fois au demarrage (grille de 32 m).
  Arbres de decor (Vegetation) : NON verifies (semis prive, pas d'API) ; F45 < 0,20
  les evite en grande partie (leur seuil : couvert >= 0,20). Limite assumee.
- Une des trois cartes absente -> ZERO buisson (pas de substitut). Terrain synthetique
  et dalles `LHD_*` : aucun buisson.

## Parametres (`BushParams`)

- Cellule 8 m, 6 emplacements (<= 940 sujets/ha). Densite = 0,20 + 0,80 x rampe(F3,
  0,10 -> 0,30) ; l'emplacement k n'est retenu que si k < densite x 6 (regle Rocks/Vegetation).
- Hauteur = clamp(h_max_c3 x U[0,35 ; 0,85], 0,5, 3,0) m. Moyenne du tirage 0,60 =
  rapport mesure par l'Agent 1 entre hauteur moyenne (p50 1,5 m) et max (p50 2,5 m).
  h_max < 0,3 m -> pas de sujet (pas de hauteur inventee). Ecretage 3 m : conception.
- Mesh par la HAUTEUR : >= 1,8 m -> petit arbre (jeune resineux), sinon buisson.
  Dans le masque, h_max p50 = 3,35 m : les petits arbres dominent (mesure en jeu :
  1 523 petits arbres pour 86 buissons visibles au coeur de la zone test). C'est la donnee.
- Portee par emplacement : buissons 150 -> 55 m, petits arbres 260 -> 100 m, fondu par
  retrecissement sur les 18 derniers % (comme Rocks). Cache par bloc 8x8 cellules,
  budget 4 blocs/image, purge a 1,5 x portee. Premier reglage 220/380 m : 276 us de
  semis/image en Debug dans la zone dense -> ramene.
- Graine 20260928. Ondulation 4 cm (buisson), 0 (petit arbre) ; ni decoupe ni mode rocher.
- Journal en jeu : masque 9,7 % de la zone (donnee : 10,45 % ; echantillonnage 1/7 +
  bilineaire), 21,7 buissons et 31,0 petits arbres par ha de zone, 16 emprises exclues.

## Adret / ubac : NON applique (decision)

Examine : buis (Buxus sempervirens) sur adrets calcaires chauds ; rhododendron
ferrugineux et aulne vert en ubac. Rejete pour ce jeu de donnees :
- le buis est une espece de l'etage collineen/montagnard inferieur ; la zone est a
  1029-2285 m et le masque classe 3 est surtout en altitude (landes et pins a crochets
  des Hauts-Plateaux) : l'appliquer par l'exposition seule le placerait hors de son etage ;
- le rhododendron ferrugineux est lie a un humus ACIDE (sur calcaire, seulement sous
  les pins a crochets sur humus epais) : facteur edaphique, absent de nos donnees ;
- l'aulne vert est silicicole, marginal dans le Vercors calcaire.
Sources (connaissances generales, NON reverifiees en ligne pendant cette session) :
Ozenda, *La vegetation de la chaine alpine*, Masson 1985 ; Rameau et al., *Flore
forestiere francaise* t.1 (plaines et collines) et t.2 (montagnes), IDF 1989/1993.
Le LiDAR ne donne pas l'espece (conception, section 2). Donc : ni teinte ni espece
selon l'exposition ; la variation de teinte est l'alea par instance de foliage.vert.

## Collision : AUCUNE (decision)

Buissons et petits arbres se traversent : ce sont des ligneux bas et souples (landes,
fourres), qui ralentiraient plus qu'ils n'arretent ; les rendre bloquants ferait de
10 % de la zone un labyrinthe pour la marche, le velo et la faune, et Wildlife/activites
n'ont pas ete concus avec. Les petits arbres (<= 3 m) restent traversables pour la meme
raison. Le `worldSolver` n'est pas touche -> aucun effet sur la faune, l'artisanat, les
activites (Forest, Rocks, Vegetation : code non modifie).

## Tests headless

`tools\run_logic_tests.bat` : **3968/3968** (3930 + 38). Meshes (valides, 16-400 tri,
hauteur 1, pied enterre <= 0,08, emprise dans le rayon annonce) ; chaque critere isole ;
sans carte -> vide ; h = 0 -> vide ; **0 buisson hors critere sur 25 211** (champ en
rampe F3 x F45) et frontieres reproduites ; <= 6/cellule ; densite pleine a F3 0,30,
clairsemee a 0,11 ; hauteur dans [0,5 ; 3] sous 10 m et <= h carte sous 1,2 m ;
petit arbre ssi h >= 1,8 ; determinisme au bit, graine ; disjoint du masque Rocks
sur pierrier ; emprise d'arbre respectee ; gather (portee, fondu, echelle, LOD).
PIEGE rencontre : `C1041/C1090` sur `build-tests\logic_tests.pdb` quand build.bat et
les tests s'enchainent vite (mspdbsrv tient le PDB) ; supprimer le .pdb et relancer.

## Build et cout (Debug + validation, `profile_phase29.ps1 -Toggle --no-bushes`, 3 tours A/B entrelaces, 20 s)

Build Debug : 0 erreur, 0 avertissement. Charge avant : 0-2 % ; aucun autre vercora ni
build ; arret par PID. **0 `[Validation]` sur 12 runs** + fumee + capture.

| vue | A (buissons) | B (`--no-bushes`) | A-B | semis | RAM WS / pic |
|---|---|---|---|---|---|
| alpage ref. (900500, 6439000) | 148,3 img/s, 6 744 us | 151,3 img/s, 6 609 us | **+135 us (+2,0 %)** | 87 us, 41 + 92 | 299/367 vs 291/359 Mo |
| buissons (901160, 6436275, 90 % de masque a 100 m) | 151,8 img/s, 6 586 us | 159,6 img/s, 6 264 us | **+322 us (+5,1 %)** | 192 us, 86 + 1 523 | 299/368 vs 291/360 Mo |

RAM +8 Mo = les deux cartes 1001 x 1001 float. Capture : `build\phase29\buissons.png`.
`run_vulkan_check.ps1` NON lance (Agent 4).

## Limites

- Arbres de decor Vegetation non exclus (voir plus haut) ; recouvrement possible en lisiere.
- Dalle `0902_6438` peu classee : pas de buissons, par la donnee.
- Especes non identifiees (LiDAR) ; hauteur tiree, pas mesuree par sujet.
- Le cout Debug en zone dense (+0,32 ms) est surtout le parcours des instances ; pistes :
  portee plus courte, ou tri des blocs par distance.


# Verification (Agent 4/4) (2026-09-27)

Session sans supervision. Avant chaque etape : aucun vercora/cmake/ninja/cl/msbuild/
cargo/rustc, charge CPU 3-26 %. Une seule chose a la fois. Arrets par PID (scripts).
`data\` non touche. Journaux et CSV : `build\phase29\agent4_*`.

## 1. Build Debug propre - VERT
`cmake --build build --clean-first` : 86/86, **0 avertissement**. Tests reconstruits
de zero (obj + pdb supprimes) : seul avertissement = `main.cpp(1297) C4244`, la MEME
ligne que `main.cpp(1280)` de la sauvegarde (Phase 27, decalee de 17 lignes par les
ajouts). Aucun avertissement dans Rocks/RockMesh/Bushes/BushMesh.

## 2. Tests headless - VERT
`tools\run_logic_tests.bat` : **SUCCES : 3968 verifications passees** (3968 attendues).

## 3. run_vulkan_check.ps1, suite complete - 0 `[Validation]`, seul C echoue (connu)
0 `[Validation]`, 0 `[ERROR]`, 4 `[WARN]` habituels, codes de sortie 0, fermetures propres.

| scenario | verdict |
|---|---|
| A gameplay synthetique | passe (2 abattus, 4 ramasses, percussion) |
| B chemin de collision | passe |
| C dalle LiDAR reelle | **ECHEC** : arbre "MANQUE", nav arrivee a 0,81 / 0,83 m |
| D parapente | passe |
| E velo | passe (en selle / pied a terre, 50 km/h) |
| F escalade | passe (sommet, 41,8 m) |
| G1-G3 editeur | passe (0,28 m avec scene contre 9,71 m temoin) |
| H besoins | passe |
| I faune | passe (24 animaux, fuite, frequentation 100 %) |
| J / J3 recolte, fibres zone | passe (repousse) |

Scenario C : Phase 28 = 0,82/0,84 m en suite complete ; ici 0,81/0,83 -> **pas pire**.
`-Only 'C'` (via `-Command`) x3 : echec (0,85/0,68 m), **passe** (0,73/0,90), **passe**
(0,75/0,90), 0 `[Validation]`. **La Phase 29 ne peut pas y jouer** : sur la dalle
`LHD_0907_6440`, le journal dit "aucune carte de couvert LiDAR, donc AUCUN rocher" et
"carte classe 3 ou couvert absent ... AUCUN buisson" (13 et 11 processus) ; `Rocks`
n'est ajoute au `worldSolver` que s'il est actif. A a I tournent sur des terrains sans
couvert : rochers et buissons n'y existent pas. Seul J3 (zone 25 km2) a rochers et
buissons actifs, et il passe. Rien corrige (rien a corriger).

## 4. Captures avant/apres - `build\phase29\captures\`
Meme binaire Debug, j12 13 h, temps gele, entree verrouillee ; avant = `--no-rocks --no-bushes`.

| paire | vue | pixels changes (seuil 30) |
|---|---|---|
| `belvedere_*` (+ `_zoom`) | E900000 N6436000 `--look=270,-24` (Phase 25) | 1,3 % |
| `pierrier_*` (+ `_apres_zoom`) | 903452,6439601 `--look=180,-15` | 0,3 % |
| `buissons_*` | 901160,6436275 `--look=0,-6` | non chiffre (lande nettement visible) |
| `buissons_sol_*` | idem `--look=-90,-15` (dans l'herbe) | 9,5 % |

Constats a l'oeil :
- Rochers : blocs POSES au sol (aucun flottant vu), sur ruptures de pente du versant,
  aucun sur les replats de prairie. **Trainees NON visibles** a cette densite : les blocs
  sont epars (77 blocs et 131 rochers dessines dans la vue de profil pierrier). La
  coherence en trainees n'est donc prouvee que par le test d'anisotropie headless.
- Pierrier : le premier cadrage (`-90,-20`, face au versant) donne une image quasi
  identique ; les vues 0 et 90 montrent la roche nue, sans rocher proche.
- Buissons : une lande de jeunes resineux comble la pente AU-DESSUS de la foret de
  decor ; aucune intersection evidente avec les arbres de decor ni avec les arbres
  Forest. Au belvedere, deux petits arbres apparaissent pres d'un feuillu de decor, en
  avant de lui (profondeur non mesurable a l'oeil). Des petits arbres voisins
  s'interpenetrent entre eux (jusqu'a 6 par cellule de 8 m, sans espacement), ce qui se
  voit de pres.

## 5. Cout total rochers + buissons (A = normal, B = `--no-rocks --no-bushes`)
`profile_phase29.ps1 -Toggle "--no-rocks --no-bushes"`, 3 tours entrelaces, 20 s, medianes
des runs sains. Release : build separe `build-release\` (cmake Release + Ninja, 86/86),
meme script pointe sur l'exe Release (copie hors projet) ; pas de couche de validation.

| config / vue | A | B | A-B | RAM WS A/B |
|---|---|---|---|---|
| Debug alpage (ref. Phase 27) | 6 755 us (148,0 img/s) | 6 591 us (151,7) | **+164 us (+2,5 %)** | 299/291 Mo |
| Debug pierrier | 3 118 us (320,7) | 2 866 us (346,1) | **+252 us (+8,8 %)** | 304/295 |
| Debug buissons | 6 494 us (154,0) | 6 038 us (165,6) | **+456 us (+7,6 %)** | 299/290 |
| Release alpage | 4 954 us (201,5) | 4 830 us (204,1) | **+124 us (+2,6 %)** | 285/277 |
| Release pierrier | 1 748 us (571,1) | 1 679 us (595,7) | **+70 us (+4,2 %)** | 290/282 |
| Release buissons | 5 887 us (169,9) | 5 211 us (191,0) | **+676 us (+13 %)** | 285/277 |

Semis CPU (A) : rochers 62/72/127 us en Debug et 8/8/19 us en Release ; buissons
88/83/190 us en Debug. Ecart min-max intra-condition <= 3 img/s. 4 runs NON CONCLUANTS
(autres processus > 1 coeur) ecartes. 0 `[Validation]` sur les 18 runs Debug.
Situation : reference Phase 27 Release alpage = 4 962 us (201 img/s) ; Phase 29 mesuree
a 4 954 us -> **au niveau de la reference** (+124 us sur le temoin du jour). Pire cas
mesure (zone dense de petits arbres, Release) : 5,9 ms = 170 img/s ; le semis n'y pese
que ~19 us de rochers, c'est donc le dessin (~1 600 petits arbres, GPU). Reste tres
au-dessus de 60 img/s ; poste a surveiller : portee des petits arbres (260 m).

## 6. Non-regression par relecture - VERT
`diff -rq` de src et shaders contre `..\_Vercora_backups\avant-phase29-20260927-0513` :
fichiers existants modifies = **`src/main.cpp` et `shaders/foliage.vert` seulement** ;
nouveaux : Rocks, RockMesh, Bushes, BushMesh. Forest, Vegetation, Wildlife, Crafting,
Climber, Ground, Bike, Paraglider, Needs, collision : **identiques octet pour octet**.
- main.cpp : options `--no-rocks/--no-rock-collision/--no-bushes`, construction apres
  les fibres, `worldSolver.add(rocks)` seulement si des rochers existent (voulu : les gros
  blocs bloquent joueur ET faune, comme Forest), `forest->trees()` lu en const pour les
  emprises, 5 appels instancies derriere la touche G, 2 lignes de bilan. Ligne
  `Cout par image (PHASE 19)` inchangee.
- foliage.vert : pour `params.w >= 0` (arbres, herbe, fibres, buissons), `tilt` =
  `mat3(1.0)` et `tintSeed` = `variation` -> meme position, meme normale, meme teinte ;
  decoupe (w > 0) et `fragBladeSeed` inchanges. Le mode rocher n'agit que si w < 0.

## Ecart de documentation releve
Le journal actuel donne `masque pierrier 9,4 % ; 1,82 gros blocs, 4,66 rochers, 23,2
plaques / ha` (9,3 / 1,55 / 3,62 / 36,2 dans le processus J3, autre terrain de depart),
et non 8,4 % / 1,85 / 4,75 / 46 comme dans la section Agent 2 : `Rocks.cpp` date de
09:37, apres les mesures de l'Agent 2 (09:32). Les chiffres a jour sont ceux du journal.

## Non mesure
Scintillement et apparitions en mouvement ; a-coup du premier semis d'un bloc ; cout
sous la pluie ; vue de survol. Trainees de rochers a l'oeil (voir 4).


# Reprise du 2026-10-11 - prompt Phase 29 relance alors que la phase etait FAITE

Le prompt de cette session decrivait l'etat "Agent 1 termine, aucun code ecrit" et
attendait 3877 tests. **Faux** : Agents 2, 3 et 4 sont livres depuis le 2026-09-27
(sections ci-dessus). Decision (session sans supervision) : NE RIEN reecrire, aucun
fichier de `src/`, `shaders/`, `data/`, `assets/` ni `dist/` modifie ; etat verifie,
manques comblés par mesure seulement.

## Agent 0 - etat de depart
- Projet DEPLACE : `_CODE\_Vercora` -> `_CODE\4-vercors\vercora` (rangement du 09/10).
  `build\CMakeCache.txt` pointe encore vers `_CODE/_Vercora` -> `build\` est PERIME
  (et `tools\screenshot_scene.ps1` / `run_vulkan_check.ps1` lancent `build\vercora.exe`).
  Des dossiers vides `_CODE\_Vercora\build` et `external\glfw-build\src` ont ete
  recrees le 11/10 a 12:08 par quelque chose qui suivait l'ancien chemin.
- Aucun vercora/find/harnais orphelin au depart. Charge CPU 22 % (CurseForge,
  Antigravity, navigateur) : machine PAS au repos -> aucune mesure de perf refaite.
- Build Debug depuis zero dans un dossier NEUF `build-agent0\` (pour ne pas detruire
  `build\phase29\`) : 86/86, 0 avertissement compilateur (2 avertissements CMake
  "deprecated" de glm-src, connus).
- Tests headless : **3968/3968** = 3877 + 53 (rochers) + 38 (buissons). L'ecart avec
  les 3877 du prompt est exactement la Phase 29.

## Ecarts entre le prompt et l'existant (non corriges, documentes)
- Temoin buissons : `--no-bushes` (pas `--no-shrubs`).
- Collision : les gros blocs >= 1,3 m sont SOLIDES (via le solveur composite,
  decision Agent 2 documentee) ; le prompt demandait "non solides par defaut sauf si
  trivial" - c'est le cas "trivial" qui a ete retenu.
- Criteres, rugosite, semis par hachage, trainees, LOD, copie des cartes lowveg
  (`_canopy_lowveg*`) : conformes.

## --demo : decision
Dans le code, `--demo` ne coupe ni rochers ni buissons (rendu pur, c'est le propos
de la demo : regarder le terrain). Decision : **les garder actifs**. MAIS le paquet
`dist\VercoraDemo\` date du 27/08 (Phase 21) et ne contient que `vercora_zone_1m.vchm`
et l'ortho : **aucune carte de couvert**, donc dans la demo distribuee (et dans
`deploy_beta_to_desktop.ps1`, qui copie ces assets) : ZERO rocher, ZERO buisson
(et rien de ce qui depend du couvert). Pour les y mettre il faut ajouter au paquet
`vercora_zone_canopy.vchm`, `_canopy_height`, `_canopy_lowveg`, `_canopy_lowveg_height`
(+ ~16 Mo). NON fait : c'est un changement de ce qui part chez les testeurs.

## Captures aux points demandes - `build\phase29\captures_20261011\`
Exe `build-agent0`, `--hour=13 --day=12 --time-scale=0 --look=yaw,-20 --lock-input`,
avant = `--no-rocks --no-bushes`. 0 `[Validation]` sur 15 lancements.

| site | y0 | y90 | y180 | y270 |
|---|---|---|---|---|
| pierrier E903578 N6438125 (rug 0,10) | 1,0 % | 0,5 % | 1,1 % | avant manquant (*) |
| pied de barre E903602 N6438269 (rug 3,38) | 0,0 % | 0,1 % | 0,4 % | 0,0 % |

(*) le script de capture est reste bloque 20 min sans ecrire de ligne CAPTURE alors
que le jeu repondait et journalisait ; processus tue par PID. Cause non instruite.
Images `*_diff.png` : pixels changes en rouge.

**Constat** : au pierrier, l'ajout visible se reduit a UN petit arbre et 3-4
cailloux minuscules gris clair sur calcaire gris clair ; le reste des pixels rouges
est l'herbe Phase 28 qui ondule (le vent n'est pas gele par --time-scale=0). Au pied
de barre : rien de visible. Les rochers existent (journal : masque 9,4 %, 1,82 blocs
/ 4,66 rochers / 23,2 plaques par ha) mais **ne se voient pas aux deux sites que la
conception avait choisis** : densite faible + teinte proche du calcaire. Pistes (non
faites) : densite, teinte plus sombre/contrastee, ou blocs plus gros en pied de barre.
Et ces sites "pierrier" portent de l'herbe dense : pierrier vs pelouse raide, non
separables au LiDAR (limite Agent 1), se voit ici.

## Toujours non fait / non verifie
Zone boisee avec lisiere et zone a buissons trouvee par sondage aux points du prompt
(les captures du 27/09 `buissons_*` et `belvedere_*` couvrent le sujet ailleurs) ;
perf refaite (machine non au repos) ; suite A-J (inchangee depuis le 27/09, binaire
identique aux sources) ; scintillement en mouvement.
