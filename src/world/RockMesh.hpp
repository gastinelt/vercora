#pragma once

#include "renderer/MeshData.hpp"

namespace vercora {

// PHASE 29 (Agent 2) : geometries des rochers de PIERRIER, generees EN CODE sur
// le modele de StoneMesh / TreeMesh / CubeMesh. Aucun fichier externe.
//
// Contrairement au galet ramassable (StoneMesh, dimensions absolues), ces meshes
// sont a l'ECHELLE UNITE, comme l'arbre et la touffe d'herbe : ils passent par le
// rendu INSTANCIE (world/Rocks.hpp), dont l'instance porte une echelle uniforme.
// Convention commune aux trois :
//   - hauteur visible au-dessus de l'origine ~ kRockVisibleHeight x echelle ;
//   - une partie ENTERREE sous y = 0 (kRockBuriedDepth x echelle) : un bloc pose
//     sur une pente de 0,6 a 0,84 flotterait sinon de son cote aval. C'est aussi
//     ce qu'on voit d'un vrai bloc d'eboulis, jamais pose sur un point ;
//   - empreinte horizontale ~ rayon kRockFootprintRadius x echelle (sert a la
//     collision des gros blocs).
//
// Trois variantes, choisies par la RUGOSITE comme le propose
// NOTES-PHASE29-DESIGN.md (rugosite >= 0,10 m -> blocs, < 0,10 -> pierraille) :

// Gros bloc anguleux : polyedre irregulier coupe par des plans (faces de
// fracture), plus large que haut.
MeshData boulderMeshData();

// Rocher moyen : meme construction, graine et proportions differentes, plus
// arrondi. Accompagne les gros blocs sur les zones rugueuses.
MeshData rockMeshData();

// Pierraille fine : une PLAQUE d'une dizaine de petits cailloux dans un disque
// de rayon 0,5 (echelle 1 = plaque de 1 m). Une instance = une poignee de
// cailloux : c'est ce qui rend la pierraille dense abordable en instances.
MeshData gravelMeshData();

inline constexpr float kRockVisibleHeight   = 0.70f; // bloc et rocher
inline constexpr float kRockBuriedDepth     = 0.25f;
inline constexpr float kRockFootprintRadius = 0.50f;

// PHASE 30 (Agent B) : OMBRE DE CONTACT cuite dans la couleur de sommet. Le
// moteur n'a pas d'ombre portee ; sans elle un bloc pose sur un sol clair n'a
// pas de pied et semble colle a plat. Facteur x kRockContactShade au niveau du
// sol (y <= 0), remontant en smoothstep jusqu'a x1 a kRockContactHeight du
// sommet visible de la piece (`top`, repere du mesh). Aucune modification du
// shader : foliage.vert transmet deja la couleur de sommet.
inline constexpr float kRockContactShade  = 0.55f;
inline constexpr float kRockContactHeight = 0.40f;
inline float rockContactShade(float y, float top) {
    const float h = kRockContactHeight * top;
    if (h <= 0.0f) {
        return 1.0f;
    }
    float t = y / h;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return kRockContactShade + (1.0f - kRockContactShade) * smooth;
}

} // namespace vercora
