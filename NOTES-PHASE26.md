# Phase 26 — Flakiness du scénario I, et espèces d'arbres par exposition

## TÂCHE 1 — La flakiness résiduelle du scénario I : REPRODUITE ET EXPLIQUÉE

### Ce que la Phase 25 laissait ouvert

Trois suites complètes, résultat vert / échec / vert. Le run en échec avait perdu
78 % de la télémétrie du scénario I. La cause n'avait pas pu être retrouvée : le
dump de diagnostic portait un nom fixe et avait été écrasé. La Phase 25 a
horodaté le dump pour que la prochaine occurrence soit traçable.

### Ce qui manquait ENCORE au dump, et qu'il a fallu ajouter

Le dump horodaté aurait survécu — mais il n'aurait toujours rien appris. Il
concaténait les **quatorze processus bout à bout, sans le moindre séparateur** :
aucune ligne du journal n'était rattachable à un scénario. On ne pouvait même pas
compter combien de lignes appartenaient au scénario I.

Ajouté ici, dans `Stop-Game` : un **en-tête par processus** portant le libellé,
l'heure de démarrage, la **durée mur**, le code de sortie et le nombre de lignes.
L'accumulation se fait dans `Stop-Game` lui-même et non en énumérant les `$runX`
à la fin — le scénario J recompose `$runJ` en concaténant ses trois processus, si
bien qu'une énumération des variables compterait J2 et J3 deux fois.

`$dumpLines` est construit **à part de `$all`** : aucune assertion du harnais ne
voit ces en-têtes, donc l'instrumentation ne peut pas changer un verdict.

### Le discriminateur, et pourquoi il tranche seul

La ligne `Faune :` et la ligne `Camera :` sont émises dans le **même bloc « une
fois par seconde »** de `src/main.cpp`. Mais `Camera :` est émise **sans aucune
condition** — ni mode éditeur, ni faune vivante, ni activité en cours. Comparer
les deux comptes sépare donc deux causes qu'aucune ne distingue seule :

* les deux comptes chutent ensemble → c'est **le tic lui-même** qui n'a pas
  battu : le jeu n'a pas rendu d'images ;
* `Camera :` intact et `Faune :` effondré → c'est la **condition** de la ligne de
  faune qui a sauté.

### La mesure

Échec reproduit **dès la suite 2**, et bien pire que celui de la Phase 25 :

    [diag I] duree mur 276,3 s, tics 'Camera :' 3, releves faune 3

**Les deux s'effondrent ensemble.** Ce n'est donc ni un timeout, ni un tampon, ni
la condition de la ligne de faune, ni quoi que ce soit qui appartienne au
scénario I. Le jeu était vivant, sa fenêtre existait, son initialisation était
complète (~70 lignes) — et il ne rendait pas d'images.

### Ce que le tableau par processus a montré, et qui referme la question

| fenêtre d'horloge | ce qui tournait | état |
|---|---|---|
| jusqu'à **22:09:53** | suite 1 entière + suite 2 A/B/C | normal |
| **22:09:53 → 22:35:15** | suite 2 D→J3, **puis** suite 3 A→F | dégradé, codes de sortie −1 |
| à partir de **22:35:15** | suite 3 G1→J3 | normal, et identique à la suite 1 |

La dégradation est une **fenêtre continue d'horloge murale qui traverse la
frontière entre deux suites**. Elle frappe tous les processus lancés pendant
qu'elle dure, sans égard pour le scénario ; elle s'ouvre au milieu d'une suite et
se referme au milieu de la suivante.

Aucune propriété du scénario I ne peut produire cela — ni ses 240 s, ni sa
position dans la suite, ni un ordre d'exécution. Les deux pistes proposées au
départ sont donc écartées **par la mesure** : ce n'est pas la durée, et ce n'est
pas la place dans la suite.

Comparaison du même scénario, même binaire, à deux moments :

    scenario I, run sain    : 256,6 s de duree mur -> 254 tics  (99 %)
    scenario I, run degrade : 276,3 s de duree mur ->   3 tics  ( 1 %)

Et la suite 3, une fois la fenêtre refermée, redonne **1349 lignes** au scénario I
là où la suite 1 en donnait 1347. Le jeu n'a jamais bougé.

### La cause matérielle, identifiée

Un processus `find.exe` de Git Bash, **orphelin** (son parent n'existe plus),
lancé à **18:04:59** — soit près de quatre heures avant le début de cette
session — balaie le disque entier :

    "C:\Program Files\Git\usr\bin\find.exe" / -maxdepth 9 -type d -ipath *config/totochetools/horizons*

Au moment du constat il avait accumulé **20 334 secondes de CPU** (5,6 heures).
Sur cette machine le GPU est un **Intel UHD 770 intégré** (confirmé : le même GPU
est sélectionné dans les 14 processus des 3 suites, il n'y a pas de bascule de
GPU), qui partage sa bande passante mémoire avec le CPU. Un balayage de système
de fichiers saturant un cœur en continu est exactement ce qui affame un tel GPU.

S'y ajoutent, sur la même machine et pendant la même période, une **compilation
C++ complète** et un **second harnais Vulkan** lancés par une autre session.

### Le correctif : un TÉMOIN DE SANTÉ, pas un délai élargi

Le harnais ne savait pas distinguer deux choses qui n'ont rien à voir :

* « le jeu a régressé »            → il faut chercher dans le jeu ;
* « la machine n'a pas pu rendre » → il n'y a **rien** à chercher dans le jeu.

Faute de les séparer, il déclarait ROUGE dans les deux cas. Ce projet a perdu
cinq phases sur exactement cette confusion (dette du vélo, Phases 17 à 22).

La mesure qui les sépare **existait déjà** : le nombre de lignes `Camera :`
rapporté à la durée mur du processus. Le harnais le calcule désormais pour chaque
run et, si un seul run tombe sous **50 %** du débit attendu, il conclut :

    [NON CONCLUANT] ... exit 2

**Ce témoin ne rend rien vert.** Un run dégradé n'est pas déclaré réussi, il est
déclaré non concluant, avec un code de sortie qui lui est propre pour qu'aucune
automatisation ne le confonde avec un succès. On refuse de conclure sur une
mesure qui n'a pas eu lieu ; on ne prononce pas l'acquittement.

Le seuil de 50 % n'arbitre rien : les deux populations mesurées sont à **99 %** et
à **1 %**. N'importe quelle valeur entre les deux donnerait le même verdict.

### La vérification : 3 suites consécutives, machine au repos

Le `find` orphelin disparu et la charge globale retombée à 0 %, trois suites
complètes ont été enchaînées :

| suite | verdict | durée | scénario I |
|---|---|---|---|
| 1 | `[OK]` exit **0** | 14,2 min | 256,5 s → **254 tics / 254 relevés** |
| 2 | `[OK]` exit **0** | 14,2 min | 256,6 s → **254 tics / 254 relevés** |
| 3 | `[OK]` exit **0** | 14,2 min | 256,4 s → **254 tics / 254 relevés** |

Zéro message `[Validation]`, zéro ligne `[ERROR]`, et le témoin de santé conclut
« tous les processus ont rendu des images à cadence normale » dans les trois cas.

**Ce qui frappe, et qui est la vraie preuve** : la reproductibilité. **Onze des
quatorze processus rendent un nombre de lignes RIGOUREUSEMENT identique** d'une
suite à l'autre — scénario I à 1605 lignes les trois fois, scénario H à 1013.
Seuls le scénario A (430 / 442 / 436) et l'éditeur G1 (894 / 887 / 911) varient
de quelques lignes. Un harnais dont onze processus sur quatorze sont au relevé
près identiques n'est plus un harnais flaky.

Le critère « zéro faux-négatif sur plusieurs runs consécutifs », ouvert depuis la
Phase 25, est donc **atteint** — et il l'est sans qu'une seule ligne du jeu ait
été modifiée pour la fiabilité du harnais. Ce qui manquait était l'instrument, et
la machine.

À noter : les espèces de la Tâche 2 sont exercées par la suite complète (mesh
feuillu téléversé, « 10 résineux sur 16 arbres abattables » journalisé) — les
trois suites vertes valent donc aussi non-régression pour le chantier 4.

---

## TÂCHE 2 — Espèces d'arbres et placement par exposition

### L'architecture : une seule loi, trois consommateurs

`world/TreeSpecies.hpp` est le **seul** endroit qui décide d'une essence.
L'essence est une **fonction pure de la position monde et d'une graine** :

    TreeSpecies treeSpeciesAt(worldX, worldZ, aspectExposure, params)

Elle n'est stockée nulle part comme vérité — `Forest` la mémorise dans chaque
arbre par commodité d'accès, `Vegetation` la met en cache avec sa cellule — mais
les deux la tirent du **même appel**. Ce qui est résineux à l'œil est donc
résineux à la saignée par construction, et non par accord entre deux tables
qu'il faudrait tenir à jour.

Le tirage porte sur la **position** et non sur un indice : un indice n'existe que
dans le système qui l'attribue, et les deux systèmes n'en ont pas de commun. La
position est quantifiée à 25 cm avant hachage — bien plus fin que l'espacement
minimal entre deux pieds (5,5 m), bien plus gros que le bruit de calcul.

### L'exposition : consommée, pas recalculée

Le signal d'adret/ubac de `WeatherSample::aspectC` dépend du **soleil courant** :
il tourne avec l'heure et s'éteint la nuit. Une essence ne peut pas changer entre
midi et minuit.

La formule a donc été **extraite** de `sampleAt` dans
`WeatherSystem::aspectSignal`, sans en changer une ligne de calcul, et
`WeatherSystem::aspectExposureAt` l'évalue **au soleil de midi**. Une seule
formule, deux instants d'évaluation. Preuve que l'extraction n'a rien changé :
les tests headless redonnent **14,7 °C d'adret contre 10,7 °C d'ubac, écart
4,0 °C** — les chiffres exacts documentés en Phase 23.

`Forest` et `Vegetation` ne connaissent pas `WeatherSystem` : elles voient
`AspectExposureSource`, une interface minuscule sur le modèle de
`GroundCoverSource` (Phase 17). Contrat total : source absente = exposition nulle
= part de résineux neutre partout, donc le comportement d'avant la phase.

La source est **paramètre du constructeur** de `Forest`, et non un simple setter :
le constructeur sème immédiatement, si bien qu'un setter aurait créé un ordre à
respecter, donc un piège silencieux du type dont ce projet a déjà payé le prix.

### La tendance, jamais la règle

    part de resineux = coniferShareNeutral - aspectSwing x exposition
                     = 0,55 - 0,35 x exposition       (bornee a [0,10 ; 0,92])

0,55 reprend **tel quel** l'ancien `ResinParams::coniferShare` : le réglage de la
Phase 22 n'est pas perdu, il devient le point neutre autour duquel l'exposition
fait pencher la balance. Les bornes dures ne mordent pas avec ces valeurs ; elles
sont là pour qu'un reréglage d'`aspectSwing` ne puisse pas, par inadvertance,
transformer la tendance en règle stricte.

### La présence n'a pas bougé, et c'est structurel

Dans `Vegetation::buildTreeCell`, l'appel à `treeSpeciesAt` est placé **après**
tous les filtres de présence — couvert LiDAR, pente, altitude, hauteur de
canopée. L'emplacement **est** déjà retenu quand l'essence est choisie.
L'exposition n'a donc aucun moyen d'ajouter ou de retirer un arbre. La contrainte
du chantier est tenue par la **structure** du code : déplacer cet appel de vingt
lignes vers le haut serait le seul moyen de la rompre.

**Et c'est vérifié par la mesure** (voir plus bas) : le nombre total d'arbres est
rigoureusement identique avec et sans le couplage.

### Les deux silhouettes

`TreeMesh.hpp` annonçait depuis la Phase 5 : « la variété de silhouette viendra de
plusieurs meshes distincts le jour où ce sera un sujet, pas d'un étirement ».
C'est ce jour-là.

Le conifère existant est conservé tel quel (tronc conique + deux cônes empilés,
**facetté**). Le feuillu ajoute un houppier **arrondi** : des anneaux empilés sur
un profil elliptique, **lissés** (normales radiales partagées). L'opposition ne
tient donc pas qu'au contour — les deux arbres n'accrochent pas la lumière de la
même façon. Teintes du feuillu plus claires et plus jaunes que le vert bleuté du
conifère, second indice de lecture après la silhouette.

**Le tronc est le même, et ce n'est pas un raccourci** : la souche interpole son
rayon de coupe sur la droite du tronc de l'arbre. Un second tronc aurait imposé
une seconde souche, et surtout la souche laissée par un feuillu abattu ne
coïnciderait plus avec le trait de coupe. **Une** souche sert les deux essences
parce que les deux partagent exactement le même tronc.

### La dette de la Phase 22, refermée

`world/Resin.hpp` écrivait : « un joueur ne peut pas VOIR quel arbre est résineux
avant d'essayer, puisque tous les arbres ont le même mesh ».

`ResinStand` **ne décide plus de rien**. Plus de graine d'essence, plus de part de
résineux, plus de hachage : `coniferShare` et `seed` ont quitté `ResinParams`.
`tap()` **reçoit** l'essence de l'arbre visé, lue dans `Tree::species` — celle-là
même dont la silhouette est dessinée à l'écran.

Faire lire à `ResinStand` la même fonction que le rendu aurait suffi à les faire
coïncider aujourd'hui ; le lui faire **donner** par l'appelant garantit qu'ils ne
peuvent pas diverger demain, parce qu'il n'y a plus de second calcul à maintenir.
La même exigence que la Phase 23 pour le soleil et la Phase 25 pour l'écart
d'incidence.

### Le rendu

Deux géométries distinctes = **deux appels de dessin instanciés** au lieu d'un.
C'est irréductible : l'instanciation tient UNE géométrie pour N instances. Les
mélanger demanderait une matrice par arbre, c'est-à-dire de défaire la Phase 11.
Le surcoût est d'**un appel de dessin par image**, pas d'un par arbre — le nombre
d'instances est inchangé.

`Vegetation::gatherTrees` a une surcharge à deux vecteurs ; la surcharge à un seul
vecteur appelle la première **avec le même vecteur passé deux fois**, ce qui rend
l'union exactement dans l'ordre d'avant la phase, sans copie et sans qu'il existe
une seconde version du parcours.

### Les deux témoins

| option | ce qu'elle neutralise | ce qu'elle sert à mesurer |
|---|---|---|
| `--no-species-aspect` | le **couplage au versant** seul (`aspectSwing = 0`) | l'effet revendiqué par cette phase |
| `--single-tree-species` | la **seconde essence** (100 % résineux, donc 1 appel) | le coût du second appel de dessin |

`--no-species-aspect` est délibérément étroit : éteindre les essences aurait
mélangé deux effets, « il y a deux formes d'arbres maintenant » et « leur
répartition suit le versant ». Il garde le même nombre d'arbres, la même
proportion globale et les deux silhouettes — seule la corrélation au versant
disparaît. Toute différence entre les deux captures ne peut donc venir que de ce
couplage, qui est exactement ce que cette phase prétend avoir ajouté.

### La vérification, mesurée

Zone de 25 km² (`vercora_zone_1m.vchm`, la seule qui porte une carte de couvert
LiDAR), vue plongeante sur le versant où l'on se tient.

| vue | avec couplage | témoin sans couplage | total d'arbres |
|---|---|---|---|
| **ubac** (exposition −0,75) | 3034 / 1522 = **66,6 %** de résineux | 2494 / 2062 = **54,7 %** | 4556 **/ 4556** |
| **adret** (exposition +0,20) | 810 / 723 = **52,8 %** | 840 / 693 = **54,8 %** | 1533 **/ 1533** |

Trois choses se lisent d'un coup :

1. **Le total est rigoureusement identique** dans chaque paire. La carte de
   couvert LiDAR pilote toujours seule la présence — l'espèce n'ajoute ni ne
   retire un arbre. Le point 3 du chantier est vérifié, pas supposé.
2. **Le témoin tombe sur 54,7 % et 54,8 %** dans les deux vues, c'est-à-dire sur
   `coniferShareNeutral` (0,55). Le témoin fait ce qu'il annonce.
3. **L'écart est bipolaire** : +11,9 points de résineux en ubac, −2,0 points en
   adret. Le signe est correct des deux côtés. L'amplitude plus faible en adret
   suit l'exposition, qui n'y vaut que +0,20 contre −0,75 en ubac — les adrets de
   cette zone sont réels mais peu marqués.

Une capture supplémentaire au belvédère de la Phase 25 (E 900000 / N 6436000,
`--look=270,-24`) donne 1985 / 866 avec couplage contre 1616 / 1235 au témoin,
pour un total identique de 2851 arbres.

La télémétrie qui porte ces chiffres est émise par le jeu une fois par seconde
(`Essences : N résineux et M feuillus à l'écran, exposition ici X`) : une capture
montre une différence, elle ne la **chiffre** pas.

### Validation Vulkan

**Zéro message `[Validation]`** sur toutes les captures et mesures (couches
actives, build Debug).

### Tests headless

`tools\run_logic_tests.bat` : **3607 vérifications passées**, dont la section
« Phase 22 / 26 : essences d'arbres et résine de pin » — déterminisme du tirage,
part observée conforme à la loi, les deux témoins (part nulle / part pleine), une
graine différente qui change réellement le tirage, et la bipolarité adret/ubac
sur la loi elle-même.

### Le coût : mesuré, et il n'y en a pas

Quatre paires de runs **entrelacés** (témoin, condition, témoin, condition…) sur
la même vue et le même binaire, machine au repos. Moyennes sur trois paires
exploitables :

| | 1 appel (témoin) | 2 appels | écart |
|---|---|---|---|
| images en 30 s | 3135 | 3168 | **+1,0 %** |
| `arbres` (semis CPU) | 1634,0 µs | 1633,4 µs | −0,0 % |
| **`herbe` (poste témoin)** | 4972,5 µs | 4977,3 µs | **+0,1 %** |
| `drawFrame` | 1950,2 µs | 1854,8 µs | −4,9 % |
| tampon d'instances | 522 Ko | 522 Ko | — |
| géométrie terrain GPU | 22 Mo | 22 Mo | — |

**Le poste témoin valide la mesure.** L'herbe ne dépend en rien des essences :
elle diffère de 0,1 %, donc la machine n'a pas dérivé entre les conditions. C'est
le contrôle qui manquait à la première tentative, et c'est lui qui autorise à
lire les autres lignes.

**Les deux écarts sont dans le bruit.** La dispersion à l'intérieur d'une même
condition vaut 0,8–0,9 % sur les images et **5,3–5,7 % sur `drawFrame`** : le
−4,9 % constaté est donc plus petit que le bruit du poste lui-même. Il n'y a pas
de surcoût mesurable, et il n'y a pas non plus de gain à revendiquer.

**Ce qui rend le résultat intéressant** : la condition à deux appels soumet
**45 % de triangles d'arbres en plus** —

    1 appel  : 4556 x 48                = 218 688 triangles
    2 appels : 3034 x 48 + 1522 x 112   = 316 096 triangles

le feuillu ayant 112 triangles contre 48 au conifère. Quarante-cinq pour cent de
géométrie supplémentaire ne se voient pas dans le temps d'image : l'image n'est
pas limitée par le débit de sommets des arbres. C'est cohérent avec ce que la
Phase 19 avait établi — le frame attend ailleurs.

Côté mémoire, rien ne bouge : le tampon d'instances est identique (le nombre
d'instances l'est), et le feuillu ajoute **une** géométrie de 129 sommets /
112 triangles téléversée une seule fois.

### Ce qui reste ouvert

* La faune, les plantes à fibres et l'herbe ne sont pas touchées par les essences.
* Le shader `foliage` ne teinte toujours pas les arbres selon l'adret/ubac
  (limite héritée de la Phase 25) : l'indice visuel de température reste sur le
  terrain seul.
