#include "assets/TextureFile.hpp"

#include "core/Logger.hpp"

#include <bit>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace vercora {
namespace {

constexpr std::size_t kHeaderSize = 128;
constexpr std::size_t kLabelSize = 64;
constexpr uint32_t kExpectedVersion = 1;

// Meme garde-fou que .vchm : un en-tete corrompu ne doit pas pouvoir demander
// une allocation absurde avant qu'on ait pu constater que le fichier est trop
// court. 32768 est deja le double de la limite materielle usuelle
// (maxImageDimension2D = 16384 sur les GPU de bureau actuels).
constexpr uint32_t kMaxDimension = 32768;

// Une chaine de mipmaps ne peut pas depasser log2(taille max) + 1 niveaux.
constexpr uint32_t kMaxMipLevels = 16;

static_assert(std::endian::native == std::endian::little,
              "Le format .vcto est petit-boutiste ; portage requis sur une "
              "plateforme gros-boutiste.");

template <typename T>
T readField(const unsigned char* header, std::size_t offset) {
    T value{};
    std::memcpy(&value, header + offset, sizeof(T));
    return value;
}

} // namespace

uint64_t textureLevelSize(TextureFormat format, uint32_t width, uint32_t height) {
    if (format == TextureFormat::BC1) {
        // Blocs de 4x4 texels sur 8 octets. Les dimensions non multiples de 4
        // (les tout derniers niveaux : 2x2, 1x1) occupent un bloc entier : c'est
        // ce que fait l'outil et ce qu'attend le materiel.
        const uint64_t blocksX = (width + 3u) / 4u;
        const uint64_t blocksY = (height + 3u) / 4u;
        return blocksX * blocksY * 8u;
    }
    return static_cast<uint64_t>(width) * height * 4u;
}

double TextureData::metresPerTexel() const {
    if (width == 0 || height == 0) {
        return 0.0;
    }
    const double perX = (eastMax - eastMin) / static_cast<double>(width);
    const double perY = (northMax - northMin) / static_cast<double>(height);
    return 0.5 * (perX + perY);
}

TextureData loadTextureFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Texture : fichier introuvable ou illisible : " + path);
    }

    unsigned char header[kHeaderSize];
    if (!file.read(reinterpret_cast<char*>(header), kHeaderSize)) {
        throw std::runtime_error(
            "Texture : " + path + " fait moins de " + std::to_string(kHeaderSize) +
            " octets, ce n'est pas un fichier .vcto.");
    }

    if (std::memcmp(header, "VCTO", 4) != 0) {
        throw std::runtime_error(
            "Texture : " + path + " ne commence pas par la magie \"VCTO\". "
            "Fichier d'un autre type, ou produit par un autre outil ?");
    }

    const uint32_t version = readField<uint32_t>(header, 4);
    if (version != kExpectedVersion) {
        throw std::runtime_error(
            "Texture : " + path + " est en version " + std::to_string(version) +
            ", ce moteur lit la version " + std::to_string(kExpectedVersion) +
            ". Regenerer avec tools/make_ortho_texture.py.");
    }

    TextureData texture;
    texture.width = readField<uint32_t>(header, 8);
    texture.height = readField<uint32_t>(header, 12);
    const uint32_t mipLevels = readField<uint32_t>(header, 16);
    const uint32_t rawFormat = readField<uint32_t>(header, 20);
    texture.eastMin = readField<double>(header, 24);
    texture.northMin = readField<double>(header, 32);
    texture.eastMax = readField<double>(header, 40);
    texture.northMax = readField<double>(header, 48);
    texture.epsg = readField<uint32_t>(header, 56);

    char label[kLabelSize + 1] = {};
    std::memcpy(label, header + 64, kLabelSize);
    texture.label = label;

    if (texture.width == 0 || texture.height == 0 ||
        texture.width > kMaxDimension || texture.height > kMaxDimension) {
        throw std::runtime_error(
            "Texture : " + path + " annonce des dimensions inexploitables (" +
            std::to_string(texture.width) + "x" + std::to_string(texture.height) + ").");
    }
    if (mipLevels == 0 || mipLevels > kMaxMipLevels) {
        throw std::runtime_error(
            "Texture : " + path + " annonce " + std::to_string(mipLevels) +
            " niveaux de mipmap, ce qui est impossible.");
    }
    if (rawFormat > static_cast<uint32_t>(TextureFormat::BC1)) {
        throw std::runtime_error(
            "Texture : " + path + " utilise le format inconnu " +
            std::to_string(rawFormat) + ".");
    }
    if (texture.eastMax <= texture.eastMin || texture.northMax <= texture.northMin) {
        throw std::runtime_error(
            "Texture : " + path + " porte une emprise vide ou inversee.");
    }
    texture.format = static_cast<TextureFormat>(rawFormat);

    // Les niveaux sont deduits, pas lus : la seule chose qui vient du fichier est
    // leur NOMBRE. Un decompte incoherent se traduira par une taille totale qui
    // ne correspond pas au fichier, donc par un refus juste en dessous.
    uint64_t total = 0;
    texture.mips.reserve(mipLevels);
    for (uint32_t level = 0; level < mipLevels; ++level) {
        TextureMipLevel mip;
        mip.width = texture.width >> level;
        mip.height = texture.height >> level;
        if (mip.width == 0) mip.width = 1;
        if (mip.height == 0) mip.height = 1;
        mip.offset = total;
        mip.size = textureLevelSize(texture.format, mip.width, mip.height);
        total += mip.size;
        texture.mips.push_back(mip);
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    const uint64_t expected = static_cast<uint64_t>(kHeaderSize) + total;
    if (fileSize < 0 || static_cast<uint64_t>(fileSize) != expected) {
        throw std::runtime_error(
            "Texture : " + path + " fait " + std::to_string(fileSize) +
            " octets, alors que son en-tete en decrit " + std::to_string(expected) +
            ". Fichier tronque ou en-tete incoherent.");
    }

    file.seekg(static_cast<std::streamoff>(kHeaderSize), std::ios::beg);
    texture.pixels.resize(static_cast<std::size_t>(total));
    if (!file.read(reinterpret_cast<char*>(texture.pixels.data()),
                   static_cast<std::streamsize>(total))) {
        throw std::runtime_error("Texture : lecture interrompue dans " + path + ".");
    }

    logInfo("Texture : " + texture.label + " " + std::to_string(texture.width) + "x" +
            std::to_string(texture.height) + ", " + std::to_string(mipLevels) +
            " mipmaps, " +
            (texture.format == TextureFormat::BC1 ? std::string("BC1") : std::string("RGBA8")) +
            ", " + std::to_string(total / (1024 * 1024)) + " Mo.");

    return texture;
}

} // namespace vercora
