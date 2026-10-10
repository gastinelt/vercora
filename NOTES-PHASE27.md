# Phase 27 — Météo visible, micro-relief du sol, profil de performance

Règle de la phase : **aucune valeur ni mécanique météo inventée**. Tout effet
visuel ajouté est piloté par une grandeur que `WeatherSystem` calculait déjà,
ou par une donnée réelle du projet (orthophoto, heightmap LiDAR). Ce qui aurait
demandé une mécanique nouvelle est listé en fin de chantier 1 comme
**prérequis manquant**, et n'est pas implémenté.

Ordre de travail : chantier 1 (météo visible), chantier 3 (micro-relief),
chantier 2 (profilage), conformément à la priorité demandée.

---

## Outillage ajouté (préalable aux trois chantiers)

* **`--day=N`** (`src/main.cpp`) : jour de partie de départ. `--hour` seul ne
  choisissait que l'heure du **jour 0**, qui ne contient **aucune
  perturbation** : il était impossible de capturer la pluie. Le calendrier est
  déterministe (seed fixe), donc `--day` + `--hour` + `--time-scale=0` épinglent
  exactement un état météo.
* **`tools/shot_weather.ps1`** : captures épinglées (position, cadrage, jour,
  heure, horloge gelée, `--lock-input`) avec **journal du jeu à côté de chaque
  image** — nombre de `[Validation]` et ligne `Visuels meteo` relus pour chaque
  capture. Une capture dont on ne sait pas quel temps il faisait ne prouve rien.
* **`tools/profile_phase27.ps1`** : mesure A/B **entrelacée**, poste témoin,
  santé machine (voir chantier 2).
* **`--no-weather-visuals`**, **`--no-ground-detail`** : témoins **étroits**.
  Chacun coupe l'ajout de la phase et lui seul (la météo continue de peser sur
  les besoins et les activités ; le terrain garde son orthophoto).
* Bilan de fin de partie : poste `visuels meteo` (CPU) et ligne **`Cadence de
  boucle (PHASE 27)`** — images / secondes de boucle réelles, sans dépendre du
  titre de fenêtre (±15 %, leçon de la Phase 18).

### Instants de référence (calendrier déterministe, seed fixe)

Trouvés en balayant `synopticAt()` sur 30 jours puis sur un an, **pas** choisis
au hasard :

| instant | nébulosité | précipitation | usage |
|---|---|---|---|
| j12 13 h | 0,14 | 0 | grand beau |
| j0 13 h | 0,54 (0,61 local) | 0 | mi-couvert |
| j8 13 h | 0,95 (0,99 local) | 4,9 mm/h (9,2 local) | pluie franche |
| j8 22 h | 0,98 local | 8,6 mm/h local | pluie au crépuscule |
| j169 9 h 15 | 0,99 local | 9,2 mm/h local, **neige**, −3 °C | neige de jour |

L'écart synoptique/local vient du renforcement orographique et du gradient
altitudinal déjà modélisés (section 7 de `WeatherParams`) : la vue de l'alpage
est à ~1 480 m sur un versant au vent.

---

## CHANTIER 1 — Rendre la météo visible

### Constat avant

Un jour de pluie à 95 % de nébulosité se rendait **exactement** comme un jour
de grand beau : ciel bleu vif, soleil franc (captures `build/phase27/avant/`).
Au passage, un **commentaire mensonger** dans `sunLightFor()` affirmait depuis
la Phase 23 que l'ambiante était « écrasée par les nuages en plein jour » ; le
code n'a **jamais** lu la nébulosité. Corrigé.

### Architecture

Une seule fonction pure, **`weatherVisualsFor(sky, local, params)`**
(`world/Weather.hpp/.cpp`), est la traduction météo → image de tout le jeu.
Elle ne calcule **aucune** grandeur météo : elle lit l'état synoptique et
l'échantillon **local au joueur** que `main.cpp` calculait déjà une fois par
image, et part de `sunLightFor()`. Par ciel clair et sol sec, elle rend
`sunLightFor()` **au bit près** (testé).

### Traçabilité : quelle grandeur pilote quel effet

| effet visuel | grandeur Weather qui le pilote | coefficient d'affichage (PROVISOIRE, `WeatherParams` §6 ter) |
|---|---|---|
| soleil direct voilé | `WeatherSample::cloudCover` (local) | **aucun nouveau** : réutilise `cloudAspectDamping` (0,85), qui modélisait déjà « les nuages cachent le soleil » pour l'adret |
| ambiante diffuse renforcée, de jour seulement | `cloudCover` × part de jour (`daylight`) | `overcastAmbientGain` = 1,0 |
| ciel grisé (désaturé vers sa propre luminance, jour/nuit intact) | `cloudCover` | `overcastSkyDarkening` = 0,80 |
| pluie / neige à l'écran : densité et opacité | `precipitationMmPerHour` (local) / `precipitationMaxMmPerHour` | aucun (ratio de deux valeurs existantes) |
| neige plutôt que pluie | `snowing` (donc `temperatureC` local vs `snowThresholdC`) | — |
| inclinaison des traits | `windVector` (vent **local** : sondage amont, cols, abris) | `rainFallSpeedMps` 6,5 / `snowFallSpeedMps` 1,2 (vitesses terminales usuelles) |
| brouillard de distance | `precipitationMmPerHour` (local), `snowing` | `rainExtinctionPerKm` 0,25 × R^`0,63` (1/km), × `snowExtinctionMultiplier` 8 pour la neige |
| sol assombri | `wetness` (relecture de la pluie des 6 dernières heures, Phase 23) | `wetGroundDarkening` = 0,35 |
| couleur du brouillard | = couleur du ciel ci-dessus | — |

**Aucun effet n'existe sans sa grandeur.** C'est vérifié par
`testWeatherVisuals()` : ciel clair et sec ⇒ lumière de la Phase 23 au bit
près, brouillard nul, pluie nulle, sol non assombri ; et, sur **30 jours de jeu
à la minute**, pluie et brouillard à l'écran **exactement** quand la météo
calcule une précipitation, jamais sinon (0 écart sur 43 200 minutes).

### Cohérence temporelle (point 4 du cahier des charges)

Mesurée, pas supposée : sur 30 jours à la minute, le saut maximal d'une minute
à l'autre vaut 0,022 pour la couleur du ciel, 0,004 pour l'intensité de pluie,
6,5·10⁻⁵ /m pour le brouillard (montée d'une averse de neige, lisse). Les effets
suivent le calendrier synoptique comme le cycle jour/nuit, sans aléa propre.

**Une discontinuité, trouvée par le test et laissée telle quelle** : la bascule
**pluie ↔ neige** (j1 6 h 55 à 1 500 m : visibilité de 1,6 km à 13 km en une
minute). Elle vient du **modèle** — `snowing` est un booléen au seuil
`snowThresholdC` — et non du rendu. La lisser demanderait une phase mixte que
Weather ne modélise pas (voir prérequis). Le test isole ces minutes (2 bascules
sur 30 jours) et fait échouer **tout autre** saut.

### Rendu

* `UniformBufferObject` : quatre `vec4` en fin de bloc (`fog`, `precip`, `fall`,
  `detail`), **neutres par défaut** — sans appel, l'image est celle de la
  Phase 26.
* Brouillard : extinction de Beer-Lambert `exp(-σ d)`, **même fonction** dans
  `terrain.frag`, `foliage.frag`, `cube.frag` (sinon les arbres ressortiraient
  nets du voile). σ = 0 ⇒ couleur rendue inchangée.
* Pluie/neige : `precip.vert/.frag`, **un triangle plein écran** sans buffer de
  sommets, trois couches de traits procéduraux, mélange alpha, sans profondeur.
  La passe est **sautée entièrement** (aucun bind, aucun draw) quand
  l'intensité vaut 0. Les traits sont collés à l'écran : ils ne passent pas
  derrière un tronc — compromis assumé « effet d'écran simple ».
* Sol mouillé : `albedo *= 1 − precip.w` dans `terrain.frag`.

### Un effet mort trouvé par le journal des captures

Première série « après » : le journal affichait **`ambiante x1.00` sous 99 % de
nuages**. Le multiplicateur d'ambiante était écrêté à [0, 1] deux fois (dans
`weatherVisualsFor` et dans `Renderer::setSun`) ; or il vaut déjà 1 en plein
jour : le gain de ciel couvert ne s'appliquait **jamais**. Le test « monotone »
ne pouvait pas le voir (une constante ne décroît pas). Corrigé (borne à 4), et
un test exige désormais que l'effet **existe** (> ×1,5 sous ciel couvert).

### Vérification

* Tests headless : **3631 / 3631** (3607 + 24 nouveaux, `testWeatherVisuals`).
* Captures `build/phase27/planche_meteo.png` (témoin `--no-weather-visuals` à
  gauche, Phase 27 à droite), 5 instants, **0 `[Validation]`** sur les 20
  lancements Debug (avant, après, témoin, v1).
* Neutralité du témoin : témoin vs capture d'avant = 0,4 à 2,9 % de pixels
  à plus de 8 niveaux (herbe animée) — le drapeau rend bien la Phase 26.
* Effet proportionné à la donnée : grand beau (nébulosité 0,14) = 3,5 % de
  pixels changés ; mi-couvert 88 % ; pluie 94 %.
* Fausse alerte écartée par la mesure : le ciel « gris clair » de j8 22 h n'est
  pas un bug — à 22 h fin mai c'est encore le crépuscule ; ciel témoin
  (48, 56, 84), ciel Phase 27 (51, 51, 52) : même luminance, désaturée.

### Prérequis manquants — NON implémentés, faute de grandeur Weather

| effet envisagé | ce qui manque dans Weather |
|---|---|
| brume par temps sec, brume de vallée dans les cuvettes (la carte de piégeage d'air froid existe) | humidité relative / point de rosée : sans eux, rien ne dit **quand** l'air froid piégé condense |
| nuages accrochés au relief, plafond bas | altitude de la **base des nuages** : la nébulosité est une fraction de ciel, pas une hauteur |
| transition douce pluie ↔ neige (neige fondue) | une **phase mixte** de précipitation ; `snowing` est un booléen |
| sol **blanchi** par la neige | un **manteau neigeux** (accumulation / fonte) ; aujourd'hui une chute de neige mouille le sol (`wetness`) et le fonce, ce qui est ce que dit le modèle |
| nuages visibles, soleil voilé localement, éclaircies qui défilent | une nébulosité **spatialisée** (champ 2D advecté par le vent) ; la nébulosité est 0D + un renforcement orographique au point |
| éclairs / orage | aucune convection profonde modélisée |

---

## CHANTIER 3 — Micro-relief du sol vu de près

### Approche retenue

Texture de détail **procédurale** perturbant la **normale** (le grain réagit au
soleil de Weather) et, légèrement, l'albédo — pas de micro-déplacement
géométrique (il aurait fallu densifier les tuiles CDLOD proches : bien plus
cher et plus risqué pour le LOD).

* **Bande de fréquences** : de 0,61 m (= le texel de l'orthophoto, lu sur la
  texture réellement chargée, `TextureData::metresPerTexel()`) jusqu'à ~5 mm,
  en 7 octaves de rapport 2,2. Rien au-dessus du texel : le grain ne contredit
  jamais la photo.
* **Amplitude pilotée par des données réelles** : contraste local de
  l'orthophoto (photo nette contre sa version 2 mipmaps plus floue — éboulis
  contrasté contre prairie lisse) et pente du relief LiDAR (1 − N.y). Plancher
  50 %. **Le motif lui-même est un bruit** et ne représente aucun caillou réel ;
  c'est dit dans le shader.
* **La normale perturbée ne sert qu'à l'éclairage** : l'indice adret/ubac garde
  la normale du relief (il doit dire la même chose que la météo).
* **Deux fondus** : par distance (5 → 20 m) et **par octave selon l'empreinte
  du pixel** (`fwidth`, Nyquist : plein poids ≥ 4 px, éteint à 2 px) — c'est ce
  second fondu qui interdit le moiré en vue rasante. Les mipmaps de la texture
  filtrent en plus le sous-pixel. Lectures en `textureGrad` avec dérivées
  prises hors branche (boucle à sortie non uniforme).

### Paramètres finaux

| paramètre | valeur | où |
|---|---|---|
| force | 1,0 | `kGroundDetailStrength` (`main.cpp`) |
| fondu distance | 5 → 20 m | `kGroundDetailFadeStart/End` |
| motif de base | texel de l'ortho (0,610 m), 0,61 par défaut | `groundDetailPattern` |
| octaves / lacunarité | 7 / 2,2 | `terrain.frag` |
| pente max du grain | 1,3 × amplitude | `kDetailSlope` |
| couleur | ±30 % en crête, ±10 % typique | `kDetailAlbedo` = 0,30 |
| rugosité pleine | contraste photo 0,12 ou pente 0,35 | `kDetailFullContrast/Slope` |
| texture de bruit | 256², 32 cellules, RGBA8 + 9 mips (341 Ko) | `assets/DetailNoise.cpp`, binding 2, adressage REPEAT |

### Vérification

`build/phase27/planche_sol.png` (témoin `--no-ground-detail` à gauche).
Énergie haute fréquence (laplacien) mesurée sur les captures :

| vue | sol proche (moitié basse) | loin (tiers haut) |
|---|---|---|
| au ras du sol | 5,92 → **7,24 (+22 %)** | — |
| vue de marche | — | 3,01 → **3,01 (inchangé)** |

Le sol est visiblement grenu sous les pieds, et **rien** n'est ajouté au loin
(pas de moiré mesurable sur image fixe). **Limite** : des images fixes ne
prouvent pas l'absence de scintillement **en mouvement** ; le fondu de Nyquist
le prévient par construction, non vérifié en déplacement.

### Une première version rejetée PAR LA MESURE

La v1 calculait le bruit dans le shader. Première mesure de l'effet visuel :
énergie HF proche **inchangée** (5,92 → 5,91) — 4 octaves s'arrêtaient à 6 cm,
soit 15 pixels à 2 m : de l'ondulation, pas du grain ; et l'amplitude était
divisée deux fois (normalisation + moyenne des gradients). Corrigé à 7 octaves.
Puis son coût s'est révélé inacceptable → bruit en texture (chantier 2).

---

## CHANTIER 2 — Profilage et optimisation

### Protocole (`tools/profile_phase27.ps1`)

* Paires A/B **entrelacées** (AB, BA, AB…), B = témoin **étroit** de l'ajout
  mesuré (`--no-weather-visuals`, `--no-ground-detail`, ou les deux = Phase 26).
* Métrique : `Cadence de boucle` (µs par image réelles) et les postes
  `Cout par image (PHASE 19)`.
* **Santé machine par run** : charge totale échantillonnée pendant le run moins
  le CPU de vercora. Plus d'un cœur entier ⇒ **NON CONCLUANT**, exclu des
  médianes ; moins de 20 img/s ⇒ NON CONCLUANT (« n'a pas pu rendre »).
* Machine : i5-14600K, UHD 770 intégré, 1280×720.

**Deux défauts de l'outil trouvés en l'écrivant** : la 1re version du témoin de
santé sommait `TotalProcessorTime` sur une liste de processus qui change pendant
le run (« 318 910 % d'un cœur ») — remplacée par un échantillonnage de charge ;
et le bureau au repos (application Claude, webviews, Discord) consomme 25 à
130 % d'un cœur, d'où un seuil à 1 cœur, justifié dans le script. Beaucoup de
runs ont été écartés à ce titre : les tableaux ci-dessous n'utilisent que les
runs sains, et le signalent quand il n'en restait pas.

### Leçon de méthode : un poste CPU n'est pas un témoin sur GPU intégré

Sous la pluie, l'herbe (CPU) a été **systématiquement** plus lente en A qu'en B
(495 contre 374 µs, +32 %), tour après tour, **dans les deux ordres** — donc pas
une dérive de la machine. Par beau temps, où A et B chargent le GPU pareil :
écart 4 %. Sur un GPU **intégré**, CPU et GPU partagent le budget de puissance
du boîtier ; charger le GPU ralentit le CPU. Le témoin CPU est donc **couplé** à
toute modification GPU et ne peut pas la valider. Il reste valide quand la
condition ne change pas la charge GPU (beau temps : 4 % ; vue de marche finale :
2 %).

### Profil actuel (tout le contenu depuis la Phase 19), médianes

| config / vue | total | drawFrame | herbe | arbres | fibres | streaming |
|---|---:|---:|---:|---:|---:|---:|
| Release, vue fixe alpage, beau | 4 962 µs | 4 228 (**85 %**) | 408 | 253 | 17 | 14 |
| Release, survol 900 m/s (Phase 26) | 1 495 µs | 1 126 (75 %) | 1 | 241 | 10 | 82 |
| Debug, vue fixe alpage, beau | 7 838 µs | 2 099 (27 %) | **3 482 (44 %)** | 1 775 | 191 | 95 |
| Debug, survol 900 m/s (Phase 26) | 4 429 µs | 1 236 (28 %) | 9 | 1 987 | 124 | 939 |

* **Release est limité par le GPU** (75–85 % dans `drawFrame`) : optimiser les
  postes CPU (herbe 0,4 ms, arbres 0,25 ms) ne rendrait **rien**, conformément
  à la Phase 19. **Aucune optimisation CPU n'a donc été faite.**
* **Debug est limité par le CPU** (herbe 44 %) : le même changement GPU y est
  invisible — la pluie y coûte −83 µs (bruit). Mesurer en Debug seul aurait
  conclu « gratuit ». La leçon de la Phase 19 se vérifie encore.
* Les chiffres de survol ne sont pas comparables tels quels à ceux de l'audit
  (378 img/s) : l'audit ne précisait ni jour, ni heure, ni drapeaux ; ici le
  survol est épinglé à j8/j12 13 h.

### Coût des ajouts de la Phase 27 (Release, A − B)

| mesure | coût | statut |
|---|---:|---|
| beau temps, vue fixe (contrôle négatif météo) | −28 µs (0) | gratuit par temps sec |
| survol beau temps (shaders modifiés, rien d'actif) | +54 µs (+3,6 %) | surcoût fixe des shaders |
| **pluie, vue fixe** (passe plein écran + brouillard + sol mouillé) | **+608 µs (+12 %)** | **accepté** : seulement quand il pleut, ~180 img/s |
| survol sous la pluie | +684 µs | idem (≈ passe de pluie) |
| micro-relief v1, au ras du sol | **+1 683 µs (×1,94)** | **rejeté** |
| v1 + Nyquist resserré à 8 px | +1 490 µs | insuffisant |
| v1 sans aucune octave (bissection) | +330 µs | ⇒ ~200 µs **par octave** |
| v1 à 2 octaves (bissection) | +720 µs | confirme : ALU, pas un surcoût fixe |
| v1 avec hash flottant au lieu d'entier | ≈ inchangé | hypothèse réfutée |
| **v3 : bruit en texture**, au ras du sol | **≈ +900 µs** | −47 % |
| v3 sans la lecture floue d'ortho (bissection) | ≈ inchangé | lecture gratuite, conservée |
| v3, **vue de marche**, fondu 35 m | +623 µs (+15 %) | |
| **v3, vue de marche, fondu 20 m (final)** | **+480 µs (+11 %)** | **retenu**, témoin stable (2 %) |

(Les valeurs « ≈ » viennent de runs dont les A ont tous dépassé de peu le seuil
de santé, sans médiane formelle ; leur dispersion était de ±0,2 %.)

Seule optimisation livrée : **le micro-relief**, le seul poste nouveau mesuré
comme significatif — de +1 683 à ~+900 µs dans le pire cas, et +480 µs en vue de
marche. Trois hypothèses ont été **réfutées par la mesure** avant de trouver la
bonne (octaves fines, hash entier, lecture floue) ; aucune n'a été gardée « au
cas où ».

**Non fait, et pourquoi** : la passe de pluie (+608 µs) n'est active que
pendant les précipitations (~9 % du temps sur 30 jours de jeu) et le jeu y
reste vers 180 img/s ; l'alléger (moins de couches, demi-résolution) dégraderait
la lisibilité, qui est l'objectif du chantier 1. À reprendre si une cible plus
faible que l'UHD 770 est visée.

---

## Vérification globale

* Tests headless : **3636 / 3636** (3607 + 29 : `testWeatherVisuals`,
  `testDetailNoise`).
* 0 `[Validation]` sur toutes les captures Debug de la phase et sur les runs
  Debug de profilage.
* Suite `run_vulkan_check.ps1` (build Debug final, couches de validation) :
  **verte au premier passage** — scénarios A à J3 (14 processus : gameplay,
  collision, dalle LiDAR, parapente, vélo, escalade « sommet atteint : True »,
  éditeur G1-G3, besoins, faune, récolte et fibres), **Messages [Validation] :
  0**, fermeture propre. Un seul passage : la flakiness historique du harnais
  (D2 de l'audit) n'a donc pas été re-mesurée ici.

## Où je me suis arrêté

Les trois chantiers sont livrés. Restent ouverts, sciemment :
* scintillement du micro-relief **en mouvement** : non vérifié (images fixes
  seulement) ;
* passe de pluie à +608 µs : non optimisée (voir chantier 2) ;
* effets météo qui demanderaient une mécanique absente de Weather : listés
  dans « Prérequis manquants », non implémentés, par règle.
