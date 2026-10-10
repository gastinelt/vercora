# Vercora — Phase 6

Session du 2026-08-14. Comble les trous laissés par la Phase 5 (collision,
ramassage, transition de chute) et pose le premier maillon d'artisanat
(taillage de pierre par percussion).

> **Note de la Phase 7 (2026-08-16).** Ce document reste le compte rendu de la
> Phase 6 et n'a pas été récrit, mais deux choses y ont bougé depuis :
> `tools\dump_world.cpp` a été supprimé — c'est maintenant `vercora.exe
> --dump-world` — et `run_vulkan_check.ps1` compte trois scénarios, pas deux.
> Voir **NOTES-PHASE7.md**.

---

## État : compilé, exécuté, vérifié sous les couches de validation Vulkan

Mise à jour du **2026-08-15**, sur une nouvelle machine (64 Go de RAM). La dette
de la session précédente — « les couches de validation n'ont pas pu être
vérifiées, faute de SDK » — est **soldée**.

### Build

```bash
tools\build.bat
```

Configure et compile tout (47 cibles), sans rien télécharger : GLFW, GLM et
tinyobjloader sont réutilisés depuis `external/` via
`-DFETCHCONTENT_FULLY_DISCONNECTED=ON`.

### Vérification

```bash
tools\run_logic_tests.bat      # 121 assertions de comportement, sans GPU
tools\run_vulkan_check.ps1     # le VRAI exécutable, sous validation layers
```

**Les deux passent.** `run_vulkan_check.ps1` lance le vrai `vercora.exe` en
Debug (donc `VERCORA_DEBUG`, donc `VK_LAYER_KHRONOS_validation` — le jeu logge
« Validation layers activees ») et pilote sa fenêtre par messages Win32.

Résultat : **0 message `[Validation]`, 0 `[ERROR]`, 0 `[WARN]`, codes de sortie
0/0, fermeture propre.** Couvert : init, rendu, abattage complet (chute →
amortissement → débitage progressif), ramassage des 4 natures d'objet,
percussion, collision contre tronc debout puis souche, 6 redimensionnements,
minimisation/restauration, et un vrai `WM_CLOSE` (pas un kill).

Le shim Vulkan maison (`tests/shim/`) a été supprimé : le harnais headless
compile désormais contre les vrais en-têtes du SDK, ce qui est plus strict.
`Pipeline.cpp`, seul fichier jamais vérifié la session précédente, compile.

### Particularités de cette machine

L'installation Visual Studio 18 est **partiellement corrompue** (1695 fichiers
sur 9692 examinés remplis de zéros binaires) : son CMake et son Ninja sont
inutilisables, et MSBuild fait échouer le générateur « Visual Studio ». Le
compilateur, lui, est intact. `tools\build.bat` utilise donc CMake et Ninja
**autonomes** (winget) avec le générateur Ninja, et vcvars64 de VS uniquement
pour l'environnement MSVC. Il préchauffe aussi `rc.exe`, dont le premier appel
échoue systématiquement (antivirus temps réel). Voir l'en-tête du script.

*Correctifs machine recommandés, à faire par l'utilisateur :* « Réparer » depuis
Visual Studio Installer, vérifier la santé du disque, et ajouter une exclusion
Defender sur le dossier du Windows Kit.

### Ce qui reste non vérifié

- **Le comportement de la collision sur le jeu réel.** Il est prouvé par les 121
  assertions headless (marche simulée pas à pas, lecture directe des positions),
  mais aucun observable externe ne permet de le confirmer sur l'exécutable : un
  collider cylindrique *repousse* au lieu de bloquer, donc le joueur contourne le
  tronc et se retrouve derrière lui que la collision marche ou non ; et la
  déflexion latérale (0,66 u) est plus petite que la tolérance de visée (~1,4 u).
- **La jouabilité.** Rayon du joueur, portée de ramassage, durées de chute et de
  débitage sont des valeurs raisonnées, validées comme *fonctionnelles* mais
  jamais éprouvées manette en main.

---

## Ce qui a été fait

### 0. Préalable — `Vertex.hpp` ne dépend plus de Vulkan

`renderer/Vertex.hpp` incluait `<vulkan/vulkan.h>` pour deux fonctions statiques
de description du format de sommet. `MeshData` s'annonçait « indépendant de
Vulkan » tout en le tirant transitivement : **toute** la logique de jeu devenait
incompilable sans SDK.

Ces deux fonctions vivent désormais dans `renderer/VertexFormat.hpp`, inclus par
le seul `Pipeline.cpp` (`Vertex::getBindingDescription()` →
`vertexBindingDescription()`). C'est ce qui rend le harnais de tests possible —
et accessoirement ce que l'en-tête prétendait déjà faire.

### 1. Collision troncs et souches

- `core/CollisionSolver.hpp` : interface + `BodyCylinder`, dans le même esprit
  que `HeightSampler` — `Camera` ne dépend pas de `world/`.
- `world/Forest` l'implémente : c'est elle qui sait où sont ses troncs.
- Joueur = cylindre vertical (r = 0,35 m). Obstacle = tronc sur toute sa hauteur
  s'il est debout, souche seule sinon. **Le houppier ne bloque pas** : sa base
  est à 35 % de la hauteur, on doit pouvoir marcher dessous.
- Résolution par **repoussement**, ce qui donne gratuitement le glissement le
  long d'un tronc.
- ⚠ Piège corrigé en cours de route : le repoussement seul **fait passer** le
  joueur à travers un passage plus étroit que lui (chaque obstacle le pousse à
  son tour, la somme l'extrude de l'autre côté). D'où `moveBody()`, qui vérifie
  le résultat avec `overlaps()` et refuse le déplacement s'il reste bloqué.
  **`moveBody()` est le point d'entrée obligatoire ; `resolve()` seul ne suffit
  pas.**
- La collision n'agit qu'en **mode marche**. Le vol libre reste un mode
  d'inspection et traverse tout.

**Décisions prises seul :** la souche bloque (c'était demandé explicitement,
mais une souche de 0,3 m qui arrête net est discutable — un franchissement
automatique serait le prochain raffinement) ; un arbre *en cours de chute*
n'expose que sa souche, donc on peut traverser un tronc en vol pendant ~1,5 s.

### 2. Ramassage réel — touche `R`

- `world/Ground` : nouvelle classe propriétaire de tout ce qui traîne au sol.
  `Forest` garde les arbres debout et la chute, et lui dépose son bois.
- `world/GroundItem.hpp` : `GroundItem` sort de `Tree.hpp`, et un enum
  `GroundItemKind {Log, Branch, Stone, Flake}` remplace le booléen `isBranch`,
  devenu intenable dès l'arrivée de la pierre.
- `world/Inventory.hpp` : compteurs simples, affichés **dans le titre de la
  fenêtre** et les logs. Aucun menu.
- Portée 2,5 m, mesurée à l'horizontale.

### 3. Transition de chute améliorée

Nouvel état `TreeState::Settling` entre `Falling` et `Fallen` :

1. **Chute** — pendule inverse (inchangé).
2. **Amortissement (0,65 s)** — l'arbre **reste dessiné**, couché au sol, avec un
   rebond amorti maintenu sous l'horizontale. C'est le cœur du correctif : en
   Phase 5 l'arbre disparaissait à l'instant précis où il atteignait 90°.
3. **Débitage** — les bûches apparaissent **une par une, du pied vers la cime**
   (0,16 s d'intervalle), puis les branches.

**Limite résiduelle assumée :** le tronc disparaît toujours d'un coup au début du
débitage. Le faire rétrécir progressivement demanderait une échelle **non
uniforme**, interdite par le contrat de `DrawItem` (`mat3(model)` sert de normal
matrix) — il faudrait un mesh par arbre. Piste pour plus tard : remplacer le
tronc par une file de bûches posées bout à bout sur son axe, puis les écarter une
à une.

### 4. Taille de pierre par percussion — touche `C`

- `world/StoneMesh` : galet (octaèdre subdivisé, bruité, facetté, aplati) et
  éclat (face ventrale plate, face dorsale en arête). Générés **en code**.
- `world/Knapping` : `knapStones()`.

**Décisions prises seul :**

- **Deux pierres requises** — un percuteur et un nucleus. C'est la seule
  « recette » du système, et il n'y a volontairement aucune table.
- **Une seule est consommée** (le nucleus). Le percuteur est l'outil, pas la
  matière.
- **L'éclat tombe dans le monde**, devant le joueur, et doit être ramassé avec
  `R`. Il ne va pas directement en inventaire : « zéro menu virtuel » veut que ce
  qui est produit existe d'abord comme une chose posée quelque part.
- **6 pierres semées** : deux suffiraient au sens strict, mais obligeraient à
  quadriller la carte pour trouver la seconde.

L'éclat ne sert encore à rien — c'était le périmètre demandé.

---

## Touches

| Touche | Action |
|---|---|
| clic droit + ZQSD | regarder / se déplacer |
| `F` | bascule marche / vol libre |
| `E` | abattre l'arbre visé |
| `R` | ramasser l'objet au sol le plus proche |
| `C` | percuter deux pierres |

(GLFW raisonne en positions physiques QWERTY : `E`, `R` et `C` tombent bien sur
les touches gravées d'un clavier AZERTY, contrairement à `W`/`A` → `Z`/`Q`.)

---

## Outils

| Script | Rôle |
|---|---|
| `tools\build.bat` | configure + compile (Debug par défaut, donc validation layers) |
| `tools\run_logic_tests.bat` | 121 assertions de comportement, sans GPU |
| `tools\run_vulkan_check.ps1` | lance le vrai jeu et traque les erreurs de validation |
| `tools\dump_world.cpp` | imprime le monde semé (positions + indices), sert au pilotage — *supprimé en Phase 7, remplacé par `vercora.exe --dump-world`* |

## Prochaines étapes suggérées

1. Essayer manette en main et régler : rayon du joueur, portée de ramassage,
   durées d'amortissement et de débitage.
2. Franchissement automatique des souches plutôt que blocage net.
3. Rendre l'éclat utile (entaille d'abattage, gain de vitesse sur `E`).
4. Réparer l'installation Visual Studio, ce qui permettrait de revenir au
   générateur Visual Studio si on y tient (rien ne l'impose : Ninja marche).
