# NOTES — Phase 11 : habillage visuel du terrain reel

Session du 2026-08-21 (la meme journee que les Phases 9 et 10). Objet : donner
une COULEUR au terrain LiDAR, qui n'en avait aucune, puis l'habiller de
vegetation a l'echelle des 25 km2 reels.

Le point de depart est la Phase 10 : quadtree CDLOD, 487 FPS sur la zone au pas
de 1 m, et un terrain colore par une rampe altitude/pente codee dans le fragment
shader. Aucune texture n'existait dans le moteur — ni image, ni echantillonneur,
ni binding de descriptor. La Phase 11 a donc du poser tout le chemin de texture
avant de pouvoir afficher le premier texel de photo aerienne.

---

## Resume des chiffres

Toutes les mesures : Intel UHD 770, build **Debug avec couches de validation**,
1280x720, `tools\measure_terrain.ps1`, camera VERROUILLEE (voir plus bas).
**Zero message `[Validation]` sur la totalite des mesures de ce document.**

### Zone 25 km2 au pas de 1 m, position de depart par defaut

| Etat | FPS median | Pic RAM |
|---|---:|---:|
| Phase 10 reproduite (`--no-ortho --no-vegetation`) | **514** | 240 Mo |
| Phase 11 complete (orthophoto + arbres + herbe) | **271** | 332 Mo |

Le repere de la Phase 10 inscrit dans MEMORY.md etait 487 FPS / 246 Mo ; les
514 FPS / 240 Mo mesures ici sont le MEME chemin de code, l'ecart venant du
verrouillage de la camera (voir « captures reproductibles »). Le chemin Phase 10
n'a donc pas regresse.

### Cout de chaque brique, meme vue (E 901500 N 6436600, yaw 100, pitch -7)

| Etat | FPS median | Pic RAM |
|---|---:|---:|
| Terrain seul (Phase 10) | 658 | 231 Mo |
| + orthophoto | 637 | 332 Mo |
| + arbres de decor | 466 | 332 Mo |
| + herbe | 283 | 332 Mo |

**L'orthophoto coute 3 % de FPS et 101 Mo de RAM.** C'est le meilleur rapport
de la session : le gain visuel est enorme et le prix en images par seconde
quasi nul. Les 101 Mo sont la texture (48 Mo cote GPU) plus le tampon de
transfert transitoire.

**Les arbres coutent 26 %, l'herbe 39 % de plus.** Voir « ce que coute
reellement la vegetation » ci-dessous : ce n'etait PAS la geometrie.

### Autres scenes — non-regression

| Scene | Phase 10 | Phase 11 `--no-vegetation` | Phase 11 complete |
|---|---:|---:|---:|
| Terrain procedural | 1 244 FPS | **1 348 FPS** | 258 FPS |
| Dalle Phase 7 (1 km) | 847 FPS | **884 FPS** | 212 FPS |
| Cube | 3 403 FPS | — | **3 593 FPS** |

Le chemin de la Phase 10 est intact et meme legerement plus rapide. La baisse
en « Phase 11 complete » sur ces deux scenes n'est pas une regression : c'est le
COUT DE LA VEGETATION, qui y est semee par un substitut (ces scenes n'ont pas de
carte LiDAR de couvert) et qui y est proportionnellement tres visible faute
d'autre chose a dessiner. `--no-vegetation` rend exactement les anciens chiffres.

---

## TACHE 1 — Orthophoto BD ORTHO drapee sur le relief

### Provenance et telechargement

`tools/fetch_ortho.py`. Service **WMS raster de la Geoplateforme**
(`https://data.geopf.fr/wms-r/wms`), couche `HR.ORTHOIMAGERY.ORTHOPHOTOS`
(intitulee « Ortho 20 cm », BD ORTHO V3), licence Etalab — le meme portail et la
meme licence que le LiDAR HD de la Phase 9.

Le WMS a ete prefere au telechargement des dalles BD ORTHO livrees : il accepte
une bbox arbitraire en Lambert 93, donc il donne EXACTEMENT l'emprise du terrain
sans avoir a recouper des dalles dont le decoupage n'est pas celui du LiDAR.

Trois contraintes du service, verifiees et non supposees :

1. **Le serveur ne parle que WMS 1.3.0.** Une requete 1.1.1 est rejetee
   (`VersionNegotiationFailed`).
2. **L'ordre des axes en EPSG:2154 est EST, NORD.** C'est le piege classique de
   WMS 1.3.0, ou l'ordre depend du CRS. Tranche par l'experience et non par la
   lecture de la norme : `BBOX=minE,minN,maxE,maxN` renvoie l'imagerie attendue,
   l'ordre inverse renvoie une image **BLANCHE UNIFORME**. Le controle est
   rejoue a chaque execution (`--self-test`), pour qu'un changement de
   comportement du serveur se voie tout de suite et pas sur une texture en
   miroir.
3. **MaxWidth = MaxHeight = 5010 px**, d'ou le decoupage en tuiles.

Resultat : **25 tuiles de 1 km a 0,5 m/px (2000 px), 23,8 Mo en 101 s**, aucun
echec. Le decoupage kilometrique est aligne sur celui des dalles LiDAR, ce qui
rend les deux jeux de donnees directement comparables.

L'emprise est **lue dans l'en-tete du .vchm**, jamais passee en argument. Une
bbox recopiee a la main derive au premier changement de zone, et une orthophoto
decalee de quelques metres est exactement le genre de defaut qu'on ne voit pas
sur une capture mais qui met les arbres a cote des arbres.

Emprise obtenue : **E 899 000 – 904 000, N 6 435 000 – 6 440 000** (5 000 m de
cote), identique au terrain assemble en Phase 9.

### Alignement texture / terrain

C'est le point ou une erreur passe inapercue, donc celui qui est le plus
detaille.

**Difference de nature entre les deux grilles.** La heightmap est un semis de
NOEUDS : le noeud `ix = 0` est exactement a E 899 000. L'orthophoto est une
grille de PIXELS-SURFACES : le pixel 0 COUVRE `[E0, E0+res]` et son centre est a
`E0 + res/2`. La bbox demandee au WMS est donc `[E0, E1] x [N0, N1]` : l'image
couvre exactement le carre delimite par les noeuds extremes, bord a bord.

**Le demi-texel residuel est assume.** Echantillonner a `u = 0` vise le BORD du
premier texel et non son centre ; l'ecart vaut `res/2`, soit 30 cm au pas de la
texture finale. C'est en dessous de la precision de la BD ORTHO elle-meme et
sans commune mesure avec le pas du terrain.

**Les UV n'ont rien coute.** `TerrainNodeMesh` emettait deja, depuis la Phase 10,
`uv = (gx/cells, gz/cells)` sur le terrain ENTIER et non sur la tuile — un choix
fait a l'epoque « pour une future orthophoto ». Consequence : l'UV est continu
d'une tuile de quadtree a l'autre, independant du niveau de detail, et
`gz = 0` correspond au NORD, c'est-a-dire a la ligne 0 de l'image, c'est-a-dire
a `v = 0`. **Aucun retournement n'a ete necessaire.**

**L'UV n'est PAS morphe** (`shaders/terrain.vert`). La cible de geomorphing d'un
sommet est un autre noeud de la meme grille, qui porte donc naturellement l'UV
de sa propre position. Interpoler entre les deux ferait glisser la photo par
rapport au relief pendant une transition de niveau.

**Verification de coherence a l'execution.** `orthoMatchesTerrain()` dans
`main.cpp` compare l'emprise inscrite dans le `.vcto` a celle de la heightmap
chargee, a 0,5 m pres, et REFUSE une texture qui ne correspond pas. Sans ce
controle, associer par megarde la photo d'une zone au relief d'une autre produit
un rendu parfaitement plausible que rien ne signale.

### Deux controles independants de l'alignement

1. **Photo contre relief ombre.** La mosaique et un ombrage du relief de la meme
   emprise, rendus a la meme taille, superposent falaises, ravines et
   escarpements. C'est le controle que la Phase 9 disait explicitement ne pas
   pouvoir faire « faute d'orthophoto a plaquer » : il est fait, et il confirme
   l'orientation nord-en-haut du `.vchm`.
2. **Classification LiDAR contre couleur de la photo** (voir tache 2) :
   correlation couvert / luminance de **-0,729**. Les deux jeux de donnees sont
   georeferences par des chaines completement independantes ; leur accord vaut
   validation croisee.

### Format `.vcto` et compression

Nouveau format maison, meme esprit que le `.vchm` : en-tete de 128 octets
(magie, version, dimensions, nombre de mipmaps, format, emprise Lambert 93 en
double, EPSG, libelle) puis les niveaux de mipmap consecutifs. La definition de
reference est en tete de `tools/make_ortho_texture.py`, le chargeur est
`src/assets/TextureFile.cpp` (aucune dependance Vulkan, donc teste sans GPU).

**Pourquoi les mipmaps sont PRECALCULEES.** Le chemin habituel — les generer sur
le GPU par `vkCmdBlitImage` en cascade — est inapplicable a un format compresse :
on ne sait pas filtrer des blocs BC1. Il fallait donc soit renoncer a la
compression, soit reduire PUIS compresser chaque niveau hors ligne. C'est le
second choix, et il rend le code de chargement plus court, pas plus long : un
`vkCmdCopyBufferToImage` par niveau, sans transition intermediaire.

**Pourquoi BC1.** A 8192 px de cote sur 5 km :

| | Niveau 0 | Chaine complete |
|---|---:|---:|
| RGBA8 non compresse | 268 Mo | 357 Mo |
| **BC1 (6:1)** | **32 Mo** | **44,7 Mo** |

Sur un GPU integre dont la memoire est prise sur la RAM systeme, et apres une
Phase 10 qui avait ramene l'empreinte a 246 Mo, la difference n'est pas
cosmetique. Le support de BC1 est **verifie** (`vkGetPhysicalDeviceFormatProperties`)
et le refus est explicite, avec le repli nomme (`--format rgba`), plutot qu'un
message de driver.

**Encodeur BC1 maison** (numpy, vectorise, par paquets de 262 144 blocs) : boite
englobante avec retrait d'un seizieme, le chemin rapide de stb_dxt. Qualite
mesuree et AFFICHEE a chaque encodage : **PSNR 33,2 dB** sur le niveau 0, mesure
par un decodeur ecrit independamment qui repart des octets produits — donc
capable d'attraper une erreur d'ordre des bits qu'un aller-retour interne
masquerait.

> **Bug trouve par cette mesure.** Le premier encodage donnait 22,0 dB. Cause :
> la matrice des distances au carre etait accumulee en `int16` par `np.einsum`,
> alors qu'une distance monte a `3 x 255^2 = 195 075`. Le debordement etait
> silencieux, l'`argmin` choisissait une couleur au hasard, et rien n'echouait.
> Sans le PSNR affiche systematiquement, la texture serait partie en production
> avec un quart de sa qualite.

### Resolution : le compromis, et sa limite

- Source BD ORTHO : **20 cm** natif.
- Telecharge : **50 cm** (10 000 x 10 000 px, 23,8 Mo).
- Texture GPU : **8192 x 8192, soit 0,61 m/texel**.

Le 8192 est choisi pour rester proche du pas de la source (0,5 m) sans gaspiller :
descendre a 20 cm aurait demande 25 000 px de cote, soit 625 Mpx, ce qui n'a de
sens qu'avec un systeme de texture virtuelle qui n'est pas au programme.

**Limite assumee, visible.** A 0,61 m/texel, un texel couvre environ 150 pixels
d'ecran quand on regarde le sol a 3 m devant soi : la photo y est etiree et
floue. L'orthophoto est excellente a moyenne et longue distance et molle sous les
pieds. Le remede standard est une texture de detail melangee de pres selon la
pente et l'altitude ; ce n'est pas fait dans cette phase.

### Integration au moteur

- `src/renderer/Texture.{hpp,cpp}` : image, memoire, vue, echantillonneur.
  **Anisotropie demandee au maximum du GPU**, ce qui compte particulierement ici
  (un plan regarde en rasant depuis la hauteur d'oeil). La feature
  `samplerAnisotropy` est OPTIONNELLE en Vulkan : elle est activee sur le device
  logique si le GPU la propose (`VulkanContext::anisotropyEnabled()`), et
  abandonnee proprement sinon — la demander sans l'avoir activee est une erreur
  de validation immediate.
- `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE`, jamais REPEAT : la texture recouvre
  exactement l'emprise, et un sommet legerement hors bornes (l'ecretage de la
  heightmap en produit) irait sinon chercher la couleur du bord oppose,
  plusieurs kilometres plus loin.
- `VK_FORMAT_BC1_RGB_UNORM_BLOCK` et non la variante `_SRGB` : la swapchain est
  en UNORM et l'eclairage est calcule tel quel depuis la Phase 2 ; une variante
  sRGB ferait subir a la seule orthophoto une conversion que rien d'autre
  n'applique.
- **Binding 1 du descriptor set, declare pour TOUS les materiaux**, y compris
  ceux qui ne l'echantillonnent pas. Un descriptor present mais non lu est
  legal ; deux layouts differents auraient impose deux jeux de pipelines et deux
  pools. Le binding n'est jamais vide : une texture **1x1 blanche** l'occupe tant
  qu'aucune photo n'est chargee.
- **Repli conserve.** `ubo.terrain.z` porte le dosage orthophoto / coloration
  procedurale. Touche **O** en jeu, `--no-ortho` en ligne de commande. Le dosage
  est force a zero quand aucune texture n'est reliee, sinon la bascule
  afficherait un terrain uniformement blanc au lieu du repli procedural.
- **L'eclairage continue de s'appliquer par-dessus la photo.** Une orthophoto est
  prise a la verticale par temps clair, donc deja a peu pres uniformement
  eclairee ; la relancer dans le Blinn-Phong avec la normale du relief est ce qui
  redonne du volume aux versants qu'une photo plaquee telle quelle aplatit.

---

## TACHE 2 — Arbres a l'echelle des 25 km2

### La classification LiDAR est exploitable, et elle est excellente

C'etait la question ouverte. Reponse : oui, sans reserve.

`tools/lidar_to_canopy.py` depouille les 25 dalles (**150 s au total**, ~4 s par
dalle) et produit deux cartes. Repartition mesuree des classes ASPRS sur une
dalle representative :

| Classe | | Part |
|---:|---|---:|
| 2 | sol | 89,9 % |
| 5 | vegetation haute | 4,7 % |
| 1 | non classe | 2,8 % |
| 3 | vegetation basse | 1,9 % |
| 4 | vegetation moyenne | 0,7 % |
| 6, 64 | bati, bruit | < 0,1 % |

**Deux grandeurs, deux fichiers**, parce que ce sont deux questions distinctes :

- **Couvert** (`vercora_zone_canopy.vchm`) : part des retours de vegetation
  (classes 3, 4, 5) parmi tous les retours valides de la cellule. Une FRACTION,
  donc insensible a la densite de vol, contrairement a un comptage brut. Dit
  **ou** semer.
- **Hauteur de canopee** (`vercora_zone_canopy_height.vchm`) : altitude du plus
  haut retour de classe 5 moins celle du plus bas retour de sol, dans la meme
  cellule. Dit **quelle taille** donner aux arbres.

Grille : **1001 x 1001 au pas de 5 m**, exactement la meme origine et la meme
emprise que le terrain — seul le pas differe, donc un point du monde s'y lit avec
la meme formule, sans demi-cellule a corriger.

Le maximum plutot qu'un percentile pour la hauteur : il se calcule en un seul
passage sans rien stocker, et sur une cellule de 5 m il designe le plus grand
arbre de la cellule, ce qui est exactement la grandeur voulue. Les classes de
bruit sont exclues et la valeur est ecretee a 45 m.

**Reutilisation du format `.vchm`, assumee et justifiee.** Un `.vchm` est une
GRILLE DE FLOTTANTS GEOREFERENCEE ; rien dans le format n'impose que le flottant
soit une altitude. Le moteur relit donc ces cartes avec `loadTerrainFile()` sans
une ligne de code nouvelle, et herite gratuitement de l'interpolation bilineaire
de `heightAt()` et de la conversion monde <-> Lambert 93.

Statistiques obtenues : couvert moyen **0,418**, **58 %** des cellules au-dessus
de 0,15 ; canopee moyenne **15,5 m** sur les cellules boisees, **55,6 %** des
cellules a 3 m ou plus ; **0,05 %** de cellules sans aucun retour.

### Validation de la carte : le test decisif

Une carte de couvert plausible mais fausse (miroir, decalage) serait
indetectable a l'oeil. Elle a donc ete confrontee a l'orthophoto, qui est une
source totalement independante :

| | RVB moyen sur la photo | Luminance |
|---|---|---:|
| Cellules tres couvertes (couvert > 0,70 ; 34,8 % de la zone) | (67, 75, 72) — **vert sombre** | 71 |
| Cellules nues (couvert < 0,05 ; 33,1 % de la zone) | (162, 162, 153) — **gris clair** | 159 |

**Correlation couvert / luminance : -0,729.** Plus il y a de vegetation dans le
LiDAR, plus la photo est sombre — exactement ce qu'on attend d'une pessiere sur
du calcaire. L'indice d'exces de vert, lui, ne correle presque pas (+0,079) :
sur ce massif la foret de coniferes est sombre mais peu saturee, et c'est la
LUMINANCE qui discrimine. Ce detail a son importance pour qui refera le controle.

### Architecture : deux systemes de vegetation, pas un

`world/Vegetation.{hpp,cpp}`, a cote de `world/Forest` qui reste inchange.

| | `Forest` (Phases 5-6) | `Vegetation` (Phase 11) |
|---|---|---|
| Nombre | 16 | dizaines de milliers |
| Etat | debout / en chute / en debitage | aucun |
| Interaction | abattage, collision, visee | decor pur |
| Stockage | vecteur, parcouru en entier | **rien** |
| Rendu | un DrawItem par arbre | **une instance par arbre** |

Les fusionner obligerait a payer partout le prix du plus exigeant : `resolve()`
et `pickTree()` de `Forest` parcourent le vecteur en entier a chaque appel, ce
qui est parfait pour seize arbres et intenable pour cinquante mille.

**Aucun arbre n'est stocke pour les 25 km2.** Le semis est une FONCTION
DETERMINISTE de la cellule, obtenue par HACHAGE et non par un generateur a etat.
Un `std::mt19937` imposerait un ordre d'appel : la cellule (17, -42) ne donnerait
le meme resultat que si toutes les precedentes avaient ete tirees dans le meme
ordre — faux des que le joueur arrive par un autre chemin. Le hachage ne depend
que de ses entrees. **C'est teste** (« semis independant du chemin parcouru »).

### Placement

Cellules de **24 m**, **8 emplacements** par cellule. Pour chaque emplacement :

1. **Densite par le couvert reel** : `ramp(couvert, 0,20, 0,75)`, et
   l'emplacement `k` n'est retenu que si `k < densite x 8`. Comparer l'indice a
   un seuil plutot que tirer un de a chaque emplacement garantit que les
   emplacements sont retenus **toujours dans le meme ordre** — ce qui rend
   coherente la portee par emplacement ci-dessous, et evite qu'un arbre change
   d'identite quand la densite bouge d'un cheveu.
2. **Filtre de pente** : rejet au-dela de 1,1 en tangente (~48 deg).
3. **Hauteur** : la carte de canopee donne le plus grand arbre de sa cellule de
   5 m ; les sujets d'un meme bosquet se repartissent entre 55 % et 100 % de
   cette valeur, sinon la futaie a l'aspect d'une brosse.

Densite obtenue sur toute la zone : **59 arbres/ha** en moyenne (journalisee au
demarrage), soit de l'ordre de 150 000 arbres sur les 25 km2 — dont seuls
quelques milliers existent a un instant donne.

### Niveau de detail de DENSITE

Chaque emplacement a sa **PROPRE portee** : l'emplacement 0 porte a 1 100 m, le
dernier a 320 m, les autres interpoles. De loin, seuls les premiers emplacements
de chaque cellule subsistent : la foret s'eclaircit progressivement au lieu de
disparaitre d'un bloc. Chaque arbre a donc sa propre distance de fondu, et il
n'y a jamais de saut collectif de bosquet.

**Le fondu se fait par RETRECISSEMENT, pas par transparence.** Une instance qui
rapetisse jusqu'a zero disparait sans melange alpha, donc sans imposer de tri en
profondeur ni de seconde passe de rendu.

### Instanciation GPU

Le moteur ne savait pas instancier. Ajoute :

- `renderer/InstanceData.hpp` : **28 octets** par instance (position, echelle
  uniforme, lacet, variation, attenuation). Pas de matrice 4x4 : une instance de
  vegetation ne subit qu'une rotation autour de Y et une echelle uniforme, la
  matrice est reconstituee dans le vertex shader. Sur 300 000 touffes, l'ecart
  serait de 11 Mo par image ecrits sur un bus partage avec le CPU.
- Binding de sommets 1, cadence `VK_VERTEX_INPUT_RATE_INSTANCE`, locations 6 a 8.
  Decrit au pipeline **uniquement** pour les materiaux instancies
  (`ShaderSet::usesInstancing`), meme raisonnement que `usesMorphAttributes` en
  Phase 10 : c'est une correction, pas une optimisation.
- `Material::Foliage` + `shaders/foliage.{vert,frag}`. **Un seul materiau pour
  les arbres ET l'herbe** : ils ont exactement le meme besoin, et ce qui les
  distingue (ondulation, teinte, portee) tient dans le maillage et les push
  constants.
- Buffer d'instances **par frame-in-flight**, host-visible, mappe en permanence,
  qui grandit a la demande et ne retrecit jamais. Reecrit a chaque image parce
  que son contenu change a chaque image. Le remplacer est sans risque : `drawFrame`
  a deja attendu la fence de cette frame-in-flight.
- `static_assert` sur la taille et la contiguite des membres lus comme `vec2`.
  Il a servi des la premiere compilation : la struct fait 28 et non 32 octets
  (`glm::vec3` s'aligne sur 4).

### Ce que coute reellement la vegetation : une mesure qui a change le code

Les arbres faisaient tomber la scene de 637 a 250 FPS pour 2 797 instances de
48 triangles, soit +32 % de geometrie seulement. Le compte ne tombait pas juste.

**Experience discriminante** : camera tournee vers le CIEL, donc pratiquement
aucun arbre dessine, mais le meme balayage de cellules effectue.

| | FPS |
|---|---:|
| Ciel, sans vegetation | 1 946 |
| Ciel, avec vegetation (balayage seul) | **351** |

**2,3 ms par image de balayage PUR, sans le moindre triangle en plus.** Le cout
des arbres n'etait pas geometrique : il etait dans la boucle qui visitait les
~8 400 cellules du carre de portee et faisait une recherche dans le cache pour
chacune, y compris pour celles situees derriere le joueur.

**Correctif : culling par BLOCS de 8x8 cellules.** Un bloc est teste d'un coup
contre le tronc de vision ; s'il echoue, 64 cellules sont ecartees pour un seul
test. Les bornes verticales du bloc sont estimees par cinq sondes d'altitude plus
une marge large — sans danger, parce qu'un bloc situe derriere le joueur est
rejete par les plans lateraux, qui ne dependent pas de cette estimation.

| | Avant | Apres |
|---|---:|---:|
| Ciel, balayage seul | 351 FPS | **1 614 FPS** |
| Vue paysage, arbres | 250 FPS | **466 FPS** |
| Vue paysage, tout | 193 FPS | **283 FPS** |

**Verifie non destructif**, ce qui est indispensable — d'excellents FPS obtenus
en ne dessinant plus rien sont le piege que la Phase 10 avait deja identifie :
comptes d'instances **identiques** (2 797 arbres, 1 839 cellules en cache), et
**98 pixels differents sur 879 360** (0,011 %) entre les images d'avant et
d'apres, hors barre de titre.

Le meme traitement a ete applique a l'herbe (blocs de 16x16 cellules), avec en
plus la suppression d'un `heightAt()` calcule deux fois par touffe. Gains :
procedural 199 -> 258 FPS, dalle Phase 7 176 -> 212 FPS. Comptes de touffes
inchanges (2 158).

---

## TACHE 3 — Herbe procedurale (essai)

Explicitement un essai. **Ce qui marche, et ce qui ne convainc pas, sont dits
tels quels.**

### Placement en touffes, pas en grille

Le risque a eviter etait le motif regulier. Approche retenue : **bruit de VALEUR
2D a deux octaves**, longueur d'onde **14 m**, qui module la densite locale ; la
position exacte dans la cellule vient d'un hachage.

Bruit de valeur plutot que Perlin : Perlin interpole des gradients, plus isotrope
mais quatre produits scalaires par point. Ici le champ ne sert qu'a moduler une
probabilite, jamais a etre regarde ; sa legere preference pour les axes de la
grille est invisible une fois traduite en presence ou absence de touffes. La
courbe de lissage est celle de Perlin (`6t^5 - 15t^4 + 10t^3`), dont les derivees
premiere et seconde s'annulent aux noeuds — donc aucune arete aux frontieres.

> Premier reglage a 45 m de longueur d'onde : a hauteur d'oeil on ne voyait
> qu'une seule tache, donc « une moitie de paysage herbeuse et une moitie nue »
> plutot que des touffes. Ramene a 14 m.

**Teste automatiquement** : la dispersion de la distance au plus proche voisin
depasse 25 % de la moyenne. Sur une grille parfaite elle serait nulle.

### Altitude

Traduction directe de la demande :

| | |
|---|---|
| Taille en bas / en haut | 0,30 m / 0,11 m |
| Alpage dense jusqu'a | 1 750 m |
| Herbe absente au-dessus de | 2 250 m |

La MEME rampe pilote la densite et la taille, ce qui rend la transition vers la
pelouse rase continue. Teste : moins de touffes ET plus petites en altitude.

> Premier reglage a 1 500 / 2 150 m : ces valeurs etaient calquees sur les bornes
> d'altitude de la zone plutot que sur sa vegetation reelle, et supprimaient
> l'herbe precisement la ou le massif est ouvert — les alpages et les pistes du
> nord-ouest. La limite des arbres y est vers 1 800-1 900 m.

### Donnee LiDAR pour distinguer pre, sous-bois et lapiaz

`couvert` dit qu'il y a de la vegetation (sinon c'est du rocher nu) ; `canopee`
dit si c'est une futaie. La densite d'herbe vaut donc
`ramp(couvert, 0,05, 0,30) x mix(0,35, 1, ouverture)`.

> Le facteur sous-canopee valait **0** au premier reglage, avec un seuil de
> pente a 0,9. Diagnostic chiffre sur une fenetre de 80 m : canopee moyenne
> 15,7 m et pente moyenne 0,86, donc **79 % des cellules a densite nulle et 42 %
> rejetees par la pente**. C'etait faux physiquement : une futaie de montagne est
> claire et son sol porte de l'herbe, et une pelouse d'alpage tient sur des
> versants bien plus raides. Porte a 0,35 et 1,3.

### Performance

Portee courte et fondu large : rendu jusqu'a **34 m**, attenuation commencee des
**14 m**. Cellules de 1 m, 24 emplacements, soit au plus 4,5 touffes/m2.
Instanciation GPU, un seul appel de dessin pour toute l'herbe visible.

### Etat honnete du resultat

**Ce qui fonctionne** : le groupement est visible et non repetitif ; la taille et
la densite suivent l'altitude ; l'herbe evite le rocher nu ; le cout est maitrise
(un appel de dessin, portee courte, fondu sans transparence).

**Ce qui ne convainc pas** : avec 2 158 touffes dans une vue de prairie, le
resultat se lit comme un **semis de touffes espacees, pas comme un tapis**. Il
faudrait environ 8 a 10 touffes/m2 la ou il y en a 2. Chaque touffe est par
ailleurs un eventail de trois quads OPAQUES et effiles, sans texture ni decoupe
alpha : de pres cela ressemble a des rubans verts plus qu'a des brins. L'opacite
est un choix — elle evite entierement le melange alpha, donc le tri en
profondeur — mais c'est elle qui plafonne le realisme.

**Une remarque de fond sur cette zone en particulier** : le massif choisi est
majoritairement forestier et rocheux, avec des pentes fortes. La prairie ouverte
sous 1 500 m y represente environ **0,4 % des cellules**. L'herbe a donc
structurellement peu de terrain a couvrir ici, et c'est une propriete du lieu,
pas du systeme.

---

## Captures reproductibles : un probleme decouvert en route

Deux executions du MEME script de capture, mêmes arguments, ne cadraient pas la
meme chose : **yaw -90 dans un cas, yaw -50 dans l'autre**, soit deux images
differant sur 65 % de leurs pixels. Toute comparaison avant/apres etait donc sans
valeur.

Cause : le jeu lit le clavier et la souris **SYSTEME** (GLFW ne voit pas les
messages postes, limite deja documentee en Phase 10). Une manipulation de la
machine pendant la pose reoriente la camera — ce qui s'etait effectivement
produit, l'utilisateur ayant tourne la vue a la souris.

Ajoute :

- **`--lock-input`** : la camera ignore entierement clavier et souris. La vue est
  alors entierement determinee par la ligne de commande. `screenshot_scene.ps1`
  le passe **par defaut** ; `-FreeInput` redonne la main.
- **`--start=est,nord`** (Lambert 93) et **`--look=yaw,pitch`** (degres). Sans
  eux, la camera demarrait toujours au CENTRE du terrain, qui tombe sur du lapiaz
  nu au milieu de cette zone : impossible d'aller cadrer une foret ou une prairie
  sans piloter la souris a la main.
- L'orientation de la camera et les comptes de vegetation sont journalises une
  fois par seconde, a cote du compteur de FPS.

Verifie : deux executions consecutives donnent desormais un rendu **identique au
pixel pres**, seule la barre de titre (qui porte les FPS) differe.

> **Effet de bord utile** : deux processus `vercora.exe` de la Phase 10 tournaient
> encore au demarrage de cette session, empechant l'edition de liens. C'est
> vraisemblablement la trace de l'incident de fermeture non elucide de la
> Phase 10 (« pas sorti dans les 30 s apres WM_CLOSE »). Le fait qu'ils aient
> survecu plus de deux heures rend l'hypothese « lenteur de fermeture » moins
> credible qu'un blocage franc.

---

## Verification

- **Tests headless : 268 assertions** (223 en Phase 10, **+45**), toutes passent.
  Nouveaux blocs : format `.vcto` (tailles de niveaux BC1, chaine complete,
  m/texel, refus d'un fichier absent), maillage de touffe (convention unite,
  `uv.y` nul au pied, normales relevees), semis d'arbres (le couvert commande le
  semis, la frontiere de foret est reproduite, determinisme, **independance du
  chemin parcouru**, respect de l'emprise, niveau de detail de densite compare en
  DENSITE et non en compte brut, attenuation, substitut sans carte, filtre de
  pente), semis d'herbe (altitude sur taille et densite, portee, **placement
  groupe et non regulier**, determinisme), format d'instance.

  > Les tests ont attrape un comportement reel que j'avais oublie : `gatherTrees`
  > ne construit que 96 cellules neuves par appel (budget anti-a-coup), donc un
  > test qui n'appelle qu'une fois ne juge que du premier lot — c'est-a-dire du
  > coin sud-ouest de la zone balayee. D'ou le helper `gatherTreesUntilStable`.

- **`tools\run_vulkan_check.ps1` : 4 scenarios sur 4, du premier coup.** Zero
  `[Validation]`, zero `[ERROR]`, zero `[WARN]`, fermeture propre partout.
  Gameplay reellement exerce : 2 abattages, 3 arbres au sol, 3 debitages,
  8 ramassages (buche, branche, pierre, eclat), 1 percussion reussie, vol de
  parapente avec atterrissage.

- **`--dump-world`** retrouve ses arbres visables : non-regression de la visee.

- **Zero message de validation** sur la totalite des mesures et des captures de
  cette session, y compris les scenes non terrain.

### Captures

| Fichier | Contenu |
|---|---|
| `build/p11_A_avant.png` | Etat Phase 10 : coloration procedurale, sans vegetation |
| `build/p11_B_ortho.png` | + orthophoto |
| `build/p11_C_apres.png` | + arbres + herbe |
| `build/p11_foret.png` | Interieur d'une futaie dense (E 901315 N 6435855) |
| `build/p11_herbe.png` | Herbe sur pelouse (E 902010 N 6436415) |

Les trois premieres sont prises a la MEME position et au MEME cadrage
(E 901500 N 6436600, yaw 100, pitch -7), avec **exactement la meme geometrie de
terrain** (51 tuiles, 417 792 triangles) dans les trois cas.

Le detail le plus parlant de la comparaison : dans `p11_C_apres.png`, **la limite
de foret s'arrete exactement la ou le boisement s'arrete sur la photo**, et le
versant de lapiaz reste nu. C'est la carte de couvert LiDAR qui se voit
directement a l'image.

---

## Nouveaux fichiers

| Fichier | Role |
|---|---|
| `tools/fetch_ortho.py` | Telecharge la BD ORTHO sur l'emprise d'un `.vchm` |
| `tools/make_ortho_texture.py` | Mosaique + mipmaps + BC1 -> `.vcto` |
| `tools/lidar_to_canopy.py` | Cartes de couvert et de hauteur de canopee |
| `src/assets/TextureFile.{hpp,cpp}` | Chargeur `.vcto` (sans Vulkan) |
| `src/renderer/Texture.{hpp,cpp}` | Texture GPU : image, vue, echantillonneur |
| `src/renderer/InstanceData.hpp` | Format d'instance (28 octets) |
| `src/world/Vegetation.{hpp,cpp}` | Semis d'arbres de decor et d'herbe |
| `src/world/GrassMesh.{hpp,cpp}` | Touffe d'herbe (3 quads croises) |
| `shaders/foliage.{vert,frag}` | Vegetation instanciee |

Donnees produites : `data/ortho/` (25 tuiles, 23,8 Mo),
`assets/terrain/vercora_zone_ortho.vcto` (44,7 Mo),
`assets/terrain/vercora_zone_canopy.vchm` et `..._canopy_height.vchm` (4 Mo
chacune). **Disque : 274 Go libres avant, largement suffisant.**

## Options de ligne de commande ajoutees

| Option | Effet |
|---|---|
| `--no-ortho` | Coloration procedurale (repli Phase 10) |
| `--ortho=chemin.vcto` | Texture explicite au lieu de la convention de nommage |
| `--no-vegetation` | Ni arbres de decor ni herbe |
| `--no-grass` | Arbres de decor seuls |
| `--start=est,nord` | Position de depart en Lambert 93 |
| `--look=yaw,pitch` | Orientation de depart, en degres |
| `--lock-input` | Camera figee : ni clavier ni souris |

Touches ajoutees : **O** (orthophoto / procedural), **G** (tout / arbres seuls /
rien).

---

## Ce qui reste a faire, par ordre d'importance

1. **Texture de detail de pres.** A 0,61 m/texel l'orthophoto est molle sous les
   pieds. Melanger une texture de detail (roche, herbe, eboulis) selon la pente
   et l'altitude est le remede standard et ne demande pas de texture virtuelle.
2. **Densifier l'herbe et lui donner une decoupe alpha.** C'est ce qui separe le
   « semis de touffes » du « tapis ». Le melange alpha imposerait un tri en
   profondeur, ou un test alpha (`discard`) qui l'evite — c'est la piste a
   preferer.
3. **Promotion d'un arbre de decor en arbre abattable** quand le joueur s'en
   approche. L'architecture a deux niveaux est faite pour ca ; le lien n'est pas
   pose. Aujourd'hui seuls les 16 arbres de `Forest`, semes autour de l'origine,
   sont abattables — un joueur qui marche 200 m se retrouve dans une foret
   entierement decorative.
4. **Collision avec les arbres de decor** : ils se traversent.
5. **Transfert asynchrone des tuiles de terrain** (verrou identifie en Phase 10,
   toujours ouvert), et **index buffer partage**.
6. **Ombres portees** : sans elles, les arbres flottent visuellement sur le sol.
7. **Elargir la zone de l'orthophoto au-dela du terrain** n'a pas de sens ; en
   revanche, produire l'orthophoto de la dalle de Phase 7 rendrait les scenes de
   test comparables.
8. Le paquet `Pillow` a ete installe dans l'environnement Python 3.14 de la
   Phase 9 (a cote de `laspy`, `lazrs`, `numpy`).

## Compromis assumes

- **0,61 m/texel** : compromis explicite entre memoire et nettete. La source est
  a 20 cm.
- **BC1 destructif** (33,2 dB) : invisible a la distance ou le terrain est
  regarde, et six fois plus compact.
- **Demi-texel de decalage** entre bord d'image et centre du premier texel :
  30 cm, sous la precision de la BD ORTHO.
- **Herbe opaque sans texture** : evite tout melange alpha, plafonne le realisme.
- **Arbres de decor sans collision ni interaction** : c'est la definition meme du
  decor dans cette architecture.
- **Cartes de vegetation au pas de 5 m** : une clairiere de 3 m n'existe pas pour
  le semis.
- **Vegetation active par defaut sur toutes les scenes de terrain**, y compris
  celles sans carte LiDAR, ou un substitut par pente et altitude prend le relais.
  C'est ce qui fait baisser les chiffres du terrain procedural et de la dalle de
  Phase 7 ; `--no-vegetation` rend exactement les performances de la Phase 10.
