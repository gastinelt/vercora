# Phase 28 - Faconnage generique et diegetique (implementation, Agent 2/4)

Session sans supervision du 2026-09-27. Conception de reference :
`NOTES-PHASE28-DESIGN.md`. Sauvegarde des sources avant la phase :
`_Vercora_backups\avant-phase28-20260927-0403`.

**Resultats** : tests headless **3636 -> 3807** (+171), tous verts.
Build Debug OK. Lancement court du jeu : **0 `[Validation]`** au demarrage
(PID arrete explicitement). `run_vulkan_check.ps1` et mesures de performance
NON lances (role de l'Agent 4).

## Architecture reelle

- **`src/world/Crafting.hpp/.cpp`** (sans Vulkan) : generalisation de
  `knapStones`.
  - `kRecipes` : table `constexpr` de 8 recettes `{id, label, in[3], inCount,
    hasOutput, out, station, effect, successMessage}`. Ingredient =
    `{kind, count, source Carried|Placed, consumed}` (`consumed=false` = outil).
  - `craftAt(inventory, ground, hearths, pos, facing, nowHours)` : collecte
    les objets POSES portables a `kCraftReach = 2,5 m` (nouvelle
    `Ground::indicesWithin`), les postes (foyer allume / eteint a
    `kStationReach = 2 m` du joueur), puis execute la PREMIERE recette
    satisfaite. Verification complete avant tout retrait ; retraits Ground par
    indices decroissants ; produit au barycentre des ingredients poses
    consommes, sinon a 1,1 m devant (meme projection/repli que knapStones).
  - Refus : recette "la plus proche" = moins d'unites manquantes, puis plus
    d'unites deja reunies, puis la plus simple (plus loin dans la table). Le
    message liste ce qui manque ("a porter" / "a poser a moins de 2,5 m" /
    "un foyer allume ... (il faut chauffer)"). Si la plus proche est la
    percussion, le message historique `knapResultMessage(NotEnoughStones)`
    est rendu mot pour mot.
  - `HearthSet` : etat de combustion A PART du Ground (modele ResinStand).
    Foyer identifie par sa POSITION (les indices Ground ne sont pas stables).
    Temps = heures de JEU (`GameClock::totalHours`). `update()` eteint les
    foyers echus et depose **une cendre** au centre. `relight`, `feed`,
    `remainingHours`, `nearest`.
  - `chooseKindToPutDown` / `putDown` : le geste "reposer".
- **`src/world/CraftMesh.hpp/.cpp`** : 5 meshes bas-poly generes en code
  (ligature = anneau, hache = manche + lame + ligature, foyer = 3 pierres +
  2 branches croisees + fond noirci, cendre = tas gris plat, poix = galette
  brun-noir). Dimensions absolues, bas du mesh a -rayon (teste).
- `knapStones` (Knapping.cpp) **inchange** : facade testee ; la recette
  `knap` en reproduit le resultat au bit pres (test dedie).

## Recettes (ordre = priorite)

| id | ingredients | poste | produit / effet |
|---|---|---|---|
| `hearth` | 3 pierres posees + 2 branches posees (consommees), eclat PORTE (outil) | - | Foyer (fixe) ALLUME, 1 h de jeu |
| `hearth_relight` | 2 branches posees + eclat porte (outil) | foyer eteint | foyer rallume 1 h |
| `axe` | eclat + branche + ligature, POSES | - | Hache (au sol, ramassable) |
| `pitch` | 2 resines + 1 cendre, POSEES | foyer allume | Poix |
| `cord` | 3 fibres PORTEES | - | Ligature a 1,1 m devant |
| `hearth_feed_log` | 1 buche posee | foyer allume | +1 h de combustion |
| `hearth_feed_branch` | 1 branche posee | foyer allume | +1 h de combustion |
| `knap` | 2 pierres portees (1 consommee, 1 outil) | - | Eclat (Phase 6) |

## Nouveaux `GroundItemKind` (ajoutes EN FIN d'enum)

`Cord, Axe, Hearth, Ash, Pitch` ; `kGroundItemKindCount` 8 -> **13**. Mis a
jour : rayons + 3 libelles (`Ground.cpp`), table `groundMeshes` (`main.cpp`),
test de la table de rendu (`testDrawListConstruction`), nouvelle fonction
`groundItemIsPortable` (faux pour `Hearth` seul) utilisee par `findNearest`.
`Inventory::summary()` : les 8 matieres toujours affichees (comportement et
test anterieurs), les objets faconnes seulement si leur compteur est non nul.

## Controles

- **C** : faconner (inchange pour la percussion : deux pierres en poche et
  rien de pose autour => meme resultat, memes messages).
- **P** (nouveau) : reposer au sol, 0,9 m devant, le dernier type ramasse s'il
  en reste, sinon la premiere nature portee. Decalage en spirale d'angle d'or
  (0,25 m x sqrt(n)) pour ne pas superposer les objets. Coupe en mode demo,
  en vol, a velo, en escalade et en editeur, comme C et R.
- Combustion des foyers avancee a chaque image si l'horloge avance (horloge
  gelee => feu fige) ; extinction journalisee.

## Decisions (non specifiees, prises par defaut)

1. **Touche P et non "R maintenu"** (proposition du §4 de la conception) : le
   ramassage se declenche a l'APPUI de R ; un maintien aurait d'abord ramasse
   puis repose. P est a la meme position en AZERTY et QWERTY.
2. **Foyer eteint = reste en place** (objet fixe) et peut etre **rallume**
   (recette `hearth_relight`) : sans cela le test "poix refusee foyer eteint"
   n'aurait pas d'objet, et un cercle de pierres qui disparait serait
   invisible pour le joueur. Chaque extinction laisse UNE cendre.
3. **Portee du poste mesuree depuis le joueur** (2 m), pas depuis les
   ingredients. Plus simple a lire en jeu et a tester.
4. **Alimentation** : n'importe quelle branche/buche POSEE a 2,5 m du joueur
   avec un foyer allume a 2 m (le "1 m du foyer" de la conception n'a pas ete
   retenu : une seule regle de portee). Consequence documentee : une branche
   isolee posee pres d'un feu est brulee au prochain C si aucune recette plus
   complexe n'est satisfaite.
5. **Duree 1 h de jeu, +1 h par alimentation** (valeurs proposees par la
   conception), dans `HearthParams`. Pas de plafond.
6. **Pas de lumiere ni de flamme** : un seul mesh de foyer, allume ou eteint
   (le renderer n'a pas de lumiere ponctuelle, §4 de la conception).
7. **Pas de sauvegarde** (aucune n'existe). `Recipe::id` stables et temps en
   heures de jeu pour la suite.
8. `CraftMesh.cpp` recopie un helper de facette plutot que de modifier
   `HarvestMesh.cpp` (zero risque de regression sur les meshes Phase 22).
9. Aucun fichier de `data/` touche.

## Points d'accroche pour l'Agent 3 (NON branches)

- **Chaleur** : `HearthSet::warmthAt(pos)` (pure, testee : 8 C au centre,
  lineaire jusqu'a 0 a 6 m, 0 si eteint ; `HearthParams::warmthC/warmthRadius`).
  A ajouter dans `main.cpp` apres `needsContext.ambientTemperatureC =
  localWeather.temperatureC;` : `+= hearths.warmthAt(playerPosition)`.
  `hearths` est declare dans `main.cpp` a cote de `inventory`.
- **Hache** : `inventory.count(GroundItemKind::Axe) > 0` ; `Forest::scheduleBucking`
  n'a PAS ete modifie (bonus de buches a ajouter, defaut 0).
- **Abri** : rien (recette P6 non implementee) ; `groundItemIsPortable` est
  pret pour un futur `Shelter` fixe.
- Scenario Vulkan K (conception §3.7) non ecrit.

## Fichiers touches

Nouveaux : `src/world/Crafting.hpp`, `src/world/Crafting.cpp`,
`src/world/CraftMesh.hpp`, `src/world/CraftMesh.cpp`, `NOTES-PHASE28.md`.
Modifies : `src/world/GroundItem.hpp`, `src/world/Ground.hpp`,
`src/world/Ground.cpp`, `src/world/Inventory.hpp`, `src/main.cpp`,
`CMakeLists.txt`, `tools/run_logic_tests.bat`, `tests/logic_tests.cpp`,
`MEMORY.md`.

## Tests ajoutes (`tests/logic_tests.cpp`)

`testCraftingRecipeTable` (ids, ordre, recettes, rayons/libelles/meshes de
toutes les natures, resume), `testCraftingKnapUnchanged` (percussion = knapStones
au bit pres, message de refus historique, regard vertical sans NaN),
`testCraftingCord`, `testCraftingHearth` (construction, eclat conserve,
barycentre, non ramassable, combustion, extinction + cendre, rallumage,
alimentation, chaleur), `testCraftingAxe` (ingredients non contigus
entrelaces, refus octet pour octet, porte vs pose, portee 2,6 m),
`testPitchNeedsLitHearth` (sans foyer, foyer eteint, foyer trop loin,
succes et consommation exacte), `testPutDown`. `testStoneKnapping` et
`testNeeds` inchanges.

---

# Agent 3/4 - effets branches (2026-09-27)

**Resultats** : tests headless **3807 -> 3877** (+70), tous verts. Build Debug
OK (tests + jeu). Lancement court (25 s, PID arrete explicitement) :
**0 `[Validation]`**. `run_vulkan_check.ps1` et mesures NON lances (Agent 4).

## Effets branches (tous sur des systemes EXISTANTS)

| Objet | Branchement | Chiffres |
|---|---|---|
| Foyer allume | `main.cpp` (bloc besoins) : `needsContext.ambientTemperatureC += hearths.warmthAt(playerPosition)` juste apres l'ambiante meteo | +8 C au centre, lineaire -> 0 a 6 m, somme des foyers allumes, 0 si eteint |
| Hache | `forest->setBuckingBonusLogs(axeBonusLogs(inventory))` avant `forest->update` ; `Forest::scheduleBucking` ajoute les buches | +1 buche par arbre (3..5 au lieu de 2..4) |
| Hache renforcee (P5, NOUVELLE) | recette `pitched_axe` : hache + poix POSEES ; meme branchement | +2 buches (pas de cumul avec la hache : le meilleur outil) |
| Abri (P6, NOUVEAU) | recette `shelter` : 4 buches + 6 branches + 2 ligatures POSEES ; `needsContext.sheltered = ground && shelterNear(*ground, playerPosition)` (premier appelant) | abrite a <= 2,5 m horizontal, tolerance verticale 3 m ; effet Needs existant : +5 C ressentis et vent annule |

**Interaction ambiante / Needs verifiee dans le code** (`Needs.cpp:171-205`) :
l'ambiante recue devient `lastAmbientC_` (journal "ambiante"), puis refroidissement
eolien (`windChillC`, seulement < 10 C et vent > 4,8 km/h, annule si abrite),
+ effort, - epuisement, + 5 C si abrite, puis marge au confort (12 C). La chaleur
du foyer passe donc par la MEME formule que la meteo, aucune seconde formule.
Le foyer rechauffe l'air AVANT le vent (le vent mord moins sur un air plus chaud).

## Decisions

1. **Poix** : la conception prevoit P5 (hache + poix -> +2 buches). Implementee
   comme recette + nouvelle nature `PitchedAxe` (mesh = hache + manchon de poix).
   Sans hache, la poix reste un ingredient (aucun autre effet).
2. **Hache jamais requise pour abattre** : `fell()` inchange ; bonus 0 par
   defaut ; les buches bonus sont tirees APRES les buches de base et avant les
   branches, avec des tirages RNG seulement si bonus > 0 -> sans hache, suite
   aleatoire et journal ("Debitage en cours : N buches") identiques.
3. **Bonus lu au moment du debitage** (fin de l'amortissement), pas au coup
   d'abattage : c'est l'outil porte a ce moment qui compte.
4. Buches bonus rangees a 1 m sur le cote du tronc (alternance gauche/droite),
   jamais au-dela de la hauteur de l'arbre.
5. **Ordre de la table** (10 recettes) : `shelter, hearth, hearth_relight,
   pitched_axe, axe, pitch, cord, hearth_feed_log, hearth_feed_branch, knap`.
6. **Abri NON solide** : pas de collision ajoutee (la conception la jugeait
   necessaire ; l'ajouter touchait le CollisionSolver compose -> risque de
   regression refuse en session sans supervision). Le joueur traverse l'abri.
7. Abri FIXE (`groundItemIsPortable` faux), mesh appentis 2,1 x 1,8 x 1,2 m
   (`kShelterRadius = 0,6`), pose au barycentre, yaw = regard du joueur.
8. Deux assertions anterieures mises a jour (volontairement) : 8 -> 10 recettes,
   13 -> 15 natures ; table de rendu du test (`testDrawListConstruction`) completee.
9. Aucun fichier de `data/` touche.

## Tests ajoutes

`testHearthWarmthEffect` (0/1,5/3/6/20 m, Needs recoit 2+6 C, jauge plus
haute pres du feu, eteint = identique au froid, rallume, deux feux additionnes),
`testAxeYieldEffect` (bonus 0/1/2, `buckingLogCount`, abattage reel 12 m :
4/5/6 buches et toujours 4 branches, abattage sans hache OK), `testShelterEffect`
(refus atomique ligature manquante, construction, barycentre, non ramassable,
portee 2,4/2,6 m, survol, ressenti > +5 C, mesh), hache renforcee.

## Pas fini / pour l'Agent 4

- Scenario Vulkan K (feu / abri au clavier) non ecrit ; A..H non relances ici.
- Collision de l'abri ; lumiere du foyer (renderer sans lumiere ponctuelle).
- Pot d'argile (P7) non fait.
- Fichiers touches : `GroundItem.hpp`, `Ground.cpp`, `Crafting.hpp/.cpp`,
  `CraftMesh.hpp/.cpp`, `Forest.hpp/.cpp`, `main.cpp`, `tests/logic_tests.cpp`.

---

# Verification (Agent 4/4) - 2026-09-27

Protocole : avant chaque etape lourde, `Get-Process` sur vercora, cmake,
ninja, cl, msbuild, cargo et rustc (aucun trouve a chaque fois). Une seule
execution a la fois (lecon de la Phase 26). Aucun processus tue. `data\`
n'a pas ete touche. Aucune modification de code (aucun bug de la Phase 28 trouve).

## Resultats MESURES

| Point | Resultat |
|---|---|
| Tests headless (`tools\run_logic_tests.bat`) | **3877 / 3877**, code de retour 0 |
| Avertissements `/W4` des tests | 1 seul : `main.cpp(1280) C4244` (double -> float, `ortho.metresPerTexel()`), **anterieur** : meme ligne en 1268 dans la sauvegarde (Phase 27). Zero dans les fichiers de la Phase 28 |
| Build jeu (`clean` puis `tools\build.bat` Debug) | OK, 82/82, **zero** `warning C` |
| Suite Vulkan complete, passage 1 | 0 `[Validation]`, 0 `[ERROR]`, 4 `[WARN]` (les memes qu'avant), fermeture propre partout ; A B D E F G1-3 H I J J3 OK ; **C : arbre MANQUE** -> `[ECHEC]` |
| Suite Vulkan complete, passage 2 | identique : 0 `[Validation]`, seul C echoue, meme cause |
| `run_vulkan_check.ps1 -Only C` | **OK** : arbre abattu, 6 objets ramasses, 0 `[Validation]` |

**Diagnostic de C (harnais, pas Phase 28)** : le jeu repond "aucun arbre dans la
ligne de visee". Arrivee de la navigation a 0,82 m puis 0,84 m de la cible
(echecs) contre 0,75 m (succes). Dans le journal brut, la camera diverge
DES LE PREMIER PAS entre la Phase 27 et maintenant (x = -5,05 contre -4,97),
avant toute logique neuve ; `pickTree`, `fell`, `Camera`, collision : zero
ligne modifiee (diff). C'est la limite du canal d'entree deja mesuree
(MEMORY, Phase 26 : ~0,8 m, 0,78 abattu / 0,80 manque). **Aucune execution
complete n'a donc ete verte de bout en bout** ; chaque scenario a ete vert
au moins une fois sur le binaire final.

## Performance (mesuree, Debug + validation, meme harnais)

Comparaison des 14 lignes `Cadence de boucle (PHASE 27)` du journal brut de
la suite de la Phase 27 (`%TEMP%\vercora_run_20260924-175538.log`) et du
passage 1 (`vercora_run_20260927-043937.log`), processus par processus (us/image) :
5760->5343, 5623->4759, 6395->5532, 6685->5992, 5848->5646, 4147->3976,
1852->1804, 1689->1632, 2113->2135 (+1 %), 2096->2092, 7127->6347,
7024->6101, 4788->4030, 7030->6913. **Aucune regression** : 13 sur 14
egaux ou plus rapides, un seul a +1 % (bruit). NON mesure : Release, A/B
entrelace (`profile_phase27.ps1`) - sans foyer ni abri le cout ajoute par
image est une boucle `shelterNear` sur les objets du sol et un
`warmthAt` sur une liste vide.

## Non-regression par relecture (diff sauvegarde -> actuel)

Fichiers modifies : `main.cpp`, `Forest.cpp/.hpp`, `Ground.cpp/.hpp`,
`GroundItem.hpp`, `Inventory.hpp`, `CMakeLists.txt`, `run_logic_tests.bat`,
`logic_tests.cpp` (+790, aucune ligne retiree) ; nouveaux `Crafting.*`,
`CraftMesh.*`. **Aucune ligne changee** dans Weather, Needs, Wildlife,
Habitat, Paraglider, Bike, Climber, CollisionSolver/Camera, StoneMesh, shaders.
- Taille de pierre : `kStoneRadius`/StoneMesh intouches ; ordre de l'enum 0..7 conserve.
- Abattage sans hache (A) : `fell` intouche ; `buckingLogCount(h,0)` = ancienne
  formule ; boucle bonus sans tirage RNG si bonus 0 ; message "Debitage" identique
  a bonus 0. Scenario A vert dans les deux passages (2 arbres, 8 ramassages, 1 percussion).
- Needs : `+ warmthAt` = +0 sans foyer ; `sheltered` passe de "jamais ecrit"
  (defaut false) a `shelterNear` = false sans abri. Scenario H vert.
- Percussion : message de succes identique mot pour mot (harnais : "eclat
  tranchant se detache" trouve). Seule difference de journal : la ligne
  "Inventaire : ..." dit "un eclat est au sol" au lieu de "l'eclat est au sol".
- `findNearest` ignore les objets fixes (Hearth, Shelter seulement).
- Remarque cosmetique : le BOM UTF-8 de `main.cpp` a disparu ; ses seuls octets
  non ASCII sont dans des commentaires (l. 899-900), sans effet sur le binaire.
- Comportement neuf a connaitre (voulu, non bug) : C avec 3 fibres en poche fait
  une ligature AVANT la percussion ; une branche posee pres d'un feu allume est
  brulee au prochain C.

## Pas fini (inchange)

Collision de l'abri (le joueur le traverse) ; pot d'argile (P7) ; lumiere du
foyer (pas de lumiere ponctuelle dans le renderer) ; scenario Vulkan K (feu/abri
au clavier) non ecrit ; flakiness de navigation du scenario C (harnais).
