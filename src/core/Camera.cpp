#include "core/Camera.hpp"

#include "core/CollisionSolver.hpp"
#include "core/HeightSampler.hpp"
#include "core/SlopeLimits.hpp"
#include "core/Logger.hpp"
#include "core/Window.hpp" // apporte aussi les constantes GLFW_*

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace vercora {

Camera::Camera(glm::vec3 position, float yawDegrees, float pitchDegrees)
    : position_(position), yaw_(yawDegrees), pitch_(pitchDegrees) {
    updateVectors();
}

void Camera::setMode(CameraMode mode) {
    if (mode == CameraMode::Walk && heightSampler_ == nullptr) {
        logWarn("Mode marche indisponible : aucun terrain n'est charge.");
        return;
    }

    mode_ = mode;
    if (mode_ == CameraMode::Walk) {
        // Poser la camera tout de suite : sinon elle resterait a son altitude de
        // vol jusqu'a la premiere frame, ce qui donnerait un saut visible.
        snapToTerrain();
    }
    switch (mode_) {
        case CameraMode::Walk:
            logInfo("Camera : mode MARCHE (la camera suit le relief).");
            break;
        case CameraMode::Free:
            logInfo("Camera : mode LIBRE (vol).");
            break;
        case CameraMode::Glide:
            logInfo("Camera : mode PARAPENTE (vue a la troisieme personne).");
            break;
        case CameraMode::Ride:
            logInfo("Camera : mode VELO (vue a la troisieme personne).");
            break;
        case CameraMode::Climb:
            logInfo("Camera : mode ESCALADE (premiere personne, regard libre).");
            break;
    }
}

void Camera::setChasePose(const glm::vec3& eye, const glm::vec3& lookAtPoint) {
    position_ = eye;
    front_ = glm::normalize(lookAtPoint - eye);
    right_ = glm::normalize(glm::cross(front_, worldUp_));
    up_ = glm::normalize(glm::cross(right_, front_));

    yaw_ = glm::degrees(std::atan2(front_.z, front_.x));
    pitch_ = glm::degrees(std::asin(std::clamp(front_.y, -1.0f, 1.0f)));
}

void Camera::snapToTerrain() {
    if (heightSampler_ == nullptr) {
        return;
    }
    position_.y = heightSampler_->heightAt(position_.x, position_.z) + eyeHeight_;
}

void Camera::resolveCollisions(const glm::vec3& previousPosition) {
    if (collisionSolver_ == nullptr) {
        return;
    }

    BodyCylinder body;
    body.position = position_;
    body.radius = bodyRadius_;
    // position_ est la hauteur d'OEIL : les pieds sont une hauteur d'oeil plus
    // bas, la tete juste au-dessus des yeux. Cette bande est ce qui permet a une
    // souche basse de bloquer les jambes sans qu'on ait a la traiter a part.
    body.feetY = position_.y - eyeHeight_;
    body.headY = position_.y + 0.15f;

    // moveBody et non resolve directement : c'est lui qui refuse le deplacement
    // quand aucune position libre n'existe, au lieu de laisser le joueur etre
    // extrude au travers d'un passage plus etroit que lui (voir CollisionSolver).
    const glm::vec3 corrected = moveBody(*collisionSolver_, body, previousPosition);

    // Seul l'horizontal est repris : la verticale appartient au terrain, et la
    // reprendre ici ferait lutter la collision contre snapToTerrain.
    position_.x = corrected.x;
    position_.z = corrected.z;
}

void Camera::setOrientation(float yawDegrees, float pitchDegrees) {
    yaw_ = yawDegrees;
    pitch_ = std::clamp(pitchDegrees, -89.0f, 89.0f);
    updateVectors();
}

void Camera::update(const Window& window, float deltaTime) {
    if (!inputEnabled_) {
        // Camera verrouillee : ni clavier ni souris. La position et
        // l'orientation restent celles fixees par l'appelant. Voir
        // setInputEnabled().
        return;
    }

    if (mode_ == CameraMode::Glide || mode_ == CameraMode::Ride) {
        // L'activite est pilotee de l'exterieur (world/Paraglider,
        // world/Bike) : ni le clavier ni la souris n'agissent ici, y compris la
        // touche F. C'est delibere - voir le commentaire de setChasePose() - et
        // ca evite qu'une bascule Free/Walk parasite ne s'active en plein vol
        // ou en pleine descente.
        return;
    }

    // PHASE 14. L'escalade a un contrat MIXTE, et c'est le seul mode qui en a
    // un : la POSITION appartient a world/Climber (imposee par setPosition() une
    // fois par frame), l'ORIENTATION reste au joueur.
    //
    // Pourquoi la souris continue d'agir alors qu'elle est coupee en vol et a
    // velo : en escalade, les commandes sont RELATIVES A LA PAROI et non au
    // regard - monter suit la ligne de plus grande pente, traverser suit la
    // ligne de niveau, toutes deux imposees par le terrain. Le regard n'a donc
    // plus aucune influence sur le deplacement, et il redevient libre sans rien
    // casser. C'est meme necessaire : accroche a une face, on doit pouvoir lever
    // les yeux pour lire la suite de la voie et regarder en bas pour juger la
    // hauteur, sans que ca deplace le grimpeur d'un centimetre.
    //
    // Concretement : on saute la bascule F et tout le deplacement clavier (que
    // main.cpp consomme pour grimper), on garde le bloc souris tout en bas.
    const bool orientationOnly = (mode_ == CameraMode::Climb);

    if (!orientationOnly) {
        // --- Bascule de mode (front descendant sur F) ---
        const bool toggleDown = window.isKeyPressed(GLFW_KEY_F);
        if (toggleDown && !toggleKeyWasDown_ && !modeLocked_) {
            setMode(mode_ == CameraMode::Free ? CameraMode::Walk : CameraMode::Free);
        }
        toggleKeyWasDown_ = toggleDown;

        // --- Clavier : deplacement ---
        const float velocity = moveSpeed_ * deltaTime;

        if (mode_ == CameraMode::Walk) {
            // Position d'avant le pas. La collision en a besoin comme position de
            // repli : c'est la derniere dont on sait qu'elle etait libre.
            const glm::vec3 previousPosition = position_;

            // Direction du regard projetee sur le plan horizontal : regarder le ciel
            // ne doit pas faire reculer, ni ralentir la marche.
            // pitch_ est ecrete a +/-89 degres, la composante horizontale ne peut
            // donc jamais etre nulle : normalize() est sur.
            const glm::vec3 flatFront = glm::normalize(glm::vec3(front_.x, 0.0f, front_.z));

            // Le pas DEMANDE par le clavier. Il n'est plus applique directement :
            // depuis la Phase 24 il passe par le filtre de pente (voir plus bas).
            // right_ est deja horizontal par construction (cross(front, worldUp)).
            glm::vec3 desiredStep(0.0f);
            if (window.isKeyPressed(GLFW_KEY_W)) desiredStep += flatFront * velocity;
            if (window.isKeyPressed(GLFW_KEY_S)) desiredStep -= flatFront * velocity;
            if (window.isKeyPressed(GLFW_KEY_A)) desiredStep -= right_ * velocity;
            if (window.isKeyPressed(GLFW_KEY_D)) desiredStep += right_ * velocity;
            // Espace/shift volontairement ignores : c'est le terrain qui decide de Y.

            // PHASE 24 : la pente limite la marche. Le filtre rend la position
            // horizontale retenue - pas a vitesse pleine sous kWalkComfortSlope,
            // ralenti au-dela, nul a kWalkMaxSlope, plus une glissade vers l'aval
            // si le joueur se trouve sur du terrain ou il ne tient pas debout.
            // Voir core/SlopeLimits.hpp pour le choix des seuils et, surtout,
            // pour la raison qui les lie a ClimberParams::wallSlope.
            //
            // Le filtre a besoin du terrain : sans HeightSampler il n'y a de
            // toute facon pas de mode Walk (setMode le refuse), mais update()
            // peut etre atteint avec un sampler retire entre-temps.
            if (heightSampler_ != nullptr) {
                lastWalkStep_ = resolveWalkStep(*heightSampler_, position_, desiredStep,
                                                deltaTime, walkSlopeParams_);
                position_ = lastWalkStep_.position;
            } else {
                position_ += desiredStep;
            }

            // ORDRE IMPORTANT : on corrige d'abord la position horizontale contre
            // les obstacles, on repose ENSUITE la camera sur le terrain. L'inverse
            // laisserait la camera a l'altitude de l'endroit ou elle voulait aller
            // et non de celui ou elle a fini, ce qui se voit immediatement quand on
            // longe un tronc sur une pente.
            resolveCollisions(previousPosition);
            snapToTerrain();
        } else {
            if (window.isKeyPressed(GLFW_KEY_W)) position_ += front_ * velocity;   // Z (AZERTY)
            if (window.isKeyPressed(GLFW_KEY_S)) position_ -= front_ * velocity;   // S
            if (window.isKeyPressed(GLFW_KEY_A)) position_ -= right_ * velocity;   // Q (AZERTY)
            if (window.isKeyPressed(GLFW_KEY_D)) position_ += right_ * velocity;   // D
            if (window.isKeyPressed(GLFW_KEY_SPACE))        position_ += worldUp_ * velocity;
            if (window.isKeyPressed(GLFW_KEY_LEFT_SHIFT))   position_ -= worldUp_ * velocity;
        }
    }

    // --- Souris : rotation seulement si clic droit maintenu ---
    if (window.isMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) {
        double x = 0.0;
        double y = 0.0;
        window.getCursorPos(x, y);

        if (!rotating_) {
            // Premier frame du drag : on initialise sans "saut" de camera.
            rotating_ = true;
            lastX_ = x;
            lastY_ = y;
        }

        const double dx = x - lastX_;
        const double dy = lastY_ - y; // ecran : Y vers le bas -> on inverse
        lastX_ = x;
        lastY_ = y;

        yaw_ += static_cast<float>(dx) * mouseSensitivity_;
        pitch_ += static_cast<float>(dy) * mouseSensitivity_;
        pitch_ = std::clamp(pitch_, -89.0f, 89.0f); // evite le retournement

        updateVectors();
    } else {
        rotating_ = false;
    }
}

glm::mat4 Camera::viewMatrix() const {
    return glm::lookAt(position_, position_ + front_, up_);
}

void Camera::updateVectors() {
    const float yawRad = glm::radians(yaw_);
    const float pitchRad = glm::radians(pitch_);

    glm::vec3 front;
    front.x = std::cos(yawRad) * std::cos(pitchRad);
    front.y = std::sin(pitchRad);
    front.z = std::sin(yawRad) * std::cos(pitchRad);

    front_ = glm::normalize(front);
    right_ = glm::normalize(glm::cross(front_, worldUp_));
    up_ = glm::normalize(glm::cross(right_, front_));
}

} // namespace vercora
