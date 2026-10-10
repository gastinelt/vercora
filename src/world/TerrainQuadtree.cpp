#include "world/TerrainQuadtree.hpp"

#include "assets/Heightmap.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vercora {

TerrainQuadtree::TerrainQuadtree(const Heightmap& heightmap, const Params& params)
    : heightmap_(heightmap), params_(params) {
    if (params_.nodeCells < 2 || (params_.nodeCells & (params_.nodeCells - 1)) != 0) {
        throw std::runtime_error(
            "TerrainQuadtree : nodeCells doit etre une puissance de deux >= 2 (recu " +
            std::to_string(params_.nodeCells) + ").");
    }
    if (heightmap.resolution() < 2) {
        throw std::runtime_error("TerrainQuadtree : grille trop petite pour former un noeud.");
    }
    if (!(params_.morphStartRatio > 0.5f) || params_.morphEndRatio > 1.0f ||
        params_.morphEndRatio <= params_.morphStartRatio) {
        throw std::runtime_error(
            "TerrainQuadtree : bande de morphing invalide (il faut "
            "0.5 < morphStartRatio < morphEndRatio <= 1).");
    }

    cells_ = heightmap.resolution() - 1;

    // Racine = plus petit niveau dont l'emprise couvre toute la grille. Un
    // terrain procedural de 128 echantillons (127 cellules) tient donc en un
    // seul niveau au-dessus des feuilles, et la dalle de la Phase 7 (1000
    // cellules) en quatre.
    uint32_t span = params_.nodeCells;
    rootLevel_ = 0;
    while (span < cells_) {
        span *= 2;
        ++rootLevel_;
        if (rootLevel_ > 24) { // garde-fou : 64 * 2^24 cellules, hors de portee
            throw std::runtime_error("TerrainQuadtree : grille trop grande.");
        }
    }

    ranges_.resize(static_cast<std::size_t>(rootLevel_) + 1);
    const float leafWorldSize = static_cast<float>(params_.nodeCells) * heightmap.spacing();
    for (uint8_t level = 0; level <= rootLevel_; ++level) {
        ranges_[level] = params_.rangeFactor * leafWorldSize *
                         static_cast<float>(1u << level);
    }

    // Reservation genereuse : le quadtree complet d'une grille de 5000 cellules
    // fait ~8300 noeuds, soit moins d'un demi-megaoctet.
    nodes_.reserve(1024);

    const int32_t root = build(0, 0, span, rootLevel_);
    if (root < 0) {
        throw std::runtime_error("TerrainQuadtree : aucune racine construite.");
    }
    rootIndex_ = static_cast<uint32_t>(root);
}

int32_t TerrainQuadtree::build(uint32_t ix0, uint32_t iz0, uint32_t span, uint8_t level) {
    // Entierement au-dela du bord : le noeud n'existe pas. Le test porte sur le
    // COIN : un noeud qui commencerait sur la derniere ligne d'echantillons ne
    // couvrirait aucune cellule.
    if (ix0 >= cells_ || iz0 >= cells_) {
        return -1;
    }

    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(Node{});
    {
        Node& node = nodes_[index];
        node.ix0 = ix0;
        node.iz0 = iz0;
        node.span = span;
        node.stride = span / params_.nodeCells;
        node.level = level;
    }

    if (level == 0) {
        float minY = 0.0f;
        float maxY = 0.0f;
        leafBounds(nodes_[index], &minY, &maxY);

        // Emprise monde ECRETEE sur la grille : c'est ce qui sera reellement
        // maille (voir TerrainNodeMesh), donc ce qui doit etre teste au culling.
        const uint32_t ix1 = std::min(ix0 + span, cells_);
        const uint32_t iz1 = std::min(iz0 + span, cells_);

        Node& node = nodes_[index];
        node.boundsMin = { heightmap_.worldCoord(ix0), minY, heightmap_.worldCoord(iz0) };
        node.boundsMax = { heightmap_.worldCoord(ix1), maxY, heightmap_.worldCoord(iz1) };
        return static_cast<int32_t>(index);
    }

    // Fils : quatre quadrants de demi-emprise, un niveau plus fin.
    const uint32_t half = span / 2;
    const uint32_t offsets[4][2] = {
        { ix0,        iz0        },
        { ix0 + half, iz0        },
        { ix0,        iz0 + half },
        { ix0 + half, iz0 + half },
    };

    int32_t childIndices[4];
    for (int i = 0; i < 4; ++i) {
        childIndices[i] = build(offsets[i][0], offsets[i][1], half,
                                static_cast<uint8_t>(level - 1));
        // PHASE 12 : chainage vers le pere, pose des le retour du fils. Ecrit
        // par INDICE et non par reference : nodes_ vient d'etre reallouee par
        // l'appel recursif ci-dessus (meme piege que plus bas).
        if (childIndices[i] >= 0) {
            nodes_[static_cast<uint32_t>(childIndices[i])].parent =
                static_cast<int32_t>(index);
        }
    }

    // ATTENTION : nodes_ a pu etre REALLOUE par les appels recursifs ci-dessus.
    // Toute reference prise avant eux serait pendante - d'ou la reprise par
    // indice ici, et les blocs a portee courte plus haut.
    Node& node = nodes_[index];
    bool hasChild = false;
    glm::vec3 boundsMin(0.0f);
    glm::vec3 boundsMax(0.0f);
    for (int i = 0; i < 4; ++i) {
        node.children[i] = childIndices[i];
        if (childIndices[i] < 0) {
            continue;
        }
        const Node& child = nodes_[static_cast<uint32_t>(childIndices[i])];
        if (!hasChild) {
            boundsMin = child.boundsMin;
            boundsMax = child.boundsMax;
            hasChild = true;
        } else {
            boundsMin = glm::min(boundsMin, child.boundsMin);
            boundsMax = glm::max(boundsMax, child.boundsMax);
        }
    }

    // Les bornes remontent des fils : exact ET gratuit, alors qu'un balayage par
    // noeud relirait la grille entiere une fois par niveau.
    node.boundsMin = boundsMin;
    node.boundsMax = boundsMax;
    return static_cast<int32_t>(index);
}

void TerrainQuadtree::leafBounds(const Node& node, float* minY, float* maxY) const {
    // Balayage a la PLEINE resolution, et non au pas du noeud : la boite doit
    // englober le relief reel, y compris un pic qui tomberait entre deux
    // echantillons retenus par le maillage. Une boite trop petite ferait
    // disparaitre du terrain au culling.
    const uint32_t ix1 = std::min(node.ix0 + node.span, cells_);
    const uint32_t iz1 = std::min(node.iz0 + node.span, cells_);

    float lo = heightmap_.at(node.ix0, node.iz0);
    float hi = lo;
    for (uint32_t iz = node.iz0; iz <= iz1; ++iz) {
        for (uint32_t ix = node.ix0; ix <= ix1; ++ix) {
            const float h = heightmap_.at(ix, iz);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
    *minY = lo;
    *maxY = hi;
}

void TerrainQuadtree::select(const glm::vec3& cameraPosition, const Frustum& frustum,
                             std::vector<SelectedNode>& out) const {
    out.clear();
    selectRecursive(rootIndex_, cameraPosition, frustum, out);

    // Du plus proche au plus lointain : quand le budget de construction d'une
    // frame est sature, ce sont les tuiles LOINTAINES qui attendent une frame de
    // plus, jamais celles sous les pieds du joueur.
    std::sort(out.begin(), out.end(),
              [](const SelectedNode& a, const SelectedNode& b) {
                  return a.distance < b.distance;
              });
}

void TerrainQuadtree::selectRecursive(uint32_t nodeIndex, const glm::vec3& cameraPosition,
                                      const Frustum& frustum,
                                      std::vector<SelectedNode>& out) const {
    const Node& node = nodes_[nodeIndex];

    // --- Frustum culling ----------------------------------------------------
    // Fait AVANT la descente : un noeud hors champ elimine tout son sous-arbre
    // d'un coup. C'est ce qui rend le culling par quadtree bien moins cher qu'un
    // test par tuile a plat.
    if (!frustum.intersectsAABB(node.boundsMin, node.boundsMax)) {
        return;
    }

    const float distance = distanceToAABB(cameraPosition, node.boundsMin, node.boundsMax);

    // On descend tant que le niveau INFERIEUR a encore de la portee ici.
    const bool refine = (node.level > 0) && (distance < ranges_[node.level - 1]);

    if (refine) {
        bool descended = false;
        for (int i = 0; i < 4; ++i) {
            if (node.children[i] >= 0) {
                selectRecursive(static_cast<uint32_t>(node.children[i]), cameraPosition,
                                frustum, out);
                descended = true;
            }
        }
        if (descended) {
            return;
        }
        // Aucun fils : ne devrait pas arriver (un noeud existe parce qu'il couvre
        // de la grille, donc au moins un quadrant en couvre aussi). On retombe
        // sur le dessin du noeud lui-meme plutot que de laisser un trou.
    }

    SelectedNode selected;
    selected.nodeIndex = nodeIndex;
    selected.level = node.level;
    selected.morphStart = ranges_[node.level] * params_.morphStartRatio;
    selected.morphEnd = ranges_[node.level] * params_.morphEndRatio;
    selected.distance = distance;
    out.push_back(selected);
}

TerrainQuadtree::SelectedNode TerrainQuadtree::describeNode(
    uint32_t nodeIndex, const glm::vec3& cameraPosition) const {
    const Node& node = nodes_[nodeIndex];

    // Exactement le calcul de la fin de selectRecursive : un noeud dessine par
    // repli doit etre decrit comme s'il avait ete selectionne normalement, sans
    // quoi sa bande de morphing differerait de celle qu'il aurait eue une frame
    // plus tot, ce qui se verrait comme un saut de geometrie.
    SelectedNode selected;
    selected.nodeIndex = nodeIndex;
    selected.level = node.level;
    selected.morphStart = ranges_[node.level] * params_.morphStartRatio;
    selected.morphEnd = ranges_[node.level] * params_.morphEndRatio;
    selected.distance = distanceToAABB(cameraPosition, node.boundsMin, node.boundsMax);
    return selected;
}

std::string TerrainQuadtree::describe() const {
    const float leafWorldSize = static_cast<float>(params_.nodeCells) * heightmap_.spacing();
    std::string text =
        "Quadtree terrain : " + std::to_string(nodes_.size()) + " noeuds, " +
        std::to_string(static_cast<int>(rootLevel_) + 1) + " niveaux (0.." +
        std::to_string(static_cast<int>(rootLevel_)) + "), " +
        std::to_string(params_.nodeCells) + " cellules par cote et " +
        std::to_string(trianglesPerNode()) + " triangles par noeud, tuile de niveau 0 = " +
        std::to_string(static_cast<int>(leafWorldSize)) + " m. Portees :";
    for (uint8_t level = 0; level <= rootLevel_; ++level) {
        text += " L" + std::to_string(static_cast<int>(level)) + "<=" +
                std::to_string(static_cast<int>(ranges_[level])) + "m";
    }
    return text;
}

} // namespace vercora
