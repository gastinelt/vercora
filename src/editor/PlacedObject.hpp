#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <string>

namespace vercora {

// Identifiant "aucun objet". Les identifiants reels commencent a 1, ce qui rend
// zero utilisable comme absence sans valeur sentinelle a part.
//
// Declare ICI et non dans EditorScene.hpp, ou il vivait d'abord : c'est une
// valeur de PlacedObject::id, donc elle appartient au meme en-tete que le champ
// qu'elle qualifie. Le controleur, qui manipule des identifiants sans avoir
// besoin de la scene, ne doit pas avoir a inclure celle-ci pour ca.
inline constexpr uint32_t kNoPlacedObject = 0;

// ---------------------------------------------------------------------------
// PHASE 15 : un objet pose par l'editeur.
// ---------------------------------------------------------------------------
// C'est l'unite de la scene editable, et c'est exactement ce qui est serialise
// dans le fichier de scene (voir editor/SceneFile.hpp). Volontairement PLAT et
// sans pointeur : l'objet designe son asset par son IDENTIFIANT texte, pas par
// un indice de catalogue ni par un MeshHandle.
//
// Pourquoi cet indirection par nom, alors qu'un indice serait plus rapide : un
// indice de catalogue depend de l'ORDRE de scan du dossier d'assets, donc du
// systeme de fichiers. Ajouter un .obj dans assets/editor/ deplacerait tous les
// objets deja poses vers un autre modele, en silence. Le nom, lui, survit a
// l'ajout, au retrait et au renommage de tout le reste - et quand l'asset
// manque vraiment, on peut le DIRE au lieu d'afficher n'importe quoi (voir
// EditorScene::resolve).
//
// TRANSFORMATION AUTORISEE : translation, rotation autour de Y, echelle
// UNIFORME. Pas de rotation libre, pas d'echelle non uniforme. Ce n'est pas une
// limitation de l'editeur mais le contrat de `DrawItem` : le vertex shader
// utilise `mat3(model)` comme normal matrix, ce qui n'est exact que pour une
// rotation suivie d'une echelle uniforme. Autoriser plus imposerait de calculer
// et de pousser une vraie normal matrix - possible (il reste 48 octets de push
// constant) mais hors scope de cette session.
struct PlacedObject {
    // Identite STABLE, attribuee par EditorScene et jamais reutilisee dans une
    // session. La selection, la suppression et l'annulation raisonnent la-dessus
    // et non sur un indice de tableau, qui bouge des qu'un objet est supprime au
    // milieu de la liste.
    uint32_t id = 0;

    // Identifiant de catalogue de l'asset (ex. "rocher"). Voir plus haut.
    std::string asset;

    // Position monde du point d'ANCRAGE, c'est-a-dire du pied de l'objet (les
    // assets d'editeur ont leur origine au sol - voir editor/EditorAssets.hpp).
    glm::vec3 position{0.0f};

    // Rotation autour de l'axe vertical, en degres, dans le repere main droite
    // du moteur (Y vers le haut). 0 = l'asset tel qu'il a ete cree.
    float yawDegrees = 0.0f;

    // Echelle UNIFORME. 1 = taille reelle de l'asset.
    float scale = 1.0f;

    // L'objet bloque-t-il le joueur ? Voir EditorScene, qui implemente
    // CollisionSolver : un objet solide est un cylindre vertical de plus dans le
    // monde, au meme titre qu'un tronc.
    //
    // Reglable par objet et non par asset : le meme rocher peut etre un obstacle
    // sur un sentier et un simple decor a flanc de falaise, la ou le joueur ne
    // passera jamais. Et surtout, un asset dont l'empreinte cylindrique est
    // fausse (l'abri, qui a un interieur) doit pouvoir etre pose sans bloquer.
    bool solid = true;

    // Matrice modele. translate * rotateY * scale : rotation suivie d'une
    // echelle uniforme, donc conforme au contrat de DrawItem.
    glm::mat4 modelMatrix() const {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), position);
        model = glm::rotate(model, glm::radians(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
        return glm::scale(model, glm::vec3(scale));
    }
};

} // namespace vercora
