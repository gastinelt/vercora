#pragma once

namespace vercora {

// ===========================================================================
// PHASE 22 : ce que le semis des PLANTES A FIBRES a besoin de savoir du milieu.
// ===========================================================================
// Une seule question, posee en coordonnees monde : ce sol a-t-il ete OUVERT ?
// 0 = intact, 1 = entierement degage.
//
// C'est le pendant EXACT de world/GroundCover.hpp, et l'interface existe pour
// la meme raison : world/FiberPlants ne doit PAS dependre de world/Habitat. Le
// semis consomme ce contrat et ne sait rien de ce qui le calcule.
//
// MAIS CE N'EST PAS LA MEME QUESTION, ET C'EST TOUT L'INTERET D'AVOIR DEUX
// INTERFACES PLUTOT QU'UNE. `groundCover` demande "reste-t-il du couvert ici ?"
// et sert a RARÉFIER l'herbe ; `clearing` demande "a-t-on ouvert ici ?" et sert
// a FAIRE POUSSER les ronces. Les deux se lisent aujourd'hui dans le meme
// compteur de deforestation d'Habitat, mais elles n'y liront pas toujours la
// meme chose : le jour ou l'usure des sols par pietinement degradera le couvert
// (annonce en Phase 17 et toujours pas fait), elle fera baisser groundCover sans
// ouvrir quoi que ce soit - un sentier tasse ne fait pas pousser d'orties. Les
// fusionner maintenant obligerait a les separer a ce moment-la.
//
// Contrat : la fonction est TOTALE - partout ou aucune ouverture n'est
// enregistree, elle doit rendre 0, pas echouer. Un monde sans Habitat branche
// se comporte donc comme un monde jamais coupe, et les plantes a fibres n'y
// poussent que par le critere de LISIERE (voir FiberPlants.hpp).
class ClearingSource {
public:
    virtual ~ClearingSource() = default;

    // Part de sol ouvert en (worldX, worldZ), dans [0, 1].
    virtual float clearing(float worldX, float worldZ) const = 0;
};

} // namespace vercora
