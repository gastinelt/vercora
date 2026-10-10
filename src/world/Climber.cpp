#include "world/Climber.hpp"

#include "core/CollisionSolver.hpp"
#include "core/HeightSampler.hpp"

#include <algorithm>
#include <cmath>

namespace vercora {

namespace {

// Nombre de sondes reparties sur params.reach pour trouver la paroi. Sept sondes
// sur 2 m = une tous les 33 cm, largement plus fin que la base de 4 m sur
// laquelle la pente est lue : sonder plus fin ne revelerait rien de nouveau.
constexpr int kReachProbes = 7;

// Distance a laquelle on verifie que le terrain reste marchable pour declarer un
// sommet, en m. Voir Climber::hasToppedOut.
constexpr float kTopOutConfirmDistance = 3.0f;

// En dessous de cette longueur, un pas est considere nul : on ne normalise pas
// un vecteur de longueur nulle et on ne facture pas d'energie pour du bruit de
// virgule flottante.
constexpr float kMinStep = 1e-5f;

} // namespace

Climber::Climber(const ClimberParams& params) : params_(params) {}

glm::vec2 Climber::gradientAt(const HeightSampler& terrain, float worldX,
                              float worldZ) const {
    const float s = params_.slopeSampleStep;
    // Differences CENTREES, comme Bike::terrainGradient : symetriques, donc la
    // meme paroi a la meme pente qu'on l'aborde par la gauche ou par la droite.
    const float hxPlus = terrain.heightAt(worldX + s, worldZ);
    const float hxMinus = terrain.heightAt(worldX - s, worldZ);
    const float hzPlus = terrain.heightAt(worldX, worldZ + s);
    const float hzMinus = terrain.heightAt(worldX, worldZ - s);
    return glm::vec2((hxPlus - hxMinus) / (2.0f * s), (hzPlus - hzMinus) / (2.0f * s));
}

float Climber::slopeAt(const HeightSampler& terrain, float worldX, float worldZ) const {
    const glm::vec2 g = gradientAt(terrain, worldX, worldZ);
    return std::sqrt(g.x * g.x + g.y * g.y);
}

glm::vec3 Climber::wallFacing(const HeightSampler& terrain) const {
    const glm::vec2 g = gradientAt(terrain, anchor_.x, anchor_.z);
    const float length = std::sqrt(g.x * g.x + g.y * g.y);
    if (length < kMinStep) {
        // Surface strictement plate : la ligne de plus grande pente n'existe
        // pas. Ne peut pas arriver en paroi (on n'y entre qu'au-dessus de
        // wallSlope), mais la fonction est publique donc elle doit repondre.
        return glm::vec3(0.0f);
    }
    // Le gradient pointe vers la montee : c'est la direction dans laquelle le
    // grimpeur fait face.
    return glm::vec3(g.x / length, 0.0f, g.y / length);
}

void Climber::snapAnchor(const HeightSampler& terrain) {
    anchor_.y = terrain.heightAt(anchor_.x, anchor_.z);
}

glm::vec3 Climber::position() const {
    // L'oeil est decale vers l'AVAL (outward_) et un peu vers le haut. Le
    // decalage aval garantit que l'oeil est hors du relief : la surface y est
    // plus basse qu'a l'accroche, puisqu'on descend la ligne de plus grande
    // pente.
    return anchor_ + outward_ * params_.wallOffset +
           glm::vec3(0.0f, params_.eyeRise, 0.0f);
}

float Climber::wallRise(const HeightSampler& terrain, float worldX,
                        float worldZ) const {
    // On suit la ligne de plus grande pente exactement comme le fait update(),
    // par petits pas, en reevaluant le gradient a chaque fois : une paroi reelle
    // tourne, et un pas unique en ligne droite sortirait de la face.
    constexpr int kSteps = 12;
    const float stepLength = params_.minWallRun / static_cast<float>(kSteps);

    float x = worldX;
    float z = worldZ;
    const float startY = terrain.heightAt(x, z);

    for (int i = 0; i < kSteps; ++i) {
        const glm::vec2 g = gradientAt(terrain, x, z);
        const float slope = std::sqrt(g.x * g.x + g.y * g.y);
        if (slope < kMinStep) {
            break; // plat : plus rien a gagner, on s'arrete la
        }
        // Meme projection que le deplacement reel : avancer stepLength LE LONG DE
        // LA SURFACE ne consomme que stepLength / sqrt(1 + pente^2) a
        // l'horizontale.
        const float horizontal = stepLength / std::sqrt(1.0f + slope * slope);
        x += (g.x / slope) * horizontal;
        z += (g.y / slope) * horizontal;
    }

    return terrain.heightAt(x, z) - startY;
}

bool Climber::tryStart(const HeightSampler& terrain, const glm::vec3& eyePosition,
                       const glm::vec3& lookDirection) {
    if (climbing_ || stamina_ < minStaminaToStart()) {
        return false;
    }

    // Seule la projection horizontale du regard compte : lever les yeux vers le
    // haut de la falaise ne doit pas raccourcir la portee au sol.
    glm::vec3 flat(lookDirection.x, 0.0f, lookDirection.z);
    const float flatLength = std::sqrt(flat.x * flat.x + flat.z * flat.z);
    if (flatLength < kMinStep) {
        return false; // regard strictement vertical : aucune direction au sol
    }
    flat /= flatLength;

    for (int i = 0; i <= kReachProbes; ++i) {
        const float distance =
            params_.reach * static_cast<float>(i) / static_cast<float>(kReachProbes);
        const float px = eyePosition.x + flat.x * distance;
        const float pz = eyePosition.z + flat.z * distance;
        if (!isWall(terrain, px, pz)) {
            continue;
        }

        // Raide ne suffit pas : il faut que grimper la MENE quelque part. Sans
        // ce controle, on s'accroche au premier ressaut de deux metres devant la
        // paroi et l'escalade se termine par un "sommet" trois secondes plus
        // tard - constate sur la paroi de reference LiDAR, voir minWallRun.
        if (wallRise(terrain, px, pz) < params_.minWallRise) {
            continue;
        }

        // Le point de repli d'une chute est la position du joueur AU MOMENT DE
        // L'ENGAGEMENT, et non le pied de la paroi calcule apres coup : c'est
        // par construction du terrain ou il se tenait debout la frame d'avant,
        // donc du terrain ou l'on peut le reposer sans risque. Le calculer
        // autrement, ce serait chercher une position libre sans garantie d'en
        // trouver une.
        //
        // Stocke SUR LA SURFACE et non a hauteur d'oeil : groundAnchor_ et
        // anchor_ doivent etre dans le meme repere, sans quoi heightGained()
        // annoncerait 1,7 m de moins que la realite. L'appelant qui repose le
        // joueur y ajoutera sa hauteur d'oeil (Camera::setMode le fait tout
        // seul via snapToTerrain).
        groundAnchor_ = glm::vec3(eyePosition.x,
                                  terrain.heightAt(eyePosition.x, eyePosition.z),
                                  eyePosition.z);

        anchor_ = glm::vec3(px, 0.0f, pz);
        snapAnchor(terrain);
        outward_ = -wallFacing(terrain);
        climbing_ = true;
        return true;
    }

    return false;
}

bool Climber::hasToppedOut(const HeightSampler& terrain) const {
    if (slopeAt(terrain, anchor_.x, anchor_.z) >= params_.walkableSlope) {
        return false;
    }
    // Confirmation 3 m plus loin dans la pente. Sans elle, la moindre dalle
    // horizontale au milieu de la face terminerait l'escalade en plein vide.
    const glm::vec3 facing = wallFacing(terrain);
    if (facing == glm::vec3(0.0f)) {
        // Plat parfait dans toutes les directions : c'est un sommet, et il n'y a
        // pas de direction dans laquelle confirmer.
        return true;
    }
    const float cx = anchor_.x + facing.x * kTopOutConfirmDistance;
    const float cz = anchor_.z + facing.z * kTopOutConfirmDistance;
    return slopeAt(terrain, cx, cz) < params_.walkableSlope;
}

ClimbExit Climber::release() {
    if (!climbing_) {
        return ClimbExit::None;
    }
    climbing_ = false;
    // Retour au pied. La position de repli est celle de l'engagement, dont on
    // sait qu'elle etait libre et marchable.
    anchor_ = groundAnchor_;
    return ClimbExit::Released;
}

void Climber::restOnGround(float deltaTime) {
    if (climbing_ || deltaTime <= 0.0f) {
        return;
    }
    stamina_ = std::min(1.0f, stamina_ + params_.staminaRecoveryPerSecond * deltaTime);
}

float Climber::gripForWetness(float wetness) const {
    const float wet = std::isfinite(wetness) ? std::clamp(wetness, 0.0f, 1.0f) : 0.0f;
    const float wetGrip = std::clamp(params_.wetGrip, 0.05f, 1.0f);
    return 1.0f + (wetGrip - 1.0f) * wet;
}

ClimbExit Climber::update(float deltaTime, float climbInput, float traverseInput,
                          const HeightSampler& terrain,
                          const CollisionSolver* obstacles, float grip) {
    if (!climbing_ || deltaTime <= 0.0f) {
        return ClimbExit::None;
    }

    climbInput = std::clamp(climbInput, -1.0f, 1.0f);
    traverseInput = std::clamp(traverseInput, -1.0f, 1.0f);

    // PHASE 23 : adherence. Le plancher a 0,05 n'est pas cosmetique - il garantit
    // qu'une paroi ruisselante reste GRIMPABLE, tres lentement et tres chere,
    // plutot que de devenir une impasse ou le joueur resterait bloque en paroi
    // sans pouvoir ni monter ni redescendre.
    grip = std::isfinite(grip) ? std::clamp(grip, 0.05f, 1.0f) : 1.0f;

    // Le surcout suit l'INVERSE de l'adherence : a grip 1 il vaut 1, et il monte
    // jusqu'a wetStaminaGain quand l'adherence tombe a sa valeur mouillee. Le
    // rapporter a params_.wetGrip plutot qu'a zero est ce qui garde les deux
    // constantes d'accord entre elles - regler l'une sans l'autre ne peut pas
    // produire un surcout absurde.
    const float wetGrip = std::clamp(params_.wetGrip, 0.05f, 1.0f);
    const float slipperiness = wetGrip < 1.0f
                                   ? std::clamp((1.0f - grip) / (1.0f - wetGrip), 0.0f, 1.0f)
                                   : 0.0f;
    const float staminaScale = 1.0f + (params_.wetStaminaGain - 1.0f) * slipperiness;

    const glm::vec2 gradient = gradientAt(terrain, anchor_.x, anchor_.z);
    const float slope = std::sqrt(gradient.x * gradient.x + gradient.y * gradient.y);

    glm::vec3 facing(0.0f);
    if (slope >= kMinStep) {
        facing = glm::vec3(gradient.x / slope, 0.0f, gradient.y / slope);
    }
    outward_ = -facing;

    // -----------------------------------------------------------------------
    // 1. Deplacement LE LONG DE LA SURFACE.
    // -----------------------------------------------------------------------
    // Monter/descendre suit la ligne de plus grande pente. Avancer d'une
    // longueur L le long de la surface consomme L / sqrt(1 + pente^2) de
    // deplacement HORIZONTAL - c'est cette projection qui fait qu'une paroi
    // quasi verticale se monte quasi verticalement, sans aucun cas particulier.
    glm::vec3 horizontalStep(0.0f);
    if (facing != glm::vec3(0.0f) && climbInput != 0.0f) {
        const float alongSurface = params_.climbSpeed * grip * climbInput * deltaTime;
        const float horizontal = alongSurface / std::sqrt(1.0f + slope * slope);
        horizontalStep += facing * horizontal;
    }

    // Traverser suit la LIGNE DE NIVEAU : perpendiculaire au gradient dans le
    // plan XZ. Meme construction que Camera::right_ / Bike::right()
    // (cross(front, worldUp)), donc +1 est bien la droite du grimpeur face a la
    // paroi. Une ligne de niveau etant horizontale par definition, il n'y a pas
    // de projection a faire ici.
    if (facing != glm::vec3(0.0f) && traverseInput != 0.0f) {
        const glm::vec3 right(-facing.z, 0.0f, facing.x);
        horizontalStep += right * (params_.traverseSpeed * grip * traverseInput * deltaTime);
    }

    const glm::vec3 previousAnchor = anchor_;
    anchor_.x += horizontalStep.x;
    anchor_.z += horizontalStep.z;

    // -----------------------------------------------------------------------
    // 2. Obstacles, par le point d'entree UNIQUE du projet.
    // -----------------------------------------------------------------------
    if (obstacles != nullptr) {
        BodyCylinder body;
        body.position = anchor_;
        body.radius = params_.bodyRadius;
        // Bande CENTREE sur l'accroche, et non partant des pieds comme a la
        // marche : en paroi le corps est etendu le long de la surface, le point
        // d'accroche est a peu pres a mi-corps.
        body.feetY = anchor_.y - params_.bodyHalfHeight;
        body.headY = anchor_.y + params_.bodyHalfHeight;

        const glm::vec3 corrected = moveBody(*obstacles, body, previousAnchor);
        anchor_.x = corrected.x;
        anchor_.z = corrected.z;
    }

    // Contact PERMANENT avec la surface, meme invariant que le velo : la
    // verticale n'est pas integree, elle est relue.
    snapAnchor(terrain);

    // -----------------------------------------------------------------------
    // 3. Energie. Facturee sur le deplacement REELLEMENT obtenu (donc apres les
    //    obstacles) : buter contre un tronc ne doit pas couter comme si on avait
    //    grimpe.
    // -----------------------------------------------------------------------
    const float dx = anchor_.x - previousAnchor.x;
    const float dz = anchor_.z - previousAnchor.z;
    const float dHorizontal = std::sqrt(dx * dx + dz * dz);
    const float dAltitude = anchor_.y - previousAnchor.y;

    if (dHorizontal < kMinStep && std::abs(dAltitude) < kMinStep) {
        // Suspendu sans bouger : le seul cout au temps du modele.
        // Rester suspendu a une paroi mouillee coute plus cher : on serre.
        stamina_ -= params_.staminaHangPerSecond * staminaScale * deltaTime;
    } else {
        stamina_ -= dHorizontal * params_.staminaPerMeterLateral * staminaScale;
        stamina_ -= (dAltitude > 0.0f
                         ? dAltitude * params_.staminaPerMeterUp
                         : -dAltitude * params_.staminaPerMeterDown) *
                    staminaScale;
    }
    stamina_ = std::clamp(stamina_, 0.0f, 1.0f);

    // -----------------------------------------------------------------------
    // 4. Conditions de sortie. L'ORDRE COMPTE : le sommet est teste AVANT
    //    l'epuisement. Un grimpeur qui pose la main sur la vire avec sa derniere
    //    goutte d'energie a topppe, il n'est pas tombe - et c'est la seule
    //    lecture qui ne frustre pas.
    // -----------------------------------------------------------------------
    if (hasToppedOut(terrain)) {
        climbing_ = false;
        return ClimbExit::ToppedOut;
    }

    if (stamina_ <= 0.0f) {
        climbing_ = false;
        // Chute : reinitialisation PROPRE de la position, pas de degats (hors
        // scope de cette session). On revient au point d'engagement, dont on
        // sait qu'il etait marchable et libre.
        anchor_ = groundAnchor_;
        return ClimbExit::Exhausted;
    }

    return ClimbExit::None;
}

} // namespace vercora
