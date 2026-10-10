# Phase 25 — Indice visuel adret / ubac

## Le manque

Depuis la Phase 23, l'écart adret/ubac est **calculé, mesuré et journalisé** —
14,7 °C contre 10,7 °C à midi sur la zone de référence — et **strictement
invisible à l'écran**. La Phase 23 l'avait notée elle-même comme sa limite
assumée : « pas de capture adret/ubac, et c'est une limite assumée : le jeu n'a
aucun indice VISUEL de température ».

## La piste retenue : teinte du terrain (piste 1)

Retenue sans hésitation, et le rapport impact / effort n'est pas la seule
raison : c'est la seule des trois pistes qui **réutilise la grandeur existante
telle quelle**.

`WeatherSystem::sampleAt` définit l'adret/ubac par l'écart entre le cosinus
d'incidence du point et celui qu'aurait du terrain **plat** au même instant. Or
`shaders/terrain.frag` dispose déjà des deux moitiés de ce calcul : la normale
du relief par fragment, et `ubo.sun.xyz` — la direction solaire **de Weather**,
celle que la Phase 23 a fait descendre jusqu'au rendu précisément pour qu'il n'y
ait qu'un seul soleil dans le jeu.

Il n'y avait donc **rien à recalculer**, seulement à consommer.

### Le partage des rôles, et pourquoi il est là

| | qui | quoi |
|---|---|---|
| hauteur du soleil, nébulosité | **CPU** (`main.cpp`) | appartiennent à la météo, passés dans `ubo.terrain.w` |
| cosinus d'incidence par fragment | **shader** | seule part qui a besoin de la normale |

Les deux règles portées par le CPU le sont délibérément : ce sont exactement
celles qu'un shader re-dériverait « à peu près », et qui divergeraient ensuite
de la température sans que rien ne le signale. C'est la contrainte « pas de
seconde source de calcul » prise au sérieux — la même exigence qui, en Phase 23,
avait fait sortir la température de `Needs`.

Le champ `terrain.w` était marqué « réservé » depuis la Phase 11. Il l'est resté
jusqu'ici : **aucun octet d'uniforme ajouté, aucune allocation.**

### Conséquence gratuite : la cohérence avec le cycle jour/nuit

Aucune règle horaire n'est écrite nulle part. Le soleil tourne, l'incidence de
chaque versant change avec lui ; il se couche, `terrain.w` tombe à zéro.
La condition « le soleil doit être levé » est celle de la Phase 23 — celle dont
l'absence rendait un versant sud plus chaud à 2 h du matin.

## Les paramètres visuels

| paramètre | valeur | où |
|---|---|---|
| teinte adret | `(1.14, 1.04, 0.88)` | `shaders/terrain.frag` |
| teinte ubac | `(0.86, 0.96, 1.12)` | idem |
| écart de référence | `0.35` | = `WeatherParams::aspectIncidenceReference` |
| intensité maîtresse | `1.0` | `kAspectTintIntensity`, `src/main.cpp` |

Deux propriétés voulues :

- **Symétrie exacte autour de 1**, canal par canal. Sur du terrain ni adret ni
  ubac le signal vaut 0, donc le mélange rend le *milieu* des deux teintes : un
  milieu qui ne serait pas `(1,1,1)` teinterait tout le plat du massif. Un
  premier jeu de valeurs le faisait, d'un demi pour cent vers le chaud.
- **Modulation, jamais remplacement** : l'orthophoto porte tout le détail, elle
  est seulement réchauffée ou refroidie.

### L'amplitude a été corrigée après mesure, pas devinée

Premier essai à ±7 % : à **13 h — l'heure où l'écart thermique est maximal —
93,5 % des pixels ne bougeaient pas d'un cran sur 255.** Un indice invisible
manque sa seule raison d'être. Amplitude portée à ±14 % (rouge) / ±12 % (bleu).

**Le signal, lui, n'a pas changé** : c'est toujours celui de la météo. Seule
l'amplitude d'affichage a été revue, et c'est un choix de rendu, légitimement
distinct du modèle physique.

## Vérification

### La vue : un belvédère, pas un point

Première tentative : un point choisi pour ses pentes. Résultat — une image
entièrement remplie par le versant sous les pieds, 0 % de ciel, aucun relief
lisible. **Savoir où sont les versants ne dit pas d'où on les voit.**

La recherche a donc été refaite avec un vrai calcul de **visibilité** (balayage
d'horizon le long de chaque rayon : un point n'est vu que si son élévation
dépasse celle de tout ce qui est plus proche). Belvédère retenu :

    E 900000 / N 6436000, altitude 1950 m, --look=270,-24
    108 points d'adret franc et 107 d'ubac franc VISIBLES, ciel sur 15 rayons/15

### Avant / après, mesuré

Témoin : **même binaire, même vue, `kAspectTintIntensity` à 0**. La mesure porte
sur la variation de « chaleur » `(R−B)` pixel à pixel.

| heure | réchauffés | refroidis | inchangés |
|---|---|---|---|
| 09 h | 3,2 % | **52,2 %** | 44,6 % |
| **13 h** | **20,4 %** | **29,0 %** | 50,6 % |
| 18 h | **45,8 %** | 6,0 % | 48,2 % |
| 22 h | 2,0 % | 0,0 % | 98,0 % |

Ce qu'on cherchait n'était pas « les images diffèrent » — un filtre global
donnerait ça aussi et ne prouverait rien. C'est que l'écart soit **bipolaire
dans la même image** : à 13 h, 20 % des pixels se réchauffent *pendant que*
29 % se refroidissent. C'est la signature d'un indice qui distingue deux
versants.

La rotation est cohérente avec le soleil : à 9 h, en regardant au nord, les
faces ouest sont à l'ombre — refroidies ; à 18 h les mêmes sont au soleil —
réchauffées.

### Plancher de bruit : la mesure qui rend les chiffres lisibles

Deux captures du **même** binaire, pour savoir ce que vaut une différence :

| | 13 h | 22 h |
|---|---|---|
| bruit (même binaire) | 0,6 % | 1,2 % |
| effet (avant/après) | **49,4 %** | 2,0 % |

À midi, l'effet est **80 fois le bruit**. À 22 h il est **dans** le bruit :
l'indice est donc réellement éteint la nuit, et le résidu de 2 % est l'herbe
animée — vérifié, pas supposé. Le journal du jeu le confirme de son côté :
`versant -2.1` à 13 h, `versant 0.0` à 22 h.

### Coût

| | témoin | avec l'indice |
|---|---|---|
| `drawFrame` | 672 µs | **665 µs** |
| orthophoto GPU | 48 Mo | 48 Mo |
| géométrie terrain GPU | 30 Mo | 30 Mo |

L'écart sur `drawFrame` est du bruit, et dans le sens d'un gain : autrement dit,
non mesurable. Attendu — quelques opérations ALU par fragment, aucune texture
supplémentaire, aucune allocation.

### Validation Vulkan

**Zéro message `[Validation]`** sur les deux séries de captures (8 lancements du
jeu, couches de validation actives).

## La divergence possible, rendue bruyante

`kAspectIncidenceReference` (0,35) est écrit **en dur dans le shader** : un
shader ne peut pas lire un paramètre C++. Si quelqu'un règle
`WeatherParams::aspectIncidenceReference` sans toucher au shader, l'indice
visuel cesserait d'être d'accord avec la température — exactement la double
source de vérité que la Phase 23 avait pris soin d'éviter.

On ne peut pas empêcher la divergence ; on peut refuser qu'elle soit
silencieuse. `main.cpp` compare les deux au démarrage et émet un `[WARN]`
explicite si elles ont cessé de s'accorder.

## Les pistes NON retenues

**Piste 2 — végétation différenciée par versant.** Écartée pour cette session,
et pas par manque de temps : l'équilibre de densité d'herbe est calibré depuis
la Phase 18 (124 touffes sur la vue de référence, un repère qui sert encore), et
la végétation n'a **aucune distinction d'espèces** — pas de résineux contre
feuillus, un seul type d'arbre. « Résineux en ubac, feuillus en adret » n'est
donc pas un réglage mais un **prérequis manquant**. À traiter avec le chantier 4
(placement des arbres), où la distinction d'espèces devrait de toute façon
apparaître.

**Piste 3 — température affichée dans l'interface.** Non nécessaire : elle était
le filet de sécurité si la piste 1 s'avérait risquée, et la piste 1 a tenu. À
noter que la température ambiante **est déjà** dans le titre de la fenêtre
(`chaud 99%`) et dans le journal des besoins.

## Limites assumées

- **L'indice n'est pas quantitatif.** Il dit « ce versant reçoit plus de soleil
  que celui-là », pas « il fait 4 °C de plus ». Un joueur ne peut pas en lire une
  température.
- **Le terrain seulement.** L'herbe et les arbres (shader `foliage`) ne sont pas
  teintés : un versant densément boisé montrera donc l'indice sur le sol entre
  les arbres, moins sur le couvert.
- **Pas d'ombres portées** (limite héritée de la Phase 23) : un versant à
  l'ombre d'une crête est traité comme s'il voyait le soleil. L'indice suit
  l'orientation de la surface, pas l'occultation par le relief.
- **L'amplitude est un réglage d'auteur**, pas une grandeur physique. Le seul
  bouton est `kAspectTintIntensity` dans `src/main.cpp`, nommé pour être trouvé
  sans lire le shader.
- **L'effet sature** au-delà de 0,35 d'écart d'incidence, comme la température :
  deux versants très raides d'orientations opposées ne se distinguent plus
  au-delà de ce seuil.
