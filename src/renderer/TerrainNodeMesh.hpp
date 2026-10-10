#pragma once

#include "renderer/MeshData.hpp"

#include <cstdint>
#include <vector>

namespace vercora {

class Heightmap;
class TerrainQuadtree;

// Maillage d'UN noeud de quadtree (Phase 10).
//
// Le pendant "par tuile" de renderer/TerrainMesh.hpp, qui lui maille la grille
// ENTIERE en un seul bloc. Les deux coexistent volontairement : TerrainMesh
// reste le chemin de reference (option --no-lod du jeu), ce qui permet de
// comparer avant/apres sur exactement la meme scene.
//
// Ce que le maillage d'un noeud a de particulier :
//
//  - Il echantillonne la grille au PAS DU NOEUD (node.stride), pas au pas de la
//    donnee. Un noeud de niveau 3 lit un point sur huit : meme nombre de
//    sommets qu'une feuille, huit fois plus de terrain couvert.
//
//  - Les indices de grille sont ECRETES sur le bord du terrain. Un noeud qui
//    depasse ne produit donc pas une "etagere" plate au-dela des donnees, mais
//    des triangles degeneres (sommets confondus) que le rasterizer ignore.
//
//  - Chaque sommet porte sa CIBLE DE GEOMORPHING : sa position et sa normale
//    dans la grille du niveau superieur, obtenues en arrondissant ses indices
//    LOCAUX vers le bas a un multiple de deux (c'est exactement la grille que
//    dessinerait le noeud parent). Un sommet d'indice local pair est deja sur
//    cette grille et ne bouge pas ; un sommet impair vient se confondre avec son
//    voisin pair. A morph = 1, la tuile a donc EXACTEMENT la forme de son
//    parent, ce qui est la condition pour que le raccord avec une tuile voisine
//    plus grossiere soit sans fissure.
//
//  - Les indices (la topologie) sont IDENTIQUES pour tous les noeuds de tous les
//    niveaux : c'est toujours la meme grille reguliere de nodeCells x nodeCells
//    cellules. Ils sont donc calcules une fois et recopies (voir
//    terrainNodeIndices).
//
// `withIndices` = false laisse `indices` VIDE ; c'est a l'appelant de poser la
// topologie. PHASE 18 : elle est identique pour toutes les tuiles de tous les
// niveaux (la remarque etait deja dans cet en-tete depuis la Phase 10, mais le
// code la reconstruisait quand meme a chaque tuile), et TerrainStreamer en
// garde desormais UN exemplaire qu'il recopie. Le defaut reste `true` pour que
// tout autre appelant - les tests headless notamment - obtienne un maillage
// complet et utilisable tel quel.
//
// Lance std::runtime_error si le noeud ne peut pas produire un maillage sain.
// `reuseParentGrid` = false recalcule la cible de morphing SOMMET PAR SOMMET,
// c'est-a-dire quatre fois par bloc 2x2 : c'est le comportement d'avant la
// Phase 18, conserve comme TEMOIN de mesure (option de jeu --no-tile-cache).
// Le resultat est identique - c'est la meme fonction avec les memes arguments -
// seul le nombre d'appels change, ce que verifie tests\logic_tests.cpp.
MeshData terrainNodeMeshData(const Heightmap& heightmap, const TerrainQuadtree& quadtree,
                             uint32_t nodeIndex, bool withIndices = true,
                             bool reuseParentGrid = true);

// Topologie commune a tous les noeuds : deux triangles par cellule, meme
// diagonale que TerrainMesh. Calculee une fois par appelant et reutilisee.
std::vector<uint32_t> terrainNodeIndices(uint32_t nodeCells);

} // namespace vercora
