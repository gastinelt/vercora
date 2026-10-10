# assets/editor — modèles placables par l'éditeur (Phase 15)

Tout fichier `.obj` déposé ici est ajouté au catalogue de l'éditeur au prochain
démarrage du jeu. Aucune inscription nulle part : le dossier est scanné, c'est
tout.

- **Identifiant** = le nom du fichier sans extension, ramené en minuscules ASCII
  (les espaces et les accents deviennent `_`). C'est cet identifiant qui est
  écrit dans les fichiers de scène `assets/scenes/*.vcsc` : **le renommer casse
  les objets déjà posés**.
- **Ordre du catalogue** : les modèles générés en code viennent en premier, puis
  les fichiers **triés par nom**. Les touches `1`..`9` de l'éditeur désignent
  des rangs dans cette liste, d'où le tri — sans lui, la même touche poserait un
  modèle différent d'une exécution à l'autre.
- **Ancrage** : le loader recentre le modèle en X et Z sur sa boîte englobante et
  pose son point le plus bas à `y = 0`. Un `.obj` dont l'origine est n'importe où
  se pose donc quand même exactement au point visé. La documentation complète de
  la convention est dans `src/editor/EditorAssets.hpp`.
- **Unités** : mètres. Un modèle exporté en centimètres sera 100 fois trop grand ;
  le catalogue le signale au chargement (`dimension suspecte`) mais le charge
  quand même — l'échelle se corrige objet par objet dans l'éditeur.
- **Collision** : déduite automatiquement de la boîte englobante (cylindre
  vertical *contenant*). Un objet à intérieur — un abri, une arche — sera plein.
  Le remède immédiat est de le poser *traversable* (touche `K`).

`octaedre.obj` est là comme témoin : c'est le seul asset du catalogue qui vienne
d'un fichier, donc le seul qui prouve que ce chemin fonctionne dans une partie
réelle.

**glTF (`.gltf` / `.glb`) n'est pas encore supporté** — voir `NOTES-PHASE15.md`.
Le seul endroit à étendre est `AssetCatalog::supportedExtensions()`.
