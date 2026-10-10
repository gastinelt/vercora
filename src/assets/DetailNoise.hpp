#pragma once

#include "assets/TextureFile.hpp"

#include <cstdint>

namespace vercora {

// ===========================================================================
// PHASE 27 - Texture de bruit pour le micro-relief du sol
// ===========================================================================
// POURQUOI UNE TEXTURE et non un bruit calcule dans le shader : la premiere
// version calculait un bruit de valeur a derivees analytiques PAR PIXEL. La
// mesure (Release, UHD 770, vue au ras du sol, conditions entrelacees) a donne
// +1 490 us par image, dont ~200 us PAR OCTAVE : du calcul pur, confirme par
// bissection (sans octave : +330 us ; 2 octaves : +720 us). Changer le hash
// n'y changeait rien. Une lecture de texture par octave remplace les quatre
// hash et l'interpolation.
//
// CE QUE CONTIENT LA TEXTURE : un bruit de valeur 2D PERIODIQUE (il se raccorde
// bord a bord, pour etre echantillonne en repetition), sur une grille de
// `cells` x `cells` cellules, et ses derivees analytiques :
//   R = valeur dans [0, 1]
//   G = d(valeur)/du, B = d(valeur)/dv, par unite de CELLULE, codees de
//       [-kDetailNoiseSlopeRange, +kDetailNoiseSlopeRange] vers [0, 255]
//   A = 255
// Les mipmaps sont des moyennes 2x2 : moyenner des derivees ATTENUE le relief
// des niveaux lointains, ce qui est exactement le comportement voulu (un grain
// sous-pixel doit s'effacer, pas scintiller).
//
// Fonction PURE et deterministe : aucune dependance Vulkan, testable headless.
inline constexpr float kDetailNoiseSlopeRange = 1.5f;

TextureData makeDetailNoiseTexture(uint32_t size = 256, uint32_t cells = 32,
                                   uint32_t seed = 0x6D1C0u);

} // namespace vercora
