#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace vercora {

// Encodage des texels d'une texture .vcto. Les valeurs sont celles ECRITES dans
// le fichier par tools/make_ortho_texture.py : ne jamais les reordonner.
enum class TextureFormat : uint32_t {
    RGBA8 = 0, // 4 octets par texel, non compresse
    BC1   = 1, // blocs de 4x4 texels sur 8 octets (RGB, 6:1)
};

// Un niveau de mipmap : ou le trouver dans le bloc d'octets, et quelle taille il
// fait. Les offsets sont calcules au chargement plutot que stockes : le fichier
// les range consecutivement, et une taille deduite ne peut pas contredire les
// donnees (alors qu'une table lue pourrait).
struct TextureMipLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t offset = 0; // depuis le debut de pixels
    uint64_t size = 0;   // en octets
};

// Texture chargee depuis un .vcto, cote CPU uniquement : aucune dependance
// Vulkan, comme Heightmap. C'est ce qui permet de la tester sans GPU.
//
// PHASE 11. Le terrain LiDAR ne porte que des altitudes ; la couleur vient
// d'ailleurs. Ce fichier est le pendant exact de TerrainFile.hpp pour la photo
// aerienne : un format maison, produit hors ligne, que le jeu se contente de
// relire. La justification detaillee du format (et pourquoi les mipmaps sont
// precalculees : on ne peut pas filtrer des blocs BC1 sur le GPU) est en tete de
// tools/make_ortho_texture.py.
struct TextureData {
    uint32_t width = 0;
    uint32_t height = 0;
    TextureFormat format = TextureFormat::RGBA8;
    std::vector<TextureMipLevel> mips;
    std::vector<unsigned char> pixels;

    // Emprise Lambert 93 COUVERTE par l'image, bord a bord. C'est ce qui permet
    // de verifier qu'une texture correspond bien au terrain charge au lieu de
    // l'appliquer de travers en silence.
    double eastMin = 0.0;
    double northMin = 0.0;
    double eastMax = 0.0;
    double northMax = 0.0;
    uint32_t epsg = 0;

    std::string label;

    bool empty() const { return pixels.empty(); }

    // Metres de terrain par texel du niveau 0, moyenne des deux axes. Sert au
    // journal de demarrage : c'est le chiffre qui dit si la texture est a la
    // hauteur du niveau de detail du terrain.
    double metresPerTexel() const;

    // PHASE 30. Octets de TOUTE la chaine de mipmaps, deduits des niveaux :
    // c'est la taille de `pixels` une fois charge, et celle du staging.
    uint64_t payloadSize() const {
        return mips.empty() ? 0 : mips.back().offset + mips.back().size;
    }
};

// Charge un .vcto. Lance std::runtime_error avec un message exploitable si le
// fichier est absent, tronque, d'une autre version, ou incoherent (nombre de
// niveaux impossible, taille de charge utile ne correspondant pas aux niveaux
// annonces, ...).
TextureData loadTextureFile(const std::string& path);

// PHASE 30. Les deux moities de loadTextureFile, pour ne jamais tenir la
// charge utile en RAM : l'en-tete d'abord (memes controles, taille du fichier
// comprise, `pixels` reste VIDE), puis la lecture des `payloadSize()` octets
// directement la ou l'appelant les veut - en pratique le staging mappe de la
// texture GPU (Renderer::setTerrainTextureFromFile). Sur l'orthophoto de la
// zone, c'est une copie de 42 Mo de moins au chargement, et autant de pic RAM.
TextureData loadTextureFileHeader(const std::string& path);
void readTexturePayload(const std::string& path, const TextureData& header, void* destination);

// Variante par TRANCHE, sur un fichier deja ouvert : `bytes` octets de la
// charge utile a partir de son octet `offset`. Sert au televersement par
// bandes de Texture (staging de 4 Mo au plus).
void readTexturePayload(std::istream& file, uint64_t offset, uint64_t bytes, void* destination);

// Nombre d'octets d'un niveau de mipmap pour un format donne. Expose parce que
// c'est la seule formule qui lie les deux formats a leur taille, et que le
// chargeur ET les tests s'en servent.
uint64_t textureLevelSize(TextureFormat format, uint32_t width, uint32_t height);

} // namespace vercora
