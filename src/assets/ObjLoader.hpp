#pragma once

#include "renderer/MeshData.hpp"

#include <string>

namespace vercora {

// Charge un fichier Wavefront .obj en geometrie CPU (MeshData), prete a etre
// envoyee au GPU par la classe Mesh.
//
// Ce que fait le loader :
//  - triangule les faces (un .obj peut contenir des quads ou des n-gones) ;
//  - lit positions, normales et UV quand ils sont presents ;
//  - genere des normales lissees pour les sommets qui n'en ont pas, sinon
//    l'eclairage de la Phase 2 produirait un objet totalement noir ;
//  - deduplique les sommets sur le triplet (position, UV, normale) pour ne pas
//    exploser inutilement la taille du vertex buffer.
//
// Aucune ressource Vulkan n'est touchee ici : appeler cette fonction AVANT
// d'initialiser le GPU permet d'echouer proprement sur un fichier manquant ou
// invalide sans avoir rien alloue.
//
// Lance std::runtime_error avec un message explicite si le fichier est
// introuvable, illisible, mal forme, ou ne contient aucune geometrie.
MeshData loadObjMesh(const std::string& path);

} // namespace vercora
