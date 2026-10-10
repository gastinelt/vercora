#include "renderer/TerrainMesh.hpp"

#include "assets/Heightmap.hpp"
#include "core/Logger.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace vercora {

MeshData terrainMeshData(const Heightmap& heightmap) {
    const uint32_t res = heightmap.resolution();
    if (res < 2) {
        throw std::runtime_error(
            "TerrainMesh : une grille de " + std::to_string(res) +
            " echantillons par cote ne forme aucune cellule.");
    }

    const uint32_t cells = res - 1;
    const float spacing = heightmap.spacing();

    MeshData mesh;
    mesh.name = heightmap.describe();
    mesh.vertices.reserve(static_cast<std::size_t>(res) * res);
    mesh.indices.reserve(static_cast<std::size_t>(cells) * cells * 6);

    // --- 1. Sommets ---------------------------------------------------------
    // Un sommet par echantillon, PARTAGE entre les triangles voisins : c'est ce
    // partage qui rend le terrain lisse (normales moyennees implicitement) et
    // qui divise par ~6 la taille du vertex buffer par rapport a des triangles
    // independants. L'inverse du cube, dont chaque face voulait ses propres
    // normales pour garder des aretes vives.
    //
    // Les positions sont en espace MONDE des maintenant : la model matrix reste
    // l'IDENTITE pour le terrain (voir RenderOptions::spinModel). C'est un choix
    // assume pour cette phase - il evite la question de la normal matrix, qui se
    // poserait des qu'on appliquerait au terrain un facteur d'echelle vertical
    // different de l'horizontal (dans ce cas mat3(model) est FAUX : il faudrait
    // transpose(inverse(mat3(model))), calcule cote CPU et passe par l'UBO).
    // Toute mise a l'echelle du relief se fait donc dans la Heightmap, en amont.
    for (uint32_t iz = 0; iz < res; ++iz) {
        for (uint32_t ix = 0; ix < res; ++ix) {
            Vertex vertex{};

            vertex.pos = { heightmap.worldCoord(ix),
                           heightmap.at(ix, iz),
                           heightmap.worldCoord(iz) };

            // Teinte neutre : le fragment shader du terrain colore par altitude
            // et par pente, cette valeur ne fait que le multiplier. Elle reste
            // disponible pour, plus tard, marquer des zones (classification
            // LiDAR : sol / vegetation / bati) sans toucher au shader.
            vertex.color = { 1.0f, 1.0f, 1.0f };

            // Normale analytique de la surface y = h(x,z) : le vecteur normal
            // vaut (-dh/dx, 1, -dh/dz). Les derivees sont estimees par
            // difference CENTREE sur les deux voisins (plus precis et plus
            // symetrique qu'un produit vectoriel sur une seule face).
            // Sur les bords, at() ecrete les indices : la difference devient
            // unilaterale, d'ou le pas reel (xp - xm) au denominateur plutot
            // qu'un 2 en dur - sinon les normales du bord seraient deux fois
            // trop plates.
            const int64_t xm = (ix > 0) ? ix - 1 : ix;
            const int64_t xp = (ix + 1 < res) ? ix + 1 : ix;
            const int64_t zm = (iz > 0) ? iz - 1 : iz;
            const int64_t zp = (iz + 1 < res) ? iz + 1 : iz;

            const float dhdx = (heightmap.at(xp, iz) - heightmap.at(xm, iz)) /
                               (static_cast<float>(xp - xm) * spacing);
            const float dhdz = (heightmap.at(ix, zp) - heightmap.at(ix, zm)) /
                               (static_cast<float>(zp - zm) * spacing);

            vertex.normal = glm::normalize(glm::vec3(-dhdx, 1.0f, -dhdz));

            // UV sur tout le terrain. Pas encore consomme (pas de texturing),
            // mais c'est le repere naturel pour y plaquer une orthophoto le jour
            // ou les vraies donnees arriveront.
            vertex.uv = { static_cast<float>(ix) / static_cast<float>(cells),
                          static_cast<float>(iz) / static_cast<float>(cells) };

            mesh.vertices.push_back(vertex);
        }
    }

    // --- 2. Indices ---------------------------------------------------------
    // Chaque cellule donne deux triangles, coupes le long de la diagonale
    // v00-v11. Heightmap::heightAt reproduit EXACTEMENT ce decoupage pour que
    // la camera marche sur la surface reellement affichee : changer la diagonale
    // ici oblige a la changer la-bas aussi.
    //
    // Enroulement anti-horaire vu de dessus (+Y), coherent avec la convention
    // des .obj de la Phase 3. Le culling est desactive dans Pipeline, donc c'est
    // sans effet visible aujourd'hui, mais ca rend le back-face culling
    // activable sans retoucher la geometrie.
    for (uint32_t iz = 0; iz < cells; ++iz) {
        for (uint32_t ix = 0; ix < cells; ++ix) {
            const uint32_t v00 = iz * res + ix;
            const uint32_t v10 = v00 + 1;
            const uint32_t v01 = v00 + res;
            const uint32_t v11 = v01 + 1;

            mesh.indices.push_back(v00);
            mesh.indices.push_back(v01);
            mesh.indices.push_back(v11);

            mesh.indices.push_back(v00);
            mesh.indices.push_back(v11);
            mesh.indices.push_back(v10);
        }
    }

    // --- 3. Controle de coherence ------------------------------------------
    // Meme esprit que les gardes-fous du loader .obj (Phase 3) : on refuse de
    // pousser vers le GPU une geometrie qu'on sait cassee, plutot que d'afficher
    // une fenetre vide sans explication. Ici le generateur est deterministe,
    // donc ces tests attrapent une erreur de code (indexation, resolution), pas
    // une donnee douteuse - ils resteront pertinents quand la grille viendra du
    // LiDAR.
    if (mesh.indices.size() % 3 != 0) {
        throw std::runtime_error("TerrainMesh : nombre d'index non multiple de 3.");
    }
    for (uint32_t index : mesh.indices) {
        if (index >= mesh.vertices.size()) {
            throw std::runtime_error(
                "TerrainMesh : index " + std::to_string(index) + " hors des " +
                std::to_string(mesh.vertices.size()) + " sommets generes.");
        }
    }
    for (const Vertex& vertex : mesh.vertices) {
        if (!std::isfinite(vertex.pos.x) || !std::isfinite(vertex.pos.y) ||
            !std::isfinite(vertex.pos.z) || !std::isfinite(vertex.normal.y)) {
            throw std::runtime_error(
                "TerrainMesh : coordonnee ou normale non finie (NaN/infini) "
                "produite par la heightmap.");
        }
    }

    logInfo("Maillage terrain genere : " + std::to_string(mesh.vertices.size()) +
            " sommets, " + std::to_string(mesh.triangleCount()) + " triangles, "
            "emprise " + std::to_string(heightmap.worldSize()) + " x " +
            std::to_string(heightmap.maxHeight() - heightmap.minHeight()) + " x " +
            std::to_string(heightmap.worldSize()) + ".");

    return mesh;
}

} // namespace vercora
