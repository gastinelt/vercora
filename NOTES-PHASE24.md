# Phase 24 — Cohérence marche / escalade

Session à quatre chantiers, traités dans l'ordre de priorité donné. Ce document
n'est complet que pour les chantiers marqués **FAIT** ; les autres portent
explicitement ce qui a été fait et ce qui ne l'a pas été.

---

## CHANTIER 1 — Cohérence marche / escalade — **FAIT**

### Le défaut, tel qu'il était

`Camera`, en mode `Walk`, n'appliquait **aucune** limite de pente : un versant à
90 %, et même une paroi à 300 %, se montait debout à 14 m/s. Deux conséquences,
dont la seconde est la vraie :

1. c'est irréaliste ;
2. **l'escalade devenait facultative**. Une activité entière (Phase 14, avec sa
   jauge, son humidité, son hystérésis) que rien n'obligeait jamais à employer,
   puisque la marche passait partout.

### Les seuils, et pourquoi ceux-là

Tout est dans `src/core/SlopeLimits.hpp`, en *rise par mètre horizontal*
(1,00 = 45° = 100 %), la même unité que `ClimberParams` et `BikeParams`.

| Constante | Valeur | Ce qu'elle fait |
|---|---|---|
| `kWalkComfortSlope` | **0,60** (31°) | fin de la pleine vitesse |
| `kWalkMaxSlope` | **0,84** (40°) | plus aucune progression debout |

**0,60 n'est pas une valeur neuve** : c'est exactement
`ClimberParams::walkableSlope`, déjà justifiée en Phase 14 comme la pente
au-delà de laquelle un sentier de montagne ne se fait plus debout de façon
soutenue, et déjà employée par `tools/find_climb_walls.py`. Les trois lisent
désormais la même constante au lieu de trois copies du même nombre.

**0,84** a été choisi dans la fourchette imposée (entre 0,60 et le seuil de
paroi) pour trois raisons :

1. c'est la **borne haute de l'angle de repos des matériaux meubles** (35–40°),
   exactement la grandeur physique que la Phase 14 invoquait déjà pour placer
   `wallSlope`. Au-dessus, un versant de ce massif ne retient ni terre ni
   éboulis : il n'y a plus de sol, seulement du rocher. C'est littéralement la
   pente maximale à laquelle il existe encore quelque chose sur quoi se tenir ;
2. **la randonnée raide n'est pas touchée** — contrainte explicite de la
   consigne. Un sentier soutenu vit entre 0,30 et 0,60 ; les raidillons montent
   à 0,70–0,80. Tout cela passe encore, ralenti. Mesuré par les tests :

   | pente | 30 % | 60 % | 72 % | 80 % | 84 % |
   |---|---|---|---|---|---|
   | vitesse accordée | 100 % | 100 % | 50 % | 17 % | 0 % |

3. **ça ne ferme pas le terrain**. La distribution des pentes de la zone de jeu
   (25 M d'échantillons, `find_climb_walls.py`) donne p75 = 65,7 % et
   p90 = 92,4 % : le seuil tombe entre les deux et interdit à la marche de
   l'ordre de 13 % du massif.

### Le comportement au-delà : ni mur, ni blocage sec

Trois règles, dans `resolveWalkStep()` :

1. **Rampe, pas marche d'escalier.** La vitesse décroît linéairement de 0,60 à
   0,84. Le joueur *sent* le terrain se durcir bien avant d'être arrêté — à
   72 % il n'avance déjà plus qu'à moitié — et l'arrêt est la fin d'un mouvement
   continu, pas un événement. C'est ce qui évite le « mur invisible
   incompréhensible ».
2. **Dissymétrie amont / aval.** Seule la composante *vers l'amont* est
   facturée : **redescendre est toujours accordé, à pleine vitesse**. Sans ça,
   un joueur posé sur du trop raide serait figé — précisément le blocage
   frustrant à éviter.
3. **Glissade.** Au-delà de 0,84 sous les pieds, le joueur glisse vers l'aval à
   une vitesse proportionnelle à l'excès de pente (0,48 m/s à 100 % : un
   décrochage lent, pas une éjection), plafonnée à 6 m/s — sans plafond, les
   5018 % de pente mesurés sur cette dalle éjecteraient le joueur à 150 m/s.

La règle 3 est la **garantie de fond** : où qu'on dépose le joueur, il finit sur
du terrain marchable sans rien faire. Vérifié par un test — déposé en pleine
paroi à 160 %, il est ramené sur du 84 % en 21,4 s.

Le déplacement **latéral est facturé lui aussi**, et c'est délibéré : s'il était
gratuit, monter en travers à 45° rendrait 71 % du gain d'altitude à pleine
vitesse et la limite ne servirait à rien. Un contrôle négatif l'assure (« le
zigzag ne fait pas gagner plus de hauteur que monter droit »).

### LE VRAI BUG DE COHÉRENCE : le trou entre les deux activités

C'est le point 3 de la consigne, et c'était bien un défaut réel.

L'escalade ne s'engageait qu'à partir de `wallSlope` = **1,00**. Une limite de
marche à 0,84 aurait donc ouvert une bande **[0,84 ; 1,00[** — environ **5 % du
massif, soit un versant sur vingt** — où le joueur ne pourrait **ni marcher ni
grimper**. Pas un cas théorique.

**La correction n'est pas un réglage, elle est structurelle** :
`ClimberParams::wallSlope` **vaut désormais `kWalkMaxSlope`, la même constante**.
Le seuil d'entrée en escalade *est* la limite de marche. Un trou ne peut plus
être réintroduit par un réglage distrait, parce qu'il n'y a plus deux nombres à
faire diverger. Un ajout de seuil aurait, lui, seulement déplacé le problème.

Ce que ça change pour les trois arguments de la Phase 14 : 40° au lieu de 45°
(toujours au-dessus de l'angle de repos, donc toujours du rocher nu) ; la part
grimpable de la zone passe de 8,4 % à ~13 %, ce qui reste loin des 20,7 % que la
Phase 14 jugeait excessifs.

L'hystérésis de **sortie** reste à 0,60, strictement en dessous : c'est elle qui
empêche le mode escalade de battre à chaque frame sur une paroi réelle.

**Effet de bord traité, pas subi** : `WildlifeParams::maxSlope` passe de 0,90 à
0,80. Cette valeur n'a jamais eu de justification propre ; ce qu'elle porte est
un *rapport*, gardé par une assertion depuis la Phase 20 — aucun animal ne se
tient sur une pente grimpable, sans quoi on croiserait des cervidés en pleine
paroi. Le plafond devait descendre avec le seuil de paroi. L'effet réel sur le
jeu est proche de nul et c'est mesurable plutôt que supposé : les animaux sont
semés sur les zones boisées, or `ForestParams::maxSlope` vaut 0,6 — aucun animal
n'est jamais *placé* au-delà ; le seuil ne borne que l'errance.

L'assertion du test, elle, n'a pas changé d'une ligne : elle portait le rapport
et non les valeurs, donc elle a survécu au réglage. C'était exactement ce qu'on
lui demandait.

### Vérification sur le VRAI relief LiDAR

Pas seulement en synthétique — la leçon de la Phase 13 est explicite là-dessus.
Voie de référence de la Phase 14 : pied à **E 899900 / N 6435550** (x = +400,
z = −50 sur `LHD_0899_6436`), cap plein est, déjà connue comme une vraie
transition marche / paroi.

Profil de pente réellement mesuré sur la dalle (base de sondage 4 m) :

| x | 400 | 401 | 402 | 403 | 407 | 409 | 420 | 424 | 426 |
|---|---|---|---|---|---|---|---|---|---|
| pente | 57 % | 75 % | 89 % | 109 % | 119 % | 129 % | 100 % | 63 % | 42 % |

Résultat en jeu : **la marche progresse de 1,4 m puis s'arrête, sur du 81 %**.
Ce chiffre a été vérifié plutôt que cru — 1,4 m paraissait suspect. Le profil
ci-dessus le confirme : le pied de cette voie passe de 57 % à 89 % **en deux
mètres**. L'arrêt court est un fait du terrain, pas un artefact du test.

Et surtout : **depuis ce point d'arrêt exact, `Climber::tryStart()` accepte**.
La jonction est vérifiée là où elle se joue, sur du relief mesuré.

Bénéfice collatéral, mesuré : l'ascension de la voie de référence gagne
désormais **20,8 m** (contre ~19 m avant), parce qu'on s'engage plus bas — plus
proche des 21,4 m que l'outil de repérage annonce en ligne droite.

### Ce qui a été touché

| Fichier | Nature |
|---|---|
| `src/core/SlopeLimits.hpp` | **neuf** — les deux seuils, le filtre, header-only |
| `src/core/Camera.{hpp,cpp}` | le pas clavier passe par le filtre en mode `Walk` |
| `src/world/Climber.hpp` | `wallSlope` et `walkableSlope` lisent les constantes partagées |
| `src/world/Wildlife.hpp` | `maxSlope` 0,90 → 0,80 (voir ci-dessus) |
| `src/main.cpp` | journal sur **changement d'état** uniquement |
| `tests/logic_tests.cpp` | section `testWalkSlopeLimit()`, **+30 assertions** |

Header-only à dessein : ni `CMakeLists.txt` ni `tools/run_logic_tests.bat`
n'ont eu à changer de liste de sources.

**Le journal est volontairement avare** — une ligne sur *transition* seulement
(marchable ↔ trop raide, glisse ↔ tient debout), jamais par frame. Ce n'est pas
de l'économie de place : le harnais paie un coût par ligne de journal, et une
ligne par frame ferait échouer des scénarios entiers pour un jeu correct. C'est
exactement ce qui s'est produit en Phase 23.

### Tests

`tools\run_logic_tests.bat` : **3608 assertions, toutes passantes** (3578 avant
la phase, +30 neuves). Zéro régression sur les 42 sections existantes.

L'assertion qui compte le plus est celle qui balaie 300 pentes de 0 à 300 % et
vérifie qu'**aucune** n'est à la fois trop raide pour marcher et trop douce pour
grimper. C'est la seule chose ici qu'un réglage distrait pourrait casser sans
rien casser d'autre.

### Limites assumées

- La limite ne s'applique qu'à la **marche**. Le vol libre reste un mode
  d'inspection sans contrainte, comme la collision l'est déjà depuis la Phase 6.
- **Pas d'indice visuel** de la limite : le joueur ralentit et glisse, mais rien
  à l'écran ne dit « trop raide ». Un curseur ou une inclinaison de caméra
  seraient l'étape suivante.
- La glissade **ne coûte rien** : ni énergie, ni dégât, ni perte de matériel.
  C'est un renvoi en arrière, pas une sanction.
- Une **marche trop raide mais courte** (un ressaut de 2 m à 90 %) reste ni
  marchable ni grimpable, parce que `minWallRise` exige 4 m de gain : c'est un
  obstacle qu'on contourne. Assumé — et sans piège, la glissade garantissant
  qu'on n'y reste jamais coincé.

---

## CHANTIER 2 — Flakiness du harnais — **FAIT, avec une correction du diagnostic**

### Le point 1 de la consigne était déjà livré

Le remplacement demandé — délégué .NET compilé au lieu d'un script-block
PowerShell par ligne de journal — **existe depuis la Phase 22**, avec sa mesure
(1833 ms contre 5 ms). Rien à refaire de ce côté.

Et c'est justement ce qui a masqué la suite : le harnais paraissait soldé.

### Ce que la dette restante était RÉELLEMENT

Trois exécutions de la suite ont donné le même échec **au détail près** sur les
scénarios A et C. Ce n'était donc **pas de la flakiness** : c'était un défaut
**déterministe**, classé « aléatoire » depuis des phases — ce qui explique
pourquoi élargir des délais ne l'a jamais corrigé, puisqu'aucun délai n'était
en cause.

Deux contrôles négatifs, dans les deux sens, écartent le chantier 1 :

| contrôle | résultat |
|---|---|
| limite de pente **entièrement neutralisée** (`kWalkMaxSlope` = 1000), rebuild | échec **identique** |
| exécution de référence du scénario A | le filtre de pente **ne s'est déclenché aucune fois**, et l'échec est identique |

Cause réelle, lue dans le journal brut du jeu :

    [INFO]  Ramassage : rien a portee de main.        (x 24)
    [INFO]  Percussion impossible : il faut deux pierres, un percuteur...
    [INFO]  Abattage : aucun arbre dans la ligne de visee.

**Tout le scénario A tombait d'une seule cause : le joueur n'arrivait pas.**
« Percussions 0 » et « cible manquée » en étaient les conséquences, pas des
défauts distincts. `GoTo` naviguait **à l'estime** — tenir une touche pendant
`distance / 14 m/s` — puis *déclarait* le joueur arrivé sans rien vérifier.
L'erreur ne se corrigeait jamais : elle s'**accumulait** d'un `GoTo` au suivant.

### Le correctif, sans toucher au jeu

Contrainte respectée : aucune ligne du jeu modifiée pour ce chantier.

`src/main.cpp` journalise la position de la caméra **une fois par seconde, sans
condition**, depuis la Phase 15 — et le commentaire de cette ligne dit
explicitement que c'est pour que le harnais sache où le joueur a réellement
fini. Le harnais ne s'en servait pas. `GoTo` la lit désormais et corrige
(4 passes, tolérance 0,6 m), avec **repli exact sur l'ancien comportement** si
la ligne n'arrive pas.

Effet mesuré, scénario A :

| | avant | après |
|---|---|---|
| arbre visé | **manqué** | abattu |
| objets ramassés | **1** | 7–8 |
| percussions | **0** | 1 |

### Une limite du canal d'entrée, et ce qu'on en fait

La boucle fermée **plafonne à ~0,8 m** et n'y descend pas : `Hold` refuse tout
pas sous 0,28 m et `Start-Sleep` a une granularité de ~16 ms — une correction
fine est soit ignorée soit dépassée. Ce n'est pas un réglage à ajuster, c'est
une limite du pilotage par `PostMessage`.

Or la marge de visée du scénario C est plus fine que ce plafond, et ça s'est vu :
deux exécutions du **même binaire** ont donné **0,78 m (abattu)** et
**0,80 m (manqué)**. Une réussite qui tient à deux centimètres de résidu n'est
pas une réussite. Remède : `Fell-WithRetry`, un petit balayage latéral alterné —
exactement ce que le harnais fait déjà pour le même problème au ramassage
(`Vacuum`), au lieu d'exiger du canal d'entrée une précision qu'il n'a pas.

### Une régression de MOI, trouvée et corrigée

La première suite complète après le correctif de navigation a fait échouer le
scénario I : population de faune lue à 0, **150 relevés au lieu de 254**.

C'était mon journal du chantier 1. « Journaliser sur changement d'état » semble
déjà économe — un état qui **bat** autour d'un seuil produit pourtant une ligne
par frame. **494 lignes** sur la suite. C'est le défaut de la Phase 23,
reproduit malgré la leçon, parce que la règle paraissait bornée.

Limiteur de cadence ajouté (une seconde entre deux lignes). Résultat : **30
lignes** au lieu de 494, et le scénario I retrouve ses 216 relevés et ses 24
animaux.

### Diagnostics permanents ajoutés au harnais

- **Le journal brut du jeu est écrit sur disque** à chaque exécution
  (`%TEMP%ercora_last_run.log`). C'est le manque qui a coûté cinq phases sur
  la dette du vélo : le harnais imprimait ses *conclusions*, et quand l'une
  d'elles était fausse, rien ne permettait de savoir ce que le jeu avait
  réellement émis.
- **Report des déclenchements du filtre de pente**, pour que « ralenti par le
  relief » et « dérive du pilotage » cessent d'être indiscernables.

### Le résultat sur plusieurs runs : le critère N'EST PAS tenu

Trois suites complètes après tous les correctifs :

| run | verdict | ce qui a lâché |
|---|---|---|
| 4 | **VERT** | — première suite verte de bout en bout du projet |
| 5 | **ÉCHEC** | scénario C : arbre manqué ; scénario I : **48 relevés au lieu de 216** |
| 6 | **VERT** | — |

Donc : **2 verts sur 3**, et le critère demandé — zéro faux-négatif sur
plusieurs runs consécutifs — **n'est pas atteint**. Il faut le dire tel quel
plutôt que de retenir le run 4.

Ce qui a néanmoins changé, et qui est solide :

- le défaut **déterministe** de navigation est corrigé, et il ne se reproduit
  plus (le scénario A est « pilotage précis » dans les runs 4, 5 et 6, avec
  7–8 objets ramassés) ;
- la première suite entièrement verte du projet existe, ce qui n'était jamais
  arrivé (voir `NOTES-PHASE23.md`) ;
- ce qui reste est une **flakiness authentique**, cette fois : le run 5 a perdu
  78 % de la télémétrie du scénario I, avec seulement 6 lignes de filtre de
  pente — mon journal du chantier 1 est donc hors de cause, contrairement à la
  régression trouvée plus haut.

### Un défaut de mon propre diagnostic, corrigé

Je n'ai **pas pu** remonter à la cause de l'échec du run 5 : le dump de journal
brut que j'avais ajouté écrivait toujours `vercora_last_run.log`, et le run 6 a
donc effacé la seule trace de l'échec.

Un journal de diagnostic qui ne survit pas à l'exécution suivante ne sert à
rien — précisément parce que les échecs de ce harnais sont ceux qu'on ne
reproduit pas à volonté. Le dump est désormais **horodaté**
(`vercora_run_AAAAMMJJ-HHMMSS.log`), avec une copie à nom fixe pour la
commodité. La prochaine occurrence sera diagnosticable.

### Ce qui reste ouvert

- **La flakiness résiduelle du scénario I** (télémétrie tronquée) et **du
  scénario C** (visée). Piste à instruire en premier avec le journal horodaté :
  savoir si le jeu a réellement émis moins de lignes, ou si le harnais en a
  perdu — la distinction n'a jamais été tranchée pour ce symptôme.
- Les échecs « pas de sortie 20 s après WM_CLOSE » documentés en Phase 23 ne se
  sont **pas reproduits** ici, mais aucune exécution n'a été conduite pour les
  provoquer.
