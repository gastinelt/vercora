#include "world/Tree.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace vercora {

glm::mat4 Tree::modelMatrix() const {
    // Arbre debout : le mesh unite est oriente puis mis a l'echelle de sa
    // hauteur reelle. Rotation + echelle UNIFORME = mat3(model) reste une normal
    // matrix valide (contrat de DrawItem).
    const glm::mat4 standing =
        glm::scale(glm::rotate(glm::translate(glm::mat4(1.0f), base),
                               yaw, glm::vec3(0.0f, 1.0f, 0.0f)),
                   glm::vec3(height));

    const bool tilted = (state == TreeState::Falling || state == TreeState::Settling);
    if (!tilted || fallAngle <= 0.0f) {
        return standing;
    }

    // Chute : rotation MONDE autour de la charniere (le trait de coupe).
    //
    // L'axe est horizontal et perpendiculaire a la direction de chute. On veut
    // qu'un angle croissant couche l'arbre VERS fallDirection : en notant f
    // cette direction et Y la verticale, la derivee de Y par rotation autour
    // d'un axe A vaut A x Y. Il faut donc A x Y = f, ce que satisfait
    // A = (f.z, 0, -f.x). Se tromper de signe ici fait tomber l'arbre a
    // l'exact oppose de la zone degagee - erreur silencieuse et tres visible.
    const glm::vec3 axis(fallDirection.z, 0.0f, -fallDirection.x);
    const glm::vec3 pivot = hinge();

    const glm::mat4 aboutHinge =
        glm::translate(glm::mat4(1.0f), pivot) *
        glm::rotate(glm::mat4(1.0f), fallAngle, axis) *
        glm::translate(glm::mat4(1.0f), -pivot);

    return aboutHinge * standing;
}

glm::mat4 Tree::stumpMatrix() const {
    return glm::scale(glm::rotate(glm::translate(glm::mat4(1.0f), base),
                                  yaw, glm::vec3(0.0f, 1.0f, 0.0f)),
                      glm::vec3(height));
}

} // namespace vercora
