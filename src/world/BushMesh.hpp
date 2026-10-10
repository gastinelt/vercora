#pragma once

#include "renderer/MeshData.hpp"

namespace vercora {

// PHASE 29 (Agent 3) : geometries des BUISSONS et PETITS ARBRES, generees en
// code (aucun fichier externe), a l'ECHELLE UNITE : hauteur totale 1 au-dessus
// de l'origine, qui est le pied pose sur le terrain. L'instance porte une
// echelle uniforme = la HAUTEUR du sujet en metres, tiree sous la carte de
// hauteur de la classe 3 LiDAR (voir world/Bushes.hpp).
//
// Deux variantes, choisies par la HAUTEUR (donnee), pas par l'exposition :
//  - buisson arrondi : trois lobes bosseles aplatis sur une souche courte,
//    plus large que haut (emprise ~1,3 x la hauteur) ;
//  - petit arbre : tronc court + deux cones decales, silhouette de jeune
//    resineux (pin a crochets / epicea juvenile de lisiere) - c'est la
//    vegetation ligneuse qui depasse ~1,8 m hors couvert forestier.
MeshData shrubMeshData();
MeshData smallTreeMeshData();

// Rayon d'emprise horizontale a l'echelle 1 (x hauteur en metres).
inline constexpr float kShrubFootprintRadius = 0.68f;
inline constexpr float kSmallTreeFootprintRadius = 0.40f;
// Partie enterree sous l'origine (x echelle) : pas de pied qui flotte en pente.
inline constexpr float kBushBuriedDepth = 0.08f;

} // namespace vercora
