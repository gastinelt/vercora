#pragma once

#include "renderer/MeshData.hpp"
#include "renderer/Vertex.hpp"

#include <cstdint>
#include <vector>

namespace vercora {

// Geometrie du cube, codee en dur (Phase 1).
// 24 sommets = 4 par face, pour avoir une couleur ET une normale propres a
// chaque face (des sommets partages ne pourraient pas porter 6 normales).
// Cube centre sur l'origine, arete de 1 (coordonnees +/-0.5).
//
// Convention d'enroulement : chaque quad est defini dans l'ordre anti-horaire
// vu de l'exterieur. Le culling est desactive (voir Pipeline), donc l'ordre
// n'est pas critique ici, mais il reste correct pour activer le back-face
// culling plus tard.
inline const std::vector<Vertex> kCubeVertices = {
    // Face +Z (avant)  - rouge      normale (0,0,1)
    {{-0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},

    // Face -Z (arriere) - vert       normale (0,0,-1)
    {{ 0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
    {{ 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},

    // Face +X (droite)  - bleu       normale (1,0,0)
    {{ 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},

    // Face -X (gauche)  - jaune      normale (-1,0,0)
    {{-0.5f, -0.5f, -0.5f}, {1.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f,  0.5f}, {1.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f,  0.5f,  0.5f}, {1.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f,  0.5f, -0.5f}, {1.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},

    // Face +Y (haut)    - magenta    normale (0,1,0)
    {{-0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},

    // Face -Y (bas)     - cyan       normale (0,-1,0)
    {{-0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
    {{ 0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
    {{ 0.5f, -0.5f,  0.5f}, {0.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
    {{-0.5f, -0.5f,  0.5f}, {0.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
};

// 2 triangles par face : (0,1,2) et (2,3,0), decales de 4 par face.
inline const std::vector<uint32_t> kCubeIndices = {
     0,  1,  2,   2,  3,  0,   // +Z
     4,  5,  6,   6,  7,  4,   // -Z
     8,  9, 10,  10, 11,  8,   // +X
    12, 13, 14,  14, 15, 12,   // -X
    16, 17, 18,  18, 19, 16,   // +Y
    20, 21, 22,  22, 23, 20,   // -Y
};

// Emballe le cube en dur dans le meme MeshData que celui produit par le loader
// .obj (Phase 3). Le cube reste ainsi utilisable tel quel comme geometrie de
// repli / de reference : meme chemin de code, meme classe Mesh, aucune
// duplication du cote GPU.
// Les sommets du cube ne portent pas d'UV : le membre Vertex::uv garde sa valeur
// par defaut (0,0), ce qui est sans effet tant qu'aucun shader ne l'utilise.
inline MeshData cubeMeshData() {
    MeshData data;
    data.name = "cube (geometrie en dur, Phase 1)";
    data.vertices = kCubeVertices;
    data.indices = kCubeIndices;
    return data;
}

} // namespace vercora
