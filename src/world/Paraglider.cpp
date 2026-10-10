#include "world/Paraglider.hpp"

#include "core/HeightSampler.hpp"

#include <algorithm>
#include <cmath>

namespace vercora {

Paraglider::Paraglider(const ParagliderParams& params) : params_(params) {}

glm::vec3 Paraglider::forward() const {
    const float rad = glm::radians(heading_);
    return glm::vec3(std::cos(rad), 0.0f, std::sin(rad));
}

void Paraglider::launch(const glm::vec3& position, float headingDegrees) {
    position_ = position;
    heading_ = headingDegrees;
    // Depart au regime de croisiere : pas de phase d'acceleration a simuler,
    // l'aile est deja gonflee et volante au moment du declencheur (voir la
    // remarque de main.cpp sur l'absence de course d'elan pour cette session).
    speed_ = params_.cruiseSpeed;
    sinkRate_ = params_.cruiseSinkRate;
    climbRate_ = -params_.cruiseSinkRate;
    flightTime_ = 0.0f;
    flying_ = true;
}

void Paraglider::update(float deltaTime, float turnInput, float brakeInput,
                        const AirState& air) {
    if (!flying_) {
        return;
    }

    turnInput = std::clamp(turnInput, -1.0f, 1.0f);
    brakeInput = std::clamp(brakeInput, 0.0f, 1.0f);

    const float turbulence = std::clamp(std::isfinite(air.turbulence) ? air.turbulence : 0.0f,
                                        0.0f, 1.0f);
    flightTime_ += deltaTime;

    heading_ += turnInput * params_.turnRateDegPerSec * deltaTime;

    // --- PHASE 23 : chahut turbulent ---------------------------------------
    // Deux sinusoides de periodes INCOMMENSURABLES (le rapport est irrationnel)
    // plutot qu'un tirage aleatoire, et c'est un choix, pas un raccourci :
    //  - le vol reste DETERMINISTE, donc rejouable et testable, ce qui est la
    //    regle de toute la Phase 23 ;
    //  - deux periodes sans rapport simple ne se resynchronisent jamais, donc le
    //    chahut ne degenere pas en balancement regulier, qui est ce qui trahit
    //    immediatement une turbulence fabriquee.
    // Le modele ne represente NI fermeture NI decrochage : une aile chahutee ici
    // perd du cap et de la finesse, elle ne tombe pas.
    if (turbulence > 0.0f) {
        const float period = std::max(0.1f, params_.turbulencePeriod);
        const float yawPhase = 2.0f * 3.14159265358979323846f * flightTime_ / period;
        const float yaw = std::sin(yawPhase) + 0.5f * std::sin(yawPhase * 2.618f);
        heading_ += yaw * turbulence * params_.turbulenceYawDegPerSec * deltaTime;
    }

    // Reponse EXPONENTIELLE au frein plutot qu'un saut instantane : une aile
    // met une fraction de seconde a se stabiliser sur son nouveau regime de
    // vitesse/taux de chute. La constante de temps responseTime fixe cette
    // fraction de seconde (1 - e^-1 ~ 63 % du chemin parcouru apres ce delai).
    const float targetSpeed = glm::mix(params_.cruiseSpeed, params_.brakeSpeed, brakeInput);
    const float targetSink = glm::mix(params_.cruiseSinkRate, params_.brakeSinkRate, brakeInput);
    const float responseRate = 1.0f - std::exp(-deltaTime / params_.responseTime);
    speed_ += (targetSpeed - speed_) * responseRate;
    sinkRate_ += (targetSink - sinkRate_) * responseRate;

    // --- PHASE 23 : le vol se fait DANS une masse d'air, pas dans le vide ---
    //
    // speed_ est une vitesse AIR : c'est ce que l'aile fait par rapport a l'air
    // qui l'entoure. La vitesse SOL est cette vitesse air plus le vent. C'est
    // l'ajout le plus important de cette phase pour le vol, et le plus lisible
    // en jeu : par vent d'ouest, un vol vers l'est va bien plus loin qu'un vol
    // vers l'ouest, sans qu'aucune regle particuliere n'ait ete ecrite.
    glm::vec3 wind(0.0f);
    if (std::isfinite(air.wind.x) && std::isfinite(air.wind.z)) {
        wind = glm::vec3(air.wind.x, 0.0f, air.wind.z);
    }
    position_ += (forward() * speed_ + wind) * deltaTime;

    // Verticale : le taux de chute de l'aile DANS l'air, plus le mouvement de
    // l'air lui-meme. Dans une thermique plus forte que le taux de chute, la
    // somme est positive et l'aile MONTE - c'est la definition du vol thermique,
    // et elle sort d'elle-meme de cette addition.
    const float airVertical = std::isfinite(air.vertical) ? air.vertical : 0.0f;
    const float turbulentSink = turbulence * params_.turbulenceSinkPenalty;
    climbRate_ = airVertical - sinkRate_ - turbulentSink;
    position_.y += climbRate_ * deltaTime;
}

bool Paraglider::checkLanding(const HeightSampler& terrain, float eyeHeight) {
    if (!flying_) {
        return false;
    }

    const float groundEyeY = terrain.heightAt(position_.x, position_.z) + eyeHeight;
    if (position_.y > groundEyeY) {
        return false;
    }

    // Collee au sol exactement, comme Camera::snapToTerrain : sans ca, chaque
    // atterrissage laisserait un residu (le pas de descente du dernier
    // deltaTime) sous le relief.
    position_.y = groundEyeY;
    flying_ = false;
    return true;
}

} // namespace vercora
