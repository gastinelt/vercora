#include "world/Bike.hpp"

#include "core/CollisionSolver.hpp"
#include "core/HeightSampler.hpp"

#include <algorithm>
#include <cmath>

namespace vercora {

namespace {

// Pesanteur, en m/s^2. Le reste du jeu travaille en metres depuis la Phase 7
// (donnees LiDAR), la valeur reelle a donc un sens ici.
constexpr float kGravity = 9.81f;

// En dessous de cette vitesse, un velo qui RALENTIT est considere a l'arret : on
// annule la vitesse au lieu de laisser un residu qui ferait deriver la position
// d'un millimetre par seconde indefiniment.
//
// Le "qui ralentit" n'est pas un detail : applique inconditionnellement, ce
// seuil empeche tout DEMARRAGE dont l'acceleration est inferieure a
// kSpeedEpsilon / dt (1,2 m/s^2 a 60 images/s). Constate directement - le velo
// restait cloue a 0 km/h des 10 % de montee, et ne pouvait pas non plus se
// laisser descendre une pente de 10 % en roue libre, alors que le bilan des
// accelerations etait franchement positif dans les deux cas.
constexpr float kSpeedEpsilon = 0.02f;

} // namespace

Bike::Bike(const BikeParams& params) : params_(params) {}

glm::vec3 Bike::forward() const {
    const float rad = glm::radians(heading_);
    return glm::vec3(std::cos(rad), 0.0f, std::sin(rad));
}

glm::vec3 Bike::right() const {
    // cross(forward, worldUp) avec forward = (cos, 0, sin) : (-sin, 0, cos).
    const float rad = glm::radians(heading_);
    return glm::vec3(-std::sin(rad), 0.0f, std::cos(rad));
}

glm::vec2 Bike::terrainGradient(const HeightSampler& terrain) const {
    const float s = params_.slopeSampleStep;
    // Differences finies CENTREES : deux fois plus precises qu'une difference
    // avant pour le meme nombre de points, et surtout SYMETRIQUES - une
    // difference avant donnerait une pente differente selon le sens de marche
    // sur le meme sentier, ce qui se sentirait a l'aller-retour.
    const float hxPlus = terrain.heightAt(position_.x + s, position_.z);
    const float hxMinus = terrain.heightAt(position_.x - s, position_.z);
    const float hzPlus = terrain.heightAt(position_.x, position_.z + s);
    const float hzMinus = terrain.heightAt(position_.x, position_.z - s);
    return glm::vec2((hxPlus - hxMinus) / (2.0f * s), (hzPlus - hzMinus) / (2.0f * s));
}

float Bike::slopeAlongHeading(const HeightSampler& terrain) const {
    const glm::vec2 gradient = terrainGradient(terrain);
    const glm::vec3 f = forward();
    return gradient.x * f.x + gradient.y * f.z;
}

void Bike::mount(const glm::vec3& position, float headingDegrees,
                 const HeightSampler& terrain) {
    position_ = position;
    heading_ = headingDegrees;
    // Contact permanent : la verticale n'est jamais une variable d'etat du velo,
    // elle est TOUJOURS relue du terrain. C'est vrai des la premiere frame, sans
    // quoi enfourcher le velo depuis une camera en vol libre le ferait apparaitre
    // suspendu.
    position_.y = terrain.heightAt(position_.x, position_.z) + params_.eyeHeight;
    speed_ = 0.0f; // pas de demarrage lance : on part a l'arret
    pitch_ = 0.0f;
    roll_ = 0.0f;
    riding_ = true;
}

void Bike::update(float deltaTime, float steerInput, float pedalInput, float brakeInput,
                  const HeightSampler& terrain, const CollisionSolver* obstacles,
                  float wetness) {
    if (!riding_ || deltaTime <= 0.0f) {
        return;
    }

    steerInput = std::clamp(steerInput, -1.0f, 1.0f);
    pedalInput = std::clamp(pedalInput, 0.0f, 1.0f);
    brakeInput = std::clamp(brakeInput, 0.0f, 1.0f);

    // PHASE 23 : etat du sol. Deux coefficients, aucun etat ajoute - voir
    // BikeParams::wetRollingGain pour ce que ce modele fait et ne fait pas.
    const float wet = std::isfinite(wetness) ? std::clamp(wetness, 0.0f, 1.0f) : 0.0f;

    // -----------------------------------------------------------------------
    // 1. Direction. Rayon de braquage fini, et qui GRANDIT avec la vitesse.
    // -----------------------------------------------------------------------
    // La vitesse angulaire tenable est celle qui n'excede pas l'adherence :
    //   a_lat = omega * v <= maxLateralAccel   =>   omega <= maxLateralAccel / v
    // Elle est donc INVERSEMENT proportionnelle a la vitesse : a 10 m/s le velo
    // ne tourne qu'a 23 deg/s (rayon 25 m), a 5 m/s a 46 deg/s (rayon 6,3 m).
    // Le plafond maxTurnRateDegPerSec ne mord qu'a tres basse vitesse, ou la
    // formule diverge - c'est la manoeuvre pieds a terre, pas un virage.
    const float gripTurnRate = params_.maxLateralAccel / std::max(speed_, 1e-3f);
    const float turnRate = std::min(gripTurnRate,
                                    glm::radians(params_.maxTurnRateDegPerSec));
    const float yawRate = steerInput * turnRate; // rad/s, > 0 = a droite
    heading_ += glm::degrees(yawRate) * deltaTime;

    // -----------------------------------------------------------------------
    // 2. Dynamique longitudinale. Somme d'accelerations, INTEGREE : aucun terme
    //    n'agit sur la vitesse instantanement, ni le pedalage ni le frein.
    // -----------------------------------------------------------------------
    const float slopeTan = slopeAlongHeading(terrain);
    // La composante de la pesanteur le long de la pente vaut g * sin(theta), pas
    // g * tan(theta) : sur une paroi LiDAR a 300 % la difference est un facteur
    // trois, et tan() divergerait. sin = tan / sqrt(1 + tan^2).
    const float slopeSin = slopeTan / std::sqrt(1.0f + slopeTan * slopeTan);
    const float gravityAccel = -kGravity * slopeSin; // < 0 en montee

    float accel = gravityAccel;

    if (brakeInput > 0.0f) {
        // Freiner et pedaler en meme temps n'a pas de sens : le frein l'emporte.
        accel -= params_.brakeDeceleration * (1.0f - params_.wetBrakingLoss * wet) * brakeInput;
    } else {
        const float pedalFade =
            std::max(0.0f, 1.0f - speed_ / params_.pedalSpeedLimit);
        accel += params_.pedalAcceleration * pedalInput * pedalFade;
    }

    // Resistances : toujours opposees au mouvement. Elles s'appliquent aussi a
    // l'arret, et c'est ce qui empeche le velo de se mettre a rouler seul sur
    // une pente insuffisante (l'ecretage a zero juste apres s'en charge).
    accel -= params_.rollingResistance * (1.0f + params_.wetRollingGain * wet) +
             params_.dragCoefficient * speed_ * speed_;

    speed_ += accel * deltaTime;
    speed_ = std::clamp(speed_, 0.0f, params_.maxSpeed);
    // Seuil d'ARRET, pas seuil de mouvement : on ne l'applique que si le bilan
    // des accelerations est negatif, sinon il tuerait le demarrage (voir
    // kSpeedEpsilon).
    if (speed_ < kSpeedEpsilon && accel <= 0.0f) {
        speed_ = 0.0f;
    }

    // -----------------------------------------------------------------------
    // 3. Deplacement horizontal, puis OBSTACLES, puis contact avec le sol.
    //    Meme ordre que Camera::update en mode marche, et pour la meme raison :
    //    l'inverse laisserait le velo a l'altitude de l'endroit ou il VOULAIT
    //    aller et non de celui ou il a fini.
    // -----------------------------------------------------------------------
    const glm::vec3 previousPosition = position_;
    const glm::vec3 step = forward() * speed_ * deltaTime;
    position_ += step;

    if (obstacles != nullptr) {
        BodyCylinder body;
        body.position = position_;
        body.radius = params_.bodyRadius;
        // Meme bande verticale que le joueur a pied (Camera::resolveCollisions) :
        // c'est ce qui fait qu'une souche basse arrete le velo comme elle arrete
        // les jambes, sans avoir a la traiter a part.
        body.feetY = position_.y - params_.eyeHeight;
        body.headY = position_.y + 0.15f;

        // moveBody() et non resolve() : c'est le point d'entree UNIQUE de la
        // collision du projet, celui que la camera et les tests empruntent deja.
        // Rien n'est reecrit ici.
        const glm::vec3 corrected = moveBody(*obstacles, body, previousPosition);
        position_.x = corrected.x;
        position_.z = corrected.z;

        // Le choc coute de la vitesse, proportionnellement a ce que l'obstacle a
        // mange du pas. Frotter un tronc en le longeant n'en retire presque rien ;
        // le percuter de face arrete net. C'est la SEULE consequence d'un choc
        // dans cette session - ni degats ni chute, explicitement hors scope.
        const float desired = std::sqrt(step.x * step.x + step.z * step.z);
        if (desired > 1e-4f) {
            const float dx = position_.x - previousPosition.x;
            const float dz = position_.z - previousPosition.z;
            const float actual = std::sqrt(dx * dx + dz * dz);
            speed_ *= std::clamp(actual / desired, 0.0f, 1.0f);
            if (speed_ < kSpeedEpsilon) {
                speed_ = 0.0f;
            }
        }
    }

    // Contact PERMANENT avec la surface : la verticale n'est pas integree, elle
    // est imposee. Un velo ne decolle pas d'une bosse dans ce modele.
    position_.y = terrain.heightAt(position_.x, position_.z) + params_.eyeHeight;

    // -----------------------------------------------------------------------
    // 4. Assiette VISUELLE. N'entre dans aucun calcul physique.
    // -----------------------------------------------------------------------
    const glm::vec2 gradient = terrainGradient(terrain);
    const glm::vec3 f = forward();
    const glm::vec3 r = right();

    // Tangage : le nez suit la pente le long du cap. atan d'une tangente.
    const float alongTan = gradient.x * f.x + gradient.y * f.z;
    pitch_ = glm::degrees(std::atan(alongTan));

    // Roulis, deux causes qui s'ajoutent :
    //  - le DEVERS : si le sol monte a droite, le velo penche a gauche, donc
    //    un roulis negatif (positif = penche a droite, cf. l'en-tete) ;
    //  - le VIRAGE : le cycliste s'incline vers l'interieur de l'angle
    //    d'equilibre atan(a_lat / g), exactement comme un vrai deux-roues.
    const float crossTan = gradient.x * r.x + gradient.y * r.z;
    const float bankFromSlope = -glm::degrees(std::atan(crossTan));
    const float lateralAccel = yawRate * speed_; // > 0 = vers la droite
    const float bankFromTurn = glm::degrees(std::atan(lateralAccel / kGravity));

    roll_ = std::clamp(bankFromSlope + bankFromTurn,
                       -params_.maxVisualTiltDegrees, params_.maxVisualTiltDegrees);
    pitch_ = std::clamp(pitch_, -params_.maxVisualTiltDegrees,
                        params_.maxVisualTiltDegrees);
}

} // namespace vercora
