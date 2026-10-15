# Phase 30 - Rendre les rochers visibles (2026-10-11 / 12)

Session sans supervision, sous-agents sequentiels. Detail complet :
`build-phase30\agentA\DIAGNOSTIC.md`, `build-phase30\agentB\RAPPORT.md`,
`build-phase30\agentC\VERIFICATION.md`. Agent D (paquet de demo) NON fait.

Point de depart : Phase 29 livree, mais rochers invisibles aux sites de la conception
(pierrier E 903578 / N 6438125 : <= 1,1 % de pixels changes ; pied de barre
E 903602 / N 6438269 : 0-0,4 %). Build de reference : `build-agent0\` (Debug) ;
`build\` et `build-release\` ont un cache CMake perime (ancien chemin `_CODE\_Vercora`),
non touches.

## Agent A - diagnostic (aucun code modifie)

- Sonde compilee sur `Rocks.cpp` + capture ; la sonde retrouve a l'unite les comptes du
  journal du jeu.
- **Plancher de bruit : 0 px** avec `--no-grass --no-wildlife --no-bushes` dans les deux
  conditions. Les 1,1 % du 11/10 etaient l'herbe qui ondule (le vent n'est pas gele par
  `--time-scale=0`).
- Vues de reference (cap vers l'amont tire du gradient DTM, hauteur d'yeux) :
  pierrier `--look=-120,10`, pied de barre `--look=-175,10` (le cap -100 y montre une
  paroi pleine, hors masque : 0 px).
- **Facteur : la TAILLE ANGULAIRE.** Tache mediane 3 px ; pierraille depassant de 2-5 cm,
  seule presente a < 20 m ; gros bloc le plus proche a 33 m. Le relief masque 84 % des blocs
  dans le cadre. Contraste suffisant (luminance 143 contre 224, Weber -0,36) ; portee non
  limitante (rochers vus de 7 a 175 m pour des portees de 70 a 700 m).
- Le site 903578 est sur la frontiere de rugosite (0,104 m au point, 0,074 au centre de sa
  cellule de pierraille).

## Agent B - correction

Fichiers : `src/world/Rocks.{hpp,cpp}`, `src/world/RockMesh.{hpp,cpp}`,
`tests/logic_tests.cpp`. Shaders et `main.cpp` NON modifies. Sources avant/apres :
`build-phase30\agentB\sources_avant\`, `sources_apres\`.

| | Phase 29 | Phase 30 |
|---|---|---|
| rochers moyens (echelle) | 0,35-0,95 | 0,60-1,40 x clamp(rugosite/0,15 ; 0,8 ; 2,0), plafond 1,29 (< seuil de collision 1,3) ; mediane reelle 0,51 -> 0,99 |
| gros blocs | - | taille INCHANGEE (collision identique), teinte x0,85 |
| pierraille | cailloux 5-12 cm, 8 faces, 40 % hors sol | 10-25 cm, 20 faces, ~55 % hors sol, plaques enfoncees sur sol convexe |
| ombre de contact | aucune | x0,55 au pied -> x1 a 0,4 de la hauteur visible (couleur de sommet) |

- Nombre et placement **identiques au bit** (empreinte de hachage dans les tests ; 4 755
  instances reelles comparees ; comptes du journal identiques). Masque inchange. Aucune
  nouvelle collision.
- 726 cailloux flottaient en Phase 29 ; 0 maintenant (sur 449 blocs, 1 103 rochers et
  21 846 cailloux).
- Resultat (meme binaire, rochers contre `--no-rocks`, bruit 0 px sur 8 paires) :

| vue | avant | apres | taches > 100 px |
|---|---|---|---|
| pierrier -120,10 | 0,386 % | **1,313 %** | 4 -> 27 |
| pied de barre -175,10 | 0,144 % | 0,159 % | 1 -> 1 |

- **Critere de l'Agent A : PARTIELLEMENT atteint.** Les taches (>= 10) sont atteintes, pas
  les 2 % : la moitie basse du cadre (0-6,6 m) n'a aucun rocher, donc c'est une affaire de
  densite, hors perimetre. Au pied de barre, les pixels viennent des gros blocs, laisses
  tels quels pour ne pas changer la collision.
- Un caillou de 32 faces donnait 1,498 % mais coutait +0,50 ms : rejete. Un test limite
  le caillou a 20 triangles.
- Tests : **4000/4000** (3968 + 32). Trois tests de la Phase 29 ajustes parce qu'ils
  encodaient les anciennes tailles : bornes de la plaque de pierraille, tolerance "pose au
  sol" (pierraille jusqu'a 0,5 m enfoncee), borne basse des rochers moyens.
- `shotx.ps1` (capture) corrige : une fenetre de taille nulle laissait le jeu orphelin.

## Agent C - verification (aucun code modifie)

| point | verdict |
|---|---|
| build Debug + headless 4000 (x2) | VERT |
| suite Vulkan A-J | VERT : 0 `[Validation]`, sorties 0, fermetures propres ; C passe (0,75/0,89 m, imprecision connue) |
| build Release `build-release-p30\` | VERT, apres restauration de `external\` (voir piege) |
| captures lisiere (0,38 %, bruit 0,16 %) et zone a buissons 902660,6438835 (15 %, bruit 0,10 %) | VERT : rien de flottant |
| scintillement : 14 positions espacees de 0,75 m, avec et sans rochers | VERT dans les limites de la methode : aucun pop ni moire au loin |

Cout Release (A contre `--no-rocks --no-bushes`, entrelace avec A2 temoin) :

| vue | rochers + buissons | dont rochers | img/s |
|---|---|---|---|
| pierrier | +697 us (+23,5 %) | tout | 273 |
| alpage ref. | +144 us (+3 %) | 0 | 201 |
| zone a buissons | +810 us (+33 %) | +109 us | 309 |

Part propre a la Phase 30 : un temoin Release (code actuel + fichiers rochers de la
Phase 29) donne au pierrier 3 636 us (Phase 30), 3 347 us (rochers Phase 29) et
2 891 us (sans rochers), 12 runs valides sur 12. Le cout des rochers passe donc de
0,46 a **0,75 ms** (+0,29 ms), tout sur le GPU (le semis coute 24 us). Le pierrier reste
a 273 img/s.

Conditions : l'utilisateur jouait sur le meme GPU de 21:27 a 23:23 (10 captures sur 12
etaient vides) ; tout a ete refait apres 23:24. `TextInputHost.exe` occupait ~90 % d'un
coeur, d'ou un seuil de rejet releve de 1 a 1,5 coeur ; 16 runs sur 72 restent NON
CONCLUANTS. Debug a l'alpage : +1,5 ms par rapport a la Phase 29 dans TOUTES les
conditions, meme sans rochers ni buissons ; le Release concorde. Non explique, sans lien
avec cette phase.

## Pieges

- `CMakeLists.txt` : `FETCHCONTENT_BASE_DIR = external`, donc les dossiers de build de
  GLFW et GLM sont PARTAGES entre tous les builds. Le build Release a ecrase 28 fichiers,
  dont `glfw3.lib` ; ils ont ete restaures depuis `build-phase30\agentC\outils\external_avant_release\`.
  Correctif recommande, non applique : `${CMAKE_BINARY_DIR}/_deps`.
- Arbre temoin `build-phase30\agentC\temoin_p29\` : sa jonction `assets` vers les vrais
  assets a ete RETIREE (rmdir sans /s) par l'orchestrateur.

## Non fait / non verifie

- Agent D (candidat de demo `dist\VercoraDemo-candidate`) : pas fait. `dist\VercoraDemo`
  date du 27/08, sans carte de couvert : dans la demo, ni rochers ni buissons.
- Scintillement d'une image a l'autre en mouvement continu : un pas de 0,75 m vaut environ
  0,5 s de marche.
- Lumiere rasante : seul 13 h a ete teste.
- Densite des rochers pres du joueur : c'est elle qui manque pour atteindre 2 %, hors
  perimetre (le nombre devait suivre le masque).
