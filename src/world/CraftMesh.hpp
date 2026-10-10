#pragma once

#include "renderer/MeshData.hpp"

namespace vercora {

// ===========================================================================
// PHASE 28 : les geometries des objets FACONNES (world/Crafting).
// ===========================================================================
// Meme contrat que world/HarvestMesh.hpp et world/StoneMesh.hpp, dont ce fichier
// est le prolongement : genere EN CODE, dimensions ABSOLUES (aucune echelle a
// l'instance), mesh centre sur l'origine y compris verticalement, objets
// couches le long de +X (meme convention de yaw que la buche et l'eclat).
//
// Ce sont des geometries de LISIBILITE bas-poly, pas des modeles finis : chaque
// objet doit se reconnaitre a sa silhouette, puisque le jeu n'a aucune liste
// d'inventaire a l'ecran (philosophie "zero menu").
//
// Le foyer n'a qu'UN mesh, qu'il soit allume ou eteint : le renderer n'a ni
// lumiere ponctuelle ni particules (NOTES-PHASE28-DESIGN.md §4). L'etat de
// combustion vit dans world/Crafting (HearthSet) et se lit dans le journal.

// Ligature : un echeveau de fibres torsadees, anneau plat pose au sol.
MeshData cordMeshData();

// Hache emmanchee : un manche (branche ecorcee) le long de +X, un eclat en
// lame a son extremite, et la ligature qui les tient.
MeshData axeMeshData();

// Foyer : trois pierres en cercle autour de deux branches croisees.
MeshData hearthMeshData();

// Cendre : un petit tas gris, tres plat.
MeshData ashMeshData();

// Poix : une galette sombre et luisante, plus plate que la perle de resine.
MeshData pitchMeshData();

// PHASE 28 (Agent 3). Hache renforcee : la hache, dont la ligature est noyee
// dans un manchon de poix brun-noir (meme encombrement que la hache).
MeshData pitchedAxeMeshData();

// PHASE 28 (Agent 3). Abri en appentis : deux montants (buches), une faitiere
// (buche) et une buche de base, six branches inclinees de la faitiere au sol,
// deux ligatures aux noeuds. Le SEUL objet de la famille qui n'est pas a
// l'echelle de la main : il n'est pas ramassable (groundItemIsPortable).
MeshData shelterMeshData();

// --- Dimensions en unites monde (les "rayons" sont des DEMI-HAUTEURS) --------
inline constexpr float kCordRadius   = 0.020f; // demi-epaisseur de l'echeveau
inline constexpr float kCordWidth    = 0.075f; // rayon exterieur de l'anneau
inline constexpr float kAxeRadius    = 0.030f; // demi-epaisseur de la lame couchee
inline constexpr float kAxeLength    = 0.50f;  // longueur totale du manche
inline constexpr float kHearthRadius = 0.110f; // demi-hauteur du cercle de pierres
inline constexpr float kHearthWidth  = 0.40f;  // rayon du cercle de pierres
inline constexpr float kAshRadius    = 0.020f; // demi-hauteur du tas de cendre
inline constexpr float kAshWidth     = 0.13f;
inline constexpr float kPitchRadius  = 0.022f; // demi-hauteur de la galette
inline constexpr float kPitchWidth   = 0.05f;
inline constexpr float kShelterRadius = 0.60f; // demi-hauteur de l'abri (1,2 m de haut)
inline constexpr float kShelterHalfWidth = 1.05f; // demi-largeur (le long de +X)
inline constexpr float kShelterHalfDepth = 0.90f; // demi-profondeur (le long de Z)

} // namespace vercora
