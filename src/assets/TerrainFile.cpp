#include "assets/TerrainFile.hpp"

#include "core/Logger.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace vercora {
namespace {

constexpr std::size_t kHeaderSize = 120;
constexpr std::size_t kLabelSize = 64;
constexpr uint32_t kExpectedVersion = 1;
constexpr uint32_t kFlagAbsoluteAltitude = 1u << 0;

// Garde-fou contre un en-tete corrompu. Sans lui, un champ "samplesX" errone
// demanderait une allocation absurde AVANT qu'on ait pu constater que le fichier
// est trop court pour la contenir. 16384 echantillons par cote, c'est deja
// 268 millions de sommets : bien au-dela de ce qu'on chargera jamais d'un bloc.
constexpr uint32_t kMaxResolution = 16384;

// Le format est petit-boutiste, comme toutes les machines visees (x86-64,
// ARM64). Plutot que d'ecrire un echangeur d'octets qui ne serait jamais
// exerce - donc jamais teste, donc probablement faux - on refuse a la
// compilation les plateformes gros-boutistes.
static_assert(std::endian::native == std::endian::little,
              "Le format .vchm est petit-boutiste ; portage requis sur une "
              "plateforme gros-boutiste.");
static_assert(sizeof(float) == 4 && sizeof(double) == 8,
              "Le format .vchm suppose des float IEEE 754 de 4 et 8 octets.");

// Lecture d'un champ a un offset FIXE du tampon d'en-tete. On ne surimpose pas
// une struct C++ : son agencement dependrait du padding decide par le
// compilateur, alors que le format, lui, est fige par l'outil Python.
template <typename T>
T readField(const unsigned char* header, std::size_t offset) {
    T value{};
    std::memcpy(&value, header + offset, sizeof(T));
    return value;
}

} // namespace

Heightmap loadTerrainFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Terrain : fichier introuvable ou illisible : " + path);
    }

    unsigned char header[kHeaderSize];
    if (!file.read(reinterpret_cast<char*>(header), kHeaderSize)) {
        throw std::runtime_error(
            "Terrain : " + path + " fait moins de " + std::to_string(kHeaderSize) +
            " octets, ce n'est pas un fichier .vchm.");
    }

    if (std::memcmp(header, "VCHM", 4) != 0) {
        throw std::runtime_error(
            "Terrain : " + path + " ne commence pas par la magie \"VCHM\". "
            "Fichier d'un autre type, ou produit par un autre outil ?");
    }

    const uint32_t version = readField<uint32_t>(header, 4);
    if (version != kExpectedVersion) {
        throw std::runtime_error(
            "Terrain : " + path + " est en version " + std::to_string(version) +
            ", ce moteur lit la version " + std::to_string(kExpectedVersion) +
            ". Regenerer avec tools/lidar_to_heightmap.py.");
    }

    const uint32_t samplesX = readField<uint32_t>(header, 8);
    const uint32_t samplesZ = readField<uint32_t>(header, 12);
    const float    spacing  = readField<float>(header, 16);
    const double   originE  = readField<double>(header, 24);
    const double   originN  = readField<double>(header, 32);
    const float    headerMin = readField<float>(header, 40);
    const float    headerMax = readField<float>(header, 44);
    const uint32_t epsg     = readField<uint32_t>(header, 48);
    const uint32_t flags    = readField<uint32_t>(header, 52);

    if (samplesX != samplesZ) {
        throw std::runtime_error(
            "Terrain : " + path + " est une grille " + std::to_string(samplesX) + "x" +
            std::to_string(samplesZ) + ". Seules les grilles CARREES sont "
            "supportees pour l'instant (Heightmap n'expose qu'une resolution).");
    }
    if (samplesX < 2 || samplesX > kMaxResolution) {
        throw std::runtime_error(
            "Terrain : " + path + " annonce " + std::to_string(samplesX) +
            " echantillons par cote, hors des bornes acceptees [2, " +
            std::to_string(kMaxResolution) + "].");
    }

    const std::size_t count =
        static_cast<std::size_t>(samplesX) * static_cast<std::size_t>(samplesZ);

    // Le fichier doit faire EXACTEMENT la taille annoncee par son en-tete. Trop
    // court, on lirait des zeros silencieusement (terrain plat par endroits) ;
    // trop long, l'en-tete ment sur son contenu. Dans les deux cas mieux vaut
    // refuser que jouer sur un terrain a moitie faux.
    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    const std::streamoff expected =
        static_cast<std::streamoff>(kHeaderSize + count * sizeof(float));
    if (fileSize != expected) {
        throw std::runtime_error(
            "Terrain : " + path + " fait " + std::to_string(fileSize) +
            " octets, alors que son en-tete (" + std::to_string(samplesX) + "x" +
            std::to_string(samplesZ) + " echantillons) en decrit " +
            std::to_string(expected) + ". Fichier tronque ou en-tete corrompu.");
    }

    std::vector<float> heights(count);
    file.seekg(static_cast<std::streamoff>(kHeaderSize), std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(heights.data()),
                   static_cast<std::streamsize>(count * sizeof(float)))) {
        throw std::runtime_error("Terrain : lecture interrompue dans " + path + ".");
    }

    // Le label du fichier n'est pas garanti termine par un zero s'il occupe les
    // 64 octets : on borne explicitement plutot que de faire confiance.
    const char* labelBytes = reinterpret_cast<const char*>(header + 56);
    std::size_t labelLength = 0;
    while (labelLength < kLabelSize && labelBytes[labelLength] != '\0') { ++labelLength; }
    std::string label(labelBytes, labelLength);
    if (label.empty()) { label = "dalle sans nom"; }

    GeoReference geo;
    geo.originEast = originE;
    geo.originNorth = originN;
    geo.epsg = epsg;
    geo.absoluteAltitude = (flags & kFlagAbsoluteAltitude) != 0;

    // Le constructeur valide le reste : taille coherente, pas > 0, et surtout
    // aucune altitude non finie.
    Heightmap heightmap(std::move(heights), samplesX, spacing, label, geo);

    // Les bornes de l'en-tete sont redondantes avec les donnees. Les comparer
    // est un controle d'integrite gratuit : un ecart signale un fichier modifie
    // apres coup, ou un outil de generation qui a diverge du format.
    const float tolerance = 0.01f; // les altitudes LiDAR sont au centimetre
    if (std::abs(headerMin - heightmap.minHeight()) > tolerance ||
        std::abs(headerMax - heightmap.maxHeight()) > tolerance) {
        logWarn("Terrain : les bornes de l'en-tete (" + std::to_string(headerMin) +
                " .. " + std::to_string(headerMax) + ") ne correspondent pas aux "
                "altitudes lues (" + std::to_string(heightmap.minHeight()) + " .. " +
                std::to_string(heightmap.maxHeight()) + "). Fichier suspect.");
    }

    logInfo("Terrain reel charge : " + label + ", " + std::to_string(samplesX) + "x" +
            std::to_string(samplesZ) + " echantillons au pas de " +
            std::to_string(spacing) + " m, emprise " +
            std::to_string(heightmap.worldSize()) + " m, altitudes " +
            std::to_string(heightmap.minHeight()) + " a " +
            std::to_string(heightmap.maxHeight()) + " m" +
            (geo.absoluteAltitude ? " (IGN69)" : " (base ramenee a zero)") + ".");

    if (geo.isGeoreferenced()) {
        // Le coin nord-ouest de la dalle est le seul point dont on connaisse la
        // coordonnee projetee sans calcul : le journaliser permet de retrouver
        // la dalle sur une carte a partir d'un simple log de partie.
        logInfo("  georeference EPSG:" + std::to_string(geo.epsg) +
                ", coin nord-ouest E " + std::to_string(geo.originEast) +
                " N " + std::to_string(geo.originNorth) +
                " ; le centre de la dalle est a l'origine du monde.");
    }

    return heightmap;
}

} // namespace vercora
