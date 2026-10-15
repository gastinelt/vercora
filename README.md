# Vercora

Jeu d'exploration de montagne solo (marche, abattage et taillage, velo,
parapente, escalade, faune, meteo, artisanat) sur une zone LiDAR reelle de
25 km2 du Vercors. Moteur maison C++20 / Vulkan, GLFW, CMake ; Windows et Linux.

Ce fichier est le demarrage rapide. L'etat du projet est dans `MEMORY.md`, le
detail de chaque phase dans `NOTES-PHASE*.md` (la derniere : `NOTES-PHASE32.md`,
optimisation des ressources).

## Construire

Prerequis communs : CMake >= 3.20, Ninja, un compilateur C++20, le SDK Vulkan
(en-tetes, chargeur, `glslc`, couches de validation). GLFW, GLM et
tinyobjloader sont telecharges par CMake dans `external/`.

**Windows** (machine de developpement) :

    tools\build.bat            :: Debug, couches de validation actives
    tools\build.bat Release

**Linux** (Ubuntu 24.04) :

    sudo apt-get install libvulkan-dev glslc vulkan-validationlayers ninja-build \
        libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev
    cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DGLFW_BUILD_WAYLAND=OFF
    ninja -C build-rel

La configuration Debug definit `VERCORA_DEBUG`, seule a activer les couches de
validation Vulkan.

## Donnees

Les dalles LiDAR et les grilles `.vchm` ne sont pas versionnees ; seule
l'orthophoto `assets/terrain/vercora_zone_ortho.vcto` l'est. Les produire :
`tools/fetch_dalles.py`, `tools/lidar_to_heightmap.py`,
`tools/assemble_terrain.py`, `tools/lidar_to_canopy.py` (voir leurs en-tetes).
Sans elles, le jeu retombe sur un terrain synthetique.

Pour MESURER sans les vraies donnees (session cloud, nouveau poste),
`python3 tools/make_bench_zone.py` ecrit une zone proxy a la vraie echelle
(relief synthetique, couvert tire de l'orthophoto). Il refuse d'ecraser des
fichiers existants sans `--force`.

## Lancer

    vercora assets/terrain/vercora_zone_1m.vchm      # zone 25 km2, jeu complet
    vercora --demo                                     # demo : marche et vol libre
    vercora terrain                                    # terrain synthetique

Options utiles (liste complete en tete de `src/main.cpp`) :

| option | effet |
|---|---|
| `--vsync` | presentation calee sur l'ecran : le reglage pour JOUER (consommation) |
| `--start=E,N --look=yaw,pitch --lock-input` | vue reproductible (Lambert 93) |
| `--hour=H --day=D --time-scale=0` | heure et meteo figees |
| `--freeze-anim` | animation des shaders figee (captures comparables au pixel) |
| `--quit-after=S` | fermeture propre apres S s, bilans de fin ecrits |
| `--flythrough=V[,H]` | survol scripte (stress du streaming) |
| `--no-lod`, `--no-vegetation`, `--no-wildlife`, ... | temoins de mesure |
| `--force-staging` | televersement par staging meme en memoire unifiee |

Chaque partie journalise ses couts (`Cout par image`, `Cadence de boucle`,
`Construction de tuiles`) et ses ressources (`Ressources (PHASE 32)` : RAM,
memoire GPU exacte, allocations Vulkan, pool de geometrie).

## Tester

- **Logique, sans GPU** : `tools\run_logic_tests.bat` (Windows). Sous Linux,
  compiler `tests/logic_tests.cpp` avec la liste de sources de ce script.
- **Vulkan reel** : `tools\run_vulkan_check.ps1` (scenarios A a J, `-Only` pour
  un sous-ensemble). Contrat du projet : zero message `[Validation]`.
- **Synchronisation** : Debug avec
  `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`.
- **Sans GPU (Linux)** : lavapipe (`mesa-vulkan-drivers`) sous Xvfb ; le rendu y
  est deterministe. Recette : `NOTES-PHASE32.md`, section 9.

## Arborescence

    src/core      fenetre, camera, collisions, chemins, memoire du processus
    src/assets    heightmap, formats .vchm / .vcto, chargeur .obj
    src/renderer  Vulkan : contexte, pool de geometrie, streaming du terrain, rendu
    src/world     terrain, vegetation, faune, meteo, activites, artisanat
    src/editor    editeur de niveau (F2)
    shaders/      GLSL compiles en SPIR-V par CMake
    tools/        build, tests, harnais Vulkan, preparation des donnees
    tests/        tests headless de la logique de jeu
