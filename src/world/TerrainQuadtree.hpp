#pragma once

#include "core/Frustum.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vercora {

class Heightmap;

// Quadtree de terrain + selection de niveau de detail par distance (CDLOD) +
// frustum culling.
//
// PHASE 10. Motivation, mesuree en Phase 9 : le moteur envoyait UN SEUL
// maillage entier a chaque image, donc un cout proportionnel a la SURFACE du
// terrain (247 -> 67 -> 12 FPS pour 1 -> 4 -> 25 km2), alors que le joueur n'en
// voit qu'une fraction infime.
//
// --- Structure -------------------------------------------------------------
// La grille d'altitudes est decoupee en noeuds carres. Chaque noeud est TOUJOURS
// maille avec le meme nombre de sommets (nodeCells + 1 par cote, 65 par defaut) :
// ce qui change d'un niveau a l'autre, c'est le PAS d'echantillonnage.
//
//   niveau 0 (feuille) : pas 1  echantillon, couvre nodeCells cellules
//   niveau L           : pas 2^L echantillons, couvre nodeCells * 2^L cellules
//
// Un noeud de niveau L a donc exactement le meme cout GPU qu'une feuille tout en
// couvrant 4^L fois plus de terrain : c'est tout le principe. La racine est le
// plus petit niveau dont l'emprise couvre la grille entiere.
//
// Les noeuds qui tombent entierement HORS de la grille ne sont pas crees ; ceux
// qui depassent partiellement voient leurs indices ECRETES sur le bord (les
// sommets excedentaires se superposent au bord, ce qui produit des triangles
// degeneres invisibles plutot qu'une "etagere" plate au-dela du terrain).
//
// --- Selection de LOD ------------------------------------------------------
// Une portee par niveau, qui DOUBLE a chaque niveau :
//     range[L] = rangeFactor * tailleMonde(noeud de niveau L)
// Comme la taille d'un noeud double elle aussi, range[L] = range[0] * 2^L.
//
// La descente part de la racine :
//   - noeud invisible (frustum)                 -> abandonne, rien dessous
//   - niveau 0, ou camera hors de range[L-1]    -> le noeud est DESSINE tel quel
//   - sinon                                     -> on descend dans les 4 fils
// Le resultat est un anneau concentrique de niveaux autour de la camera, avec la
// pleine resolution (pas de 1 m) au contact immediat.
//
// --- Geomorphing -----------------------------------------------------------
// Chaque sommet porte AUSSI sa position dans la grille du niveau superieur (voir
// renderer/TerrainNodeMesh.hpp). Le vertex shader interpole entre les deux selon
// la distance du sommet a la camera, dans la bande [morphStart, morphEnd] que
// cette classe calcule par niveau. Deux consequences :
//   - pas de "popping" au changement de niveau : la geometrie fine a DEJA pris
//     la forme de la grossiere quand la bascule a lieu ;
//   - pas de fissure entre deux noeuds de niveaux differents : au-dela de
//     range[L], un noeud de niveau L est entierement morphe sur la grille L+1,
//     et son voisin de niveau L+1 y est encore a morph = 0, donc sur la meme
//     grille. Les deux bords coincident exactement.
// C'est pour cette derniere raison que morphStart doit rester >= range[L] / 2.
class TerrainQuadtree {
public:
    struct Params {
        // Cellules par cote d'un noeud de niveau 0. Fixe le cout GPU d'un noeud
        // (nodeCells^2 * 2 triangles) et la granularite du culling. 64 -> 8192
        // triangles et 4225 sommets par noeud : assez gros pour que le cout par
        // draw call reste negligeable, assez petit pour que le frustum culling
        // morde vraiment.
        // DOIT etre une puissance de deux (l'ecretage du morphing suppose que la
        // grille du niveau superieur s'obtient en ne gardant qu'un indice sur 2).
        uint32_t nodeCells = 64;

        // Portee du niveau 0, exprimee en MULTIPLES de la taille monde d'un
        // noeud de niveau 0. 3 = "on garde la pleine resolution jusqu'a environ
        // trois largeurs de tuile", soit ~192 m au pas de 1 m.
        float rangeFactor = 3.0f;

        // Bande de morphing, en fraction de range[L] : le geomorphing commence a
        // morphStartRatio * range[L] et est termine a morphEndRatio * range[L].
        // morphStartRatio DOIT rester > 0.5 (voir l'argument anti-fissure
        // ci-dessus) et morphEndRatio <= 1.
        float morphStartRatio = 0.70f;
        float morphEndRatio = 0.95f;
    };

    // Un noeud du quadtree. Les bornes sont en espace MONDE (celui des sommets
    // produits par TerrainNodeMesh), pretes pour le culling et la distance.
    struct Node {
        uint32_t ix0 = 0;    // indice de grille du coin nord-ouest
        uint32_t iz0 = 0;
        uint32_t span = 0;   // cellules couvertes par cote (avant ecretage)
        uint32_t stride = 0; // pas d'echantillonnage = span / nodeCells = 2^level
        uint8_t  level = 0;  // 0 = feuille, pleine resolution

        // Indices dans nodes_, -1 si le fils n'existe pas (hors grille).
        int32_t children[4] = { -1, -1, -1, -1 };

        // PHASE 12 : indice du pere, -1 pour la racine. Sert au REPLI SUR
        // L'ANCETRE : quand une tuile selectionnee n'est pas encore construite,
        // on remonte cette chaine jusqu'a une tuile deja residente (donc de
        // resolution inferieure, mais couvrant la meme surface) et on dessine
        // celle-la plutot que de laisser un trou. Voir TerrainStreamer.
        int32_t parent = -1;

        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
    };

    // Un noeud retenu pour cette frame.
    struct SelectedNode {
        uint32_t nodeIndex = 0;
        uint8_t  level = 0;
        // Bande de geomorphing en metres, poussee telle quelle au vertex shader.
        float morphStart = 0.0f;
        float morphEnd = 0.0f;
        // Distance camera -> boite du noeud : sert a servir les noeuds les plus
        // proches en premier quand le budget de construction d'une frame est
        // sature (les trous eventuels sont alors repousses vers l'horizon).
        float distance = 0.0f;
    };

    // Construit l'arbre a partir d'une grille DEJA CHARGEE. La Heightmap n'est ni
    // copiee ni possedee : elle reste la source unique des altitudes, aussi bien
    // pour le rendu que pour la COLLISION (voir la note de decouplage dans
    // MEMORY.md). L'arbre n'en garde que des bornes.
    // Lance std::runtime_error si les parametres sont inexploitables.
    TerrainQuadtree(const Heightmap& heightmap, const Params& params);
    // Parametres par defaut : surcharge et non `= {}`, voir Heightmap() (refus
    // de GCC/Clang, CWG 1397).
    TerrainQuadtree(const Heightmap& heightmap) : TerrainQuadtree(heightmap, Params{}) {}

    // Remplit `out` avec les noeuds a dessiner, tries du plus proche au plus
    // lointain. `out` est vide au prealable ; sa capacite est conservee.
    void select(const glm::vec3& cameraPosition, const Frustum& frustum,
                std::vector<SelectedNode>& out) const;

    const Node& node(uint32_t index) const { return nodes_[index]; }

    // PHASE 12. Indice du pere d'un noeud, ou -1 s'il n'en a pas (racine).
    int32_t parentIndex(uint32_t index) const { return nodes_[index].parent; }

    // PHASE 12. Renseigne un SelectedNode pour un noeud QUELCONQUE, hors de la
    // descente de select(). Sert au repli sur l'ancetre : celui-ci est dessine
    // alors que la selection ne l'avait pas retenu, mais il lui faut quand meme
    // sa bande de geomorphing et sa distance.
    SelectedNode describeNode(uint32_t nodeIndex, const glm::vec3& cameraPosition) const;
    uint32_t nodeCount() const { return static_cast<uint32_t>(nodes_.size()); }
    uint8_t rootLevel() const { return rootLevel_; }
    uint32_t nodeCells() const { return params_.nodeCells; }
    const Params& params() const { return params_; }

    // Portee (en unites monde) du niveau L. Exposee pour les tests et les logs.
    float range(uint8_t level) const { return ranges_[level]; }

    // Nombre de triangles qu'un noeud dessine, quel que soit son niveau.
    uint32_t trianglesPerNode() const {
        return params_.nodeCells * params_.nodeCells * 2;
    }

    // Resume lisible, journalise au demarrage.
    std::string describe() const;

private:
    // Cree recursivement le sous-arbre et renvoie son indice, ou -1 si le noeud
    // est entierement hors de la grille.
    int32_t build(uint32_t ix0, uint32_t iz0, uint32_t span, uint8_t level);

    // Min/max des altitudes sur la region ECRETEE d'un noeud feuille.
    void leafBounds(const Node& node, float* minY, float* maxY) const;

    void selectRecursive(uint32_t nodeIndex, const glm::vec3& cameraPosition,
                         const Frustum& frustum, std::vector<SelectedNode>& out) const;

    const Heightmap& heightmap_;
    Params params_;

    std::vector<Node> nodes_;
    uint32_t rootIndex_ = 0;
    uint8_t  rootLevel_ = 0;
    uint32_t cells_ = 0; // resolution - 1

    // ranges_[L] = portee du niveau L. Dimensionne a rootLevel_ + 1.
    std::vector<float> ranges_;
};

} // namespace vercora
