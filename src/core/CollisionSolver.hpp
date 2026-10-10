#pragma once

#include <glm/glm.hpp>

namespace vercora {

// Le corps du joueur, modelise par un CYLINDRE VERTICAL.
//
// Un cylindre et non une capsule ou un mesh : les seuls obstacles de cette phase
// sont des troncs et des souches, eux-memes cylindriques et verticaux. Le test
// se reduit donc a un probleme de cercles dans le plan XZ plus un recouvrement
// d'intervalles en Y - quelques lignes, aucun cout mesurable, et une precision
// tres suffisante tant que rien n'est en surplomb.
struct BodyCylinder {
    // Position de l'OEIL (c'est ce que manipule la camera), en monde. Seuls X et
    // Z sont corriges par la resolution : la verticale reste le domaine du
    // terrain (voir Camera::snapToTerrain).
    glm::vec3 position{0.0f};

    // Demi-largeur du joueur.
    float radius = 0.35f;

    // Bande verticale occupee par le corps. Un obstacle qui n'y mord pas ne
    // bloque pas : c'est ce qui permettra plus tard de passer sous une branche
    // haute, et c'est deja ce qui distingue un tronc entier d'une souche basse.
    float feetY = 0.0f;
    float headY = 0.0f;
};

// Ce que la camera a besoin de savoir du monde pour ne pas le traverser.
//
// Meme intention que HeightSampler juste a cote : core/Camera ne doit dependre
// NI de world/Forest, ni de ce qui peuplera le monde demain (rochers, batis,
// couche LiDAR classifiee). La camera consomme ce contrat, l'implementation vit
// du cote des donnees - ici world/Forest, qui connait ses troncs et ses souches.
class CollisionSolver {
public:
    virtual ~CollisionSolver() = default;

    // Renvoie la position corrigee : identique a body.position si le joueur ne
    // chevauche aucun obstacle, repoussee juste en dehors sinon.
    //
    // Contrat : la fonction est TOTALE et ne deplace jamais le joueur qu'a
    // l'horizontale. Une implementation qui n'a rien a dire renvoie la position
    // telle quelle. Corriger par repoussement (et non en annulant le
    // deplacement) est ce qui donne gratuitement le glissement le long d'un
    // obstacle : on longe un tronc au lieu de s'y coller net.
    //
    // Le repoussement ne garantit PAS a lui seul une position libre : entre deux
    // obstacles plus rapproches que la largeur du joueur, il n'en existe pas. Le
    // resultat doit donc etre verifie par overlaps() - c'est ce que fait
    // moveBody() ci-dessous, et c'est par la que doit passer tout appelant.
    virtual glm::vec3 resolve(const BodyCylinder& body) const = 0;

    // Le corps chevauche-t-il un obstacle a cette position ?
    virtual bool overlaps(const BodyCylinder& body) const = 0;
};

// Deplace un corps d'une position CONNUE LIBRE vers une position desiree, en
// respectant les obstacles. C'est le point d'entree unique de la collision : la
// camera et les tests passent tous les deux par ici, donc ils exercent
// exactement la meme logique.
//
// Pourquoi le repli sur la position precedente : le repoussement seul peut
// FAIRE PASSER le joueur au travers d'un passage plus etroit que lui. Chaque
// obstacle le repousse tour a tour, et la somme de ces poussees a une composante
// le long du passage : le joueur se retrouve extrude de l'autre cote, alors
// qu'il n'aurait jamais du pouvoir s'y engager. Verifier le resultat et refuser
// le deplacement quand il reste bloque supprime ce cas, et c'est la seule facon
// simple de le garantir avec un solveur par repoussement.
inline glm::vec3 moveBody(const CollisionSolver& solver, const BodyCylinder& desired,
                          const glm::vec3& previousPosition) {
    const glm::vec3 pushed = solver.resolve(desired);

    BodyCylinder probe = desired;
    probe.position = pushed;
    if (!solver.overlaps(probe)) {
        return pushed;
    }

    // Le repoussement n'a pas trouve de place. On reste ou on etait, si tant est
    // que ce soit encore libre.
    probe.position = { previousPosition.x, desired.position.y, previousPosition.z };
    if (!solver.overlaps(probe)) {
        return probe.position;
    }

    // Meme l'ancienne position est occupee - typiquement un arbre plante sur le
    // joueur, ou un obstacle apparu autour de lui. Le repoussement reste alors
    // le moins mauvais choix : il l'ecarte au lieu de le laisser enfonce.
    return pushed;
}

} // namespace vercora
