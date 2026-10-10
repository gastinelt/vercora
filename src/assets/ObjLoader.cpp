#include "assets/ObjLoader.hpp"

#include "core/Logger.hpp"

// tinyobjloader est header-only : l'implementation doit etre compilee dans
// EXACTEMENT une unite de traduction, celle-ci.
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace vercora {
namespace {

// Cle de deduplication : un sommet .obj est un TRIPLET d'indices independants
// (position / UV / normale). Deux faces ne partagent un sommet de vertex buffer
// que si les trois coincident : deux faces qui partagent une position mais ont
// des normales differentes (arete vive) doivent rester deux sommets distincts.
struct AttribKey {
    int position = -1;
    int uv = -1;
    int normal = -1;

    bool operator==(const AttribKey& other) const = default;
};

struct AttribKeyHash {
    std::size_t operator()(const AttribKey& key) const noexcept {
        // Melange multiplicatif simple (constante du nombre d'or 64 bits) :
        // suffisant ici, les indices sont petits et bien repartis.
        constexpr std::uint64_t kMix = 0x9E3779B97F4A7C15ull;
        std::uint64_t h = static_cast<std::uint32_t>(key.position);
        h = (h + static_cast<std::uint32_t>(key.uv)) * kMix;
        h = (h + static_cast<std::uint32_t>(key.normal)) * kMix;
        return static_cast<std::size_t>(h);
    }
};

// Supprime les retours a la ligne finaux des messages de tinyobjloader, qui
// sinon cassent le formatage du logger.
std::string trimTrailingNewlines(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

} // namespace

MeshData loadObjMesh(const std::string& path) {
    namespace fs = std::filesystem;

    // --- 1. Verifications de fichier, avant meme d'invoquer le parseur -------
    // On veut un message qui distingue "chemin faux" de "contenu invalide".
    std::error_code ec;
    const fs::path filePath(path);

    if (!fs::exists(filePath, ec) || ec) {
        throw std::runtime_error(
            "Fichier .obj introuvable : \"" + path + "\" (chemin absolu teste : " +
            fs::absolute(filePath, ec).string() + ").");
    }
    if (!fs::is_regular_file(filePath, ec) || ec) {
        throw std::runtime_error(
            "Le chemin \"" + path + "\" existe mais n'est pas un fichier.");
    }
    if (fs::file_size(filePath, ec) == 0 || ec) {
        throw std::runtime_error("Fichier .obj vide : \"" + path + "\".");
    }

    // --- 2. Parsing ---------------------------------------------------------
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn;
    std::string err;

    // mtl_basedir = dossier du .obj, pour qu'un eventuel materiau reference
    // soit cherche a cote du fichier et non dans le repertoire courant.
    const std::string baseDir = filePath.parent_path().string();

    // triangulate = true : les quads et n-gones sont decoupes en triangles, le
    // seul type de primitive que le pipeline accepte (TRIANGLE_LIST).
    const bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
                                     path.c_str(),
                                     baseDir.empty() ? nullptr : baseDir.c_str(),
                                     /*triangulate=*/true);

    if (!warn.empty()) {
        // Typiquement un .mtl manquant : non bloquant, on n'utilise pas encore
        // les materiaux.
        logWarn("tinyobjloader (" + filePath.filename().string() + ") : " +
                trimTrailingNewlines(warn));
    }
    if (!ok) {
        throw std::runtime_error("Fichier .obj invalide : \"" + path + "\" - " +
                                 trimTrailingNewlines(err.empty() ? "erreur inconnue" : err));
    }
    if (shapes.empty() || attrib.vertices.empty()) {
        throw std::runtime_error(
            "Fichier .obj lu mais sans geometrie exploitable : \"" + path +
            "\" (aucune face ou aucun sommet).");
    }

    // --- 3. Construction du MeshData ---------------------------------------
    const std::size_t positionCount = attrib.vertices.size() / 3;
    const std::size_t normalCount = attrib.normals.size() / 3;
    const std::size_t uvCount = attrib.texcoords.size() / 2;
    // attrib.colors est rempli par tinyobjloader (blanc par defaut) et a la
    // meme taille que attrib.vertices ; on se protege quand meme.
    const bool hasColors = attrib.colors.size() == attrib.vertices.size();

    MeshData mesh;
    mesh.name = filePath.filename().string();

    std::unordered_map<AttribKey, uint32_t, AttribKeyHash> uniqueVertices;
    std::size_t verticesWithoutNormal = 0;
    bool anyUv = false;

    for (const tinyobj::shape_t& shape : shapes) {
        for (const tinyobj::index_t& idx : shape.mesh.indices) {
            if (idx.vertex_index < 0 ||
                static_cast<std::size_t>(idx.vertex_index) >= positionCount) {
                throw std::runtime_error(
                    "Fichier .obj invalide : \"" + path + "\" - index de position " +
                    std::to_string(idx.vertex_index) + " hors des " +
                    std::to_string(positionCount) + " positions declarees.");
            }

            // Un indice de normale/UV present mais hors limites est une erreur ;
            // absent (-1) est un cas legitime, traite plus bas.
            if (idx.normal_index >= 0 &&
                static_cast<std::size_t>(idx.normal_index) >= normalCount) {
                throw std::runtime_error(
                    "Fichier .obj invalide : \"" + path + "\" - index de normale " +
                    std::to_string(idx.normal_index) + " hors limites.");
            }
            if (idx.texcoord_index >= 0 &&
                static_cast<std::size_t>(idx.texcoord_index) >= uvCount) {
                throw std::runtime_error(
                    "Fichier .obj invalide : \"" + path + "\" - index d'UV " +
                    std::to_string(idx.texcoord_index) + " hors limites.");
            }

            const AttribKey key{ idx.vertex_index, idx.texcoord_index, idx.normal_index };

            const auto [it, inserted] =
                uniqueVertices.try_emplace(key, static_cast<uint32_t>(mesh.vertices.size()));

            if (inserted) {
                Vertex vertex{};
                const std::size_t v = static_cast<std::size_t>(idx.vertex_index) * 3;
                vertex.pos = { attrib.vertices[v + 0],
                               attrib.vertices[v + 1],
                               attrib.vertices[v + 2] };

                vertex.color = hasColors ? glm::vec3{ attrib.colors[v + 0],
                                                      attrib.colors[v + 1],
                                                      attrib.colors[v + 2] }
                                         : glm::vec3{ 1.0f, 1.0f, 1.0f };

                if (idx.normal_index >= 0) {
                    const std::size_t n = static_cast<std::size_t>(idx.normal_index) * 3;
                    vertex.normal = { attrib.normals[n + 0],
                                      attrib.normals[n + 1],
                                      attrib.normals[n + 2] };
                } else {
                    // Marqueur : normale a calculer une fois toutes les faces
                    // connues (etape 4).
                    vertex.normal = { 0.0f, 0.0f, 0.0f };
                    ++verticesWithoutNormal;
                }

                if (idx.texcoord_index >= 0) {
                    const std::size_t t = static_cast<std::size_t>(idx.texcoord_index) * 2;
                    // .obj a l'origine des UV en bas a gauche, Vulkan en haut a
                    // gauche : on inverse V des maintenant pour que la donnee
                    // stockee soit deja dans la convention du moteur.
                    vertex.uv = { attrib.texcoords[t + 0],
                                  1.0f - attrib.texcoords[t + 1] };
                    anyUv = true;
                }

                mesh.vertices.push_back(vertex);
            }

            mesh.indices.push_back(it->second);
        }
    }

    if (mesh.indices.empty()) {
        throw std::runtime_error(
            "Fichier .obj sans face exploitable : \"" + path + "\".");
    }
    if (mesh.indices.size() % 3 != 0) {
        // Ne devrait pas arriver avec triangulate=true, mais mieux vaut le dire
        // que de laisser passer une geometrie tronquee.
        throw std::runtime_error(
            "Fichier .obj : la triangulation a produit " +
            std::to_string(mesh.indices.size()) +
            " index, ce qui n'est pas un multiple de 3 (\"" + path + "\").");
    }

    // --- 4. Normales manquantes : moyenne des faces adjacentes --------------
    // Un .obj sans 'vn' est courant. Sans normale, le shader Blinn-Phong de la
    // Phase 2 rendrait l'objet uniformement noir (produit scalaire nul), donc on
    // les reconstruit. Les sommets ayant deja une normale ne sont pas touches.
    if (verticesWithoutNormal > 0) {
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            Vertex& a = mesh.vertices[mesh.indices[i + 0]];
            Vertex& b = mesh.vertices[mesh.indices[i + 1]];
            Vertex& c = mesh.vertices[mesh.indices[i + 2]];

            // Normale geometrique de la face. L'enroulement du .obj etant
            // anti-horaire par convention, ce produit vectoriel pointe vers
            // l'exterieur.
            const glm::vec3 faceNormal = glm::cross(b.pos - a.pos, c.pos - a.pos);

            for (Vertex* vertex : { &a, &b, &c }) {
                if (vertex->normal == glm::vec3(0.0f)) {
                    // Non normalise volontairement : la longueur du produit
                    // vectoriel vaut le double de l'aire du triangle, ce qui
                    // pondere la moyenne par l'aire (les grandes faces pesent
                    // plus, resultat plus lisse).
                    vertex->normal += faceNormal;
                }
            }
        }

        // Normalisation finale, une seule fois par sommet.
        for (Vertex& vertex : mesh.vertices) {
            const float lengthSquared = glm::dot(vertex.normal, vertex.normal);
            if (lengthSquared > 1e-12f) {
                vertex.normal /= std::sqrt(lengthSquared);
            } else {
                // Sommet isole ou faces degenerees : valeur par defaut plutot
                // qu'une division par zero (qui donnerait des NaN a l'ecran).
                vertex.normal = { 0.0f, 1.0f, 0.0f };
            }
        }
    }

    // --- 5. Controle de coherence geometrique -------------------------------
    // tinyobjloader est tres permissif : un token non numerique ("v abc def")
    // est lu comme 0.0 sans erreur, et un tel fichier produirait une geometrie
    // degeneree, invisible a l'ecran, sans aucun message. On mesure donc la
    // boite englobante : elle sert de garde-fou ET d'information utile (echelle
    // du modele, indispensable pour cadrer la camera sur du terrain plus tard).
    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());

    for (const Vertex& vertex : mesh.vertices) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(vertex.pos[axis])) {
                throw std::runtime_error(
                    "Fichier .obj invalide : \"" + path +
                    "\" - coordonnee non finie (NaN ou infini) dans les positions.");
            }
        }
        minBounds = glm::min(minBounds, vertex.pos);
        maxBounds = glm::max(maxBounds, vertex.pos);
    }

    const glm::vec3 extent = maxBounds - minBounds;
    if (glm::dot(extent, extent) <= 0.0f) {
        throw std::runtime_error(
            "Fichier .obj sans geometrie visible : \"" + path +
            "\" - tous les sommets sont confondus (boite englobante de taille "
            "nulle). Fichier probablement mal forme : tinyobjloader lit toute "
            "valeur non numerique comme 0.");
    }

    logInfo("Boite englobante de " + mesh.name + " : " +
            std::to_string(extent.x) + " x " + std::to_string(extent.y) + " x " +
            std::to_string(extent.z) + ".");

    logInfo("Charge " + mesh.name + " : " + std::to_string(mesh.vertices.size()) +
            " sommets uniques, " + std::to_string(mesh.triangleCount()) + " triangles" +
            (anyUv ? ", UV lus" : ", pas d'UV") +
            (verticesWithoutNormal > 0
                 ? ", " + std::to_string(verticesWithoutNormal) + " normales generees"
                 : ", normales lues") +
            ".");

    return mesh;
}

} // namespace vercora
