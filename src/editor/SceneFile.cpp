#include "editor/SceneFile.hpp"

#include "assets/Heightmap.hpp"
#include "core/Logger.hpp"

#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace vercora {
namespace {

constexpr uint32_t kFormatVersion = 1;

// Retire le commentaire (tout ce qui suit '#') et les blancs de bord.
std::string stripComment(const std::string& line) {
    const std::size_t hash = line.find('#');
    std::string body = (hash == std::string::npos) ? line : line.substr(0, hash);
    const std::size_t first = body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = body.find_last_not_of(" \t\r\n");
    return body.substr(first, last - first + 1);
}

// Flux de lecture en locale CLASSIQUE.
//
// Ce n'est pas de la precaution gratuite : dans une locale francaise, le
// separateur decimal est la VIRGULE, et un `907512.250` se relirait alors comme
// 907512 - l'objet se retrouverait a 25 cm de la, sans le moindre message. Le
// jeu ne change jamais de locale aujourd'hui, mais un fichier de donnees doit
// se relire a l'identique quoi qu'il arrive autour.
void useClassicLocale(std::ios_base& stream) { stream.imbue(std::locale::classic()); }

} // namespace

SceneLoadResult loadSceneFile(const std::string& path, const Heightmap& terrain) {
    SceneLoadResult result;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return result; // fileExisted reste false : ce n'est pas une erreur
    }
    result.fileExisted = true;

    std::ifstream file(path);
    if (!file) {
        result.error = "fichier illisible";
        return result;
    }
    useClassicLocale(file);

    const float halfSize = 0.5f * terrain.worldSize();
    bool headerSeen = false;
    uint32_t lineNumber = 0;
    std::string line;

    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string body = stripComment(line);
        if (body.empty()) {
            continue;
        }

        std::istringstream fields(body);
        useClassicLocale(fields);
        std::string directive;
        fields >> directive;

        if (directive == "version") {
            uint32_t version = 0;
            if (!(fields >> version)) {
                result.error = "ligne " + std::to_string(lineNumber) +
                               " : version illisible";
                return result;
            }
            result.version = version;
            headerSeen = true;
            if (version > kFormatVersion) {
                // On CONTINUE : les directives inconnues sont ignorees une a
                // une, donc un fichier plus recent se lit partiellement au lieu
                // d'etre refuse en bloc. C'est le comportement utile pour un
                // outil dont le format bougera encore.
                logWarn("Scene : le fichier est en version " + std::to_string(version) +
                        ", cet executable connait la " + std::to_string(kFormatVersion) +
                        ". Lecture au mieux, les champs inconnus seront perdus a "
                        "la prochaine sauvegarde.");
            }
            continue;
        }

        if (!headerSeen) {
            result.error = "ligne " + std::to_string(lineNumber) +
                           " : la directive \"version\" doit venir en premier";
            return result;
        }

        if (directive == "terrain") {
            fields >> result.terrainLabel;
            continue;
        }
        if (directive == "crs") {
            fields >> result.epsg;
            continue;
        }
        if (directive == "object") {
            std::string asset;
            double first = 0.0;
            double second = 0.0;
            float dz = 0.0f;
            float yaw = 0.0f;
            float scale = 1.0f;
            int solid = 1;
            if (!(fields >> asset >> first >> second >> dz >> yaw >> scale >> solid)) {
                ++result.skippedObjects;
                logWarn("Scene : ligne " + std::to_string(lineNumber) +
                        " mal formee, objet ignore : " + body);
                continue;
            }
            if (!std::isfinite(dz) || !std::isfinite(yaw) || !std::isfinite(scale) ||
                !(scale > 0.0f)) {
                ++result.skippedObjects;
                logWarn("Scene : ligne " + std::to_string(lineNumber) +
                        " : valeurs hors domaine (echelle nulle ou negative ?), "
                        "objet ignore.");
                continue;
            }

            PlacedObject object;
            object.asset = asset;
            object.yawDegrees = yaw;
            object.scale = scale;
            object.solid = (solid != 0);

            if (result.epsg != 0) {
                // Coordonnees projetees : c'est Heightmap qui detient la
                // convention d'axes, dans les deux sens (voir fromProjected).
                terrain.fromProjected(first, second, &object.position.x,
                                      &object.position.z);
            } else {
                object.position.x = static_cast<float>(first);
                object.position.z = static_cast<float>(second);
            }

            if (std::abs(object.position.x) > halfSize ||
                std::abs(object.position.z) > halfSize) {
                ++result.outsideExtent;
            }

            // Altitude ABSOLUE recalculee ici : c'est le seul endroit ou le dz
            // du fichier redevient un Y monde (voir l'en-tete du header).
            object.position.y =
                terrain.heightAt(object.position.x, object.position.z) + dz;

            result.objects.push_back(std::move(object));
            continue;
        }

        ++result.unknownDirectives;
        logWarn("Scene : directive inconnue ligne " + std::to_string(lineNumber) +
                ", ignoree : " + directive);
    }

    if (!headerSeen) {
        result.error = "aucune directive \"version\" : ce n'est pas un fichier .vcsc";
        return result;
    }

    result.ok = true;
    return result;
}

bool saveSceneFile(const std::string& path, const std::vector<PlacedObject>& objects,
                   const Heightmap& terrain, const std::string& terrainLabel) {
    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path target(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            logError("Scene : impossible de creer " + target.parent_path().string() +
                     " (" + ec.message() + ").");
            return false;
        }
    }

    // Ecriture dans un temporaire puis renommage : voir l'en-tete du header.
    fs::path temporary = target;
    temporary += ".tmp";

    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            logError("Scene : impossible d'ecrire " + temporary.string() + ".");
            return false;
        }
        useClassicLocale(file);

        const bool georeferenced = terrain.geo().isGeoreferenced();

        std::ostringstream out;
        useClassicLocale(out);
        out << "# Vercora scene v" << kFormatVersion
            << " - genere par l'editeur (Phase 15).\n"
               "# Format documente dans src/editor/SceneFile.hpp et NOTES-PHASE15.md.\n"
               "# object <asset> "
            << (georeferenced ? "<est> <nord>" : "<x> <z>")
            << " <dz> <cap deg> <echelle> <solide>\n";
        out << "version " << kFormatVersion << "\n";
        out << "terrain " << (terrainLabel.empty() ? std::string("inconnu") : terrainLabel)
            << "\n";
        out << "crs " << (georeferenced ? terrain.geo().epsg : 0u) << "\n";

        out << std::fixed;
        for (const PlacedObject& object : objects) {
            double first = 0.0;
            double second = 0.0;
            if (georeferenced) {
                terrain.toProjected(object.position.x, object.position.z, &first, &second);
            } else {
                first = object.position.x;
                second = object.position.z;
            }
            // dz : l'altitude REDEVIENT relative au terrain sous l'objet. Un
            // aller-retour save/load sur le meme terrain est donc l'identite -
            // c'est ce que verifie le test headless de round-trip.
            const float dz = object.position.y -
                             terrain.heightAt(object.position.x, object.position.z);

            out << "object " << object.asset << ' '
                << std::setprecision(3) << first << ' ' << second << ' '
                << dz << ' '
                << std::setprecision(3) << object.yawDegrees << ' '
                << std::setprecision(4) << object.scale << ' '
                << (object.solid ? 1 : 0) << '\n';
        }

        file << out.str();
        if (!file) {
            logError("Scene : ecriture interrompue dans " + temporary.string() + ".");
            return false;
        }
    }

    fs::rename(temporary, target, ec);
    if (ec) {
        // rename echoue sur certains systemes quand la cible existe deja.
        fs::remove(target, ec);
        fs::rename(temporary, target, ec);
        if (ec) {
            logError("Scene : impossible de remplacer " + target.string() + " (" +
                     ec.message() + ").");
            return false;
        }
    }
    return true;
}

std::string scenePathFor(const std::string& terrainPath,
                         const std::string& scenesDirectory) {
    std::filesystem::path source(terrainPath);
    std::string stem = source.stem().string();

    // Meme regle que orthoPathFor dans main.cpp : le suffixe de resolution ne
    // fait pas partie de l'identite du lieu.
    for (const char* suffix : { "_1m", "_2m", "_5m" }) {
        const std::string tail(suffix);
        if (stem.size() > tail.size() &&
            stem.compare(stem.size() - tail.size(), tail.size(), tail) == 0) {
            stem.resize(stem.size() - tail.size());
            break;
        }
    }
    if (stem.empty()) {
        stem = "scene";
    }

    return (std::filesystem::path(scenesDirectory) / (stem + ".vcsc")).string();
}

} // namespace vercora
