# NOTES-BENCH-GTX1660S.md — Banc de mesures GPU NVIDIA GeForce GTX 1660 SUPER (Zone 25 km²)

Date : 2026-10-20  
Machine : Intel Core i5-14600K (20 cœurs logiques), 64 Go RAM, Windows 11 64-bit (build 26300), Vulkan SDK 1.4.357.0.  
**Nouveau GPU testé : NVIDIA GeForce GTX 1660 SUPER (6 Go VRAM GDDR6 dédiée, architecture Turing, pilote 32.0.16.1742).**  
Binaire : `build\vercora.exe` en configuration **Release** (optimisations C++20 `/O2`, couches de validation Vulkan inactives).  
Données : Zone réelle LiDAR 25 km² (`assets/terrain/vercora_zone_1m.vchm`, 5001×5001 m, 100 Mo), orthophoto 8192×8192 BC1 (42 Mo), canopée LiDAR, rochers, pierriers et buissons (Phases 29-31).

---

## 1. Contexte et détection matérielle

La machine disposait précédemment de l'Intel UHD Graphics 770 intégrée comme unique GPU (suite au retrait de l'ancienne GTX 960 en Phase 9).  
Le 2026-10-20, une **NVIDIA GeForce GTX 1660 SUPER (6 Go VRAM)** a été installée.

Au démarrage de Vulkan, la carte est immédiatement et automatiquement sélectionnée comme GPU de rendu principal :
```
[INFO]  GPU selectionne : NVIDIA GeForce GTX 1660 SUPER
[INFO]  Memoire GPU : 3 tas - [0] 5966 Mo device-local [1] 24433 Mo [2] 214 Mo device-local ; memoire UNIFIEE (device-local et host-visible).
[INFO]  Device logique et queues cree(e)s.
[INFO]  Familles de queues : 5 - [0] x16 graphics compute transfer sparse [1] x2 transfer sparse [2] x8 compute transfer sparse
[INFO]  Present mode retenu : IMMEDIATE (V-Sync off)
[INFO]  Geometrie : pool de blocs de 4 Mo, televersement par staging
```

---

## 2. Bancs automatisés standardisés (30 secondes par scénario)

Les trois scénarios de référence ont été exécutés en conditions contrôlées (`--lock-input --hour=13 --time-scale=0 --freeze-anim --quit-after=30`) sur un bureau virtuel dédié (« Bench Vercora ») pour garantir l'absence totale de perturbations extérieures :

| Métrique | Survol express (900 m/s)<br>*(Stress streaming LOD)* | Panorama Sommet<br>*(Vue longue portée)* | Forêt dense au sol<br>*(Végétation, rochers, faune)* |
|---|---:|---:|---:|
| **Cadence moyenne (FPS)** | **2 975.4 FPS** | **1 034.9 FPS** | **1 308.3 FPS** |
| **Temps moyen par image** | **336.1 µs** (0.34 ms) | **966.3 µs** (0.97 ms) | **764.4 µs** (0.76 ms) |
| **Temps GPU pur (`drawFrame`)** | **165.6 µs** | **335.7 µs** | **163.9 µs** |
| **Images rendues en 30 s** | **89 267 frames** | **31 048 frames** | **39 250 frames** |
| **Instances & géométrie à l'écran** | 38 tuiles LOD (311k triangles)<br>3 176 arbres | 66 tuiles LOD (540k triangles)<br>4 144 arbres, 15 522 herbes | 61 tuiles LOD (500k triangles)<br>5 278 arbres, 12 214 herbes, 24 animaux |
| **Tuiles construites (session)** | **3 812 tuiles** | 79 tuiles | 75 tuiles |
| **Trous de streaming (tuiles vides)** | **0 frame (0 %)** | **0 frame (0 %)** | **0 frame (0 %)** |
| **Téléversement GPU par tuile** | **64.8 µs** / tuile | 273.7 µs / tuile | 45.5 µs / tuile |
| **VRAM GPU allouée (sur 6 Go)** | **83.0 Mo** | **56.1 Mo** | **55.7 Mo** |
| **RAM résidente processus** | 210 Mo | 206 Mo | 206 Mo |

---

## 3. Banc et observations de Théo : Test regard vers le ciel (5 448 FPS)

Lors d'un test interactif libre sur la zone réelle de 25 km², Théo a orienté la caméra vers le haut (regard incliné vers le ciel à environ les 3/4 de l'écran) et a relevé une cadence record :

$$\mathbf{5\ 448\ FPS} \quad \left(\approx 183.5\ \mu\text{s par image}\right)$$

### Analyse technique de cette performance :
1. **Efficacité du Frustum Culling :**
   En orientant la caméra vers le ciel, la quasi-totalité des milliers d'arbres instanciés, des touffes d'herbe, des rochers et une grande partie des tuiles de terrain au sol sortent immédiatement du tronc pyramidal de vision. Le quadtree et le culling CPU rejettent ces objets en quelques microsecondes sans émettre de draw call inutile.
2. **Absence de goulet d'étranglement CPU (Overhead CPU quasi nul) :**
   Atteindre 5 448 FPS prouve que la boucle applicative C++20 et la soumission des command buffers Vulkan s'exécutent en moins de 185 µs au total. Aucun ramasse-miettes (GC), aucune allocation dynamique mémoire pendant la frame (`vkAllocateMemory` pré-alloué en blocs de 4 Mo via le `GpuBufferPool` de la Phase 32).
3. **Plafond matériel GPU :**
   Le GPU n'a plus qu'à traiter l'éclairage atmosphérique/ciel et le clear des buffers, atteignant la limite brute de débit de commande de la GTX 1660 SUPER.

---

## 4. Comparatif direct vs Intel UHD Graphics 770

Sur le scénario le plus contraignant pour le moteur (Survol express à 900 m/s sur 25 km²) :
* **Intel UHD Graphics 770 (Release) :** 378.0 FPS (temps de frame : ~2.64 ms, `drawFrame` : ~1.8 ms)
* **NVIDIA GeForce GTX 1660 SUPER (Release) :** **2 975.4 FPS** (temps de frame : **0.33 ms**, `drawFrame` : **0.16 ms**)
* **Facteur de gain :** **×7.87 (+687 % de performance brute)**.

### Points clés validés :
- **Streaming parfait :** Zéro trou de géométrie à Mach 2.6 au ras du sol sur les 89 267 frames.
- **VRAM ultra optimisée :** Moins de 85 Mo de VRAM consommée sur les 6 Go dédiés.
- **Stabilité :** Zéro plantage, allocations mémoire stables, staging buffer efficace.
