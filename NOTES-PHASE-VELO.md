# Phase 13 (2026-08-22) : physique du velo

Premiere version de la troisieme activite. Meme intention que le parapente de la
Phase 8 : un modele **simplifie mais credible**, chiffre et testable, pas une
simulation de dynamique du vehicule.

Fichiers neufs : `src/world/Bike.{hpp,cpp}`. Touches par `src/main.cpp`,
`src/core/Camera.{hpp,cpp}`, `tests/logic_tests.cpp`,
`tools/run_vulkan_check.ps1`, `CMakeLists.txt`, `tools/run_logic_tests.bat`.

---

## 1. Le modele

`Bike` ne connait ni Vulkan ni la `Camera`. Il consomme les deux interfaces qui
existaient deja - `core/HeightSampler` pour l'altitude et la pente,
`core/CollisionSolver` pour les obstacles - et produit une position, un cap, une
vitesse et deux angles d'assiette. **Exactement la meme separation que
`Paraglider`**, ce qui lui vaut d'etre compile ET execute dans les tests
headless, sans GPU.

### Dynamique longitudinale

Une somme d'accelerations, integree a chaque pas. Aucun terme n'agit
instantanement sur la vitesse : ni le pedalage, ni le frein.

```
a = -g.sin(theta)                            pesanteur le long de la pente
  + pedalAcceleration . (1 - v/pedalSpeedLimit)   pedalage (nul si on freine)
  - brakeDeceleration . brakeInput           frein manuel
  - rollingResistance - dragCoefficient.v^2  resistances
```

`sin(theta)` et non `tan(theta)` : sur une paroi LiDAR a 300 % la difference est
un facteur trois, et `tan` divergerait. La zone de jeu contient reellement de
telles parois (voir `NOTES-PHASE9.md`).

### Les valeurs, et pourquoi

| parametre | valeur | justification |
|---|---:|---|
| `pedalAcceleration` | 2,6 m/s² | ~250 W au demarrage sur ~95 kg (velo + cycliste) : un cycliste ordinaire en danseuse |
| `pedalSpeedLimit` | 6,5 m/s | au-dela la cadence s'emballe, l'effort n'est plus utile |
| `rollingResistance` | 0,35 m/s² | coeff. ~0,036 : pneu large basse pression sur terre |
| `dragCoefficient` | 0,014 /m | **terme groupe** : aero (~0,004) + sol irregulier + suspension + le fait qu'un vetetiste ne laisse pas filer sa machine. **C'est lui qui fixe les vitesses de pointe**, et c'est le parametre le plus arbitraire du modele |
| `brakeDeceleration` | 6,0 m/s² | ~0,61 g, la limite avant passage par-dessus le guidon |
| `maxSpeed` | 13,9 m/s (50,0 km/h) | plafond absolu ; ne mord qu'au-dela de ~32 % de pente |
| `maxLateralAccel` | 4,0 m/s² | ~0,41 g, adherence d'un pneu large sur terre seche |
| `maxTurnRateDegPerSec` | 140 °/s | manoeuvre pieds a terre ; sans lui la formule de braquage diverge a l'arret |
| `bodyRadius` | 0,45 m | contre 0,35 m a pied : un guidon est plus large que des epaules |
| `slopeSampleStep` | 2,0 m | demi-base de mesure de la pente ; **voir la section 3**, c'est le reglage qui a demande une mesure sur relief reel |

### Les vitesses obtenues — LE chiffre qui compte

Mesurees sur plans inclines, avec le code du jeu (sonde hors ligne, puis
re-verifiees par les tests headless) :

| pente | pedalage a fond | roue libre |
|---:|---:|---:|
| plat | **16,9 km/h** | 0 |
| +5 % | 13,5 km/h | 0 |
| +10 % | **10,0 km/h** | 0 |
| +15 % | 6,2 km/h | 0 |
| +20 % | 2,4 km/h | 0 |
| +25 % | **0 (cale)** | 0 |
| -5 % | 20,2 km/h | 8,6 km/h |
| -10 % | 23,3 km/h | 22,9 km/h |
| -20 % | 37,4 km/h | **37,4 km/h** |
| -30 % | 47,2 km/h | **47,2 km/h** |
| -40 % | 50,0 km/h (plafond) | 50,0 km/h |

La consigne demandait 30-50 km/h en descente et beaucoup moins en montee : c'est
obtenu entre -20 % et -40 %, et la montee tombe a un cinquieme du plat des 15 %.

**Pente maximale franchissable : 23,6 %.** C'est ou `g.sin(theta)` egale
`pedalAcceleration - rollingResistance`. Au-dela, le velo cale et **ne recule
pas** (la vitesse est ecretee a zero). Voir la section 5 : c'est la limite la
plus visible du modele sur ce massif.

Le velo ne part pas non plus tout seul sur une pente faible : a -2 % en roue
libre la resistance au roulement l'emporte, il reste immobile.

### Direction

Rayon de braquage **fini et croissant avec la vitesse** :

```
omega = min( maxLateralAccel / v , maxTurnRateDegPerSec )     R = v² / maxLateralAccel
```

- a 10 m/s : R = 25 m, soit 23 °/s ;
- a 5 m/s : R = 6,3 m, soit 46 °/s ;
- a l'arret : 140 °/s, le plafond - on replace la machine, on ne "tourne" pas.

Impossible, donc, de pivoter sur place a pleine vitesse. Verifie par la mesure :
le test confronte le rayon OBSERVE a `v²/maxLateralAccel`.

### Contact avec le sol et assiette

La verticale **n'est jamais une variable d'etat** : `position.y` est relue de
`heightAt()` a chaque pas, y compris des `mount()`. Le velo ne decolle donc
jamais d'une bosse, par construction et non par reglage.

L'assiette est **purement visuelle** (elle n'entre dans aucun calcul) :

- tangage = `atan` de la pente le long du cap ;
- roulis = devers (`-atan` de la pente en travers) **+** inclinaison en virage
  (`atan(a_lat / g)`, l'angle d'equilibre d'un vrai deux-roues).

Les deux sont ecretes a ±45° : une paroi LiDAR verticale coucherait sinon le
marqueur a plat.

### Obstacles : rien n'a ete reecrit

`Bike::update` construit un `BodyCylinder` et appelle **`moveBody()`**, le point
d'entree unique de la collision du projet - le meme que
`Camera::resolveCollisions`, le meme que les tests de la Phase 6. Meme bande
verticale (`feetY` = pieds, `headY` = un peu au-dessus des yeux), donc une souche
basse arrete le velo comme elle arrete les jambes.

Seul ajout : **le choc coute de la vitesse**, proportionnellement a ce que
l'obstacle a mange du pas voulu. Frotter un tronc en le longeant n'en retire
presque rien ; le percuter de face arrete net. C'est la seule consequence d'un
choc dans cette session - ni degats ni chute, explicitement hors scope.

---

## 2. Integration : touche, camera, exclusions

**Touche B** (`kBikeKey` dans `main.cpp`). B occupe la **meme position physique
en AZERTY et en QWERTY** (rangee du bas W-X-C-V-B contre Z-X-C-V-B), donc le
token GLFW tombe sur la touche gravee B des deux cotes - contrairement a W/A qui
donnent Z/Q. Et elle est juste a cote de V, la touche du parapente.

- **B depuis la marche** : en selle, cap = la direction du regard projetee.
- **B a velo** : pied a terre, la camera revient en mode marche sur la position
  du velo.
- Pendant la sortie : **Z** pedale, **S** freine, **Q/D** tournent.
- E/R/C (abattre, ramasser, percuter) sont **desactives** a velo, comme en vol.
- On ne peut pas decoller en parapente a velo, ni enfourcher le velo en vol.

**Camera : nouveau mode `CameraMode::Ride`**, troisieme personne, meme contrat
que `Glide` (la camera est passive, elle recoit sa pose par `setChasePose()`).
Un mode distinct et non un alias de `Glide` : les journaux doivent dire lequel
est actif. Choix de la troisieme personne : la meme raison qu'au parapente, la
lisibilite du mouvement - ici l'assiette du velo sur le relief.

Une contrainte que le parapente n'avait pas : **une garde au sol minimale de
1,2 m pour l'oeil**. En descente la camera recule *dans* la pente qu'on vient de
devaler ; sans ce plancher elle passe sous la surface et on voit l'interieur du
terrain.

Une ligne de telemetrie par seconde tant qu'on roule (vitesse, pente sous les
roues, cap, assiette, altitude). Ce n'est pas du confort : c'est la seule facon
pour le harnais automatise de constater que la physique a travaille sur du vrai
relief - un velo bloque a 0 km/h donnerait autrement un run parfaitement vert.

---

## 3. La mesure qui a change le code : `slopeSampleStep`

Le premier reglage - differences finies a ±1 m, le pas de la heightmap - semblait
evident. **Il ne marche pas**, et c'est le run sur relief reel qui l'a montre :
le velo s'immobilisait definitivement au milieu d'un versant qui descend a 27 %.

Depouillement de ce versant (200 m plein nord depuis E 907172 / N 6439790,
dalle `LHD_0907_6440`, descente moyenne de **26,7 %**) :

| demi-base | ecart-type de la pente | sections en **contre**-pente | pic |
|---:|---:|---:|---:|
| **1 m** | 16,4 % | **8,0 %** | **+22,0 %** |
| 2 m | 15,1 % | 6,1 % | +10,5 % |
| **2 m sur 4 m de base (retenu)** | | **1,6 %** | **+1,1 %** |
| 4 m | 14,0 % | 1,6 % | +1,1 % |

A ±1 m, une descente franche presente **8 % de micro-sections montantes a plus de
20 %** - donc au-dela de la pente maximale franchissable (23,6 %). Le velo calait
en pleine descente.

A ±2 m (base de 4 m), ces sections tombent a 1,6 % avec un pic a +1,1 %, **sans
toucher a la pente moyenne** (-26,9 % contre -26,8 %). Ce n'est pas un artifice :
un DTM LiDAR a 1 m porte du bruit decimetrique, et une machine de 1,1 m
d'empattement lancee a 20 km/h ne "sent" pas un creux d'un metre. Au-dela de 4 m
de base on commencerait a effacer de vraies ruptures de pente.

**Autre defaut trouve par la meme voie** : le seuil d'arret (`kSpeedEpsilon`,
2 cm/s) etait applique inconditionnellement apres chaque integration. Consequence
non voulue : il **empechait tout demarrage** dont l'acceleration etait inferieure
a `kSpeedEpsilon / dt`, soit 1,2 m/s² a 60 images/s. Le velo restait cloue a
0 km/h des 10 % de montee, et ne pouvait meme pas se laisser descendre une pente
de 10 % en roue libre, alors que le bilan des accelerations etait franchement
positif. C'est un seuil d'**arret** : il ne s'applique plus que si le bilan des
accelerations est negatif.

Aucun des deux defauts n'etait visible sur un plan incline synthetique. Les deux
ont ete trouves en roulant sur la dalle LiDAR.

---

## 4. Verification

### Tests headless : 334 assertions (280 avant, **+54**)

`tools\run_logic_tests.bat`, sans GPU ni Vulkan. La section Phase 13 ne
re-derive pas les equations - ca ne prouverait que l'accord du test avec
lui-meme - elle **mesure les vitesses d'equilibre reelles** et les confronte aux
fourchettes annoncees. Couvert :

- contact permanent avec le sol, verifie a chacun des 300 pas d'une descente ;
- lecture de la pente le long du cap (montee, descente, en travers) ;
- inertie : une frame de pedalage ne donne qu'une fraction de m/s ; le frein a
  fond ne met pas a l'arret en une frame ; distance d'arret plausible ;
- vitesses d'equilibre sur plat, +10 %, +25 % (cale, **et ne recule pas**),
  -20 %, -30 %, et **-200 %** (pente extreme : bornee, aucun NaN, aucune
  divergence) ;
- roue libre a -2 % : le velo ne part pas tout seul ;
- rayon de braquage fini, croissant avec la vitesse, egal a `v²/a_lat` mesure ;
  plafond a l'arret ; sens de rotation dans les deux sens ;
- assiette : tangage sur la pente, devers en travers, inclinaison en virage
  (droite et gauche), ecretage sur une paroi ;
- **collision** : le velo n'entre pas dans un tronc, s'arrete au contact, y perd
  sa vitesse - **avec temoin** : sans solveur, le meme trajet traverse. Une
  souche basse l'arrete aussi ;
- `update()` sans `mount()`, apres `dismount()`, a `deltaTime` nul : sans effet ;
- ecretage des entrees : passer 50 se comporte comme passer 1.

Le temoin de collision merite d'etre souligne : sans lui, un test ou le velo
n'avance pas du tout passerait pour un test de collision reussi.

### Vulkan reel : `run_vulkan_check.ps1`, **5 scenarios sur 5**

Nouveau **scenario E** : sortie a velo sur la dalle LiDAR reelle, sous couches de
validation. **Zero `[Validation]`, zero `[ERROR]`, zero `[WARN]`, codes de sortie
0/0/0/0/0, fermeture propre 5 fois sur 5.**

**Quatre versions du scenario ont ete necessaires**, et les trois premieres ont
echoue chacune pour une raison differente. C'est le vrai contenu de cette
section : ce qu'il faut pour qu'un scenario automatise PROUVE quelque chose.

1. **Depart quelconque.** Le harnais ne sait pas tourner la camera (GLFW lit le
   curseur systeme), donc le velo part forcement au cap initial. Lance n'importe
   ou, il roule sur du plat : run vert, rien de montre.
2. **Depart sur un versant a -21 %.** La descente marchait, la remontee non : elle
   depassait la pente franchissable (23,6 %) et le velo s'immobilisait pour de
   bon. C'est ce run qui a mis au jour le reglage de `slopeSampleStep`
   (section 3).
3. **Descendre, puis faire demi-tour et remonter la meme ligne.** Piege double.
   D'une part, plus la descente est longue - et il lui faut de la longueur pour
   que la vitesse se developpe - plus elle depose le velo bas dans le versant, la
   ou la pente depasse le franchissable : 18 s de roue libre donnaient 9,2 m de
   denivele (trop peu), 30 s en donnaient 24,4 mais aboutissaient a une remontee
   a 0,1 km/h. **Incompatibles dans cet ordre.** D'autre part - l'erreur de
   raisonnement de fond - **"faire demi-tour" ne redonne pas la pente inverse** :
   le terrain de l'autre cote du point de depart est un AUTRE terrain. Sur la
   croupe essayee, le nord descendait a -11 %... et le sud descendait aussi, a
   -36 %.

**La version retenue evalue la pente le long de CHAQUE cap** (differences
centrees a ±2 m, exactement comme le fait `Bike`) sur toute la dalle, et retient
un point qui offre les deux a la fois :

> depart `--start=907875,6439450`, altitude 513,2 m
> - **montee cap 220°** : +14,8 % en moyenne sur 90 m, pire section +16,8 %, donc
>   **aucune section infranchissable** - le velo monte sans caler ;
> - **descente cap 40°** : -25,0 % en moyenne sur 170 m, pire section -63 %,
>   franche mais roulable (les parois a -600 % de la dalle ne testeraient que le
>   plafond de vitesse, pas une descente VTT).

La montee se fait en premier : la descente qui suit repasse par le point de
depart et gagne le denivele grimpe en elan supplementaire.

Le **demi-tour est ASSERVI**, pas minute. Un demi-tour "1,4 s a 140 °/s" a ete
essaye et rate : le temps vu par le jeu n'est pas celui du `Start-Sleep`, et sur
ces versants le devers depasse 25 %, donc 18° d'ecart suffisent a sortir de la
fourchette mesuree. Le jeu journalise deja son cap chaque seconde : le script
tourne par courtes rafales, frein tire, en relisant le cap entre chaque, jusqu'a
±8° de la cible.

**Resultats du scenario E**, deux runs consecutifs :

| | run 1 | run 2 |
|---|---:|---:|
| Releves de telemetrie | 66 | 66 |
| Vitesse maximale | **50,0 km/h** | 49,7 km/h |
| En descente (pente <= -8 %) | 50,0 au pic, **31,4 km/h de moyenne** sur 39 releves | 49,7 au pic, 29,4 de moyenne sur 42 |
| En montee (pente >= +8 %) | 6,0 au pic, **5,4 km/h de moyenne** sur **20** releves en mouvement | 6,0 au pic, 5,5 de moyenne sur 20 |
| Releves a l'arret en cote | **2** | **2** |
| Denivele reellement gagne en pedalant | **4,4 m** | **4,4 m** |
| Pente rencontree | -71,0 % a +16,2 % | -69,9 % a +16,4 % |
| Altitude parcourue | 422,5 a 519,3 m, **96,9 m suivis** | 424,0 a 519,4 m, 95,4 m suivis |

Les deux runs sont **reproductibles au dixieme** sur les grandeurs qui comptent
(denivele gagne, nombre de releves en montee, calages) - ce qui n'etait pas le
cas des versions precedentes, ou le denivele gagne oscillait entre 1,9 et 6,3 m
d'un run a l'autre. C'est le signe que le scenario ne fonctionne plus par chance.

Le critere de succes ne se contente pas de constater que les touches ont ete
recues. Il exige une descente ET une montee franches, une vitesse de descente
reelle sous le plafond de 50 km/h, une montee plus lente que la descente **en
moyenne calculee en mouvement**, au moins **2 m d'altitude reellement GAGNES en
pedalant** - une pente positive peut etre "rencontree" a l'arret, et un pic de
vitesse en cote peut n'etre que l'elan emporte depuis la descente - et au moins
12 m de denivele suivi.

Le velo a traverse des pentes locales jusqu'a -71 % sans diverger, sans NaN, sans
jamais depasser son plafond (il l'atteint : 50,0 km/h pile).

---

## 5. Ce qui n'est pas satisfaisant, honnetement

1. **La pente franchissable de 23,6 % est basse pour ce massif.** En dessous, le
   velo monte proprement et sans a-coups : sur le versant a +14,8 % du scenario E,
   20 releves en mouvement contre **2 calages** seulement. Au-dessus, il
   s'immobilise net - et le massif depasse 23,6 % tres souvent (les versants
   depouilles ont des devers moyens de 25 a 29 %). Sur une premiere version du
   scenario, posee sur un versant a 27 %, le rapport s'inversait : 41 releves a
   l'arret contre 3 en mouvement.

   Le modele **suppose qu'on zigzague** - il lit la pente LE LONG DU CAP, donc
   traverser en biais reduit reellement l'effort, et c'est exactement ce que fait
   un vetetiste. C'est jouable, mais un joueur qui pointe droit dans la pente est
   bloque sans explication. `pedalAcceleration` est le parametre a revoir, **mais
   pas a l'aveugle : il faut d'abord decider si "monter droit dans un versant a
   27 %" doit etre possible dans ce jeu.** A pied, ca ne l'est pas non plus.

2. **La vitesse limite demande beaucoup plus de distance sur du relief MESURE que
   sur un plan.** Les 47 km/h annonces a -30 % supposent une pente soutenue ; un
   vrai versant est fait de terrasses et de ressauts. Le premier essai de
   scenario, sur un versant a -21 %, plafonnait a 23 km/h parce qu'une terrasse
   coupait la descente au bout de 25 m. Le versant retenu, a -25 %, atteint bien
   le plafond de 50,0 km/h - mais la **moyenne** sur toute la descente n'est que
   de 31 km/h. C'est un resultat, pas un defaut : le terrain reel est plus
   contraignant que le modele.

3. **Le marqueur est un cube.** Comme le pilote de parapente. Il est en revanche
   oriente CORRECTEMENT (cap, tangage, roulis), ce qui est le seul moyen de lire
   a l'ecran l'assiette calculee - donc de verifier a l'oeil ce que disent les
   tests. Un vrai mesh de velo viendra avec l'habillage.

4. **Pas de suspension, pas de transfert de charge, pas de derapage.** Le
   `dragCoefficient` groupe tout ca dans un seul terme, ce qui est assume mais
   fait de lui le parametre le plus arbitraire du modele.

5. **La collision ne concerne que les arbres de `Forest`** (16, autour de
   l'origine). Les dizaines de milliers d'arbres de `Vegetation` se traversent
   toujours - limite deja ouverte depuis la Phase 11, que le velo rend plus
   voyante puisqu'on couvre du terrain bien plus vite.

6. **Aucun controle d'image** n'a ete fait sur la vue a la troisieme personne du
   velo. La garde au sol de la camera est raisonnee et codee, pas mesuree sur une
   capture. C'est la meme limite que celle ouverte depuis la Phase 10 : aucun
   test automatique ne regarde l'IMAGE.

## 6. Hors scope, assume (conforme a la consigne)

Pas de degats ni de chute en cas de choc ; pas d'usure du velo ; pas de velo
comme objet ramassable ou fabricable. Le velo est disponible en permanence par la
touche B.

---

## 7. Non-regression

- **334 assertions headless** (280 avant, +54), du premier coup.
- **`run_vulkan_check.ps1` : 5 scenarios sur 5, deux runs consecutifs**, zero
  `[Validation]`, zero `[ERROR]`, zero `[WARN]`, codes de sortie 0/0/0/0/0,
  fermeture propre partout. Scenarios A a D (gameplay synthetique, collision,
  dalle LiDAR, parapente) **inchanges et verts** : 2 abattages, 3 debitages,
  ramassages, 1 percussion, vol de parapente avec atterrissage naturel.
- **Scenes annexes** relancees une a une (lancement, 6 s, `WM_CLOSE`) : `cube`,
  terrain procedural, dalle de Phase 7 en `--no-lod`, et la zone de 25 km2 a
  1 m. **Toutes : code de sortie 0, zero `[Validation]`, zero `[ERROR]`, zero
  `[WARN]`, fermeture propre.**
- `vercora.exe --dump-world` : **16 lignes AIM sur 16**, inchange.
- Aucun chemin de rendu touche : `Bike` ne parle pas a Vulkan, et le seul ajout
  cote rendu est un `DrawItem` de plus (le marqueur), sur le materiau `Object`
  qui existait deja.
- `Camera` : une valeur d'enum ajoutee (`Ride`) et deux branches qui la traitent
  comme `Glide`. Aucune modification du mode Walk, Free ou Glide.
