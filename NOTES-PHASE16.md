# NOTES-PHASE16.md — Besoins primaires du joueur

Session du 2026-08-24. Suite de la Phase 15 (editeur de niveau). Objectif :
donner au joueur quatre besoins primaires — faim, soif, fatigue, temperature —
qui evoluent avec le temps et l'activite, qui ont des CONSEQUENCES sur ce qu'il
peut faire, et qu'on puisse au moins partiellement satisfaire.

Ce fichier documente ce qui a ete construit, les chiffres retenus et POURQUOI,
les decisions de design prises sans arbitrage exterieur, et surtout ce qui ne
marche pas ou pas encore.

---

## 1. Ce qui existe maintenant

| Fichier | Role |
|---|---|
| `src/world/Needs.hpp/.cpp` | les quatre jauges, le climat, l'horloge de jeu, les coefficients de penalite |
| `src/world/WaterSource.hpp/.cpp` | detection des creux ou il est credible de boire, sur relief reel |
| `src/main.cpp` | cablage : contexte par image, application des penalites, touche T, titre, journal |
| `tests/logic_tests.cpp` | sections « Phase 16 » : **129 assertions** ajoutees (659 → **788**) |
| `tools/run_vulkan_check.ps1` | **scenario H** : les jauges et le point d'eau sous Vulkan, sur la dalle reelle |

Rien dans `Bike`, `Climber`, `Paraglider` ou `Camera` n'a change de comportement.
La seule modification de `Camera` cote appelant est que sa vitesse de marche est
desormais reglee a chaque image au lieu d'une fois au demarrage.

---

## 2. Le modele, et les chiffres

Les quatre jauges vivent dans `[0, 1]`, **1 = tout va bien** pour les quatre.
C'est une uniformite voulue : « fatigue 100 % » se lirait a l'envers, d'ou le
libelle **forme** a l'affichage.

### 2.1 L'unite de temps est la SECONDE REELLE

Tous les taux sont par seconde reelle, pas par heure simulee. L'horloge de jeu
(§2.4) n'accelere **que** l'heure, donc l'ambiante — jamais les jauges. C'est le
point le plus facile a confondre en relisant `NeedsParams`.

Cible du cahier des charges : rien de perceptible en quelques minutes, un vrai
enjeu sur 30 a 60 minutes. Valeurs retenues, jauges pleines, marche continue :

| Besoin | A l'arret | A la marche | Verifie par |
|---|---|---|---|
| faim | vide en 4 h | vide en 2 h 06 | `5 min de marche : la faim se voit a peine`, `30 min…` |
| soif | vide en 2 h 30 | vide en 1 h 05 | idem |
| forme | **remonte** (plein en 20 min) | vide en 1 h 30 | `45 min de marche : la moitie de la forme` |
| temperature | depend du relief et de l'heure | idem | §2.3 |

A 5 minutes, les trois jauges sont encore au-dessus de 90 % ; a 30 minutes, la
soif est sous 60 % et la forme sous 75 %. **L'ordre est voulu** : on a soif avant
d'avoir faim, et c'est la soif qu'on peut etancher. La faim est deliberement la
plus lente parce que c'est le seul besoin qu'on ne peut pas encore satisfaire.

### 2.2 L'effort est le seul curseur d'intensite

Une seule table (`activityEffort`), la marche valant 1 :

```
repos 0,0   marche 1,0   velo 1,8   escalade 4,0   parapente 0,3
```

Faim, soif, fatigue et chaleur produite en derivent toutes. Consequence
mesuree : l'escalade fatigue **exactement quatre fois** plus vite que la marche
(assertion dediee), le velo 1,8 fois.

L'escalade a deja sa jauge courte (`Climber::stamina`, quelques minutes). La
fatigue est l'**enveloppe longue** : elle n'empeche pas une paroi, elle empeche
d'en enchainer dix dans l'apres-midi.

Le **repos est le seul etat qui refait la forme**, et il exige l'immobilite
reelle : `main.cpp` lit le deplacement effectif de la camera entre deux images,
pas les touches. Un joueur qui pousse contre un rocher tient sa touche, n'avance
pas — et ne recupere pas non plus. Le mode vol libre (F) compte comme du repos :
ce n'est pas une facon de se deplacer dans le monde.

### 2.3 La temperature, et le lien avec `heightAt()`

```
ambiante = 15 °C (niveau de la mer)
         + 8·cos(2π(h − 15)/24)          cycle diurne, max a 15 h
         − 6,5 · altitude_km              gradient adiabatique standard
         + 4 · adret · ensoleillement     adret/ubac, nul la nuit

ressenti = ambiante + 1,5·effort − 3·(1 − forme) [− 6 en vol] [+ 5 a l'abri]
```

La jauge descend a `1/(1800·10)` par degre et par seconde sous le confort
(12 °C) : dix degres en dessous la vident en 30 minutes, cinq degres en une
heure. Elle remonte trois fois plus vite qu'elle ne descend — se remettre d'un
coup de froid ne doit pas couter une demi-session.

Sur la dalle chargee (420 → 982 m), le gradient donne **3,7 °C d'ecart permanent
entre le fond de vallee et la crete**, et le cycle diurne 16 °C entre 3 h et
15 h. Marcher de nuit coute donc a peu pres autant que marcher 2400 m plus haut.
Releve reel du scenario H : ambiante de **10 a 15 °C** entre 9 h et ~10 h de jeu
a 586 m.

**L'adret/ubac est calcule, pas devine.** `terrainExposure()` lit le gradient du
terrain : `+Z` va vers le sud (convention `Heightmap::toProjected`), donc un
versant qui descend vers `+Z` (`dh/dz < 0`) est un adret. L'exposition est
**ponderee par la pente** — sur du plat il n'y a ni adret ni ubac, sans quoi une
plaine bruitee produirait des expositions extremes et aleatoires d'un metre a
l'autre. La base de lecture est de 4 m, meme valeur et meme raison que
`BikeParams::slopeSampleStep`.

### 2.4 L'horloge

Le jeu n'avait aucune notion d'heure (le soleil des shaders est une direction
fixe). `GameClock` en ajoute une, au facteur **12** : une minute reelle vaut
douze minutes de jeu, donc une session d'une heure traverse reellement une
tombee de nuit. **Rien dans le rendu ne change avec elle** — voir §5.

### 2.5 Les consequences

Quatre coefficients multiplies, plancher absolu a **0,35** :

| Jauge | Seuil ou la penalite commence | Coefficient a jauge nulle |
|---|---|---|
| forme | 0,50 | 0,55 |
| faim | 0,25 | 0,70 |
| soif | 0,25 | 0,65 |
| froid | 0,25 | 0,70 |

Le produit brut des quatre a jauge nulle vaut 0,175 ; le plancher le remonte a
0,35. C'est la traduction chiffree de « pas de punition brutale » : au plus mal,
le joueur avance encore a plus du tiers de sa vitesse, il n'est jamais immobilise
loin de tout. Une assertion dediee verifie que ce plancher **mord vraiment**
(le produit brut est bien en dessous) — sans quoi il serait decoratif.

Les manques s'aggravent entre eux : un joueur affame, assoiffe ou glace se
fatigue exactement **deux fois** plus vite et recupere deux fois moins vite. Ce
qui empeche de compenser indefiniment la soif par des pauses.

**Un seul coefficient pour les quatre activites**, applique en trois points de
`main.cpp` :

* marche : `camera.setMoveSpeed(kWalkSpeed × facteur)` ;
* velo : la commande de pedalage est multipliee par le facteur. Le modele du velo
  n'a pas ete touche — il ecrete deja ses entrees dans `[0, 1]`, et une commande
  plus faible y produit exactement ce qu'on veut : moins d'acceleration, donc une
  vitesse d'equilibre plus basse, donc **moins de distance et une pente
  franchissable plus faible**. Un joueur epuise cale la ou il passait le matin ;
* escalade : la commande de montee, meme mecanique. **La desescalade est ralentie
  aussi** (le facteur s'applique au signe compris) : une desescalade a pleine
  vitesse serait la faille evidente du systeme.

Les tests ne verifient pas que le coefficient est petit, ils verifient l'**effet
reel** : distance parcourue a velo en une minute, et hauteur gagnee en 20 s
d'escalade, mesurees avec et sans la penalite d'un joueur epuise.

---

## 3. Les points d'eau

Le jeu n'a pas d'eau : ni surface liquide, ni rendu, ni physique de fluide.
`WaterSource` ne pretend pas en ajouter. Il repond a une question etroite : **ou,
sur un relief LiDAR reel, serait-il credible de boire ?**

Reponse cherchee dans la forme du terrain : les points a la fois **peu pentus**
et **concaves**. Le balayage se fait une seule fois au chargement.

### 3.1 Ce qui a ete essaye et ce qui a ete retenu

Premiere version : 4 sondes sur les axes a 25 m, pente ≤ 10 %, 3 sondes sur 4
plus hautes, balayage au pas de 50 m. Resultat sur la dalle reelle : **zero point
d'eau**. Trois causes distinctes, toutes mesurees :

1. **Ce massif est raide.** 562 m de denivele sur 1 km : au pas de 50 m,
   **seuls 10 points sur 361** passent sous 10 % de pente. Seuil porte a 15 %,
   ce qui reste tres loin des 60 % typiques du versant.
2. **Le pas de balayage etait trop grossier.** Le rayon utile d'un creux (celui
   ou la pente reste sous le seuil tout en gardant assez de concavite) vaut une
   vingtaine de metres. Un pas de 50 m passe simplement a cote. Ramene a **20 m**
   (62 500 candidats sur 25 km², ~800 000 `heightAt`, une seule fois).
3. **Quatre sondes ne savent pas distinguer un talweg d'un col.** Un talweg a
   deux sondes plus hautes (les versants) et deux plus basses (amont, aval) :
   exiger 3 sur 4 ne retient que les cuvettes fermees, qui n'existent
   pratiquement pas ici. Mais descendre a 2 sur 4 laisse passer les **cols**, qui
   ont exactement la meme signature et ou il n'y a rien a boire.
   → **Huit directions** (axes + diagonales), seuil a **5 sur 8** : un talweg a
   ses deux versants qui couvrent chacun plus d'une direction, un col n'en a que
   quatre. Le seuil passe entre les deux.

Parametres finaux : pas 20 m, sondes a 25 m, pente ≤ 15 %, concavite moyenne
≥ 1,0 m, 5 sondes sur 8 montantes, ecartement minimal 150 m, 64 points au plus.

**Resultat sur la dalle du Gua : 2 points d'eau**, a 3,27 m et 2,03 m de creux.
Le premier est en monde `(−75, 285)`, soit E 907425 / N 6439215.

### 3.2 Ce que cette methode ne sait pas faire

* une **source de versant** ne passe pas (elle n'est ni plate ni concave) ;
* un **replat de col** assez ferme peut passer ;
* une cuvette placee **entre deux points de la grille** peut passer inapercue ;
* elle ne sait rien des **saisons**, du debit, ni du gel.

C'est une heuristique, elle est retenue parce qu'elle est verifiable, bon marche,
et qu'elle donne un ancrage testable. La lecon de la Phase 14 vaut ici aussi :
savoir ou l'eau **pourrait** etre ne dit pas qu'elle y est. Le jour ou le reseau
hydrographique de l'IGN sera charge, c'est cette fonction qui disparaitra, pas
ses appelants.

### 3.3 Boire

Touche **T**, portee **4 m** a l'horizontale (un peu plus que le ramassage
d'objet : un point d'eau est une zone, pas un objet pose au centimetre pres).
Le refus est journalise **avec la distance** — sans elle, « trop loin » ne dit
pas s'il faut faire deux pas ou deux kilometres.

---

## 4. Ce qui prouve que ca marche

### 4.1 Tests headless — 129 assertions (659 → 788)

`tools\run_logic_tests.bat`. Couvrent :

* l'etat de depart, et le fait qu'un `deltaTime` nul ou negatif ne fait rien ;
* les **rythmes annonces** : 5 min ≈ rien, 30 min ≈ enjeu, 45 min = demi-forme,
  90 min = forme a plat. Le test de fatigue maintient faim et soif pleines, sans
  quoi il mesurerait en realite le couplage entre jauges et non la fatigue ;
* l'ordre des efforts entre les quatre activites, et le rapport exact de 4 entre
  escalade et marche ;
* le repos : recuperation en 20 min, et le fait qu'il ne rend **ni** faim **ni**
  soif — sans quoi s'arreter reglerait tout et le point d'eau ne servirait a rien ;
* l'aggravation croisee, verifiee au facteur exact (× 2 a manque total) ;
* les quatre coefficients, seuil par seuil, plancher par plancher, **plus** le
  controle que le plancher global mord ;
* le climat : gradient d'altitude au degre pres, denivele reel de la dalle,
  amplitude diurne, adret/ubac de jour **et** son absence de nuit ;
* l'exposition lue sur quatre reliefs : plat, est-ouest, adret franc, ubac franc,
  pente douce ponderee. Un terrain penche **selon Z** a du etre ajoute pour ca :
  `SlopedTerrain` penche selon X, et un test d'exposition dessus passerait meme
  avec un signe inverse ;
* la vitesse de refroidissement et de rechauffement, l'effet de l'effort, du vent
  de vol, de l'abri, et le fait qu'a 1000 m plus haut on se refroidit plus vite ;
* l'horloge : facteur 12, passage de minuit, reglages hors bornes ;
* les points d'eau : une cuvette trouvee **au bon endroit**, quatre controles
  negatifs (plat, versant raide, versant doux **sans** concavite, sommet de
  bosse), le retrait aux bords, l'emprise degeneree, l'ecartement, le
  determinisme, la recherche du plus proche en distance **horizontale** ;
* les **consequences reelles** : distance a velo et hauteur d'escalade, mesurees
  avec et sans penalite, sur les vraies classes `Bike` et `Climber`.

### 4.2 Vulkan reel — scenario H (8 scenarios, tous verts)

`tools\run_vulkan_check.ps1`. Depart **sur un point d'eau que le jeu a lui-meme
detecte**, puis :

* T au bord du creux → `Boire : soif etanchee au point d'eau (0.000000 m)` ;
* 30 m plus loin, T → `le point d'eau le plus proche est a N m`. Les deux
  ensemble, jamais l'un seul : une portee infinie donnerait le premier message,
  une portee nulle le second ;
* 160 s de marche puis 60 s d'immobilite complete. La telemetrie (une ligne par
  seconde) doit montrer la forme **descendre puis remonter** — les deux sens.
  Une jauge qui ne fait que descendre est une jauge sans repos ; une qui ne fait
  que monter ne coute rien.

Releve du run de reference : 236 echantillons, forme 100 → **98** → 100,
soif minimale 95 %, ambiante 10 à 15 °C, 2 points d'eau, **zero `[Validation]`**,
fermeture propre. Les sept scenarios anterieurs sont inchanges et verts.

Deux pieges rencontres en ecrivant ce scenario, notes parce qu'ils se
reproduiront :

* le scenario attendait d'abord `Boire : pas soif` au point d'eau, en supposant
  la jauge encore exactement pleine. **Elle ne l'est jamais** : deux secondes de
  jeu l'ont deja fait descendre de quelques millioniemes, invisibles a
  l'affichage entier mais suffisants pour que `drink()` reussisse ;
* a 100 s de marche, la forme tombait a 99 % puis 98 % selon les runs — la marge
  tenait a un point d'arrondi. Portee a 160 s (~3 %). Une marche genee par le
  relief (le joueur bute, donc ne se fatigue plus) aurait autrement efface le
  signal.

---

## 5. Decisions prises seul, et ce qu'elles coutent

1. **Aucune jauge ne tue.** Pas de mort, pas d'ecran de fin, pas de degat.
   Coherent avec « pas de punition brutale », mais ca veut dire qu'un joueur peut
   rester indefiniment a zero partout, ralenti et rien de plus. C'est un choix,
   pas un oubli.

2. **Les penalites sont appliquees aux ENTREES, pas dans les modeles.**
   `Bike`, `Climber` et `Paraglider` ignorent tout des besoins. Avantage : leur
   physique, testee depuis deux phases, ne peut pas regresser. Inconvenient : la
   fatigue ne peut pas exprimer autre chose qu'« une commande plus faible » — un
   grimpeur epuise ne tremble pas, il monte lentement.

3. **Une horloge de jeu, mais aucun changement visuel.** Le facteur 12 fait
   tomber la nuit pendant une session, et la temperature en tient compte, mais le
   soleil des shaders reste une direction fixe : **il fait toujours jour a
   l'ecran**. C'est incoherent visuellement et c'est assume — un cycle
   jour/nuit rendu est une phase a lui seul.

4. **`eat()` existe sans aucun appelant.** Le jeu ne produit rien de comestible
   (l'inventaire ne connait que buches, branches, pierres, eclats), et inventer
   une cueillette sans contexte narratif depassait le cadre. La fonction est
   ecrite et testee pour que le jour venu il n'y ait rien a inventer ici. **La
   faim n'est donc, a ce stade, satisfaisable par rien** — d'ou le choix d'en
   faire de loin le besoin le plus lent.

5. **`NeedsContext::sheltered` existe sans aucun appelant non plus**, pour la
   meme raison : aucun abri n'existe dans le jeu. Le champ est le point
   d'ancrage prevu pour l'artisanat, et les tests l'exercent deja.

6. **Le mode editeur gele les quatre jauges.** C'est un outil de developpement :
   voir la fatigue tomber en survolant 25 km² n'aurait aucun sens. Elles
   reprennent exactement ou elles en etaient a la sortie (F2).

7. **Affichage minimal, dans le titre de la fenetre**, comme l'inventaire depuis
   la Phase 6 : `faim 87% soif 62% forme 44% chaud 91% - 14:35`. Pourcentages
   **entiers**, et le titre n'est reecrit qu'une fois par seconde (au rythme du
   compteur de FPS) — le rafraichir soixante fois par seconde ferait soixante
   appels systeme pour montrer le meme texte. Le detail (temperatures, facteur de
   deplacement) va dans le journal, qui est ce que depouille le harnais Vulkan.

8. **Le seuil de soif liee a la chaleur est distinct du confort.** Une premiere
   version reutilisait la temperature de confort (12 °C) comme seuil de « chaud »,
   ce qui rendait le joueur assoiffe par une journee de printemps a 17 °C : la
   soif doublait, et cinq minutes de marche la faisaient deja tomber de 15 %.
   Deux seuils separes desormais, 12 °C pour le froid et 22 °C pour la soif.

---

## 6. Ce qui reste ouvert

* **De quoi manger.** C'est le trou beant du systeme. Cueillette, chasse ou
  reserves de depart : c'est une decision de cadrage, pas une decision technique.
* **Un abri**, qui donnerait un sens a `sheltered` et un usage aux buches.
* **Le rendu du cycle jour/nuit**, sans lequel l'horloge reste invisible.
* **Le vrai reseau hydrographique** (IGN), qui remplacerait l'heuristique de
  detection des creux.
* **Le sommeil.** Le repos refait la forme, mais rien ne distingue une pause
  d'une nuit.
* **Le portage.** L'inventaire n'a ni masse ni encombrement ; le jour ou il en
  aura, la charge devrait entrer dans l'effort, donc dans `activityEffort`.
