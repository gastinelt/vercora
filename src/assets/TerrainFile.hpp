#pragma once

#include "assets/Heightmap.hpp"

#include <string>

namespace vercora {

// Chargement d'une heightmap .vchm ("Vercora HeightMap") produite hors ligne par
// tools/lidar_to_heightmap.py a partir d'une dalle LiDAR HD de l'IGN.
//
// POURQUOI UN FORMAT MAISON plutot que lire le .laz directement. Un .laz est un
// nuage de points COMPRESSE et non trie : en extraire une grille demande un
// decodeur LASzip, puis un tri, une agregation par cellule, un bouchage des
// trous et un filtrage des valeurs aberrantes - environ 3,5 s et 170 Mo de
// lecture pour la dalle de test, pour un resultat toujours identique. C'est du
// travail de PREPARATION, pas de chargement. Le jeu, lui, lit 4 Mo de float
// deja propres et les envoie au maillage. La frontiere est la meme que celle des
// shaders, compiles en SPIR-V par le build et non en GLSL a l'execution.
//
// FORMAT (petit-boutiste, en-tete de 120 octets, puis les altitudes) - la
// definition de reference, avec les justifications, est en tete de
// tools/lidar_to_heightmap.py :
//
//     0    "VCHM"                 magie
//     4    uint32  version = 1
//     8    uint32  samplesX       echantillons d'ouest en est
//     12   uint32  samplesZ       echantillons du nord vers le sud
//     16   float32 spacing        metres entre deux echantillons
//     20   uint32  reserve
//     24   float64 originEast     Lambert 93 X de l'echantillon ix = 0
//     32   float64 originNorth    Lambert 93 Y de l'echantillon iz = 0
//     40   float32 minHeight      \ redondants avec les donnees : sert de
//     44   float32 maxHeight      / controle de coherence, pas de source
//     48   uint32  epsg
//     52   uint32  flags          bit 0 : altitudes absolues (IGN69)
//     56   char[64] label
//     120  float32 heights[samplesZ * samplesX]
//
// LIMITE ASSUMEE : seules les grilles CARREES sont acceptees, parce que
// Heightmap et TerrainMesh n'exposent qu'une seule resolution. Une dalle IGN
// etant un carre de 1 km, la contrainte ne coute rien aujourd'hui ; elle devra
// tomber le jour ou l'on assemblera plusieurs dalles en un rectangle. Un fichier
// non carre est refuse avec un message explicite plutot que charge de travers.
//
// Lance std::runtime_error avec un message exploitable si le fichier est
// absent, tronque, d'une autre version, ou incoherent.
Heightmap loadTerrainFile(const std::string& path);

} // namespace vercora
