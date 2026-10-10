#pragma once

#include "renderer/MeshData.hpp"

namespace vercora {

// ===========================================================================
// PHASE 22 : les geometries des matieres recoltees.
// ===========================================================================
// Meme regle que world/StoneMesh.hpp, dont ce fichier est le prolongement
// direct : tout est genere EN CODE, aucun fichier, aucun telechargement, et les
// dimensions sont ABSOLUES. Une motte d'argile fait la meme taille partout, donc
// une instance se pose par translate * rotate SANS aucune echelle - la normal
// matrix reste exacte au sens strict.
//
// Les meshes sont centres sur l'origine, y compris verticalement : Ground::drop
// les pose en ajoutant simplement leur demi-hauteur a l'altitude du terrain
// (voir groundItemRestRadius).
//
// POURQUOI QUATRE MESHES ET NON UN SEUL RECOLORE. Parce que la philosophie
// "zero menu virtuel" fait reposer TOUTE la lisibilite sur la silhouette : le
// joueur n'a aucune liste d'inventaire a l'ecran pour distinguer deux objets
// gris poses cote a cote. Une motte d'argile ronde, un tas de sable etale, une
// perle de resine et un fagot de tiges se reconnaissent de loin ; quatre galets
// de couleurs differentes, non.

// Une motte d'argile : bloc arrondi, legerement aplati, facette grossierement.
// Plus lisse et plus ramassee que le galet - l'argile se prend a la main et
// garde la forme qu'on lui donne.
MeshData clayLumpMeshData();

// Une poignee de sable : petit cone tres surbaisse, comme un tas verse au sol.
// C'est la seule des quatre matieres qui n'a pas de forme propre, et son mesh
// le dit.
MeshData sandPileMeshData();

// Une perle de resine : goutte allongee, facettee, la pointe vers le haut.
MeshData resinBlobMeshData();

// Un fagot de tiges a fibres : quelques tiges couchees en faisceau, liees au
// milieu. Cest ce qui reste d'une ronce ou d'une ortie une fois arrachee et
// ebranchee.
MeshData fiberBundleMeshData();

// La PLANTE SUR PIED (ronce ou ortie), par opposition au fagot recolte
// ci-dessus : une touffe de tiges dressees et divergentes, bas-poly. C'est elle
// qui est semee dans le monde par world/FiberPlants.hpp ; le fagot n'apparait
// qu'une fois la plante arrachee.
//
// Le mesh est a l'echelle UNITE (une tige d'un metre) et non a dimensions
// absolues, contrairement aux quatre precedents : une ronce n'a pas de taille
// unique, et le semis lui en donne une par instance - meme convention que la
// touffe d'herbe de world/GrassMesh.hpp.
MeshData fiberPlantMeshData();

// --- Dimensions en unites monde --------------------------------------------
// Les "rayons" sont des DEMI-HAUTEURS : l'objet est centre sur son origine, et
// c'est cette valeur qui le pose sur le sol sans qu'il s'enfonce ni flotte.
inline constexpr float kClayRadius        = 0.075f; // demi-hauteur de la motte
inline constexpr float kClayWidth         = 0.11f;  // demi-largeur horizontale
inline constexpr float kSandRadius        = 0.035f; // demi-hauteur du tas
inline constexpr float kSandWidth         = 0.15f;
inline constexpr float kResinRadius       = 0.030f; // demi-hauteur de la perle
inline constexpr float kResinWidth        = 0.035f;
inline constexpr float kFiberBundleRadius = 0.045f; // demi-epaisseur du fagot
inline constexpr float kFiberBundleLength = 0.38f;

} // namespace vercora
