#pragma once

#include "core/CollisionSolver.hpp"

#include <vector>

namespace vercora {

// ---------------------------------------------------------------------------
// PHASE 15 : plusieurs sources d'obstacles, un seul solveur.
// ---------------------------------------------------------------------------
// Jusqu'ici le monde n'avait qu'UNE source d'obstacles - `world/Forest`, qui
// connait ses troncs et ses souches - et `Camera::setCollisionSolver` prenait
// donc un pointeur unique. L'editeur en ajoute une seconde (les objets poses),
// et il n'y a aucune raison de faire de la Forest le proprietaire de rochers
// qu'elle n'a pas semes.
//
// D'ou ce composite : il implemente le meme contrat `CollisionSolver` et le
// delegue a une liste. La camera, le velo et le grimpeur continuent de ne voir
// qu'un seul solveur - AUCUN des trois n'a ete modifie pour la Phase 15, et
// c'est precisement ce que l'interface de la Phase 6 permettait.
//
// SEMANTIQUE DE `resolve` : les repoussements sont CHAINES, pas moyennes. Le
// resultat du premier solveur devient la position soumise au deuxieme, et ainsi
// de suite. C'est le comportement correct pour un solveur par repoussement -
// sortir d'un tronc peut faire entrer dans un rocher, et il faut alors sortir du
// rocher aussi - et ca reste conforme au contrat : la fonction est totale, et un
// solveur qui n'a rien a dire rend la position telle quelle.
//
// Ce chainage ne garantit pas a lui seul une position libre (deux obstacles plus
// rapproches que le joueur n'en laissent aucune), mais ce n'etait deja pas le
// cas d'un solveur seul : c'est `moveBody()` qui verifie le resultat par
// `overlaps()` et refuse le deplacement le cas echeant. Rien ne change de ce
// cote, le composite repond simplement "oui" des qu'UN de ses membres chevauche.
//
// Les solveurs ne sont PAS possedes : ils doivent vivre plus longtemps que le
// composite. Un pointeur nul est ignore, ce qui evite a l'appelant de tester
// l'existence de chaque source avant de l'ajouter (la Forest n'existe pas sur
// une scene sans terrain, l'editeur pas encore charge, etc.).
class CompositeCollisionSolver : public CollisionSolver {
public:
    void add(const CollisionSolver* solver) {
        if (solver != nullptr) {
            solvers_.push_back(solver);
        }
    }

    void clear() { solvers_.clear(); }

    std::size_t size() const { return solvers_.size(); }

    glm::vec3 resolve(const BodyCylinder& body) const override {
        BodyCylinder probe = body;
        for (const CollisionSolver* solver : solvers_) {
            probe.position = solver->resolve(probe);
        }
        return probe.position;
    }

    bool overlaps(const BodyCylinder& body) const override {
        for (const CollisionSolver* solver : solvers_) {
            if (solver->overlaps(body)) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<const CollisionSolver*> solvers_; // non possedes
};

} // namespace vercora
