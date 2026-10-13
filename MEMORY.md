# Vercora — MEMORY.md

Point d'entree rapide sur l'etat du projet. Pour le detail d'une phase donnee,
voir `NOTES-PHASE6.md`, `NOTES-PHASE7.md`, `NOTES-PHASE9.md`, `NOTES-PHASE10.md`,
`NOTES-PHASE11.md`, `NOTES-PHASE12.md`, `NOTES-PHASE-VELO.md`,
`NOTES-PHASE-ESCALADE.md`, `NOTES-PHASE15.md`, `NOTES-PHASE16.md`,
`NOTES-PHASE17.md`, `NOTES-PHASE18.md`, `NOTES-PHASE19.md`,
`NOTES-PHASE20.md` et `NOTES-PHASE21.md` ; ce fichier est
un resume vivant, mis a jour a la fin de chaque session, pas un journal complet.

## Ce qu'est Vercora

Jeu d'exploration de montagne solo, moteur maison C++20/Vulkan, cross-platform
Windows/Linux (GLFW + CMake). **LES QUATRE ACTIVITES DU CADRAGE ONT DESORMAIS UNE
BASE** : marche/abattage/taillage (fonctionnelles), parapente (Phase 8), velo
(Phase 13) et **escalade (Phase 14)**. Aucune n'est finie - chacune est un modele
simplifie, chiffre et teste - mais plus aucune n'est vide.

**Depuis la Phase 15, le projet a aussi un OUTIL** : un editeur de niveau interne
(touche F2) qui pose des objets sur le relief. Ce qu'il pose fait partie du
MONDE - une partie normale les charge, les dessine et s'y cogne. Suite du
2026-08-23 : **F3/F4 selectionnent l'objet pose suivant/precedent SANS viser**
(detail plus bas, section Phase 15).

## Etat verifie (2026-08-26, fin de Phase 20)

- **GPU DE CETTE MACHINE : Intel UHD 770 integree.** Il n'y a plus de GTX 960
  (confirme par l'utilisateur, ce n'est pas un defaut de detection). Tous les FPS
  releves avant la Phase 9 - dont les 848 FPS de la Phase 7 - ont ete mesures sur
  la GTX et ne sont PAS comparables aux chiffres actuels. Repere re-mesure sur le
  GPU actuel : la dalle unique de Phase 7 tourne a **247 FPS**. i5-14600K,
  64 Go de RAM, mesures en Debug avec couches de validation.
- **Build** : `tools\build.bat` (CMake + Ninja autonomes, cl.exe de VS 18 - voir
  l'en-tete du script pour la corruption partielle de VS sur cette machine).
- **Tests headless** : `tools\run_logic_tests.bat` - **3578 assertions** (chiffre
  de la Phase 23 ; il etait annonce a 935 en Phase 20, chiffre deja perime
  ensuite), sans GPU
  ni Vulkan (terrain, foret/abattage, ramassage, taillage, parapente, quadtree /
  selection de LOD / frustum culling, format de texture .vcto, semis d'arbres et
  d'herbe, format d'instance, chaine de peres du quadtree et invariants du repli
  sur l'ancetre, la physique du velo depuis la Phase 13, celle de l'escalade
  depuis la Phase 14, et **tout l'editeur sauf le dessin depuis la Phase 15** -
  catalogue, raycast terrain, scene, annulation, collision, format .vcsc,
  machine a etats, et **les quatre jauges de besoins plus la detection des
  points d'eau depuis la Phase 16** - 129 assertions a elles seules, et
  **l'empreinte environnementale depuis la Phase 17** - memoire du milieu, faune
  et degradation du sol, 97 assertions, et le **SYSTEME METEO depuis la
  Phase 23** - determinisme et independance du chemin, les deux couches, le
  sondage amont du vent, la carte de piegeage d'air froid, le cycle jour/nuit et
  les effets sur les quatre activites).
- **Vulkan reel** : `tools\run_vulkan_check.ps1` - **9 scenarios** (A gameplay
  synthetique, B collision, C dalle LiDAR reelle, D vol en parapente, E sortie a
  velo sur un versant mesure, F escalade sur une paroi mesuree, **G editeur de
  niveau en trois processus - edition, relecture en jeu normal, et TEMOIN sans
  la scene**, **H besoins primaires : depart sur un point d'eau detecte par le
  jeu lui-meme, deux touches T qui exercent le test de portee dans les DEUX
  sens, puis 160 s de marche et 60 s d'immobilite - la forme doit descendre PUIS
  remonter**, **I empreinte environnementale : faune semee sur relief mesure,
  240 s de presence continue, la zone proche doit se VIDER**), sous couches de
  validation. Zero `[Validation]`, zero
  `[ERROR]`/`[WARN]` a chaque run reussi.
  **`-Only <lettres>`** (Phase 15) ne joue qu'une partie des scenarios, pour
  iterer sans payer les quinze minutes de la suite complete ; un scenario non joue
  est IGNORE, pas valide, donc un run de reference se fait sans `-Only`.
  **Flakiness connue** : les scenarios A/B pilotes par PostMessage
  (scancodes, pas de vraie souris) echouent parfois sur un detail de timing
  (cible d'abattage manquee de peu, ou WM_CLOSE lent a etre traite) - c'est une
  variance d'automatisation deja documentee dans le script (antivirus/
  ordonnancement), PAS un defaut du jeu. Reste correle : re-executer suffit.
  Le scenario D (parapente), lui, a reussi 3 fois sur 3 lors de son ajout.
- **Validation de SYNCHRONISATION** (Phase 12, nouvelle) : les couches standard
  ne verifient PAS les dependances memoire - elles etaient a zero message y
  compris avec une synchronisation fausse. Pour tout changement de synchro GPU,
  relancer avec `set VK_LAYER_VALIDATE_SYNC=1`, et faire le CONTROLE NEGATIF
  (retirer la barriere, verifier que l'outil crie). C'est ce qui a mis au jour un
  defaut de dependance de subpass anterieur a la Phase 12.
- **Le jeu lit le clavier et la souris SYSTEME.** Toute manipulation de la
  machine pendant une capture ou une mesure automatisee deplace la camera. Depuis
  la Phase 11, `--lock-input` (passe PAR DEFAUT par `screenshot_scene.ps1`) fige
  entierement la camera, et `--start=est,nord` / `--look=yaw,pitch` determinent la
  vue. Sans ce verrou, deux captures du meme script differaient sur 65 % de leurs
  pixels. **PRECISION DE LA PHASE 12** : `--lock-input` seul ne suffit PAS a
  garantir le cadrage - trois captures de la meme scene ont donne 38,2 / 38,3 /
  32,9 % de ciel, donc trois vues differentes. **Pour toute comparaison d'image,
  epingler aussi `--look`** ; on retombe alors dans le bruit (une soixantaine de
  pixels sur 921 600 entre deux runs identiques).
- **PRECISION DE LA PHASE 15, et c'est un piege a part** : `--lock-input` ne
  verrouillait que la CAMERA. L'editeur, lui, lisait la souris directement, et
  une execution complete du harnais a pose **9 objets pour 3 touches envoyees** -
  les clics REELS de l'utilisateur atterrissant dans la fenetre du jeu, qui prend
  le focus a chaque nouveau processus. La nuance GLFW qui rendait ca invisible :
  la POSITION du curseur vient de `GetCursorPos()` (un WM_MOUSEMOVE poste n'a
  aucun effet, limite connue), mais **l'ETAT DES BOUTONS est mis en cache depuis
  les messages de fenetre** - donc les clics passent la ou le curseur ne passe
  pas. Corrige : `--lock-input` coupe aussi la pose au clic. Controle negatif,
  3 clics postes -> **3 objets avant, 0 apres**. Le CLAVIER reste vivant (touche
  INSER), sans quoi aucun scenario ne pourrait piloter l'editeur.
- Terrain LiDAR reel (dalle Le Gua/Isere, 1001x1001, 562 m de denivele) chargee
  et fonctionnelle par defaut ; terrain synthetique reste le monde de reference
  des tests (semis deterministe).

## Phase 8 (2026-08-20) : visee sur relief + base du parapente

### Correction de la visee d'arbre (pas un bug de gameplay)

Le vrai souci de la Phase 7 ("6 arbres sur 16 abattables sur relief reel")
n'etait **pas** dans le jeu : `Forest::pickTree` (src/world/Forest.cpp) a
toujours fait une intersection rayon 3D correcte, et le gameplay reel
(`main.cpp`, appel a `forest->pickTree(camera.position(), camera.front(), ...)`)
vise deja avec le pitch de la camera inclus (`Camera::front()` a une composante
Y des que `pitch_ != 0`). Le rayon HORIZONTAL etait confine au harnais de test
`--dump-world` (`dumpWorld()` dans `main.cpp`), necessaire parce que
`run_vulkan_check.ps1` ne peut pas piloter la souris (GLFW lit le curseur
SYSTEME, pas les messages postes - voir l'en-tete du script).

Corrige : `dumpWorld()` vise maintenant le tronc de chaque arbre (mi-hauteur)
avec un rayon pitch-inclus (`normalize(cible - oeil)`), et journalise le pitch
requis + la categorie (au-dessus/en-dessous/au-niveau). **16 arbres sur 16
sont abattables sur la dalle LiDAR reelle** des que le pitch est utilise -
confirme par `vercora.exe --dump-world` (voir aussi le terrain synthetique,
16/16 egalement, avec les 3 categories representees). Une seconde ligne
`AIMLEVEL` (rayon horizontal, comme avant) reste emise pour le tir REELLEMENT
joue par le script, puisque celui-ci ne peut physiquement pas incliner la
camera pendant un run.

### Parapente : premiere session de physique

Nouveau : `src/world/Paraglider.{hpp,cpp}` - modele de vol volontairement
simplifie (pas d'aerodynamique, pas de vent/thermiques - viendront avec la
meteo) :
- vitesse de croisiere ~38 km/h, taux de chute 1,25 m/s en air calme ;
- virage a vitesse angulaire constante (25 deg/s), donc rayon de virage fini,
  pas une rotation instantanee ;
- frein (touche S en vol) : reponse EXPONENTIELLE (constante de temps 0,5 s)
  vers une vitesse plus faible et un taux de chute legerement plus fort - pas
  un interrupteur ;
- atterrissage detecte des que l'altitude rejoint `terrain.heightAt(x,z) +
  eyeHeight`, position collee exactement au sol.

Integration dans `main.cpp` : touche **V** decolle (uniquement en mode Free,
au moins 5 m au-dessus du terrain sous les pieds) ou force un atterrissage si
deja en vol (filet de securite). Pendant le vol, A/D tournent, S freine ;
`Forest::update` continue de tourner (une chute d'arbre en cours ne se fige
pas parce qu'on decolle) mais E/R/C sont desactives. Un cube fait office de
marqueur visuel du pilote (pas de vrai mesh d'aile pour cette session).

Camera : nouveau mode `CameraMode::Glide` (src/core/Camera.hpp/cpp) - la
camera ne pilote plus rien elle-meme en vol, elle affiche une vue chase-cam
(troisieme personne, derriere et au-dessus du pilote) via la nouvelle methode
`Camera::setChasePose(eye, lookAt)`, qui recalcule aussi yaw_/pitch_ pour que
le retour en marche apres atterrissage ne fasse pas sauter l'orientation.

Verifie : 16 nouvelles assertions headless (`tests/logic_tests.cpp`, section
"Phase 8") + scenario D de `run_vulkan_check.ps1` (decollage, virage, frein,
atterrissage NATUREL contre le relief LiDAR reel, resize, fermeture propre) -
zero erreur de validation, 3 executions sur 3 reussies.

### Hors scope, assume (a reprendre plus tard)

- Pas de vent/thermiques/ascendances (systeme meteo a venir).
- Pas de collision avec les arbres pendant le vol.
- Pas de gestion de crash/dommages : un atterrissage brutal se comporte comme
  un atterrissage normal.
- Pas de vrai mesh d'aile/pilote ni de course d'elan au decollage.
- Le marqueur cube n'a pas d'animation ni d'inclinaison en virage (visuel
  minimal, purement pour la lisibilite du test).

## Phase 9 (2026-08-21) : la vraie zone de jeu, 25 dalles assemblees

Detail complet dans `NOTES-PHASE9.md`. (La consigne de session parlait de
"Phase 8" ; ce numero etait deja pris par la session parapente ci-dessus, d'ou
Phase 9.)

**La zone de jeu est choisie et assemblee** : grille 5 x 5 km, Lambert 93
X 899-903 / Y 6436-6440, massif d'altitude (1028,8 a 2285,2 m, **1256 m de
denivele**). C'est la session qui a mis a l'epreuve les deux bugs d'arrondi
corriges en Phase 7 mais jamais eprouves en multi-dalles.

### Chiffres

| | |
|---|---|
| Telechargement | 25/25 dalles, **4,99 Go en 73 s** (~85 Mo/s), 0 echec |
| Sur disque | `data/lidar` **5,2 Go**, `assets/terrain` 271 Mo |
| Conversion | ~6 s/dalle, ~2,5 min au total, toutes en 1001 x 1001 au pas de 1 m |
| Terrain assemble 1 m | **5001 x 5001**, 95,4 Mo (et non 5005 : bords PARTAGES) |
| Terrain assemble 2 m | 2501 x 2501, 25 Mo |

### Continuite aux frontieres : VERIFIEE, conforme

Deux controles independants, sur les **8 frontieres interieures**, pas seulement
a l'oeil :

1. `tools/check_seams.py` (nouveau) confronte le terrain assemble aux **nuages
   bruts**, 1 360 sondes exploitables. Lecture directe **0,020 m** d'ecart median
   contre **0,290 m** pour le meilleur decalage d'un noeud, soit **x14,5** : c'est
   ce CONTRASTE qui prouve l'alignement. Les 8 frontieres ont une mediane
   <= 0,030 m ; les temoins en plein milieu de dalle sont a 0,000 m.
2. `tools/assemble_terrain.py` mesure la pente aux jointures sur le produit fini :
   aucune ne se distingue du relief environnant (un decalage la rendrait trop
   grande, une colonne dupliquee trop faible).

Ecart entre les deux versions d'un noeud partage (40 040 noeuds) : mediane
**0,170 m**, p99 2,78 m, max 79,23 m. **Les valeurs extremes sont sur des parois
verticales** (relief local median de 34 a 60 m d'amplitude sur 5 m, contre 1,6 m
pour les noeuds concordants) : c'est la limite d'une heightmap 2,5D sur une
falaise, pas un defaut d'assemblage. Verifie point par point sur le pire noeud.

### Performances - LE POINT IMPORTANT

| Scene | Triangles | FPS | Pic RAM |
|---|---:|---:|---:|
| Procedural | 32 258 | 1381 | 108 Mo |
| 1 dalle 1 m (Phase 7) | 2 000 000 | 247 | 302 Mo |
| 2 x 2 dalles 1 m | 8 000 000 | 67 | 805 Mo |
| **5 x 5 dalles 2 m (25 km2)** | 12 500 000 | **46** | 1 083 Mo |
| **5 x 5 dalles 1 m (25 km2)** | 50 000 000 | **12** | 4 014 Mo |

Zero erreur de validation Vulkan sur les cinq scenes. Le 25 km2 a 1 m **charge et
fonctionne, mais n'est pas jouable**. La camera traverse 5,6 km en franchissant
les anciennes frontieres sans aucune chute de FPS localisee, sans blocage, sans
chute a travers le sol (`tools/traverse_terrain.ps1`).

**=> LE STREAMING PAR TUILES DEVIENT NECESSAIRE MAINTENANT, pas plus tard.** Le
facteur limitant n'est ni la RAM (4 Go sur 64), ni le disque, ni la conversion :
c'est que le moteur envoie **un seul maillage entier a chaque image**, sans LOD ni
culling, alors que le joueur n'en voit qu'une fraction infime. Le cout est
proportionnel a la surface (247 -> 67 -> 12 FPS pour 1 -> 4 -> 25 km2). Les 25
heightmaps individuelles sont conservees et leur georeferencement est exact : le
decoupage en tuiles n'aura rien a redecouper.

En attendant, **`assets/terrain/vercora_zone_2m.vchm` est le repli utilisable** :
meme zone de 25 km2, 46 FPS, 1 Go. Ce n'est pas un substitut au 1 m, qui est
produit, verifie et livre.

### Non-regression : complete

Terrain procedural (1381 FPS) et dalle unique de Phase 7 (247 FPS) intacts et
toujours disponibles ; **185 assertions headless** passent ;
**`run_vulkan_check.ps1` : 4 scenarios sur 4, zero validation, du premier coup**.
`vercora.exe` sans argument charge **toujours la dalle de Phase 7** : la zone
25 km2 se charge par chemin explicite, le choix du defaut revient a
l'utilisateur une fois le streaming en place.

**Aucun fichier de `src/` n'a ete modifie en Phase 9.**

## Phase 10 (2026-08-21) : quadtree, LOD continu (CDLOD) et frustum culling

Detail complet dans `NOTES-PHASE10.md`. C'est la reponse au verdict de la
Phase 9 : le moteur envoyait UN SEUL maillage entier par image, sans LOD ni
culling, d'ou un cout proportionnel a la surface du terrain.

### Le resultat

Meme zone, meme position de camera, meme binaire, Debug avec couches de
validation, Intel UHD 770.

| Scene | Phase 9 (bloc unique) | **Phase 10 (LOD)** |
|---|---:|---:|
| **25 km2 au pas de 1 m** | 12 FPS / 4 014 Mo | **487 FPS / 246 Mo** |
| **25 km2 au pas de 2 m** | 46 FPS / 1 083 Mo | **601 FPS / 166 Mo** |
| 1 dalle 1 m (Phase 7) | 247 FPS / 302 Mo | **847 FPS / 141 Mo** |
| Procedural (Phase 4) | 1 381 FPS / 108 Mo | 1 244 FPS / 106 Mo |

Sur la zone 1 m : **50 000 000 -> 548 864 triangles soumis** (1,1 %), chargement
**10,4 s -> 0,5 s**. Zero message `[Validation]` sur les quatre scenes.
Controle visuel automatise (`tools\screenshot_scene.ps1`) : la vue avec LOD est
identique a la vue sans LOD, ce qui ecarte l'hypothese "excellents FPS parce que
le culling est casse".

### Comment le quadtree est structure

Nouveaux fichiers : `src/core/Frustum.hpp`, `src/world/TerrainQuadtree.{hpp,cpp}`,
`src/renderer/TerrainNodeMesh.{hpp,cpp}`, `src/renderer/TerrainStreamer.{hpp,cpp}`.
Les trois premiers ne dependent que de GLM et sont **executes** dans les tests
headless ; seul le streamer touche a Vulkan.

**Chaque noeud est toujours maille avec le meme nombre de sommets** (65 x 65,
8 192 triangles). Ce qui change avec le niveau, c'est le PAS d'echantillonnage :
niveau L = pas 2^L, couvre 64 x 2^L cellules. Un noeud de niveau L coute donc le
meme prix GPU qu'une feuille en couvrant 4^L fois plus de terrain. La racine est
le plus petit niveau couvrant toute la grille (zone 1 m : niveaux 0..7, 8 380
noeuds).

Deux details qui comptent : les bornes verticales d'une feuille sont calculees a
la PLEINE resolution (une boite trop petite ferait disparaitre du terrain au
culling), et les indices sont ECRETES sur le bord du terrain (5 000 n'est pas
multiple de 64 ; sans ecretage la racine produirait une "etagere" plate de 3,2 km
au-dela des donnees).

### Comment les LOD sont selectionnes

`range[L] = 3 x tailleMonde(tuile L)`, donc DOUBLE a chaque niveau (au pas de
1 m : L0 <= 192 m ... L7 <= 24 576 m). Descente depuis la racine : hors frustum ->
abandon de tout le sous-arbre ; niveau 0 ou distance >= range[L-1] -> le noeud est
dessine ; sinon on descend dans les 4 fils. La distance est celle a la BOITE du
noeud, pas a son centre.

**Geomorphing** : chaque sommet porte aussi sa position/normale dans la grille du
niveau superieur (attributs de sommet 4 et 5, obtenus en arrondissant les indices
LOCAUX a un multiple de 2). Le vertex shader interpole selon la distance du
sommet, dans la bande `[0,70 ; 0,95] x range[L]` poussee par push constant.
Ca supprime le popping ET les fissures : au-dela de range[L] une tuile L est
entierement morphee sur la grille L+1, ou sa voisine L+1 est encore a morph = 0.
**C'est pourquoi morphStartRatio doit rester > 0,5** (le constructeur refuse le
contraire). La distance est prise sur la position NON morphee, pour que deux
tuiles voisines s'accordent sur un sommet partage.

### Decouplage rendu / collision : verifie, rien n'a change cote physique

La `Heightmap` pleine resolution reste la source UNIQUE de `heightAt()` :
`Camera`, `Forest`, `Ground`, `Paraglider` la consomment inchangee. Le quadtree ne
detient que des BORNES, pas d'altitudes. Verifie par un test headless dedie
(`heightAt()` identique au bit pres selon que le quadtree vient d'etre selectionne
de tres loin ou au contact) et par `run_vulkan_check.ps1` : **4 scenarios sur 4,
du premier coup**, 2 abattages, 3 debitages, 8 ramassages, 1 percussion, collision
tronc/souche, vol de parapente avec atterrissage naturel. Au contact du joueur les
tuiles sont de niveau 0, donc la surface affichee y est aussi celle de la donnee.

### Ce qui reste a ameliorer (par ordre d'importance)

1. ~~**Transfert asynchrone.**~~ **LEVE EN PHASE 12.** Le `vkQueueWaitIdle` par
   upload a disparu du chemin de streaming, le budget est passe de 4 a 64 tuiles
   par frame.
2. **Index buffer partage.** La topologie est identique pour toutes les tuiles
   (teste), mais chacune a encore son propre index buffer : 98 Ko et une
   allocation Vulkan gaspilles par tuile.
3. **Alleger le sommet.** Il est passe de 44 a 68 octets pour porter le
   geomorphing. Consequence honnete : le chemin `--no-lod` a REGRESSE (zone 1 m :
   12 -> 9 FPS, 4,0 -> 5,6 Go) et le terrain procedural perd 10 % (1 381 ->
   1 244 FPS). Le chemin reel, lui, n'a jamais 25 M de sommets en vol. Piste :
   n'encoder que le DELTA de morphing, ou reprendre `color`/`uv` inutilises.
4. ~~Pas de repli sur l'ancetre quand une tuile n'est pas encore construite.~~
   **LEVE EN PHASE 12** (repli + sommet du quadtree epingle).
5. Pas de streaming DISQUE : la heightmap entiere (95 Mo) reste chargee d'un bloc.
6. Les parametres du quadtree ne sont pas reglables en ligne de commande.
7. Aucun test automatique ne cherche une fissure dans l'IMAGE rendue.

### Traversee en mouvement : 20 km, zero validation

`traverse_terrain.ps1` sur la zone 25 km2 au pas de 1 m, 20 km parcourus (2,5 km
par branche, aller-retour sur les deux axes, en sortant deux fois de l'emprise) :
FPS moyen de 459 a 725 selon la branche, **minimum instantane 146 FPS** - soit
douze fois le chiffre MOYEN de la Phase 9 a l'arret. Zero `[Validation]`, aucune
chute localisee aux anciennes frontieres de dalles, aucune chute a travers le sol.

**Un incident non elucide** : sur ce run de 20 km, le jeu n'est pas sorti dans les
30 s apres `WM_CLOSE` (tue par le script). Enquete : une traversee de 3,2 km sort
proprement, et une sonde chronometrant la fermeture apres 0 s / 60 s / 180 s de
marche continue (jusqu'a 130 tuiles residentes) donne **0,1 s dans les trois cas**.
Le temps de sortie ne depend donc PAS du nombre de tuiles residentes ; rien ne
rattache l'incident au streaming, et ca ressemble a la flakiness `PostMessage`
deja connue. Mais 30 s reste beaucoup, et ce n'est pas reproduit : **a re-tenter
au prochain run long**, pas classe.

### Non-regression

**223 assertions headless** (185 avant, +38 pour la Phase 10) ;
**`run_vulkan_check.ps1` : 4/4, zero validation, du premier coup** ; terrain
procedural et dalle de Phase 7 intacts et plus rapides qu'avant ; scenes non
terrain intactes (`vercora.exe cube` a 3 403 FPS, zero validation - le materiau
`Object` ne se voit decrire que 4 attributs de sommet, cf. `ShaderSet::
usesMorphAttributes`) ; `--dump-world` retrouve 16 arbres visables sur 16.
`vercora.exe` sans argument charge **toujours la dalle de Phase 7** ; toute scene
de terrain passe desormais par le quadtree, sauf avec l'option **`--no-lod`**, qui
reproduit le comportement de la Phase 9 pour la comparaison.

## Phase 11 (2026-08-21) : orthophoto, arbres a l'echelle, herbe

Detail complet dans `NOTES-PHASE11.md`. Le terrain LiDAR ne portait QUE de la
geometrie : cette session lui donne une couleur reelle et l'habille.

### Le resultat

| Scene, position de depart par defaut | FPS median | Pic RAM |
|---|---:|---:|
| Zone 25 km2 a 1 m, chemin Phase 10 (`--no-ortho --no-vegetation`) | 514 | 240 Mo |
| **Zone 25 km2 a 1 m, Phase 11 complete** | **271** | **332 Mo** |

Cout brique par brique, meme vue : terrain seul 658 FPS -> +orthophoto 637 ->
+arbres 466 -> +herbe 283. **L'orthophoto coute 3 % de FPS et 101 Mo** : c'est de
loin le meilleur rapport de la session. Zero `[Validation]` partout.

### Orthophoto

BD ORTHO IGN par le WMS de la Geoplateforme (couche `HR.ORTHOIMAGERY.ORTHOPHOTOS`,
licence Etalab), meme portail que le LiDAR. **25 tuiles, 23,8 Mo en 101 s.**

- **L'emprise est LUE dans l'en-tete du .vchm**, jamais recopiee a la main.
- **Le serveur ne parle que WMS 1.3.0, et l'ordre des axes en EPSG:2154 est
  EST,NORD** - tranche par l'experience : l'ordre inverse renvoie une image
  BLANCHE UNIFORME. Rejoue a chaque execution (`--self-test`).
- Les UV du terrain etaient DEJA en coordonnees du terrain entier depuis la
  Phase 10 : rien a changer cote maillage, et `gz = 0` correspond au nord donc a
  `v = 0` - **aucun retournement necessaire**.
- Format maison **`.vcto`** : 8192 x 8192, **0,61 m/texel**, BC1, 14 mipmaps
  PRECALCULEES (on ne peut pas filtrer des blocs BC1 sur le GPU), 44,7 Mo au lieu
  de 357 Mo en RGBA8. Encodeur numpy maison, **PSNR 33,2 dB**.
- `samplerAnisotropy` activee (optionnelle en Vulkan, abandonnee proprement si
  absente) : indispensable pour un plan regarde en rasant.
- Binding 1 declare pour TOUS les materiaux, occupe par une texture 1x1 blanche
  tant qu'aucune photo n'est chargee. Repli procedural conserve : touche **O**,
  ou `--no-ortho`.
- `orthoMatchesTerrain()` REFUSE une texture dont l'emprise ne correspond pas au
  terrain charge, a 0,5 m pres.

### Arbres : la classification LiDAR est exploitable, et excellente

`tools/lidar_to_canopy.py` depouille les 25 dalles en **150 s** et produit deux
cartes au pas de 5 m, **au format .vchm** (une grille de flottants georeferencee
n'a pas a contenir des altitudes) : le **couvert** (part des retours de classes
3/4/5, dit OU semer) et la **hauteur de canopee** (dit QUELLE TAILLE).

**Validation croisee decisive**, contre l'orthophoto qui est une source
totalement independante : correlation couvert / luminance de **-0,729** ;
cellules a couvert > 0,70 -> RVB (67,75,72) vert sombre ; cellules nues ->
(162,162,153) gris clair. Cela valide d'un coup le georeferencement des DEUX
jeux de donnees.

`world/Vegetation` a cote de `world/Forest`, **deux systemes et non un** :
`Forest` garde ses 16 arbres abattables a etat, `Vegetation` produit des dizaines
de milliers d'arbres de decor sans etat. **Rien n'est stocke pour les 25 km2** :
le semis est une fonction deterministe de la cellule, par HACHAGE et non par un
generateur a etat - un `mt19937` dependrait de l'ordre d'appel, donc du chemin
parcouru (teste). Densite obtenue : **59 arbres/ha** sur toute la zone.

**Niveau de detail de densite** : chaque emplacement de cellule a sa PROPRE
portee (1100 m pour le premier, 320 m pour le dernier), donc la foret
s'eclaircit arbre par arbre au lieu de sauter par bosquets. Le fondu se fait par
RETRECISSEMENT, pas par transparence : aucun tri en profondeur necessaire.

**Instanciation GPU ajoutee au moteur** (elle n'existait pas) : binding de
sommets 1 cadence par instance, `InstanceData` de **28 octets**, materiau
`Foliage` partage par les arbres et l'herbe, buffer par frame-in-flight qui
grandit a la demande.

### La mesure qui a change le code

Les arbres coutaient bien plus que leur geometrie. Experience discriminante :
camera vers le CIEL, donc presque aucun arbre dessine mais le meme balayage.
**1946 FPS sans vegetation contre 351 avec** : **2,3 ms par image de balayage
PUR**. Le cout etait la boucle qui visitait ~8400 cellules par image, y compris
derriere le joueur.

Correctif : **culling par blocs de 8x8 cellules** (16x16 pour l'herbe), plus la
suppression d'un `heightAt()` calcule deux fois par touffe. Balayage 2,3 ms ->
0,11 ms ; scene complete 193 -> 283 FPS. **Verifie NON DESTRUCTIF** (le piege des
"excellents FPS parce que le culling est casse") : comptes d'instances
identiques au chiffre pres, et 98 pixels differents sur 879 360.

### Herbe : un essai, etat honnete

Bruit de valeur a deux octaves (longueur d'onde 14 m) pour GROUPER les touffes,
taille et densite pilotees par l'altitude (0,30 m en bas, 0,11 m en haut ; dense
sous 1750 m, absente au-dessus de 2250 m), portee courte (34 m, fondu des 14 m),
instanciation GPU. La donnee LiDAR distingue le pre du sous-bois et du lapiaz.

**Ce qui ne convainc pas** : avec 2158 touffes dans une vue de prairie, cela se
lit comme un **semis de touffes espacees, pas comme un tapis** - il en faudrait
4 a 5 fois plus. Chaque touffe est un eventail de 3 quads OPAQUES sans texture ni
decoupe alpha (choix qui evite tout melange alpha, donc tout tri en profondeur,
mais qui plafonne le realisme). A noter : ce massif est majoritairement forestier
et rocheux, la prairie ouverte sous 1500 m n'y represente que **0,4 %** des
cellules - l'herbe y a structurellement peu de terrain.

### Non-regression

**268 assertions headless** (223 avant, +45) ; **`run_vulkan_check.ps1` : 4/4 du
premier coup**, zero validation, 2 abattages / 3 debitages / 8 ramassages /
1 percussion / vol de parapente. `--dump-world` intact.

Terrain procedural et dalle de Phase 7 : le chemin Phase 10 est **intact et meme
plus rapide** (1348 et 884 FPS avec `--no-vegetation`, contre 1244 et 847 en
Phase 10). Leur baisse en Phase 11 complete (258 et 212 FPS) est le COUT de la
vegetation, qui y est semee par un substitut par pente et altitude faute de carte
LiDAR - pas une regression.

### Options et touches ajoutees

`--no-ortho`, `--ortho=chemin.vcto`, `--no-vegetation`, `--no-grass`,
`--start=est,nord`, `--look=yaw,pitch`, `--lock-input`.
Touches **O** (orthophoto / procedural) et **G** (tout / arbres seuls / rien).

### Ce qui reste a ameliorer (par ordre d'importance)

1. **Texture de detail de pres** : a 0,61 m/texel l'orthophoto est molle sous les
   pieds (un texel couvre ~150 pixels d'ecran a 3 m). C'est le defaut visuel le
   plus visible.
2. **Densifier l'herbe et lui donner une decoupe alpha** (`discard` plutot que
   melange, pour eviter le tri).
3. **Promotion d'un arbre de decor en arbre abattable** a l'approche : seuls les
   16 arbres de `Forest`, semes autour de l'origine, sont abattables. Un joueur
   qui marche 200 m est dans une foret entierement decorative.
4. **Collision avec les arbres de decor** : ils se traversent.
5. Ombres portees (les arbres flottent visuellement sur le sol).

## Phase 12 (2026-08-21) : transferts de tuiles asynchrones et repli sur l'ancetre

Detail complet dans `NOTES-PHASE12.md`. C'est la levee du **verrou n 1** pose par
la Phase 10 : `VulkanBuffer::copyFrom` faisait un `vkQueueWaitIdle` par upload de
tuile, d'ou un budget de 4 tuiles par frame et un TROU la ou une tuile n'etait pas
prete.

### Le GPU de cette machine n'a PAS de queue de transfert dediee

Journalise a chaque demarrage : `Familles de queues : 2 - [0] x1 graphics compute
transfer sparse [1] x2`. La famille 1 est une famille video, sans bit transfert.
La branche "queue dediee" ne s'applique donc pas ici, et elle n'a **pas** ete
ecrite : elle imposerait un transfert de propriete de famille de queue
(release/acquire) qu'aucun test de cette machine ne pourrait exercer. La detection
existe et est journalisee (`QueueFamilyIndices::dedicatedTransferFamily`).

### L'architecture retenue

Les copies staging -> device-local ne sont plus attendues : `Mesh` les confie a un
**`BufferUploader`** (interface dans `VulkanBuffer.hpp`, implementee par le
`Renderer`), qui les accumule et les enregistre **en tete du command buffer de la
frame**, avant `vkCmdBeginRenderPass`, suivies d'**UNE** `VkMemoryBarrier`
`TRANSFER_WRITE -> VERTEX_ATTRIBUTE_READ | INDEX_READ`
(`TRANSFER -> VERTEX_INPUT`).

Une seule soumission par frame, aucune attente CPU, dependance memoire
**explicite et verifiable**, et la tuile est lisible **des cette frame** - il n'y
a donc aucune latence de transfert a masquer.

Les buffers staging survivent via le meme compte a rebours que `retiredMeshes_`
de la Phase 10 (`kMaxFramesInFlight + 1` vieillissements).

**Un seul `vkQueueWaitIdle` subsiste**, dans `Renderer::flushCopiesNow`, atteint
uniquement quand le staging vivant depasse **128 Mo** : c'est le garde-fou du
chemin `--no-lod`, dont le maillage unique de 3,4 Go faisait autrement passer le
pic memoire de 5,6 a 6,9 Go. **0 declenchement** sur tout le streaming.

### La verification qui compte : controle NEGATIF

Un journal propre ne prouve rien si l'outil ne regarde pas au bon endroit. La
barriere a donc ete retiree volontairement :

| Barriere | Validation de synchronisation |
|---|---|
| **retiree** | **10 x `READ_AFTER_WRITE hazard ... reads index VkBuffer ... written by vkCmdCopyBuffer`** |
| en place | **0** |

Au passage, la validation de synchronisation a revele un defaut **anterieur** : la
dependance de subpass de `SwapChain` avait `srcAccessMask = 0`, alors qu'il n'y a
qu'UN buffer de profondeur pour deux frames-in-flight - WRITE_AFTER_WRITE entre le
clear de la frame N+1 et le storeOp de la frame N. Reproduit sur la scene `cube`,
donc sans aucun upload. Corrige.

### Le repli sur l'ancetre, et pourquoi il ne suffisait pas

Une tuile pas encore construite n'est plus un trou : on remonte sa chaine de peres
(nouveau `Node::parent`) jusqu'a une tuile residente. Le repli **remplace** le
sous-arbre au lieu de s'y superposer, sinon deux surfaces de niveaux differents se
disputeraient le z-buffer.

**Le repli seul ne suffisait pas**, et c'est une mesure qui l'a montre. Un
diagnostic sur les tuiles restees non couvertes a donne : niveaux 3, 4 et 5, dont
les ancetres de niveau 6 et 7 n'avaient **jamais ete dessines** - un noeud proche
de la racine ne se dessine que vu de plus loin que le terrain n'est grand, ce qui
n'arrive jamais quand on est dedans.

D'ou le **sommet du quadtree epingle** : les 3 niveaux les plus grossiers sont
construits au demarrage et jamais evincees (**14 tuiles, 5 Mo** sur la zone de
25 km2). Tout noeud selectionne est alors soit epingle, soit descendant d'un
epingle : **un trou devient structurellement impossible**, et le compteur
`uncoveredNodes()` reste mesure a chaque frame pour le prouver.

Une hypothese intermediaire - "le LRU evince les ancetres", corrigee par une
eviction biaisee par niveau - a ete **testee, mesuree marginale (25 -> 23 frames)
et retiree**.

### Les trous : mesures

Zone 25 km2 a 1 m, survol scripte (`--flythrough`), meme binaire, seules les
options changent. A **60 m/s** (six fois un parapente) meme les reglages Phase 10
ne produisent aucun trou : il a fallu monter en vitesse pour que la comparaison
prouve quelque chose.

| Vitesse | Budget | Repli | Sommet epingle | Frames a trou |
|---:|---:|:---:|:---:|---:|
| 300 m/s | 4 | non | non | 21 / 6 750 |
| 900 m/s | 4 | non | non | 112 / 4 780 |
| 900 m/s | 1 | non | non | 1 973 / 4 798 |
| 900 m/s | 4 | oui | non | 25 / 4 739 |
| **900 m/s** | **1** | **oui** | **oui** | **0 / 4 748** |
| **900 m/s** | **64 (defaut)** | **oui** | **oui** | **0 / 4 063** |

A 900 m/s avec 1 tuile/frame, le repli est sollicite sur **1 969 frames sur
4 748** et ne manque jamais sa cible.

**Vol en parapente REEL** (physique du jeu, decollage, 12 virages, atterrissage
naturel contre le relief) sur la zone de 25 km2, sous validation de
synchronisation : **0 trou sur 25 501 frames**, 0 hazard, 0 `[Validation]`.

### FPS/RAM : le FPS n'a pas bouge, et c'est normal

| Scene, a l'arret | Avant | Apres | RAM avant | RAM apres |
|---|---:|---:|---:|---:|
| Zone 25 km2 a 1 m | 312 | 292 | 332,2 Mo | 332,3 Mo |
| Dalle de Phase 7 | 218 | 216 | 128,8 Mo | 151,2 Mo |
| Terrain procedural | 267 | 263 | 106,9 Mo | 109,9 Mo |
| Zone 1 m `--no-lod` | 9 | 9 | 5 600 Mo | 5 637 Mo |

**Les ecarts de FPS sont du bruit**, et un temoin le prouve sur le MEME binaire :
a l'arret aucun transfert n'a lieu, donc les reglages de streaming ne peuvent
rien changer - reglages Phase 10 **285 FPS** contre reglages Phase 12 **292 FPS**,
et sous survol a 60 m/s **422** contre **418**. Cinq runs consecutifs de la meme
configuration donnent 278/280/292/293/295 : le bruit de cette machine vaut ~5 %.
Le verrou leve ne se manifestait pas en FPS moyen mais en **trous visuels**.

La RAM, elle, a bouge et c'est explicable : +22,4 Mo sur la dalle de Phase 7
(21 tuiles epinglees + les staging qui coexistent au lieu de se succeder), zone
25 km2 inchangee, `--no-lod` +0,7 % grace au garde-fou.

### Controle visuel

Avec la vue EPINGLEE (`--look`), zone 25 km2, vegetation coupee : reglages Phase 12
contre reglages Phase 10 = **45 pixels differents sur 921 600**, soit MOINS que le
temoin de la meme configuration relancee deux fois (60 pixels). Meme nombre de
tuiles dessinees (66/66) et de triangles (540 672).

**Piege a connaitre** : sans `--look`, trois captures de la meme scene ont donne
38,2 / 38,3 / 32,9 % de ciel, donc trois cadrages differents, et une comparaison
pixel a pixel sans valeur (2,4 % d'ecart, pur artefact). `--lock-input` seul n'a
pas suffi. **Pour toute comparaison d'image, epingler `--look`.**

### Options ajoutees

`--tile-budget=N` (defaut 64 ; 4 = plafond Phase 10), `--no-tile-fallback`
(comportement Phase 10), `--flythrough=V[,H]` (survol scripte a V m/s, H m
au-dessus du relief, virage de 90 deg au bord).

### Non-regression

**280 assertions headless** (268 avant, +12) ; **`run_vulkan_check.ps1` : 4/4 du
premier coup**, zero validation ; **traversee de 20 km** sur la zone 25 km2, zero
validation, code de sortie 0 (et **l'incident de sortie lente de la Phase 10 ne
s'est pas reproduit** - toujours pas explique, toujours pas reproduit) ;
`--dump-world` 16 AIM sur 16 ; `cube`, `icosphere.obj`, `--no-lod`, procedural,
dalle de Phase 7 intacts. Le decouplage
rendu/collision de la Phase 10 n'est pas touche : le repli ne concerne que ce qui
est DESSINE.

### Ce qui reste a ameliorer

1. **Le maillage d'une tuile reste synchrone sur le thread de rendu** (~385 Ko a
   produire par tuile). C'est desormais LE cout du streaming : le prochain verrou
   est un thread de maillage, plus le transfert.
2. La branche "queue de transfert dediee" n'est pas ecrite (voir plus haut).
3. Le repli degrade la resolution sur toute l'emprise de l'ancetre, pas seulement
   sur la tuile manquante : une L4 manquante fait basculer ~2 km de terrain en L5
   le temps d'une frame ou deux. Compromis assume.
4. Une fissure fine reste possible au bord d'un ancetre de repli, et **aucun test
   automatique ne cherche une fissure dans l'IMAGE** (limite ouverte depuis la
   Phase 10).
5. Le cadrage de `screenshot_scene.ps1` n'est pas reproductible sans `--look`,
   malgre `--lock-input` : a comprendre.

## Phase 13 (2026-08-22) : physique du velo

Detail complet dans `NOTES-PHASE-VELO.md`. Troisieme activite, meme intention que
le parapente : un modele simplifie mais chiffre et testable.

Nouveau : `src/world/Bike.{hpp,cpp}`. Comme `Paraglider`, la classe ne connait ni
Vulkan ni la `Camera` ; elle consomme `core/HeightSampler` (altitude et pente) et
`core/CollisionSolver` (obstacles), donc elle est **executee** dans les tests
headless.

### Le modele en une ligne

Somme d'accelerations integree a chaque pas - pesanteur le long de la pente,
pedalage decroissant avec la vitesse, frein manuel, resistances - donc **aucun
changement instantane de vitesse**. La verticale n'est jamais une variable
d'etat : `position.y` est relue de `heightAt()` a chaque pas, le velo ne decolle
donc pas d'une bosse par construction.

| pente | pedalage a fond | roue libre |
|---:|---:|---:|
| plat | 16,9 km/h | 0 |
| +10 % | 10,0 km/h | 0 |
| +25 % | **cale** | 0 |
| -20 % | 37,4 km/h | 37,4 km/h |
| -30 % | 47,2 km/h | 47,2 km/h |
| -40 % | 50,0 km/h (plafond) | 50,0 km/h |

**Pente maximale franchissable : 23,6 %** ; au-dela le velo cale et **ne recule
pas**. Braquage a rayon fini et CROISSANT avec la vitesse (`R = v²/4,0`, soit 25 m
a 10 m/s, 6,3 m a 5 m/s), plafonne a 140 deg/s a l'arret pour pouvoir replacer la
machine. Collision : **`moveBody()`, le meme point d'entree que le joueur a
pied** - rien n'est reecrit ; un choc coute simplement de la vitesse.

### Touche B, camera Ride

**B** enfourche le velo depuis la MARCHE et en descend. Choisie parce qu'elle
occupe la meme position physique en AZERTY et en QWERTY (contrairement a W/A qui
donnent Z/Q) et qu'elle est voisine de V, le parapente. En selle : **Z** pedale,
**S** freine, **Q/D** tournent ; E/R/C sont desactives, comme en vol.

Nouveau `CameraMode::Ride` - meme contrat que `Glide` (camera passive,
`setChasePose()`), mais un mode distinct pour que les journaux disent lequel est
actif. Une contrainte que le parapente n'avait pas : **garde au sol minimale de
1,2 m pour l'oeil**, sinon en descente la camera recule dans la pente et passe
sous la surface.

### LES DEUX DEFAUTS QUE SEUL LE RELIEF REEL A REVELES

Aucun des deux n'etait visible sur un plan incline synthetique.

1. **`slopeSampleStep` a 1 m (le pas de la heightmap) ne marche pas.** Sur un
   versant reel qui descend a 26,7 %, les differences finies a +/-1 m donnent
   **8 % de micro-sections en CONTRE-pente, avec des pics a +22 %** - au-dela du
   franchissable, donc le velo s'immobilisait definitivement en pleine descente.
   A +/-2 m (base de 4 m) : 1,6 % et un pic a +1,1 %, **pente moyenne inchangee**
   (-26,9 % contre -26,8 %). Justification physique : un DTM LiDAR a 1 m porte du
   bruit decimetrique, et une machine de 1,1 m d'empattement lancee a 20 km/h ne
   sent pas un creux d'un metre.
2. **Un seuil d'arret applique inconditionnellement empeche tout DEMARRAGE.**
   `kSpeedEpsilon` (2 cm/s) ecretait la vitesse apres chaque integration, donc
   toute acceleration inferieure a 1,2 m/s² a 60 images/s etait annulee a chaque
   frame : le velo restait cloue a 0 km/h des 10 % de montee et ne pouvait meme
   pas descendre une pente de 10 % en roue libre. C'est un seuil d'**arret** : il
   ne s'applique plus que si le bilan des accelerations est negatif.

### Scenario E : ce qu'il a fallu pour qu'il prouve quelque chose

Le harnais ne sait pas tourner la camera, donc le velo part forcement au cap
initial : lance n'importe ou il roulerait sur du plat et le run serait **vert
sans rien montrer**. Quatre versions ont ete necessaires ; les trois premieres
ont echoue, et la troisieme sur une **erreur de raisonnement** qui merite d'etre
retenue : *faire demi-tour ne redonne pas la pente inverse*. Le terrain de
l'autre cote du point de depart est un AUTRE terrain - sur la croupe essayee, le
nord descendait a -11 % et le sud descendait AUSSI, a -36 %.

La version retenue evalue la pente le long de **chaque cap** (differences
centrees a +/-2 m, comme le fait `Bike`) sur toute la dalle, et prend un point
qui offre les deux : `--start=907875,6439450`, **montee cap 220 deg** (+14,8 %
sur 90 m, pire section +16,8 %, donc aucune section infranchissable) et
**descente cap 40 deg** (-25,0 % sur 170 m). La montee se fait en premier, la
descente gagne ainsi l'elan du denivele grimpe. Le **demi-tour est ASSERVI** sur
le cap que le jeu journalise (courtes rafales, frein tire), pas minute : le temps
vu par le jeu n'est pas celui du `Start-Sleep`.

Resultat, **deux runs consecutifs quasi identiques** : 50,0 km/h au pic en
descente (31,4 de moyenne sur 39 releves), montee a 5,4 km/h de moyenne sur
**20 releves en mouvement contre 2 calages**, 4,4 m d'altitude reellement gagnes
en pedalant, **96,9 m de denivele suivi**, pentes locales jusqu'a -71 % sans
divergence ni NaN, zero `[Validation]`.

### Ce qui reste a ameliorer

1. **La pente franchissable de 23,6 % est basse pour ce massif.** En dessous le
   velo monte proprement (20 releves en mouvement contre 2 calages sur le versant
   a +14,8 % du scenario E) ; au-dessus il s'immobilise net, et les versants
   depouilles ont des devers moyens de 25 a 29 %. Une premiere version du
   scenario, posee sur un versant a 27 %, donnait 41 calages contre 3 releves en
   mouvement. Le modele suppose qu'on ZIGZAGUE (il lit la pente le long du cap,
   donc traverser en biais reduit reellement l'effort). Avant de toucher a
   `pedalAcceleration`, **decider si monter droit dans un versant a 27 % doit
   etre possible** - a pied non plus, ca ne l'est pas.
2. **La vitesse limite theorique n'est presque jamais atteinte sur du relief
   MESURE** : un vrai versant est fait de terrasses. C'est un resultat, pas un
   defaut.
3. Marqueur = cube (mais oriente correctement en cap/tangage/roulis, ce qui rend
   l'assiette lisible a l'ecran). Vrai mesh a venir avec l'habillage.
4. Pas de suspension ni de derapage : le `dragCoefficient` groupe tout, ce qui en
   fait le parametre le plus arbitraire du modele.
5. La collision ne concerne toujours que les 16 arbres de `Forest` - le velo rend
   cette limite de la Phase 11 plus voyante, puisqu'on couvre du terrain vite.
6. Aucun controle d'IMAGE sur la vue a la troisieme personne du velo.

### Hors scope, assume

Pas de degats ni de chute, pas d'usure, pas de velo ramassable ou fabricable.

### Non-regression

**334 assertions headless** (280 avant, +54) ; **`run_vulkan_check.ps1` : 5/5,
deux runs consecutifs**, zero validation ; scenarios A a D inchanges et verts.
Aucun chemin de rendu touche (le marqueur est un `DrawItem` de plus sur le
materiau `Object` existant). `Camera` : une valeur d'enum ajoutee (`Ride`) et
deux branches qui la traitent comme `Glide`, rien d'autre. Scenes annexes
relancees une a une (`cube`, procedural, dalle de Phase 7 en `--no-lod`, zone
25 km2 a 1 m) : **toutes a 0 validation / 0 erreur / fermeture propre** ;
`--dump-world` retrouve **16 AIM sur 16**.

## Phase 14 (2026-08-22) : physique de l'escalade

Detail complet dans `NOTES-PHASE-ESCALADE.md`. **Quatrieme et derniere activite
du cadrage** : aucune des quatre n'est vide desormais.

Nouveau : `src/world/Climber.{hpp,cpp}` et `tools/find_climb_walls.py`. Comme
`Paraglider` et `Bike`, la classe ne connait ni Vulkan ni la `Camera` ; elle
consomme `core/HeightSampler` (surface et pente) et `core/CollisionSolver`
(obstacles), donc elle est **executee** dans les tests headless.

**Aucun chemin de rendu n'est touche** - pas un `DrawItem` de plus. C'est la
seule des quatre activites dans ce cas : elle est a la premiere personne, il n'y
a meme pas de marqueur a dessiner. La synchronisation GPU n'ayant pas bouge, la
validation de synchronisation n'avait rien de nouveau a regarder.

### Le seuil de paroi : 100 % (45 deg), et c'est un CHOIX

`tools/find_climb_walls.py` (nouveau) a depouille la zone de jeu entiere
(24 970 009 echantillons, base de 4 m comme le velo) :

| | p50 | p75 | p90 | p95 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|
| pente | 47,7 % | 65,7 % | 92,4 % | 129,4 % | 342,5 % | 5018,2 % |

**Ce massif est raide partout - la pente MEDIANE vaut 47,7 % - et la
distribution est un CONTINUUM sans aucune bimodalite.** Il n'existe donc AUCUN
creux ou poser le seuil : c'est une decision de conception, pas une mesure, et
il faut le presenter ainsi. Les trois arguments retenus : 45 deg est la borne
basse conventionnelle du terrain d'escalade ; c'est nettement au-dessus de
l'angle de repos des materiaux meubles (35-40 deg), donc une face a 45 deg dans
ce massif est du rocher nu ; et ca retient **8,4 %** de la zone - assez pour que
grimper serve, assez rare pour que marcher reste le defaut (a 70 % on en
retiendrait 20,7 %, tout versant deviendrait une paroi).

**Hysteresis obligatoire** : on entre a 100 %, on ressort a 60 %. Sans cet ecart,
une paroi reelle - qui oscille autour de son seuil d'un metre a l'autre - ferait
clignoter le mode a chaque image.

### Le modele, en une phrase

Le grimpeur reste **exactement sur la surface** et s'y deplace le long de deux
tangentes : ligne de plus grande pente (monter/desescalader) et ligne de niveau
(traverser). Meme invariant que le velo - *la verticale n'est jamais une variable
d'etat* - et c'est ce qui rend l'escalade compatible avec une heightmap **sans
rien ajouter au moteur**.

La projection `L / sqrt(1 + pente^2)` produit toute seule le comportement voulu,
sans aucun cas particulier : sur une paroi a 87 deg, un metre parcouru le long de
la surface donne **0,999 m d'altitude et 0,050 m a l'horizontale**.

Vitesses : 1,0 m/s en ascension (14 fois plus lent que la marche du moteur),
0,6 m/s en traversee. Collision : `moveBody()`, le meme point d'entree unique que
le joueur a pied et le velo.

### DEUX PIEGES QUE SEUL LE RELIEF REEL A REVELES

1. **Un seuil de pente ne suffit pas a trouver une paroi.** Sur du relief mesure,
   le joueur qui s'approche s'accroche au PREMIER point raide a portee, qui est
   souvent un ressaut isole de deux metres devant la paroi : l'escalade se
   terminait par un "SOMMET" apres **0,3 m gagnes en 1,2 s**. La base de 4 m
   elimine le bruit decimetrique mais pas un accident de terrain REEL. D'ou
   `wallRise()` : simuler 6 m de progression et exiger 4 m de gain, c'est-a-dire
   demander *"grimper ici mene-t-il quelque part ?"* plutot que *"est-ce raide
   ici ?"*.
2. **LA MEME ERREUR DE RAISONNEMENT QU'EN PHASE 13.** Le premier test sur relief
   reel visait le centre d'une paroi et l'abordait plein est. Or **cette paroi
   DESCEND vers l'est** : le test approchait par le SOMMET et le grimpeur
   remontait correctement les 3,9 m qu'il venait de descendre. *Savoir qu'il y a
   de la pente quelque part ne dit pas PAR OU l'aborder.* D'ou le mode
   `--routes` de l'outil, qui evalue le relief **le long de chaque cap** depuis
   un pied marchable - exactement ce qu'avait fallu faire pour le scenario velo.

Corollaire a retenir : **le grimpeur suit la ligne de plus grande pente, qui
s'incurve ; l'outil mesure le long d'un cap FIXE.** Les deux divergent (19,5 m
reellement gagnes contre 21,4 m projetes). Ne jamais recopier le chiffre de
l'outil dans une assertion.

### La jauge d'energie

Une seule jauge dans [0, 1], isolee. **Les couts sont PAR METRE, pas par
seconde** : monter 40 m coute pareil vite ou lentement (comportement correct), et
la jauge devient independante du nombre d'images par seconde.

| montee | descente | horizontal | suspendu | recuperation (AU SOL seulement) |
|---:|---:|---:|---:|---:|
| 0,016 /m | 0,005 /m | 0,005 /m | 0,004 /s | 0,05 /s |

Descendre coute 3 fois moins cher que monter : **choix de JEU, pas de physique** -
sans porte de sortie, s'engager trop haut ne laisserait que la chute.

**Le chiffre qui donne son sens a l'activite** : une jauge pleine vaut **61,5 m**
d'ascension (mesure sur paroi a 87 deg). Les parois abordables de la zone font
30 a 44 m -> elles passent. Les grandes falaises font **250 a 276 m** -> hors de
portee. Ce n'est pas un reglage rate, c'est la limite qui fait exister l'activite.

### Trois sorties, une seule reinitialisation de position

`ToppedOut` (on reste OU L'ON EST - toute la recompense), `Released` et
`Exhausted` (retour au pied). Les deux dernieres partagent **exactement le meme
code** : lacher une paroi, c'est se retrouver au pied, qu'on l'ait decide ou non.
Le point de repli est la position AU MOMENT DE L'ENGAGEMENT, dont on sait qu'elle
etait marchable et libre - la calculer apres coup serait chercher une position
libre sans garantie d'en trouver une. Aucun degat (hors scope).

Le sommet est teste **AVANT** l'epuisement : poser la main sur la vire avec sa
derniere goutte d'energie, c'est topper, pas tomber. Fige par test.

Confirmation du sommet sur **DEUX** points (l'accroche et 3 m plus loin) : un
seul point sortirait du mode escalade sur la moindre dalle horizontale au milieu
d'une face, donc EN PLEIN VIDE. Une vire de 1 m ne stoppe pas, une vire de 6 m
si - et c'est correct, 6 m de terrain marchable, on y tient debout.

### Camera : premiere personne, regard LIBRE

Nouveau `CameraMode::Climb`, **contrat MIXTE, le seul du projet** : la position
vient de `Climber` (comme `Glide`/`Ride`), mais **l'orientation reste au joueur** -
la souris marche, alors qu'elle est coupee en vol et a velo.

Pourquoi c'est coherent et pas une exception : **les commandes de l'escalade sont
relatives a la PAROI, pas au regard** (monter suit la ligne de plus grande pente,
traverser la ligne de niveau, toutes deux imposees par le terrain). Le regard n'a
donc aucune influence sur le deplacement, et il redevient libre sans rien casser -
c'est meme necessaire pour lire la suite de la voie et juger la hauteur.

Premiere personne parce que l'information utile est la surface devant le visage
(un avatar la masquerait), et parce qu'une chase-cam n'a pas de bonne place sur
une paroi : derriere le grimpeur c'est le vide, et des que la face s'incurve
l'oeil traverse le rocher. Le velo avait deja du rustiner ce probleme
(`kRideChaseMinClearance`) sur du terrain bien plus doux.

**L'oeil n'a PAS la convention des trois autres activites** : le grimpeur est
CONTRE la surface, l'oeil est donc a `accroche + normale sortante x 0,6 m + 0,3 m`
et non a 1,7 m au-dessus du sol. Le decalage se fait vers l'AVAL, ce qui garantit
que l'oeil est hors du relief - verifie sur 200 pas et sur l'ascension LiDAR reelle.

### Touche X, et la rangee des activites

**X** s'engage / lache (puis Z monter, S desescalader, Q/D traverser). Meme
raisonnement que le B du velo : X occupe la MEME position physique en AZERTY et en
QWERTY. Les quatre activites et les deux actions d'outillage forment desormais une
rangee contigue sous la main gauche : **X** escalade, **C** percussion,
**V** parapente, **B** velo.

### Scenario F : ce qu'il a fallu

Le harnais ne peut pas tourner la camera, donc le joueur regarde forcement plein
NORD. Un balayage de la dalle entiere n'a trouve que **TROIS** points reunissant
les trois conditions (depart marchable + paroi grimpable a moins de 2 m plein nord
+ ascension gagnant au moins 15 m). Retenu : **E 907616 / N 6439312**.

A noter : **le scenario F n'a RIEN a asservir**, la ou le scenario E avait exige
un asservissement de cap (`Turn-BikeTo`). C'est une consequence directe du choix
de commandes relatives a la paroi.

Resultat : engagement, montee, traversee des deux cotes, desescalade, **lachage**,
repos, **re-engagement**, **SOMMET a 41,6 m** avec **14 % de jauge restante**,
75 releves de telemetrie, zero validation.

**La CHUTE par epuisement n'est PAS exercee sous Vulkan** : elle demanderait une
paroi de plus de 62 m et il n'en existe pas d'abordable plein nord sur cette
dalle. Couverte par les tests headless jusqu'a la position de repli exacte.

### Reproductibilite : l'escalade est deterministe, le velo ne l'est pas

Deux runs consecutifs, meme binaire : le scenario F rend **41,6 m / 14 % de jauge
/ 75 releves** les DEUX fois, quand le scenario E varie de 50,0 a 44,1 km/h et de
96,8 a 67,7 m de denivele suivi. Raison : le velo INTEGRE une vitesse, donc le
moindre decalage de timing du harnais s'accumule en distance et change le terrain
rencontre ; l'escalade n'a aucune quantite de mouvement (la vitesse est imposee)
et ses commandes sont relatives a la paroi, donc un decalage de timing ne change
que la duree, jamais le chemin. Les assertions du scenario F peuvent donc etre
serrees sans devenir instables.

### Non-regression : complete

**425 assertions headless** (334 avant, +91) ; **`run_vulkan_check.ps1` : 6/6, du
premier coup, deux runs consecutifs**, zero validation ; scenarios A a E inchanges
et verts ; scenes annexes relancees une a une (`cube`, procedural, dalle de
Phase 7, dalle de Phase 7 en `--no-lod`, zone 25 km2 a 1 m, zone 25 km2 sans
vegetation) : **toutes a 0 validation / 0 ERROR / 0 WARN / fermeture propre** ;
`--dump-world` retrouve **16 AIM sur 16**. `Camera` : une valeur d'enum ajoutee et
un booleen `orientationOnly` dans `update()`, sans duplication du bloc souris.

### Ce qui reste a ameliorer

1. **Les grandes falaises sont inaccessibles, et pas d'abord a cause de
   l'energie** : les six meilleures par denivele (250 a 276 m) ont **0 a 2 % de
   pied marchable**. On ne peut litteralement pas les atteindre a pied. Les
   debloquer demande des RELAIS *et* un moyen d'approche - ce qui pointe vers une
   interaction parapente/escalade qui n'existe pas.
2. **Aucune difficulte locale** : une paroi a 105 % se grimpe comme une a 2000 %.
   Faire dependre vitesse et cout de la pente locale serait l'amelioration a cout
   nul, bien avant un vrai systeme de prises.
3. Le seuil de 100 % est un choix : a revoir en premier si l'escalade se revele
   trop presente ou trop rare a l'usage.
4. La desescalade n'a aucun risque, ce qui affaiblit la tension de la jauge.
5. **Rien ne relie l'escalade a la marche en dehors de la touche X** : la marche
   n'a toujours AUCUNE limite de pente, donc un versant a 90 % se monte encore a
   pied. Tant que ce sera vrai, l'escalade restera facultative.
6. Aucun controle d'IMAGE sur la vue en escalade.

## Phase 15 (2026-08-23) : editeur de niveau interne

Detail complet dans `NOTES-PHASE15.md`. Premier OUTIL du projet, par opposition
aux quatorze phases precedentes qui construisaient le jeu. Usage developpeur :
aucune interface graphique, le catalogue est une liste texte dans le journal.

**Le point qui compte** : ce qui est pose fait partie du MONDE, pas de l'outil.
Une partie normale - sans `--editor` - charge la scene, la dessine et s'y cogne.

### Nouveau : src/editor/ (5 unites) + 2 en-tetes dans core/

`EditorAssets` (5 modeles generes en code), `AssetCatalog` (builtins + scan de
`assets/editor/`), `PlacedObject`, `EditorScene` (liste + visee + collision +
annulation), `EditorController` (machine a etats), `SceneFile` (format `.vcsc`).
Plus `core/TerrainRay.hpp` (rayon / surface) et `core/CompositeCollisionSolver.hpp`.

**Aucun ne connait Vulkan** : tout l'editeur SAUF LE DESSIN est execute dans les
tests headless. Meme decoupage que les quatre activites.

`EditorController` ne lit pas le clavier - il recoit un `EditorInput` deja
depouille par `main.cpp`. La vraie raison n'est pas la proprete : le harnais
automatise ne peut PAS piloter la souris (GLFW lit le curseur systeme), donc un
controleur qui lirait lui-meme les entrees ne serait testable qu'a la main.

### La collision : l'interface de la Phase 6 sert enfin a quelque chose

Le monde n'avait qu'UNE source d'obstacles (`Forest`). `CompositeCollisionSolver`
implemente le meme contrat et le delegue a une liste, repoussements CHAINES.
**Ni `Camera`, ni `Bike`, ni `Climber` n'ont ete modifies** : seul l'objet qu'on
leur passe a change.

Empreinte = cylindre vertical CONTENANT, deduite de la boite englobante du mesh
(un obstacle trop gros se contourne, un obstacle trop petit se traverse par les
coins). Drapeau `solide` par OBJET, parce que le cylindre est faux des qu'un
asset a un interieur - l'abri a une porte, mais son cylindre est plein.

### LA MESURE : collision prouvee par TEMOIN NEGATIF, et chiffre PREDIT

Trois processus, meme binaire, meme depart, meme duree de touche maintenue ;
seule la presence de la scene change.

| | distance parcourue vers le nord |
|---|---:|
| avec un rocher pose devant | **0,69 m** |
| temoin, `--no-scene` | **21,17 m** |

Et le chiffre etait PREVISIBLE : rocher pose a 2,51 m, rayon de collision
1,47 m, rayon du corps 0,35 m -> arret attendu a 2,51 - 1,82 = **0,69 m**. C'est
ce qui separe "ca a l'air de bloquer" de "ca bloque la ou le modele le dit".

### Le format `.vcsc` : deux decisions, une seule intention

Fichier TEXTE, une directive par ligne (`version` / `terrain` / `crs` / `object`),
range dans `assets/scenes/` - donc VERSIONNE, contrairement a `data/`.

1. **Coordonnees PROJETEES (Lambert 93), pas locales.** Le repere du moteur est
   centre sur le terrain CHARGE : une position locale ne veut rien dire sans
   savoir quelle emprise l'etait. Cette zone existe deja sous quatre emprises.
2. **Altitude RELATIVE au terrain (`dz`), pas absolue.** La surface a 2 m n'est
   pas celle a 1 m, et le repli sur l'ancetre (Phase 12) peut afficher une tuile
   plus grossiere une image durant. `dz = 0` = "pose au sol", vrai partout.

=> une scene composee sur la dalle isolee se recharge au bon endroit du massif
sur la zone de 25 km2. **Verifie par test** : rechargee au pas de 2 m, les objets
restent au sol au demi-centimetre pres et celui qui etait souleve de 2,5 m garde
ses 2,5 m.

Robustesse : directive inconnue ignoree (pas fatale), ligne `object` fautive
comptee et sautee, ecriture par temporaire renomme, locale C forcee (en locale
francaise `907512.250` se relirait `907512`).

`assets/scenes/demo_phase15.vcsc` est ECRITE A LA MAIN : c'est ce qui prouve que
le format est editable sans lancer le jeu.

### Le fantome est TEINTE, pas transparent

Trois etats du meme maillage (pose / fantome / selectionne) se distinguent par
une teinte poussee en push constant (`params.x` = force, `yzw` = couleur), lue
par `shaders/cube.frag`. Pas de melange alpha : il imposerait un tri en
profondeur. **Meme choix qu'en Phase 11** (les arbres fondent par
retrecissement, l'herbe est opaque). **Force nulle = rendu bit a bit identique a
l'avant-Phase 15**, la branche n'est meme pas prise - c'est ce qui autorisait a
toucher au shader du materiau `Object`.

### CE QUE L'ESSAI SOUS VULKAN A CORRIGE

La pose SELECTIONNAIT l'objet pose, par analogie avec les editeurs 3D. Les
reglages visant la selection en priorite (SAISIE > SELECTION > PINCEAU), poser un
objet detournait donc silencieusement les fleches du PINCEAU vers l'objet pose :
"regler une fois, poser dix blocs alignes" - le geste le plus courant de l'outil -
devenait impossible sans deselectionner entre chaque. **La pose ne selectionne
plus**, fige par test.

Au passage, la ligne d'etat n'affichait que le cap du PINCEAU : tourner un objet
selectionne affichait `cap 0 deg` pendant qu'il tournait. Elle decrit desormais
la cible reelle des reglages.

### Ancrage n'est pas centrage

Convention des assets : dimensions ABSOLUES en metres, pied a `y = 0`, ancrage
horizontal a l'origine. Mais pas forcement au centre de la boite : le poteau de
balisage a son MAT a l'origine, sa plaque debordant. Planter un panneau, c'est
planter le mat au point vise.

Un asset charge d'un FICHIER ne dit rien de son intention -> `normalizeAssetMesh`
le recentre de force sur sa boite. Defaut le moins mauvais, pas regle generale.
La propriete que tous respectent (et que le test verifie) est plus faible :
**l'ancrage tombe DANS l'empreinte horizontale**, jamais a cote.

*(Le premier test exigeait un centrage exact et a echoue sur 3 assets sur 5.
C'est la convention qui etait fausse, pas la geometrie.)*

Le bruit des rochers est un HACHAGE de l'indice de sommet, pas un `mt19937` -
meme lecon qu'en Phase 11. Ca compte ici parce que la COLLISION est deduite de la
boite englobante : un mesh qui varierait donnerait une collision qui varie.

### Touches

**F2** entre/sort. Le mode est EXCLUSIF (aucune touche de jeu n'est lue, et
reciproquement), ce qui autorise a reprendre des lettres deja prises. A chaque
bascule, TOUS les fronts descendants sont resynchronises - sinon entrer avec `E`
enfonce selectionnerait, en sortir avec `E` enfonce abattrait un arbre.

Vol ZQSD + espace/maj, molette = vitesse (2 a 600 m/s). **Clic gauche ou INSER**
pose, `E` selectionne, `G` saisit/repose, `SUPPR` supprime, `ECHAP` abandonne,
`TAB`/`PAGE` parcourent le catalogue, `1..9` rang direct, fleches = cap et
echelle, `DEBUT`/`FIN` = hauteur, `K` solide/traversable, `CTRL` pas fin,
`RETOUR ARRIERE` annule, `F5` sauve, `F9` recharge, `L` liste.

Touches choisies parmi celles qui occupent la MEME position physique en AZERTY et
en QWERTY (fleches, pave de navigation, fonctions, chiffres) ; seules lettres
utilisees : `E`, `G`, `K`.

`INSER` double le clic gauche parce que le harnais ne pilote que le clavier :
sans elle, la pose serait la seule fonction de l'editeur qu'aucun test ne
pourrait exercer sous Vulkan.

### Options ajoutees

`--editor`, `--scene=chemin.vcsc`, `--no-scene`, `--editor-assets=rep`.
**`--no-scene` reproduit exactement le comportement d'avant la Phase 15** : c'est
le temoin de toute mesure avant/apres.

### Ce qui reste a ameliorer (par ordre d'importance)

1. **glTF : NON FAIT.** `AssetCatalog::supportedExtensions()` ne connait que
   `.obj`. Un loader glTF honnete (JSON + conteneur `.glb` + accesseurs +
   hierarchie de noeuds) fait 600 a 800 lignes : une session a part, dont le
   risque etait reel alors que la consigne exigeait zero validation et une
   non-regression complete. Choix assume de livrer l'editeur COMPLET. Cout : les
   bibliotheques libres distribuent surtout du glTF, il faut convertir avant.
   **`supportedExtensions()` est le seul point d'extension** - ancrage,
   collision, catalogue, placement et serialisation sont deja agnostiques.
2. **Collision par cylindre englobant** : fausse pour tout objet a interieur.
3. **Aucun frustum culling sur les objets poses** : un `DrawItem` par objet et
   par image. Sans consequence a l'echelle d'une scene d'editeur, `Frustum`
   existe deja et se brancherait en quelques lignes.
4. Catalogue fige au demarrage (deposer un `.obj` demande de relancer).
5. Pas de sauvegarde automatique : `F5` est explicite, sortir du mode n'ecrit rien.
6. Aucune aide au placement : ni grille, ni magnetisme, ni alignement sur la
   normale du terrain (`terrainNormalAt()` est pourtant deja ecrit et teste).
7. Pas de rotation libre ni d'echelle non uniforme (contrat de `DrawItem` :
   `mat3(model)` comme normal matrix).
8. Pas de selection multiple ni de copier-coller.
9. Les objets poses n'interagissent avec rien (pas d'abattage, pas d'ombre), et
   l'editeur ne peut pas toucher aux arbres de `Forest` ni aux pierres de `Ground`.
10. Rien ne previent qu'on empile deux objets au meme endroit.

### Suite du 2026-08-23 : selection au clavier sans viser (F3/F4)

Detail complet dans `NOTES-PHASE15.md` section 12. Verification prealable que
la session tenait toujours : build propre, 650/650 assertions headless, puis
`run_vulkan_check.ps1` complet - zero validation, scenario G vert ; seul le
scenario F a echoue sur la fermeture propre, rejoue seul aussitot apres il
passe (flakiness `PostMessage` deja connue, pas une regression).

Choix motive parmi les trois pistes proposees (annulation deja livree, glTF,
ergonomie) : le glTF reste la plus grosse limite documentee, mais c'est
justement pourquoi la session initiale l'avait ecarte - 600-800 lignes neuves,
un risque reel face a l'exigence de zero regression, et le choix assume de
livrer un editeur COMPLET plutot qu'un editeur et un loader a moitie. Le
retenter dans une suite courte aurait repris ce risque sans le temps de le
tester aussi rigoureusement que le reste. L'ergonomie clavier, elle, correspond
a une lacune precise et deja nommee (§9) : `pickPressed` (touche E) exige que
la visee touche l'objet, donc retrouver un objet pose hors du champ courant
n'avait qu'une voie, y revenir a la souris.

**F3** (suivant) / **F4** (precedent) parcourent desormais la liste des objets
poses independamment de la visee, avec la meme garde que la selection visee
(ignore pendant qu'un objet est en main). 9 assertions headless de plus
(650 -> **659**), et le harnais Vulkan (`run_vulkan_check.ps1`, scenario G1)
envoie desormais F3/F4 par `PostMessage` et EXIGE exactement 3 selections
journalisees pour les 3 touches envoyees - un front descendant perdu ferait
donc echouer le run. Aucun fichier de rendu touche.

## Phase 16 (2026-08-24) : besoins primaires du joueur

Detail complet dans **NOTES-PHASE16.md**. Resume de ce qu'il faut savoir sans
ouvrir ce fichier.

### Quatre jauges, aucune ne tue

`src/world/Needs.hpp/.cpp` - faim, soif, fatigue, temperature, dans [0, 1],
**1 = tout va bien pour les quatre** (d'ou le libelle "forme" a l'affichage :
"fatigue 100 %" se lirait a l'envers). Meme moule que Paraglider/Bike/Climber :
ni Vulkan, ni GLFW, ni Camera. La classe recoit un pas de temps et un CONTEXTE
(activite, altitude, heure, exposition du versant) et rend des jauges et des
COEFFICIENTS ; c'est `main.cpp` qui applique.

**Les taux sont en SECONDES REELLES**, pas en heures simulees - c'est le point
le plus facile a confondre. Depuis des jauges pleines, en marchant sans arret :
forme vide en 1 h 30, soif en 1 h 05, faim en 2 h 06. A l'arret : forme
**remonte** (pleine en 20 min), soif 2 h 30, faim 4 h. A cinq minutes, tout est
encore au-dessus de 90 %.

Une seule table d'effort commande tout (marche = 1) : **repos 0 / marche 1 /
velo 1,8 / escalade 4 / parapente 0,3**. L'escalade fatigue donc exactement
quatre fois plus vite que la marche. Elle a deja sa jauge courte
(`Climber::stamina`) ; la fatigue est l'ENVELOPPE LONGUE - elle n'interdit pas
une paroi, elle interdit d'en enchainer dix.

**Le repos exige l'immobilite REELLE** : `main.cpp` lit le deplacement effectif
de la camera entre deux images, pas les touches. Un joueur qui pousse contre un
rocher tient sa touche, n'avance pas, et ne recupere pas.

### La temperature relie enfin le relief a l'etat du joueur

`ambiante = 15 C - 6,5 C/km d'altitude + cycle diurne (+/-8 C, max a 15 h)
+ 4 C d'adret pondere par le soleil`. Sur la dalle chargee (420 -> 982 m), ca
fait **3,7 C d'ecart permanent entre le fond de vallee et la crete**, et 16 C
entre 3 h et 15 h : marcher de nuit coute a peu pres autant que marcher 2400 m
plus haut. Le ressenti ajoute la chaleur de l'effort, retire le vent du vol
(-6 C) et l'epuisement (-3 C a forme nulle).

**L'adret/ubac est CALCULE, pas devine** (`terrainExposure`) : +Z va vers le
SUD (convention `Heightmap::toProjected`), donc un versant qui descend vers +Z
est un adret. Lu sur une base de **4 m**, meme valeur et meme raison que
`BikeParams::slopeSampleStep`, et **pondere par la pente** - sur du plat il n'y a
ni adret ni ubac, sans quoi une plaine bruitee donnerait des expositions
extremes et aleatoires d'un metre a l'autre.

### Les consequences sont appliquees aux ENTREES, pas dans les modeles

Un SEUL coefficient (`Needs::movementFactor`, produit de quatre penalites,
plancher a **0,35**) multiplie trois choses dans `main.cpp` : la vitesse de
marche, la commande de pedalage, la commande de montee en paroi (desescalade
comprise - sinon on monterait epuise et on redescendrait gratis).

`Bike`, `Climber` et `Paraglider` **n'ont pas ete touches**, et c'est
volontaire : ils ecretent deja leurs entrees dans [0, 1], donc une commande
plus faible y produit exactement ce qu'on veut (moins d'acceleration, vitesse
d'equilibre plus basse, pente franchissable plus faible) sans risquer une
regression sur une physique testee depuis deux phases. Les tests ne verifient
pas le coefficient mais l'EFFET : distance parcourue a velo en une minute,
hauteur gagnee en 20 s d'escalade, avec et sans penalite.

Plancher a 0,35 : au plus mal, le joueur avance encore a plus du tiers de sa
vitesse. Le produit brut des quatre penalites vaudrait 0,175 - une assertion
verifie que le plancher MORD vraiment et n'est pas decoratif.

### Points d'eau : la seule regeneration reelle (touche T)

`src/world/WaterSource.hpp/.cpp`. Le jeu n'a PAS d'eau ; cette fonction cherche
seulement **ou il serait credible de boire**, dans la forme du terrain : les
points a la fois peu pentus et concaves. Balaye une fois au chargement.

**Trois reglages ont du etre corriges apres mesure sur la dalle reelle, ou la
premiere version rendait ZERO point** :

1. **ce massif est raide** - au pas de 50 m, seuls 10 points sur 361 passent
   sous 10 % de pente. Seuil porte a **15 %** ;
2. **le pas de balayage etait trop grossier** - le rayon utile d'un creux vaut
   une vingtaine de metres, un pas de 50 m passe a cote. Ramene a **20 m** ;
3. **quatre sondes ne distinguent pas un talweg d'un col.** Un talweg a deux
   sondes hautes (les versants) et deux basses (amont/aval) : exiger 3 sur 4 ne
   retient que les cuvettes fermees, qui n'existent pas ici ; accepter 2 sur 4
   laisse passer les COLS, qui ont la meme signature et ou il n'y a rien a boire.
   D'ou **HUIT directions, seuil a 5 sur 8** - un talweg a ses deux versants qui
   couvrent chacun plus d'une direction, un col n'en a que quatre.

Resultat sur la dalle du Gua : **2 points d'eau**, le premier en monde
(-75, 285) = E 907425 / N 6439215. Portee pour boire : **4 m** horizontaux.
C'est cette heuristique qui disparaitra le jour ou le reseau hydrographique IGN
sera charge, pas ses appelants.

### Ce qui n'existe PAS, et qu'il faut savoir

- **De quoi manger.** `Needs::eat()` est ecrit et teste mais **n'a aucun
  appelant** : le jeu ne produit rien de comestible. C'est pour ca que la faim
  est de loin le besoin le plus lent. C'est le trou beant du systeme.
- **Un abri.** `NeedsContext::sheltered` existe et est teste, sans appelant non
  plus.
- **Le cycle jour/nuit RENDU.** `GameClock` (facteur 12 : une session d'une
  heure traverse douze heures de jeu) ne pilote que la temperature. Le soleil des
  shaders reste une direction fixe : **il fait toujours jour a l'ecran** alors
  que la temperature, elle, tombe la nuit. Incoherence assumee.
- Le mode EDITEUR **gele les quatre jauges** (outil de developpement).

### Affichage

Titre de la fenetre, comme l'inventaire depuis la Phase 6 :
`faim 87% soif 62% forme 44% chaud 91% - 14:35`. Pourcentages ENTIERS, et le
titre n'est reecrit qu'une fois par seconde (au rythme du compteur de FPS). Le
detail (ambiante, ressenti, coefficient de deplacement) va dans le journal -
c'est cette ligne que depouille le scenario H.

### Deux pieges du harnais, notes parce qu'ils se reproduiront

- le scenario attendait `Boire : pas soif` au point d'eau, en supposant la jauge
  encore exactement pleine. **Elle ne l'est jamais** : deux secondes de jeu l'ont
  deja fait descendre de quelques millioniemes, invisibles a l'affichage entier
  mais suffisants pour que `drink()` reussisse ;
- a 100 s de marche, la forme tombait a 99 % puis 98 % selon les runs - la marge
  tenait a un point d'arrondi. Portee a **160 s** (~3 %).

## Phase 17 (2026-08-25) : premiere empreinte environnementale

Detail complet dans **NOTES-PHASE17.md**. Resume de ce qu'il faut savoir sans
ouvrir ce fichier.

### Le monde garde une trace, et ne le dit jamais

Trois briques neuves, aucune modification de comportement ailleurs :

| Fichier | Role |
|---|---|
| `src/world/Habitat.hpp/.cpp` | la memoire locale : deux compteurs par cellule de 32 m |
| `src/world/Wildlife.hpp/.cpp` | la faune : semis par tirage-rejet, deux etats, fuite |
| `src/world/AnimalMesh.hpp/.cpp` | le cervide bas-poly, genere en code |
| `src/world/GroundCover.hpp` | l'interface par laquelle l'herbe interroge l'etat du sol |

`Forest` n'a PAS ete touchee : c'est `main.cpp` qui inscrit l'abattage dans le
milieu apres un `fell()` reussi. L'abattage est une mecanique de la Phase 5,
l'empreinte qu'il laisse est une lecture de la Phase 17.

### Deux memoires, jamais une seule

**Frequentation** : monte avec la presence (ponderee par le BRUIT de
l'activite), saturee en 60 s de marche sur place, oubliee en 4 min.
**Deforestation** : un cran par arbre, 3 arbres saturent une cellule, 15 min
pour se resorber. Un derangement CESSE quand on part, un prelevement NON.

Le bruit n'est PAS l'effort de la Phase 16 : **repos 0,25 / marche 1 / velo 2 /
escalade 0,6 / parapente 0,15**. L'escalade est l'effort MAXIMAL du joueur et
l'une des activites qui derange le MOINS.

La grille est CREUSE et oublieuse : seules les cellules touchees existent, et
une cellule revenue a neuf est effacee (meme principe que le cache de cellules
de `Vegetation`).

### Consequences, toutes observables en jouant

- au-dela de 0,55 de frequentation, la faune presente PART ; elle ne revient que
  sous 0,35 (hysteresis deliberee, sinon les animaux clignotent sur le seuil) ;
- au-dela de 0,40 de deforestation, plus aucune reapparition ;
- le sol se DENUDE : la densite d'herbe de `Vegetation::gatherGrass` est
  multipliee par le couvert rendu par l'`Habitat` (jusqu'a 0,15 au pire). Au pied
  d'une coupe saturee, plus de la moitie de l'herbe disparait ; elle revient
  integralement en 20 min.

**Le shader terrain n'a PAS ete touche.** Passer les compteurs au GPU aurait
demande un binding, une texture a re-televerser et donc un chantier de
synchronisation Vulkan a part entiere. Faire disparaitre l'herbe donne le meme
signal pour zero objet Vulkan neuf. Contrepartie : **la ou il n'y a pas d'herbe
(lapiaz, au-dessus de 2250 m), une coupe ne se voit pas.**

### DEUX RAYONS REGLES SEPAREMENT QUI NE SE RECOUVRAIENT PAS

La lecon de la session, et elle est generale. `noiseRadius` valait 45 m,
`spawnRadiusMin` valait 90 m. Chacun defendable seul. Ensemble : **aucun animal
ne se trouvait jamais dans le rayon derange**. Run mesure : 24 animaux au depart,
24 apres deux minutes de presence, frequentation a 100 %, ZERO fuite. Le code
etait juste, les tests headless verts, le resultat en jeu nul.

Corrige : `noiseRadius` = 120 m et `spawnRadiusMin` = 55 m (a 90 m, un cervide
de 1,3 m est un point : la faune existait sans qu'on puisse la regarder). Apres
correction : 4 animaux a portee de vue au depart, 1 apres quatre minutes.

**Il faut que `noiseRadius` depasse `spawnRadiusMin`**, sans quoi la deuxieme
mecanique de la phase n'existe pas. Aucun test unitaire ne l'aurait dit : chaque
constante est correcte de son cote.

### Le defaut qui rendait la memoire muette

Premier essai : rien ne montait, `trackedCellCount` a zero. Au premier pas de
temps une cellule ne contient que 0,0003, soit MOINS que l'epsilon d'oubli
(1e-3) : `update` l'effacait a l'image meme ou `notePresence` venait de la creer.
Corrige : une cellule OCCUPEE n'est ni oubliee ni decrue tant que le joueur y est.

### La bonne metrique n'est pas la population

L'effectif total ne bouge pas quand une zone se vide : les animaux s'ECARTENT
puis d'autres apparaissent plus loin. Ce qui se vide, et ce qu'un joueur voit,
c'est le VOISINAGE - d'ou `Wildlife::countWithin`, et le scenario I qui la
depouille. Attendre zero serait faux : la ponderation en `1 - (d/r)^2` tend vers
zero au bord du rayon, donc un residu d'un ou deux animaux a la limite est le
comportement CORRECT du modele.

### Deux effets de bord sur le harnais, a connaitre

**`--no-wildlife`** est ajoute au jeu : il coupe la faune ET la memoire du
milieu, et rend le comportement d'avant la phase. TEMOIN, au meme titre que
`--no-vegetation` - il a servi des cette session.

1. **Le scenario F (escalade) n'avait aucune marge.** La faune coute 6 % de FPS
   (301 contre 320, mesure avec le temoin). La trajectoire du grimpeur depend du
   pas de temps - sa commande est projetee sur la ligne de plus grande pente
   LOCALE a chaque image - donc 6 % de FPS suffisent a changer le chemin suivi
   sur la paroi. Le scenario laissait 18 s de marge sur une montee de 57 s :
   **echeance portee de 75 s a 140 s**, cote harnais, pas cote jeu.
2. **Le scenario E (velo) echoue sur son "pied a terre" final, et ce n'est PAS
   du a la Phase 17** : rejoue deux fois avec `--no-wildlife`, il echoue
   pareil. La touche B finale n'emet aucune ligne, et la sonde montre le velo
   encore lance a 31 km/h alors que le scenario vient de freiner 5 s - la
   descente scriptee ne finit plus ou elle a ete reglee. **Non corrige** : hors
   perimetre, et le corriger a l'aveugle aurait ete plus risque que le signaler.

### Ce que la faune n'a pas

Aucune IA (deux etats, fuite en ligne droite opposee au joueur), ~~aucune
collision dans aucun sens~~ - **la collision JOUEUR -> ANIMAL a ete ajoutee en
Phase 20** ; le sens ANIMAL -> MONDE reste absent, et volontairement (voir plus
bas) -, aucune persistance, une seule espece, aucun lien avec
l'heure du jour ni avec les besoins du joueur. La Phase 17 construit la BOUCLE,
pas un bestiaire. Et rien n'est sauvegarde : la memoire du milieu meurt avec le
processus.

## Phase 18 (2026-08-25) : un vrai tapis d'herbe, et le profilage de la tuile

Detail complet : `NOTES-PHASE18.md`. Deux chantiers independants.

### Tache 1 - l'herbe

**Decoupe alpha par masque CALCULE**, pas par texture. `shaders/foliage.frag`
decoupe 4 brins par quad (12 par touffe) a partir des seuls UV et de l'alea
d'instance, et `discard` le reste. Le quad n'est plus le brin : il est la
PLANCHE dans laquelle on decoupe, d'ou son elargissement (demi-largeur au pied
0,26 -> 0,42, a la pointe 0,06 -> 0,30).

Trois raisons de ne pas prendre une texture RGBA : un binding de descriptor de
plus sur ce seul pipeline, un asset a produire, et surtout le filtrage mipmap
qui FAIT FONDRE des brins fins en bouillie grise a quelques metres.

`discard` et non melange alpha : le melange imposerait de trier 20 000 touffes
en profondeur a chaque image et une seconde passe. Contrepartie, un bord franc
sans MSAA (`rasterizationSamples = 1`) - compense en EFFACANT la decoupe avec la
distance (brins elargis jusqu'a refermer le quad entre 7 m et 17 m), sinon
l'herbe lointaine scintille.

**Zero pipeline, zero descriptor, zero binding ajoute** : la force de decoupe
passe par `params.w`, RESERVE dans le bloc de push constants depuis la Phase 11.

**Densite : `grassSlots` 24 -> 56.** Choisi CONTRE l'autre chemin possible
(baisser les seuils de selection), qui aurait mis de l'herbe la ou il n'y en a
pas - rocher, futaie dense, versants nus selon le LiDAR. Aucun seuil de
repartition n'a bouge : la carte de la Phase 11 est litteralement la meme, seul
son REMPLISSAGE change. 8 867 -> 20 098 touffes sur la vue d'alpage.

### Tache 2 - la construction d'une tuile

`TerrainStreamer::buildProfile()` chronometre trois postes separes. Premier
relevé, survol a 900 m/s : geometrie 1 858 us/tuile (66 %), **indices
636 us (23 %)**, televersement GPU 307 us (11 %).

Deux optimisations EXACTES (maillage identique, seul le travail change) :
topologie construite une fois et recopiee, et cibles de morphing regroupees par
bloc 2x2 (grille 33x33 au lieu de 65x65 echantillons de parent).

**2 609 -> 1 512 us par tuile (-42 %), +20 % de frames, 0 frame a trou avant
comme apres.** Temoin `--no-tile-cache`, deux passages par colonne, ecart entre
passages sous 0,5 %.

### Chiffres

| Vue | Avant | Apres |
|---|---:|---:|
| Reference Phase 11 (E 901500 N 6436600, yaw 100, pitch -7) | 271 FPS / 332,4 Mo | **293 FPS / 332,7 Mo** |
| Alpage (E 900500 N 6439000, yaw 100, pitch -7) | 137 FPS / 338,5 Mo, 8 867 touffes | **122 FPS / 340,2 Mo, 20 098 touffes** |

**Tests headless : 896 assertions** (885 avant, +11). Zero `[Validation]`
partout. Nouvelles options temoins : `--no-grass-cutout`, `--no-tile-cache`.

### Les trois lecons a retenir

1. **LA VUE DE REFERENCE D'UNE PHASE NE VAUT QUE POUR CE QU'ELLE CONTIENT.** La
   vue de la Phase 11, reprise pour mesurer l'herbe, n'a que **124 touffes** :
   tout changement du semis s'y perd dans le bruit. Il a fallu sonder cinq
   positions pour trouver un alpage reellement herbeux. Meme piege que « savoir
   ou il y a de la pente ne dit pas par ou l'aborder ».
2. **LA DECOUPE ALPHA COUTE ZERO FPS ; C'EST LE NOMBRE D'INSTANCES QUI COUTE.**
   95 FPS avec la decoupe, 95 FPS sans (temoin `--no-grass-cutout`, meme
   binaire, meme vue). Le fragment shader n'etait pas le facteur limitant. Tout
   le cout de la densite etait CPU : supprimer l'appel a `heightAt()` par touffe
   au profit de quatre par CELLULE a rendu 95 -> 118 FPS **sans changer un
   pixel**.
3. **LE DEUXIEME POSTE DE COUT D'UNE TUILE ETAIT UNE BOUCLE D'INDICES
   REDONDANTE.** 636 us/tuile, soit 23 % - deux fois le televersement GPU qui
   avait mobilise toute la Phase 12. L'en-tete de `TerrainNodeMesh.hpp` disait
   depuis la Phase 10 que la topologie etait « calculee une fois et recopiee » ;
   le code, lui, la reconstruisait a chaque tuile. **Un commentaire n'est pas
   une mesure.**

### Deux points rouges du harnais, non causes par cette phase

- Scenario E (velo), « pied a terre » : echec **deja documente en Phase 17**,
  non corrige (hors perimetre). Ni cause ni aggrave ici.
- Scenario I (faune), « animaux a portee de vue » : rouge au premier passage,
  **vert au rejeu isole ET au passage final**. Le critere compte des releves sur
  240 s, donc depend du FPS : c'est du bruit, pas une regression. Le harnais
  gagnerait a rendre E et I insensibles au debit d'images.

Harnais joue DEUX FOIS en entier cette session : 0 `[Validation]`, 0 `[ERROR]`,
fermeture propre et code 0 pour les onze processus, les deux fois.

### Ce qui n'est pas fait

Parallelisation multi-thread de `terrainNodeMeshData` (83 % du cout restant, et
la fonction est deja PURE - `Renderer::addMesh` n'est simplement pas sur pour
plusieurs threads) ; buffer d'indices GPU partage (~48 Mo recuperables) ;
teinte du terrain sous l'herbe (le sol entre les touffes reste l'orthophoto
grise, et c'est ce qui plafonne encore le tapis de tres pres).

## Phase 19 (2026-08-26) : le profil de la Phase 18 etait un profil de Debug

Detail complet dans **NOTES-PHASE19.md**. *(Section ecrite le 2026-08-26 au
debut de la Phase 20 : la session de la Phase 19 s'etait arretee apres avoir
redige ses NOTES, sans reporter le resume ici. Les NOTES, elles, sont
completes - harnais joue deux fois en entier.)*

### Le resultat principal annule les deux taches prevues

La mesure qui fondait la parallelisation du maillage de tuiles avait ete relevee
en **Debug** et ne tient pas en **Release**. En Release une tuile ne coute plus
140 us de geometrie (contre 1 344 en Debug) : le poste dominant devient le
**televersement** (63 %), et le frame entier n'est pas limite par le CPU.

**La lecon, et elle est generale : un profil de Debug n'est pas un profil de
Release.** Comme toutes les mesures du projet sont en Debug - la seule
configuration avec les couches de validation - rien ne pouvait le signaler.

- `Renderer::addMesh` **n'a pas ete rendu thread-safe** : les deux points
  dangereux (`commandPool_`, `graphicsQueue_` dans `flushCopiesNow`) ne sont
  atteints que si le staging en vol depasse 128 Mo, donc **jamais pendant le
  streaming**. La partie risquee aurait ete exactement celle qu'aucun test
  n'aurait pu exercer. Aucun appel Vulkan ne change de thread.
- Le maillage CPU, lui, est parallelisable sans toucher au Renderer
  (`terrainNodeMeshData` est PURE) : `--tile-threads=N`, **defaut 1**
  (sequentiel, le reglage que la mesure designe en Release). Le chemin parallele
  est garde comme outillage (2,4x et +14 % d'images en Debug) et comme TEMOIN.
- **Tache 2 : rien n'a ete optimise, a dessein.** Le frame est limite par le GPU
  (81 % de l'image passee a attendre dans `drawFrame`, verifie par ABLATION et
  non affirme). Supprimer entierement `gatherTrees` et `gatherGrass` ne rendrait
  rien. Ce que la phase livre est le **profileur permanent** (`Cout par image`,
  cinq postes CPU, non conditionnel) et `tools\profile_tiles.ps1`.
- Verifie : **902 assertions** headless (+6), zero `[Validation]`,
  `VK_LAYER_VALIDATE_SYNC=1` sur les deux chemins avec son **controle negatif**
  (barriere retiree -> la couche crie immediatement). Harnais joue deux fois en
  entier, 11 processus, un seul point rouge : le scenario E (velo, "pied a
  terre"), **anterieur a la phase** et documente depuis la Phase 17.

## Phase 20 (2026-08-26) : la collision cesse d'etre une affaire d'arbres

Detail complet dans **NOTES-PHASE20.md**.

### L'inventaire a reduit la consigne a UNE lacune

La consigne demandait d'etendre la collision "a tous les objets du monde qui
n'en ont pas encore". Etat reel trouve dans le code, avant d'ecrire une ligne :

| Categorie | Etat trouve | Decision |
|---|---|---|
| Arbres, souches (`Forest`) | collision depuis la Phase 6 | inchange |
| Objets d'editeur (`EditorScene`) | collision depuis la **Phase 15**, drapeau `solid` **par instance** (touche K, serialise en `.vcsc`) | inchange |
| **Faune (`Wildlife`)** | **aucune collision** | **corrige** |
| Objets au sol (`Ground`) | aucune collision | **laisse ainsi, delibere** |

Deux points a retenir. D'abord, **la decision "flag solide" que la consigne
proposait de prendre etait deja prise en Phase 15**, et dans le sens le plus fin
des deux (par instance, pas par asset). Ensuite, **l'architecture ne demandait
aucune generalisation** : `CollisionSolver` + `moveBody()` (Phase 6) et
`CompositeCollisionSolver` (Phase 15) suffisaient. Brancher la faune est **une
ligne** dans `main.cpp`, et cette ligne couvre **les trois activites au sol** -
ni `Camera`, ni `Bike`, ni `Climber` n'ont ete modifies. Deuxieme fois que
l'interface de la Phase 6 tient sa promesse.

### Ce qui a ete ecrit

`Wildlife` implemente `CollisionSolver`. Cylindre vertical, rayon **0,65 m** =
demi-longueur du mesh de cervide, x `Animal::scale` ; geometrie **copiee de
`Forest::resolve`**, pas reinventee. Le lien mesh/collision est tenu par un
**`static_assert`** : le jour ou le cervide changera de taille, ca echoue a la
COMPILATION au lieu de le rendre traversable par le flanc.

### LA MESURE : temoin negatif et chiffre predit

Nouveau temoin **`--no-animal-collision`** : la faune reste VISIBLE et redevient
traversable. `--no-wildlife` ne pouvait pas jouer ce role - supprimer les
animaux ne montre pas qu'on traverse celui qui est la.

| Marche de 20 m droit sur un cervide | approche la plus proche | parcouru |
|---|---:|---:|
| Temoin `--no-animal-collision` | **0,0010 m** (le centre) | **20,00 m** |
| Avec collision | **1,0418 m** | 13,20 m |

**1,0418 m est le chiffre PREDIT** (0,65 x echelle + 0,35 de demi-largeur du
joueur), atteint au millimetre pres.

**Le test attendait d'abord un ARRET NET a 8,96 m, et il avait tort.** Un
obstacle convexe ISOLE se **contourne** : la poussee frontale n'est jamais
parfaitement frontale a l'arrondi pres, et la composante laterale accumulee fait
glisser autour - c'est le comportement voulu du solveur par repoussement
(`CollisionSolver.hpp`). La grandeur que le modele predit n'est pas la distance
d'arret, c'est le **rayon de contact**. *Un chiffre attendu qui ne sort pas
n'est pas forcement un bug : il faut d'abord savoir lequel, du code ou de
l'attente, decrit le modele.*

### Toutes les activites, verifiees une par une

- **Marche** : mesuree ci-dessus.
- **Velo** : mesure a part, et c'est le cas qui compte - le velo roule a 30 km/h,
  soit la vitesse de FUITE de l'animal (8 m/s). **C'est la seule activite qui
  peut rattraper un cervide.** A pied on ne l'approche jamais, il part a 30 m.
- **Escalade** : concernee au PIED de la paroi, jamais dessus, et pour une raison
  de MODELE : `WildlifeParams::maxSlope` (0,90) < `ClimberParams::wallSlope`
  (1,00), donc **aucun animal ne peut se tenir sur une pente grimpable**. Meme
  genre de rapport entre deux constantes reglees separement que la lecon de la
  Phase 17, sauf qu'ici il est bon - une assertion le fige.
- **Parapente** : hors perimetre, inchange, **coherent avec la Phase 8** qui
  documente deja l'absence de collision en vol. A l'atterrissage le joueur
  repasse en `Walk` et retrouve tout.

### DECISION : les objets au sol ne bloquent pas

Buches, branches, pierres et eclats restent traversables. Trois raisons, et
c'est une decision documentee, pas un oubli : ce sont des cibles de RAMASSAGE
(`findNearest` exige de s'en approcher) ; le plus haut - la pierre - culmine a
**0,40 m**, sous le genou, et le moteur n'a pas de step-up ; et un debitage en
depose **3 a 4 autour du joueur**, ce qui pourrait l'emmurer dans le bois qu'il
vient de produire. Un test fige la decision au lieu de la laisser au
commentaire. Ce qui la rendrait fausse : un objet au sol nettement plus gros -
il faudrait alors un drapeau par NATURE d'objet, pas un collider pour toute la
categorie.

### Ce qui reste faux, volontairement

**Un animal traverse toujours les troncs, les rochers et ses congeneres.**
Corollaire du "aucune IA" de la Phase 17 : la fuite est une ligne droite sans
evitement, donc un animal qui buterait resterait COLLE a l'obstacle. Un cervide
encastre dans un arbre serait plus laid que le probleme resolu. Le cylindre
reste aussi trop genereux **de face** (0,65 m pour 0,42 m de large) : un
cylindre ne peut pas etre juste dans les deux sens.

### Verification

**935 assertions** headless (902 avant, **+33**), zero avertissement `/W4`.
Aucun objet Vulkan, aucun thread, aucune barriere touches - d'ou **pas de
`VK_LAYER_VALIDATE_SYNC`** cette fois, et c'est justifie plutot qu'omis (le
chemin de televersement des tuiles est rigoureusement inchange).

**Pas de scenario Vulkan dedie** : le harnais ne pilote que le clavier et la
faune est semee ALEATOIREMENT autour du joueur, donc un scenario reproductible
demanderait une entree de debogage dans `Wildlife`. La preuve de collision est
headless et deterministe - ce qui est aussi le seul endroit ou le temoin negatif
est rigoureux. Le harnais complet a servi a la NON-REGRESSION.

**Harnais joue en entier** : **zero `[Validation]`, zero `[ERROR]`** sur 5 510
lignes, 4 `[WARN]` benins habituels. Non-regression confirmee, et la ligne qui
compte pour cette phase : **l'editeur (scenario G) rend exactement le meme
chiffre qu'en Phase 15 - 0,28 m avec la scene contre 21,03 m sans**. Ajouter une
source au composite n'a rien change aux deux autres. Faune (I) : 4 animaux a
portee -> 1. Besoins (H), escalade (F), parapente (D), dalle reelle (C) : verts.

Deux points rouges, aucun imputable a la phase :
1. **Scenario E (velo), "pied a terre" `False`** : echec ANTERIEUR, documente
   depuis la Phase 17, hors perimetre.
2. **Scenario B, code -1** : celui-la ne pouvait PAS etre classe sans verifier,
   parce que le scenario B EST le chemin de collision. **Rejoue seul : vert**
   (code 0, fermeture propre, "tronc debout PUIS souche pousses"). Flakiness
   `PostMessage` deja documentee. Pas une regression.

## Prochaines etapes suggerees

0. **glTF dans le catalogue d'assets** - la seule chose demandee en Phase 15 qui
   n'ait pas ete livree, et desormais le goulot de tout habillage : les
   bibliotheques libres de droits distribuent surtout du glTF, il faut convertir
   a la main avant de deposer dans `assets/editor/`.
   `AssetCatalog::supportedExtensions()` est le SEUL point d'extension - ancrage,
   collision, catalogue, placement et serialisation sont deja agnostiques du
   format. Compter un analyseur JSON, le conteneur `.glb`, les accesseurs et
   l'aplatissement de la hierarchie de noeuds.
0bis. **Paralleliser la construction des tuiles** (Phase 18) : c'est 83 % du
   cout d'une tuile apres les optimisations exactes de cette phase, et
   `terrainNodeMeshData` est deja PURE. Le seul obstacle est
   `Renderer::addMesh`, qui doit rester sur le thread de rendu.
1. **Texture de detail de pres** pour completer l'orthophoto (voir Phase 11),
   et **teinter le terrain sous l'herbe** (Phase 18).
2. **Promotion decor -> abattable** et **collision avec les arbres de decor** :
   sans elles, la foret des 25 km2 reste un fond de scene.
3. ~~Transfert asynchrone des tuiles de terrain.~~ **FAIT EN PHASE 12.** Le
   verrou suivant est le **maillage CPU d'une tuile**, toujours synchrone sur le
   thread de rendu : un thread de maillage dedie est la suite logique.
4. **Index buffer partage** entre toutes les tuiles, et **sommet allege** (delta
   de morphing plutot que position complete) : deux economies faciles, chiffrees
   dans NOTES-PHASE10.md. Le partage de l'index buffer economiserait aussi
   98 Ko par tuile epinglee.
5. **Decider de la scene par defaut** : `vercora.exe` sans argument charge
   toujours la dalle de Phase 7, alors que la zone 25 km2 est desormais habillee
   et jouable (271 FPS).
6. **Densifier l'herbe** (decoupe alpha) - explicitement laisse en l'etat.
7. Habillage visuel du parapente (mesh d'aile, inclinaison en virage) **et du
   velo** (mesh de VTT a la place du cube).
7bis. **Trancher la question de la pente franchissable a velo** (voir Phase 13) :
   23,6 % rend le massif largement non-montable en ligne droite.
8. Vent/thermiques une fois le systeme meteo pose. **La Phase 16 a pose la
   premiere brique de ce systeme** : temperature ambiante fonction de l'altitude,
   de l'heure et de l'exposition du versant (`ambientTemperature`). Le vent n'en
   fait pas partie.
8bis. ~~**Escalade** : la quatrieme activite, seule encore non commencee.~~
   **FAITE EN PHASE 14.** Les quatre activites du cadrage ont desormais une base.
   Les suites les plus utiles sont, dans l'ordre : faire dependre la vitesse et le
   cout d'escalade de la PENTE LOCALE (amelioration a cout nul, avant tout systeme
   de prises), et **donner une limite de pente a la MARCHE** - sans elle un
   versant a 90 % se monte encore a pied et l'escalade reste facultative.
8ter. **Une interaction entre activites**, la premiere du projet : les grandes
   falaises (250 a 276 m) ont 0 a 2 % de pied marchable, on ne peut pas les
   atteindre a pied. Le parapente est le moyen d'approche evident - mais il faut
   aussi des RELAIS pour reprendre de l'energie a mi-paroi.
8quater. **DONNER DE QUOI MANGER** (Phase 16). C'est desormais la premiere
   priorite de gameplay : le joueur a une jauge de faim que rien au monde ne peut
   remplir. Cueillette, chasse, ou simples reserves de depart - c'est une
   decision de cadrage, pas une decision technique. Dans la foulee : un ABRI
   (qui donnerait un sens a `NeedsContext::sheltered` et un usage aux buches) et
   le SOMMEIL (le repos refait la forme, mais rien ne distingue une pause d'une
   nuit).
8quinquies. **Rendre le cycle jour/nuit.** L'horloge existe depuis la Phase 16 et
   fait tomber la temperature la nuit, mais le soleil des shaders reste une
   direction fixe : il fait toujours jour a l'ecran. C'est la seule incoherence
   VISIBLE introduite par la Phase 16.
9. Ombres portees. (Les objets poses par l'editeur les rendent plus voyantes
   encore que la vegetation : un rocher de deux metres sans ombre flotte.)
10. **Suites de l'editeur**, par ordre d'utilite (detail dans NOTES-PHASE15.md) :
   collision par volumes composes plutot que par cylindre englobant, frustum
   culling sur les objets poses, alignement sur la normale du terrain
   (`terrainNormalAt()` est deja ecrit et teste), et selection multiple.
   ~~Selectionner sans viser.~~ **FAIT LE 2026-08-23** (touches F3/F4, voir
   plus haut) - mais un vrai reticule et des gizmos 3D restent a faire (§9).

## PHASE 21 (2026-08-27) — build demo + packaging distribuable

**Objectif atteint** : un `.exe` envoyable a quelqu'un sans environnement de
dev. Detail complet dans `NOTES-PHASE21.md`. Resume :

- **`--demo`** (flag runtime, pas de macro de compilation) : coupe faune,
  besoins, velo/parapente/escalade, artisanat interactif (abattage/ramassage/
  percussion) et l'editeur. Garde marche, vol libre (F), LOD/streaming du
  terrain 25 km², orthophoto, vegetation procedurale, FPS. Charge par defaut
  `vercora_zone_1m.vchm` (toute la zone) au lieu de la dalle unique de dev.
- **Chemins codes en dur elimines** : nouveau `src/core/Paths.hpp/.cpp`
  (`assetsDir()`, `shadersDir()`) prefere le dossier a cote de l'executable
  s'il existe, sinon retombe sur les macros CMake (`ASSETS_DIR`,
  `SHADERS_OUTPUT_DIR`) - donc AUCUN changement pour le developpement
  courant. C'est ce qui rend le binaire copiable ailleurs.
- **Build Release confirme sans validation layers** (`VERCORA_DEBUG` non
  definie hors Debug, deja le cas avant cette phase - juste verifie).
- **Dependances runtime** : `vulkan-1.dll` vient du pilote GPU (absent du SDK
  Vulkan, non fourni dans le package) ; `MSVCP140.dll`/`VCRUNTIME140.dll`/
  `VCRUNTIME140_1.dll` (VC++ Redistributable) sont fournies dans le package,
  copiees depuis `VC\Redist\MSVC\...` de Visual Studio.
- **Package final** (`dist/VercoraDemo/`) : **140 Mo** au total (dalle 1 m
  95 Mo + orthophoto 43 Mo + exe/DLL/shaders ~1 Mo). Trop gros pour un email
  standard (limite Gmail 25 Mo) -> **necessite un service de transfert**
  (WeTransfer, lien Drive/OneDrive, etc.). Alternative plus legere possible :
  substituer `vercora_zone_2m.vchm` (24 Mo) -> package ~68 Mo, deja gere par
  le code en repli automatique.
- **Teste** : build Debug intact (0 erreur de validation sur le scenario A du
  harnais Vulkan), build Release compile et tourne, **lance et tourne
  correctement copie sur un lecteur different (E:)** - preuve que plus rien
  ne depend du chemin du depot de developpement.
- **Ce qui n'a PAS pu etre verifie** (pas de machine tierce reelle) : GPU/
  pilotes de l'ami, reaction de SmartScreen chez lui (l'exe n'est pas signe),
  Windows 10/11 cote lui. Voir "§7" de `NOTES-PHASE21.md` pour la liste exacte
  a demander de verifier.

## PHASE 22 (2026-08-27) — trois matieres recoltables, et la dette du velo enfin comprise

Detail complet : `NOTES-PHASE22.md`. Deux chantiers independants.

### Tache 2 — la dette du velo : LE JEU N'A JAMAIS EU DE DEFAUT

**C'est le harnais qui regardait trop tot.** Le scenario E echouait sur son
« pied a terre » depuis la Phase 17 ; cinq phases l'ont reporte. La cause est
maintenant mesuree, et elle n'est pas dans `Bike.cpp` :

`Start-Game` capturait la sortie du jeu avec `Register-ObjectEvent -Action`,
c'est-a-dire **un script-block PowerShell execute PAR LIGNE de journal**. Ces
blocs ne tournent que quand le runspace est disponible ; or les scenarios passent
leur temps dans des `Start-Sleep`. Les evenements s'empilent, et le tampon que
`Wait-ForLog` interroge est **EN RETARD** sur ce que le jeu a ecrit. **Le retard
CROIT avec la duree du scenario.**

Temoin decisif — meme binaire, meme dalle, meme instant de lecture, seul le
collecteur change :

| collecteur | « en selle » | « pied a terre » |
|---|---|---|
| `Register-ObjectEvent -Action` | 1 833 ms | **jamais vu en 3 s** (9,6 s reels) |
| delegue .NET compile | 5 ms | 0 ms |

La ligne `[INFO]  Velo : pied a terre.` **etait dans le journal du jeu** (ligne
490 sur 520) a chaque run depuis la Phase 17, suivie de « Fermeture propre. » et
d'un code de sortie 0. Verifie aussi par trois reproductions hors harnais :
enfourcher + 40 s de pedalage (3/3), + 14 rafales de demi-tour (3/3), et **la fin
exacte du scenario E — elan, 32 s de descente, freinage, pied a terre — 3/3
propres** (cap impose par `--look=40,0` au lieu d'etre asservi).

**Correctif** : le collecteur est un delegue .NET compile (`VercoraLogSink`,
`Add-Type`), qui se branche lui-meme (PowerShell 5.1 ne convertit pas une
`PSMethod` en `DataReceivedEventHandler`). `ToString()` surcharge => **aucun
appelant touche**. **Aucune ligne du jeu modifiee.**

**Nota qui vaut pour tout le harnais** : « en selle » consommait deja **61 % de
son budget de 3 s**. Le defaut menacait TOUS les `Wait-ForLog`, pas seulement
celui du velo ; le scenario E etait juste le plus long, donc le premier a
franchir le seuil. Elargir le delai aurait masque cela au lieu de le resoudre.

**Confirmation non cherchee** : le second demi-tour du velo converge desormais en
**3 rafales au lieu de 9** — `Turn-BikeTo` s'asservit sur un cap qui n'est plus
perime. Meme cause, deuxieme preuve.

**Fausse piste a retenir** : le premier run de diagnostic montrait le jeu mourant
a ~30 s (sortie `-1`), puis a 105 s, puis survivant 280 s. C'etait un
**processus `vercora.exe` orphelin** d'une mesure precedente qui se disputait le
GPU. **Verifier qu'aucun processus de jeu ne survit AVANT de conclure quoi que ce
soit d'une mesure de stabilite ou de performance.**

### Tache 1 — argile, sable, resine, plantes a fibres

Les trois matieres du document de cadrage qui manquaient. Toutes arrivent **par
le sol**, comme l'eclat de percussion de la Phase 6 : l'objet tombe aux pieds du
joueur et doit etre ramasse (`R`). Rien ne tombe dans l'inventaire.

**`world/Deposits` — argile et sable. LE PLACEMENT EST UN PROXY, PAS UNE CARTE.**
Le LiDAR ne porte AUCUNE information de nature de terrain (ni lithologie, ni
granulometrie, ni formations superficielles) : distinguer une vraie argile d'un
vrai sable y est **strictement impossible**. La regle codee est morphologique :
un **creux** (memes huit sondes que `WaterSource`) + un **versant amont** de 3 m
minimum — c'est ce second critere, et non la concavite, qui ecarte les replats de
crete. Partage **argile a moins de 120 m d'un point d'eau detecte, sable
sinon** ; **sans point d'eau connu, tout est du sable**. Seuils plus permissifs
que ceux de l'eau (pente 22 % contre 15 %, concavite 0,6 m contre 1,0 m, 4 sondes
sur 8 contre 5) : *l'eau doit stagner pour qu'on y boive, un sediment n'a qu'a
s'etre depose*. **Sur la dalle de reference : 18 gisements, 2 d'argile et 16 de
sable — et les deux argiles tombent exactement sur les deux points d'eau de la
Phase 16.** Touche `Y`, portee 3,5 m.

**`world/Resin` — resine, recoltee SANS abattre.** Le jeu n'a aucune notion
d'essence d'arbre : mesh unique, et la carte de canopee donne une hauteur et un
couvert, pas une classification feuillu/resineux. D'ou un **marquage
DETERMINISTE** par hachage (55 % de resineux), recalcule et jamais stocke. On
vise le meme arbre, par le **meme** `Forest::pickTree` — mais rien n'est abattu,
rien n'est debite, et l'`Habitat` n'enregistre **aucune coupe**. Redonne au bout
de 600 s, epuise apres 3 entailles. Touche `U`.
**DETTE PRINCIPALE : un joueur ne peut pas VOIR quel arbre est un resineux** —
tous ont le meme mesh. Une silhouette distincte est le prolongement naturel, non
fait.

**`world/FiberPlants` — ronces et orties.** Deux voies combinees par un
**MAXIMUM** (pas une somme : une coupe en pleine lisiere ne doit pas porter deux
fois plus de ronces) :
1. **la lisiere**, lue dans la carte de couvert LiDAR, avec un **toit
   triangulaire** sur la bande (maximum au milieu, nul aux deux bords) — un
   creneau ferait porter des ronces a tout un versant clairseme, ce qui est une
   lande et non une lisiere ;
2. **le sol recemment ouvert**, via une nouvelle interface `world/Clearing.hpp`
   (`ClearingSource`) implementee par `Habitat`. **Une zone ou le joueur a abattu
   des arbres se met a porter des ronces.** C'est la **PREMIERE fois dans ce
   projet qu'une action du joueur FAIT APPARAITRE quelque chose au lieu de le
   retirer** — la Phase 17 avait construit « je coupe, le milieu s'appauvrit »,
   celle-ci lui donne son revers.

Deux interfaces et non une : `groundCover` demande « reste-t-il du couvert ? » et
**rarefie** l'herbe, `clearing` demande « a-t-on ouvert ? » et **fait pousser**
les ronces. Le jour ou l'usure par pietinement degradera le couvert, elle fera
baisser l'une sans ouvrir l'autre — un sentier tasse ne fait pas pousser
d'orties.
**Sans carte de couvert, la voie 1 est entierement muette**, et c'est voulu : on
n'invente pas une lisiere par un substitut de pente. Temoin naturel — un monde
sans carte ne porte QUE les plantes que le joueur a fait pousser.
Semis **deterministe par cellule** (rien de stocke sur 25 km2) ; seules les
plantes **arrachees** sont memorisees, le temps de leur repousse (420 s), apres
quoi la meme plante revient **a la meme place et a la meme taille**. Touche `I`,
portee 2,5 m.

**Les touches** : une rangee contigue, `E abattre / R ramasser / T boire /
Y creuser / U saigner / I arracher`. Pas une coquetterie : la rangee du haut est
`A-Z-E-R-T-Y-U-I-O-P` en AZERTY et `Q-W-E-R-T-Y-U-I-O-P` en QWERTY, donc **tout
ce qui suit le Z/W occupe la meme position physique**, et GLFW raisonne en
positions physiques.

**`world/HarvestMesh`** : quatre meshes d'objets au sol + un mesh de plante sur
pied, generes en code. Quatre silhouettes et non un mesh recolore — en « zero
menu virtuel », toute la lisibilite repose sur la forme.

### Verifications

**Headless : 1 095 verifications** (935 avant, soit **+160**), toutes les
anciennes toujours vertes (`Forest`, `Knapping`, `Habitat`, `Bike`, `Climber`,
editeur, besoins, faune). Temoins negatifs partout.

**TROIS TESTS AVAIENT TORT, PAS LE CODE**, et c'est la mesure qui l'a montre :
- le **pied d'une bosse** etait ecrit comme un controle negatif. Mesure : deux
  gisements a 160 m du centre, 8,1 m de concavite, 26,8 m de versant au-dessus.
  C'est la definition d'un cone de dejection, l'endroit le plus credible d'un
  relief pour trouver du sediment. Devenu un controle **positif** ;
- le temoin de `minUpslope` plafonnait la cuvette a 40 m : le detecteur retenait
  alors trois points sur un anneau a 60 m, ou le flanc fournit 5,5 m d'amont.
  **Une cuvette n'est « sans amont » qu'a son CENTRE, pas partout** ;
- l'assertion de demi-hauteur exigeait l'egalite sur des meshes **bruites** :
  elle testait le tirage aleatoire, pas le contrat.

Un test **existant** a fait son travail : il a signale que les quatre nouvelles
natures manquaient dans la table mesh-par-nature. Ces `switch` ont un `return`
par defaut — le compilateur, lui, n'aurait rien dit.

**Vulkan : DIX scenarios (A a J), QUATORZE processus.** Zero `[Validation]`, zero
`[ERROR]`, codes de sortie nuls, fermeture propre partout, **et « pied a terre :
True »**. **Scenario J ajoute, en TROIS processus** : (1-2) le script relit les gisements
que LE JEU a journalises, s'y rend par `--start`, creuse, ramasse, puis s'eloigne
de 30 m et recreuse — le refus a distance est le controle negatif, sans lequel on
ne distinguerait pas « la portee marche » de « la touche produit toujours quelque
chose » ; (3) les PLANTES A FIBRES sur la **zone de 25 km2**, la seule qui ait
une carte de couvert. **Le point de depart a ete trouve PAR MESURE** (balayage de
la zone au pas de 50 m avec la fonction de semis elle-meme : 1 656 cellules
candidates, meilleure a monde (1450, -1750), alt 1777 m, couvert 0,240). **Le
point de depart par DEFAUT de la zone est a 1947 m, au-dessus de la limite des
arbres : zero plante, zero herbe — un scenario pose la aurait ete VERT SANS RIEN
EXERCER.** Chaine verifiee : 295 plantes semees -> arrachage -> 294 et « 1 en
repousse » (et non disparue) -> ramassage du fagot.

**Cout mesure du semis** (zone 25 km2, 10 486 images) : plantes a fibres
**185,9 us/image**, contre herbe 828,2, `drawFrame` 838,5 et arbres 1 278,3. Le
moins cher des trois semis, d'un facteur quatre.

### Un critere du harnais qui se contredisait (scenario I, faune)

La suite est passee rouge UNE fois sur « animaux a portee de vue » : 2 au dernier
releve, la ou le seuil n'en tolerait qu'1. **Verifie avant de toucher au code —
une regression serait reproductible** : MEME binaire, scenario I en suite
complete -> 2 ; en isolation, trois fois -> 1, 1, 1. C'est du bruit lie a la
CHARGE DE LA MACHINE (le scenario compte des releves sur 240 s, et les tentatives
d'apparition suivent le FPS). Deja consigne en Phase 18 : « rouge au premier
passage, vert au rejeu isole ET au passage final [...] c'est du bruit ».

Surtout, **le critere contredisait sa propre documentation** : le commentaire qui
le precede dit depuis la Phase 17 qu'un residu « d'un ou DEUX animaux » est
correct (ponderation `1 - (d/r)^2`, qui tend vers zero au bord du rayon), et le
seuil n'en tolerait qu'un. Plancher porte de 1 a 2 — **mise en accord du seuil
avec le modele, pas assouplissement de confort** : l'assertion exige toujours de
partir de >= 3 et de finir a 2 au plus. La vraie correction (rendre E et I
insensibles au FPS) reste a faire.

### Ce qui n'est pas fait

Aucun **usage fonctionnel** des quatre matieres (recoltables, rien ne s'en
fabrique — c'etait le perimetre demande). Les **resineux n'ont pas de silhouette
propre**. **La resine n'est pas exercee sous Vulkan** : elle demande de VISER un
arbre, or le harnais ne sait pas tourner la camera et l'unique position de tir de
la dalle est deja consommee par le scenario C. (Les plantes a fibres, elles, le
sont depuis le scenario J3.) Rien n'est
**sauvegarde** : entailles et plantes arrachees meurent avec le processus. Une
seule espece de plante a fibres (mesh unique, seule l'echelle varie).

---

## PHASE 23 (2026-08-27) — systeme meteo deterministe, et le jour se met enfin a tomber

Fichiers neufs : `src/world/Weather.hpp` / `.cpp`, `NOTES-PHASE23.md`,
`tools/shot_daynight.ps1`.

### AVERTISSEMENT EN TETE, parce qu'il conditionne la lecture de tout le reste

**Aucune valeur numerique de ce systeme n'a ete validee par un climatologue.**
Ce sont des ordres de grandeur plausibles. Le code separe donc strictement :

- **les mecanismes** — implementes serieusement, testes, independants des
  chiffres ;
- **les valeurs** — TOUTES dans une seule struct, `WeatherParams`, chacune
  commentee avec ce qu'elle fait, sa valeur provisoire et d'ou elle sort. Les
  recalibrer se fait en ouvrant un seul fichier.

Deux exceptions non provisoires et sourcables : le **refroidissement eolien**
(formule JAG/TI, Environnement Canada / NWS depuis 2001) et la **declinaison
solaire** (Cooper, 1969). Une troisieme est un fait : la latitude, 45,0 degres,
parce que la dalle est en Isere.

`NOTES-PHASE23.md` liste par ordre de doute les sept valeurs qui demandent une
vraie relecture externe. La plus arbitraire est
`precipitationCloudThreshold = 0,72`, qui fixe a elle seule la frequence des
precipitations.

Les tests verifient en consequence des **proprietes** ("un adret est plus chaud
qu'un ubac au meme instant", "un col vente plus qu'un plateau adjacent") et
**jamais des valeurs absolues**.

### L'architecture : une fonction d'etat PURE

Aucun etat cache, aucune integration dependante du pas de temps. Pour un point
`(x, y, z)` et un instant `t`, le module rend l'etat meteo exact.

- En jeu : deux joueurs au meme endroit au meme instant vivent exactement les
  memes conditions, quel que soit le chemin, la duree, le nombre d'images par
  seconde ou les pauses.
- En test : on saute directement a `t = 137 h` sans simuler les 137 heures.
  C'est ce qui rend la couverture de cette phase possible.

Contraste avec `Needs` (Phase 16), qui INTEGRE : les jauges ont une memoire,
c'est leur role, et rien n'a change de ce cote.

**Couche 1, synoptique (0D)** — saison et cycle diurne a deux harmoniques,
perturbations par bruit de gradient 1D, position solaire, vent de reference,
nebulosite, precipitation. Ne connait aucune position. **0,38 us**.

**Couche 2, spatialisation (3D)** — gradient altitudinal, adret/ubac par produit
scalaire, inversion de fond de cuvette, vent local par sondage amont, ridge
lift, subsidence, turbulence, thermiques, orographie, mouillure. **4,0 us**.

**Seed FIXE par defaut**, derivable d'une graine de partie par le constructeur.
Delibere : la zone de jeu est unique et faite a la main, rien d'autre dans
Vercora n'est reseede par partie, et un calendrier fixe est ce qui rend deux
captures du meme instant comparables (lecon de la Phase 12).

### UNE SEULE source de verite sur la temperature

C'etait la contrainte la plus explicite du cahier des charges, et elle a demande
de DEPLACER du code, pas d'en ajouter.

Ont quitte `NeedsParams` pour `WeatherParams` : `seaLevelMeanTemperatureC`,
`lapseRateCPerKm`, `diurnalAmplitudeC`, `diurnalPeakHour`, `exposureBonusC`.
Ont quitte `Needs.cpp` pour `Weather.cpp` : `ambientTemperature()`,
`daylightFactor()`, `terrainExposure()`. A ete **supprime** :
`paraglideWindChillC`.

`NeedsContext` porte desormais `ambientTemperatureC` et `windSpeedMps` au lieu
de `altitudeMeters` / `timeOfDayHours` / `exposure`. **Needs ne calcule plus
aucune ambiante** : il en recoit une et n'y ajoute que ce qui concerne le corps
du joueur — effort, fatigue, abri, vent subi. Tant que les deux calculs
coexistaient, rien n'empechait qu'un reglage n'en corrige qu'un.

Ce n'est pas un renommage : la nouvelle ambiante connait en plus la saison, la
masse d'air, la nebulosite (qui ecrase l'amplitude diurne — c'est ce qui fait
qu'une nuit couverte est douce et une nuit claire glaciale) et l'inversion de
fond de cuvette. `daylightFactor` etait une demi-sinusoide fixe de 6 h a 18 h ;
c'est maintenant le sinus de l'elevation solaire reelle, donc les journees
s'allongent avec la saison.

**Le refroidissement eolien remplace le forfait de -6 degres reserve au vol.**
La marche en crete par vent fort refroidit desormais elle aussi. Hors du domaine
de validite publie (T > 10 degres ou vent < 4,8 km/h), la fonction rend la
temperature INCHANGEE plutot qu'une extrapolation : au-dessus de 10 degres la
formule brute rendrait un vent *rechauffant*.

### L'adret/ubac par produit scalaire, et ce qu'il produit de mieux

On compare le cosinus d'incidence du point a celui qu'aurait du terrain PLAT au
meme instant. Le comportement en sort tout seul :

- **a midi**, un versant sud ne gagne presque rien (il est deja perpendiculaire
  au rayonnement) alors qu'un versant nord perd beaucoup ;
- **au petit matin**, c'est le versant **est** qui prend le maximum — un modele
  purement nord/sud ne pouvait pas le produire.

Mesure a midi, 1000 m, pente 60 % : adret 14,7 C, ubac 10,7 C.

### GameClock compte maintenant le temps TOTAL

`hours()` est inchange (heure locale dans [0, 24)) ; `totalHours()` est le
compteur monotone. Le systeme meteo en a besoin : une perturbation dure
plusieurs jours, elle ne peut pas etre indexee par une heure qui reboucle.
`setHours()` recale sur le jour courant au lieu de remettre a zero — regler
l'heure a la main deplace dans la journee, pas dans le calendrier meteo.

### QUATRE DEFAUTS TROUVES PAR LES TESTS

Ce sont les seules parties du travail dont on sache qu'elles ne sont pas
seulement plausibles.

**(a) Un fBm n'atteint jamais ses bornes theoriques.** Sans gain, la nebulosite
restait entre 0,20 et 0,70 sur trente jours simules : **jamais de grand beau**,
donc jamais de nuit claire, donc **tout le mecanisme d'inversion restait mort**,
et 13 heures de pluie sur 720. Corrige par `frontNoiseGain = 2,2` + ecretage.
Apres : 61 h de pluie, 63 h de grand beau. Le test qui l'a trouve exige qu'un
mois de jeu contienne des journees claires ET des journees de pluie — c'est le
controle negatif d'un modele qui a l'air de marcher tout en etant quasi
constant. **Le meme doute vaut pour le bruit de valeur de `Heightmap.cpp` et
`Vegetation.cpp`, jamais mesure de cette facon.**

**(b) Un versant sud plus chaud de 2,9 degres a 2 h DU MATIN.** Le produit
scalaire continuait de se calculer avec le soleil sous l'horizon, ou la
reference "terrain plat" tombe a zero et AMPLIFIE l'ecart au lieu de l'eteindre.
Une formule geometrique reste definie hors de son domaine physique ; le zero
attendu ne sort pas des maths, il faut le poser. Corrige par
`aspectFullDaylightSin`. **C'est le controle negatif de NUIT qui l'a trouve, pas
le test positif de midi, qui passait.**

**(c) La moyenne des sondages amont effacait l'abri.** Un point 250 m sous le
vent d'une crete de 200 m — un abri franc — obtenait un signal d'exposition de
**-0,05** : la sonde proche voyait bien la crete, mais la plaine des sondes a
750 m et 2000 m ramenait la moyenne au niveau du point. La regle correcte tient
en une phrase : **etre abrite, c'est avoir du relief au-dessus de soi dans AU
MOINS UNE echelle ; etre expose, c'est dominer TOUTES les echelles.** Donc un
`max` d'un cote et un `min` de l'autre, jamais une moyenne. Une moyenne suppose
les echelles interchangeables ; elles ne le sont pas.

**(d) La turbulence de sillage se mesurait sur le mauvais vent.** Elle etait
calculee sur le vent LOCAL, qui est justement faible sous le vent : un sillage
etait d'autant plus calme qu'il etait plus abrite, l'inverse exact de la
realite. Ce qui agite un sillage est le flux qui vient de decoller de la crete,
lequel a la vitesse AMONT.

### Et une cinquieme fois ou le test avait tort

Le test d'inversion employait d'abord une cuvette de 400 m de creux pour 500 m
de rayon. Il echouait — mais le modele avait raison : a 400 m de denivele, le
seul gradient adiabatique rend le fond 2,6 degres PLUS CHAUD que le rebord, et
l'inversion, correctement calculee, ne suffisait pas tout a fait a renverser ce
handicap. Le relief etait invraisemblable : 400 m de creux pour 500 m de rayon
est une falaise circulaire, pas une combe. Ramene a 150 m, le test passe
largement. Voir la note memoire sur l'hypothese "le test a tort" — elle merite
d'etre instruite aussi serieusement que l'inverse, et aussi serieusement rejetee
quand elle est fausse, ce qui a ete le cas pour les quatre defauts ci-dessus.

### Le sondage amont : 7 lectures de heightmap, et ce qu'on en tire

Le point, trois en amont (250 / 750 / 2000 m), un en aval (100 m), deux lateraux
(± 400 m perpendiculairement au flux). C'est le budget d'une seule lecture de
pente du velo.

Le **Venturi** est detecte par les deux sondes laterales, mais **il faut une
ponderation et elle n'est pas cosmetique** : un fond de vallee a lui aussi ses
deux cotes plus hauts, exactement comme un col. La seule chose qui les distingue
est le rapport a l'AMONT — un col domine son amont (le vent le franchit), un
fond de vallee non (le vent passe au-dessus). Sans ce poids, chaque fond de
vallee de la zone de jeu deviendrait une soufflerie.

Mesures sur le relief de test (crete nord-sud de 400 m avec col, vent d'ouest) :

| Point | Vent | Facteur de relief |
|---|---|---|
| Col | 14,2 m/s | x 2,01 |
| Crete pleine | — | x 1,80 |
| Plateau au vent | 6,4 m/s | x 1,00 |
| Versant au vent | ridge lift **+4,99 m/s**, turbulence 0,00 | |
| Versant sous le vent | subsidence **-1,80 m/s**, turbulence 0,60 | |

Le col vente 2,2 fois plus que le plateau, et plus que la crete pleine **a vent
de reference egal** — l'effet d'entonnoir est bien distingue de l'effet
d'altitude.

### La carte de piegeage d'air froid

Precalculee UNE FOIS au chargement, 128 x 128 sur les 5 km (cellules de 39 m,
la bonne echelle pour une cuvette), 393 216 lectures de heightmap payees une
seule fois. Lecture au runtime : une bilineaire.

Elle mesure l'**ENFERMEMENT**, pas l'altitude : huit directions a trois rayons
(120 / 350 / 900 m), on compte combien de ces 24 sondes sont plus hautes. Un
versant a mi-hauteur et un fond de vallee peuvent etre a la meme altitude, et
seul le second piege l'air froid. Meme lecon que la Phase 22 sur le relief.

Controle negatif, celui qu'employait deja la Phase 16 pour les points d'eau :
une BOSSE de geometrie identique doit obtenir un index quasi nul. Mesure :
creux 0,65, plateau 0,00, bosse 0,00.

L'inversion exige TROIS conditions simultanees (creux ferme, nuit, ciel clair et
calme) et le test verifie qu'elle **renverse le signe du gradient** — le fond,
150 m plus bas, doit etre plus FROID. Mesure : fond 2,1 C, plateau 3,6 C.

Sans carte reliee, l'inversion vaut zero partout et le reste fonctionne : repli
explicite et teste, pas une panne.

### LE CYCLE JOUR/NUIT VISUEL — l'incoherence de la Phase 16 est corrigee

Le commentaire de `GameClock` disait : *"cette horloge n'existe QUE pour la
temperature, et rien dans le rendu ne change avec elle"*. **Ce n'est plus
vrai.**

La lumiere directionnelle existante (Blinn-Phong, Phase 2) est alimentee par la
MEME position solaire que l'adret/ubac et les thermiques. Il n'y a donc toujours
qu'**un seul soleil** dans le jeu, et un test le verifie explicitement
(`dot(light.direction, sky.sunDirection) == 1`).

Chemin : `SynopticState` -> `sunLightFor()` -> `Renderer::setSun()` ->
`UniformBufferObject::sun` / `sunColor` -> les trois shaders de fragment, ou
`kLightDirection` et `kLightColor` etaient des constantes en dur depuis la
Phase 2.

**Deux details qui evitent des regressions de rendu :**

- `sunColor.a` est un **MULTIPLICATEUR** de l'ambiante, pas une valeur absolue.
  Les trois shaders ont des ambiantes differentes et justifiees (0,15 cube /
  0,18 terrain / 0,24 feuillage, qui laisse passer la lumiere) : les remplacer
  par une valeur unique aurait ete une regression deguisee en correction.
- Les valeurs **par defaut** de l'UBO et du Renderer reproduisent exactement
  l'ancien eclairage. Un rendu qui n'appelle pas `setSun()` est pixel pour pixel
  celui de la Phase 22.

**LE CIEL AUSSI, et il a fallu une capture pour s'en apercevoir.** La premiere
version ne touchait qu'a la lumiere. Le relief s'assombrissait correctement,
mais la couleur d'effacement restait le bleu vif de midi : la capture de 3 h du
matin montrait la silhouette NOIRE des sapins sur un ciel de plein jour. La
lumiere etait juste, l'image ne l'etait pas. `SunLight` porte donc aussi une
`skyColor`, avec sa propre palette (jour / crepuscule / nuit) parce que le ciel
n'est pas eclaire par le modele d'eclairage — c'est un fond. Le crepuscule occupe
la premiere moitie de la montee du soleil, ce qui fait DURER l'aube et le
coucher au lieu d'un fondu direct du noir au bleu.

C'est le seul defaut de cette phase qu'aucun test headless ne pouvait trouver :
il n'existait que dans l'image.

Quand le soleil passe sous l'horizon, la direction **bascule a la verticale** :
une directionnelle venue d'en dessous eclaire le relief par le bas, defaut
visuel classique d'un cycle bacle. Test : `direction.y >= 0` a toute heure.

**`nightAmbientFactor = 0,30` est un choix de JOUABILITE assume, pas une
mesure.** Une vraie nuit sans lune est noire ; un jeu d'exploration ou l'on ne
voit rien la moitie du temps n'est pas jouable tant qu'aucune source de lumiere
portable n'existe. A revoir avec le feu de camp.

### Deux options de ligne de commande, pour la meme raison que `--look`

`--hour=<h>` epingle l'instant de jeu, `--time-scale=0` **GELE l'horloge**.
C'est le pendant exact de `--lock-input` pour le TEMPS : sans elles, le cycle
jour/nuit rendait toute capture non reproductible, puisque les secondes de mise
en place deplacaient le soleil. `tools\shot_daynight.ps1` s'en sert pour
produire une serie d'images de la meme vue a des heures differentes.

### Effets gameplay : branches sur l'existant, jamais en parallele

Chaque nouveau parametre a une valeur par defaut NEUTRE, et un test de
non-regression verifie que l'omettre reproduit exactement — comparaison bit a
bit sur les positions — le comportement d'avant la Phase 23.

**Paraglider** — nouvelle struct `AirState` (vent, vertical, turbulence),
volontairement NUE et non un `WeatherSample` : Paraglider ne doit pas plus
dependre de la meteo qu'il ne depend de Vulkan (meme separation que
`HeightSampler`).
- `speed_` est une vitesse **AIR** ; la vitesse sol est cette vitesse plus le
  vent. 30 s a 6 m/s : **495 m vent arriere, 315 m en air calme, 135 m vent de
  face**, sans aucune regle particuliere.
- Dans une ascendance plus forte que le taux de chute, la somme est positive et
  l'aile MONTE : la definition du vol thermique sort de l'addition. 30 s :
  1462 m en air calme, 1553 m dans 3 m/s.
- Turbulence : deux sinusoides de periodes **incommensurables**, pas un tirage
  aleatoire — le vol reste deterministe donc rejouable, et deux periodes sans
  rapport simple ne se resynchronisent jamais, donc le chahut ne degenere pas en
  balancement regulier, qui est ce qui trahit une turbulence fabriquee. Ni
  fermeture ni decrochage : l'aile perd du cap et de la finesse, elle ne tombe
  pas.

**Climber** — adherence dans [0, 1] tiree de l'humidite par
`Climber::gripForWetness()` (methode de la classe, pour qu'il n'y ait qu'UNE
traduction humidite -> adherence dans le jeu). L'effet est **double, et c'est le
point** : la vitesse baisse ET le cout par metre monte (jusqu'a x 1,8). Sans le
second terme, un modele qui ne ferait que ralentir laisserait la jauge dire le
contraire — moins de metres, donc moins d'energie, donc une paroi mouillee
*moins* fatigante. Mesure sur 20 s : sec 19,3 m a 0,018 de jauge/m, mouille
8,9 m a **0,032**. Plancher d'adherence a 0,05 pour qu'une paroi ruisselante
reste grimpable au lieu de devenir une impasse ou le joueur serait bloque.

**Bike** — fait, en version minimale : une seule grandeur (humidite) et deux
coefficients branches sur des termes existants, **sans ajouter le moindre
etat**. Roulement +45 %, freinage -30 % sur sol detrempe. Mesure : equilibre a
plat 16,9 -> 15,9 km/h ; une demi-seconde de frein enleve 11,7 -> 8,8 km/h.
**Pas touche : l'adherence en VIRAGE** — le rayon de braquage est un equilibre
deja verifie par les tests de la Phase 13, y toucher sans mesure serait un pari.

### La mouillure du sol : RELUE, pas accumulee

Le seul endroit qui pourrait sembler avoir une memoire n'en a pas :
`groundWetness()` remonte le temps dans la fonction synoptique — qui est pure —
au lieu d'accumuler par image. Huit echantillons sur six heures de jeu, decrus
exponentiellement, on garde le plus mouille. Testee : un joueur trouve la meme
mouillure au meme endroit au meme instant, qu'il ait mis dix minutes ou deux
heures a venir.

### Cout

| | Cout |
|---|---|
| Evaluation complete (couche 1 + 2, 7 sondages, mouillure) | **4,0 us** |
| Couche synoptique seule | **0,38 us** |
| Carte de piegeage 128 x 128 | 393 216 `heightAt()`, **une fois au chargement** |

**Une seule** evaluation par image, au point du joueur — soit 0,024 % d'une
image de 16,7 ms. Le repere de la Phase 19 tient : le frame est domine par le
GPU (81 % dans `drawFrame` en Release), et la meteo n'avait aucune chance de
changer ca. Le test de cout porte un seuil TRES large (50 us) et volontairement :
il protege contre un facteur mille — un sondage devenu une recherche, une carte
recalculee par image — pas contre 20 %, que le bruit de mesure de cette machine
ne distingue pas (lecon de la Phase 17).

### Verification

- **Tests headless** : **3578 assertions**, toutes passantes. Deux sections
  neuves, `testWeather()` et `testWeatherGameplay()`.
- **Vulkan reel** : **cinq executions completes** de la suite. **Zero
  `[Validation]` et zero `[ERROR]` dans les cinq** ; 4 `[WARN]`, les memes
  qu'avant. **Chaque scenario a passe ses assertions de gameplay** sur le binaire
  final. Aucune execution n'a ete verte de bout en bout : les echecs residuels
  sont tous du cote du harnais, frappent un scenario different a chaque fois, et
  sont detailles juste en dessous.
- **Visuel** : `tools\shot_daynight.ps1`, six captures de la MEME vue a
  3 / 6,5 / 9 / 13 / 18 / 21 h, horloge gelee et cadrage epingle. La lumiere
  tourne d'est en ouest, l'intensite suit, le ciel passe du bleu de midi au bleu
  nuit. Le detecteur de ciel du script le chiffre tout seul : **20,6 % de ciel a
  13 h, 0,0 % a 21 h** - il ne compte que les pixels bleu clair, et la nuit il
  n'y en a plus.
- **Pas de capture adret/ubac**, et c'est une limite assumee : le jeu n'a aucun
  indice VISUEL de temperature. L'ecart est mesure par les tests (14,7 C contre
  10,7 C a midi), pas montre. Un indice visuel est note comme amelioration
  future.

### LE HARNAIS VULKAN : une regression a moi, et une flakiness enfin mesuree

**(a) Ma regression.** La premiere version journalisait DEUX LIGNES DE METEO PAR
SECONDE. Le harnais paie un script-block PowerShell PAR LIGNE de journal et son
retard CROIT avec le volume (dette ouverte en Phase 17, comprise en Phase 22).
Le scenario H, le plus long, a echoue et n'a collecte que **87 releves sur 220**.
Le jeu etait correct.

La cadence a ete resserree DEUX FOIS, et la valeur intermediaire est la lecon :

| Cadence | Lignes ajoutees au scenario H | Releves collectes |
|---|---|---|
| 2 / seconde | ~440 | **87** - echec |
| 2 / 10 s | ~44 | 235 puis **181** - echec marginal |
| **1 / 30 s** | **~7** | **236 / 240** |

La version a 10 s PASSAIT, mais de justesse, et un run ulterieur est retombe a
181 releves - assez pour perdre les derniers, donc pour que la recuperation de la
jauge (qui se lit dans les derniers) disparaisse. **Une correction qui passe de
justesse n'est pas une correction.**

**(b) La flakiness, desormais MESUREE et non supposee.** Il reste des echecs
"pas de sortie 20 s apres WM_CLOSE" qui frappent un scenario DIFFERENT a chaque
execution (H, puis E, puis D+G1+G3, puis H), sans jamais aucun `[Validation]` ni
`[ERROR]`. **Le jeu se ferme en 0,1 / 0,1 / 0,1 / 0,2 seconde** sur quatre essais
chronometres par `CloseMainWindow()` + `WaitForExit`. Un "pas de sortie apres
20 s" est donc un echec de LIVRAISON du message par le harnais, jamais un blocage
du jeu. Meme famille que la variance A/B/G deja documentee (`PostMessage`,
scancodes, pas de vraie souris) : les runs qui echouent montrent d'ailleurs des
assertions de gameplay a False EN AMONT - pas de decollage, pas d'entree en mode
editeur - donc des touches perdues, pas une fermeture ratee.

### Limites assumees

Pas de vraie ombre pluviometrique sous le vent (seulement l'absence de
renforcement) ; pas de nature de sol pour le velo (le LiDAR n'en porte aucune,
limite deja etablie en Phase 22) ; pas d'adherence en virage ; ni fermeture ni
decrochage en parapente ; `snowing` calcule et journalise mais **sans aucun
effet** gameplay ni visuel ; **aucune precipitation visible** — il pleut dans les
chiffres, pas a l'ecran ; pas d'ombres portees ; pas d'etoiles ni de lune ; l'ondulation de
l'herbe (Phase 18) est toujours pilotee par un temps qui defile et non par le
vent local, alors que celui-ci est desormais disponible en tout point ; modele
solaire sans equation du temps ni refraction (au plus une quinzaine de minutes
d'erreur sur le lever et le coucher, invisible dans une horloge acceleree douze
fois).


---

## PHASE 24 (2026-08-27) - coherence marche/escalade, et une fausse flakiness

Voir `NOTES-PHASE24.md` pour le detail complet.

### Chantier 1 : la marche a enfin une limite de pente

Jusqu'ici `Camera` en mode `Walk` n'avait AUCUNE limite : un versant a 90 %,
voire une paroi a 300 %, se montait debout a 14 m/s. Le vrai probleme n'etait pas
le realisme, c'etait que **l'escalade devenait facultative** - une activite
entiere que rien n'obligeait jamais a employer.

`src/core/SlopeLimits.hpp` (neuf, header-only) porte deux seuils PARTAGES :

- `kWalkComfortSlope` = **0,60** (31 deg) - fin de la pleine vitesse. Ce n'est
  pas une valeur neuve : c'est `ClimberParams::walkableSlope`, deja justifiee en
  Phase 14. Les trois lecteurs lisent desormais la meme constante.
- `kWalkMaxSlope` = **0,84** (40 deg) - plus aucune progression debout. C'est la
  borne haute de l'angle de repos des materiaux meubles, la grandeur que la
  Phase 14 invoquait deja pour placer `wallSlope`. Vitesse accordee : 100 % a
  60 %, 50 % a 72 %, 17 % a 80 %, 0 % a 84 %. La randonnee raide passe intacte.

Trois regles au-dela : rampe progressive (pas de mur sec), **redescendre est
toujours accorde a pleine vitesse** (sans quoi un joueur pose sur du trop raide
serait fige), et **glissade** vers l'aval (0,48 m/s a 100 % de pente, plafonnee
a 6 m/s). La glissade est la garantie de fond : aucune position n'est un piege.

**LE VRAI BUG DE COHERENCE, et sa correction structurelle.** L'escalade ne
s'engageait qu'a 1,00 : une limite de marche a 0,84 aurait ouvert une bande
[0,84 ; 1,00[ - environ **5 % du massif** - ou l'on ne pouvait NI marcher NI
grimper. `ClimberParams::wallSlope` vaut desormais `kWalkMaxSlope`, **la meme
constante** : il n'y a plus deux nombres a faire diverger. La part grimpable de
la zone passe de 8,4 % a ~13 %. `WildlifeParams::maxSlope` descend de 0,90 a
0,80 pour garder le rapport "aucun animal sur une pente grimpable".

Verifie sur le relief LiDAR reel (voie de reference E 899900 / N 6435550) : la
marche progresse de 1,4 m puis s'arrete sur du 81 %, et `Climber::tryStart()`
accepte **depuis ce point exact**. Le 1,4 m paraissait suspect : le profil
mesure montre 57 % -> 89 % en deux metres. C'est le terrain, pas le test.

Tests : **3608 assertions** (3578 avant, +30 neuves), zero regression.

### Chantier 2 : ce n'etait pas de la flakiness

Le delegue .NET compile demande par la consigne **existait deja** (Phase 22) -
et c'est ce qui a masque la suite. Trois executions ont donne le meme echec au
detail pres sur les scenarios A et C : **defaut DETERMINISTE**, classe
"aleatoire" depuis des phases, d'ou l'inefficacite d'elargir des delais.

Deux controles negatifs ecartent le chantier 1 : limite de pente entierement
neutralisee -> echec identique ; et dans l'execution de reference le filtre ne
s'est **declenche aucune fois**.

Cause reelle : `GoTo` naviguait **a l'estime** et declarait le joueur arrive
sans rien verifier ; l'erreur s'accumulait d'un `GoTo` au suivant. Tout le
scenario A tombait d'une seule cause. Correctif **sans toucher au jeu** :
`main.cpp` journalise la position de la camera une fois par seconde depuis la
Phase 15 - le harnais ne s'en servait pas. Boucle fermee, tolerance 0,6 m.
Scenario A : arbre abattu au lieu de manque, **7-8 objets ramasses au lieu de
1**, percussion 1 au lieu de 0.

Limite du canal d'entree, mesuree : la boucle **plafonne a ~0,8 m** (`Hold`
refuse tout pas sous 0,28 m, `Start-Sleep` granularite ~16 ms). Deux executions
du meme binaire ont donne 0,78 m (abattu) et 0,80 m (manque) - d'ou
`Fell-WithRetry`, un balayage lateral, comme `Vacuum` le fait deja au ramassage.

**Une regression de la phase, trouvee et corrigee** : le journal "sur changement
d'etat" du chantier 1 CHATTE quand le joueur longe le seuil - 494 lignes, et le
scenario I perdait 40 % de sa telemetrie. Limiteur de cadence (1 s) : 30 lignes,
scenario I retabli. Un etat qui BAT n'est pas un evenement.

Diagnostics permanents ajoutes : le **journal brut du jeu ecrit sur disque** a
chaque execution (HORODATE - le premier jet ecrasait le precedent, et le run 6 a
efface la seule trace de l'echec du run 5), et un report des declenchements du
filtre de pente.

**RESULTAT SUR PLUSIEURS SUITES COMPLETES, et il faut le dire tel quel :**
run 4 **VERT** (la premiere suite verte de bout en bout du projet), run 5
**ECHEC** (scenario C : arbre manque ; scenario I : 48 releves au lieu de 216),
run 6 **VERT**. Donc **2 verts sur 3** : le critere "zero faux-negatif sur
plusieurs runs consecutifs" **n'est PAS atteint**. Le defaut DETERMINISTE, lui,
est bien corrige - le scenario A est "pilotage precis" dans les trois runs.
Ce qui reste est une flakiness authentique, et le journal horodate est ce qui
permettra de la trancher a la prochaine occurrence.

---

## PHASE 25 (2026-08-27) - l'adret/ubac devient VISIBLE

Voir `NOTES-PHASE25.md` pour le detail complet.

L'ecart adret/ubac etait calcule et journalise depuis la Phase 23 (14,7 C contre
10,7 C a midi) et **strictement invisible a l'ecran** - limite que la Phase 23
avait notee elle-meme.

**Piste retenue : teinte du terrain**, parce que c'est la seule des trois qui
reutilise la grandeur existante telle quelle. `shaders/terrain.frag` avait deja
les deux moities du calcul de `WeatherSystem::sampleAt` : la normale par fragment
et `ubo.sun.xyz`, la direction solaire de Weather descendue jusqu'au rendu en
Phase 23. **Rien a recalculer, seulement a consommer.**

Partage des roles : le CPU porte dans `ubo.terrain.w` ce qui appartient a la
meteo (hauteur du soleil, nebulosite) ; le shader ne fait que le cosinus
d'incidence par fragment. `terrain.w` etait "reserve" depuis la Phase 11 :
**aucun octet d'uniforme ajoute, aucune allocation**.

Consequence gratuite : la coherence avec le cycle jour/nuit ne demande AUCUNE
regle horaire. Le soleil tourne, l'incidence suit ; il se couche, l'effet
s'eteint.

**Parametres** : teinte adret `(1.14, 1.04, 0.88)`, ubac `(0.86, 0.96, 1.12)`,
exactement symetriques autour de 1 (sinon tout le plat du massif serait teinte),
ecart de reference 0,35 = `WeatherParams::aspectIncidenceReference`, intensite
maitresse `kAspectTintIntensity` = 1,0 dans `src/main.cpp`.

**L'amplitude a ete corrigee apres mesure** : a +/- 7 %, 93,5 % des pixels ne
bougeaient pas d'un cran sur 255 a 13 h - donc un indice invisible. Le SIGNAL
reste celui de la meteo ; seule l'amplitude d'AFFICHAGE a change.

**Verification.** Belvedere trouve par un vrai calcul de VISIBILITE (la premiere
tentative, qui ne comptait que les pentes presentes dans le cone, a donne un
aplat a 0 % de ciel) : **E 900000 / N 6436000, alt 1950 m, `--look=270,-24`**,
108 points d'adret et 107 d'ubac VISIBLES.

| heure | rechauffes | refroidis | inchanges |
|---|---|---|---|
| 09 h | 3,2 % | 52,2 % | 44,6 % |
| **13 h** | **20,4 %** | **29,0 %** | 50,6 % |
| 18 h | 45,8 % | 6,0 % | 48,2 % |
| 22 h | 2,0 % | 0,0 % | 98,0 % |

Le critere n'est pas "les images different" - un filtre global donnerait ca
aussi - mais la **bipolarite dans la meme image**. Plancher de bruit mesure en
capturant deux fois le MEME binaire : 0,6 % a 13 h, 1,2 % a 22 h. L'effet est
donc **80 fois le bruit a midi**, et **dans le bruit a 22 h** - reellement eteint
la nuit, le residu etant l'herbe animee.

**Cout** : `drawFrame` 665 us contre 672 us au temoin (bruit, et dans le sens
d'un gain) ; memoire GPU strictement identique. **Zero `[Validation]`** sur les
huit lancements de capture.

Pistes NON retenues : la **vegetation differenciee par versant** demande une
distinction d'especes qui **n'existe pas** (un seul type d'arbre) - c'est un
prerequis manquant, pas un reglage, a traiter avec le chantier 4 ; l'**affichage
de la temperature** etait le filet de securite, inutile puisque la piste 1 a
tenu (et la temperature est deja dans le titre de la fenetre).

Limites assumees : indice **non quantitatif** ; **terrain seulement** (l'herbe et
les arbres ne sont pas teintes) ; **pas d'ombres portees** (herite de la Phase
23) - un versant a l'ombre d'une crete est traite comme s'il voyait le soleil ;
l'effet **sature** au-dela de 0,35 d'ecart d'incidence, exactement comme la
temperature.

## PHASE 27 (2026-09-24) - meteo visible, micro-relief du sol, profil de performance

Detail complet : `NOTES-PHASE27.md`. Tests 3636/3636, 0 `[Validation]`.

**Meteo visible** : une seule fonction pure, `weatherVisualsFor(sky, local,
params)` (world/Weather), traduit en image ce que Weather calculait deja, au
point du joueur. **Tracabilite** (aucune mecanique inventee) :
- soleil direct voile <- `cloudCover` local, loi `cloudAspectDamping` (reutilisee,
  la meme que l'adret) ; ambiante diffuse x(1 + 1,0 x nuages x jour) ; ciel
  desature vers sa luminance x0,80 <- `cloudCover` ;
- pluie/neige plein ecran (densite, opacite) <- `precipitationMmPerHour` local /
  `precipitationMaxMmPerHour` ; neige <- `snowing` (temperature) ; inclinaison
  <- `windVector` local + vitesse terminale 6,5 / 1,2 m/s ;
- brouillard exp(-sigma d), sigma = 0,25 R^0,63 /km (x8 en neige) <-
  precipitation locale ; **nul sans precipitation** ; couleur = ciel ;
- sol fonce x(1 - 0,35 x `wetness`).
Coefficients d'affichage PROVISOIRES regroupes dans `WeatherParams` section
6 ter. Ciel clair + sol sec = lumiere de la Phase 23 AU BIT PRES (teste).
Prerequis manquants documentes, non implementes : humidite/point de rosee
(brume de vallee), base des nuages, phase mixte pluie/neige (la bascule est un
saut du modele, isole par le test), manteau neigeux, nebulosite spatialisee.

**Micro-relief** : bruit en TEXTURE (256 px, 32 cellules, mips, binding 2)
perturbant la normale + l'albedo, 7 octaves de 0,61 m (texel ortho) a 5 mm,
amplitude pilotee par le contraste local de l'ortho et la pente LiDAR, fondu
5 -> 20 m + fondu de Nyquist par octave. HF du sol proche +22 %, lointain
inchange. Non verifie EN MOUVEMENT (scintillement).

**Outils** : `--day=N`, `--no-weather-visuals`, `--no-ground-detail` (temoins
etroits), `tools/shot_weather.ps1` (captures + journal par image),
`tools/profile_phase27.ps1` (A/B entrelaces, sante machine), ligne
`Cadence de boucle (PHASE 27)` en fin de partie.

**Performance (Release, UHD 770)** : toujours GPU-bound (drawFrame 75-85 %) ;
Debug CPU-bound (herbe 44 %). Pluie +608 us (accepte, seulement quand il
pleut) ; micro-relief v1 +1 683 us rejete, bissection -> ~200 us PAR OCTAVE
d'arithmetique ; v3 texture +480 us en vue de marche. **Un poste CPU n'est pas
un temoin valide pour un changement GPU sur GPU integre** (budget de puissance
partage : herbe +32 % des que le GPU charge plus).

## PHASE 28 (2026-09-27) - faconnage generique (Agent 2 : Foyer, Ligature, Hache, Poix)

Detail complet : `NOTES-PHASE28.md` (conception : `NOTES-PHASE28-DESIGN.md`).
Tests **3807/3807** (3636 avant), 0 `[Validation]` au demarrage.

- `world/Crafting` : table `constexpr kRecipes` (8 recettes), `craftAt` sur la
  touche **C** (premiere recette satisfaite ; ingredients PORTES ou POSES a
  2,5 m ; poste = foyer a 2 m ; tout verifie avant retrait ; produit au sol).
  La percussion est la derniere recette ; `knapStones` reste, inchange.
- `HearthSet` : combustion en heures de JEU, 1 h (+1 h par branche/buche
  posee), extinction => 1 cendre ; foyer fixe (`groundItemIsPortable`),
  rallumable.
- Nouveaux `GroundItemKind` en fin d'enum : Cord, Axe, Hearth, Ash, Pitch
  (`kGroundItemKindCount` = 13), meshes `world/CraftMesh`.
- **P** = reposer au sol (dernier type ramasse). Coupe en demo/vol/velo/escalade.
- (Agent 2) effets non branches -> faits par l'Agent 3 ci-dessous : `HearthSet::warmthAt` pret pour
  `ambientTemperatureC` ; hache = `inventory.count(Axe)` pour le rendement.
- **Agent 3 (effets branches)** : tests **3877/3877** (3807 avant), 0 `[Validation]`.
  Foyer : `ambientTemperatureC += hearths.warmthAt(pos)` (+8 C centre -> 0 a 6 m).
  Hache : `Forest::setBuckingBonusLogs(axeBonusLogs(inventory))` +1 buche ;
  hache renforcee (`pitched_axe` = hache + poix posees, `PitchedAxe`) +2.
  Abri (`shelter` = 4 buches + 6 branches + 2 ligatures posees, `Shelter` fixe) :
  `needsContext.sheltered = shelterNear(...)` a 2,5 m (+5 C, vent annule).
  10 recettes, `kGroundItemKindCount` = 15. Abattage jamais conditionne.
  NON fait : collision de l'abri, scenario Vulkan K, pot (P7). Voir NOTES-PHASE28.md.
- **Agent 4 (verification, 2026-09-27)** - detail : `NOTES-PHASE28.md` fin.
  MESURE : headless **3877/3877** ; build Debug propre 82/82, **0 avertissement**
  (le seul `/W4` des tests, `main.cpp` C4244 l.1280, date de la Phase 27) ;
  `run_vulkan_check.ps1` complet x2 : **0 `[Validation]`, 0 `[ERROR]`**, 4 `[WARN]`
  habituels, tout vert SAUF C (arbre "MANQUE" : navigation arrivee a 0,82/0,84 m,
  limite connue ~0,8 m du harnais) ; `-Only C` ensuite : **vert**. Aucun passage
  complet vert de bout en bout. Cadence Debug par processus vs journal brut de la
  Phase 27 : 13/14 egaux ou plus rapides, 1 a +1 % -> pas de regression (Release
  NON mesure). Relecture du diff vs sauvegarde : Weather/Needs/faune/collision/
  activites/pierre intouches ; aucun bug trouve, aucun code modifie.
- **Recettes retenues (10, ordre = priorite)** : shelter (4 buches+6 branches+
  2 ligatures posees -> abri fixe, sheltered), hearth (3 pierres+2 branches posees,
  eclat porte outil -> foyer allume), hearth_relight, pitched_axe (hache+poix ->
  +2 buches), axe (eclat+branche+ligature posees -> +1 buche), pitch (2 resines+
  1 cendre, foyer allume), cord (3 fibres portees), hearth_feed_log/branch (+1 h),
  knap (Phase 6, au bit pres). Justification : chaque objet branche un systeme
  EXISTANT (Needs ambiante/sheltered, rendement Forest) ; le foyer existe parce que
  la poix exige de la cendre ; couteau ecarte (rien a influencer).
- **Architecture** : `world/Crafting` sans Vulkan, table `constexpr kRecipes`,
  `craftAt` = premiere recette satisfaite (porte = Inventory, pose = Ground a
  2,5 m, poste = foyer a 2 m), verification avant retrait, produit au sol ;
  `HearthSet` hors Ground, heures de jeu ; touches C (faconner) et P (reposer).
- **Pas fini** : collision de l'abri, pot d'argile (P7), lumiere du foyer,
  scenario Vulkan K, fiabilite de navigation du scenario C.

## PHASE 29 (2026-09-27) - Agent 2 : rochers et pierriers (detail : NOTES-PHASE29.md)

- `src/world/Rocks.{hpp,cpp}` + `RockMesh.{hpp,cpp}` : semis deterministe sous le
  masque de NOTES-PHASE29-DESIGN.md (Climber::slopeAt 0,60-0,84, couvert < 0,10),
  rugosite >= 0,10 m -> gros blocs/rochers, sinon pierraille. Sans carte de
  couvert : ZERO rocher. Masque en jeu : 8,4 % de la zone.
- Rendu instancie (Material::Foliage), mode rocher de `foliage.vert` si
  `params.w < 0` : gradient encode dans la partie entiere de `variation`.
- Collision : gros blocs >= 1,3 m seulement ; `--no-rocks`, `--no-rock-collision`.
- Tests **3930/3930** (+53). Cout Debug : +85-89 us/image, RAM +1 Mo, 0 `[Validation]`.
- `tools\profile_phase29.ps1` : A/B entrelace, arret par PID exact (profile_phase27
  tue PAR NOM, a eviter). PIEGE : `run_vulkan_check.ps1 -Only E,F` via `-File` ne
  joue RIEN ; passer par `-Command "& .\tools\run_vulkan_check.ps1 -Only 'E','F'"`.

## PHASE 29 (2026-09-27) - Agent 3 : buissons et petits arbres (detail : NOTES-PHASE29.md)

- Cartes classe 3 COPIEES dans `assets\terrain\vercora_zone_canopy_lowveg.vchm` et
  `_canopy_lowveg_height.vchm` (schema `canopyPathFor(path, "_lowveg")`), sources intactes.
- `src/world/Bushes.{hpp,cpp}` + `BushMesh.{hpp,cpp}` : critere F3 >= 0,10 ET couvert-F3 < 0,20
  ET couvert >= 0,10 (=> disjoint des Rochers par construction) ; hors houppier des arbres
  Forest ; densite lineaire en F3, hauteur = h max c3 x U[0,35;0,85] bornee 0,5-3 m ;
  >= 1,8 m -> petit arbre. Une carte absente -> ZERO buisson.
- Rendu : chemin ARBRE de `foliage.vert` (w = 0), shader NON modifie ; `--no-bushes` temoin.
- Adret/ubac NON applique (buis hors etage, rhododendron = sol acide, aulne vert silicicole).
- Collision : aucune (traversables). Tests **3968/3968** (+38).
- Cout Debug : alpage +135 us/img (+2,0 %), zone dense +322 us (+5,1 %), RAM +8 Mo, 0 `[Validation]`.
- `profile_phase29.ps1 -View buissons -Toggle --no-bushes` (defaut inchange).

## PHASE 29 (2026-09-27) - Agent 4 : verification (detail : NOTES-PHASE29.md fin)

- MESURE : build Debug clean 86/86, 0 avertissement (seul C4244 main.cpp l.1297 = l.1280
  Phase 27 decalee) ; headless **3968/3968** ; `run_vulkan_check.ps1` complet :
  **0 `[Validation]`**, tout vert SAUF C (0,81/0,83 m, comme en Phase 28) ; `-Only 'C'` x3 :
  1 echec, 2 verts. Sur les dalles LHD (scenarios A a I) : ZERO rocher/buisson (pas de couvert),
  donc la Phase 29 ne peut pas y jouer ; J3 (zone) les a et passe.
- Placement : rochers = Climber::slopeAt 0,60-0,84 ET couvert < 0,10 ; rugosite >= 0,10 m
  -> blocs, sinon pierraille ; trainees = bruit anisotrope 110 m / 28 m. Buissons = F3 >= 0,10
  ET couvert-F3 < 0,20 ET couvert >= 0,10 (disjoints des rochers), hors houppier Forest.
- Densite : blocs 8 / 1024 m2, rochers 20 / 1024 m2, pierraille 12 plaques / 64 m2 ;
  buissons 6 / cellule de 8 m. Journal actuel : masque rochers 9,4 %, 1,82 blocs, 4,66
  rochers, 23,2 plaques / ha (les 8,4 % / 46 de l'Agent 2 precedent une retouche de Rocks.cpp) ;
  buissons 9,7 %, 21,7 buissons + 31,0 petits arbres / ha.
- Cout TOTAL (A contre `--no-rocks --no-bushes`, 3 tours entrelaces) : Debug alpage +164 us
  (+2,5 %), pierrier +252 (+8,8 %), buissons +456 (+7,6 %) ; **Release** (`build-release\`
  separe) alpage +124 us (+2,6 %) = 4 954 us, au niveau de la ref. Phase 27 (4 962) ;
  pierrier +70 us ; zone dense de petits arbres **+676 us (+13 %)**, 170 img/s, GPU-bound.
  RAM +8-9 Mo.
- Limites : trainees NON visibles a l'oeil (blocs epars) ; petits arbres voisins
  s'interpenetrent ; arbres de decor Vegetation non exclus ; rien de mesure en mouvement.
- Relecture : seuls main.cpp et foliage.vert modifies ; Forest/Vegetation/faune/
  artisanat/collision/activites identiques octet pour octet ; chemin w >= 0 du shader inchange.
- Outil : `build-release\` existe desormais (profil Release hors du `build\` Debug).

## PHASE 30 (2026-10-13) - optimisation des ressources (detail : NOTES-PHASE30.md)

Session cloud (Linux, sans GPU ni donnees LiDAR) : mesure sous **lavapipe +
Xvfb** (rendu logiciel DETERMINISTE : deux runs identiques = 0 pixel
d'ecart, donc preuves au bit pres possibles) sur une **zone proxy** a la
vraie echelle (`tools/make_bench_zone.py` : relief fBm 5001 x 5001, georef
et altitudes de la vraie zone, couvert tire de l'orthophoto versionnee).
Les chiffres sont comparables avant/apres, PAS a ceux de l'UHD 770.

- **Build Linux** : GCC 13 / Clang 18 (`= {}` sur struct imbriquee ->
  surcharges, CWG 1397 ; `GLFW_INCLUDE_NONE`). `cmake -G Ninja
  -DGLFW_BUILD_WAYLAND=OFF`. Tests 3 963 / 3 963 (+20).
- **Instrumentation** : toute memoire GPU via `VulkanContext::allocateMemory`
  (comptee), RAM via `core/ProcessMemory`, ligne `Ressources (PHASE 30)` au
  chargement / toutes les 10 s / en fin de partie. Options `--quit-after=S`,
  `--freeze-anim`, `--force-staging`, `--vsync`.
- **Geometrie** : `Vertex` (68 o) reste le format CPU ; GPU = `ObjectVertex`
  44 o (exact) et `TerrainVertex` 16 o (x/z/UV/morph recalcules par
  terrain.vert depuis `gl_VertexIndex` + `DrawItem::grid`, `precise`).
  Topologie des tuiles partagee en uint16. `GpuBufferPool` (blocs 4 Mo) :
  ecriture DIRECTE en memoire unifiee, staging + barriere VERTEX_INPUT ->
  TRANSFER sinon. Tuiles interieures construites sans relecture (bit a bit).
- **Rendu** : terrain dessine en DERNIER (early-Z) + culling de ses faces
  arriere. Orthophoto lue par tranches dans un staging de 4 Mo reutilise.
- **Bilan survol 900 m/s** : pic GPU 285 -> 87 Mo, terrain 188 -> 33 Mo,
  vkAllocateMemory ~17 000 -> 22, streaming 4,3 -> 0,93 ms/image.
  Foret : pic GPU 103 -> 56 Mo, +44 % d'images/s (lavapipe). Chargement :
  pic RAM -68 Mo, pic GPU 89 -> 50 Mo.
- **Image** : quantification des normales (+/- 1 niveau sur ~0,04 % des
  pixels), egalites de profondeur aux pieds des brins/troncs (24 px), et
  ~250 px isoles sur parois rasantes (culling). Staging, parallele, ortho,
  tuiles rapides : identiques au bit pres a ce qu'ils remplacent.
- **Validation** : 0 message (standard ET synchronisation, chemins direct et
  staging). Controle negatif : sans la barriere, 414 SYNC-HAZARD en survol.
- **PIEGE** pour les comparaisons d'images : la FAUNE bouge en temps reel ;
  comparer avec `--no-wildlife`, sinon un animal deplace fait 6-11 pixels.
- **A faire sur la machine de dev** : re-mesurer en Release sur l'UHD 770
  (profile_phase27/29.ps1, run_vulkan_check.ps1 complet).
