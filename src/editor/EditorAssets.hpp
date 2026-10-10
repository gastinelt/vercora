#pragma once

#include "renderer/MeshData.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vercora {

// ---------------------------------------------------------------------------
// PHASE 15 : le premier lot d'assets de l'editeur, genere EN CODE.
// ---------------------------------------------------------------------------
// Meme parti pris que l'arbre (Phase 5), la pierre (Phase 6) et la touffe
// d'herbe (Phase 11) : aucun fichier, aucun telechargement, aucune licence a
// verifier. Le but de cette session n'est pas d'habiller le jeu mais de VALIDER
// LE PIPELINE de placement de bout en bout - catalogue, fantome, pose, collision,
// sauvegarde, rechargement. Pour ca il faut des objets qui existent tout de
// suite et dont on connait les dimensions exactes.
//
// Les vrais assets telecharges viendront par le meme chemin, sans rien changer :
// `AssetCatalog` traite un builtin et un fichier de la meme facon des lors que
// tous deux produisent un `MeshData` (voir editor/AssetCatalog.hpp).
//
// --- CONVENTION D'ANCRAGE, valable pour TOUS les assets d'editeur -----------
// Elle differe de celle de l'arbre, et pour une raison precise.
//
//   * L'arbre (world/TreeMesh.hpp) est genere "unite" : hauteur exactement 1.0,
//     et une instance se met a l'echelle de sa vraie hauteur. C'etait la bonne
//     convention la-bas parce que la Forest fait VARIER la taille des arbres et
//     qu'un mesh unite evite une echelle non uniforme.
//
//   * Un asset d'editeur, lui, a une taille REELLE et intentionnelle : un abri
//     de 3 m est un abri de 3 m. Le facteur d'echelle de l'editeur est un
//     REGLAGE (1,0 par defaut), pas une mise a l'echelle obligatoire. Poser un
//     objet sans y toucher doit donner l'objet a sa taille.
//
// Donc, pour tout asset d'editeur - genere ici ou charge depuis un fichier :
//   - dimensions ABSOLUES, en unites monde (= metres) ;
//   - PIED a y = 0 : le mesh occupe y >= 0, son point d'ancrage est le sol ;
//   - ANCRAGE HORIZONTAL a l'origine : (x, z) = (0, 0) est le point qui se posera
//     exactement sous le reticule.
// C'est ce qui rend le placement trivial : la matrice modele d'un objet pose est
//     translate(pointVise) * rotateY(cap) * scale(echelle uniforme)
// c'est-a-dire exactement le contrat de `DrawItem` (rotation puis echelle
// UNIFORME, donc mat3(model) reste une normal matrix valide).
//
// --- ANCRAGE N'EST PAS CENTRAGE, et la nuance est deliberee ----------------
// Un asset GENERE ICI choisit son point d'ancrage. Il est presque toujours au
// milieu du volume, mais pas toujours, et c'est voulu : le poteau de balisage a
// son MAT a l'origine, sa plaque debordant vers +X. Poser un panneau, c'est
// planter le mat au point vise - pas y placer le milieu de l'ensemble
// mat + panneau, qui n'existe nulle part physiquement. La meme remarque vaudra
// pour un arbre penche, une porte, une passerelle.
//
// Un asset CHARGE DEPUIS UN FICHIER, lui, ne dit rien de son intention : son
// origine est la ou l'auteur l'a laissee, parfois a plusieurs dizaines de metres
// si la scene d'origine contenait plusieurs objets. Faute de mieux,
// `AssetCatalog::normalizeAssetMesh` le RECENTRE de force sur sa boite
// englobante. C'est le defaut le moins mauvais, pas une regle generale - un
// modele importe dont on voudrait un autre ancrage se corrige aujourd'hui en
// deplacant ses sommets dans un modeleur.
//
// La propriete que TOUS respectent, et que le test headless verifie, est plus
// faible et plus utile : l'ancrage tombe DANS l'empreinte horizontale de
// l'objet, pas a cote. Un objet pose entoure donc toujours le point vise.

// Un asset genere en code : son identifiant de catalogue et sa geometrie.
// L'identifiant est ce qui est ECRIT DANS LE FICHIER DE SCENE : il ne doit
// contenir ni espace ni caractere accentue, et ne doit jamais changer une fois
// des objets poses (ce serait casser les scenes existantes).
struct BuiltinAsset {
    std::string id;
    std::string description;
    MeshData (*build)();
};

// Le catalogue des builtins, dans l'ordre ou ils apparaissent dans la liste.
const std::vector<BuiltinAsset>& builtinEditorAssets();

// --- Les geometries ---------------------------------------------------------

// Rocher arrondi de ~1,8 m, facette. C'est le "rocher" de la consigne : le cas
// d'usage evident du placement a la main (un bloc erratique sur une croupe) et
// le premier objet dont la collision doit se comporter comme un tronc.
MeshData editorBoulderMeshData();

// Bloc anguleux de ~3,2 m : une dalle basculee, faces planes et aretes vives.
// Present a cote du rocher parce que les deux exercent des choses differentes -
// le rocher est large et bas (empreinte de collision genereuse), le bloc est
// haut et etroit (il faut le contourner).
MeshData editorSlabMeshData();

// Cairn : quatre pierres empilees en decroissant, ~1,3 m. Le repere de sentier
// classique. Sert surtout de test d'ECHELLE : c'est un petit objet, donc celui
// sur lequel un decalage vertical d'un decimetre se voit tout de suite.
MeshData editorCairnMeshData();

// Abri de pierre a toit a deux pentes, 3,4 x 2,6 x 2,6 m. Le seul asset a
// INTERIEUR : il a une porte, donc il exerce le cas ou la collision par cylindre
// englobant est FAUSSE (on ne peut pas y entrer). C'est volontaire - voir la
// limite documentee dans NOTES-PHASE15.md.
MeshData editorShelterMeshData();

// Poteau de balisage de 2,1 m avec sa plaque. Volontairement FIN : c'est l'asset
// de mesure de l'editeur. Un objet mince pose au point vise montre a l'oeil, au
// centimetre pres, ou le rayon a touche le sol - ce qu'un rocher d'un metre de
// large ne peut pas montrer.
MeshData editorMarkerPostMeshData();

} // namespace vercora
