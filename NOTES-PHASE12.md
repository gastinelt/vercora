# NOTES-PHASE 12 — Transferts de tuiles asynchrones et repli sur l'ancetre

Session du 2026-08-21. Objectif unique : lever le **verrou n 1** identifie et
documente en Phase 10 — `VulkanBuffer::copyFrom` faisait un `vkQueueWaitIdle`
par upload de tuile, ce qui imposait un budget artificiel de 4 tuiles construites
par frame et laissait un TROU la ou une tuile n'etait pas encore prete.

Machine : Intel UHD 770 integree, i5-14600K, 64 Go. Build **Debug**, couches de
validation actives, comme toutes les mesures depuis la Phase 9.

---

## 1. Ce que le GPU de cette machine expose vraiment

La consigne etait « une queue de transfert dediee si le GPU en expose une
separee de la queue graphique, sinon une soumission asynchrone sur la meme
queue ». Il fallait donc commencer par regarder. `VulkanContext` journalise
desormais l'inventaire des familles a chaque demarrage :

```
Familles de queues : 2 - [0] x1 graphics compute transfer sparse [1] x2
Aucune famille de transfert dediee sur ce GPU : les transferts partagent
forcement la queue graphique.
```

La famille 1 n'expose **aucun** des bits graphics/compute/transfer/sparse : c'est
une famille video (decode/encode), inutilisable pour un `vkCmdCopyBuffer`. Il n'y
a donc **qu'une seule queue utilisable**, et la branche « queue de transfert
dediee » ne s'applique pas ici.

La detection est conservee dans `QueueFamilyIndices::dedicatedTransferFamily` et
journalisee, mais **elle n'est pas utilisee** : ecrire le transfert de propriete
de famille de queue (barriere de release cote transfert + barriere d'acquire cote
graphique) qu'imposerait une famille distincte reviendrait a livrer de la
synchronisation qu'aucun test de cette machine ne peut exercer. C'est exactement
ce que la consigne interdisait.

## 2. L'architecture retenue

Sur une machine a une seule queue, « soumettre de facon asynchrone sur la meme
queue avec suivi par fence » et « enregistrer la copie dans le command buffer de
la frame » aboutissent au meme ordonnancement GPU. La seconde forme a ete
retenue, pour quatre raisons concretes :

1. **Une seule soumission par frame**, au lieu d'une soumission + une fence
   supplementaires.
2. **La dependance memoire est explicite** — une `VkMemoryBarrier`
   `TRANSFER_WRITE -> VERTEX_ATTRIBUTE_READ | INDEX_READ`, entre
   `VK_PIPELINE_STAGE_TRANSFER_BIT` et `VK_PIPELINE_STAGE_VERTEX_INPUT_BIT` —
   donc **verifiable par la validation de synchronisation**, au lieu de reposer
   sur l'effet de bord d'un vidage de queue.
3. **Aucun transfert de propriete de famille** a ecrire : meme famille de bout en
   bout.
4. **La tuile est lisible DES CETTE FRAME.** Il n'y a aucune latence de
   transfert a masquer : le repli sur l'ancetre ne sert donc qu'aux tuiles pas
   encore *construites* (budget CPU), jamais a une copie en vol.

### Le chemin, concretement

```
TerrainStreamer::buildNode
  -> Renderer::addMesh
       -> Mesh(...)  : maille, ecrit un buffer staging host-visible,
                       cree le buffer device-local, puis
       -> Renderer::enqueueCopy(staging, destination)   [ne bloque pas, ne soumet rien]

Renderer::drawFrame
  -> vkBeginCommandBuffer
  -> recordPendingCopies()      : N x vkCmdCopyBuffer, puis UNE VkMemoryBarrier
  -> vkCmdBeginRenderPass ...   : les draws lisent une geometrie deja ordonnancee
  -> une seule vkQueueSubmit
```

`Mesh` ne connait plus ni command pool ni queue : il recoit une interface
`BufferUploader` (declaree dans `VulkanBuffer.hpp`), que le `Renderer` implemente.
Sans cette interface, `Mesh` dependrait du `Renderer`, qui possede des `Mesh`.

### Duree de vie des buffers staging

C'est le point qui pouvait silencieusement corrompre le rendu. Un staging dont la
copie est enregistree mais pas encore executee **ne doit pas etre detruit**. Il
passe donc dans `retiredStaging_`, avec le meme compte a rebours de
`kMaxFramesInFlight + 1` vieillissements que `retiredMeshes_` de la Phase 10 —
mecanisme deja eprouve, decremente juste apres l'attente de la fence de frame,
donc a un instant ou l'on sait qu'une frame de plus est reellement terminee.

### Le garde-fou memoire (et le seul `vkQueueWaitIdle` qui subsiste)

Differer les copies veut dire garder les staging trois frames de plus. Pour le
streaming c'est negligeable (64 tuiles ~ 25 Mo). Pour le chemin `--no-lod`, qui
televerse la zone de 25 km2 en **un seul maillage**, le staging pese 3,4 Go a lui
seul : la premiere version faisait passer le pic memoire de ce chemin de
**5,6 a 6,9 Go**, soit +23 % — une vraie regression sur une mesure de la Phase 10.

`Renderer::enqueueCopy` bascule donc sur une soumission **synchrone**
(`flushCopiesNow`) des que le staging vivant depasse **128 Mo**. Le seuil laisse
passer tout le streaming (verifie : **0 declenchement** sur les survols a 60 et
900 m/s) et n'attrape que les televersements geants, qui ont lieu avant la
premiere frame et sont donc hors du chemin critique. Pic `--no-lod` ramene a
**5 636 Mo**, la valeur de la Phase 10.

## 3. Verification de la synchronisation

Les couches de validation **standard** ne verifient pas les dependances
memoire : elles etaient a zero message avant comme apres, y compris avec une
synchronisation fausse. Cette session active donc la **validation de
synchronisation**, qui n'avait ete activee dans aucune phase precedente :

```bash
set VK_LAYER_VALIDATE_SYNC=1
```

### Un defaut ANTERIEUR, mis au jour et corrige

Premier resultat, avant meme de regarder les transferts :

```
[ERROR] [Validation] vkQueueSubmit(): WRITE_AFTER_WRITE hazard detected.
vkCmdBeginRenderPass ... writes to resource, which was previously written at the
end of subpass 0 (vkCmdEndRenderPass) by the attachment storeOp ...
```

Il ne vient pas des transferts : il se reproduit sur la scene `cube` et avec
`--no-lod`, donc sans le moindre upload en vol. Cause : la dependance de subpass
de `SwapChain` avait `srcAccessMask = 0`, ce qui n'ordonne que l'execution, pas
les ecritures — or il n'y a **qu'un seul buffer de profondeur pour les deux
frames-in-flight**, donc le clear de la frame N+1 courait sans dependance contre
le storeOp de la frame N.

Corrige (`srcStageMask` = `COLOR_ATTACHMENT_OUTPUT | LATE_FRAGMENT_TESTS`,
`srcAccessMask` = les deux ecritures d'attache). Corrige **ici et pas plus tard**
parce qu'une erreur permanente dans le journal de synchronisation rendrait
invisible un vrai probleme de transfert.

### Controle NEGATIF : la verification mord-elle vraiment ?

Un journal propre ne prouve rien si la verification ne regarde pas au bon
endroit. La barriere de transfert a donc ete **retiree volontairement**, le jeu
rebuilde et rejoue sur la meme scene :

| Barriere | Messages de la validation de synchronisation |
|---|---|
| **retiree** | **10 x `vkCmdDrawIndexed(): READ_AFTER_WRITE hazard ... reads index VkBuffer ... previously written by vkCmdCopyBuffer`** |
| en place | **0** |

La verification voit donc bien les copies de tuiles, et c'est la barriere qui
rend le journal propre — pas un angle mort de l'outil.

### Etat final

Zero `hazard`, zero `[Validation]`, zero `[ERROR]`/`[WARN]` sur : `cube`,
`assets/icosphere.obj`, dalle de Phase 7, zone 25 km2 a 1 m statique, `--no-lod`,
survol scripte a 60 et 900 m/s, et vol de parapente reel sur la zone.

## 4. Le repli sur l'ancetre

### Le principe

Une tuile selectionnee mais pas encore construite n'est plus laissee en trou : on
remonte sa **chaine de peres** (nouveau champ `Node::parent`) jusqu'a une tuile
deja residente, et c'est celle-la qu'on dessine. Elle couvre exactement la meme
surface, en resolution inferieure.

**Le repli REMPLACE le sous-arbre, il ne s'y superpose pas.** Les autres tuiles
selectionnees situees sous l'ancetre retenu sont retirees de la frame, meme
residentes. Sans cela deux surfaces de niveaux differents se recouvriraient et se
disputeraient le z-buffer : on aurait remplace un trou par un mouchetage
clignotant. L'ensemble dessine reste ainsi une **coupe** de l'arbre — chaque
metre carre visible est couvert une fois et une seule.

Les tuiles ecartees au profit d'un ancetre sont quand meme marquees « utilisees a
cette frame », sinon le LRU evincerait une geometrie que la selection redemande.

### Ce qui ne marchait pas, et la mesure qui l'a dit

Premiere version : repli seul, sans rien d'autre. Resultat a 900 m/s avec un
budget de 4 tuiles/frame : **112 -> 25 frames a trou**. Mieux, mais pas zero.

Hypothese initiale — « le LRU evince les ancetres » — **testee et fausse** : une
eviction biaisee par niveau (sursis proportionnel au niveau) n'a fait passer que
de 25 a 23 frames, et de 288 a 255 dans la configuration la plus serree. Ce code
a donc ete **retire** : garder un parametre de reglage dont la justification a ete
mesuree comme marginale n'aurait fait qu'ajouter du bruit.

Un diagnostic temporaire journalisant le niveau des tuiles restees non couvertes
a donne la vraie reponse en une execution :

| Niveau de la tuile non couverte | Ancetres au-dessus d'elle | Occurrences |
|---|---|---|
| 5 | 2 | 324 |
| 4 | 3 | 270 |
| 3 | 4 | 33 |

Les tuiles a trou sont **grossieres**, a l'horizon, et leurs ancetres sont de
niveau 6 et 7 — **jamais dessines de toute la session**. Un noeud proche de la
racine ne se dessine que vu de plus loin que le terrain n'est grand, ce qui
n'arrive jamais quand on est dedans. Le repli n'avait donc structurellement
aucune cible.

### Le correctif : le sommet du quadtree est epingle

`TerrainStreamer` construit au demarrage **toutes les tuiles des trois niveaux
les plus grossiers** (`Params::pinnedTopLevels = 3`) et ne les evince jamais.
Elles sont peu nombreuses — au plus 1 + 4 + 16 = 21, et **14 en pratique sur la
zone 25 km2** (les autres tombent hors de la grille), soit **5 Mo**.

La propriete obtenue est structurelle, pas statistique : tout noeud selectionne
est soit epingle lui-meme, soit descendant d'un noeud epingle, donc **il existe
toujours un ancetre resident**. `uncoveredNodes()` ne peut plus valoir autre chose
que zero — et reste mesure a chaque frame pour le prouver plutot que pour le
supposer.

## 5. Le budget par frame

`maxUploadsPerFrame` passe de **4 a 64**. Le budget ne borne plus une
synchronisation GPU — il n'y en a plus — mais le seul cout restant, qui est le
**maillage CPU** d'une tuile (65 x 65 sommets a calculer, ~385 Ko a recopier). Il
subsiste pour deux raisons : lisser l'a-coup d'un demi-tour brutal, et borner le
staging vivant simultanement (64 tuiles ~ 25 Mo, tres en dessous du seuil de
128 Mo du garde-fou).

Reglable par `--tile-budget=N`, ce qui permet de rejouer le plafond de la
Phase 10 **sur le meme binaire**.

### Un budget plus large ne fait-il pas des a-coups ?

C'est le risque evident : concentrer le maillage de 64 tuiles dans une frame
ferait un pic de temps d'image. Mesure a 120 m/s, deux runs par budget :

| Budget | FPS median | FPS min instantane |
|---:|---|---|
| 4 | 363 / 296 | 289 / 201 |
| 16 | 346 / 319 | 223 / 253 |
| 64 (defaut) | 334 / 354 | 243 / 240 |
| 256 | 347 / 350 | 239 / 222 |

Aucune degradation systematique. **Mais cette mesure a une limite qu'il faut
dire** : le compteur de FPS du jeu moyenne sur ~250 ms, donc il ne peut PAS voir
un a-coup d'une seule frame. Ce n'est donc pas une preuve d'absence de pic.

Ce que les donnees prouvent vraiment, et qui suffit : le pic de tuiles construites
en une frame vaut **53, et il vaut 53 AUSSI avec `--tile-budget=4`**. Ce 53 vient
donc de la **frame d'amorcage**, qui ignore le budget (et l'ignorait deja en
Phase 10). Apres l'amorcage, le budget n'est jamais la contrainte qui mord — a
900 m/s le budget de 64 laisse **0 tuile en attente**, alors que 4 en laissait.
Le passage de 4 a 64 enleve un plafond artificiel, il n'ajoute pas de travail.

## 6. Le scenario qui a motive le chantier

`--flythrough=V[,H]` : la camera avance seule a V m/s, H metres au-dessus du
relief, et vire de 90 degres en arrivant au bord de l'emprise. Scripte plutot que
pilote au clavier parce que le harnais ne peut pas commander la souris (voir
`run_vulkan_check.ps1`) et parce qu'une trajectoire reproductible est la condition
d'une comparaison.

### Premier resultat : a vitesse realiste, le probleme ne se reproduit pas

A 60 m/s (216 km/h, six fois la vitesse d'un parapente), **meme les reglages de la
Phase 10 ne produisent aucun trou** : 0 frame a trou sur 10 591. A 400 FPS la
camera n'avance que de 15 cm par frame, et 4 tuiles/frame font 1 600 tuiles/s.

Il a donc fallu monter la vitesse jusqu'a ce que les reglages de la Phase 10
cassent vraiment, sans quoi la comparaison n'aurait rien prouve.

### Le tableau

Zone 25 km2 au pas de 1 m, survol scripte, meme binaire, seules les options
changent. « Frames a trou » = frames comptant au moins une tuile selectionnee que
**rien** ne couvrait.

| Vitesse | Budget | Repli | Sommet epingle | Frames a trou | Pic de tuiles manquantes |
|---:|---:|:---:|:---:|---:|---:|
| 60 m/s | 4 | non | non | 0 / 10 591 | 0 |
| 300 m/s | 4 | non | non | **21 / 6 750** | 7 |
| 900 m/s | 4 | non | non | **112 / 4 780** | 17 |
| 900 m/s | 1 | non | non | **1 973 / 4 798** | 23 |
| 900 m/s | 4 | oui | non | 25 / 4 739 | 7 |
| 900 m/s | 1 | oui | non | 288 / 4 640 | 11 |
| **900 m/s** | **1** | **oui** | **oui** | **0 / 4 748** | **0** |
| **900 m/s** | **64 (defaut)** | **oui** | **oui** | **0 / 4 063** | **0** |
| **60 m/s** | **64 (defaut)** | **oui** | **oui** | **0 / 6 990** | **0** |

A 900 m/s avec un budget de 1 tuile/frame, le repli est sollicite sur **1 969
frames sur 4 748** (pic de 9 ancetres simultanes) et **ne manque jamais sa
cible**. C'est la ligne qui montre que le filet fonctionne vraiment, et pas
seulement qu'il n'est jamais appele.

### Vol en parapente reel

Physique du jeu, pas un survol scripte : passage en mode libre, montee, decollage
(V), douze virages pendant la descente, atterrissage **naturel** contre le relief
LiDAR, sur la zone de 25 km2, sous validation de synchronisation.

```
Decollage en parapente, cap 62.8 degres.
Parapente : atterrissage.
Trous de streaming : 0 frame(s) sur 25 501 avec au moins une tuile NON COUVERTE
(pic 0 tuiles) ; repli sur ancetre utilise dans 0 frame(s).
hazards=0  [Validation]=0  [ERROR]/[WARN]=0  pic RAM=434,1 Mo
```

## 7. Mesures FPS / RAM

Toutes avec `--lock-input` (sans le verrou, la souris systeme reoriente la camera
et l'ecart entre deux runs atteint 15 % — voir Phase 11). Mediane de plusieurs
runs, chacun de 12 a 15 s apres 5 s de chauffe.

| Scene, camera a l'arret | Phase 11 (avant) | **Phase 12 (apres)** | RAM avant | **RAM apres** |
|---|---:|---:|---:|---:|
| Zone 25 km2 a 1 m | 312 FPS | 292 FPS | 332,2 Mo | 332,3 Mo |
| Zone 25 km2 a 2 m | 315 FPS | 276 FPS | 260,7 Mo | 259,8 Mo |
| Dalle de Phase 7 | 218 FPS | 216 FPS | 128,8 Mo | 151,2 Mo |
| Terrain procedural | 267 FPS | 263 FPS | 106,9 Mo | 109,9 Mo |
| Zone 1 m `--no-lod` | 9 FPS | 9 FPS | 5 600 Mo | 5 637 Mo |

### Comment lire la colonne FPS : c'est du bruit, et voici la preuve

Les ecarts ci-dessus vont de -1 % a -12 %, mais **le bruit de mesure de cette
machine est du meme ordre** : cinq runs consecutifs de la zone 1 m dans la
configuration Phase 12 ont donne 278 / 280 / 292 / 293 / 295, et la zone 2 m a
donne 208 / 276 / 309 sur trois runs.

Un temoin tranche la question, sur **le meme binaire** et donc sans aucun bruit
de compilation. A l'arret, aucun transfert n'a lieu : les reglages de streaming
ne peuvent rien changer.

| Zone 1 m, camera a l'arret | FPS median |
|---|---:|
| Reglages Phase 10 (`--tile-budget=4 --no-tile-fallback`) | 285 |
| Reglages Phase 12 (defaut) | 292 |

Et sous charge de streaming, survol a 60 m/s :

| Zone 1 m, survol 60 m/s | FPS median (3 runs) |
|---|---:|
| Reglages Phase 10 | 422 (378 / 422 / 435) |
| Reglages Phase 12 (defaut) | 418 (404 / 418 / 438) |

Les deux configurations sont donc **indiscernables**, a l'arret comme en
mouvement. Il n'y a ni gain ni perte de FPS a attendre de cette session : le
verrou leve ne se manifestait pas en FPS moyen mais en **trous visuels**, et
c'est la que la mesure a bouge.

### La RAM, elle, a bouge — et c'est explicable

- **Dalle de Phase 7 : +22,4 Mo.** 21 tuiles epinglees (7 Mo de geometrie GPU) et
  le staging d'un demarrage qui construit 51 tuiles d'un coup au lieu d'une a la
  fois. C'est le prix direct du « ne plus attendre » : les staging coexistent au
  lieu de se succeder.
- **Terrain procedural : +3 Mo.** Tout l'arbre y tient en 5 tuiles.
- **Zone 25 km2 : inchangee** (332,2 -> 332,3 Mo). Le demarrage y construisait
  deja 67 tuiles d'un bloc ; 14 tuiles epinglees s'y perdent dans le bruit.
- **`--no-lod` : +37 Mo** (5 600 -> 5 637), soit +0,7 %, grace au garde-fou de
  128 Mo. Sans lui : 6 915 Mo.

## 8. Non-regression

- **280 assertions headless** (268 en Phase 11, +12 pour la Phase 12), via
  `tools\run_logic_tests.bat`.
- **`tools\run_vulkan_check.ps1` : 4 scenarios sur 4, du premier coup**, zero
  `[Validation]`, zero `[ERROR]`/`[WARN]`, fermeture propre sur les quatre —
  2 abattages, 3 debitages, 9 ramassages, 1 percussion, dalle LiDAR reelle,
  decollage et atterrissage en parapente.
- **Traversee de 20 km** (`tools\traverse_terrain.ps1 -Distance 2500`, zone
  25 km2 au pas de 1 m, six branches, en sortant deux fois de l'emprise) :
  **0 message `[Validation]`, 0 `[ERROR]`/`[WARN]`, code de sortie 0**, aucune
  chute a travers le sol, aucune anomalie aux anciennes frontieres de dalles.
  FPS moyen de 146 a 230 selon la branche. **Ces chiffres ne se comparent pas a
  ceux de la Phase 10** (459 a 725) : cette traversee-la precedait la vegetation
  de la Phase 11, qui est de loin le poste le plus cher en marche.
  **Incident de la Phase 10 non reproduit** : le jeu etait alors reste bloque
  plus de 30 s apres `WM_CLOSE` sur un run de 20 km, sans explication. Cette fois
  il est sorti proprement. Toujours pas explique, mais toujours pas reproduit non
  plus.
- `--dump-world` : **16 lignes AIM sur 16 arbres**, inchange.
- `cube`, `assets/icosphere.obj`, `--no-lod`, terrain procedural : intacts, zero
  validation, zero hazard.
- Le decouplage rendu / collision de la Phase 10 n'est pas touche : le repli ne
  concerne que ce qui est DESSINE, `Heightmap::heightAt()` reste la source unique
  de la physique.

### Controle visuel : la vue est-elle la meme ?

C'est le garde-fou pose en Phase 10 contre le piege « excellents FPS parce que le
culling est casse ». Zone 25 km2, vegetation et herbe coupees (pour retirer toute
animation), **vue epinglee** par `--look=-90,-8` :

| Comparaison | Pixels differents sur 921 600 |
|---|---:|
| Reglages Phase 12 contre eux-memes (temoin) | 60 (0,007 %) |
| **Reglages Phase 12 contre reglages Phase 10** | **45 (0,005 %)** |

L'ecart entre les deux configurations est **plus petit que le temoin de la meme
configuration relancee deux fois** : les images sont identiques a la
reproductibilite pres du harnais. Meme nombre de tuiles dessinees (66 sur 66) et
de triangles (540 672) sur les deux.

**Piege rencontre, a retenir.** Sans `--look`, trois captures de la meme scene ont
donne 38,2 / 38,3 / 32,9 % de ciel : trois cadrages differents, donc une
comparaison pixel a pixel sans aucune valeur (elle annoncait 2,4 % d'ecart, pur
artefact de cadrage). `--lock-input` seul ne suffit pas a fixer la vue de depart.
Pourquoi le verrou laisse passer ce residu n'est **pas elucide** ; la parade est
connue et suffisante : epingler `--look` pour toute comparaison d'image.

### Les 12 nouvelles assertions

Le repli lui-meme vit dans `TerrainStreamer`, qui pilote Vulkan : il n'est pas
testable en headless. Ce qui l'est, et dont il depend entierement, c'est la
structure sous-jacente — une erreur y serait silencieuse :

- la chaine de peres est complete (un seul noeud sans pere), coherente (le pere
  est toujours un niveau au-dessus) et **reciproque** (chaque noeud figure bien
  dans les `children` de son pere — c'est ce test qui attraperait un chainage
  pose sur une reference devenue pendante apres reallocation de `nodes_` pendant
  la construction recursive) ;
- la remontee se termine, en au plus `rootLevel` etapes ;
- **l'emprise d'un pere contient celle de chacun de ses fils** — la propriete qui
  rend le repli correct ;
- `describeNode()` redonne **au bit pres** ce que produit `select()` pour un
  noeud selectionne, et suit la portee du bon niveau pour un ancetre ;
- le sommet du quadtree compte au plus 21 tuiles sur trois niveaux (l'hypothese
  qui rend l'epinglage defendable est figee par un test).

## 9. Options ajoutees

| Option | Effet |
|---|---|
| `--tile-budget=N` | N tuiles construites au plus par frame (defaut 64 ; 4 = plafond de la Phase 10) |
| `--no-tile-fallback` | pas de repli sur l'ancetre : une tuile pas encore construite laisse un trou (comportement de la Phase 10) |
| `--flythrough=V[,H]` | survol scripte a V m/s, H m au-dessus du relief (defaut 120), virage de 90 deg au bord |

Les trois n'existent que pour rejouer et comparer les deux moities du chantier
sans recompiler, dans l'esprit de `--no-lod` de la Phase 10.

## 10. Limites restantes

1. **La branche « queue de transfert dediee » n'est pas ecrite.** Sur un GPU
   discret qui expose une famille transfert-seule, le moteur continuera d'utiliser
   la queue graphique. Le gain serait reel (recouvrement des copies avec le
   rendu) mais impose un transfert de propriete de famille de queue, non
   eprouvable sur cette machine. La detection, elle, est en place et journalisee.
2. **Le maillage d'une tuile reste synchrone sur le thread de rendu.** C'est
   desormais LE cout du streaming : ~385 Ko a produire et recopier par tuile. Un
   thread de maillage dedie est le prochain verrou, pas le transfert.
3. **Le repli degrade la resolution sur toute l'emprise de l'ancetre**, pas
   seulement sur la tuile manquante (c'est le prix de la coupe sans
   recouvrement). Une tuile L4 manquante fait basculer ~2 km de terrain en L5 le
   temps d'une frame ou deux. Invisible dans les conditions testees, mais c'est
   un compromis assume, pas une absence de defaut.
4. **Une fissure fine est possible au bord d'un ancetre de repli** : dessine plus
   pres que sa portee nominale, son geomorphing est a zero alors qu'un voisin
   plus fin est deja sur sa propre grille. Transitoire, et sans commune mesure
   avec un trou de ciel — mais aucun test automatique ne cherche une fissure dans
   l'IMAGE rendue (limite deja notee en Phase 10, toujours ouverte).
5. **Aucun instrument ne mesure le temps d'UNE image.** Le compteur de FPS
   moyenne sur ~250 ms : un a-coup isole (une frame a 30 ms) est invisible dans
   toutes les mesures de ce projet depuis la Phase 7. Tant que le budget par
   frame reste au-dessus de la demande reelle, ca n'a pas mordu ; c'est le
   premier instrument a ajouter si un jour un a-coup est ressenti sans etre
   mesurable.
6. **Le staging vit trois frames.** Le garde-fou borne le pic, mais un materiel a
   memoire video etroite verrait cette pression avant nous.
7. **La frame d'amorcage n'a pas de budget** (`primingFrames = 1`) : elle
   construit toute la vue initiale d'un coup, donc autant de staging simultanes.
   Mesure a 67 tuiles (~26 Mo) sur la zone 25 km2, tres loin du seuil.
8. Les points 2, 3, 5, 6 et 7 de la liste « ce qui reste a ameliorer » de la
   Phase 10 **restent ouverts** : index buffer partage, sommet allege, streaming
   DISQUE, parametres de quadtree non reglables, absence de test de fissure.
