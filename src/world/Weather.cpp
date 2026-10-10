#include "world/Weather.hpp"

#include "core/HeightSampler.hpp"

#include <algorithm>
#include <cmath>

namespace vercora {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float clamp01(float value) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

float safeFinite(float value, float fallback = 0.0f) {
    return std::isfinite(value) ? value : fallback;
}

// ---------------------------------------------------------------------------
// Bruit deterministe 1D
// ---------------------------------------------------------------------------
// Bruit de GRADIENT (la famille de Perlin/Simplex), et non de valeur comme
// Heightmap et Vegetation. La raison est propre a ce module : un bruit de valeur
// atteint ses extremes SUR les noeuds entiers, donc a intervalles reguliers -
// une perturbation tomberait pile toutes les 3,5 journees de jeu, et le joueur
// le remarquerait en une semaine. Un bruit de gradient vaut zero sur les noeuds
// et place ses extremes entre eux, a des positions qui dependent des gradients
// tires : le calendrier meteo est irregulier, ce qui est le but.
//
// En 1D, Simplex et Perlin sont la MEME construction (le simplexe de dimension 1
// est le segment) : parler de "Simplex 1D" comme le fait le cahier des charges
// et implementer un bruit de gradient 1D est la meme chose, sans le detour du
// gradient tabule d'une vraie implementation Simplex 2D+.
uint32_t hashU32(uint32_t x) {
    // lowbias32 de Chris Wellons : melange complet, aucun motif visible sur des
    // entrees consecutives - ce qui est exactement le cas d'usage ici, ou les
    // noeuds sont 0, 1, 2, 3...
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Gradient d'un noeud, dans [-1, 1).
float nodeGradient(int32_t node, uint32_t seed) {
    const uint32_t h = hashU32(static_cast<uint32_t>(node) * 0x9E3779B9u ^ seed);
    return static_cast<float>(h) * (2.0f / 4294967296.0f) - 1.0f;
}

// Courbe de lissage de Perlin (6t^5 - 15t^4 + 10t^3) : ses derivees premiere ET
// seconde sont nulles aux noeuds, donc aucune cassure visible au raccord.
float smootherStep(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Bruit de gradient 1D dans [-1, 1] environ.
float gradientNoise1D(float x, uint32_t seed) {
    const float floored = std::floor(x);
    const int32_t i0 = static_cast<int32_t>(floored);
    const float f = x - floored;

    const float g0 = nodeGradient(i0, seed);
    const float g1 = nodeGradient(i0 + 1, seed);

    const float n0 = g0 * f;
    const float n1 = g1 * (f - 1.0f);

    // * 2 : un bruit de gradient 1D a gradients dans [-1, 1] plafonne a 0,5 en
    // valeur absolue. Le facteur le ramene a la plage [-1, 1] annoncee.
    return 2.0f * (n0 + (n1 - n0) * smootherStep(f));
}

// fBm : plusieurs octaves du bruit ci-dessus, normalise dans [-1, 1].
float fbm1D(float x, uint32_t seed, int octaves) {
    octaves = std::clamp(octaves, 1, 8);

    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float total = 0.0f;

    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * gradientNoise1D(x * frequency, seed + static_cast<uint32_t>(i) * 0x9E3779B9u);
        total += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }

    return total > 0.0f ? sum / total : 0.0f;
}

// ---------------------------------------------------------------------------
// Astronomie minimale
// ---------------------------------------------------------------------------
// Modele de position solaire "de poche" : declinaison de Cooper, angle horaire
// lineaire, pas d'equation du temps, pas de refraction, pas de longitude.
//
// C'est SUFFISANT et c'est un choix, pas un oubli : ce dont le jeu a besoin,
// c'est que le soleil se leve a l'est, culmine au sud, se couche a l'ouest, et
// que sa hauteur suive la saison. Les corrections omises valent au plus une
// quinzaine de minutes sur les heures de lever et coucher - invisible dans une
// horloge acceleree douze fois.
float solarDeclinationDegrees(float dayOfYear) {
    // Cooper (1969) : delta = 23,44 * sin(360 * (284 + n) / 365).
    return 23.44f * std::sin(2.0f * kPi * (284.0f + dayOfYear) / 365.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// Geometrie du terrain
// ---------------------------------------------------------------------------

glm::vec3 compassDirection(float degrees) {
    // Convention du projet, verifiee dans Heightmap::toProjected :
    // +X = EST, +Z = SUD. Le nord est donc -Z.
    const float rad = glm::radians(degrees);
    return glm::vec3(std::sin(rad), 0.0f, -std::cos(rad));
}

float terrainExposure(const HeightSampler& terrain, float worldX, float worldZ,
                      float sampleStep) {
    const float s = sampleStep > 0.0f ? sampleStep : 2.0f;

    const float dhdx = (terrain.heightAt(worldX + s, worldZ) -
                        terrain.heightAt(worldX - s, worldZ)) / (2.0f * s);
    const float dhdz = (terrain.heightAt(worldX, worldZ + s) -
                        terrain.heightAt(worldX, worldZ - s)) / (2.0f * s);

    const float slope = std::sqrt(dhdx * dhdx + dhdz * dhdz);
    if (!(slope > 1e-6f)) {
        return 0.5f; // du plat n'a ni adret ni ubac
    }

    // +Z va vers le SUD. Un versant qui DESCEND vers le sud (dh/dz < 0) est un
    // adret : d'ou le signe moins.
    const float aspect = -dhdz / slope;                       // [-1, 1]
    const float weight = std::min(1.0f, slope / kFullExposureSlope);
    return std::clamp(0.5f + 0.5f * aspect * weight, 0.0f, 1.0f);
}

glm::vec3 terrainNormal(const HeightSampler& terrain, float worldX, float worldZ,
                        float sampleStep) {
    const float s = sampleStep > 0.0f ? sampleStep : 2.0f;

    const float dhdx = (terrain.heightAt(worldX + s, worldZ) -
                        terrain.heightAt(worldX - s, worldZ)) / (2.0f * s);
    const float dhdz = (terrain.heightAt(worldX, worldZ + s) -
                        terrain.heightAt(worldX, worldZ - s)) / (2.0f * s);

    // Surface y = h(x, z) : la normale non normalisee vaut (-dh/dx, 1, -dh/dz).
    return glm::normalize(glm::vec3(-dhdx, 1.0f, -dhdz));
}

// ---------------------------------------------------------------------------
// Lumiere du jour
// ---------------------------------------------------------------------------

SunLight sunLightFor(const SynopticState& sky, const WeatherParams& params) {
    SunLight light;

    // Intensite directe : elle monte avec le soleil et s'eteint au coucher. Le
    // seuil lightFullDaylightSin donne l'aube et le crepuscule - sans lui, le
    // jour s'allumerait d'un coup a l'instant ou l'elevation passe zero.
    const float rise = clamp01(sky.daylight / std::max(1e-3f, params.lightFullDaylightSin));
    light.intensity = rise;

    // Direction : celle du soleil tant qu'il est leve, la verticale une fois
    // couche. Le basculement se fait sur le meme facteur que l'intensite, donc
    // il est invisible : quand la direction commence a remonter, la lumiere
    // directe est deja presque eteinte.
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 blended = glm::mix(up, sky.sunDirection, rise);
    light.direction = glm::length(blended) > 1e-4f ? glm::normalize(blended) : up;

    // Couleur : orangee quand le soleil rase, franche quand il est haut, bleutee
    // une fois couche.
    const glm::vec3 dayTint = glm::mix(params.horizonColor, params.daylightColor, rise);
    light.color = rise > 0.0f ? dayTint : params.nightColor;

    // Ambiante : elle ne descend jamais a zero (voir nightAmbientFactor - c'est
    // un choix de jouabilite assume, le jeu n'a encore aucune source de lumiere
    // portable).
    //
    // PHASE 27 - CORRECTION D'UN COMMENTAIRE MENSONGER. Ce paragraphe affirmait
    // depuis la Phase 23 que l'ambiante etait "ecrasee par les nuages en plein
    // jour". Le code n'a JAMAIS lu la nebulosite. L'effet des nuages sur la
    // lumiere existe desormais, mais dans weatherVisualsFor(), qui part de cette
    // fonction ; celle-ci reste la lumiere d'un ciel CLAIR.
    const float night = std::clamp(params.nightAmbientFactor, 0.0f, 1.0f);
    light.ambient = night + (1.0f - night) * rise;

    // Ciel : nuit -> crepuscule -> plein jour, en deux segments. Le crepuscule
    // occupe la premiere moitie de la montee du soleil, ce qui donne une aube et
    // un coucher qui DURENT au lieu d'un fondu direct du noir au bleu.
    light.skyColor = rise < 0.5f
                         ? glm::mix(params.nightSkyColor, params.duskSkyColor, rise * 2.0f)
                         : glm::mix(params.duskSkyColor, params.daySkyColor,
                                    (rise - 0.5f) * 2.0f);

    return light;
}

// ---------------------------------------------------------------------------
// PHASE 27 - Meteo visible
// ---------------------------------------------------------------------------

WeatherVisuals weatherVisualsFor(const SynopticState& sky, const WeatherSample& local,
                                 const WeatherParams& params) {
    WeatherVisuals visuals;
    visuals.light = sunLightFor(sky, params);

    // La nebulosite LOCALE et non synoptique : c'est celle que voit le joueur,
    // renforcement orographique compris (un versant au vent s'accroche des
    // nuages que la vallee n'a pas - section 7 des parametres).
    const float cloud = clamp01(safeFinite(local.cloudCover));
    const float rise = visuals.light.intensity; // 0 la nuit, 1 en plein jour

    // --- Rayonnement direct voile ------------------------------------------
    // MEME loi et MEME chiffre que l'extinction de l'adret dans sampleAt() :
    // sous un ciel couvert, il ne reste que (1 - cloudAspectDamping) du
    // rayonnement direct. Une seule definition de "les nuages cachent le
    // soleil" - l'indice visuel adret/ubac (Phase 25) l'utilise deja aussi.
    const float directKept = std::max(0.0f, 1.0f - params.cloudAspectDamping * cloud);
    visuals.light.intensity *= directKept;

    // --- Ambiante diffuse ---------------------------------------------------
    // Renforcee de jour seulement (facteur rise) : c'est le soleil que les
    // nuages diffusent, il n'y a rien a diffuser la nuit.
    //
    // PAS d'ecretage a 1 : le multiplicateur d'ambiante DEPASSE 1 sous un ciel
    // couvert, c'est tout l'objet de ce terme. La premiere version l'ecretait a
    // [0, 1] (comme sunLightFor, qui n'en avait jamais eu besoin) - et comme
    // l'ambiante de plein jour vaut deja 1, le gain etait MORT : le journal des
    // captures affichait "ambiante x1.00" sous 99 % de nuages.
    visuals.light.ambient = std::max(
        0.0f, visuals.light.ambient * (1.0f + std::max(0.0f, params.overcastAmbientGain) *
                                                  cloud * rise));

    // --- Ciel grise ---------------------------------------------------------
    // Desature vers sa PROPRE luminance puis assombri : la luminosite du ciel
    // continue de suivre le cycle jour/nuit, seule sa couleur tombe au gris.
    const glm::vec3 clearSky = visuals.light.skyColor;
    const float luminance = glm::dot(clearSky, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    const glm::vec3 overcastSky = glm::vec3(luminance * params.overcastSkyDarkening);
    visuals.light.skyColor = glm::mix(clearSky, overcastSky, cloud);

    // --- Precipitation ------------------------------------------------------
    const float precip = std::max(0.0f, safeFinite(local.precipitationMmPerHour));
    visuals.snow = local.snowing;
    visuals.precipitation =
        clamp01(precip / std::max(1e-3f, params.precipitationMaxMmPerHour));

    // Vitesse de chute apparente : le vent LOCAL (celui que le joueur subit) et
    // une vitesse verticale de chute. Le vent incline donc la pluie dans le sens
    // ou il souffle reellement a cet endroit, sondage amont et col compris.
    const float fall = visuals.snow ? params.snowFallSpeedMps : params.rainFallSpeedMps;
    visuals.fallVelocity = glm::vec3(local.windVector.x, -fall, local.windVector.z);

    // --- Brouillard de distance ---------------------------------------------
    // Seule source : la precipitation. Loi de puissance sigma = a R^b (1/km),
    // multipliee pour la neige. AUCUN brouillard sans precipitation : le modele
    // ne connait ni l'humidite relative, ni le point de rosee, ni la base des
    // nuages, donc il n'a rien pour dire qu'il y a de la brume par temps sec.
    // Voir NOTES-PHASE27.md, "Prerequis manquants".
    if (precip > 0.0f) {
        float sigmaPerKm = params.rainExtinctionPerKm *
                           std::pow(precip, params.rainExtinctionExponent);
        if (visuals.snow) {
            sigmaPerKm *= params.snowExtinctionMultiplier;
        }
        visuals.fogDensityPerMeter = std::max(0.0f, sigmaPerKm) / 1000.0f;
    }
    // Le brouillard se fond dans le ciel : sans ca, le relief lointain
    // tournerait a une couleur que rien d'autre a l'ecran ne porte.
    visuals.fogColor = visuals.light.skyColor;

    // --- Sol mouille --------------------------------------------------------
    visuals.groundDarkening =
        clamp01(params.wetGroundDarkening * clamp01(safeFinite(local.wetness)));

    return visuals;
}

// ---------------------------------------------------------------------------
// Refroidissement eolien
// ---------------------------------------------------------------------------

float windChillC(float temperatureC, float windMps) {
    const float t = safeFinite(temperatureC);
    const float v = std::max(0.0f, safeFinite(windMps));

    const float kmh = v * 3.6f;

    // Domaine de validite publie de la formule JAG/TI. En dehors, on rend la
    // temperature inchangee : mieux vaut n'appliquer AUCUN effet qu'appliquer un
    // effet faux (a 25 degres la formule rendrait un vent rechauffant).
    if (t > 10.0f || kmh < 4.8f) {
        return t;
    }

    const float p = std::pow(kmh, 0.16f);
    const float chilled = 13.12f + 0.6215f * t - 11.37f * p + 0.3965f * t * p;

    // Garde-fou : le ressenti ne peut pas DEPASSER l'ambiante par vent fort et
    // froid. La formule le respecte dans son domaine, l'ecretage protege des
    // bords.
    return std::min(chilled, t);
}

// ---------------------------------------------------------------------------
// SynopticState / WeatherSample : descriptions
// ---------------------------------------------------------------------------
namespace {

std::string round1(float value) {
    // Une decimale, sans <iomanip> : le reste du projet ecrit ses journaux avec
    // std::to_string, on garde la meme dependance.
    const float rounded = std::round(value * 10.0f) / 10.0f;
    std::string text = std::to_string(rounded);
    const std::size_t dot = text.find('.');
    if (dot != std::string::npos && text.size() > dot + 2) {
        text.erase(dot + 2);
    }
    return text;
}

} // namespace

std::string SynopticState::describe() const {
    return "meteo j+" + std::to_string(static_cast<int>(gameHours / 24.0f)) +
           " " + round1(timeOfDayHours) + "h : " + round1(seaLevelTemperatureC) +
           " C (niveau mer), vent " + round1(seaLevelWindMps) + " m/s de " +
           std::to_string(static_cast<int>(std::round(windFromDegrees))) +
           " deg, nuages " + std::to_string(static_cast<int>(std::round(cloudCover * 100.0f))) +
           " %, pluie " + round1(precipitationMmPerHour) + " mm/h, soleil " +
           round1(sunElevationDegrees) + " deg";
}

std::string WeatherSample::describe() const {
    return "local : " + round1(temperatureC) + " C (gradient " + round1(lapseC) +
           ", versant " + round1(aspectC) + ", inversion " + round1(inversionC) +
           "), vent " + round1(windMps) + " m/s, vertical " + round1(verticalMps) +
           " m/s (pente " + round1(ridgeLiftMps) + ", thermique " + round1(thermalMps) +
           ", subsidence " + round1(subsidenceMps) + "), turbulence " +
           round1(turbulence) + ", mouillure " + round1(wetness);
}

// ---------------------------------------------------------------------------
// ColdAirTrapMap
// ---------------------------------------------------------------------------

constexpr float ColdAirTrapMap::kProbeRadii[3];

ColdAirTrapMap::ColdAirTrapMap(const HeightSampler& terrain, float halfSize,
                               uint32_t resolution)
    : resolution_(resolution), halfSize_(halfSize) {
    if (resolution < 2 || !(halfSize > 0.0f)) {
        // Construction impossible : on reste une carte VIDE, donc un index nul
        // partout. Le systeme meteo continue de fonctionner sans inversion, ce
        // qui est le repli documente.
        resolution_ = 0;
        halfSize_ = 0.0f;
        return;
    }

    values_.resize(static_cast<std::size_t>(resolution) * resolution, 0.0f);

    const float span = 2.0f * halfSize;
    const float step = span / static_cast<float>(resolution - 1);

    // Huit directions : les quatre points cardinaux et les quatre diagonales.
    // Huit et non quatre parce qu'une vallee orientee en diagonale serait vue
    // comme ouverte par un echantillonnage cardinal seul.
    constexpr int kDirections = 8;

    double total = 0.0;
    float maxValue = 0.0f;

    for (uint32_t iz = 0; iz < resolution; ++iz) {
        const float z = -halfSize + static_cast<float>(iz) * step;
        for (uint32_t ix = 0; ix < resolution; ++ix) {
            const float x = -halfSize + static_cast<float>(ix) * step;
            const float h0 = terrain.heightAt(x, z);

            float enclosure = 0.0f;
            int probes = 0;

            for (int d = 0; d < kDirections; ++d) {
                const float angle = 2.0f * kPi * static_cast<float>(d) /
                                    static_cast<float>(kDirections);
                const float dx = std::cos(angle);
                const float dz = std::sin(angle);

                for (float radius : kProbeRadii) {
                    const float h = terrain.heightAt(x + dx * radius, z + dz * radius);

                    // Ce qui compte est le RELIEF AU-DESSUS de la cellule, et
                    // seulement lui : du terrain plus bas dans une direction est
                    // une FUITE pour l'air froid, elle ne contribue pas
                    // negativement, elle contribue simplement zero. Un point
                    // ouvert dans une seule direction reste largement moins
                    // piegeant qu'un point ferme partout, ce qui est le bon
                    // comportement et sort tout seul de la moyenne.
                    enclosure += clamp01((h - h0) / kFullEnclosureRise);
                    ++probes;
                }
            }

            const float value = probes > 0 ? enclosure / static_cast<float>(probes) : 0.0f;
            values_[static_cast<std::size_t>(iz) * resolution + ix] = value;
            total += value;
            maxValue = std::max(maxValue, value);
        }
    }

    maxValue_ = maxValue;
    meanValue_ = values_.empty() ? 0.0f
                                 : static_cast<float>(total / static_cast<double>(values_.size()));
}

float ColdAirTrapMap::at(float worldX, float worldZ) const {
    if (values_.empty() || resolution_ < 2) {
        return 0.0f;
    }

    const float span = 2.0f * halfSize_;
    const float step = span / static_cast<float>(resolution_ - 1);

    // Ecretage AVANT interpolation : c'est le contrat de TOTALITE de
    // HeightSampler::heightAt applique ici - un point hors emprise rend la
    // valeur du bord, jamais une erreur.
    const float fx = std::clamp((safeFinite(worldX) + halfSize_) / step, 0.0f,
                                static_cast<float>(resolution_ - 1));
    const float fz = std::clamp((safeFinite(worldZ) + halfSize_) / step, 0.0f,
                                static_cast<float>(resolution_ - 1));

    const uint32_t x0 = static_cast<uint32_t>(fx);
    const uint32_t z0 = static_cast<uint32_t>(fz);
    const uint32_t x1 = std::min(x0 + 1u, resolution_ - 1u);
    const uint32_t z1 = std::min(z0 + 1u, resolution_ - 1u);

    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);

    const auto value = [this](uint32_t ix, uint32_t iz) {
        return values_[static_cast<std::size_t>(iz) * resolution_ + ix];
    };

    const float top = value(x0, z0) + (value(x1, z0) - value(x0, z0)) * tx;
    const float bottom = value(x0, z1) + (value(x1, z1) - value(x0, z1)) * tx;
    return top + (bottom - top) * tz;
}

// ---------------------------------------------------------------------------
// WeatherSystem - COUCHE 1
// ---------------------------------------------------------------------------

WeatherSystem::WeatherSystem(const WeatherParams& params, uint32_t seed)
    : params_(params), seed_(seed) {}

float WeatherSystem::frontNoise(float gameHours, uint32_t channel) const {
    const float periodHours = std::max(1.0f, params_.frontPeriodDays * 24.0f);

    // Le canal decale la GRAINE, pas la coordonnee : deux canaux sont alors des
    // bruits independants, alors qu'un simple decalage temporel donnerait deux
    // fois le meme temps qu'il fait, avec un jour de retard.
    const uint32_t channelSeed = seed_ ^ hashU32(channel * 0x85EBCA6Bu + 1u);
    const float raw = fbm1D(gameHours / periodHours, channelSeed, params_.frontOctaves);

    // Gain puis ecretage : voir WeatherParams::frontNoiseGain pour la raison,
    // qui est un ECHEC DE TEST et non un reglage de gout - sans gain, le modele
    // ne produisait ni grand beau ni vraie pluie.
    return std::clamp(raw * params_.frontNoiseGain, -1.0f, 1.0f);
}

SynopticState WeatherSystem::synopticAt(float gameHours) const {
    SynopticState sky;
    sky.gameHours = safeFinite(gameHours);

    // --- Calendrier ---------------------------------------------------------
    sky.timeOfDayHours = std::fmod(sky.gameHours, 24.0f);
    if (sky.timeOfDayHours < 0.0f) {
        sky.timeOfDayHours += 24.0f;
    }
    sky.dayOfYear = std::fmod(params_.startDayOfYear + sky.gameHours / 24.0f, 365.0f);
    if (sky.dayOfYear < 0.0f) {
        sky.dayOfYear += 365.0f;
    }

    // --- Soleil -------------------------------------------------------------
    const float declination = solarDeclinationDegrees(sky.dayOfYear);
    const float latRad = glm::radians(params_.latitudeDegrees);
    const float decRad = glm::radians(declination);
    // Angle horaire : 15 degres par heure, nul au midi solaire.
    const float hourAngle = glm::radians(15.0f * (sky.timeOfDayHours - 12.0f));

    const float sinElevation = std::clamp(
        std::sin(latRad) * std::sin(decRad) +
            std::cos(latRad) * std::cos(decRad) * std::cos(hourAngle),
        -1.0f, 1.0f);
    const float elevation = std::asin(sinElevation);
    sky.sunElevationDegrees = glm::degrees(elevation);

    const float cosElevation = std::cos(elevation);
    float azimuth = 180.0f; // plein sud, valeur de repli au zenith exact
    if (cosElevation > 1e-4f && std::cos(latRad) > 1e-4f) {
        const float cosAzimuth = std::clamp(
            (std::sin(decRad) - sinElevation * std::sin(latRad)) /
                (cosElevation * std::cos(latRad)),
            -1.0f, 1.0f);
        azimuth = glm::degrees(std::acos(cosAzimuth));
        // acos rend [0, 180] : c'est le matin (soleil a l'est). L'apres-midi
        // (angle horaire positif) se lit en miroir a l'ouest.
        if (hourAngle > 0.0f) {
            azimuth = 360.0f - azimuth;
        }
    }
    sky.sunAzimuthDegrees = azimuth;

    // Vecteur DU POINT VERS LE SOLEIL. Verification a midi : azimut 180, donc
    // sin(180) = 0 et -cos(180) = +1, soit +Z, soit le SUD. Conforme.
    const glm::vec3 horizontal = compassDirection(azimuth);
    sky.sunDirection = glm::vec3(horizontal.x * cosElevation, sinElevation,
                                 horizontal.z * cosElevation);
    if (glm::length(sky.sunDirection) > 1e-6f) {
        sky.sunDirection = glm::normalize(sky.sunDirection);
    } else {
        sky.sunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
    }

    sky.daylight = std::max(0.0f, sinElevation);

    // --- Nebulosite (canal 1) ----------------------------------------------
    // Calculee AVANT la temperature parce que celle-ci en depend : un ciel
    // couvert ecrase l'amplitude diurne.
    const float cloudNoise = frontNoise(sky.gameHours, 1u);
    sky.cloudCover = clamp01(params_.meanCloudCover + params_.cloudAmplitude * cloudNoise);

    // --- Temperature de reference ------------------------------------------
    // Saison : deux harmoniques.
    const float seasonPhase = 2.0f * kPi * (sky.dayOfYear - params_.seasonalPeakDay) / 365.0f;
    const float seasonal = params_.seasonalAmplitudeC * std::cos(seasonPhase) +
                           params_.seasonalHarmonic2C * std::cos(2.0f * seasonPhase);

    // Diurne : deux harmoniques, attenuees par les nuages. La deuxieme
    // harmonique est ce qui rend le matin plus raide que le soir.
    const float diurnalPhase = 2.0f * kPi * (sky.timeOfDayHours - params_.diurnalPeakHour) / 24.0f;
    const float cloudDamping = 1.0f - params_.cloudDiurnalDamping * sky.cloudCover;
    const float diurnal = (params_.diurnalAmplitudeC * std::cos(diurnalPhase) +
                           params_.diurnalHarmonic2C * std::cos(2.0f * diurnalPhase)) *
                          cloudDamping;

    // Masse d'air (canal 0).
    const float airMass = params_.frontTemperatureAmplitudeC * frontNoise(sky.gameHours, 0u);

    sky.seaLevelTemperatureC = params_.seaLevelAnnualMeanC + seasonal + diurnal + airMass;

    // --- Vent (canaux 2 et 3) ----------------------------------------------
    // Le renfort de vent est pris en VALEUR ABSOLUE du bruit : un front qui
    // passe fait forcir le vent, qu'il apporte du chaud ou du froid. Sans la
    // valeur absolue, la moitie des perturbations produirait un vent NEGATIF,
    // donc un calme plat aussi frequent que la tempete.
    const float windNoise = std::abs(frontNoise(sky.gameHours, 2u));
    sky.seaLevelWindMps = std::max(0.0f, params_.baseWindMps +
                                             params_.frontWindAmplitudeMps * windNoise);

    const float directionNoise = frontNoise(sky.gameHours, 3u);
    sky.windFromDegrees = params_.meanWindFromDegrees +
                          params_.windDirectionSwingDegrees * directionNoise;
    sky.windFromDegrees = std::fmod(sky.windFromDegrees, 360.0f);
    if (sky.windFromDegrees < 0.0f) {
        sky.windFromDegrees += 360.0f;
    }

    // --- Precipitations -----------------------------------------------------
    // Rien en dessous du seuil, puis montee lineaire jusqu'a l'intensite
    // maximale a nebulosite 1.
    const float threshold = std::clamp(params_.precipitationCloudThreshold, 0.0f, 0.999f);
    if (sky.cloudCover > threshold) {
        const float excess = (sky.cloudCover - threshold) / (1.0f - threshold);
        sky.precipitationMmPerHour = params_.precipitationMaxMmPerHour * clamp01(excess);
    }

    return sky;
}

// ---------------------------------------------------------------------------
// WeatherSystem - COUCHE 2
// ---------------------------------------------------------------------------

float WeatherSystem::aspectSignal(const glm::vec3& normal,
                                  const glm::vec3& sunDirection, bool sunUp) const {
    // Le cosinus d'incidence du point, compare a celui qu'aurait du terrain PLAT
    // au meme instant. Les deux sont ecretes a zero : une surface qui tourne le
    // dos au soleil ne recoit pas un rayonnement negatif, elle n'en recoit pas.
    const float incidence = sunUp ? std::max(0.0f, glm::dot(normal, sunDirection))
                                  : 0.0f;
    const float flatIncidence = std::max(0.0f, sunDirection.y);
    return std::clamp(
        (incidence - flatIncidence) / std::max(1e-3f, params_.aspectIncidenceReference),
        -1.0f, 1.0f);
}

float WeatherSystem::aspectExposureAt(const glm::vec3& position,
                                      const HeightSampler* terrain) const {
    // Pas de relief, pas de versant. Le repli est la meme convention que celle
    // de sampleAt avec un terrain nul.
    if (terrain == nullptr) { return 0.0f; }

    // MIDI, et le meme midi pour tout le monde. C'est ce qui fait de cette
    // grandeur un CLIMAT et non une meteo : la valeur rendue pour un point donne
    // ne depend d'aucune horloge, donc une essence d'arbre ne peut pas changer
    // entre deux images. Le jour de l'annee retenu est celui de l'origine de
    // partie ; la saison deplace l'elevation du soleil de midi mais pas
    // l'ORDRE des versants, qui est tout ce dont l'appelant a besoin.
    const SynopticState noon = synopticAt(12.0f);

    const glm::vec3 normal = terrainNormal(*terrain, safeFinite(position.x),
                                           safeFinite(position.z));
    // sunUp force a true : a midi le soleil l'est, sauf a une latitude polaire
    // que ce jeu n'a pas, et le forcer evite de faire dependre une propriete
    // permanente du relief d'un test qui pourrait basculer.
    return aspectSignal(normal, noon.sunDirection, /*sunUp=*/true);
}

WeatherSample WeatherSystem::sampleAt(const glm::vec3& position, float gameHours,
                                      const HeightSampler* terrain) const {
    return sampleAt(position, synopticAt(gameHours), terrain);
}

WeatherSample WeatherSystem::sampleAt(const glm::vec3& position, const SynopticState& sky,
                                      const HeightSampler* terrain) const {
    WeatherSample out;

    const float x = safeFinite(position.x);
    const float y = safeFinite(position.y);
    const float z = safeFinite(position.z);
    const float altitudeKm = y / 1000.0f;

    out.cloudCover = sky.cloudCover;

    // =======================================================================
    // Geometrie locale : une seule lecture de normale, reutilisee partout.
    // =======================================================================
    glm::vec3 normal(0.0f, 1.0f, 0.0f);
    if (terrain != nullptr) {
        normal = terrainNormal(*terrain, x, z);
    }

    // =======================================================================
    // 1. Temperature locale
    // =======================================================================

    // --- Gradient altitudinal ----------------------------------------------
    // MEME mecanisme que NeedsParams::lapseRateCPerKm (Phase 16) - et non un
    // second gradient qui coexisterait avec lui : le parametre a ete DEPLACE
    // ici, il n'existe plus dans Needs.
    out.lapseC = -params_.lapseRateCPerKm * altitudeKm;

    // --- Adret / ubac par produit scalaire ---------------------------------
    // Le cosinus d'incidence du point, compare a celui qu'aurait du terrain PLAT
    // au meme instant. Les deux sont ecretes a zero : une surface qui tourne le
    // dos au soleil ne recoit pas un rayonnement negatif, elle n'en recoit pas.
    // Le signal lui-meme vit dans aspectSignal() depuis la Phase 26 : la Phase 26
    // en a besoin AU SOLEIL DE MIDI pour decider des essences d'arbres, et la
    // pire facon de le lui donner aurait ete d'en recopier la formule ailleurs.
    // Ce qui reste ici est ce qui appartient VRAIMENT a la meteo de l'instant :
    // la force du soleil et l'attenuation par les nuages.
    const float signal = aspectSignal(normal, sky.sunDirection, sky.daylight > 0.0f);

    // LE SOLEIL DOIT ETRE LEVE. Sans cette condition, l'ecart d'incidence
    // continuait de se calculer avec un soleil SOUS l'horizon et rendait un
    // versant sud plus chaud de 2,9 degres a deux heures du matin - un echec de
    // test, pas une hypothese. Le second facteur eteint progressivement l'effet
    // quand le soleil est rasant, ce qui est aussi le comportement reel.
    const float sunStrength = clamp01(sky.daylight /
                                      std::max(1e-3f, params_.aspectFullDaylightSin));
    out.aspectC = params_.aspectBonusC * signal * sunStrength *
                  (1.0f - params_.cloudAspectDamping * sky.cloudCover);

    // =======================================================================
    // 2. Vent local - SONDAGE AMONT
    // =======================================================================
    // Vent de reference au point : le synoptique corrige de l'altitude. C'est
    // deja un vent "en air libre" ; tout ce qui suit est la reponse du RELIEF.
    const float referenceWind =
        std::max(0.0f, sky.seaLevelWindMps + params_.windAltitudeGainMpsPerKm * altitudeKm);

    const glm::vec3 fromWind = compassDirection(sky.windFromDegrees); // vers l'AMONT
    const glm::vec3 flow = -fromWind;                                 // sens du deplacement de l'air

    float speedFactor = 1.0f;
    float exposureSignal = 0.0f; // > 0 expose, < 0 abrite
    float slopeAlongFlow = 0.0f; // > 0 : le terrain monte dans le sens du vent

    if (terrain != nullptr) {
        const float h0 = terrain->heightAt(x, z);

        const float hNear = terrain->heightAt(x + fromWind.x * params_.upwindNearMeters,
                                              z + fromWind.z * params_.upwindNearMeters);
        const float hMid = terrain->heightAt(x + fromWind.x * params_.upwindMidMeters,
                                             z + fromWind.z * params_.upwindMidMeters);
        const float hFar = terrain->heightAt(x + fromWind.x * params_.upwindFarMeters,
                                             z + fromWind.z * params_.upwindFarMeters);

        // COMBINAISON DES TROIS ECHELLES : un MAXIMUM d'abri contre un MINIMUM
        // d'exposition, et surtout PAS une moyenne des trois altitudes.
        //
        // La premiere version faisait cette moyenne ponderee, et elle etait
        // fausse - le test l'a montre. Un point situe 250 m sous le vent d'une
        // crete de 200 m, donc dans un abri franc, obtenait un signal de -0,05 :
        // la crete etait bien vue par la sonde proche, mais la plaine des sondes
        // a 750 m et 2000 m ramenait la moyenne au niveau du point lui-meme, et
        // l'abri disparaissait dans l'arithmetique.
        //
        // La regle correcte tient en une phrase : etre abrite, c'est avoir du
        // relief au-dessus de soi DANS AU MOINS UNE des echelles amont ; etre
        // expose, c'est dominer TOUTES les echelles a la fois. D'ou le max d'un
        // cote et le min de l'autre.
        const float relief = std::max(1.0f, params_.exposureReliefMeters);
        const float shelter = std::max({clamp01((hNear - h0) / relief),
                                        clamp01((hMid - h0) / relief),
                                        clamp01((hFar - h0) / relief)});
        const float dominance = std::min({clamp01((h0 - hNear) / relief),
                                          clamp01((h0 - hMid) / relief),
                                          clamp01((h0 - hFar) / relief)});

        // Les deux ne peuvent pas etre non nuls en meme temps (l'un exige d'etre
        // sous une sonde, l'autre d'etre au-dessus de toutes) : la difference est
        // donc bien un signal unique dans [-1, 1], positif = expose.
        exposureSignal = std::clamp(dominance - shelter, -1.0f, 1.0f);

        speedFactor = 1.0f + (exposureSignal > 0.0f
                                  ? exposureSignal * params_.exposureWindGain
                                  : exposureSignal * params_.shelterWindReduction);

        // --- Retrecissement (Venturi) --------------------------------------
        // Deux sondes PERPENDICULAIRES au vent. Si les deux epaules dominent le
        // point, le flux passe par un entonnoir et accelere.
        const glm::vec3 lateral(flow.z, 0.0f, -flow.x);
        const float hLeft = terrain->heightAt(x + lateral.x * params_.lateralMeters,
                                              z + lateral.z * params_.lateralMeters);
        const float hRight = terrain->heightAt(x - lateral.x * params_.lateralMeters,
                                               z - lateral.z * params_.lateralMeters);

        const float gap = std::min(hLeft, hRight) - h0;
        const float venturiSignal = clamp01(gap / std::max(1.0f, params_.venturiReliefMeters));

        // POURQUOI CETTE PONDERATION EXISTE, et ce qu'elle corrige : un FOND DE
        // VALLEE a lui aussi ses deux cotes plus hauts, exactement comme un col.
        // La seule chose qui les distingue est le rapport a l'AMONT - un col
        // domine son amont (le vent le franchit), un fond de vallee non (le vent
        // passe au-dessus). Sans ce poids, chaque fond de vallee de la zone de
        // jeu deviendrait une soufflerie, ce qui est exactement l'inverse de la
        // realite.
        const float venturiWeight = clamp01(0.5f + 0.5f * exposureSignal);
        speedFactor *= 1.0f + params_.venturiWindGain * venturiSignal * venturiWeight;

        // --- Pente dans le sens du vent, pour le ridge lift ----------------
        slopeAlongFlow = (h0 - hNear) / std::max(1.0f, params_.upwindNearMeters);
    }

    out.windMps = std::clamp(referenceWind * speedFactor, 0.0f, params_.maxLocalWindMps);
    out.windVector = flow * out.windMps;

    // =======================================================================
    // 3. Composante verticale
    // =======================================================================

    // --- Ridge lift ---------------------------------------------------------
    // L'air suit le relief : s'il monte d'une pente s en avancant a la vitesse
    // U, sa vitesse verticale vaut s * U, moins un rendement (une partie du flux
    // contourne au lieu de monter).
    if (slopeAlongFlow > 0.0f) {
        out.ridgeLiftMps = std::min(params_.maxRidgeLiftMps,
                                    params_.ridgeLiftEfficiency * slopeAlongFlow * out.windMps);
    }

    // --- Subsidence et turbulence sous le vent ------------------------------
    // Un point nettement plus bas que son amont est SOUS LE VENT du relief :
    // l'air y redescend et il y est desordonne. Les deux effets grandissent avec
    // le vent - sans vent, un versant sous le vent est parfaitement calme.
    const float leeFactor = clamp01(-exposureSignal);

    // L'intensite se mesure sur le vent DE REFERENCE et non sur le vent local, et
    // ce n'est pas un detail : le vent local d'un point sous le vent est
    // justement FAIBLE (c'est ce que le facteur d'abri vient de calculer), alors
    // que ce qui l'agite est le flux qui vient de decoller de la crete, lequel a
    // la vitesse AMONT. Prendre le vent local rendait un sillage d'autant plus
    // calme qu'il etait plus abrite - exactement l'inverse de la realite.
    const float windIntensity =
        clamp01(referenceWind / std::max(1.0f, params_.turbulenceFullWindMps));
    out.subsidenceMps = params_.leeSubsidenceMps * leeFactor * windIntensity;
    out.turbulence = clamp01(params_.leeTurbulenceMax * leeFactor * windIntensity);

    // --- Thermiques ---------------------------------------------------------
    // Pilotees par l'INSOLATION du sol (le meme cosinus d'incidence que
    // l'adret/ubac : un seul calcul de "combien de soleil ce versant prend"),
    // et par l'heure - le flux de chaleur du sol culmine avant la temperature.
    const float incidence = (sky.daylight > 0.0f) ? std::max(0.0f, glm::dot(normal, sky.sunDirection)) : 0.0f;
    if (sky.daylight > 0.0f && incidence > 0.0f) {
        const float hourOffset = (sky.timeOfDayHours - params_.thermalPeakHour) /
                                 std::max(0.5f, params_.thermalHalfWidthHours);
        const float timeWindow = std::max(0.0f, 1.0f - hourOffset * hourOffset);
        const float cloudFactor = 1.0f - params_.cloudThermalDamping * sky.cloudCover;
        out.thermalMps = params_.thermalMaxMps * incidence * timeWindow *
                         std::max(0.0f, cloudFactor);
    }

    out.verticalMps = out.ridgeLiftMps + out.thermalMps + out.subsidenceMps;

    // =======================================================================
    // 4. Precipitations locales et etat du sol
    // =======================================================================
    // Barriere orographique VOLONTAIREMENT SIMPLE : un versant que le flux doit
    // gravir souleve la masse d'air, donc il pleut plus. L'ombre pluviometrique
    // sous le vent n'est PAS modelisee au-dela de l'absence de renforcement -
    // c'est la limite assumee de cette session, notee dans NOTES-PHASE23.md.
    const float orographicSignal =
        clamp01(slopeAlongFlow / std::max(1e-3f, params_.orographicFullSlope));
    const float orographic = (1.0f + params_.orographicGain * orographicSignal) *
                             (1.0f + params_.precipitationAltitudeGainPerKm * altitudeKm);

    out.precipitationMmPerHour = std::max(0.0f, sky.precipitationMmPerHour * orographic);
    out.cloudCover = clamp01(sky.cloudCover + params_.orographicCloudGain * orographicSignal);

    out.temperatureC = sky.seaLevelTemperatureC + out.lapseC + out.aspectC;

    // --- Inversion de fond de cuvette --------------------------------------
    // Calculee APRES la temperature de base parce qu'elle depend du vent (qui
    // brasse) et de la nebulosite (qui empeche le rayonnement nocturne), et
    // APPLIQUEE ensuite. Trois conditions doivent etre reunies pour qu'une
    // inversion s'installe, et le produit des trois les exprime directement :
    // il faut un creux ferme, il faut la NUIT, et il faut un ciel clair et
    // calme.
    if (trapMap_ != nullptr && !trapMap_->empty()) {
        const float trap = clamp01(trapMap_->at(x, z));
        const float nightFactor = 1.0f - clamp01(sky.daylight * 3.0f);
        const float clearFactor = 1.0f - sky.cloudCover;
        const float calmFactor =
            1.0f - clamp01(out.windMps / std::max(0.1f, params_.inversionMixingWindMps));
        out.inversionC = params_.inversionMaxC * trap * nightFactor * clearFactor * calmFactor;
        out.temperatureC += out.inversionC;
    }

    out.snowing = out.precipitationMmPerHour > 0.0f &&
                  out.temperatureC <= params_.snowThresholdC;

    out.wetness = groundWetness(sky.gameHours, y, orographic);

    return out;
}

float WeatherSystem::groundWetness(float gameHours, float altitudeMeters,
                                   float orographicFactor) const {
    const float now = safeFinite(gameHours);
    const int samples = std::clamp(params_.wetnessSamples, 1, 32);
    const float window = std::max(0.1f, params_.wetnessMemoryHours);
    const float drying = std::max(0.1f, params_.dryingTimeHours);
    const float oro = std::max(0.0f, safeFinite(orographicFactor, 1.0f));
    const float altitudeKm = safeFinite(altitudeMeters) / 1000.0f;
    const float altitudeGain = 1.0f + params_.precipitationAltitudeGainPerKm * altitudeKm;

    // NON INTEGREE, RELUE : on remonte le temps dans la fonction synoptique - qui
    // est pure - au lieu d'accumuler image par image. C'est ce qui garantit
    // qu'un joueur qui arrive au meme endroit au meme instant trouve la MEME
    // mouillure, qu'il ait mis dix minutes ou deux heures a venir.
    float wettest = 0.0f;
    for (int i = 0; i < samples; ++i) {
        const float age = window * (static_cast<float>(i) + 0.5f) / static_cast<float>(samples);
        const SynopticState past = synopticAt(now - age);

        const float precip = past.precipitationMmPerHour * oro * altitudeGain;
        const float soaked = clamp01(precip / std::max(0.01f, params_.wetnessSaturationMmPerHour));
        const float dried = std::exp(-age / drying);
        wettest = std::max(wettest, soaked * dried);
    }

    return clamp01(wettest);
}

} // namespace vercora
