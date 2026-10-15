#pragma once

#include "renderer/Vertex.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vercora {

// PHASE 32. Les formats de sommet tels que le GPU les LIT.
//
// Jusqu'ici un seul format, `Vertex` (68 octets), servait a tout : il porte la
// position, la couleur, la normale, l'UV ET la cible de geomorphing du terrain.
// Il reste le format de TRAVAIL cote CPU - tous les generateurs (arbres,
// rochers, .obj, terrain) et tous les tests le produisent et le lisent - mais
// il n'est plus televerse tel quel. Le Renderer le CONVERTIT au televersement
// dans l'un des deux formats ci-dessous, chacun reduit a ce que son shader lit
// reellement. Comme Vertex.hpp, ce fichier ne connait pas Vulkan : la
// description des attributs au pipeline vit dans renderer/VertexFormat.hpp.

// ---------------------------------------------------------------------------
// Objets et vegetation (cube.vert, foliage.vert) : 44 octets, conversion EXACTE
// ---------------------------------------------------------------------------
// `Vertex` sans ses 24 octets de cible de geomorphing, qu'aucun de ces deux
// shaders ne lit (le pipeline ne les decrivait deja pas : seul le STRIDE les
// payait). Les flottants sont recopies tels quels : l'image est identique au
// bit pres, et les maillages d'objets pesent 35 % de moins.
struct ObjectVertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec3 normal;
    glm::vec2 uv;
};
static_assert(sizeof(ObjectVertex) == 44,
              "ObjectVertex doit rester sans remplissage : sa taille est le stride "
              "du binding 0 des pipelines objet et vegetation.");

inline ObjectVertex toObjectVertex(const Vertex& vertex) {
    return ObjectVertex{ vertex.pos, vertex.color, vertex.normal, vertex.uv };
}

// ---------------------------------------------------------------------------
// Terrain (terrain.vert) : 16 octets au lieu de 68
// ---------------------------------------------------------------------------
// Une tuile de terrain est une GRILLE REGULIERE : la position horizontale d'un
// sommet, celle de sa cible de geomorphing et son UV ne dependent que de son
// indice dans la tuile et de trois entiers propres a la tuile (premier
// echantillon, pas, sommets par rangee - pousses en push constant, voir
// DrawItem::grid). Le vertex shader les recalcule avec EXACTEMENT les memes
// operations flottantes que Heightmap::worldCoord : les positions sont donc
// identiques au bit pres a celles que portait le format de 68 octets. La
// couleur, elle, valait toujours (1, 1, 1).
//
// Ne restent que ce qui ne se deduit pas de l'indice :
//   - height      : altitude du sommet, flottant EXACT (la verite physique) ;
//   - morphHeight : altitude de sa cible dans la grille du niveau superieur ;
//   - normal      : composantes x et z de la normale, en snorm16 ; y se
//                   reconstruit (une normale de champ de hauteur pointe
//                   toujours vers le haut) ;
//   - morphNormal : idem pour la normale de la cible.
// Seules les normales sont quantifiees : 1/32767 par composante, soit ~3e-5 -
// un ecart d'eclairage de l'ordre du centieme de niveau sur 255.
struct TerrainVertex {
    float height = 0.0f;
    float morphHeight = 0.0f;
    int16_t normal[2] = { 0, 0 };
    int16_t morphNormal[2] = { 0, 0 };
};
static_assert(sizeof(TerrainVertex) == 16,
              "TerrainVertex doit rester compact : sa taille est le stride du "
              "binding 0 du pipeline terrain.");

// Quantification snorm16, la convention de VK_FORMAT_R16_SNORM : l'entier c
// represente max(c / 32767, -1).
inline int16_t packSnorm16(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lround(clamped * 32767.0f));
}

inline float unpackSnorm16(int16_t value) {
    return std::max(static_cast<float>(value) / 32767.0f, -1.0f);
}

// Normale reconstruite depuis ses composantes x et z quantifiees : la
// formule exacte du vertex shader, pour que les tests mesurent ce que le GPU
// verra.
inline glm::vec3 unpackTerrainNormal(const int16_t packed[2]) {
    const float x = unpackSnorm16(packed[0]);
    const float z = unpackSnorm16(packed[1]);
    return { x, std::sqrt(std::max(0.0f, 1.0f - x * x - z * z)), z };
}

// Conversion d'un sommet de terrain du format de travail. N'utilise que les
// altitudes et les normales : le reste se recalcule dans le shader.
inline TerrainVertex toTerrainVertex(const Vertex& vertex) {
    TerrainVertex packed;
    packed.height = vertex.pos.y;
    packed.morphHeight = vertex.morphPos.y;
    packed.normal[0] = packSnorm16(vertex.normal.x);
    packed.normal[1] = packSnorm16(vertex.normal.z);
    packed.morphNormal[0] = packSnorm16(vertex.morphNormal.x);
    packed.morphNormal[1] = packSnorm16(vertex.morphNormal.z);
    return packed;
}

// La position horizontale d'un echantillon de grille, calculee comme le fait
// terrain.vert : `origin + float(index) * spacing`, sans contraction en FMA
// (qualificatif `precise` cote shader). C'est l'expression meme de
// Heightmap::worldCoord avec origin = worldCoord(0) : le resultat est identique
// au bit pres, ce que verifie tests/logic_tests.cpp.
inline float terrainGridCoord(float origin, float spacing, uint32_t index) {
    return origin + static_cast<float>(index) * spacing;
}

} // namespace vercora
