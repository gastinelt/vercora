# Phase 28 — Conception de l'artisanat diégétique (document de conception, aucun code)

Rédigé le 2026-09-27, en session sans supervision. Toutes les références
`fichier:ligne` ont été relues dans l'état actuel du dépôt (après
`3faf930`, fin de Phase 27). Aucune ligne de code n'a été modifiée ; aucun
build n'a été lancé.

**Document de cadrage d'origine : introuvable.** Il n'existe aucun fichier de
cadrage dans le dépôt ; il n'est connu que par citations (`MEMORY.md:14`,
`MEMORY.md:1825`, `NOTES-PHASE22.md:106`, « les trois matières du document de
cadrage qui manquaient »). Les exemples « outils primitifs, ligatures en fibres,
poix (résine + cendres), manches en bois dense » viennent donc de la consigne
de la Phase 28, pas d'une relecture du cadrage.

---

## 1. Inventaire vérifié

### 1.1 Le modèle d'objet commun : `GroundItem` + `Ground` + `Inventory`

| Élément | Référence | Ce qu'il fait |
|---|---|---|
| `enum class GroundItemKind : uint8_t` | `src/world/GroundItem.hpp:13-28` | 8 valeurs : `Log, Branch, Stone, Flake, Clay, Sand, Resin, Fiber` |
| `kGroundItemKindCount = 8` | `GroundItem.hpp:30` | **Taille codée en dur** de l'inventaire — à incrémenter avec chaque nouveau type |
| `groundItemRestRadius`, `groundItemLabel*` | `GroundItem.hpp:34-46` | Rayon de repos et libellés FR (sing./indéfini/pluriel), un `switch` par type dans `Ground.cpp` |
| `struct GroundItem {position, yaw, roll, kind}` | `GroundItem.hpp:51-58` | Objet couché, mesh à dimensions absolues, jamais mis à l'échelle |
| `Ground::drop(kind, x, z, yaw, roll)` | `src/world/Ground.hpp:37` | Pose à la surface (le Ground calcule Y) |
| `Ground::scatterStones` | `Ground.hpp:43` | Semis initial des pierres |
| `Ground::findNearest(pos, reach, vTol=3)` | `Ground.hpp:53` | Portée **horizontale**, un seul objet, tous types confondus |
| `Ground::pickUp(index, kindOut)` | `Ground.hpp:58` | Retire l'objet du monde |
| `Ground::items()` | `Ground.hpp:60` | Accès en lecture au vecteur — suffit pour une requête « objets de tel type dans un rayon » |
| `Inventory::add / count / remove / summary` | `src/world/Inventory.hpp:20, 24, 31, 49` | Simples **compteurs** par `GroundItemKind`, `remove` atomique (refuse sans rien toucher) |

Il n'existe **aucun** geste « reposer au sol » depuis l'inventaire (grep de
`inventory.remove` dans `main.cpp` : seul le taillage consomme). Il n'y a **pas
de notion d'objet tenu** distincte de l'inventaire : « porter » = être compté
dans `Inventory`. Pas de masse, pas d'encombrement (`Inventory.hpp:1-17`,
commentaire d'en-tête : « le jour où le portage à dos sera un vrai sujet, c'est
cette classe qui grossira »).

### 1.2 Les matériaux

| Matériau | `GroundItemKind` | Récolte (touche) | Fichiers / fonctions |
|---|---|---|---|
| Bûche | `Log` | Abattage **E** (`kFellKey`, `main.cpp:185`) : `Forest::pickTree` puis `Forest::fell` (`main.cpp:3016-3043`, `Forest.hpp:136`). Débitage différé : `Forest::scheduleBucking` (`Forest.cpp:589`), 2 à 4 bûches selon la hauteur (`Forest.cpp:606`), déposées par `advancePendingDrops` → `ground_.drop` (`Forest.cpp:507`). `Habitat::recordFelling` inscrit la coupe (`main.cpp:3033`) |
| Branche | `Branch` | Même chaîne que la bûche (houppier), `Forest::scheduleBucking` |
| Pierre | `Stone` | Semée par `Ground::scatterStones` ; ramassage **R** (`main.cpp:3049-3062`) |
| Éclat | `Flake` | Taillage **C** (`kKnapKey`, `main.cpp:187`) : `knapStones` (`src/world/Knapping.cpp:32-60`) |
| Argile | `Clay` | Creuser **Y** (`kDigKey`, `main.cpp:260`) près d'un gisement `DepositKind::Clay` : `findMineralDeposits` / `nearestDeposit` (`src/world/Deposits.hpp:165, 180`), portée `kDigReach = 3,5 m` (`Deposits.hpp:175`) ; dépôt au sol `main.cpp:2343-2379` |
| Sable | `Sand` | Idem, gisement `DepositKind::Sand` (loin de l'eau) |
| Résine | `Resin` | Saigner **U** (`kResinKey`, `main.cpp:261`) : `ResinStand::tap(treeIndex, species, now)` (`src/world/Resin.hpp:102`) ; 600 s entre deux saignées, 3 par arbre ; dépôt au pied de l'arbre `main.cpp:2385-2421` |
| Fibres | `Fiber` | Arracher **I** (`kFiberKey`, `main.cpp:262`) : `FiberPlants::pullNearest` (`src/world/FiberPlants.hpp:208`), portée `kPullReach = 2,5 m` (`:163`), repousse 420 s (`:129`) ; `main.cpp:2429-2451` |

Règle commune posée en Phase 6 et répétée en Phase 22 (`Knapping.hpp:36-40`,
`GroundItem.hpp:18-23`, `main.cpp:2337-2342`) : **ce qui est produit tombe
d'abord au sol**, jamais directement dans l'inventaire.

### 1.3 Le pattern Knapping (le modèle à généraliser)

`knapStones(Inventory&, Ground&, playerPos, facing)` (`Knapping.hpp:43`,
`Knapping.cpp:32`) :
1. vérifie les ingrédients dans l'inventaire (`count(Stone) < 2`, `.cpp:34`) ;
2. projette le regard à l'horizontale, **teste la longueur avant de normaliser**
   (`.cpp:41-43`) ;
3. consomme **un seul** ingrédient — le nucleus ; le percuteur est un **outil
   non consommé** (`.cpp:47`, justification `.hpp:29-32`) ;
4. dépose le produit à 1,1 m devant le joueur (`kDropDistance`, `.cpp:14`,
   `ground.drop` `.cpp:57`) ;
5. renvoie un `enum` de résultat + `knapResultMessage` (message FR journalisé).
Aucune table de recettes « volontairement » (`Knapping.hpp:26-27`).

`creuser` (`main.cpp:2343`) duplique à la main la même projection + dépôt à
1,1 m : c'est un **deuxième exemplaire** du même geste, déjà factorisable.

### 1.4 Systèmes qu'un objet fabriqué peut influencer

| Système | Point d'ancrage réel | État |
|---|---|---|
| **Needs — abri** | `NeedsContext::sheltered` (`src/world/Needs.hpp:126`) ; effet `shelterWarmthC = 5 °C` (`Needs.hpp:220`), et annule le refroidissement éolien (`Needs.cpp:184-191`) | **Aucun appelant** (`MEMORY.md:1278`) : `main.cpp:2276-2299` ne le remplit jamais |
| **Needs — température** | `NeedsContext::ambientTemperatureC` rempli depuis `WeatherSample::temperatureC` (`main.cpp:2284`) ; `NeedsParams::comfortTemperatureC`, `exertionWarmthCPerEffort` | Aucune source de chaleur locale n'existe |
| **Needs — soif** | `Needs::drink()` (`Needs.cpp:269`) remet la soif à 1 ; appelé seulement à `kDrinkReach` d'un point d'eau (`main.cpp:2313-2322`) | Pas de contenant : on ne boit qu'au point d'eau |
| **Needs — faim** | `Needs::eat(nutrition)` (`Needs.cpp:277`) | **Aucun appelant**, rien de comestible n'existe (`MEMORY.md:1274`) |
| **Needs — vitesse** | `Needs::movementFactor()` (`Needs.hpp:375`) module la marche | — |
| **Lumière / nuit** | `WeatherParams::nightAmbientFactor = 0,30` (`src/world/Weather.hpp:405`), `sunLightFor` (`Weather.hpp:933`) ; « à revoir avec le feu de camp » (`MEMORY.md:2214`) | **Aucune lumière ponctuelle** dans le renderer (grep `fire/feu/foyer` : zéro) |
| **Vitesse d'abattage** | `Forest::fell` est **instantané** (`main.cpp:3025`) ; seul le débitage a un délai (`PendingDrop::delay`, `Forest.hpp:163`) ; portée `kFellingReach = 40 m` (`main.cpp:170`) | **Il n'y a pas de vitesse d'abattage** ; et abattre ne demande aucun outil |
| **Vitesse de taillage** | `knapStones` est instantané | Pas de durée non plus |
| **Rendement** | `logCount = clamp(h/2,4, 2, 4)` (`Forest.cpp:606`) | Quantité modulable sans nouveau système |
| **Sauvegarde** | Aucune sauvegarde de partie. Seul `F5` écrit la scène de l'éditeur (`main.cpp:320`, `src/editor/SceneFile.cpp`). « Rien n'est sauvegardé » (`MEMORY.md:1957`, `NOTES-PHASE22.md` fin) | Inexistant |
| **Tests headless** | `tests/logic_tests.cpp` : `testPickup` (l.477), `testStoneKnapping` (l.652), `testNeeds` (l.5158), `testMineralDeposits` (l.6788), `testResin` (l.6999), `testFiberPlants` (l.7177) ; lancés par `tools\run_logic_tests.bat` | 3578 assertions (chiffre de la Phase 23) |

**Conséquence de conception majeure** : il n'existe ni feu, ni cendres, ni
vitesse d'abattage/taillage, ni sauvegarde. La « poix (résine + cendres) » du
brief suppose un **foyer** qui n'existe pas : le foyer doit donc être lui-même le
premier objet fabriqué, et c'est lui qui produira les cendres.

---

## 2. Les objets fabricables (8), et la priorité

Convention : **« posé »** = objet `Ground` à portée de l'ouvrage ; **« porté »**
= compté dans `Inventory` ; **« outil »** = requis mais non consommé (règle du
percuteur, `Knapping.hpp:29-32`).

### P1 — Foyer (`GroundItemKind::Hearth`, nouveau) — **prioritaire**
- **Ingrédients** : 3 pierres **posées** (consommées) + 2 branches **posées**
  (consommées). 
- **Interaction** : le joueur pose lui-même pierres et branches au sol, se place
  au-dessus et fait le geste d'assemblage (touche d'artisanat, cf. §3.3). Pas
  d'allumage séparé cette phase : le foyer est allumé à la construction avec
  l'éclat comme outil (percussion pierre/éclat = étincelle, même geste que le
  Knapping, **éclat porté, non consommé**).
- **Effet** : `NeedsContext::ambientTemperatureC += hearthWarmthC` (proposé
  8 °C, décroissant linéairement jusqu'à 6 m) pendant la durée de combustion
  (proposée 1 h de jeu, prolongée d'1 h par branche ou bûche posée dans le
  foyer). Branché dans `main.cpp:2284`, **même calcul de température unique** :
  on corrige l'ambiante locale, on ne crée pas une seconde formule.
  Produit **1 cendre** (`GroundItemKind::Ash`, nouveau) à son extinction, posée
  au centre.
- **Lien matière** : pierres = cercle de foyer, branches = combustible ; les
  bûches (trop grosses pour allumer) servent seulement à l'entretien.
- **Lumière** : **pas cette phase** (le renderer n'a pas de lumière ponctuelle).
  Décision documentée en §4.

### P2 — Ligature de fibres (`Cord`, nouveau) — **prioritaire**
- **Ingrédients** : 3 fibres **portées**, consommées.
- **Interaction** : torsion à la main, geste identique au Knapping (depuis
  l'inventaire, produit déposé à 1,1 m devant). Aucun poste.
- **Effet** : aucun direct — c'est l'intermédiaire obligatoire de P3, P5 et P6.
  Le justifier comme objet et non comme étape cachée est ce qui rend la chaîne
  visible au sol.
- **Lien matière** : ortie et ronce sont des sources de fibres textiles réelles.

### P3 — Hache emmanchée (`Axe`, nouveau) — **prioritaire**
- **Ingrédients** : 1 éclat + 1 branche + 1 ligature, **posés ensemble** (outil
  composite = assemblage au sol, pas en poche). Tous consommés.
- **Interaction** : assemblage au sol à la touche d'artisanat ; même dépôt que
  le Knapping.
- **Effet** : l'abattage n'ayant pas de durée, deux effets réels possibles ;
  retenu : **rendement** — `Forest::scheduleBucking` donne +1 bûche
  (`clamp(…, 3, 5)` au lieu de `2, 4`, `Forest.cpp:606`) si une hache est portée.
  Option non retenue : rendre `fell` impossible sans hache (voir §4, risque de
  régression du scénario Vulkan A).
- **Lien matière** : éclat = tranchant, branche = manche, fibre = ligature.

### P4 — Poix (`Pitch`, nouveau) — **prioritaire (dernier des quatre)**
- **Ingrédients** : 2 résines + 1 cendre, **posées près d'un foyer allumé**.
- **Interaction** : le foyer est le **poste** : on pose résine et cendre à moins
  de 2 m d'un foyer en combustion et on fait le geste d'artisanat. Refus
  journalisé « il faut chauffer la résine » sinon.
- **Effet** : ingrédient de P5 et P6 (colle imperméable) ; remplace la ligature
  seule dans la hache renforcée (voir P5).
- **Lien matière** : la poix de résine + charbon/cendre est la colle
  préhistorique attestée.

### P5 — Hache renforcée (poix) — secondaire
- 1 hache + 1 poix, posées. Effet : +2 bûches au lieu de +1. Pas d'usure
  cette phase, donc l'intérêt est modeste : secondaire.

### P6 — Abri de branches (`Shelter`, nouveau) — secondaire, fort impact
- **Ingrédients** : 4 bûches + 6 branches + 2 ligatures, posés.
- **Interaction** : assemblage au sol ; l'abri devient un objet **non
  ramassable** du Ground (voir §3.4).
- **Effet** : `NeedsContext::sheltered = true` quand le joueur est à moins de
  2,5 m d'un abri — premier appelant de ce champ (`Needs.hpp:126`), +5 °C et
  vent annulé (`Needs.cpp:184-191`). C'est l'usage des bûches annoncé en
  `MEMORY.md:1718`.
- Repoussé car il demande une collision (l'abri doit être solide) et un mesh.

### P7 — Pot d'argile cuite (`ClayPot`, nouveau) — secondaire
- **Ingrédients** : 2 argiles + 1 sable (dégraissant), façonnés au sol, puis
  **cuits** en restant posés 10 min de jeu à côté d'un foyer allumé
  (transformation `RawPot → ClayPot` pilotée par le temps, même horloge
  `elapsedSeconds` que `ResinStand`).
- **Effet** : rempli au point d'eau (touche **T**, `main.cpp:2313`), il permet
  un `Needs::drink()` hors de portée d'eau (une gorgée par remplissage).
- **Lien matière** : le sable est le dégraissant qui empêche l'argile de se
  fendre à la cuisson.

### P8 — Couteau à éclat emmanché — rejeté pour cette phase
Éclat + branche + ligature. Aucun système existant qu'il influencerait (pas de
dépeçage, pas de nourriture) : **écarté** selon la règle « aucune mécanique
déconnectée ». Listé pour mémoire.

**Les 4 prioritaires cette session : Foyer (P1), Ligature (P2), Hache (P3),
Poix (P4).** P1 donne le premier effet réel sur Needs et produit la cendre ; P2
et P3 débloquent la chaîne outil ; P4 exerce le « poste » (foyer) et termine la
chaîne cendre du brief.

---

## 3. Architecture : un craft générique qui étend Knapping, pas un système parallèle

### 3.1 Principe
Un nouveau module `src/world/Crafting.hpp/.cpp`, **sans Vulkan**, calqué sur
`Knapping` : une fonction libre qui prend `Inventory&`, `Ground&`, la position et
le regard, et renvoie un résultat + message. `knapStones` devient la **recette
n°0** exprimée dans la table (le test `testStoneKnapping` doit rester vert sans
modification, garde-fou de non-régression). La touche **C** reste la touche
unique du geste de « façonner » : pas de nouvelle touche, pas de menu.

### 3.2 Structure d'une recette
```
struct Ingredient { GroundItemKind kind; uint8_t count; Source source; bool consumed; };
enum class Source : uint8_t { Carried, Placed, Either };
enum class Station : uint8_t { None, LitHearth };
struct Recipe {
    const char* id;               // "hearth", "cord", ... (stable, sert aux logs/tests)
    std::array<Ingredient, 4> in; uint8_t inCount;
    GroundItemKind out; uint8_t outCount;
    Station station;              // poste requis à portée
    float stationReach;           // 2 m
    const char* successMessage;   // FR, même rôle que knapResultMessage
};
inline constexpr std::array<Recipe, N> kRecipes = {...}; // constexpr, pas de fichier externe
```
Table **constexpr** dans le code : testable, sans parsing, cohérente avec la
philosophie « paramètres regroupés et documentés » (`WeatherParams`, `NeedsParams`).

### 3.3 Détection et déclenchement
`CraftResult craftAt(Inventory&, Ground&, const CraftWorld&, pos, facing)`
où `CraftWorld` expose les postes (foyers allumés = objets `Hearth` du Ground
avec combustion > 0).
1. Collecte des objets **posés** : nouvelle méthode
   `Ground::indicesWithin(pos, reach=2,5 m, vTol)` — même critère horizontal
   que `findNearest` (`Ground.hpp:53`), mais renvoie tous les indices.
2. **Sélection de la recette** : on parcourt la table dans l'ordre de
   **priorité décroissante de complexité** (abri avant hache avant ligature avant
   percussion), et on prend la première satisfaite. Déterministe ; l'ordre est
   la seule règle, documentée. Le joueur choisit donc la recette **en
   choisissant ce qu'il pose** — c'est le geste diégétique.
3. Refus : si aucune recette n'est satisfaite, message sur la recette « la plus
   proche » (celle dont il manque le moins d'ingrédients) : « il manque une
   ligature », sur le modèle du refus avec distance de `creuser`
   (`main.cpp:2349-2353`).

### 3.4 Consommation et création
- Tout est vérifié **avant** tout retrait (atomicité, même discipline que
  `Inventory::remove`, `Inventory.hpp:28-30`). Retraits Ground par indices triés
  **décroissants** (sinon `pickUp` décale les indices).
- Produit : `ground.drop(out, …)` à 1,1 m devant (portée de `kDropDistance`
  réutilisée) — ou, pour un produit d'assemblage au sol, au **barycentre** des
  ingrédients consommés : l'objet naît là où on l'a construit.
- Objets fixes (`Hearth`, `Shelter`) : ajout de `groundItemIsPortable(kind)`
  dans `GroundItem.hpp` ; `findNearest` les ignore pour le ramassage.
- État propre au foyer (combustion restante) : un `HearthSet` séparé (indice
  d'objet → secondes restantes), sur le modèle de `ResinStand` qui garde « la
  mémoire des arbres saignés » hors de la Forest. `GroundItem` reste un POD
  sans état.
- Alimenter un foyer = poser une branche/bûche à moins de 1 m puis **C** : c'est
  une recette `Branch(Placed) + Station LitHearth → rien` avec effet
  `+1 h`. Pas de cas particulier.

### 3.5 Branchement des effets
- Chaleur : dans `main.cpp` à côté de `main.cpp:2284`,
  `ambientTemperatureC += hearthSet.warmthAt(playerPosition)`. Fonction pure,
  testable.
- Hache : `Forest::scheduleBucking` reçoit un `bonusLogs` (0 par défaut) —
  défaut = comportement actuel au bit près.
- Abri (P6, plus tard) : `needsContext.sheltered = shelterNear(...)`.

### 3.6 Sauvegarde
Aucune sauvegarde de partie n'existe ; **la Phase 28 n'en crée pas** (décision
§4). Préparation : `Recipe::id` stables, et `HearthSet` exprimé en temps de
jeu, pour qu'un futur format texte (modèle `SceneFile`/`.vcsc`) puisse écrire
`Ground::items()` + l'état des foyers sans changer de structure.

### 3.7 Tests headless à prévoir (`tests/logic_tests.cpp`)
- `testCraftingRecipeTable` : unicité des `id`, sorties hors ingrédients, ordre
  de priorité respecté.
- `testCraftingKnapUnchanged` : la recette n°0 reproduit exactement
  `testStoneKnapping` (2 pierres, 1 consommée, éclat à 1,1 m, regard vertical
  → repli sans NaN).
- `testCraftingAtomicity` : ingrédient manquant ⇒ Inventory et Ground
  **identiques** octet pour octet.
- `testCraftingPlacedVsCarried` : un ingrédient `Placed` hors portée (2,6 m) ne
  compte pas ; un `Carried` au sol ne compte pas.
- `testCraftingIndexRemoval` : consommation de plusieurs objets Ground non
  contigus sans décalage.
- `testHearth` : chaleur maximale au centre, nulle à 6 m, extinction ⇒ cendre
  déposée, alimentation prolonge ; foyer non ramassable.
- `testPitchNeedsLitHearth` : refus sans foyer, refus foyer éteint, succès.
- `testAxeYield` : `bonusLogs = 0` ⇒ `logCount` identique à avant ;
  `= 1` ⇒ 3..5.
- `testNeeds` inchangé (garde-fou : ambiante non corrigée hors foyer).
- Vulkan : un scénario K optionnel (pierres + branches semées au départ, **C**,
  vérifier le foyer dans le journal) — non bloquant cette phase.

---

## 4. Décisions prises par défaut et risques

| Décision | Justification |
|---|---|
| Foyer créé en P1 alors que le brief cite seulement la poix | La poix exige des cendres ; rien n'en produit. Inventer des cendres sans feu serait une mécanique déconnectée |
| Pas de lumière du foyer | Le renderer n'a aucune lumière ponctuelle ; l'ajouter est un chantier de rendu (UBO, 3 shaders) hors conception d'artisanat. `nightAmbientFactor` reste inchangé |
| Allumage inclus dans l'assemblage (éclat outil) | Évite un second geste et un état « foyer éteint neuf » sans usage |
| Hache = rendement, pas condition d'abattage | L'abattage est instantané et sans outil ; le rendre conditionnel casserait le scénario Vulkan A et le mode démo |
| Même touche **C** pour tout le façonnage | Zéro menu : la recette se choisit par ce qu'on pose, pas par une liste |
| Recettes en table constexpr | Testable, sans fichier de données à charger |
| Pas de sauvegarde | Aucune n'existe dans le jeu ; en créer une est une phase à part entière |
| Pas d'usure des outils | Aucune notion d'usure dans le jeu (`MEMORY.md:774`) |
| Couteau écarté | Aucun système à influencer |

**Risques de régression**
- `kGroundItemKindCount` (`GroundItem.hpp:30`) et tous les `switch` de
  `Ground.cpp` (libellés, rayons) et des meshes (`StoneMesh`, `TreeMesh`,
  `HarvestMesh`) : chaque nouveau type doit y être ajouté, sinon mesh ou libellé
  manquant. Un test doit itérer sur tous les types.
- `Inventory::summary()` s'allonge (titre de fenêtre) : afficher seulement les
  compteurs non nuls.
- Réécrire `knapStones` via la table : risque sur `testStoneKnapping` et les
  scénarios A/B → garder `knapStones` comme façade.
- `findNearest` modifié pour ignorer les objets fixes : risque sur `testPickup`.
- Chaleur du foyer ajoutée à l'ambiante : vérifier que `testNeeds` et le scénario
  H ne voient aucun foyer (valeur par défaut 0).
- Mode démo (`main.cpp:3010-3014`) : le façonnage doit rester coupé comme le
  taillage.
- Portage : objets « posés » requis ⇒ il faut un geste pour **reposer** un
  objet porté (absent aujourd'hui). À ajouter en même temps (proposé : **R**
  maintenu = poser le dernier type ramassé), sinon P1 et P3 sont injouables.
