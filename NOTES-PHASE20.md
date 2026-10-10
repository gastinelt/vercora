# NOTES-PHASE20.md — La collision cesse d'être une affaire d'arbres

Session du 2026-08-26, à la suite de la Phase 19.

La consigne était d'étendre la collision « à tous les objets du monde qui n'en
ont pas encore ». L'inventaire réel du code a réduit ce périmètre à **une seule
lacune** — et c'est le premier résultat de la phase, avant toute ligne écrite.

---

## 1. L'inventaire, avant de coder

| Catégorie | État trouvé dans le code | Décision de la Phase 20 |
|---|---|---|
| Arbres, souches (`Forest`) | Collision depuis la **Phase 6** | Inchangé |
| Objets posés par l'éditeur (`EditorScene`) | Collision depuis la **Phase 15**, avec un drapeau `solid` **par objet** | Inchangé |
| **Faune (`Wildlife`)** | **Aucune collision** | **Corrigé** |
| Objets au sol (`Ground` : bûches, branches, pierres, éclats) | Aucune collision | **Laissé sans collision, délibérément** (§4) |

### 1.1 Les objets d'éditeur avaient déjà tout ce que la consigne demandait

La consigne proposait de « décider d'une approche raisonnable (ex : un flag
solide par asset ou par instance placée) ». Cette décision avait déjà été prise
en Phase 15, et dans le sens le plus fin des deux : `PlacedObject::solid` est un
drapeau **par instance**, réglable en jeu par la touche `K`, sérialisé dans le
format `.vcsc`. La raison donnée à l'époque tient toujours et vaut mieux que
celle qu'on aurait inventée aujourd'hui : le même rocher peut être un obstacle
sur un sentier et un simple décor à flanc de falaise, et surtout un asset dont
l'empreinte cylindrique est fausse (l'abri, qui a un intérieur) doit pouvoir
être posé sans emmurer le joueur.

**Rien n'a donc été touché de ce côté.** Le vérifier était le travail ; le
réécrire aurait été une régression déguisée en livraison.

### 1.2 L'architecture ne demandait aucune généralisation

La consigne demandait de « généraliser le système existant plutôt que d'écrire
un système parallèle ». La généralisation était déjà faite, en deux temps :

- **Phase 6** : `CollisionSolver` (interface) + `moveBody()` (point d'entrée
  unique, qui vérifie le résultat du repoussement et refuse le déplacement quand
  aucune position libre n'existe) ;
- **Phase 15** : `CompositeCollisionSolver`, qui implémente le même contrat et
  le délègue à une liste de sources.

Résultat : brancher la faune est **une ligne** dans `main.cpp`
(`worldSolver.add(wildlife.get())`), et cette ligne suffit pour **les trois
activités qui se déplacent au sol**. C'est la deuxième fois que l'interface de
la Phase 6 tient sa promesse, et cette fois sans qu'on ait eu à l'aménager.

---

## 2. Ce qui a été écrit

`Wildlife` implémente désormais `CollisionSolver`, exactement comme `Forest` et
`EditorScene`.

- **Empreinte** : cylindre vertical, rayon `0,65 m` = **demi-longueur du mesh de
  cervide** (`kAnimalLength / 2`), multiplié par `Animal::scale` ; bande
  verticale `[sabots ; sabots + 1,05 m × échelle]`.
- **Géométrie** : celle de `Forest::resolve` — cercles dans le plan XZ,
  recouvrement d'intervalles en Y, repoussement jusqu'au contact, 4 passes avec
  sortie anticipée dès qu'une passe ne touche rien. **Aucun second modèle de
  collision n'a été écrit** : le monde doit se cogner partout de la même façon,
  sans quoi il y aurait deux comportements à régler, à tester et à garder
  d'accord.
- **Le lien mesh ↔ collision est tenu par une assertion de COMPILATION.**
  `WildlifeParams` ne peut pas inclure `AnimalMesh.hpp` sans traîner tout le
  moteur derrière tout ce qui paramètre la faune, donc `0,65` y est écrit en
  clair. Un `static_assert` dans `Wildlife.cpp` vérifie que cette valeur reste
  la demi-longueur du mesh : le jour où le cervide changera de taille, ça
  échouera à la compilation au lieu de le rendre traversable par le flanc.

### 2.1 Pourquoi le cylindre CONTENANT, et ce qu'il coûte

Même convention que l'empreinte des objets d'éditeur, et pour la même raison :
un collider plus petit que l'objet se traverse par les coins, ce qui est
exactement le défaut qu'on vient corriger.

**Contrepartie assumée, et elle est réelle** : de face, le cervide ne fait que
0,42 m de large ; son cylindre de 0,65 m est donc trop généreux d'une vingtaine
de centimètres de chaque côté. Un cylindre ne peut pas être juste dans les deux
sens, et l'erreur qui bloque un peu tôt est moins choquante que celle qui laisse
passer au travers d'un flanc.

### 2.2 Ce qui reste FAUX, et qui l'est volontairement

**Un animal, lui, traverse toujours les troncs, les rochers posés et ses
congénères.** Ce n'est pas un oubli : c'est le corollaire du « aucune IA » de la
Phase 17. La fuite est une ligne droite, sans évitement d'obstacle — un animal
qui buterait sur un tronc y resterait **collé**, faute de savoir le contourner,
et un cervide encastré dans un arbre serait plus laid que le problème résolu.
Le sens qui compte pour le joueur est celui qui a été traité.

### 2.3 Un animal n'est pas un tronc : il bouge

Deux conséquences, toutes deux assumées et toutes deux déjà couvertes par
`moveBody()` :

- un animal peut venir se poser **sur** un joueur immobile — le joueur ne bouge
  pas, donc aucun `moveBody()` n'est appelé, donc rien ne l'écarte. Il sera
  repoussé au premier pas : c'est le cas dégénéré que `moveBody()` traite déjà
  en dernier recours pour « un arbre planté sur le joueur » ;
- la position « précédente » sur laquelle `moveBody()` se replie a pu devenir
  occupée entre deux images. Le repli la teste avant de s'y poser, il n'y avait
  donc rien de particulier à écrire.

---

## 3. LA MESURE : témoin négatif et chiffre prédit

Méthode de la Phase 15 : deux mondes **identiques à une option près**, même
graine, même semis, même animal à la même place. La seule différence est la
collision.

Le témoin est `WildlifeParams::solid = false`, exposé en jeu par la nouvelle
option **`--no-animal-collision`**. C'est le témoin qui compte, et
`--no-wildlife` ne pouvait pas le remplacer : supprimer les animaux ne montre
pas qu'on traverse celui qui est là.

Marche de 20 m, droit sur un cervide isolé, par le même `moveBody()` que la
caméra :

| | approche la plus proche | distance parcourue |
|---|---:|---:|
| Témoin, `--no-animal-collision` | **0,0010 m** (le centre de l'animal) | **20,00 m** |
| Avec collision | **1,0418 m** | 13,20 m |

Et **1,0418 m est le chiffre prédit** : rayon de collision de cet animal
(`0,65 × échelle`) + demi-largeur du joueur (`0,35`). Il est atteint **au
millimètre près**. C'est ce qui sépare « ça a l'air de bloquer » de « ça bloque
là où le modèle le dit ».

### 3.1 La première version de ce test avait tort, et la mesure l'a dit

Le test attendait un **arrêt net** à `10 − 1,0418 = 8,96 m`. Mesure : **13,20 m
parcourus**, et une sortie de l'autre côté de l'animal.

Ce n'est pas un défaut, c'est le **glissement** que le solveur par repoussement
produit exprès (`CollisionSolver.hpp` : « on longe un tronc au lieu de s'y
coller net »). Contre un obstacle convexe **isolé**, une poussée frontale n'est
jamais parfaitement frontale à l'arrondi près, et la composante latérale
accumulée finit par faire contourner l'animal — comme on contournerait un tronc.

La grandeur réellement **prédite par le modèle** n'est donc pas la distance
d'arrêt, c'est le **rayon de contact**. C'est elle qui est vérifiée. La leçon
est celle des phases précédentes, un cran plus loin : *un chiffre attendu qui ne
sort pas n'est pas forcément un bug — il faut d'abord savoir lequel des deux,
du code ou de l'attente, décrit le modèle.*

*(La toute première version échouait pour une raison encore différente et plus
bête : avec la population par défaut — 24 animaux espacés d'au moins 14 m — un
trajet de 20 m en croise parfois un SECOND, et la marche s'arrêtait sur
celui-là. La collision marchait ; c'est la distance prédite qui parlait du
mauvais animal. Le test sème désormais `maxAnimals = 1`.)*

### 3.2 Toutes les activités, pas seulement la marche

C'était une exigence explicite de la consigne. Réponse par activité :

- **Marche** (`Camera::resolveCollisions`) : mesurée ci-dessus.
- **Vélo** (`Bike::update`) : **mesuré à part**, et c'est le cas qui compte le
  plus. Le vélo roule à 30 km/h, c'est-à-dire à la vitesse de **fuite** de
  l'animal (8 m/s) : c'est la seule activité qui peut réellement rattraper un
  cervide et lui rentrer dedans. À pied, on ne l'approche jamais — il part à
  30 m. Résultat : le témoin passe à moins de 0,5 m du centre, le cycliste avec
  collision reste au-delà du rayon de contact.
- **Escalade** (`Climber::update`) : concernée au **pied** de la paroi, jamais
  dessus, et pour une raison de MODÈLE et non de code — `WildlifeParams::
  maxSlope` (0,90) est inférieur à `ClimberParams::wallSlope` (1,00), donc
  **aucun animal ne peut se tenir sur une pente assez raide pour être
  grimpable**. C'est le même genre de rapport entre deux constantes réglées
  séparément que la leçon de la Phase 17 (`noiseRadius` vs `spawnRadiusMin`),
  sauf qu'ici le rapport est celui qu'on veut : une assertion le fige, pour
  qu'on ne le perde pas en retouchant l'une des deux. Un second test vérifie la
  bande verticale : un grimpeur à 2,4 m au-dessus des sabots n'est pas gêné.
- **Parapente** : **hors périmètre, inchangé, et cohérent avec la Phase 8**, qui
  documente déjà « pas de collision avec les arbres pendant le vol ». Le
  parapente ne consomme aucun `CollisionSolver` : lui en donner un serait une
  phase à part (un vol qui se cogne demande de décider ce qu'est un crash). À
  l'atterrissage, le joueur repasse en mode `Walk` et retrouve la collision
  complète, faune comprise.

**Aucune de ces trois classes n'a été modifiée.** Elles reçoivent toutes le même
`worldSolver`.

---

## 4. DÉCISION : les objets au sol ne bloquent pas

`Ground` (bûches, branches, pierres, éclats) n'implémente pas `CollisionSolver`
et n'entre pas dans le solveur composite. **C'est une décision, pas un oubli**,
et elle repose sur trois faits mesurables :

1. **Ce sont des cibles de RAMASSAGE.** `Ground::findNearest` exige de s'en
   approcher. Un collider solide ferait lutter le joueur contre la mécanique
   même qui donne leur raison d'être à ces objets.
2. **Ils sont sous le genou.** Le plus haut est la pierre : `kStoneRadius` vaut
   0,20 m, donc 0,40 m de haut posée. Un joueur enjambe ça ; le simuler
   demanderait une gestion de marche/step-up qui n'existe pas dans le moteur.
3. **Un débitage en dépose 3 à 4 autour du joueur** (`Forest::scheduleBucking`).
   Les rendre solides pourrait l'emmurer dans le bois qu'il vient de produire —
   et le solveur n'a aucun moyen de l'en sortir, puisque `moveBody()` refuse
   simplement le déplacement quand aucune position libre n'existe.

Le test headless fige cette décision plutôt que de la laisser au commentaire :
il pose une pierre et une bûche sur le trajet et vérifie que le passage reste
libre, en nommant la phase qui a tranché.

**Ce qui rendrait la décision fausse un jour** : un objet au sol nettement plus
gros (un rocher non ramassable, un tronc entier non débité). Le jour où
`GroundItemKind` en accueillera un, c'est un drapeau `solide` par nature d'objet
qu'il faudra — pas un collider pour toute la catégorie.

---

## 5. Vérification

### 5.1 Tests headless

**935 assertions** (902 avant la phase, **+33**), toutes passent, zéro
avertissement à `/W4`. Nouvelle section « Phase 20 : collision de la faune ».

Ce qu'elle couvre :

- le témoin négatif et le chiffre prédit (§3) ;
- cohérence `resolve` / `overlaps` : la sortie de `resolve()` est **libre**,
  sans quoi `moveBody()` refuserait tout déplacement le long d'un animal au lieu
  de laisser glisser (c'est la raison de la marge d'un millimètre, la même que
  dans `Forest` et `EditorScene`) ;
- le cas dégénéré « joueur exactement sur l'axe de l'animal » : aucun NaN, écarté
  au contact ;
- la bande verticale (grimpeur au-dessus, joueur au sol) ;
- le rayon suit `Animal::scale`, et le défaut est bien la demi-longueur du mesh ;
- le **solveur composite à trois sources** : le tronc bloque toujours (Phase 6),
  le rocher posé bloque toujours (Phase 15), l'animal bloque désormais
  (Phase 20), et le passage libre le reste ;
- la décision du §4 (objets au sol traversables) ;
- le vélo et l'invariant d'escalade (§3.2) ;
- l'option `--no-animal-collision` côté interface (`overlaps` faux, `resolve`
  rend la position telle quelle).

### 5.2 Build

`tools\build.bat Debug` : configuration et compilation propres, aucun
avertissement neuf. Trois unités recompilées (`Wildlife.cpp`,
`TerrainStreamer.cpp`, `main.cpp`) et l'édition de liens.

### 5.3 Vulkan

Voir §6.

### 5.4 Synchronisation

**Pas de `VK_LAYER_VALIDATE_SYNC=1` cette fois, et c'est justifié plutôt
qu'omis** : cette phase ne touche **aucun objet Vulkan, aucun thread, aucune
barrière, aucun téléversement**. Les trois fichiers modifiés sont
`world/Wildlife.{hpp,cpp}` (géométrie pure, GLM seul) et le câblage dans
`main.cpp`. Le contrôle de synchronisation de la Phase 19 portait sur le chemin
de téléversement des tuiles, qui est rigoureusement inchangé. Le relancer aurait
produit un journal vide sans rien prouver de neuf — et la Phase 12 a établi
qu'un journal propre ne prouve rien tant qu'on n'a pas montré que l'outil
regarde au bon endroit.

---

## 6. Résultat du harnais `run_vulkan_check.ps1`

Joué **en entier** (scénarios A à I) sur le binaire final, puis le scénario B
rejoué seul.

**L'essentiel : zéro `[Validation]`, zéro `[ERROR]`** sur 5 510 lignes de
journal. Les 4 `[WARN]` sont les avertissements bénins habituels (pas de point
d'eau ni de faune sur certaines dalles) ; le harnais ne les compte pas comme des
erreurs.

### 6.1 Non-régression : les systèmes des Phases 15 à 19 sont intacts

| Vérification | Résultat |
|---|---|
| **Éditeur (G), collision des objets posés** | **0,28 m avec la scène contre 21,03 m sans (témoin)** — la mesure de la Phase 15 est reproduite à l'identique |
| Éditeur : posés / supprimés / annulations / F5 / F9 / rechargés en jeu normal | 3 / 1 / 1 / 3 / 3 / 3 objets dont 3 solides |
| Sélection sans viser (F3/F4) | 3 sélections pour 3 touches envoyées |
| **Faune (I), Phase 17** | 24 animaux, fréquentation 100 %, **4 animaux à portée au premier relevé → 1 au dernier** — la zone se vide toujours |
| Besoins (H), Phase 16 | 2 points d'eau détectés, boire au point d'eau `True`, **refus à 30 m `True`** (témoin) |
| Escalade (F), Phase 14 | sommet atteint, +41,6 m, pente 63 à 154 % |
| Parapente (D), Phase 8 | décollage et atterrissage naturel |
| Dalle LiDAR réelle (C), Phase 7 | chargée, arbre visé et abattu, 5 objets ramassés |
| Streaming / LOD, Phases 10-12 et 19 | aucune erreur, fermeture propre partout |

**Le point le plus important pour cette phase est la première ligne** : la
collision des objets d'éditeur, mesurée par témoin, rend exactement le même
chiffre qu'avant. Ajouter une source au `CompositeCollisionSolver` n'a rien
changé aux deux autres.

### 6.2 Les deux points rouges, et ce qu'ils sont

1. **Scénario E (vélo), « pied à terre » `False`.** Échec **antérieur à cette
   phase**, documenté depuis la Phase 17 puis dans les NOTES des Phases 18 et
   19, laissé non corrigé pour cause de périmètre. La Phase 20 ne l'a ni causé
   ni aggravé : le reste du scénario E est vert (en selle `True`, 32,3 km/h en
   descente, 11 relevés à l'arrêt en côte au-delà de 23,6 % de pente).

2. **Scénario B, code de sortie −1, « l'arbre 7 n'a pas été abattu ».** Celui-là
   ne pouvait pas être classé sans vérification, parce que le scénario B **est**
   le chemin de collision : c'est précisément ce que cette phase touche.
   **Rejoué seul immédiatement après : vert** — code de sortie 0, fermeture
   propre, « tronc debout PUIS souche poussés, sans erreur ni plantage », zéro
   `[Validation]`. C'est la flakiness `PostMessage` déjà documentée dans
   l'en-tête du script (scancodes, pas de vraie souris : cible d'abattage
   manquée de peu ou `WM_CLOSE` lent), et le protocole du projet pour ce cas est
   justement de rejouer. **Pas une régression.**

---

## 7. Options ajoutées

| Ajout | Rôle |
|---|---|
| `--no-animal-collision` | La faune reste **visible** mais redevient **traversable** : comportement exact d'avant la Phase 20. **Témoin de mesure**, au même titre que `--no-scene` pour l'éditeur. `--no-wildlife` ne peut pas le remplacer — supprimer les animaux ne montre pas qu'on traverse celui qui est là. |
| Journal `Faune : collision ACTIVE/DÉSACTIVÉE` | Une ligne au démarrage, qui dit laquelle des deux configurations tourne et avec quel rayon. Sans elle, un run de mesure ne peut pas prouver **après coup** de quel côté du témoin il était. |

---

## 8. Ce qui n'est pas fait

1. **La faune ne se cogne à rien.** Voir §2.2 : c'est le corollaire de l'absence
   d'IA, et le lever demande un évitement d'obstacle, donc un vrai déplacement,
   donc une session à part.
2. **Le cylindre reste faux de face** (§2.1). Un collider orienté (capsule
   couchée le long du cap de l'animal) serait juste dans les deux sens — c'est
   la même limite que celle notée en Phase 15 pour les objets à intérieur, et la
   même réponse : un volume composé plutôt qu'un cylindre englobant.
3. **Aucune conséquence de gameplay au contact.** Bousculer un cervide ne le
   fait pas fuir plus vite, ne blesse personne, ne coûte rien. La collision est
   physique, pas narrative.
4. **Aucun scénario Vulkan dédié** (un « scénario J »). Le harnais ne pilote que
   le clavier et la faune est **semée aléatoirement autour du joueur** : un
   scénario reproductible demanderait de pouvoir poser un animal à une position
   choisie, c'est-à-dire une entrée de débogage dans `Wildlife`. La preuve de
   collision est donc **headless et déterministe** — ce qui est aussi le seul
   endroit où le témoin négatif est rigoureux. Le harnais complet a servi à la
   **non-régression**, pas à la mesure de cette phase.
5. **Pas de mesure de coût, et l'argument est structurel, pas mesuré.**
   `resolve`/`overlaps` balaient au plus `maxAnimals` = 24 cylindres par appel
   de déplacement, contre plusieurs centaines d'arbres pour `Forest` sur le même
   chemin : le poste ajouté est plus petit d'un ordre de grandeur qu'un poste
   déjà présent, et la Phase 19 a établi que l'image est **limitée par le GPU**
   (81 % du temps passé à attendre dans `drawFrame`). Chiffrer ce poste
   demanderait une instrumentation neuve pour mesurer une fraction d'un temps
   d'attente. C'est une justification, pas une mesure : si la population de
   faune devait un jour croître d'un ordre de grandeur, il faudrait le chiffrer
   pour de bon.
