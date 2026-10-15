#include "renderer/TerrainNodeMesh.hpp"

#include "assets/Heightmap.hpp"
#include "world/TerrainQuadtree.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace vercora {

namespace {

// Position monde + normale d'un point de grille, echantillonne au pas `stride`.
//
// La normale est calculee par difference centree SUR LE PAS DU NOEUD et non sur
// le pas de la donnee : une tuile lointaine doit avoir les normales de la
// surface qu'elle dessine reellement, sinon son eclairage serait bruite par un
// relief que sa geometrie ne represente pas. C'est aussi ce qui rend le
// morphing des normales coherent - a morph = 1, la normale d'un sommet est
// exactement celle que le parent lui donnerait.
struct GridSample {
    glm::vec3 position;
    glm::vec3 normal;
};

GridSample sampleGrid(const Heightmap& heightmap, uint32_t cells,
                      uint32_t gx, uint32_t gz, uint32_t stride) {
    const float spacing = heightmap.spacing();

    const int64_t xm = (gx >= stride) ? static_cast<int64_t>(gx) - stride : gx;
    const int64_t xp = (gx + stride <= cells) ? static_cast<int64_t>(gx) + stride : gx;
    const int64_t zm = (gz >= stride) ? static_cast<int64_t>(gz) - stride : gz;
    const int64_t zp = (gz + stride <= cells) ? static_cast<int64_t>(gz) + stride : gz;

    // Sur le bord, la difference devient unilaterale : le pas reel (xp - xm) au
    // denominateur, et non un 2*stride en dur, sinon les normales du bord
    // seraient deux fois trop plates. Meme raisonnement que TerrainMesh.
    const float dx = static_cast<float>(xp - xm) * spacing;
    const float dz = static_cast<float>(zp - zm) * spacing;

    GridSample sample;
    sample.position = { heightmap.worldCoord(gx),
                        heightmap.at(gx, gz),
                        heightmap.worldCoord(gz) };

    const float dhdx = (dx > 0.0f)
        ? (heightmap.at(xp, gz) - heightmap.at(xm, gz)) / dx : 0.0f;
    const float dhdz = (dz > 0.0f)
        ? (heightmap.at(gx, zp) - heightmap.at(gx, zm)) / dz : 0.0f;

    sample.normal = glm::normalize(glm::vec3(-dhdx, 1.0f, -dhdz));
    return sample;
}

} // namespace

std::vector<uint32_t> terrainNodeIndices(uint32_t nodeCells) {
    if (nodeCells < 1) {
        throw std::runtime_error("TerrainNodeMesh : nodeCells nul.");
    }
    const uint32_t verts = nodeCells + 1;

    std::vector<uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(nodeCells) * nodeCells * 6);

    // Meme decoupe en diagonale v00-v11 et meme enroulement que TerrainMesh :
    // Heightmap::heightAt reproduit cette diagonale pour que la camera marche
    // sur la surface reellement affichee (a la pleine resolution, c'est-a-dire
    // au niveau 0, celui qui entoure le joueur).
    for (uint32_t j = 0; j < nodeCells; ++j) {
        for (uint32_t i = 0; i < nodeCells; ++i) {
            const uint32_t v00 = j * verts + i;
            const uint32_t v10 = v00 + 1;
            const uint32_t v01 = v00 + verts;
            const uint32_t v11 = v01 + 1;

            indices.push_back(v00);
            indices.push_back(v01);
            indices.push_back(v11);

            indices.push_back(v00);
            indices.push_back(v11);
            indices.push_back(v10);
        }
    }
    return indices;
}

namespace {

// PHASE 32 : le parcours d'un noeud, commun aux deux formats de sortie
// (MeshData de reference et TerrainVertex du GPU). `sink(v, gx, gz, here,
// parent)` recoit chaque sommet - son indice dans la tuile, ses indices de
// grille ecretes, son echantillon et celui de sa cible de geomorphing. Un seul
// parcours, donc deux sorties qui ne PEUVENT PAS diverger.
template <typename Sink>
void forEachNodeVertex(const Heightmap& heightmap, const TerrainQuadtree& quadtree,
                       uint32_t nodeIndex, bool reuseParentGrid, Sink&& sink) {
    if (nodeIndex >= quadtree.nodeCount()) {
        throw std::runtime_error("TerrainNodeMesh : indice de noeud hors limites.");
    }

    const TerrainQuadtree::Node& node = quadtree.node(nodeIndex);
    const uint32_t nodeCells = quadtree.nodeCells();
    const uint32_t verts = nodeCells + 1;
    const uint32_t cells = heightmap.resolution() - 1;
    const uint32_t stride = node.stride;

    // --- PHASE 18 : la grille du PARENT, echantillonnee une seule fois ------
    // Le morphing a besoin, pour chaque sommet, du point que le noeud PARENT
    // produirait au meme endroit. Ce point ne depend que des indices locaux
    // ARRONDIS a un multiple de deux : les quatre sommets d'un bloc 2x2 partagent
    // donc exactement la meme cible. La version d'avant la Phase 18 la
    // recalculait pour chacun des quatre - soit trois echantillonnages sur
    // quatre jetes, et un echantillonnage coute cinq lectures de la heightmap
    // plus une normalisation.
    //
    // Le profilage (TerrainStreamer::buildProfile) chiffrait la geometrie a
    // 1858 us/tuile, dont la moitie environ pour ces cibles. La grille
    // grossiere ci-dessous fait 33x33 la ou la tuile en fait 65x65 : quatre fois
    // moins d'echantillons, et un resultat BIT A BIT identique - c'est la meme
    // fonction appelee avec les memes arguments, simplement moins souvent.
    const uint32_t parentVerts = (verts + 1u) / 2u;
    std::vector<GridSample> parentGrid;
    if (reuseParentGrid) {
        parentGrid.resize(static_cast<std::size_t>(parentVerts) * parentVerts);
    }
    for (uint32_t pj = 0; reuseParentGrid && pj < parentVerts; ++pj) {
        for (uint32_t pi = 0; pi < parentVerts; ++pi) {
            // L'ecretage est applique APRES l'arrondi, pour que deux tuiles
            // voisines qui butent sur le meme bord y arrivent a la meme valeur.
            const uint32_t mx = std::min(node.ix0 + (pi * 2u) * stride, cells);
            const uint32_t mz = std::min(node.iz0 + (pj * 2u) * stride, cells);
            // Pas DOUBLE : c'est litteralement ce que le parent produirait.
            parentGrid[static_cast<std::size_t>(pj) * parentVerts + pi] =
                sampleGrid(heightmap, cells, mx, mz, stride * 2u);
        }
    }

    for (uint32_t j = 0; j < verts; ++j) {
        for (uint32_t i = 0; i < verts; ++i) {
            // Indices de grille du sommet, ECRETES sur le bord du terrain.
            const uint32_t gx = std::min(node.ix0 + i * stride, cells);
            const uint32_t gz = std::min(node.iz0 + j * stride, cells);

            const GridSample here = sampleGrid(heightmap, cells, gx, gz, stride);
            // Cible de morphing : indices LOCAUX arrondis vers le bas a un
            // multiple de deux, ce qui donne la grille du niveau superieur -
            // lue dans parentGrid, deja calculee ci-dessus.
            const GridSample parentSample = reuseParentGrid
                ? GridSample{}
                : sampleGrid(heightmap, cells,
                             std::min(node.ix0 + (i & ~1u) * stride, cells),
                             std::min(node.iz0 + (j & ~1u) * stride, cells), stride * 2u);
            const GridSample& parent = reuseParentGrid
                ? parentGrid[static_cast<std::size_t>(j / 2u) * parentVerts + (i / 2u)]
                : parentSample;

            sink(static_cast<std::size_t>(j) * verts + i, gx, gz, here, parent);
        }
    }
}

// Controle de coherence, meme esprit que TerrainMesh : on refuse de pousser
// vers le GPU une geometrie qu'on sait cassee. Le generateur etant
// deterministe, ce test attrape une erreur de CODE (indexation, pas de
// parcours), pas une donnee douteuse.
[[noreturn]] void throwNonFinite() {
    throw std::runtime_error(
        "TerrainNodeMesh : coordonnee ou normale non finie (NaN/infini) "
        "produite par la heightmap.");
}

} // namespace

MeshData terrainNodeMeshData(const Heightmap& heightmap, const TerrainQuadtree& quadtree,
                             uint32_t nodeIndex, bool withIndices,
                             bool reuseParentGrid) {
    if (nodeIndex >= quadtree.nodeCount()) {
        throw std::runtime_error("TerrainNodeMesh : indice de noeud hors limites.");
    }
    const TerrainQuadtree::Node& node = quadtree.node(nodeIndex);
    const uint32_t nodeCells = quadtree.nodeCells();
    const uint32_t verts = nodeCells + 1;
    const uint32_t cells = heightmap.resolution() - 1;

    MeshData mesh;
    mesh.name = "tuile L" + std::to_string(static_cast<int>(node.level)) + " (" +
                std::to_string(node.ix0) + "," + std::to_string(node.iz0) + ")";
    mesh.vertices.resize(static_cast<std::size_t>(verts) * verts);

    forEachNodeVertex(heightmap, quadtree, nodeIndex, reuseParentGrid,
                      [&](std::size_t v, uint32_t gx, uint32_t gz,
                          const GridSample& here, const GridSample& parent) {
        Vertex& vertex = mesh.vertices[v];
        vertex.pos = here.position;
        vertex.normal = here.normal;
        vertex.morphPos = parent.position;
        vertex.morphNormal = parent.normal;

        // Teinte neutre : le fragment shader colore par altitude et par
        // pente. Reserve a une future classification LiDAR par sommet.
        vertex.color = { 1.0f, 1.0f, 1.0f };

        // UV sur TOUT le terrain (et non sur la tuile) : c'est le repere
        // naturel d'une future orthophoto, et il doit rester continu d'une
        // tuile a l'autre.
        vertex.uv = { static_cast<float>(gx) / static_cast<float>(cells),
                      static_cast<float>(gz) / static_cast<float>(cells) };
    });

    if (withIndices) {
        mesh.indices = terrainNodeIndices(nodeCells);
    }

    for (const Vertex& vertex : mesh.vertices) {
        if (!std::isfinite(vertex.pos.x) || !std::isfinite(vertex.pos.y) ||
            !std::isfinite(vertex.pos.z) || !std::isfinite(vertex.normal.y) ||
            !std::isfinite(vertex.morphPos.y)) {
            throwNonFinite();
        }
    }

    return mesh;
}

bool terrainNodeUsesFastPath(const Heightmap& heightmap, const TerrainQuadtree& quadtree,
                             uint32_t nodeIndex) {
    if (nodeIndex >= quadtree.nodeCount()) {
        return false;
    }
    const TerrainQuadtree::Node& node = quadtree.node(nodeIndex);
    const uint64_t cells = heightmap.resolution() - 1;
    const uint64_t stride = node.stride;
    // Deux pas de marge de chaque cote : les voisins d'un sommet sont a un pas,
    // ceux d'un echantillon du parent a deux pas (le pas du parent).
    const uint64_t reach = static_cast<uint64_t>(quadtree.nodeCells() + 2) * stride;
    return node.ix0 >= 2 * stride && node.iz0 >= 2 * stride &&
           node.ix0 + reach <= cells && node.iz0 + reach <= cells;
}

void writeTerrainNodeVertices(const Heightmap& heightmap, const TerrainQuadtree& quadtree,
                              uint32_t nodeIndex, TerrainVertex* out,
                              bool reuseParentGrid) {
    // --- PHASE 32 : chemin RAPIDE des tuiles interieures --------------------
    // Le parcours de reference lit chaque altitude ~6 fois, toujours par at()
    // et son ecretage : cinq lectures par echantillon (le point et ses quatre
    // voisins) pour les 4 225 sommets ET les 1 089 echantillons du parent. Or
    // les voisins d'un sommet, au pas du noeud, sont exactement les sommets
    // voisins de la tuile, et ceux du parent, au pas double, les sommets a
    // deux rangs. Loin du bord, ou aucun indice n'est ecrete, une grille de
    // (n + 5)^2 altitudes lues UNE fois contient donc tout.
    //
    // Les MEMES operations flottantes sur les MEMES valeurs que sampleGrid -
    // memes differences, memes divisions (le pas reel, 2 ou 4 fois le pas du
    // noeud, comme (xp - xm) la-bas), meme glm::normalize : le resultat est
    // identique au bit pres, et tests/logic_tests.cpp le verifie sur toutes les
    // tuiles de plusieurs grilles. Pres du bord, ou l'ecretage change les
    // voisins, le chemin de reference ci-dessous reste seul juge. Le temoin
    // --no-tile-cache (reuseParentGrid = false) le garde aussi : il mesure le
    // travail d'avant la Phase 18.
    if (reuseParentGrid && terrainNodeUsesFastPath(heightmap, quadtree, nodeIndex)) {
        const TerrainQuadtree::Node& node = quadtree.node(nodeIndex);
        const uint32_t verts = quadtree.nodeCells() + 1;
        const uint32_t stride = node.stride;
        const uint32_t res = heightmap.resolution();
        const float spacing = heightmap.spacing();
        const float* heights = heightmap.rawHeights();

        // Grille bordee de deux rangs : l'indice local k correspond a
        // l'echantillon ix0 + (k - 2) * stride. thread_local : le chemin
        // parallele (Phase 19) l'appelle depuis plusieurs threads a la fois.
        const uint32_t span = verts + 4;
        thread_local std::vector<float> grid;
        grid.resize(static_cast<std::size_t>(span) * span);
        for (uint32_t jj = 0; jj < span; ++jj) {
            const float* row = heights + static_cast<std::size_t>(node.iz0 - 2u * stride +
                                                                  jj * stride) * res;
            const float* sample = row + (node.ix0 - 2u * stride);
            float* destination = grid.data() + static_cast<std::size_t>(jj) * span;
            for (uint32_t ii = 0; ii < span; ++ii) {
                destination[ii] = sample[static_cast<std::size_t>(ii) * stride];
            }
        }
        const auto at = [&](uint32_t ii, uint32_t jj) {
            return grid[static_cast<std::size_t>(jj) * span + ii];
        };
        // Les pas reels des differences centrees : (xp - xm) * spacing dans
        // sampleGrid, soit 2 pas du noeud pour un sommet, 4 pour le parent.
        const float step = static_cast<float>(2u * stride) * spacing;
        const float parentStep = static_cast<float>(4u * stride) * spacing;

        // Grille du parent, deja quantifiee : une fois par bloc 2 x 2.
        struct ParentSample {
            float height;
            int16_t normal[2];
        };
        const uint32_t parentVerts = (verts + 1u) / 2u;
        thread_local std::vector<ParentSample> parents;
        parents.resize(static_cast<std::size_t>(parentVerts) * parentVerts);
        bool finite = true;
        for (uint32_t pj = 0; pj < parentVerts; ++pj) {
            for (uint32_t pi = 0; pi < parentVerts; ++pi) {
                const uint32_t ii = 2u * pi + 2u;
                const uint32_t jj = 2u * pj + 2u;
                const float dhdx = (at(ii + 2u, jj) - at(ii - 2u, jj)) / parentStep;
                const float dhdz = (at(ii, jj + 2u) - at(ii, jj - 2u)) / parentStep;
                const glm::vec3 normal = glm::normalize(glm::vec3(-dhdx, 1.0f, -dhdz));
                ParentSample& parent = parents[static_cast<std::size_t>(pj) * parentVerts + pi];
                parent.height = at(ii, jj);
                parent.normal[0] = packSnorm16(normal.x);
                parent.normal[1] = packSnorm16(normal.z);
                finite = finite && std::isfinite(parent.height) && std::isfinite(normal.y);
            }
        }
        for (uint32_t j = 0; j < verts; ++j) {
            for (uint32_t i = 0; i < verts; ++i) {
                const uint32_t ii = i + 2u;
                const uint32_t jj = j + 2u;
                const float dhdx = (at(ii + 1u, jj) - at(ii - 1u, jj)) / step;
                const float dhdz = (at(ii, jj + 1u) - at(ii, jj - 1u)) / step;
                const glm::vec3 normal = glm::normalize(glm::vec3(-dhdx, 1.0f, -dhdz));
                const ParentSample& parent =
                    parents[static_cast<std::size_t>(j / 2u) * parentVerts + (i / 2u)];

                TerrainVertex& packed = out[static_cast<std::size_t>(j) * verts + i];
                packed.height = at(ii, jj);
                packed.morphHeight = parent.height;
                packed.normal[0] = packSnorm16(normal.x);
                packed.normal[1] = packSnorm16(normal.z);
                packed.morphNormal[0] = parent.normal[0];
                packed.morphNormal[1] = parent.normal[1];
                finite = finite && std::isfinite(packed.height) && std::isfinite(normal.y);
            }
        }
        if (!finite) {
            throwNonFinite();
        }
        return;
    }

    bool finite = true;
    forEachNodeVertex(heightmap, quadtree, nodeIndex, reuseParentGrid,
                      [&](std::size_t v, uint32_t /*gx*/, uint32_t /*gz*/,
                          const GridSample& here, const GridSample& parent) {
        // Les memes octets que toTerrainVertex(Vertex) appliquee au sommet de
        // terrainNodeMeshData : altitudes recopiees, normales quantifiees.
        TerrainVertex packed;
        packed.height = here.position.y;
        packed.morphHeight = parent.position.y;
        packed.normal[0] = packSnorm16(here.normal.x);
        packed.normal[1] = packSnorm16(here.normal.z);
        packed.morphNormal[0] = packSnorm16(parent.normal.x);
        packed.morphNormal[1] = packSnorm16(parent.normal.z);
        out[v] = packed;
        finite = finite && std::isfinite(here.position.y) && std::isfinite(here.normal.y) &&
                 std::isfinite(parent.position.y);
    });
    if (!finite) {
        throwNonFinite();
    }
}

} // namespace vercora
