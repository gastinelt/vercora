# NOTES-PHASE17.md — Première empreinte environnementale

Session du 2026-08-25. Suite de la Phase 16 (besoins primaires). Objectif :
que le monde GARDE UNE TRACE de ce que le joueur y fait, et que cette trace
change ce qu'il y trouve — sans jamais le lui dire.

Ce fichier documente ce qui a été construit, les chiffres retenus et POURQUOI,
les deux erreurs de réglage que seule une mesure sous Vulkan a révélées, et
surtout ce qui n'est pas fait.

---

## 1. Ce qui existe maintenant

| Fichier | Rôle |
|---|---|
| `src/world/Habitat.hpp/.cpp` | la mémoire locale du milieu : deux compteurs par cellule, montée, décroissance, oubli, seuils |
| `src/world/Wildlife.hpp/.cpp` | la faune : semis par tirage-rejet, deux états, fuite, disparition, retour |
| `src/world/AnimalMesh.hpp/.cpp` | le cervidé bas-poly, généré en code comme l'arbre et le galet |
| `src/world/GroundCover.hpp` | l'interface par laquelle le semis d'herbe interroge l'état du sol |
| `src/world/Vegetation.cpp` | un facteur de densité en plus dans `gatherGrass` — la seule ligne changée d'un système existant |
| `src/main.cpp` | câblage : trois lignes dans la boucle, l'inscription de l'abattage, le dessin, le journal |
| `tests/logic_tests.cpp` | sections « Phase 17 » : **97 assertions** ajoutées (788 → **885**) |
| `tools/run_vulkan_check.ps1` | **scénario I** : la faune et la fréquentation sous Vulkan, sur la dalle mesurée |

Plus une option de jeu : **`--no-wildlife`**, qui coupe la faune et la mémoire du
milieu et rend le comportement d'avant la phase. C'est un TÉMOIN, au même titre
que `--no-vegetation`, et il a servi dès cette session (§5.3).

**Rien n'a changé de comportement** dans `Forest`, `Ground`, `Needs`, `Camera`,
`Bike`, `Climber`, `Paraglider` ni dans l'éditeur. `Forest` en particulier
n'a **pas** été touché : c'est `main.cpp` qui inscrit l'abattage dans le milieu,
après un `fell()` réussi. L'abattage est une mécanique de la Phase 5 ; l'empreinte
qu'il laisse est une lecture de la Phase 17, et les deux n'ont pas à se connaître.

---

## 2. Le modèle

### 2.1 Deux mémoires, et c'est le point de départ

| | fréquentation | déforestation |
|---|---|---|
| ce qui la fait monter | la présence du joueur, pondérée par le bruit de son activité | un arbre abattu |
| vitesse de montée | saturée en 60 s de marche sur place | 3 arbres dans une cellule |
| vitesse de descente | 4 min | 15 min |
| ce qu'elle empêche | la faune reste / revient | la faune revient, ET le sol garde son couvert |

Les fusionner aurait été une erreur de modèle : un dérangement CESSE quand on
s'en va, un prélèvement NON. C'est la différence entre « il y a quelqu'un » et
« il manque un arbre ».

Une cellule fait **32 m**, et la grille est **creuse** : seules les cellules
touchées existent, et une cellule revenue à neuf est oubliée. Sur 25 km² il y
aurait 24 000 cellules ; une partie en touche quelques dizaines. C'est le même
raisonnement que le semis à la demande de `Vegetation`.

### 2.2 Le bruit n'est pas l'effort

`activityNoise` (Habitat) est le pendant de `activityEffort` (Needs), et les deux
échelles **ne coïncident pas** :

```
              effort (Ph.16)   bruit (Ph.17)
  repos            0,0             0,25
  marche           1,0             1,00
  vélo             1,8             2,00
  escalade         4,0             0,60
  parapente        0,3             0,15
```

L'escalade est le cas qui le prouve : c'est l'effort **maximal** du joueur et
l'une des activités qui dérange le **moins**. Le repos n'est pas à zéro — un
homme immobile est quand même là.

### 2.3 La pondération, et pourquoi elle est en carré

La présence dérange sur un rayon de **120 m**, avec un poids
`1 - (d/r)²` où `d` est la distance au point le plus proche de la cellule (pas à
son centre : la cellule où l'on se tient doit être pleinement dérangée quel que
soit l'endroit où l'on y est tombé).

Une pondération **linéaire** ne marchait pas, et pas pour des raisons
esthétiques : à la périphérie du rayon elle tombe en dessous du taux de
décroissance, donc une cellule éloignée ne peut JAMAIS monter, quel que soit le
temps passé. La deuxième mécanique demandée par le cahier des charges — « rester
trop longtemps dans un rayon donné » — n'aurait tout simplement pas existé.

Corollaire assumé : au **bord** du rayon la pondération tend vers zéro, donc les
cellules situées juste en deçà de 120 m restent presque intactes et acceptent
encore une apparition de temps en temps. Un résidu d'un ou deux animaux à la
limite est le comportement **correct** du modèle.

### 2.4 Le défaut qui rendait tout muet : l'époque de l'oubli

Premier run : **rien ne montait**. `trackedCellCount` valait zéro après une
minute de présence.

La cause : au premier pas de temps une cellule ne contient que 0,0003 — donc
moins que l'epsilon d'oubli (1e-3) — et `update` l'effaçait à l'image même où
`notePresence` venait de la créer. La mécanique entière était annulée, en
silence, sans le moindre message.

Corrigé sur deux points : une cellule **occupée** n'est jamais oubliée, et elle
ne décroît pas non plus tant que le joueur y est (sans quoi montée et
décroissance se retranchent, et une présence faible mais continue — le campement
à distance — reste indéfiniment à zéro).

---

## 3. La faune

24 animaux au plus, semés par **tirage-rejet** autour du joueur, dans un anneau
de **55 à 260 m**. Un candidat est refusé s'il est hors emprise, sur une pente
> 0,9, sur un couvert LiDAR < 0,25, à moins de 14 m d'un autre animal, ou dans
une cellule que le milieu refuse. Le nombre réellement présent est donc une
**conséquence** du terrain et de l'état du milieu, jamais une consigne.

Deux états, deux déclencheurs de fuite :
* le joueur passe à moins de **30 m** → départ immédiat ;
* le milieu a retenu une présence prolongée (`wildlifeFlees`) → départ, **même
  si le joueur ne s'est jamais approché**. C'est le déclencheur qui compte : un
  camp finit par vider son vallon.

La fuite est une ligne droite à **8 m/s**, opposée au joueur, par pointes de 5 s
renouvelées tant que l'animal n'est pas tranquille. Poursuivre ne mène nulle
part, et c'est le propos.

### Ce que la faune n'a pas
* **Aucune IA** : pas de trajet, pas d'évitement d'obstacle, pas de troupeau.
* **Aucune collision** : un animal traverse les troncs, et le joueur le traverse.
  Le `CollisionSolver` de la Phase 6 est là, l'y brancher serait mécanique — ce
  n'est pas fait.
* **Aucune persistance** : ce ne sont pas des individus qu'on suit, c'est une
  population locale.
* **Une seule espèce**, un seul mesh. La Phase 17 construit la BOUCLE, pas un
  bestiaire.

---

## 4. La dégradation par surexploitation

Trois arbres abattus dans la même cellule (32 m) la saturent. Deux conséquences,
toutes deux **observables en jouant**, aucune annoncée :

1. **La faune n'y revient plus** tant que le compteur est au-dessus de 0,40 —
   soit une bonne dizaine de minutes après deux arbres.
2. **Le sol se dénude** : la densité d'herbe de `Vegetation::gatherGrass` est
   multipliée par le couvert rendu par l'`Habitat`, qui tombe jusqu'à 0,15 au
   pire. Mesuré par les tests : au pied d'une coupe saturée, **plus de la moitié
   de l'herbe disparaît**, et elle revient intégralement après vingt minutes.

### Pourquoi l'herbe et pas le shader terrain

Le cahier des charges proposait « le sol devient plus nu dans le shader terrain
existant ». Ce n'est **pas** la voie retenue, et c'est un arbitrage à connaître :

* rendre la dégradation visible dans `terrain.frag` demande de porter les
  compteurs jusqu'au GPU — un binding de plus, une texture à téléverser et à
  ré-uploader quand elle change, donc un chantier de synchronisation Vulkan à
  part entière (et la leçon de la Phase 12 s'applique : les couches standard ne
  voient PAS les dépendances mémoire) ;
* faire disparaître l'herbe donne le **même signal visuel** — un sol pelé au
  milieu d'un versant herbeux — pour zéro objet Vulkan nouveau, donc zéro risque
  de validation, et en réutilisant un système déjà mesuré.

Contrepartie honnête : **là où il n'y a pas d'herbe, la dégradation ne se voit
pas.** Sur du lapiaz ou au-dessus de 2250 m, une coupe ne laisse aucune trace
visuelle — seulement l'absence de faune. Le passage par le shader reste la bonne
réponse à terme.

Autre choix assumé : **les arbres de décor ne disparaissent pas**. Ce sont les
arbres ABATTABLES de `Forest` qui tombent, un par un, visiblement. Faire
s'évaporer la futaie de `Vegetation` aurait effacé la coupe au lieu de la montrer.

---

## 5. Deux réglages faux que seule la mesure a révélés

Ils méritent d'être écrits parce que dans les deux cas le code était juste, les
tests headless verts, et le résultat en jeu **nul**.

### 5.1 Deux rayons réglés séparément qui ne se recouvraient pas

`noiseRadius` valait 45 m (« un peu plus d'une cellule ») et `spawnRadiusMin`
valait 90 m (« ne jamais apparaître dans le champ proche »). Chacun défendable
seul. Ensemble : **aucun animal ne se trouvait jamais dans le rayon dérangé.**

Run du scénario I : 24 animaux au départ, 24 après deux minutes de présence
continue, fréquentation locale à 100 %, **zéro fuite**. Le système marchait
exactement comme écrit, et ne produisait rien.

Corrigé : `noiseRadius` = 120 m, et la condition est nommée dans l'en-tête —
**il faut que `noiseRadius` dépasse `spawnRadiusMin`**, sans quoi la deuxième
mécanique de la phase n'existe pas.

### 5.2 Le seuil d'apparition, et une faune qu'on ne voyait jamais

Même mesure, seconde lecture : à 90 m, un cervidé de 1,3 m est un point. La
faune existait sans qu'on puisse la regarder. `spawnRadiusMin` est descendu à
**55 m** — au-dessus de la distance de fuite (30 m), donc rien n'apparaît sous
le nez du joueur, et franchement en deçà du rayon dérangé.

Après correction, même scénario : **4 animaux à portée de vue au départ, 1 après
quatre minutes de présence**, deux en fuite au pic.

### 5.3 Un scénario voisin qui n'avait aucune marge (scénario F, escalade)

Troisième surprise, et elle ne vient pas du code de la phase : **le scénario
d'escalade a commencé à échouer**, alors que rien de ce qu'il exerce n'a été
touché.

Diagnostic, avec témoin : la faune coûte **6 % d'images par seconde**
(301 contre 320 FPS, mesuré sur la paroi du scénario F grâce à l'option
`--no-wildlife` ajoutée exprès). C'est peu — et c'était assez. La trajectoire du
grimpeur dépend du pas de temps : à chaque image sa commande est projetée sur la
ligne de plus grande pente **locale**, donc deux cadences d'images ne suivent pas
exactement le même chemin sur la paroi. Le scénario laissait 18 s de marge sur
une montée de 57 s ; le grimpeur montait toujours aussi haut (41,5 m contre
41,4 m au témoin) mais n'annonçait plus le sommet avant l'échéance.

Corrigé côté **harnais**, pas côté jeu : l'échéance passe de 75 s à 140 s. Le
harnais sort de la boucle dès que le sommet est annoncé, donc cette marge ne
coûte rien quand tout va bien. Vérifié : sommet atteint, faune active.

`--no-wildlife` reste dans le jeu — il coupe la faune ET la mémoire du milieu,
et rend le comportement d'avant la phase. Même rôle que `--no-vegetation` : sans
témoin, on ne peut attribuer un écart à rien.

### 5.4 La leçon générale

C'est la même que celle de la Phase 14 (« savoir où il y a de la pente ne dit pas
par où l'aborder ») sous un autre jour : **deux constantes réglées chacune pour
de bonnes raisons, dans deux fichiers différents, peuvent se rater complètement,
et aucun test unitaire ne le dira** — chacune est correcte. Il faut une mesure de
bout en bout, et une métrique qui porte sur ce que le joueur VOIT (les animaux à
portée de vue) et non sur ce qui est facile à compter (la population totale).

---

## 6. Aucune notification, et ce que ça coûte

Rien n'apparaît à l'écran : ni pop-up, ni compteur, ni ligne de titre. Le titre
de la fenêtre porte toujours l'inventaire, les jauges et l'heure — rien de plus.

Le journal, lui, émet une ligne par seconde (population, fuites, animaux à portée
de vue, fréquentation et déforestation locales, couvert au sol, cellules
mémorisées, pointes). **C'est un instrument de mesure, pas une interface** :
c'est la seule trace dépouillable par le harnais Vulkan, exactement comme la
ligne de besoins de la Phase 16.

Conséquence assumée : **un joueur peut très bien ne rien remarquer.** Le système
est observable, il n'est pas évident. C'est le prix de « pas de message
moralisateur », et c'est le bon prix — mais il faudra un jour vérifier sur
quelqu'un d'autre que soi que la boucle se lit.

---

## 7. Vérifications

* **Tests headless** : `tools\run_logic_tests.bat` — **885 assertions**
  (788 avant), dont 97 pour cette phase : les deux mémoires et leur asymétrie,
  l'oubli des cellules, les seuils et leur hystérésis, la robustesse du pas de
  temps, la convention du mesh d'animal et sa matrice d'instance, le semis et
  ses quatre filtres, le déterminisme, les deux déclencheurs de fuite, la
  disparition au loin, le refus de repeuplement d'une zone surfréquentée **et
  son retour**, et la chaîne complète coupe → sol nu → repousse.
* **Non-régression littérale** : un `Habitat` vierge branché sur la `Vegetation`
  rend **exactement le même nombre de touffes** qu'avant la Phase 17. Ce n'est
  pas « presque pareil » — c'est le contrat total de `GroundCoverSource`, et
  c'est une assertion.
* **Vulkan** : `tools\run_vulkan_check.ps1`, **scénario I** ajouté (neuf
  scénarios, onze processus). Zéro `[Validation]`, zéro `[ERROR]`, codes de
  sortie tous nuls, fermeture propre partout.

### Un échec ANTÉRIEUR à cette phase, non corrigé (scénario E, vélo)

Le scénario du vélo échoue sur sa dernière étape : le « pied à terre » (touche B
finale) n'est pas enregistré — aucune ligne de journal n'est émise, donc le front
descendant de la touche n'a pas été vu. **Ce n'est pas dû à la Phase 17** :
rejoué deux fois avec `--no-wildlife`, c'est-à-dire dans le comportement exact
d'avant la phase, il échoue de la même façon. La sonde montre par ailleurs que
le vélo dévale encore à 31 km/h à ce moment-là, alors que le scénario vient de
freiner 5 s — la descente scriptée ne se termine plus là où elle a été réglée.

C'est donc une dérive du scénario E (ou du pilotage par messages postés), pas du
gameplay ; tout le reste du vélo est vert, y compris la télémétrie de pente, de
vitesse et de dénivelé. **Non corrigé** : hors du périmètre de cette session, et
le corriger à l'aveugle aurait été plus risqué que de le signaler.

**Conséquence à connaître : la suite complète ne peut pas être verte tant que ce
point n'est pas repris.** Tout le reste l'est.

### Un aléa d'automatisation, pas un blocage

Un run complet a aussi vu le processus G2 ne pas sortir dans les 20 s après
`WM_CLOSE`. Vérifié : le scénario G rejoué **trois fois de suite en isolation
passe trois fois**, faune active. C'est exactement la flakiness déjà décrite dans
MEMORY.md (« WM_CLOSE lent à être traité »), pas un blocage à la fermeture — rien
dans cette phase ne touche à l'arrêt : ni fil d'exécution, ni ressource GPU, ni
synchronisation.

---

## 8. Ce qui n'est pas fait

* **Le couvert LiDAR réel n'est pas exercé sous Vulkan.** La dalle de référence
  du harnais (`LHD_0907_6440_1m.vchm`) n'a pas de carte de canopée produite, donc
  le scénario I place la faune avec le couvert de substitut. Le filtre par
  couvert réel est couvert par les tests headless (carte synthétique) et par la
  zone de 25 km², qui a bien ses cartes — mais aucun scénario automatisé ne
  charge celle-ci.
* **Pas de collision** entre le joueur et les animaux, dans aucun sens.
* **Pas de dégradation par piétinement** : seul l'abattage dénude le sol. La
  fréquentation, elle, n'use pas les sentiers. C'était dans l'esprit du cahier
  des charges (« usure des sols ») et ce n'est pas fait.
* **Pas de sauvegarde** : la mémoire du milieu meurt avec le processus. Une
  partie relancée retrouve un monde intact.
* **Une seule espèce, un seul mesh, aucun cycle jour/nuit** dans le comportement,
  alors que `GameClock` existe depuis la Phase 16 — une faune crépusculaire
  serait le prolongement naturel.
* **Rien ne relie encore la faune aux besoins** : on ne chasse pas, on ne mange
  pas ce qu'on voit fuir.
