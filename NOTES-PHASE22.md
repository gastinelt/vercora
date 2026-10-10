# Phase 22 (2026-08-27) — trois matières récoltables, et la dette du vélo

Deux chantiers indépendants. Le second était la priorité annoncée : une dette
ouverte en **Phase 17** et rejouée à chaque phase depuis.

---

## Tâche 2 — la dette du vélo : le jeu n'a jamais eu de défaut

### Le symptôme, tel qu'il traînait depuis cinq phases

Le scénario E du harnais Vulkan finit par un « pied à terre » (touche `B`) qui
n'était **jamais** enregistré. `Wait-ForLog "Velo : pied a terre" 3000` rendait
`False`, run après run, et la suite complète ne pouvait donc pas être verte.
NOTES-PHASE17.md concluait à « une dérive du scénario E (ou du pilotage par
messages postés), pas du gameplay », sans aller plus loin.

### Ce qui a été mesuré, dans l'ordre

**1. Isoler le vélo lui-même.** Trois scripts de reproduction, hors du harnais,
qui rejouent les morceaux du scénario un par un sur la même dalle et le même
binaire :

| ce qui est rejoué | runs | résultat |
|---|---|---|
| enfourcher + 40 s de pédalage | 3 | 3/3 propres, 41 relevés, sortie 0 |
| idem + 14 rafales de demi-tour (S+D) | 3 | 3/3 propres, 51 relevés |
| élan + 32 s de descente + freinage + **pied à terre** | 3 | 3/3 propres, `pied a terre` **journalisé**, sortie 0 |

Le troisième est décisif : **la fin exacte du scénario E, jouée seule, marche
parfaitement**. Le cap de descente y est imposé par `--look=40,0` au lieu d'être
atteint par asservissement, ce qui supprime la seule partie du scénario qui
dépendait de la télémétrie.

**2. Lire le journal du JEU, et pas seulement celui du script.** Une copie
instrumentée du harnais qui déverse `$runE.Lines` dans un fichier. Verdict sans
appel : la ligne `[INFO]  Velo : pied a terre.` **est là**, ligne 490 sur 520,
suivie de la « Fermeture propre. » habituelle et d'un code de sortie 0.

> Le jeu avait exécuté la descente de vélo correctement à chaque run depuis la
> Phase 17. Ce qui échouait, c'était l'instrument de mesure.

**3. Chronométrer le collecteur.** Un script qui reproduit **exactement** le
mécanisme de capture de `Start-Game` (`Register-ObjectEvent` +
`BeginOutputReadLine` + `StringBuilder`) et horodate l'apparition de chaque
ligne :

```
en selle      : vu=True  après 1 833 ms
pied a terre  : vu=False après 3 090 ms   (11 880 octets captés pendant l'attente)
pied a terre  : vu=True  après 9 600 ms au total
```

**4. Le témoin.** Le même script, le même jeu, le même instant de lecture — seul
le collecteur change, remplacé par un délégué .NET compilé :

| collecteur | « en selle » | « pied a terre » |
|---|---|---|
| `Register-ObjectEvent -Action` (le harnais) | 1 833 ms | **jamais vu en 3 s** (9,6 s réels) |
| délégué .NET compilé (témoin) | 5 ms | 0 ms |

### La vraie cause

Le harnais capturait la sortie du jeu avec **un script-block PowerShell exécuté
par ligne de journal**. Ces blocs ne tournent que quand le runspace est
disponible ; or les scénarios passent leur temps dans des `Start-Sleep` et des
boucles d'attente. Les événements s'empilent et le tampon que `Wait-ForLog`
interroge est **en retard** sur ce que le jeu a réellement écrit — **et le retard
croît avec la durée du scénario**.

Le scénario E est le plus long de la suite. Il était simplement le premier à
franchir le seuil : « en selle », mesuré à 1 833 ms, consommait déjà **61 % de
son budget de 3 s**. Le défaut menaçait tous les `Wait-ForLog` du harnais.

### Le correctif

`tools/run_vulkan_check.ps1` : le collecteur est désormais un délégué .NET
compilé (`VercoraLogSink`, `Add-Type`), qui se branche lui-même — PowerShell 5.1
ne sait pas convertir une `PSMethod` en `DataReceivedEventHandler`. `ToString()`
est surchargé, donc **aucun appelant n'a été touché** : `Wait-ForLog`,
`Get-BikeHeading`, `Wait-BikeSample`, `Stop-Game` et le scénario F appelaient
déjà tous `.ToString()` sur le `StringBuilder` d'avant.

**Aucune ligne du jeu n'a été modifiée pour cette tâche.**

### Le résultat, et une confirmation qui n'était pas cherchée

`pied a terre : True`. Et, indépendamment, le second demi-tour du vélo converge
maintenant **en 3 rafales au lieu de 9** : `Turn-BikeTo` s'asservit sur un cap
qui n'est plus périmé de plusieurs secondes. Même cause, deuxième preuve.

### Une fausse piste, notée parce qu'elle a coûté du temps

Le tout premier run de diagnostic a montré le jeu mourant à ~30 s avec un code
de sortie `-1`. Deux runs suivants ont duré 105 s et 280 s. C'était un
**artefact** : un processus `vercora.exe` d'une mesure précédente n'avait pas été
tué et les deux se disputaient le GPU. Une fois le parasite éliminé, le jeu a
tourné proprement dans tous les runs suivants (12 au total). **Leçon : vérifier
qu'aucun processus de jeu ne survit avant de conclure quoi que ce soit d'une
mesure de performance ou de stabilité.**

---

## Tâche 1 — argile, sable, résine, plantes à fibres

Les trois matières du document de cadrage qui manquaient. Toutes suivent le
modèle `Forest`/`Knapping` : **indépendantes de Vulkan, testables headless,
appuyées sur les vraies données LiDAR quand c'est pertinent**.

Elles arrivent toutes **par le sol**, comme l'éclat de percussion de la Phase 6 :
l'interaction dépose l'objet aux pieds du joueur, qui doit ensuite le ramasser
(`R`). Rien ne tombe directement dans l'inventaire — ce qui est produit doit
d'abord **exister quelque part**.

### 1. Argile et sable — `world/Deposits.hpp/.cpp`

**Le proxy géologique, et sa limite, à assumer sans détour.**

> Le LiDAR ne porte **aucune** information de nature de terrain : ni lithologie,
> ni granulométrie, ni carte des formations superficielles. Il donne une
> surface. Il est donc **strictement impossible**, à partir de ces données, de
> distinguer un vrai gisement d'argile d'un vrai gisement de sable, ou de l'un
> ou l'autre d'un banc de calcaire.

Ce qui est codé est un **proxy morphologique** : les gisements sont placés là où
ils seraient *plausibles*, pas là où ils sont. La règle, et son raisonnement :

* **un creux** — même critère de concavité que `WaterSource` (huit sondes de
  pourtour), parce que c'est la seule forme d'un relief mesuré où quelque chose
  peut s'accumuler au lieu d'être emporté ;
* **un versant au-dessus** (`minUpslope`, 3 m) — la matière doit *venir* de
  quelque part. C'est ce critère, et non la concavité, qui écarte les replats de
  crête : un creux de sommet est concave mais n'a rien au-dessus de lui ;
* **argile si un point d'eau détecté est à moins de 120 m, sable sinon** —
  l'argile est la fraction la plus fine, elle ne se dépose que là où l'eau
  stagne vraiment. Sans point d'eau connu, **tout est classé sable** : on
  n'affirme rien sur l'argile qu'on ne puisse pas justifier.

Seuils plus permissifs que ceux de l'eau, et pour une raison : *l'eau doit
stagner pour qu'on y boive, un sédiment n'a qu'à s'être déposé*. Pente admise
22 % (contre 15 %), concavité 0,6 m (contre 1,0 m), 4 sondes montantes sur 8
(contre 5).

**Sur la dalle de référence** : 18 gisements, **2 d'argile et 16 de sable** — et
les deux argiles tombent exactement sur les deux points d'eau détectés par la
Phase 16, ce qui est la signature attendue du critère.

**Récolte** : touche `Y`, portée horizontale 3,5 m.

### 2. Résine de pin — `world/Resin.hpp/.cpp`

**La limite, en clair** : le jeu n'a **aucune** notion d'essence d'arbre. Un
arbre est une position, une hauteur, un yaw et un état ; son mesh est unique,
généré une fois et instancié N fois. La carte de canopée LiDAR donne une hauteur
et un taux de couvert, pas une classification feuillu/résineux.

Le choix retenu est donc un **marquage déterministe** : un hachage de l'indice de
l'arbre et d'une graine décide, une fois pour toutes, quels arbres sont des
résineux (55 %). Ce n'est pas une donnée, c'est une **convention** — mais une
convention stable, recalculée et jamais stockée, comme le semis de végétation.

> **Conséquence à assumer : un joueur ne peut pas *voir* quel arbre est un
> résineux avant d'essayer.** Tous les arbres ont le même mesh. Donner une
> silhouette distincte aux résineux est le prolongement naturel de ce fichier ;
> ce n'est **pas** fait dans cette phase, et c'est la principale dette qu'elle
> laisse.

**Ce qui diffère de l'abattage, et c'est tout le propos.** On vise le même arbre,
par le **même** `Forest::pickTree` et avec la même portée — mais rien n'est
abattu, rien n'est débité, et l'`Habitat` n'enregistre **aucune coupe**. L'arbre
reste `Standing`. En échange il ne redonne pas de résine avant 600 s, et il est
épuisé au bout de trois entailles. Touche `U`.

### 3. Plantes à fibres — `world/FiberPlants.hpp/.cpp`

Ronces et orties sont des plantes de **rudéralie**. Deux voies indépendantes,
combinées par un **maximum** (et non une somme : une coupe faite en pleine
lisière ne doit pas porter deux fois plus de ronces) :

1. **La lisière**, lue dans la carte de couvert LiDAR — ni la futaie fermée
   (l'ombre les tue) ni l'alpage nu (rien ne les abrite), mais la bande de
   transition. La densité suit un **toit triangulaire** sur la bande, maximale
   en son milieu et nulle à ses deux bords : avec un créneau, un versant
   clairsemé entier porterait des ronces partout, ce qui est une lande et non
   une lisière. Les bornes encadrent volontairement `VegetationParams::coverMin`,
   le seuil à partir duquel les arbres de décor apparaissent — les deux fichiers
   parlent bien de la même frontière.

2. **Le sol récemment ouvert**, lu dans une nouvelle interface
   `world/Clearing.hpp` (`ClearingSource`), implémentée par `Habitat` à partir de
   son compteur de déforestation. **Une zone où le joueur a abattu des arbres se
   met à porter des ronces.** C'est la cohérence que le cahier des charges
   suggérait, et elle est exploitée parce qu'elle est *vraie* : la recolonisation
   d'une coupe par les ronces est le fait le plus banal de la sylviculture
   tempérée. Elle a en prime une vertu de jeu que rien d'autre dans le projet
   n'avait : **c'est la première fois qu'une action du joueur FAIT APPARAÎTRE
   quelque chose au lieu de le retirer.** La Phase 17 avait construit la boucle
   « je coupe, le milieu s'appauvrit » ; celle-ci lui donne son revers.

**Pourquoi une deuxième interface plutôt que réutiliser `GroundCoverSource`.**
`groundCover` demande « reste-t-il du couvert ici ? » et sert à *raréfier*
l'herbe ; `clearing` demande « a-t-on ouvert ici ? » et sert à *faire pousser*
les ronces. Les deux se lisent aujourd'hui dans le même compteur, mais elles n'y
liront pas toujours la même chose : le jour où l'usure des sols par piétinement
dégradera le couvert, elle fera baisser `groundCover` sans ouvrir quoi que ce
soit — un sentier tassé ne fait pas pousser d'orties.

**Sans carte de couvert, la voie 1 est entièrement muette** — et c'est le
comportement voulu, pas un repli dégradé : sans carte, on ne sait pas où est la
lisière, et l'inventer par un substitut de pente donnerait des ronces sur des
critères qui n'ont rien à voir avec la lumière. Un témoin naturel, en prime : un
monde sans carte de couvert ne porte **que** les plantes que le joueur a fait
pousser.

Le semis est une **fonction déterministe de la cellule**, comme celui de
`Vegetation` : rien n'est stocké pour les 25 km². Seules les plantes **arrachées**
sont mémorisées, dans une table creuse, et seulement le temps de leur repousse
(420 s) — après quoi la même plante revient **à la même place et à la même
taille**, et sort de la mémoire. Touche `I`, portée 2,5 m.

### Les touches

Une rangée contiguë, dans l'ordre où on les apprend :

```
E abattre   R ramasser   T boire   Y creuser   U saigner   I arracher
```

Ce n'est pas une coquetterie. La rangée du haut est `A-Z-E-R-T-Y-U-I-O-P` en
AZERTY et `Q-W-E-R-T-Y-U-I-O-P` en QWERTY : **tout ce qui suit le Z/W y occupe la
même position physique**. GLFW raisonnant en positions physiques, c'est ce qui
évite le piège qui avait fait croire pendant tout un run que le jeu ignorait `W`.

### Géométries — `world/HarvestMesh.hpp/.cpp`

Quatre meshes d'objets au sol (motte, tas, perle, fagot) et un mesh de plante sur
pied, **tous générés en code**, aucun fichier. Quatre silhouettes et non un mesh
recoloré, parce que la philosophie « zéro menu virtuel » fait reposer toute la
lisibilité sur la forme : le joueur n'a aucune liste à l'écran pour distinguer
deux objets gris posés côte à côte.

---

## Vérifications

### Tests headless — `tests/logic_tests.cpp`

**1 095 vérifications passent** (935 avant cette phase, soit **+160**), et toutes
les anciennes continuent de passer : `Forest`, `Knapping`, `Habitat`/empreinte,
`Bike`, `Climber`, éditeur, besoins, faune.

Témoins négatifs, comme partout ailleurs dans ce projet :

* terrain plat, bosse **convexe partout**, versant à 40 % → aucun gisement ;
* cuvette dont le relief total est plafonné à 2 m → rejetée par `minUpslope`
  seul, et **retenue dès que ce seuil est abaissé** : c'est ce qui prouve que
  c'est bien ce critère-là qui avait décidé ;
* sans point d'eau → tout est du sable ; avec un point d'eau au fond → le même
  balayage rend les mêmes creux, seule leur **nature** change ;
* part de résineux nulle / pleine → aucun / tous ; graine différente → marquage
  réellement différent ; un refus de récolte ne mémorise **rien** ;
* alpage nu et futaie fermée → aucune plante ; sans carte de couvert et sans
  coupe → aucune plante ; paroi et altitude au-dessus de la limite des arbres →
  aucune plante, alors que la même configuration 50 m plus bas en produit.

**Trois tests ont dû être corrigés parce qu'ils avaient tort, pas le code** — et
c'est la mesure qui l'a montré :

* le **pied d'une bosse** était écrit comme un contrôle négatif. Mesure : deux
  gisements à 160 m du centre, 8,1 m de concavité, 26,8 m de versant au-dessus.
  C'est la définition même d'un cône de déjection, l'endroit le plus crédible de
  tout un relief pour trouver du sédiment. Le test est devenu un contrôle
  **positif**, avec ses assertions.
* le témoin de `minUpslope` plafonnait la cuvette à 40 m : le détecteur retenait
  alors trois points sur un anneau à 60 m du centre, où le flanc fournit 5,5 m
  d'amont. *Une cuvette n'est « sans amont » qu'à son centre, pas partout.*
* l'assertion de demi-hauteur exigeait l'égalité sur des meshes **bruités** : elle
  testait le tirage aléatoire, pas le contrat. Remplacée par un encadrement.

Un test **existant** a par ailleurs fait son travail : il a signalé que les
quatre nouvelles natures d'objet manquaient dans la table mesh-par-nature. Ces
`switch` ont un `return` par défaut, donc le compilateur, lui, n'aurait rien dit.

Deux défauts réels de géométrie ont été trouvés et corrigés par les tests : la
plante sur pied s'enfonçait d'un centimètre sous le sol (décalage de section
oublié, puis surcorrigé — elle flottait alors d'autant).

### Coût mesuré du semis de plantes à fibres

Chronométré au même titre que l'herbe et les arbres, parce qu'un poste de semis
non mesuré est exactement celui qu'on finit par optimiser à tort
(cf. NOTES-PHASE19.md). Sur la zone de 25 km², 10 486 images :

| poste | µs / image |
|---|---|
| arbres (`gatherTrees`) | 1 278,3 |
| `drawFrame` | 838,5 |
| herbe (`gatherGrass`) | 828,2 |
| **plantes à fibres (`gather`)** | **185,9** |
| streaming terrain | 63,2 |
| tronc de vision | 4,0 |

Le poste le moins cher des trois semis, d'un facteur quatre. Aucune optimisation
n'est justifiée à ce niveau, et le frame reste de toute façon limité par le GPU.

### Harnais Vulkan — `tools/run_vulkan_check.ps1`

**Dix scénarios (A à J), quatorze processus.** Zéro `[Validation]`, zéro
`[ERROR]`, codes de sortie tous nuls, fermeture propre partout.

**Scénario J ajouté, en trois processus.**

*Les gisements.* Le point de départ n'est **pas** choisi à la main — le script
relit les gisements que *le jeu* a journalisés au chargement, s'y rend par
`--start`, creuse, ramasse la motte, puis s'éloigne de 30 m et recreuse. Le refus
à distance est le contrôle négatif : sans lui, on ne distinguerait pas « la
portée marche » de « la touche produit toujours quelque chose ».

*Les plantes à fibres*, sur la **zone de 25 km²** — il a fallu changer de dalle,
et c'est le point intéressant. La dalle de référence du harnais n'a pas de carte
de couvert produite ; la voie « lisière » y est donc muette, et un scénario qui
s'y serait tenu aurait constaté un zéro **parfaitement correct** en le prenant
pour une couverture de test. Le point de départ a été trouvé **par mesure**, avec
la fonction de semis elle-même : balayage de la zone entière au pas de 50 m, à la
recherche des cellules de densité maximale portant des plantes à portée
d'arrachage. Il y en a **1 656** ; la meilleure est monde (1 450, −1 750),
altitude 1 777 m, couvert **0,240** — le milieu exact de la bande de lisière.

> Le point de départ *par défaut* de la zone est à **1 947 m**, au-dessus de
> `maxAltitude` (1 900 m). Mesuré : zéro plante, zéro touffe d'herbe. Le refus y
> est le comportement voulu — et un scénario posé là aurait été vert sans rien
> exercer. C'est exactement le piège que la mesure a évité.

Le scénario vérifie la chaîne entière : **295 plantes semées** par la lisière,
arrachage (`I`), le compte tombe à **294** et la plante passe **en repousse et
non disparue** — c'est ce qui distingue un semis déterministe d'un semis qu'on
aurait amputé —, puis ramassage du fagot (`R`). Les sept conditions comptent dans
le verdict. J3 est ignoré sans faire échouer le script si la zone n'est pas
produite, comme C à I le sont pour la dalle.


### Un critère du harnais qui se contredisait (scénario I, faune)

La suite complète est passée rouge **une fois** sur le scénario I : « animaux à
portée de vue » finissait à **2** là où le seuil n'en tolérait qu'**1**. Vérifié
avant de toucher quoi que ce soit — une régression serait reproductible :

| scénario I, **même binaire** | dernier relevé |
|---|---|
| en suite complète | 2 |
| en isolation, 3 fois | 1, 1, 1 |

C'est donc du bruit lié à la **charge de la machine** : le scénario compte des
relevés sur 240 s, et le nombre de tentatives d'apparition suit le débit
d'images. Ce n'est pas neuf — NOTES-PHASE18.md le consignait déjà : « rouge au
premier passage, vert au rejeu isolé ET au passage final […] c'est du bruit, pas
une régression. Le harnais gagnerait à rendre E et I insensibles au débit
d'images ».

Surtout, **le critère contredisait sa propre documentation** : le commentaire qui
le précède affirme depuis la Phase 17 qu'un résidu « d'un ou **deux** animaux à
la limite du rayon est le comportement CORRECT du modèle » — la pondération du
bruit vaut `1 - (d/r)²` et tend vers zéro au bord —, et le seuil, lui, n'en
tolérait qu'un.

Le plancher passe donc de 1 à 2. **Ce n'est pas un assouplissement de confort
pour rendre le run vert** : c'est la mise en accord du seuil avec le modèle qu'il
vérifie, mesure à l'appui, et l'assertion continue d'exiger que le voisinage
parte de ≥ 3 et finisse à 2 au plus (au tiers du départ dès qu'il dépasse 6). La
vraie correction — rendre le critère insensible au FPS — reste à faire.

---

## Ce qui n'est pas fait

* **Aucun usage fonctionnel.** Les quatre matières se récoltent et se ramassent ;
  rien ne se fabrique encore avec. C'était le périmètre demandé.
* **Les résineux n'ont pas de silhouette propre.** La dette principale de la
  tâche 1 (voir plus haut).
* **La résine n'est pas exercée sous Vulkan.** Elle demande de *viser* un arbre,
  or le harnais ne sait pas tourner la caméra et l'unique position de tir connue
  de la dalle de référence est déjà consommée par le scénario C. Elle est
  couverte par les tests headless, avec ses témoins. *(Les plantes à fibres, elles,
  le sont désormais — voir le scénario J3.)*
* **Rien n'est sauvegardé** : les entailles faites aux arbres et les plantes
  arrachées meurent avec le processus, comme la mémoire du milieu depuis la
  Phase 17.
* **Une seule espèce de plante à fibres.** Rien ne distingue une ronce d'une
  ortie : le mesh est unique, seule l'échelle varie.
