# Phase 14 (2026-08-22) : premiere version de la physique d'escalade

Quatrieme et derniere activite du cadrage. Meme intention que le parapente
(Phase 8) et le velo (Phase 13) : un modele simplifie mais **chiffre et
testable**, pose sur du relief LiDAR reel, pas une maquette.

Nouveau : `src/world/Climber.{hpp,cpp}` et `tools/find_climb_walls.py`.
`CameraMode::Climb` s'ajoute a `Camera`. **Aucun chemin de rendu n'est touche** -
c'est la premiere activite qui n'ajoute meme pas un marqueur, puisqu'elle est a
la premiere personne.

---

## 1. Detection de paroi : le seuil, et pourquoi il ne se lit pas dans la donnee

### Ce que le terrain contient reellement

`tools/find_climb_walls.py` (nouveau) depouille un `.vchm` et sort la
distribution des pentes. Sur la zone de jeu assemblee (25 km2, **24 970 009
echantillons evaluables**), pente lue par differences centrees sur une base de
4 m :

| | p50 | p75 | p90 | p95 | p99 | p99,9 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| pente | 47,7 % | 65,7 % | 92,4 % | 129,4 % | 342,5 % | 1127,0 % | 5018,2 % |
| angle | 25,5° | 33,3° | 42,8° | 52,3° | 73,7° | 84,9° | 88,9° |

**Ce massif est raide partout.** La pente MEDIANE est de 47,7 % ; la moitie du
terrain est plus pentue qu'un toit. Et surtout :

> **La distribution est un CONTINUUM. Il n'y a aucune bimodalite, donc aucun
> "creux" naturel ou poser le seuil.** Le seuil de paroi est un choix de
> conception, pas une decouverte, et il faut le dire ainsi.

Part de la zone retenue selon le seuil :

| seuil | 30 % | 50 % | 70 % | **100 %** | 150 % | 200 % | 300 % |
|---|---:|---:|---:|---:|---:|---:|---:|
| angle | 16,7° | 26,6° | 35,0° | **45,0°** | 56,3° | 63,4° | 71,6° |
| part de la zone | 79,6 % | 46,5 % | 20,7 % | **8,4 %** | 3,8 % | 2,3 % | 1,2 % |

### Le seuil retenu : 1,00 (45°, 100 %)

Trois arguments, aucun tire de la distribution :

1. **45° est la borne basse conventionnelle du terrain d'escalade.** En dessous
   c'est une desescalade ou un pas d'alpinisme facile ; au-dessus il faut les
   mains.
2. **C'est nettement au-dessus de l'angle de repos des materiaux meubles**
   (35 a 40°, soit 70 a 84 %). Une face a 45° dans ce massif ne retient ni
   eboulis ni terre : c'est du rocher nu, donc precisement ce qui se grimpe. Le
   seuil separe donc une realite physique, meme si la distribution des pentes,
   elle, ne la montre pas.
3. **8,4 % de la zone.** Assez pour que l'escalade soit une vraie facon de
   franchir le relief, assez rare pour que la marche reste le mode par defaut.
   A 70 % on retiendrait 20,7 % du massif et tout versant deviendrait une
   paroi ; a 150 %, 3,8 %, et l'activite ne servirait plus que sur les grandes
   falaises inabordables.

### Deux seuils, pas un : l'hysteresis

- **entree en escalade** : pente >= `wallSlope` = **1,00**
- **sortie (sommet)** : pente < `walkableSlope` = **0,60** (31°)

Sans cet ecart, une paroi reelle - qui oscille autour de son seuil d'un metre a
l'autre - ferait entrer et sortir du mode escalade a chaque image. 0,60 est aussi
la valeur qu'emploie `find_climb_walls.py` pour juger si une paroi a un pied
abordable : les deux **doivent** rester d'accord.

### La base de mesure : 4 m, la lecon de la Phase 13 rejouee

`slopeSampleStep = 2,0 m` (donc base de 4 m), identique a
`BikeParams::slopeSampleStep` et pour la meme raison : a 1 m - le pas de la
heightmap - le bruit decimetrique d'un DTM LiDAR fabrique des pentes fantomes.
Pour le velo ca produisait 8 % de micro-sections en contre-pente ; ici ca
produirait des "parois" au milieu d'un pierrier. 4 m est aussi l'echelle physique
du probleme : un grimpeur fait 2 m, il ne s'accroche pas a une bosse d'un metre.

### Raide ne suffit pas : `minWallRun` / `minWallRise`

**Ce parametre existe parce qu'un echec l'a impose, pas par precaution.**

La premiere version n'exigeait qu'un point au-dessus de `wallSlope` a portee. Sur
du relief mesure, le joueur qui s'approche s'accroche alors au **premier point
raide rencontre**, qui est souvent un ressaut isole de deux metres devant la
paroi. Constate directement dans les tests headless :

> engagement -> "SOMMET atteint" apres **0,3 m gagnes en 1,2 s**.

La base de 4 m elimine le bruit decimetrique, mais elle ne peut rien contre un
accident de terrain **reel** de deux metres : celui-la est une vraie pente forte,
simplement il ne mene nulle part. La seule question qui distingue une paroi d'un
ressaut est donc *"est-ce qu'y grimper fait reellement gagner de la hauteur ?"* -
et c'est exactement ce que `Climber::wallRise()` demande : simuler 6 m de
progression le long de la ligne de plus grande pente et exiger **4 m de gain**.

Les deux valeurs sont accordees sur `wallSlope`, pas choisies au hasard : a 45°
pile, 6 m parcourus sur la surface font gagner 4,24 m. Un point tout juste au
seuil passe de justesse, tout ce qui s'aplatit avant est rejete.

---

## 2. Le modele de deplacement

### L'idee, en une phrase

> Le grimpeur reste **exactement sur la surface**, et il s'y deplace le long de
> deux directions tangentes : la **ligne de plus grande pente** (monter,
> desescalader) et la **ligne de niveau** (traverser).

C'est le meme invariant que le velo - *la verticale n'est jamais une variable
d'etat, elle est relue de `heightAt()` a chaque pas* - et c'est ce qui rend
l'escalade compatible avec une heightmap **sans rien ajouter au moteur** : il n'y
a pas de "volume" de falaise a representer, seulement une surface a suivre.

### Ce que la projection produit toute seule

Avancer d'une longueur `L` le long de la surface consomme
`L / sqrt(1 + pente²)` de deplacement **horizontal**. Aucun cas particulier n'est
ecrit pour les parois verticales : c'est cette seule ligne qui fait que

| pente de la paroi | angle | gain d'altitude par metre parcouru | avance horizontale |
|---:|---:|---:|---:|
| 100 % | 45,0° | 0,707 m | 0,707 m |
| 300 % | 71,6° | 0,949 m | 0,316 m |
| 2000 % | 87,1° | **0,999 m** | **0,050 m** |

Une paroi a 87° - il y en a de vraies dans la zone de jeu - se monte donc quasi
verticalement, par construction. **Verifie par test** (`tests/logic_tests.cpp`,
section Phase 14).

### La limite honnete : pas de surplomb

Une heightmap 2,5D ne peut pas representer un devers. C'est la meme limite que
celle deja documentee en Phase 9 sur les noeuds partages des falaises. Une paroi
qui deverse est vue par la donnee comme une paroi tres raide mais positive, et se
grimpe comme telle.

### Vitesses

| | valeur | commentaire |
|---|---:|---|
| ascension / desescalade (le long de la surface) | **1,0 m/s** | 14 fois plus lent que la marche du moteur (14 m/s) |
| traversee laterale | **0,6 m/s** | une traversee est reellement plus dure : on deplace un appui a la fois |

Un grimpeur reel progresse a 0,1 - 0,3 m/s en rocher soutenu : **1,0 m/s est donc
environ 5 fois trop rapide, et c'est assume.** C'est exactement la meme concession
que la vitesse de marche du moteur, qui vaut deja ~10 fois une marche reelle. Ce
qui compte pour la consigne "plus lent que la marche normale" est le RAPPORT, et
il est de 14.

### Collision

`moveBody()`, **le meme point d'entree unique que le joueur a pied et que le
velo**. Rien n'est reecrit. Une seule difference, justifiee : la bande verticale
du corps est **centree** sur le point d'accroche (`bodyHalfHeight = 0,9 m`) au
lieu de partir des pieds, parce qu'en paroi le corps est etendu le long de la
surface et non debout dessus.

---

## 3. La jauge d'energie

Une seule jauge dans `[0, 1]`, volontairement isolee : la consigne demandait ce
parametre seul, pas un systeme de besoins primaires.

**Les couts sont PAR METRE, pas par seconde**, partout ou un travail physique est
en jeu. Consequences : monter 40 m coute la meme chose qu'on le fasse vite ou
lentement (c'est le comportement correct), et la jauge est **independante du
nombre d'images par seconde**. Les deux proprietes sont testees.

| poste | valeur | ce que ca donne |
|---|---:|---|
| montee | 0,016 / m d'altitude gagnee | **62,5 m** de paroi strictement verticale par jauge pleine |
| desescalade | 0,005 / m d'altitude perdue | 3x moins cher que la montee |
| horizontal (traversee) | 0,005 / m | on ne lutte pas contre la pesanteur |
| suspendu immobile | 0,004 / s | **le seul cout au temps** : 250 s (4 min) |
| recuperation, **au sol uniquement** | 0,05 / s | 20 s pour une jauge vide |
| minimum pour s'engager | 0,15 | evite de s'accrocher avec 1 % et de retomber aussitot |

**Desescalader coute trois fois moins cher que monter, et c'est un choix de JEU,
pas de physique.** Une desescalade reelle fatigue presque autant. Mais redescendre
doit toujours rester une porte de sortie : sinon un joueur qui s'engage trop haut
n'a plus que la chute comme issue, ce qui rend l'activite punitive au lieu d'etre
tendue.

**La recuperation ne se fait jamais en paroi.** C'est ce qui fait de la jauge une
contrainte de TRAJET et pas un simple compteur : il faut topper ou redescendre.

### Le chiffre qui donne son sens a l'activite

Mesure par test, sur une paroi a 87° (la plus raide qu'on trouve reellement) :

> **jauge pleine = 61,5 m d'ascension.**

Confronte a ce que le terrain contient (mesure par `find_climb_walls.py`) :

- les parois **abordables** de la zone font 30 a 44 m de haut -> **elles passent** ;
- les grandes falaises du massif font **250 a 276 m** -> **hors de portee d'une
  seule jauge.**

Ce n'est pas un reglage rate. C'est la limite qui donne son sens a l'activite, et
elle correspond a la realite : une paroi de 270 m se fait en plusieurs longueurs
avec des relais - ce que cette session n'a pas.

---

## 4. Les transitions

Touche **X** (`GLFW_KEY_X`). Meme raisonnement que pour le B du velo : X occupe
la **meme position physique en AZERTY et en QWERTY** (rangee du bas
W-X-C-V-B / Z-X-C-V-B). Les quatre activites et les deux actions d'outillage
occupent ainsi une rangee contigue sous la main gauche : **X** escalade,
**C** percussion, **V** parapente, **B** velo.

En paroi : **Z** monter, **S** desescalader, **Q/D** traverser, **X** lacher.

### Entree

Depuis la **marche** uniquement. `Climber::tryStart` sonde 8 points repartis de 0
a `reach` = 2 m devant le **regard projete a l'horizontale** (lever les yeux vers
le haut de la falaise ne doit pas raccourcir la portee au sol) et s'accroche au
premier point qui est une paroi **et** qui passe le test `wallRise`.

L'engagement peut **echouer** - c'est ce qui distingue `tryStart()` du `mount()`
du velo, qui ne pouvait pas refuser - et le jeu dit alors pourquoi : pas de paroi
a portee, ou jauge trop basse.

### Sorties : trois, et une seule reinitialisation de position

| sortie | ou finit le joueur | declencheur |
|---|---|---|
| `ToppedOut` | **la ou il est arrive** | pente < 0,60 au point d'accroche ET 3 m plus loin |
| `Released` | au pied de la paroi | touche X |
| `Exhausted` | au pied de la paroi | jauge a 0 -> chute |

**Le sommet est la seule sortie qui ne ramene pas au pied**, et c'est toute la
recompense de l'activite.

Le point de repli des deux autres est la **position du joueur au moment de
l'engagement** : par construction du terrain ou il se tenait debout l'image
d'avant, donc du terrain ou l'on peut le reposer sans risque. Le calculer
autrement - chercher le pied de la falaise apres coup - serait chercher une
position libre sans garantie d'en trouver une. **Aucun degat** : hors scope.

`release()` et l'epuisement partagent **exactement le meme code de
reinitialisation** : lacher une paroi, c'est se retrouver au pied, qu'on l'ait
decide ou non. Une seule reinitialisation a ecrire, donc une seule a verifier.

### La confirmation du sommet : le piege de cette phase

`hasToppedOut` teste **deux** points, pas un : le point d'accroche **et** un point
3 m plus loin dans la pente.

Un seul point sortirait du mode escalade sur la moindre dalle horizontale au
milieu d'une face - donc **en plein vide**. Exiger que ca tienne sur 3 m demande
une vraie vire. Et une vire de 3 m de profondeur **est** un endroit ou l'on se
tient debout : y terminer l'escalade est donc le comportement correct, pas un
pis-aller. Les deux cas sont testes (vire de 1 m : on continue ; vire de 6 m : on
s'arrete).

**L'ordre des deux tests de sortie compte** : le sommet est teste **avant**
l'epuisement. Poser la main sur la vire avec sa derniere goutte d'energie, c'est
topper, pas tomber - et c'est la seule lecture qui ne frustre pas. Fige par test.

---

## 5. La camera : premiere personne, regard libre

`CameraMode::Climb`. **Contrat MIXTE, le seul du projet** :

- la **position** vient de `world/Climber` (imposee par `setPosition()` une fois
  par image), comme pour `Glide` et `Ride` ;
- l'**orientation reste au joueur** : la souris continue de fonctionner, alors
  qu'elle est entierement coupee en vol et a velo.

### Pourquoi la premiere personne

1. **C'est l'information utile.** Sur une paroi, ce qui compte est la surface
   immediatement devant le visage. Une vue a la troisieme personne y placerait
   l'avatar exactement devant ce qu'il faut lire.
2. **La chase-cam n'a pas de bonne place sur une paroi.** Derriere le grimpeur,
   c'est le vide ; et des que la face s'incurve, l'oeil traverse le rocher. Le
   velo a deja demande une garde au sol minimale (`kRideChaseMinClearance`) pour
   ce genre de raison, sur du terrain bien plus doux. La premiere personne
   supprime le probleme au lieu de le rustiner.
3. **Coherent avec la camera reglable** : les activites rapides et lisibles a
   l'echelle du relief (parapente, velo) sont a la troisieme personne, l'activite
   de precision est a la premiere. Ce n'est pas une exception, c'est la regle qui
   se dessine.

### Pourquoi le regard reste libre, contrairement au vol et au velo

Parce que **les commandes de l'escalade sont relatives a la PAROI, pas au
regard** : monter suit la ligne de plus grande pente, traverser suit la ligne de
niveau, toutes deux imposees par le terrain. Le regard n'a donc plus aucune
influence sur le deplacement, et il redevient libre sans rien casser.

C'est meme necessaire : accroche a une face, on doit pouvoir lever les yeux pour
lire la suite de la voie et regarder en bas pour juger la hauteur, **sans que ca
deplace le grimpeur d'un centimetre**.

Effet de bord bienvenu : le scenario Vulkan de l'escalade n'a **rien a asservir**,
la ou celui du velo a demande un asservissement de cap (voir plus bas).

### Ou est l'oeil exactement

**La convention n'est PAS celle des trois autres activites.** A pied, a velo et
en parapente, la position du joueur est l'oeil place a `eyeHeight` = 1,7 m
**au-dessus** de la surface. En escalade, le grimpeur est **contre** la surface :

    oeil = point d'accroche + normale sortante x 0,6 m + 0,3 m vers le haut

Les 1,7 m de la marche n'ont aucun sens ici - ils placeraient l'oeil bien au-dessus
des mains. Le decalage de 0,6 m (une longueur d'avant-bras) evite d'avoir le
rocher dans le plan proche.

**Et il garantit que l'oeil est hors du relief** : le decalage se fait vers
l'AVAL, donc vers une altitude de terrain plus basse que celle de l'accroche.
Ce n'est pas un espoir, c'est teste sur 200 pas d'ascension et sur toute
l'ascension LiDAR reelle.

---

## 6. Verification

### Tests headless : 334 -> **425 assertions** (+91)

`tools\run_logic_tests.bat`. Comme pour le velo, ces tests mesurent des
**grandeurs** et les confrontent a ce qu'annonce l'en-tete, au lieu de re-deriver
les equations - ce qui ne prouverait que l'accord du test avec lui-meme.

Deux terrains de test nouveaux, parce que `SlopedTerrain` (plan infini) n'a ni
pied ni sommet et ne peut donc prouver aucune transition :
- `CliffTerrain` : plat / paroi / plat. Le plus petit terrain contenant les trois
  situations de l'escalade.
- `LedgedCliffTerrain` : la meme, avec une vire de largeur reglable a mi-hauteur.

Ce qui est verifie : detection de paroi (y compris pente NEGATIVE - c'est une
magnitude), hierarchie des deux seuils, engagement et ses quatre refus (dos a la
paroi, hors de portee, pas de paroi, regard vertical degenere), contact permanent
sur 400 pas, vitesse le long de la surface et sa projection verticale, ascension
quasi verticale a 87°, traversee sur la ligne de niveau et sa symetrie
gauche/droite, oeil hors du relief, couts d'energie par metre (et leur
independance au pas de temps), cout au temps en suspension, rapport
descente/montee, recuperation au sol seulement et jamais en paroi, hauteur
maximale par jauge pleine, **les trois sorties avec leur position finale exacte**,
priorite du sommet sur l'epuisement, vire etroite contre vire large, seuil
d'energie a l'engagement, collision par `moveBody`.

**Bloc sur relief LiDAR reel** (conditionnel a la presence de la dalle) :
`LHD_0899_6436_1m.vchm`, voie de reference E 899900 / N 6435550, cap 0°.

    [mesure] jauge pleine sur une paroi a 87 deg : 61.5 m atteints
    [mesure] voie de reference E 899900 N 6435550 : pente au depart 56.9 %
    [mesure] ascension sur relief LiDAR reel : 19.5 m gagnes en 26.1 s,
             sortie SOMMET, energie restante 60 %

### Vulkan reel : scenario F, **6 scenarios sur 6**

`tools\run_vulkan_check.ps1`, sous couches de validation. Dalle
`LHD_0907_6440_1m.vchm`, depart **E 907616 / N 6439312**.

**Le point de depart a ete choisi par mesure**, comme celui du velo. Le harnais ne
peut pas tourner la camera (GLFW lit le curseur SYSTEME), donc le joueur regarde
forcement plein NORD. Un balayage de la dalle entiere n'a trouve que **trois**
points reunissant les trois conditions : depart marchable + paroi grimpable a
moins de 2 m plein nord + ascension gagnant au moins 15 m.

Le run enchaine les deux sorties que le harnais peut atteindre : montee,
traversee droite et gauche, desescalade, **lachage volontaire**, repos au sol,
**re-engagement**, puis montee continue jusqu'au **sommet**.

    Engagement / lachage      : True / True
    Re-engagement / SOMMET    : True / True
    Releves de telemetrie     : 75
    Jauge d'energie           : minimum 14 %
      recuperation constatee  : True
    Gain d'altitude maximal   : 41,6 m au-dessus du pied de la paroi
    Pente sous les mains      : 64 % a 157 %  (seuil de paroi : 100 %)
    Altitude parcourue        : 501,5 m a 542,8 m  (denivele 41,3 m)

    Messages [Validation]     : 0
    Lignes [ERROR] / [WARN]   : 0 / 0
    Code de sortie A..F       : 0 / 0 / 0 / 0 / 0 / 0
    Fermeture propre A..F     : True / True / True / True / True / True

**41,6 m gagnes avec 14 % de jauge restante** : le dimensionnement de l'energie
produit bien la tension visee sur une paroi reelle, sans la rendre infranchissable.

**La CHUTE par epuisement n'est PAS exercee sous Vulkan**, et il faut le dire :
elle demanderait une paroi de plus de 62 m, et il n'en existe pas d'abordable
plein nord sur cette dalle. Elle est couverte par les tests headless, ou elle est
verifiee jusqu'a la position de repli exacte.

### Reproductibilite : l'escalade est deterministe, le velo ne l'est pas

Deux runs consecutifs de `run_vulkan_check.ps1`, meme binaire :

| | run 1 | run 2 |
|---|---:|---:|
| **F** gain d'altitude | **41,6 m** | **41,6 m** |
| **F** jauge minimale | **14 %** | **14 %** |
| **F** releves | **75** | **75** |
| E vitesse de pointe (velo) | 50,0 km/h | 44,1 km/h |
| E denivele suivi (velo) | 96,8 m | 67,7 m |

Le scenario F se reproduit **au chiffre pres** la ou le scenario E varie de 12 a
30 %. Ce n'est pas de la chance : le velo integre une vitesse, donc le moindre
decalage de timing dans le harnais s'accumule en distance parcourue et change le
terrain rencontre. L'escalade, elle, n'a **aucune quantite de mouvement** - la
vitesse est imposee, pas integree - et ses commandes sont relatives a la paroi,
donc la trajectoire ne depend que du terrain. Un decalage de timing ne change que
la duree, jamais le chemin.

Consequence pratique : les assertions du scenario F peuvent etre serrees sans
devenir instables, contrairement a celles du scenario E.

### L'erreur de raisonnement de cette session (la meme qu'en Phase 13)

Le premier test sur relief reel visait le centre d'une paroi reperee par
`find_climb_walls.py` (E 899850 / N 6435230) et l'abordait plein est. Il a
echoue - et l'echec etait instructif :

> **cette paroi DESCEND vers l'est.** Le test approchait donc par le SOMMET,
> marchait au bord de la falaise, et le grimpeur remontait correctement les 3,9 m
> qu'il venait de descendre avant de topper. **Le modele avait raison, c'est la
> geometrie du test qui etait fausse.**

C'est exactement la lecon du scenario E de la Phase 13 : *savoir qu'il y a de la
pente quelque part ne dit pas PAR OU l'aborder, et le terrain de l'autre cote
d'un point est un AUTRE terrain.* D'ou le mode **`--routes`** ajoute a l'outil,
qui evalue le relief **le long de chaque cap** depuis un pied marchable, au lieu
de se contenter de reperer des zones de paroi.

Corollaire mesure, a retenir : **le grimpeur suit la ligne de plus grande pente,
qui s'incurve, alors que l'outil mesure le relief le long d'un cap FIXE.** Les
deux trajets divergent, et le gain reellement obtenu (19,5 m) est inferieur a la
projection en ligne droite (21,4 m). C'est un resultat, pas un defaut - mais ca
interdit de recopier le chiffre de l'outil dans une assertion.

### Non-regression : complete

- **425 assertions headless** (334 avant, +91), toutes vertes.
- **`run_vulkan_check.ps1` : 6/6, zero validation, du premier coup**, deux runs
  consecutifs. Scenarios A a E inchanges et verts (2 abattages, 3 debitages,
  9 ramassages, 1 percussion, vol de parapente avec atterrissage naturel, sortie
  a velo a 50,0 km/h au pic sur 96,8 m de denivele suivi).
- **Scenes annexes**, relancees une a une : `cube`, terrain procedural, dalle de
  Phase 7, dalle de Phase 7 en `--no-lod`, zone 25 km2 a 1 m, zone 25 km2 sans
  vegetation. **Toutes a 0 validation / 0 ERROR / 0 WARN / fermeture propre.**
- `--dump-world` retrouve **16 AIM sur 16**.
- **Aucun chemin de rendu n'est touche.** Pas de nouveau `DrawItem`, pas de
  nouveau materiau, pas de nouvelle allocation Vulkan : l'escalade est a la
  premiere personne, il n'y a meme pas de marqueur a dessiner. C'est la seule des
  quatre activites dans ce cas. La synchronisation GPU n'ayant pas ete touchee,
  la validation de synchronisation (`VK_LAYER_VALIDATE_SYNC`) n'avait rien de
  nouveau a regarder.
- `Camera` : une valeur d'enum ajoutee (`Climb`), une branche dans `setMode`, et
  un booleen `orientationOnly` dans `update()` qui saute la bascule F et le
  deplacement clavier. Le bloc souris n'a **pas** ete duplique.

---

## 7. Hors scope, assume

Conformement au cadrage de la session :

- **pas de systeme de prises individuelles** : une paroi assez raide est
  grimpable entierement, il n'y a pas de "bon" ou de "mauvais" endroit sur la
  face ;
- **pas de materiel** : ni corde, ni mousqueton, ni point d'assurage, ni relais ;
- **pas de meteo** affectant l'adherence ;
- **pas de degats** : une chute est une reinitialisation de position propre, rien
  de plus ;
- pas de surplomb (limite structurelle de la 2,5D, voir plus haut).

---

## 8. Ce qui reste a ameliorer, par ordre d'importance

1. **Les grandes falaises du massif sont inaccessibles, et pas seulement a cause
   de l'energie.** Les six meilleures parois par denivele (250 a 276 m) ont **0 a
   2 % de pied marchable** dans leur anneau a 10 m : on ne peut litteralement pas
   les atteindre a pied. C'est le vrai verrou de l'activite, avant meme la jauge.
   Ce qui les debloquerait est un systeme de **relais** (reprendre de l'energie a
   mi-paroi) ET une facon d'y arriver - ce qui pointe vers le parapente comme
   moyen d'approche, donc vers une interaction entre activites qui n'existe pas
   encore.
2. **Aucune prise individuelle, donc aucune difficulte locale.** Toute paroi
   au-dessus du seuil se grimpe a la meme vitesse et au meme cout, qu'elle fasse
   105 % ou 2000 %. Faire dependre la vitesse et le cout de la pente locale
   serait la premiere amelioration a cout nul, et bien avant un vrai systeme de
   prises.
3. **Le seuil de 100 % est un choix, pas une mesure**, et il retient 8,4 % du
   massif. C'est le parametre a revoir en premier si l'escalade se revele trop
   presente ou trop rare a l'usage. `find_climb_walls.py` est fait pour rejouer
   la mesure a n'importe quel seuil.
4. **La descente n'a pas de cout de risque.** Desescalader coute trois fois moins
   cher que monter et ne peut jamais mal tourner : c'est la sortie sans risque,
   ce qui affaiblit la tension que la jauge est censee creer.
5. **Rien ne relie l'escalade a la marche en dehors de la touche X.** Un versant
   a 90 % se monte encore a pied sans effort - la marche n'a toujours aucune
   limite de pente. Tant que ce sera le cas, l'escalade restera facultative :
   on ne grimpe que ce qu'on veut bien grimper, jamais ce qu'il faut franchir.
6. **La chute n'est pas exercee sous Vulkan** (voir plus haut), faute de paroi
   assez haute abordable plein nord sur la dalle de test.
7. Aucun controle d'IMAGE sur la vue en escalade - meme limite ouverte que pour
   la vue a la troisieme personne du velo.
8. Pas d'animation ni de son : l'escalade est un glissement silencieux le long
   d'une surface.
