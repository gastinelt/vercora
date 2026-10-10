#include "editor/AssetCatalog.hpp"

#include "assets/ObjLoader.hpp"
#include "core/Logger.hpp"
#include "editor/EditorAssets.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <limits>
#include <sstream>

namespace vercora {
namespace {

// Garde-fous de taille, en unites monde (= metres). Un modele hors de ces bornes
// n'est pas refuse - il peut etre volontaire - mais il est SIGNALE.
//
// Raison d'etre : la premiere cause d'un asset "invisible" ou "gigantesque" a
// l'import est une unite differente (un .obj exporte en centimetres est 100 fois
// trop grand, un .obj en pouces 39 fois). Sans ce message, le symptome observe
// est "j'ai pose un rocher et je ne le vois pas", et on va chercher le defaut
// dans le placement ou le rendu, jamais dans le fichier.
constexpr float kSuspiciouslySmall = 0.05f;
constexpr float kSuspiciouslyLarge = 200.0f;

std::string toLowerAscii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

} // namespace

std::string sanitizeAssetId(const std::string& raw) {
    std::string result;
    result.reserve(raw.size());
    for (const unsigned char c : raw) {
        if (std::isalnum(c) != 0) {
            result.push_back(static_cast<char>(std::tolower(c)));
        } else if (c == '_' || c == '-' || c == '.') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('_');
        }
    }
    if (result.empty()) {
        result = "asset";
    }
    return result;
}

void normalizeAssetMesh(MeshData& mesh) {
    if (mesh.vertices.empty()) {
        return;
    }

    glm::vec3 minCorner(std::numeric_limits<float>::max());
    glm::vec3 maxCorner(std::numeric_limits<float>::lowest());
    for (const Vertex& vertex : mesh.vertices) {
        minCorner = glm::min(minCorner, vertex.pos);
        maxCorner = glm::max(maxCorner, vertex.pos);
    }

    // X et Z : centre sur la boite. Y : pied a zero, PAS centre - c'est toute la
    // convention d'ancrage (voir editor/EditorAssets.hpp).
    const glm::vec3 shift(-0.5f * (minCorner.x + maxCorner.x),
                          -minCorner.y,
                          -0.5f * (minCorner.z + maxCorner.z));
    if (glm::length(shift) < 1e-6f) {
        return; // deja aligne : ne pas toucher aux valeurs, meme d'un ulp
    }
    for (Vertex& vertex : mesh.vertices) {
        vertex.pos += shift;
        // morphPos n'est PAS decale : il ne concerne que les tuiles du quadtree
        // (Phase 10) et vaut zero sur toute autre geometrie, ou il est ignore
        // parce que la bande de morphing du DrawItem est vide.
    }
}

void computeAssetBounds(AssetEntry& entry) {
    entry.collisionRadius = 0.0f;
    entry.height = 0.0f;
    entry.halfExtentX = 0.0f;
    entry.halfExtentZ = 0.0f;
    if (entry.mesh.vertices.empty()) {
        return;
    }

    glm::vec3 minCorner(std::numeric_limits<float>::max());
    glm::vec3 maxCorner(std::numeric_limits<float>::lowest());
    for (const Vertex& vertex : entry.mesh.vertices) {
        minCorner = glm::min(minCorner, vertex.pos);
        maxCorner = glm::max(maxCorner, vertex.pos);
    }

    entry.halfExtentX = std::max(std::abs(minCorner.x), std::abs(maxCorner.x));
    entry.halfExtentZ = std::max(std::abs(minCorner.z), std::abs(maxCorner.z));
    entry.height = maxCorner.y - std::min(0.0f, minCorner.y);
    // Cylindre CONTENANT : le rayon est la demi-diagonale horizontale, sans quoi
    // les coins de la boite depasseraient et l'objet se traverserait en biais.
    entry.collisionRadius = std::sqrt(entry.halfExtentX * entry.halfExtentX +
                                      entry.halfExtentZ * entry.halfExtentZ);
}

const std::vector<std::string>& AssetCatalog::supportedExtensions() {
    // PHASE 15 : Wavefront .obj uniquement, par le loader deja en place depuis la
    // Phase 3. Le glTF (.gltf / .glb) est l'ajout evident pour une bibliotheque
    // d'assets libres de droits - voir la limite documentee dans
    // NOTES-PHASE15.md. Cette liste est le SEUL endroit a etendre le jour ou un
    // second loader existe.
    static const std::vector<std::string> extensions = { ".obj" };
    return extensions;
}

bool AssetCatalog::addEntry(AssetEntry entry) {
    if (entry.mesh.empty()) {
        logWarn("Catalogue : asset \"" + entry.id + "\" sans geometrie, ignore.");
        return false;
    }
    if (indexOf(entry.id) >= 0) {
        logWarn("Catalogue : identifiant \"" + entry.id +
                "\" deja pris, ce modele est ignore" +
                (entry.sourcePath.empty() ? "" : " (" + entry.sourcePath + ")") +
                ". Renommer le fichier suffit a le reintegrer.");
        return false;
    }

    computeAssetBounds(entry);
    const float footprint = 2.0f * std::max(entry.halfExtentX, entry.halfExtentZ);
    const float largest = std::max(footprint, entry.height);
    if (largest < kSuspiciouslySmall || largest > kSuspiciouslyLarge) {
        logWarn("Catalogue : \"" + entry.id + "\" mesure " +
                std::to_string(largest) +
                " m dans sa plus grande dimension. Unites du fichier suspectes "
                "(centimetres ? pouces ?). Il reste placable, l'echelle se "
                "corrige objet par objet.");
    }

    entries_.push_back(std::move(entry));
    return true;
}

void AssetCatalog::addBuiltins() {
    for (const BuiltinAsset& builtin : builtinEditorAssets()) {
        AssetEntry entry;
        entry.id = builtin.id;
        entry.description = builtin.description + " [genere en code]";
        entry.mesh = builtin.build();
        // Pas de normalizeAssetMesh ici, volontairement : les builtins
        // respectent deja la convention d'ancrage par construction, et un test
        // headless le verifie. Les normaliser quand meme masquerait une derive
        // de leur generateur au lieu de la faire echouer.
        addEntry(std::move(entry));
    }
}

uint32_t AssetCatalog::scanDirectory(const std::string& directory) {
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(directory, ec) || !fs::is_directory(directory, ec)) {
        logInfo("Catalogue : aucun dossier d'assets a " + directory +
                " (seuls les modeles generes en code sont disponibles). "
                "Y deposer des .obj suffit a les voir apparaitre au prochain "
                "demarrage de l'editeur.");
        return 0;
    }

    // Collecte puis TRI par nom : voir la note d'en-tete sur les touches 1..9.
    std::vector<fs::path> files;
    for (const fs::directory_entry& item : fs::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (!item.is_regular_file(ec) || ec) {
            continue;
        }
        const std::string extension = toLowerAscii(item.path().extension().string());
        const auto& supported = supportedExtensions();
        if (std::find(supported.begin(), supported.end(), extension) != supported.end()) {
            files.push_back(item.path());
        }
    }
    std::sort(files.begin(), files.end());

    uint32_t added = 0;
    for (const fs::path& file : files) {
        AssetEntry entry;
        entry.id = sanitizeAssetId(file.stem().string());
        entry.sourcePath = file.string();
        try {
            entry.mesh = loadObjMesh(file.string());
        } catch (const std::exception& error) {
            // Un modele casse ne prive pas l'editeur des autres.
            logWarn("Catalogue : \"" + file.filename().string() +
                    "\" illisible, ignore. " + error.what());
            continue;
        }
        normalizeAssetMesh(entry.mesh);
        entry.description = file.filename().string();
        if (addEntry(std::move(entry))) {
            ++added;
        }
    }

    logInfo("Catalogue : " + std::to_string(added) + " modele(s) charge(s) depuis " +
            directory + " sur " + std::to_string(files.size()) + " fichier(s) reconnu(s).");
    return added;
}

int AssetCatalog::indexOf(const std::string& id) const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string AssetCatalog::listing(int selected) const {
    std::ostringstream out;
    out << "Catalogue d'assets (" << entries_.size() << ") :";
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const AssetEntry& entry = entries_[i];
        out << "\n  " << (static_cast<int>(i) == selected ? "> " : "  ")
            << (i + 1) << ". " << entry.id
            << "  [" << entry.description << "]"
            << "  h=" << entry.height << " m, r=" << entry.collisionRadius << " m";
    }
    if (entries_.empty()) {
        out << "\n  (vide)";
    }
    return out.str();
}

} // namespace vercora
