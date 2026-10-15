# Phase 31 - Builds separes, densite des rochers, demo candidate (2026-10-12)

Session sans supervision, sous-agents sequentiels. Detail complet :
`build-phase31\agent1\RAPPORT.md`, `build-phase31\agent2\RAPPORT.md`,
`build-phase31\agent3\RAPPORT.md`.

**PERF : tous les chiffres de FPS ou de cout de cette phase viennent de l'Intel UHD 770
en 1280x720 et sont INDICATIFS.** Aucune conclusion de performance n'a ete tiree ; tout
sera remesure sur la GTX 1660 Super en 1440p.

## 1. CMake : Debug et Release ne partagent plus rien

- `FETCHCONTENT_BASE_DIR = ${CMAKE_BINARY_DIR}/_deps`. Les sources restent dans
  `external\*-src` : `FETCHCONTENT_SOURCE_DIR_<NOM>` pointe dessus quand ce cache existe,
  donc il n'y a ni clone ni acces reseau.
- `SOURCE_DIR` a ete ajoute dans chaque `FetchContent_Declare`, pour le cas d'un clone neuf.
  Utilise seul, il aurait ete dangereux : l'etape de clone efface le dossier source.
- `tools\build.bat` et `tools\run_logic_tests.bat` sont inchanges.
- Preuves d'independance :
  - apres un Release complet depuis zero, les 127 fichiers de `build-agent0\` sont
    identiques au bit ;
  - `glfw3.lib` du Debug et du Release sont deux fichiers distincts ;
  - `ninja: no work to do` dans les deux sens ;
  - `external\` est reste identique tout au long de la session.
- Debug (`build-agent0\`) et Release (`build-release-p30\`) reconstruits depuis zero :
  les deux tournent, 0 `[Validation]`.
- A nettoyer par l'utilisateur (non supprime) : `external\*-build` et `external\*-subbuild`
  (~19 Mo), devenus inutiles. GARDER `external\*-src`. `build\` et `build-release\` sont
  toujours perimes.

## 2. Rochers : densite pres du joueur

Fichiers : `src/world/Rocks.{hpp,cpp}`, `tests/logic_tests.cpp`.

- Emplacements de PROXIMITE dans les memes cellules, sous le meme masque (pente
  0,60-0,84, couvert < 0,10, rugosite -> classe, trainees) :
  - rochers moyens : +60 par cellule de 32 m, portee 60 -> 18 m ;
  - pierraille : +24 par cellule de 8 m, portee 26 -> 12 m ;
  - fondu par retrecissement a partir de 60 % de la portee ; au-dela, ni parcourus ni dessines.
- AUCUN gros bloc ajoute ; les rochers moyens restent plafonnes a 1,29, donc jamais solides.
  Collision identique (empreinte sur ~25 000 corps).
- Plafond par bloc : 1 792 (grille des blocs) et 1 024 (pierraille). Seule la proximite est
  eclaircie, par un tirage uniforme independant de l'ordre de semis. Sur toute la zone, le
  plafond eclaircit 7 blocs de pierraille sur 2 275, et aucun aux deux sites.
- Correctif d'un defaut anterieur : pendant le fondu, 1 a 4 cailloux flottaient (jusqu'a 1 cm).
- Instances de la Phase 30 identiques au bit.

Pixels changes contre `--no-rocks`, meme binaire, herbe/faune/buissons coupes des deux
cotes (plancher de bruit : 0 px sur 16 paires) :

| vue | 13 h : P30 -> P31 | 5 h 48, soleil 12,8 deg : P30 -> P31 |
|---|---|---|
| pierrier -120,10 | 1,313 -> **6,891 %** (taches > 100 px : 27 -> 57) | 1,143 -> **6,065 %** |
| pierrier -120,-30 | 0 -> **5,576 %** | 0 -> **5,369 %** |
| pied de barre -175,10 | 0,159 -> 0,160 % | 0,133 -> 0,134 % |
| pied de barre -175,-30 | 0,454 -> 0,472 % | 0,418 -> 0,437 % |

- Pierrier, a moins de 15 m : 32 -> 111 plaques. Au-dela de 50 m (plaques) et de 100 m
  (rochers), les comptes sont inchanges.
- **Pied de barre : aucun gain, par la regle.** A moins de 10 m, le masque ne couvre que
  2,2 % du sol (c'est la paroi), et les rochers ajoutes plus loin sont caches par l'arete.
- A l'oeil (planche `build-phase31\agent2\captures\planche_pierrier_avant_apres.png`) :
  rochers poses, en trainees, sol nu entre elles. Les blocs proches font anguleux (bas-poly)
  en vue plongeante. Sous lumiere rasante, la limite la plus visible est l'absence d'ombre
  portee (le moteur n'en fait pas).
- Pop et scintillement :
  - `gather` rejoue au pas de 1 cm sur 10 m : rien n'apparait ni ne disparait,
    saut maximal 0,034 px ;
  - 80 captures espacees de 0,5 m : aucun pop au loin, pas de moire.
- Trainees conservees (contraste 9,8). Lecon : l'ancien test d'anisotropie passait encore
  avec une densite ajoutee SANS trainees. Deux tests ajoutes (contraste >= 3 ; aucune instance
  la ou la trainee est nulle) ; ils echouent tous deux sur ce temoin negatif.
- Tests : **4029/4029** (+29). Deux tests modifies : la borne de densite par cellule inclut
  la proximite ; l'empreinte de la Phase 30 ignore la proximite (references inchangees).
- Suite Vulkan A-J verte, 0 `[Validation]`, C passe (0,66/0,87 m).
- Release lance au pierrier : ~265 img/s. Semis Debug d'environ 170 a 294 us par image,
  releve seulement (UHD 770, indicatif).

## 3. Paquet de demo candidat (dist\VercoraDemo NON modifie)

- `dist\VercoraDemo-candidate\` (19 fichiers) contient :
  - le Release de `build-release-p30` (a jour) ;
  - les DLL VC++ (verifiees avec dumpbin) ;
  - le terrain 1 m et l'ortho, identiques a l'ancien paquet ;
  - les 4 cartes de couvert ;
  - les shaders `precip.*.spv`, indispensables au pipeline meteo.
- Exclus : `LHD_*` (casserait l'auto-detection de la demo), `test_2x2`, le terrain 2 m
  (simple repli), les scenes et `.obj` de l'editeur.
- Tailles : dossier 155,2 Mio (ancien 142,4) ; zip `dist\VercoraDemo-candidate.zip`
  110,7 Mio (ancien 103,3), sha256 `EF938B5E72AE946801E09DB6794E58179D9B542B7808D31CAB7FEBEA91527650`.
  La racine du zip s'appelle `VercoraDemo/`.
- Test depuis I: (disque SATA distinct du NVMe de C:), zip decompresse, lance sans argument :
  - la demo s'auto-detecte ;
  - le journal annonce rochers (1,82 / 4,66 / 23,2 par ha) et buissons (21,7 / 31,0 par ha) ;
  - les captures sont normales ; ~365 / 268 / 275 img/s (indicatif) ;
  - Debug `--demo` : 0 `[Validation]`.
- Copie de test laissee en place : `I:\VercoraDemo-candidate-test-20261012\` (155,2 Mio).
- Limites et points connus :
  - acces aux shaders et a l'ortho deduits du code, pas observes (pas de Process Monitor) ;
  - console noire au double-clic, deja le cas avant ;
  - chemin `C:/Users/...` de repli inscrit dans l'exe, jamais lu ;
  - la console de la demo liste encore des touches desactivees ;
  - `tools\deploy_beta_to_desktop.ps1` deploie toujours SANS cartes de couvert ni
    `precip.*.spv`, et depuis `build\` (perime) : a revoir si le candidat est promu.

## Non fait / non verifie

- Le cout (sur la nouvelle carte).
- Un pied de barre dont le premier plan serait sous masque.
- La lumiere rasante du soir (le pierrier y est a contre-jour).
- Le scintillement image par image en vrai deplacement.
- La decision de remplacer `dist\VercoraDemo` revient a l'utilisateur.
