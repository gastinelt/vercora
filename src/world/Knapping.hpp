#pragma once

#include <glm/glm.hpp>

namespace vercora {

class Ground;
class Inventory;

// Issue d'une tentative de percussion.
enum class KnapResult {
    Success,          // un eclat est tombe aux pieds du joueur
    NotEnoughStones,  // il en faut deux : un percuteur et un nucleus
};

// Message francais pret a journaliser pour une issue donnee.
const char* knapResultMessage(KnapResult result);

// --- Taillage de pierre par percussion --------------------------------------
// Le tout premier maillon de la chaine d'artisanat : deux pierres entrechoquees
// donnent un eclat tranchant.
//
// Choix de conception (Phase 6), a assumer et a revoir quand la taille de pierre
// deviendra un vrai sujet :
//
//  - IL FAUT DEUX PIERRES. Une seule ne suffit pas : la percussion suppose un
//    percuteur (celui qu'on tient) et un nucleus (celui qu'on frappe). C'est ce
//    qui oblige le joueur a explorer pour en trouver une seconde, et c'est la
//    seule "recette" du systeme - il n'y en a volontairement aucune table.
//
//  - UNE SEULE PIERRE EST CONSOMMEE, le nucleus, considere comme epuise apres
//    avoir livre son eclat. Le percuteur, lui, survit : c'est l'outil, pas la
//    matiere. Consommer les deux rendrait la mecanique decourageante ; n'en
//    consommer aucune la rendrait gratuite.
//
//  - L'ECLAT TOMBE DANS LE MONDE, devant le joueur, et doit etre ramasse comme
//    n'importe quel objet au sol. Il n'atterrit PAS directement dans
//    l'inventaire : la philosophie "zero menu virtuel" du jeu veut que ce qui
//    est produit existe d'abord comme une chose posee quelque part.
//
// facing : direction du regard du joueur (pas necessairement horizontale, elle
// est projetee et normalisee ici). playerPosition : position de l'oeil.
KnapResult knapStones(Inventory& inventory, Ground& ground,
                      const glm::vec3& playerPosition, const glm::vec3& facing);

} // namespace vercora
