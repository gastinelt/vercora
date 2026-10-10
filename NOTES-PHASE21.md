# PHASE 21 — Build demo légère + packaging distribuable

## Objectif

Produire un `.exe` Vercora envoyable à quelqu'un qui n'a ni le dépôt ni
l'environnement de développement (pas de Vulkan SDK, pas de VC++
Redistributable installé, pas CMake), qui montre le terrain LiDAR réel
25 km² avec LOD/streaming, l'orthophoto, les arbres/herbe procéduraux, la
marche et le vol libre — et rien d'autre.

## 1. Configuration "démo" : un flag runtime, pas une macro de compilation

Choix : **`--demo`**, un argument de ligne de commande lu dans `main()`
(comme `--no-wildlife`, `--no-scene`, etc.), plutôt qu'une macro
`VERCORA_DEMO` évaluée à la compilation.

Raison : la démo ne retire aucun code, seulement de l'**initialisation** —
exactement le rôle que jouent déjà `--no-wildlife` et `--no-scene` depuis les
Phases 17 et 15. Une deuxième configuration de build à maintenir en parallèle
(macro + `#ifdef` disséminés) aurait un coût permanent pour un besoin
ponctuel, et le harnais de validation Vulkan (`tools/run_vulkan_check.ps1`)
continue de piloter le binaire normal sans rien changer.

`--demo` fait exactement ceci, au démarrage (voir `src/main.cpp`, recherche
`demoMode`) :

- force `useWildlife = false` → faune et mémoire du milieu (`Habitat`)
  coupées, comme `--no-wildlife` ;
- force `useScene = false` et `startInEditor = false` → aucun objet posé
  chargé/sauvé, touche F2 sans effet (le bloc qui bascule le mode éditeur est
  déjà gardé par `terrainScene && useScene`) ;
- désactive `Needs` en entier (jauges, effet sur la vitesse de marche, boire)
  — la vitesse redevient la constante `kWalkSpeed`, non modulée ;
- coupe les touches parapente (V), vélo (B) et escalade (X) : les blocs
  d'entrée dans ces activités sont gardés par `!demoMode` ;
- coupe abattage (E), ramassage (R) et percussion (C) : même garde. La forêt
  et le sol restent construits, visibles et **solides** (collision inchangée)
  — seule l'interaction est coupée, pas le rendu ni la géométrie.
- charge par défaut la dalle de la **zone entière** (`vercora_zone_1m.vchm`,
  25 km²) et non plus la dalle unique de développement
  (`LHD_0907_6440_1m.vchm`), avec repli en cascade sur la version 2 m si le
  package ne contient que celle-ci (voir §3).

Ce que `--demo` NE touche PAS : LOD/streaming du terrain, orthophoto,
végétation procédurale (arbres et herbe), marche, vol libre (F), compteur
FPS. C'est exactement le périmètre demandé.

Usage :
```
vercora.exe --demo
```
Se combine avec les autres options existantes si besoin (`--start=E,N`,
`--no-grass`, etc.) — `--demo` ne fait que poser des défauts, il ne bloque
rien.

## 2. Chemins : la fin des chemins absolus codés en dur

Avant cette phase, `ASSETS_DIR` et `SHADERS_OUTPUT_DIR` étaient deux macros
CMake portant le chemin absolu du dépôt de développement
(`C:/Users/theog/Documents/_CODE/_Vercora/...`). Correct pour développer
(l'exécutable retrouve ses données depuis n'importe quel répertoire courant),
mais rend le binaire indistribuable : sur une autre machine, ces chemins ne
désignent rien.

Nouveau fichier `src/core/Paths.hpp` / `.cpp` : résout `assetsDir()` et
`shadersDir()` avec la règle suivante, dans cet ordre —

1. le sous-dossier `assets\` (ou `shaders\`) **à côté de l'exécutable**, s'il
   existe → cas du package distribué ;
2. sinon la macro CMake (`ASSETS_DIR` / `SHADERS_OUTPUT_DIR`) → cas du
   développement, comportement inchangé.

L'exécutable de développement (compilé dans `build\`, sans dossier `assets\`
ni `shaders\` juste à côté) retombe donc automatiquement sur l'ancien
comportement — **rien ne change pour le développement courant**, y compris
le harnais de validation Vulkan qui pointe toujours vers `build\vercora.exe`.
Le même binaire copié dans un dossier qui contient `assets\` et `shaders\`
lit ceux-là.

`GetModuleFileNameW` (Windows) donne le chemin de l'exécutable ; le résultat
est mis en cache (une résolution touche le disque, et `assetsDir()` est
interrogée à chaque chargement de dalle).

Fichiers modifiés pour utiliser ces fonctions plutôt que les macros
directement : `src/renderer/Pipeline.cpp` (chemin des .spv),
`src/main.cpp` (`kDefaultTerrainFile`, `kDemoTerrainFile`,
`kDemoTerrainFallback`, `kEditorAssetsDir`, `kScenesDir`).

**Ce qui reste dans le binaire** : les macros `ASSETS_DIR` et
`SHADERS_OUTPUT_DIR` définies par CMake existent toujours (elles servent de
repli, cas 2 ci-dessus), donc leur valeur — le chemin absolu du poste de
développement — reste une chaîne de caractères embarquée dans l'exécutable.
Elle n'est **jamais lue** quand le dossier adjacent existe (le cas du
package), mais elle est visible par un outil comme `strings`. Sans
conséquence fonctionnelle ni fuite de données sensibles (c'est un chemin de
projet local, pas une information personnelle exploitable), mais à savoir
si le binaire est un jour distribué à quelqu'un qui inspecterait ses chaînes.

## 3. Build Release

`tools\build.bat Release` fonctionne sans modification (le script acceptait
déjà `Debug`/`Release` en paramètre). Confirmé :

- `CMAKE_BUILD_TYPE=Release` correctement appliqué (vérifié dans
  `CMakeCache.txt`) ;
- `VERCORA_DEBUG` n'est définie que par
  `$<$<CONFIG:Debug>:VERCORA_DEBUG>` dans `CMakeLists.txt` → absente en
  Release ;
- `src/renderer/VulkanContext.cpp` n'active `VK_LAYER_KHRONOS_validation`
  que sous `#ifdef VERCORA_DEBUG` → **aucune couche de validation demandée
  en Release**, confirmé par lancement réel (voir §5).
- Taille de l'exécutable Release : **844 Ko** (optimisations activées, pas
  de symboles de debug embarqués comme en Debug).

Attention pratique notée pendant cette phase : `tools\build.bat` réutilise
toujours le dossier `build\` quel que soit le paramètre — changer de
configuration dedans exige que `vercora.exe` ne soit pas verrouillé par un
processus en cours (sinon `LNK1168`/`LNK1104` à l'édition de liens). Le
dossier `build\` a été remis en configuration **Debug** à la fin de cette
phase, pour laisser l'environnement de développement inchangé.

## 4. Dépendances runtime identifiées

`dumpbin /dependents` sur l'exécutable Release :

| DLL | Origine | Présente sur une machine tierce ? |
|---|---|---|
| `vulkan-1.dll` | pilote GPU (NVIDIA/AMD/Intel) | Oui sur toute machine avec des pilotes à jour depuis ~2016. **Non fournie dans le package** — installée par les pilotes, pas par le SDK Vulkan (le SDK ne redistribue pas ce fichier ; vérifié : absent de l'arborescence `C:\VulkanSDK\...`). |
| `KERNEL32.dll`, `USER32.dll`, `GDI32.dll`, `SHELL32.dll` | Windows | Toujours présentes (composants du système). |
| `MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` | Visual C++ Redistributable 2015-2022 (x64) | Pas garanti sur une machine neuve. **Fournies dans le package**, copiées depuis `VC\Redist\MSVC\...\x64\Microsoft.VC145.CRT\` de l'installation Visual Studio (fichiers officiellement redistribuables). |
| `api-ms-win-crt-*.dll` (8 fichiers) | Universal CRT | Composant du système depuis Windows 10 1607+ / présent nativement sur Windows 10/11. Non fournies, non nécessaires. |

Conséquence : **l'ami n'a rien à installer** si sa machine est un Windows
10/11 avec des pilotes GPU à jour (le cas normal de tout PC utilisé pour
jouer ou avec une mise à jour Windows Update récente). La seule vraie
inconnue est l'état de ses pilotes GPU — voir §6.

`vulkan-1.dll` n'a PAS été ajoutée au package : c'est un composant lié au
pilote installé (son fichier ICD JSON dans le registre pointe vers un pilote
précis), embarquer une copie risquerait un dépareillage silencieux plutôt que
d'aider. Mieux vaut que l'ami ait la version que son propre pilote a posée.

## 5. Contenu exact du package (`dist/VercoraDemo/`)

```
vercora.exe                          844 Ko   (Release, sans validation layers)
msvcp140.dll                         629 Ko   (VC++ Redistributable)
vcruntime140.dll                     174 Ko   (VC++ Redistributable)
vcruntime140_1.dll                    49 Ko   (VC++ Redistributable)
LisezMoi.txt                                   (mode d'emploi pour l'ami)
shaders/
  cube.vert.spv, cube.frag.spv
  terrain.vert.spv, terrain.frag.spv
  foliage.vert.spv, foliage.frag.spv           40 Ko au total
assets/terrain/
  vercora_zone_1m.vchm                95,4 Mo  (terrain 25 km², pas 1 m)
  vercora_zone_ortho.vcto             42,7 Mo  (orthophoto drapee)
```

**Total : 140 Mo.**

Alternative plus légère non retenue par défaut mais disponible si besoin :
remplacer `vercora_zone_1m.vchm` par `vercora_zone_2m.vchm` (23,9 Mo au lieu
de 95,4 Mo) → package total ≈ 68 Mo, même emprise de 25 km², relief deux fois
moins fin. Le code gère déjà ce repli tout seul si seule la dalle 2 m est
présente (voir `kDemoTerrainFallback` dans `src/main.cpp`).

Aucun fichier `_canopy` : absent du package, sans conséquence (le jeu retombe
sur un semis de végétation par pente/altitude, avec un message de log qui le
dit explicitement — comportement déjà existant, pas ajouté pour la démo).

## 6. Tests effectués

- **Build Debug normal (développement)** : compile sans erreur après tous
  les changements de cette phase.
- **Scénario A du harnais de validation Vulkan**
  (`tools\run_vulkan_check.ps1 -Only A`) sur le build Debug : **0 message
  [Validation], 0 [ERROR], fermeture propre**. Gameplay réellement exercé
  (abattage, ramassage, percussion, bascule marche/vol). Confirme que les
  changements de chemins et le flag `--demo` n'ont rien cassé sur le chemin
  de développement normal.
- **`--dump-world --demo`** (sans fenêtre ni Vulkan) : la zone 25 km² se
  charge, la faune/besoins sont bien annoncés comme désactivés dans les
  logs.
- **Build Release** : compile et édite les liens sans erreur ; confirmé
  `CMAKE_BUILD_TYPE=Release` et absence de `VERCORA_DEBUG`.
- **Test d'emplacement tiers** : le dossier `dist/VercoraDemo/` entier copié
  sur le lecteur **E:** (disque différent de celui du dépôt), lancé depuis
  là :
  - `vercora.exe --dump-world --demo` : charge correctement la zone 25 km²
    depuis `E:\VercoraDemoTest\VercoraDemo\assets\...` — preuve que la
    résolution de chemin ne dépend plus de l'emplacement du dépôt ;
  - lancement réel avec fenêtre Vulkan (`--demo --lock-input`, 5 secondes
    puis fermeture forcée pour le test) : instance Vulkan créée, GPU détecté
    (Intel UHD Graphics 770 sur cette machine), les trois pipelines
    (cube/terrain/foliage) chargés depuis `E:\...\shaders\`, orthophoto
    8192×8192 chargée et liée au terrain, quadtree et streaming actifs,
    aucune ligne d'erreur sur stderr. Le processus est resté vivant tout le
    temps du test (pas de crash immédiat).
  - Build Debug restauré dans `build\` après ce test pour ne rien changer à
    l'environnement de développement habituel.

## 7. Ce que je NE peux PAS garantir à 100 % sans machine tierce réelle

Le test ci-dessus est le plus proche possible de "ça marche chez l'ami" sans
y avoir accès, mais il reste sur **la même machine, le même GPU (Intel UHD
Graphics 770), les mêmes pilotes, la même version de Windows**. Restent
inconnus :

- **Le GPU et les pilotes de l'ami.** Vercora demande Vulkan 1.2 ; c'est
  supporté par toute carte NVIDIA/AMD/Intel de moins d'une dizaine d'années
  avec des pilotes à jour, mais je ne peux pas vérifier SA machine.
  → **À demander à l'ami** : que ses pilotes GPU soient à jour (site du
  fabricant, pas seulement Windows Update qui est parfois en retard).
- **Windows 7/8** : non testé, non visé (l'API `GetModuleFileNameW` utilisée
  fonctionne dès Windows XP donc ce n'est pas un obstacle technique connu,
  mais Vulkan et des pilotes récents sont peu probables sur ces systèmes).
  → **À demander à l'ami** : Windows 10 ou 11.
- **Antivirus/SmartScreen** : `vercora.exe` n'est pas signé numériquement.
  Sur CETTE machine, Smart App Control bloque déjà les binaires non signés
  fraîchement compilés (voir le commentaire de `dumpWorld` dans
  `src/main.cpp`) — c'est probable que Windows Defender SmartScreen affiche
  un avertissement chez l'ami aussi au premier lancement. Documenté dans
  `LisezMoi.txt` du package (bouton "Informations complémentaires" puis
  "Exécuter quand même").
- **Taille du transfert (140 Mo)** : trop gros pour la plupart des pièces
  jointes email (limite Gmail : 25 Mo). **Nécessite un service de transfert
  de fichiers** (WeTransfer, Google Drive/OneDrive avec lien de partage,
  etc.) plutôt qu'un email direct.

## Fichiers modifiés/créés dans cette phase

- `src/core/Paths.hpp`, `src/core/Paths.cpp` (nouveaux)
- `CMakeLists.txt` : ajout de `src/core/Paths.cpp` aux sources
- `src/renderer/Pipeline.cpp` : `shadersDir()` au lieu de `SHADERS_OUTPUT_DIR`
  direct
- `src/main.cpp` : flag `--demo`, `kDemoTerrainFile`/`kDemoTerrainFallback`,
  `resolvedTerrainFile`, gardes `!demoMode` sur Needs/vélo/parapente/
  escalade/abattage/ramassage/percussion, chemins via `assetsDir()`
- `NOTES-PHASE21.md` (ce fichier)
- `dist/VercoraDemo/` : package distribuable (non versionné — voir
  `.gitignore` si un dépôt git existe un jour ; ce projet n'est actuellement
  pas un dépôt git)
