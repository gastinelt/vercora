#pragma once

namespace vercora {

// ===========================================================================
// PHASE 17 : ce que le SEMIS DE DECOR a besoin de savoir de l'etat du milieu.
// ===========================================================================
// Une seule question, posee en coordonnees monde : ce sol est-il encore
// couvert ? 1 = intact, 0 = nu.
//
// L'interface existe pour la meme raison que HeightSampler : world/Vegetation
// ne doit PAS dependre de world/Habitat. Le semis d'herbe consomme ce contrat
// et ne sait rien de ce qui le calcule - une degradation par abattage
// aujourd'hui, une usure des sols par pietinement demain. Reciproquement,
// Habitat ne connait pas la vegetation : il repond a une question, il ne
// pilote pas un rendu.
class GroundCoverSource {
public:
    virtual ~GroundCoverSource() = default;

    // Facteur multiplicatif de densite du couvert au sol en (worldX, worldZ),
    // dans [0, 1]. Contrat : la fonction est TOTALE - partout ou aucune
    // degradation n'est enregistree, elle doit rendre 1, pas echouer ni rendre
    // zero. Un monde sans Habitat branche se comporte donc exactement comme
    // avant la Phase 17.
    virtual float groundCover(float worldX, float worldZ) const = 0;
};

} // namespace vercora
