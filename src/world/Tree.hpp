#pragma once

#include "world/TreeMesh.hpp"
#include "world/TreeSpecies.hpp"

#include <glm/glm.hpp>

namespace vercora {

// Proportions partagees par TOUS les arbres de la scene. Une seule valeur pour
// tout le monde, volontairement : le mesh d'arbre est genere une fois avec ces
// proportions et instancie N fois (voir la convention du mesh unite dans
// TreeMesh.hpp), donc un arbre qui aurait d'autres proportions aurait besoin de
// son propre mesh. C'est aussi ce qui garantit que la souche s'emboite pile a
// l'endroit de la coupe.
inline const TreeProportions kTreeProportions{};

// Cycle de vie d'un arbre. La physique de chute ne fait que faire avancer cet
// etat ; le rendu, lui, se contente de le lire.
//
// Settling s'intercale entre Falling et Fallen depuis la Phase 6. En Phase 5,
// l'arbre disparaissait NET a l'instant ou son angle atteignait 90 degres et
// toutes les buches apparaissaient d'un coup : on ne voyait jamais l'arbre
// couche, ce qui se lisait comme un bug d'affichage plutot que comme un
// abattage. Pendant Settling, l'arbre EST dessine, pose au sol, en train
// d'amortir son impact ; ce n'est qu'ensuite qu'il se debite, buche par buche.
enum class TreeState {
    Standing, // debout, abattable
    Falling,  // bascule en cours autour de la charniere
    Settling, // vient de toucher le sol : couche, dessine, en train de s'immobiliser
    Fallen,   // debite : l'arbre n'est plus dessine, la souche et les buches le sont
};

// Un arbre pose sur le terrain.
//
// La geometrie n'est PAS stockee ici : un arbre n'est qu'une position, une
// taille et un etat. C'est tout l'interet du couple MeshHandle/DrawItem - une
// foret de 500 arbres, c'est un seul mesh sur le GPU et 500 matrices.
struct Tree {
    // Pied du tronc, en coordonnees monde, pose sur la surface du terrain.
    glm::vec3 base{0.0f};

    // Hauteur totale en unites monde. Sert aussi de facteur d'echelle UNIFORME
    // du mesh unite : toutes les cotes ci-dessous s'en deduisent.
    float height = 7.0f;

    // Orientation autour de Y, en radians. Purement visuel (le mesh n'est pas a
    // symetrie de revolution : ses 8 facettes le sont a 45 degres pres), mais
    // suffit a ce que deux arbres voisins ne paraissent pas clones.
    float yaw = 0.0f;

    TreeState state = TreeState::Standing;

    // PHASE 26 : l'ESSENCE. Elle decide de deux choses a la fois, et c'est tout
    // l'interet de la porter ici plutot qu'en deux endroits :
    //   * quel MESH est dessine (conifere ou feuillu, world/TreeMesh.hpp) ;
    //   * si l'arbre donne de la RESINE (world/Resin.hpp).
    // La Phase 22 tirait le second d'un hachage de l'indice de l'arbre, sans
    // aucun rapport avec le premier, et notait elle-meme la dette : « un joueur
    // ne peut pas VOIR quel arbre est resineux avant d'essayer ». Il le voit
    // desormais, parce que les deux lisent ce champ.
    //
    // Ce n'est PAS une seconde source de verite : le champ est rempli au moment
    // de la plantation par treeSpeciesAt(), qui est aussi ce qu'appelle le semis
    // de decor. La memoriser evite seulement de la recalculer a chaque image.
    TreeSpecies species = TreeSpecies::Conifer;

    // --- Etat de chute (etape 2) -------------------------------------------
    // Direction horizontale normalisee vers laquelle l'arbre bascule, choisie
    // par Forest::evaluateClearance au moment de l'abattage.
    glm::vec3 fallDirection{1.0f, 0.0f, 0.0f};
    // Angle depuis la verticale, en radians. 0 = debout, pi/2 = couche.
    float fallAngle = 0.0f;
    // Vitesse angulaire courante, en rad/s.
    float fallRate = 0.0f;

    // Temps ecoule depuis l'impact au sol, en secondes. Ne sert que pendant
    // Settling : il pilote l'amortissement du rebond puis le passage au debitage.
    float settleTime = 0.0f;

    // --- Cotes derivees, en unites monde ------------------------------------
    float trunkRadius() const { return kTreeProportions.trunkRadius * height; }
    float crownRadius() const { return kTreeProportions.crownRadius * height; }
    float stumpHeight() const { return kTreeProportions.stumpHeight * height; }

    // Point autour duquel l'arbre pivote pendant sa chute : le sommet de la
    // souche, c'est-a-dire le trait de coupe.
    glm::vec3 hinge() const { return base + glm::vec3(0.0f, stumpHeight(), 0.0f); }

    // Transformation du mesh d'arbre unite vers le monde, etat de chute inclus.
    glm::mat4 modelMatrix() const;

    // Transformation du mesh de souche unite vers le monde. Independante de
    // l'etat de chute : la souche ne bouge pas.
    glm::mat4 stumpMatrix() const;

    // Rayon du CYLINDRE DE COLLISION et altitude de son sommet, en monde.
    // Debout, l'obstacle est le tronc sur toute sa hauteur ; une fois l'arbre
    // coupe, il ne reste que la souche. Le houppier n'entre jamais dans le
    // volume de collision : sa base est a 35 % de la hauteur de l'arbre, donc
    // tres au-dessus de la tete du joueur, et on doit pouvoir marcher dessous.
    float colliderRadius() const { return trunkRadius(); }
    float colliderTopY() const {
        return base.y + (state == TreeState::Standing ? height : stumpHeight());
    }
};

} // namespace vercora
