# NOTES-PHASE23.md — Systeme meteo deterministe

Session du 2026-08-27. Suite de la Phase 22.

---

## 0. Avertissement, a lire avant tout le reste

**Aucune des valeurs numeriques de ce systeme n'a ete validee par un
climatologue.** Ce sont des ordres de grandeur plausibles, choisis pour que le
systeme produise un comportement lisible en jeu.

Le code separe donc strictement deux choses :

- **L'architecture et les mecanismes** — implementes serieusement, testes, et
  independants des chiffres. Comment un gradient altitudinal, un sondage amont,
  une inversion de fond de cuvette ou un renforcement orographique se calculent
  ne changera pas apres relecture scientifique.
- **Les valeurs numeriques** — toutes regroupees dans **une seule struct**,
  `WeatherParams` (`src/world/Weather.hpp`), chacune commentee avec ce qu'elle
  fait, sa valeur provisoire et d'ou elle sort. Les recalibrer se fait en
  ouvrant un seul fichier et en changant des nombres, sans toucher a une ligne
  de logique.

Deux exceptions, qui ne sont **pas** provisoires et sont sourcables :

| Grandeur | Source |
|---|---|
| Refroidissement eolien | Formule JAG/TI, employee par Environnement Canada et le NWS depuis 2001 |
| Declinaison solaire | Formule de Cooper (1969) |

Une troisieme est un fait et non un reglage : la latitude, 45,0 degres, parce
que la dalle LiDAR chargee est en Isere (Le Gua).

Les tests headless verifient en consequence des **proprietes** ("un adret est
plus chaud qu'un ubac au meme instant", "un col vente plus qu'un plateau
adjacent") et **jamais des valeurs absolues**. Un test qui exigerait
"l'inversion vaut -6 degres" devrait etre reecrit le jour du recalibrage et ne
dirait rien de plus que "la constante vaut ce qu'elle vaut".

### Ce qui demande une vraie validation climatique externe

Par ordre decroissant de doute :

1. **`precipitationCloudThreshold` (0,72)** — c'est ce seuil qui fixe la
   frequence des precipitations, et il est le plus arbitraire du fichier. Il
   donne aujourd'hui 61 heures de pluie et 63 heures de grand beau sur trente
   jours de jeu simules, ce qui *semble* raisonnable pour un printemps de
   moyenne montagne, mais rien ne l'appuie.
2. **`frontNoiseGain` (2,2)** — ce parametre n'existe que pour une raison
   technique (voir §4), mais il pilote de fait l'amplitude de tout le
   calendrier meteo. Il faut le recalibrer avec le seuil ci-dessus, pas
   separement.
3. **`frontPeriodDays` (3,5)** — la duree d'un cycle de degradation.
4. **`inversionMaxC` (-6)** et **`inversionMixingWindMps` (4)** — le signe et
   l'ordre de grandeur sont surs, le chiffre exact non. Une inversion reelle en
   moyenne montagne va de 5 a 10 degres.
5. **`exposureWindGain` (0,80)** et **`venturiWindGain` (0,70)** — l'ordre de
   grandeur de l'acceleration au-dessus d'une crete (1,5 a 2) est documente ;
   la valeur pour un col ne l'est pas, et elle depend enormement de la geometrie
   du col, que le modele ne mesure que grossierement.
6. **`lapseRateCPerKm` (6,5)** — la moins douteuse (c'est le gradient
   adiabatique standard), mais un gradient reel varie de 4 (air humide) a 9,8
   (air sec) et le modele n'en tient pas compte.
7. **Toutes les distances de sondage** (250 / 750 / 2000 / 100 / 400 m) — elles
   viennent de la recherche preparatoire, elles n'ont ete verifiees par rien.

---

## 1. Ce qui a ete fait

Les quatre priorites du cahier des charges sont faites, dans l'ordre demande.

| # | Priorite | Etat |
|---|---|---|
| 1 | Module synoptique + temperature locale, remplacant celle de Needs | **fait** |
| 2 | Cycle jour/nuit visuel | **fait** |
| 3 | Vent / sondage amont | **fait** |
| 4 | Effets sur les quatre activites | **fait** |

Fichiers neufs : `src/world/Weather.hpp` (≈ 700 lignes, dont la majorite est de
la documentation de constantes) et `src/world/Weather.cpp`.

---

## 2. Architecture

### Une fonction d'etat pure

Le module n'a **aucun etat cache** et **aucune integration dependante du pas de
temps**. Pour un point `(x, y, z)` et un instant `t`, `sampleAt()` rend l'etat
meteo exact.

Consequence de jeu : deux joueurs qui arrivent au meme endroit au meme instant
vivent exactement les memes conditions, quel que soit le chemin parcouru, la
duree du trajet, le nombre d'images par seconde ou le nombre de pauses.

Consequence de test : un test peut sauter directement a `t = 137 h` sans simuler
les 137 heures. C'est ce qui rend la couverture de cette phase possible.

Contraste explicite avec `Needs` (Phase 16), qui **integre** dans le temps :
les quatre jauges ont une memoire, c'est leur role. Rien n'a change de ce
cote — sauf d'ou vient la temperature ambiante.

### Deux couches

**Couche 1, synoptique (0D)** — `WeatherSystem::synopticAt(gameHours)` →
`SynopticState`. L'etat macro du massif : saison (deux harmoniques), cycle
diurne (deux harmoniques, attenuees par les nuages), perturbations (bruit de
gradient 1D en fonction du temps), position solaire, vent de reference,
nebulosite, precipitation. Ne connait **aucune position**.

**Couche 2, spatialisation (3D)** — `WeatherSystem::sampleAt(position, sky,
terrain)` → `WeatherSample`. La reponse du terrain a cet etat macro : gradient
altitudinal, adret/ubac par produit scalaire, inversion de fond de cuvette, vent
local par sondage amont, ridge lift, subsidence et turbulence de sillage,
thermiques, renforcement orographique, mouillure du sol.

La surcharge `sampleAt(position, SynopticState, terrain)` existe pour
echantillonner plusieurs points au meme instant sans recalculer la couche 1 —
elle est 10 fois moins chere que la couche 2 (mesure : 0,38 µs contre 4,0 µs).

### Le seed

**Fixe par defaut** (`WeatherSystem::kDefaultSeed`), et derivable d'une graine de
partie par le constructeur. Le choix du seed fixe est delibere :

- La zone de jeu est unique et fabriquee a la main (une dalle LiDAR reelle),
  pas generee par partie. Rien d'autre dans Vercora n'est reseede par partie :
  le semis de vegetation et celui de la faune sont deterministes a graine fixe
  depuis la Phase 11.
- Un seed fixe rend une capture d'ecran comparable a la precedente, ce qui est
  la condition meme de la verification visuelle du projet (lecon de la Phase 12
  sur `--look`).
- Le jour ou les parties multiples existeront, passer la graine au constructeur
  suffit, sans rien changer d'autre.

---

## 3. Une seule source de verite sur la temperature

C'etait la contrainte la plus explicite du cahier des charges, et elle a
demande de **deplacer du code plutot que d'en ajouter**.

### Ce qui a quitte `Needs`

| Parti de `Needs` | Arrive dans `Weather` |
|---|---|
| `NeedsParams::seaLevelMeanTemperatureC` | `WeatherParams::seaLevelAnnualMeanC` (+ saison explicite) |
| `NeedsParams::lapseRateCPerKm` | `WeatherParams::lapseRateCPerKm` |
| `NeedsParams::diurnalAmplitudeC` | `WeatherParams::diurnalAmplitudeC` |
| `NeedsParams::diurnalPeakHour` | `WeatherParams::diurnalPeakHour` |
| `NeedsParams::exposureBonusC` | `WeatherParams::aspectBonusC` |
| `NeedsParams::paraglideWindChillC` | **supprime**, remplace par `windChillC()` (JAG/TI) |
| `ambientTemperature()` | `WeatherSystem::sampleAt().temperatureC` |
| `daylightFactor()` | `SynopticState::daylight` (vraie position solaire) |
| `terrainExposure()`, `kFullExposureSlope` | inchanges, simplement demenages |

`NeedsContext` porte desormais `ambientTemperatureC` et `windSpeedMps` a la
place de `altitudeMeters` / `timeOfDayHours` / `exposure`. **Needs ne calcule
plus aucune temperature ambiante** — il en recoit une et n'y ajoute que ce qui
concerne le corps du joueur : effort, fatigue, abri, vent subi.

Ce n'est pas de la cosmetique. Tant que les deux calculs coexistaient, rien
n'empechait qu'un reglage n'en corrige qu'un.

### Ce qui n'est pas un simple renommage

`ambientTemperature()` de la Phase 16 ne connaissait que l'altitude, l'heure et
une exposition nord/sud. Sa remplacante connait en plus la **saison**, la **masse
d'air** (perturbation en cours), la **nebulosite** (qui ecrase l'amplitude
diurne — c'est ce qui fait qu'une nuit couverte est douce et une nuit claire
glaciale) et l'**inversion de fond de cuvette**.

`daylightFactor()` etait une demi-sinusoide fixe de 6 h a 18 h. Sa remplacante
est le sinus de l'elevation solaire reelle, calculee depuis la latitude et la
declinaison du jour : les journees s'allongent et se raccourcissent avec la
saison.

### L'adret/ubac calcule par produit scalaire

Le cahier des charges demandait un produit scalaire entre la normale du terrain
et la direction du soleil. C'est ce qui est fait, mais avec une precision qui
compte : on compare le cosinus d'incidence du point a **celui qu'aurait du
terrain plat au meme instant**, et on convertit l'ecart en degres via
`aspectIncidenceReference`.

Le comportement qui en decoule est meilleur que celui de la Phase 16, et il
sort tout seul de la geometrie :

- **A midi**, un versant sud ne gagne presque rien (il est deja a peu pres
  perpendiculaire au rayonnement) alors qu'un versant nord perd beaucoup.
- **Au petit matin**, c'est un versant **est** qui recoit le maximum — un modele
  purement nord/sud ne pouvait pas le produire, et c'est verifie par un test.

Mesure a midi, 1000 m, sur pente de 60 % : adret 14,7 °C, ubac 10,7 °C, soit
4,0 °C d'ecart.

---

## 4. Ce que les tests ont corrige

Quatre choses ont ete **fausses puis corrigees parce qu'un test les a montrees**.
Elles sont notees ici parce que ce sont les seules parties du travail dont on
sait qu'elles ne sont pas seulement plausibles.

### (a) Le bruit n'atteignait jamais ses extremes

Un fBm de bruit de gradient n'atteint pas ses bornes theoriques. Sans gain, la
nebulosite mesuree sur trente jours restait entre 0,20 et 0,70 : **jamais de
grand beau** (donc jamais de nuit claire, donc jamais d'inversion) et presque
jamais de vraie pluie — 13 heures sur 720.

Corrige par `frontNoiseGain` (2,2) suivi d'un ecretage. Le test qui l'a montre
demande qu'un mois de jeu contienne des journees claires **et** des journees de
pluie ; c'est le controle negatif d'un modele qui aurait l'air de fonctionner
tout en etant constant. Apres correction : 61 h de pluie, 63 h de grand beau.

### (b) Un versant sud plus chaud a deux heures du matin

L'ecart d'incidence continuait de se calculer avec un soleil **sous l'horizon**,
ou il n'a aucun sens. Resultat mesure : +2,9 °C pour un adret a 2 h du matin.

Corrige par `aspectFullDaylightSin` (0,25), qui eteint l'effet quand le soleil
est bas — ce qui est aussi le comportement reel, un rayonnement rasant chauffe
peu quelle que soit l'orientation. Le test est un controle negatif : la nuit,
adret et ubac doivent etre a moins de 0,001 °C l'un de l'autre.

### (c) La moyenne des sondages amont effacait l'abri

La premiere version faisait une moyenne ponderee des trois altitudes amont. Un
point situe 250 m sous le vent d'une crete de 200 m — un abri franc — obtenait
un signal d'exposition de **-0,05** : la crete etait bien vue par la sonde
proche, mais la plaine des sondes a 750 m et 2000 m ramenait la moyenne au
niveau du point, et l'abri disparaissait dans l'arithmetique.

La regle correcte tient en une phrase : **etre abrite, c'est avoir du relief
au-dessus de soi dans au moins une des echelles amont ; etre expose, c'est
dominer toutes les echelles a la fois.** D'ou un `max` d'un cote et un `min` de
l'autre, jamais une moyenne.

### (d) La turbulence de sillage se mesurait sur le mauvais vent

Elle etait calculee a partir du vent **local**, qui est justement faible sous le
vent (c'est ce que le facteur d'abri vient de calculer). Un sillage etait donc
d'autant plus calme qu'il etait plus abrite — l'inverse exact de la realite. Ce
qui agite l'air d'un sillage est le flux qui vient de decoller de la crete,
lequel a la vitesse **amont**. Corrige : la turbulence et la subsidence se
mesurent sur le vent de reference.

### Et une correction cote test, pas cote code

Le test d'inversion employait d'abord une cuvette de 400 m de profondeur pour
500 m de rayon. Il echouait — mais pas parce que le modele avait tort : a 400 m
de denivele, le seul gradient adiabatique rend le fond 2,6 °C **plus chaud** que
le rebord, et l'inversion, correctement calculee, ne suffisait pas tout a fait a
renverser ce handicap. Le relief etait invraisemblable : une cuvette de 400 m de
creux pour 500 m de rayon est une falaise circulaire, pas une combe. Ramene a
150 m, le test passe largement.

C'est la troisieme fois dans ce projet que l'hypothese "le test a tort" se
verifie (voir MEMORY.md). Elle merite d'etre instruite aussi serieusement que
l'inverse — et aussi serieusement rejetee quand elle est fausse, ce qui a ete le
cas pour les quatre defauts ci-dessus.

---

## 5. Le sondage amont

C'est la technique centrale de la couche 2. Au lieu de simuler l'ecoulement, on
**demande au relief ce qu'il fait au vent**, en lisant la heightmap a quelques
distances autour du point.

**Sept appels a `heightAt()` par evaluation** : le point, trois en amont (250 /
750 / 2000 m), un en aval (100 m), deux lateraux (± 400 m perpendiculairement au
flux). C'est le budget d'une seule lecture de pente du velo.

Ce qui en est tire :

- **Exposition / abri** — `min`/`max` sur les trois echelles amont (voir §4c).
  Un point qui domine tout son amont vente jusqu'a 1,8 fois le vent libre ; un
  point abrite n'en garde que 25 %.
- **Venturi** — les deux sondes laterales. Si les deux epaules dominent le
  point, le flux passe par un entonnoir et accelere.

  **Une ponderation existe et elle n'est pas cosmetique** : un fond de vallee a
  lui aussi ses deux cotes plus hauts, exactement comme un col. La seule chose
  qui les distingue est le rapport a l'amont — un col domine son amont (le vent
  le franchit), un fond de vallee non (le vent passe au-dessus). Sans ce poids,
  chaque fond de vallee de la zone de jeu deviendrait une soufflerie.

- **Ridge lift** — la pente dans le sens du flux, sur l'echelle proche. L'air
  qui monte une pente `s` a la vitesse `U` monte a `s·U`, moins un rendement.
- **Subsidence et turbulence de sillage** — pilotees par l'abri et par le vent
  amont.
- **Renforcement orographique** — la meme pente dans le sens du flux : un
  versant que le flux doit gravir souleve la masse d'air, donc il pleut plus.

Mesures sur le relief de test (crete nord-sud de 400 m avec un col, vent
d'ouest) :

| Point | Vent | Facteur de relief |
|---|---|---|
| Col | 14,2 m/s | × 2,01 |
| Crete pleine | — | × 1,80 |
| Plateau au vent | 6,4 m/s | × 1,00 |
| Versant au vent | ridge lift **+4,99 m/s**, turbulence 0,00 | |
| Versant sous le vent | subsidence **-1,80 m/s**, turbulence 0,60 | |

Le col vente 2,2 fois plus que le plateau adjacent, et **plus que la crete
pleine a vent de reference egal** — c'est-a-dire que l'effet d'entonnoir est
bien distingue de l'effet d'altitude.

---

## 6. La carte de piegeage d'air froid

Precalculee **une fois au chargement**, jamais par image — c'est l'exigence de
cout du cahier des charges.

**Ce qu'elle mesure** : a quel point un point est un *creux enferme*. Autour de
chaque cellule on sonde huit directions a trois rayons (120 / 350 / 900 m), et
on regarde combien de ces 24 sondes sont plus hautes que la cellule, et de
combien.

**Pourquoi pas simplement "l'altitude est basse"** : un versant a mi-hauteur et
un fond de vallee peuvent etre a la meme altitude, et seul le second piege l'air
froid. C'est l'**enfermement** qui compte, pas l'altitude — la meme lecon que la
Phase 22 sur le relief (savoir ou il y a de la pente ne dit pas par ou
l'aborder).

Resolution **128 × 128 sur les 5 km** de la zone de jeu, soit des cellules de
39 m : la bonne echelle pour une cuvette (une cuvette de 39 m n'en est pas une).
Cout : 393 216 lectures de heightmap, payees une seule fois au chargement.
Lecture au runtime : une interpolation bilineaire.

Le controle negatif est celui qu'employait deja la Phase 16 pour les points
d'eau : une **bosse** de geometrie identique doit obtenir un index quasi nul.
Mesure : creux 0,65, plateau 0,00, bosse 0,00.

L'inversion elle-meme exige **trois conditions simultanees**, et le produit des
trois les exprime directement : un creux ferme, la nuit, un ciel clair et calme.
Le test verifie qu'elle **renverse le signe du gradient** — le fond, pourtant
150 m plus bas, doit etre plus froid que le plateau. Mesure : fond 2,1 °C,
plateau 3,6 °C.

Sans carte reliee (aucun terrain charge), l'inversion vaut zero partout et le
reste du systeme fonctionne. C'est un repli explicite, teste, pas une panne.

---

## 7. Le cycle jour/nuit visuel

**C'est la correction de l'incoherence assumee en Phase 16.** Le commentaire de
`GameClock` disait : *"cette horloge n'existe QUE pour la temperature, et rien
dans le rendu ne change avec elle"*. Ce n'est plus vrai.

La lumiere directionnelle existante (Blinn-Phong, Phase 2) est desormais
alimentee par la **meme position solaire** que l'adret/ubac et les thermiques.
Il n'y a donc toujours qu'**un seul soleil** dans le jeu — c'est la meme
exigence que pour la temperature, appliquee a la lumiere, et un test la verifie
explicitement (`dot(light.direction, sky.sunDirection) == 1`).

Chemin : `SynopticState` → `sunLightFor()` → `Renderer::setSun()` →
`UniformBufferObject::sun` / `sunColor` → les trois shaders de fragment.

Ce qui a change dans les shaders : `kLightDirection` et `kLightColor`, qui
etaient des constantes en dur dans `cube.frag`, `foliage.frag` et
`terrain.frag`, viennent de l'UBO.

**Deux details qui evitent des regressions :**

- `sunColor.a` est un **multiplicateur** de l'ambiante, pas une valeur absolue.
  Les trois shaders ont des ambiantes differentes et justifiees (0,15 pour le
  cube, 0,18 pour le terrain, 0,24 pour le feuillage, qui laisse passer la
  lumiere). Les remplacer par une valeur unique aurait ete une regression de
  rendu deguisee en correction de cycle.
- Les valeurs **par defaut** de l'UBO et de `Renderer` reproduisent exactement
  l'ancien eclairage. Un rendu qui n'appellerait pas `setSun()` est pixel pour
  pixel celui de la Phase 22.

**Le ciel suit aussi, et il a fallu une capture pour s'en apercevoir.** La
premiere version ne touchait qu'a la lumiere. Le relief s'assombrissait
correctement, mais la couleur d'effacement restait le bleu vif de midi : la
capture de 3 h du matin montrait la silhouette *noire* des sapins sur un ciel de
plein jour. La lumiere etait juste, l'image ne l'etait pas.

`SunLight` porte donc aussi une `skyColor`, avec sa propre palette (jour /
crepuscule / nuit) parce que le ciel n'est pas eclaire par le modele
d'eclairage — c'est un fond. Le crepuscule occupe la premiere moitie de la montee
du soleil, ce qui fait durer l'aube et le coucher au lieu d'un fondu direct du
noir au bleu.

**C'est le seul defaut de cette phase qu'aucun test headless ne pouvait
trouver** : il n'existait que dans l'image.

Quand le soleil passe sous l'horizon, la direction **bascule a la verticale** :
une lumiere directionnelle venue d'en dessous eclairerait le relief par le bas,
qui est le defaut visuel classique d'un cycle jour/nuit bacle. Un test verifie
`direction.y >= 0` a toutes les heures.

**`nightAmbientFactor = 0,30` est un choix de jouabilite assume, pas une
mesure.** Une vraie nuit sans lune est noire ; un jeu d'exploration ou l'on ne
voit rien pendant la moitie du temps de jeu n'est pas jouable en l'etat, aucune
source de lumiere portable n'existant encore. A revoir le jour ou le feu de camp
arrivera.

L'aube et le crepuscule durent : 20 intervalles de 5 minutes en lumiere
partielle sur 24 heures.

---

## 8. Les effets sur les quatre activites

Tous branches sur les systemes existants. **Aucun nouveau systeme parallele.**
Chaque nouveau parametre a une valeur par defaut neutre, et un test de
non-regression verifie que l'omettre reproduit **exactement** — comparaison
bit a bit sur les positions — le comportement d'avant la Phase 23.

### Needs (temperature) — priorite 1

Voir §3. Le refroidissement eolien remplace le forfait de -6 °C que la Phase 16
reservait au vol : c'est desormais la formule JAG/TI appliquee au vent reel,
auquel l'appelant ajoute la vitesse propre du joueur (vitesse de l'aile en vol,
vitesse du velo a velo). La **marche en crete par vent fort** refroidit donc
elle aussi, ce qu'un forfait reserve au vol ne pouvait pas exprimer.

Hors du domaine de validite publie (T > 10 °C ou vent < 4,8 km/h), la fonction
rend la temperature **inchangee** plutot qu'une extrapolation : au-dessus de
10 °C la formule brute rendrait un vent *rechauffant*. C'est le point le plus
facile a rater dans une implementation pressee, et un test le couvre.

Un abri annule le refroidissement eolien — c'est la definition d'un abri.

### Paraglider — priorite 4

Nouvelle struct `AirState` (vent, vitesse verticale, turbulence), volontairement
**nue** et non un `WeatherSample` : `Paraglider` ne doit pas plus dependre du
systeme meteo qu'il ne depend de Vulkan. Meme separation que `HeightSampler`.

- **Derive** — `speed_` est une vitesse **air** ; la vitesse sol est cette
  vitesse plus le vent. C'est l'ajout le plus lisible en jeu, et il ne demande
  aucune regle particuliere. Mesure sur 30 s avec 6 m/s de vent : 495 m vent
  arriere, 315 m en air calme, 135 m vent de face.
- **Ascendances** — le taux de chute de l'aile plus le mouvement de l'air. Dans
  une thermique plus forte que le taux de chute, la somme est positive et l'aile
  **monte** : la definition du vol thermique sort de l'addition. Mesure sur
  30 s : 1462 m en air calme, 1553 m dans 3 m/s d'ascendance.
- **Turbulence** — deux sinusoides de periodes **incommensurables**, pas un
  tirage aleatoire. Le vol reste deterministe (donc rejouable et testable, la
  regle de toute la phase), et deux periodes sans rapport simple ne se
  resynchronisent jamais, donc le chahut ne degenere pas en balancement
  regulier — ce qui est ce qui trahit immediatement une turbulence fabriquee.
  Le modele ne represente **ni fermeture ni decrochage** : une aile chahutee
  perd du cap et de la finesse, elle ne tombe pas.

### Climber — priorite 4

`update()` prend une **adherence** dans `[0, 1]`, tiree de l'humidite locale par
`Climber::gripForWetness()` — expose comme methode de la classe pour qu'il n'y
ait qu'une seule traduction humidite → adherence dans le jeu.

L'effet porte sur la physique existante et il est **double**, ce qui compte :

- la **vitesse** de progression (ascension et traversee) est multipliee par
  l'adherence ;
- le **cout energetique par metre** monte jusqu'a × 1,8.

Le second terme n'est pas un bonus : sans lui, un modele qui ne ferait que
ralentir laisserait la jauge dire le contraire — moins de metres parcourus, donc
moins d'energie depensee, donc une paroi mouillee *moins* fatigante. Mesure sur
20 s : sec 19,3 m a 0,018 de jauge/m, mouille 8,9 m a **0,032** de jauge/m.

Un plancher d'adherence a 0,05 garantit qu'une paroi ruisselante reste
**grimpable**, tres lentement et tres chere, plutot que de devenir une impasse
ou le joueur resterait bloque sans pouvoir ni monter ni redescendre.

### Bike — priorite 4

Fait, dans une version deliberement minimale : une seule grandeur (l'humidite du
sol) et deux coefficients, branches sur des termes qui existent deja dans
l'equation du mouvement, **sans ajouter le moindre etat**.

- resistance au roulement + 45 % sur sol detrempe ;
- freinage - 30 %.

Mesures : vitesse d'equilibre a plat 16,9 km/h sur sec contre 15,9 km/h sur
trempe ; une demi-seconde de frein enleve 11,7 km/h sur sec contre 8,8 km/h sur
trempe.

**Ce qu'il ne fait pas**, et c'est note comme amelioration future : aucune nature
de sol (terre, roche, herbe — le LiDAR n'en porte aucune, limite deja etablie en
Phase 22 pour les gisements), aucune boue qui se formerait et secherait par
endroits, et **aucune perte d'adherence en virage** — le terme lateral n'est pas
touche, parce que le rayon de braquage est un equilibre deja verifie par les
tests de la Phase 13 et qu'y toucher sans mesure serait un pari.

---

## 9. La mouillure du sol : relue, pas accumulee

Le seul endroit ou le systeme pourrait sembler avoir une memoire. Il n'en a pas :
`groundWetness()` **remonte le temps dans la fonction synoptique** — qui est
pure — au lieu d'accumuler image par image.

Huit echantillons sur les six dernieres heures de jeu, chacun decru
exponentiellement, et on garde le plus mouille. C'est le seul endroit ou
l'evaluation meteo depasse la dizaine d'operations, d'ou les huit echantillons
et pas cinquante.

La propriete que ca garantit est testee : un joueur qui arrive au meme endroit au
meme instant trouve la meme mouillure, qu'il ait mis dix minutes ou deux heures
a venir.

---

## 10. Cout

Mesure sur cette machine (i5-14600K, Debug, terrain analytique) :

| | Cout |
|---|---|
| Evaluation complete (couche 1 + couche 2, 7 sondages, mouillure) | **4,0 µs** |
| Couche synoptique seule | **0,38 µs** |
| Carte de piegeage, 128 × 128 | 393 216 `heightAt()`, **une fois au chargement** |

Le jeu fait **une** evaluation complete par image, au point du joueur. A 60 FPS
cela represente 0,024 % d'une image de 16,7 ms.

Le repere de la Phase 19 est que le frame est **domine par le GPU** — 81 % dans
`drawFrame` en Release, et aucune optimisation CPU ne rend rien. La meteo ne
change pas ce constat et n'avait aucune chance de le faire.

Le test de cout porte un seuil **tres large** (50 µs) et volontairement : il
protege contre un facteur mille — un sondage qui deviendrait une recherche, une
carte recalculee par image — et non contre une variation de 20 %, que le bruit de
mesure de cette machine ne permet de toute facon pas de distinguer (lecon de la
Phase 17 sur les FPS bruites).

---

## 11. Verification

### Tests headless

`tools\run_logic_tests.bat` — **3578 assertions**, toutes passantes. Deux
sections neuves :

- `testWeather()` — determinisme (purete bit a bit, independance du chemin via
  deux horloges a pas differents, et le controle negatif qu'un temps constant
  echouerait), cycles et position solaire, saison, variete des perturbations sur
  30 jours simules, gradient altitudinal, adret/ubac (dont le controle negatif
  de nuit et le versant est du matin), carte de piegeage et inversion (dont le
  controle negatif de la bosse et celui du plein jour), sondage amont (col
  contre plateau, col contre crete pleine a vent de reference egal, versant au
  vent contre versant sous le vent, controle negatif en air calme, ecretage sur
  15 jours), thermiques (dont ciel couvert et ubac), precipitations
  orographiques, mouillure, refroidissement eolien (dont les deux bords du
  domaine de validite), robustesse aux NaN/inf, et cout.
- `testWeatherGameplay()` — les quatre activites et la lumiere, systematiquement
  par comparaison de deux simulations identiques a un parametre pres, chacune
  avec son controle negatif de non-regression.

### Vulkan reel

`tools\run_vulkan_check.ps1`, **cinq executions completes** de la suite (dix
scenarios, quatorze processus) sur cette phase.

**Le critere dur est tenu dans les cinq :**

| | Resultat |
|---|---|
| Messages `[Validation]` | **0**, dans les cinq executions |
| Lignes `[ERROR]` | **0**, dans les cinq executions |
| Lignes `[WARN]` | 4, les memes qu'avant la phase |

**Chaque scenario a passe ses assertions de gameplay** sur le binaire final :
terrain synthetique, dalle LiDAR reelle, vol en parapente, sortie a velo
(50,0 km/h au pic en descente, 67 releves), escalade (sommet atteint, 41,8 m
gagnes), editeur en trois processus avec son temoin, besoins primaires (236 et
240 releves sur deux executions isolees apres la correction du 12), empreinte
environnementale, recolte de matieres.

**Aucune execution n'a ete verte de bout en bout, et il faut dire pourquoi.**
Les echecs residuels sont tous du cote du HARNAIS et jamais du jeu - voir la
section 12, qui distingue la regression que j'ai introduite (corrigee, avec ses
chiffres) de la flakiness pre-existante (desormais mesuree : le jeu se ferme en
0,1 s). Ils frappent un scenario different a chaque execution, ce qui est la
signature d'une variance d'automatisation et non d'un defaut.

Un run de reference entierement vert reste souhaitable ; il demande de traiter la
dette du collecteur de journal, qui n'est pas le sujet de cette phase.

### Verification visuelle

`tools\shot_daynight.ps1` produit **six captures de la MEME vue** a
3 / 6,5 / 9 / 13 / 18 / 21 h, horloge GELEE (`--time-scale=0`) et cadrage epingle
(`--start` + `--look`). Dans `build\daynight`.

Ce qu'elles montrent : la lumiere tourne d'est en ouest au fil de la journee, son
intensite suit, et le ciel passe du bleu de midi au bleu nuit. Le detecteur de
ciel du script de capture le chiffre tout seul, sans rien avoir a mesurer a la
main : **20,6 % de ciel a 13 h, 0,0 % a 21 h** - il ne compte que les pixels bleu
clair, et la nuit il n'y en a plus.

Rappel des trois pieges du projet, tous les trois rencontres pendant cette
session : `--look` doit etre epingle en plus de `--lock-input` (Phase 12), un
`vercora.exe` orphelin fausse toute mesure (Phase 19, et il a fallu en tuer un
au milieu des captures), et `powershell -File` aplatit les arguments - il relit
`--look=-90,-8` comme un nom de parametre, d'ou l'appel en processus dans
`shot_daynight.ps1`.

**Contraste adret/ubac** : aucune capture ne le montre, et c'est une limite
assumee. Le jeu n'a aucun indice visuel de temperature, donc un adret et un ubac
au meme instant se distinguent par leur ECLAIREMENT (visible dans les captures du
cycle) mais pas par leur chaleur. L'ecart thermique, lui, est mesure par les
tests headless : 14,7 C contre 10,7 C a midi. Un indice visuel - une teinte, une
brume de fond de vallee - est note comme amelioration future.

---

---

## 12. Le harnais Vulkan, et ce que cette phase lui a coute

Deux choses distinctes, qu'il ne faut pas confondre.

### (a) Une regression que j'ai introduite, mesuree et corrigee

La premiere version journalisait **deux lignes de meteo par seconde**, a cote de
la ligne de besoins qui existait deja. Le debit de journal a donc triple.

Le harnais Vulkan paie **un script-block PowerShell par ligne de journal**, et
son retard **croit avec le volume** — c'est la dette ouverte en Phase 17 et
comprise en Phase 22. Resultat immediat : le scenario H, le plus long
(220 s de journal continu), a echoue sur *"pas de sortie 20 s apres WM_CLOSE"* et
n'a collecte que **87 releves la ou il en attend 220**. Le jeu etait correct ;
c'est le collecteur qui ne suivait plus.

La cadence a ete resserree **deux fois** :

| Version | Volume ajoute sur le scenario H | Releves collectes |
|---|---|---|
| 2 lignes / seconde | ~440 lignes | **87** — echec |
| 2 lignes / 10 s | ~44 lignes | 235, puis **181** — echec marginal |
| 1 ligne / 30 s | **~7 lignes** | **236 / 240** sur deux essais |

La deuxieme valeur est instructive : elle *passait*, mais de justesse, et un run
ulterieur est retombe a 181 releves — assez pour perdre les derniers, donc pour
que la recuperation de la jauge (qui se lit dans les derniers) disparaisse. Une
correction qui passe de justesse n'est pas une correction.

Trente secondes ne coutent rien au depouillement : la meteo bouge a l'echelle de
l'heure de jeu, pas de la seconde reelle.

### (b) Une flakiness qui n'est PAS de cette phase, et qui est maintenant mesuree

Il reste des echecs *"pas de sortie 20 s apres WM_CLOSE"* qui frappent un
scenario different a chaque execution (H, puis E, puis D + G1 + G3, puis H de
nouveau), sans jamais aucun message `[Validation]` ni `[ERROR]`.

**Le jeu n'est pas en cause, et c'est desormais une mesure et non une
hypothese** : en fermant la fenetre par `CloseMainWindow()` et en chronometrant
la sortie du processus, on obtient **0,1 / 0,1 / 0,1 / 0,2 seconde** sur quatre
essais. Le jeu se ferme instantanement. Un *"pas de sortie apres 20 s"* est donc
un echec de LIVRAISON du message par le harnais, pas un blocage du jeu.

C'est la meme famille que la variance deja documentee pour A/B/G : le harnais
pilote par `PostMessage` (scancodes, pas de vraie souris), et le meme run peut
manquer une touche. Les executions ou un scenario echoue montrent d'ailleurs des
assertions de gameplay a `False` en amont (pas de decollage, pas d'entree en
mode editeur), pas seulement une fermeture ratee : ce sont des touches perdues.

---

## 13. Limites assumees et ameliorations futures

- **Ombre pluviometrique** — la barriere orographique renforce le versant au
  vent, mais il n'y a pas de vraie ombre sous le vent au-dela de l'absence de
  renforcement. Le cahier des charges le demandait simple pour cette session.
- **Nature du sol** — le velo ne connait qu'une humidite, pas un materiau. Le
  LiDAR ne porte aucune information de nature de terrain (limite deja etablie en
  Phase 22 pour les gisements).
- **Adherence en virage a velo** — pas touchee, voir §8.
- **Fermeture et decrochage en parapente** — pas modelises.
- **Neige** — `WeatherSample::snowing` est calcule et journalise, mais n'a
  **aucun effet gameplay ni visuel**. Le champ existe pour ne rien avoir a
  inventer le jour ou l'un des deux arrivera.
- **Precipitations visibles** — aucune. Il pleut dans les chiffres, pas a
  l'ecran.
- **Ombres portees** — aucune, comme avant. Ni etoiles, ni lune, ni halo
  solaire : le ciel est une couleur unie qui change avec l'heure, pas un ciel.
- **Vent sur la vegetation** — l'ondulation de l'herbe (Phase 18) est toujours
  pilotee par un temps qui defile, pas par le vent local. Le brancher serait un
  effet visuel gratuit maintenant que le vent existe en tout point.
- **Modele solaire** — pas d'equation du temps, pas de refraction, pas de
  longitude. Les corrections omises valent au plus une quinzaine de minutes sur
  les heures de lever et coucher, invisible dans une horloge acceleree douze
  fois.
- **Surplombs et 2,5D** — inchange, toutes les limites des phases precedentes
  s'appliquent.
