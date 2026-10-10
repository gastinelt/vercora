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

    MeshData mesh;
    mesh.name = "tuile L" + std::to_string(static_cast<int>(node.level)) + " (" +
                std::to_string(node.ix0) + "," + std::to_string(node.iz0) + ")";
    mesh.vertices.resize(static_cast<std::size_t>(verts) * verts);

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

            Vertex& vertex = mesh.vertices[static_cast<std::size_t>(j) * verts + i];
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
        }
    }

    if (withIndices) {
        mesh.indices = terrainNodeIndices(nodeCells);
    }

    // Controle de coherence, meme esprit que TerrainMesh : on refuse de pousser
    // vers le GPU une geometrie qu'on sait cassee. Le generateur etant
    // deterministe, ce test attrape une erreur de CODE (indexation, pas de
    // parcours), pas une donnee douteuse.
    for (const Vertex& vertex : mesh.vertices) {
        if (!std::isfinite(vertex.pos.x) || !std::isfinite(vertex.pos.y) ||
            !std::isfinite(vertex.pos.z) || !std::isfinite(vertex.normal.y) ||
            !std::isfinite(vertex.morphPos.y)) {
            throw std::runtime_error(
                "TerrainNodeMesh : coordonnee ou normale non finie (NaN/infini) "
                "produite par la heightmap.");
        }
    }

    return mesh;
}

} // namespace vercora
