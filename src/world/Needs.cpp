#include "world/Needs.hpp"

#include "world/Weather.hpp"

#include <algorithm>
#include <cmath>

namespace vercora {

namespace {

float clamp01(float value) {
    // NaN : std::clamp propagerait la valeur telle quelle et une jauge NaN
    // contaminerait ensuite tous les coefficients, donc l'affichage ET la
    // vitesse du joueur. On la ramene a 0, ce qui est visible en jeu plutot
    // que silencieux.
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

// Gravite d'un manque : 0 tant que la jauge est au-dessus du seuil critique,
// 1 quand elle est vide. Une seule ecriture pour la faim, la soif et le froid -
// c'est ce qui garantit que les trois penalites ont exactement la meme forme.
float deficitOf(float gauge, float threshold) {
    if (threshold <= 0.0f) {
        return gauge <= 0.0f ? 1.0f : 0.0f;
    }
    return std::clamp((threshold - gauge) / threshold, 0.0f, 1.0f);
}

// Penalite qui vaut 1 au-dessus du seuil et descend lineairement jusqu'a
// floorFactor quand la jauge est vide.
float factorFrom(float gauge, float threshold, float floorFactor) {
    return 1.0f - (1.0f - floorFactor) * deficitOf(gauge, threshold);
}

} // namespace

// ---------------------------------------------------------------------------
// Activites
// ---------------------------------------------------------------------------

const char* activityLabel(Activity activity) {
    switch (activity) {
        case Activity::Rest:      return "repos";
        case Activity::Walk:      return "marche";
        case Activity::Bike:      return "velo";
        case Activity::Climb:     return "escalade";
        case Activity::Paraglide: return "parapente";
    }
    return "inconnu";
}

float activityEffort(Activity activity) {
    switch (activity) {
        case Activity::Rest:      return 0.0f;
        case Activity::Walk:      return 1.0f;
        case Activity::Bike:      return 1.8f;
        case Activity::Climb:     return 4.0f;
        case Activity::Paraglide: return 0.3f;
    }
    return 0.0f;
}

// ---------------------------------------------------------------------------
// Horloge
// ---------------------------------------------------------------------------

GameClock::GameClock(float startHour, float scale)
    : hours_(0.0f), scale_(scale > 0.0f ? scale : 1.0f) {
    setHours(startHour);
}

void GameClock::setHours(float hours) {
    if (!std::isfinite(hours)) {
        hours_ = 0.0f;
        totalHours_ = 0.0f;
        return;
    }
    // std::fmod garde le signe du dividende : un reglage negatif donnerait une
    // heure negative, que clockText afficherait comme "-3:00".
    hours_ = std::fmod(hours, 24.0f);
    if (hours_ < 0.0f) {
        hours_ += 24.0f;
    }

    // Le compteur total est RECALE sur le jour courant, il n'est pas remis a
    // zero : regler l'heure a la main deplace dans la journee, pas dans le
    // calendrier meteo. Sans ca, une commande de debogage "il est 14 h" ferait
    // sauter la meteo de plusieurs jours a chaque appel.
    const float dayStart = std::floor(totalHours_ / 24.0f) * 24.0f;
    totalHours_ = dayStart + hours_;
}

void GameClock::advance(float realSeconds) {
    if (!(realSeconds > 0.0f)) {
        return;
    }
    const float gameHours = realSeconds * scale_ / 3600.0f;

    // Le TOTAL avance d'abord et sans rebouclage : c'est lui qui porte les
    // journees, donc le calendrier des perturbations meteo. hours_ n'en est
    // qu'une lecture modulo 24.
    totalHours_ += gameHours;

    hours_ = std::fmod(totalHours_, 24.0f);
    if (hours_ < 0.0f) {
        hours_ += 24.0f;
    }
}

std::string GameClock::clockText() const {
    const int hour = std::clamp(static_cast<int>(hours_), 0, 23);
    int minute = static_cast<int>((hours_ - static_cast<float>(hour)) * 60.0f);
    minute = std::clamp(minute, 0, 59);

    std::string text;
    if (hour < 10) { text += '0'; }
    text += std::to_string(hour);
    text += ':';
    if (minute < 10) { text += '0'; }
    text += std::to_string(minute);
    return text;
}

// ---------------------------------------------------------------------------
// PHASE 23 : le bloc "Climat" qui occupait cette place - daylightFactor(),
// ambientTemperature() et terrainExposure() - a ete DEPLACE dans
// world/Weather.cpp. Needs ne calcule plus aucune temperature ambiante ; il en
// RECOIT une (NeedsContext::ambientTemperatureC) et n'y ajoute que ce qui
// concerne le corps du joueur : son effort, sa fatigue, son abri, le vent qu'il
// subit. C'est la separation qui garantit qu'il n'existe qu'une seule source de
// verite sur la temperature.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Needs
// ---------------------------------------------------------------------------

Needs::Needs(const NeedsParams& params) : params_(params) {
    // La temperature ressentie de depart n'a de sens qu'apres un update ; on
    // l'initialise au confort pour qu'un affichage avant la premiere image
    // n'annonce pas 0 degre.
    lastAmbientC_ = params_.comfortTemperatureC;
    lastFeltC_ = params_.comfortTemperatureC;
}

void Needs::update(float deltaTime, const NeedsContext& context) {
    if (!(deltaTime > 0.0f) || !std::isfinite(deltaTime)) {
        return;
    }

    const float effort = activityEffort(context.activity);

    // Les quatre jauges avancent depuis le MEME instantane : le manque qui
    // aggrave la fatigue est celui du debut du pas, pas celui qu'on est en
    // train de calculer. Sans ca, l'ordre des lignes ci-dessous changerait le
    // resultat, ce qui rendrait tout test dependant d'un detail d'ecriture.
    const float deficit = worstDeficit();
    const float deficitScale = 1.0f + params_.deficitFatigueGain * deficit;

    // --- Temperature --------------------------------------------------------
    // Calculee en premier parce que la soif en depend (chaleur), mais elle
    // n'utilise que la fatigue du debut du pas.
    // PHASE 23 : plus aucun calcul d'ambiante ici. Elle arrive du systeme meteo
    // (world/Weather.hpp) par le contexte. Une valeur non finie serait la seule
    // facon de contaminer les quatre jauges d'un coup, donc on la neutralise sur
    // le confort plutot que de la propager.
    lastAmbientC_ = std::isfinite(context.ambientTemperatureC)
                        ? context.ambientTemperatureC
                        : params_.comfortTemperatureC;

    // --- Refroidissement eolien --------------------------------------------
    // Applique AVANT les termes du corps, et c'est l'ordre correct : le vent
    // refroidit l'AIR ressenti, il ne retranche pas la chaleur produite par
    // l'effort. Un abri l'annule entierement - c'est ce qu'est un abri.
    //
    // La formule (JAG/TI, voir windChillC) ne mord qu'en dessous de 10 degres et
    // au-dessus de 4,8 km/h de vent : par une apres-midi douce et calme, ce
    // terme vaut exactement zero, et le comportement des jauges est celui de la
    // Phase 16.
    float felt = context.sheltered
                     ? lastAmbientC_
                     : windChillC(lastAmbientC_, context.windSpeedMps);

    felt += params_.exertionWarmthCPerEffort * effort;
    felt += params_.exhaustionChillC * (1.0f - fatigue_);
    if (context.sheltered) {
        felt += params_.shelterWarmthC;
    }
    lastFeltC_ = felt;

    const float margin = felt - params_.comfortTemperatureC;
    if (margin < 0.0f) {
        temperature_ = clamp01(temperature_ +
                               params_.coolingPerSecondPerDegree * margin * deltaTime);
    } else {
        const float ramp = params_.fullWarmingMarginC > 0.0f
            ? std::min(1.0f, margin / params_.fullWarmingMarginC)
            : 1.0f;
        temperature_ = clamp01(temperature_ +
                               params_.warmingPerSecond * ramp * deltaTime);
    }

    // --- Faim ---------------------------------------------------------------
    const float hungerRate =
        params_.hungerPerSecondAtRest * (1.0f + params_.hungerEffortGain * effort);
    hunger_ = clamp01(hunger_ - hungerRate * deltaTime);

    // --- Soif ---------------------------------------------------------------
    float thirstRate =
        params_.thirstPerSecondAtRest * (1.0f + params_.thirstEffortGain * effort);
    const float heat = felt - params_.heatThirstThresholdC;
    if (heat > 0.0f) {
        thirstRate += params_.thirstPerSecondPerDegreeHeat * heat;
    }
    thirst_ = clamp01(thirst_ - thirstRate * deltaTime);

    // --- Fatigue ------------------------------------------------------------
    // Le repos est le SEUL etat qui remonte la jauge, et il exige l'immobilite
    // complete : c'est ce qui fait d'une pause une decision de jeu.
    if (effort <= 0.0f) {
        fatigue_ = clamp01(fatigue_ +
                           params_.fatigueRecoveryPerSecond / deficitScale * deltaTime);
    } else {
        fatigue_ = clamp01(fatigue_ - params_.fatiguePerSecondWalking * effort *
                                          deficitScale * deltaTime);
    }
}

float Needs::fatigueFactor() const {
    const float threshold = params_.fatiguePenaltyThreshold;
    if (threshold <= 0.0f) {
        return 1.0f;
    }
    if (fatigue_ >= threshold) {
        return 1.0f;
    }
    return params_.fatigueFloorFactor +
           (1.0f - params_.fatigueFloorFactor) * (fatigue_ / threshold);
}

float Needs::hungerFactor() const {
    return factorFrom(hunger_, params_.criticalThreshold, params_.hungerFloorFactor);
}

float Needs::thirstFactor() const {
    return factorFrom(thirst_, params_.criticalThreshold, params_.thirstFloorFactor);
}

float Needs::coldFactor() const {
    return factorFrom(temperature_, params_.criticalThreshold, params_.coldFloorFactor);
}

float Needs::movementFactor() const {
    const float product = fatigueFactor() * hungerFactor() * thirstFactor() * coldFactor();
    return std::clamp(product, params_.minimumMovementFactor, 1.0f);
}

float Needs::worstDeficit() const {
    const float threshold = params_.criticalThreshold;
    return std::max({deficitOf(hunger_, threshold),
                     deficitOf(thirst_, threshold),
                     deficitOf(temperature_, threshold)});
}

bool Needs::drink() {
    if (thirst_ >= 1.0f) {
        return false;
    }
    thirst_ = 1.0f;
    return true;
}

bool Needs::eat(float nutrition) {
    if (!(nutrition > 0.0f) || hunger_ >= 1.0f) {
        return false;
    }
    hunger_ = clamp01(hunger_ + std::min(nutrition, 1.0f));
    return true;
}

void Needs::reset() {
    hunger_ = 1.0f;
    thirst_ = 1.0f;
    fatigue_ = 1.0f;
    temperature_ = 1.0f;
    lastAmbientC_ = params_.comfortTemperatureC;
    lastFeltC_ = params_.comfortTemperatureC;
}

void Needs::setHunger(float value)      { hunger_ = clamp01(value); }
void Needs::setThirst(float value)      { thirst_ = clamp01(value); }
void Needs::setFatigue(float value)     { fatigue_ = clamp01(value); }
void Needs::setTemperature(float value) { temperature_ = clamp01(value); }

namespace {

// Pourcentage entier d'une jauge. Arrondi et non tronque : une jauge a 0,999
// doit afficher 100 % et non 99 %.
int percent(float gauge) {
    return static_cast<int>(std::lround(100.0f * std::clamp(gauge, 0.0f, 1.0f)));
}

// Un entier signe de degres, arrondi. Le titre n'a pas la place d'une decimale
// et le journal a statusLine pour le detail.
int degrees(float celsius) {
    if (!std::isfinite(celsius)) {
        return 0;
    }
    return static_cast<int>(std::lround(celsius));
}

} // namespace

std::string Needs::summary() const {
    // "forme" et non "fatigue" : la jauge vaut 1 quand tout va bien, comme les
    // trois autres. L'appeler "fatigue 100 %" se lirait a l'envers.
    return "faim " + std::to_string(percent(hunger_)) + "% soif " +
           std::to_string(percent(thirst_)) + "% forme " +
           std::to_string(percent(fatigue_)) + "% chaud " +
           std::to_string(percent(temperature_)) + "%";
}

std::string Needs::statusLine() const {
    return "Besoins : " + summary() + ", ambiante " +
           std::to_string(degrees(lastAmbientC_)) + " C, ressenti " +
           std::to_string(degrees(lastFeltC_)) + " C, deplacement " +
           std::to_string(percent(movementFactor())) + " %.";
}

} // namespace vercora
