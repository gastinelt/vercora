#pragma once

#include "renderer/MeshData.hpp"

namespace vercora {

// Geometrie synthetique bas-poly de la faune (Phase 17), generee EN CODE comme
// l'arbre, la souche, le galet et l'eclat : aucun fichier, aucun
// telechargement.
//
// --- Convention ------------------------------------------------------------
// Dimensions ABSOLUES (comme la pierre et la buche, pas comme l'arbre) : un
// chevreuil fait la meme taille partout. Le mesh est pose PIEDS AU SOL (y = 0
// au bas des sabots) et regarde vers +X.
//
// Une instance se place donc par
//     translate(pieds) * rotateY(-cap) * scale(facteur)
// ou le facteur est une echelle UNIFORME proche de 1 (voir Animal::scale) : la
// rotation suivie d'une echelle uniforme laisse mat3(model) valide comme normal
// matrix, exactement comme pour l'arbre.
//
// Le -cap vient de la convention deja posee par le velo : une rotation de theta
// autour de +Y envoie le +X local sur (cos theta, -sin theta) dans le plan
// (x, z) ; pour l'envoyer sur le cap (cos h, sin h), il faut theta = -h.
//
// --- Pourquoi un seul mesh -------------------------------------------------
// Une seule silhouette de quadrupede pour toute la faune de cette session. Ce
// n'est pas un choix esthetique, c'est le perimetre annonce : la Phase 17
// construit la BOUCLE (presence -> derangement -> absence), pas un bestiaire.
// Plusieurs especes se feront en ajoutant des meshes et un champ d'espece a
// Animal, sans toucher a la logique.

// Un cervide bas-poly : corps, encolure, tete, quatre pattes, queue. Facette
// (une normale par triangle) comme le galet - a cette densite de triangles, des
// normales lissees rendraient la silhouette molle.
MeshData animalMeshData();

// Dimensions en unites monde, exposees parce que le placement et la fuite en
// ont besoin (hauteur d'oeil de l'animal, distance de securite, portee de
// visee eventuelle plus tard).
inline constexpr float kAnimalLength = 1.30f; // museau -> croupe
inline constexpr float kAnimalHeight = 1.05f; // sabots -> sommet de la tete
inline constexpr float kAnimalWidth  = 0.42f;

} // namespace vercora
