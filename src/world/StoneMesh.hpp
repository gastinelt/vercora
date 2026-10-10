#pragma once

#include "renderer/MeshData.hpp"

namespace vercora {

// Geometries synthetiques du premier maillon d'artisanat (taillage de pierre par
// percussion), generees EN CODE comme celles de l'arbre : aucun fichier, aucun
// telechargement.
//
// Ces meshes ne sont PAS a l'echelle unite (contrairement a l'arbre et a la
// souche) : leurs dimensions sont ABSOLUES, comme la buche et la branche. Une
// pierre fait la meme taille partout, donc une instance se pose par
// translate * rotate SANS aucune echelle - la normal matrix est alors exacte au
// sens strict, et non seulement a un facteur pres.
//
// Les deux meshes sont centres sur l'origine, y compris verticalement : c'est ce
// qui permet a Ground::drop de les poser en ajoutant simplement leur demi-hauteur
// a l'altitude du terrain (voir groundItemRestRadius).

// Un galet : polyedre irregulier bas-poly, facette, legerement aplati pour qu'il
// ait l'air de reposer sur le sol plutot que d'y etre pose en equilibre.
MeshData stoneMeshData();

// Un eclat tranchant : face ventrale PLATE, face dorsale en arete, perimetre
// aigu. C'est la silhouette caracteristique d'un eclat de percussion, et elle se
// lit meme a quelques dizaines de triangles.
MeshData flakeMeshData();

// Dimensions en unites monde. Exposees parce que le code de placement en a
// besoin pour poser les objets sur le sol sans qu'ils s'enfoncent ni flottent.
// Les rayons sont des DEMI-HAUTEURS (l'objet est centre sur son origine).
inline constexpr float kStoneRadius    = 0.20f; // demi-hauteur du galet
inline constexpr float kStoneWidth     = 0.30f; // demi-largeur horizontale
inline constexpr float kFlakeRadius    = 0.025f; // demi-epaisseur de l'eclat
inline constexpr float kFlakeLength    = 0.24f;
inline constexpr float kFlakeWidth     = 0.15f;

} // namespace vercora
