#include "world/Habitat.hpp"

#include <algorithm>
#include <cmath>

namespace vercora {
namespace {

// Rampe lineaire ecretee, identique a celle de Vegetation.cpp : 0 sous `from`,
// 1 au-dessus de `to`, lineaire entre les deux. Redefinie ici plutot que
// partagee parce que trois lignes exportees dans un en-tete commun feraient un
// couplage pour rien.
float ramp(float value, float from, float to) {
    if (to <= from) {
        return value >= to ? 1.0f : 0.0f;
    }
    return std::clamp((value - from) / (to - from), 0.0f, 1.0f);
}

// En dessous, une cellule est consideree revenue a l'etat neuf et oubliee. La
// valeur n'a rien de critique : elle ne fait que borner la taille de la table.
constexpr float kForgetEpsilon = 1e-3f;

// Meme garde-fou que Forest::update et Needs::update : une image qui a dure une
// demi-seconde (chargement de tuiles, fenetre deplacee) ne doit pas faire
// avancer la memoire d'un demi-cran d'un coup.
constexpr float kMaxStep = 0.25f;

} // namespace

float activityNoise(Activity activity) {
    switch (activity) {
        case Activity::Rest:      return 0.25f;
        case Activity::Walk:      return 1.00f;
        case Activity::Bike:      return 2.00f;
        case Activity::Climb:     return 0.60f;
        case Activity::Paraglide: return 0.15f;
    }
    return 1.0f;
}

Habitat::Habitat(const HabitatParams& params) : params_(params) {
    // Une taille de cellule nulle ou negative rendrait tous les indices
    // indefinis ; on retombe sur le defaut plutot que de produire des NaN
    // silencieux dans toute la grille.
    if (!(params_.cellSize > 0.0f)) {
        params_.cellSize = HabitatParams{}.cellSize;
    }
}

Habitat::CellKey Habitat::keyAt(float worldX, float worldZ) const {
    return CellKey{
        static_cast<int32_t>(std::floor(worldX / params_.cellSize)),
        static_cast<int32_t>(std::floor(worldZ / params_.cellSize)),
    };
}

const HabitatCell* Habitat::find(const CellKey& key) const {
    const auto found = cells_.find(key);
    return found == cells_.end() ? nullptr : &found->second;
}

void Habitat::notePresence(const glm::vec3& position, float deltaTime, float noise) {
    const float dt = std::clamp(deltaTime, 0.0f, kMaxStep);
    if (dt <= 0.0f || noise <= 0.0f) {
        return;
    }

    // Toutes les cellules qui MORDENT sur le disque de bruit. Le rayon valant
    // 120 m pour des cellules de 32 m, il y en a une soixantaine - un balayage
    // de 9x9 au plus, une fois par image. C'est negligeable devant le semis
    // d'herbe, qui en visite des milliers.
    const CellKey centre = keyAt(position.x, position.z);
    const auto span = static_cast<int32_t>(
        std::ceil(params_.noiseRadius / params_.cellSize));

    for (int32_t dz = -span; dz <= span; ++dz) {
        for (int32_t dx = -span; dx <= span; ++dx) {
            const CellKey key{ centre.x + dx, centre.z + dz };

            // Distance du joueur au POINT LE PLUS PROCHE de la cellule, et non
            // a son centre. La difference n'est pas un detail : avec le centre,
            // la cellule ou l'on se tient recevrait une ponderation qui depend
            // de l'endroit ou l'on est TOMBE dans cette cellule - jusqu'a 23 m
            // de son centre, donc la moitie du rayon perdue pour rien. La
            // cellule qui nous contient est a distance nulle, donc pleinement
            // derangee : c'est la seule lecture qui ait un sens.
            const float minX = static_cast<float>(key.x) * params_.cellSize;
            const float minZ = static_cast<float>(key.z) * params_.cellSize;
            const float nearestX = std::clamp(position.x, minX, minX + params_.cellSize);
            const float nearestZ = std::clamp(position.z, minZ, minZ + params_.cellSize);
            const float distance = std::sqrt(
                (nearestX - position.x) * (nearestX - position.x) +
                (nearestZ - position.z) * (nearestZ - position.z));
            if (distance >= params_.noiseRadius) {
                continue;
            }

            // Ponderation en 1 - (d/r)^2 et non lineaire. Le choix est dicte par
            // le rapport montee/decroissance : une ponderation lineaire s'ecrase
            // si vite qu'a la peripherie du rayon la montee devient plus lente
            // que la decroissance - une cellule lointaine ne pourrait alors
            // jamais monter, quel que soit le temps passe, et la deuxieme
            // mecanique du cahier des charges ("rester trop longtemps dans un
            // rayon donne") n'existerait tout simplement pas. En carre, la
            // ponderation vaut encore 0,31 a 100 m : un joueur installe a cette
            // distance d'un animal vide son vallon en trois minutes, ce qui est
            // le comportement voulu.
            const float ratio = distance / params_.noiseRadius;
            const float weight = 1.0f - ratio * ratio;
            HabitatCell& cell = cells_[key];
            cell.disturbance = std::min(
                1.0f, cell.disturbance + params_.disturbanceRise * noise * weight * dt);
            // La cellule est OCCUPEE : elle ne decroit pas cette image (voir
            // update). Sans ce drapeau, la decroissance se retrancherait de la
            // montee, et une presence faible mais continue - justement le cas
            // du campement a distance - resterait indefiniment a zero au lieu
            // de s'accumuler.
            cell.occupied = true;
        }
    }
}

void Habitat::recordFelling(const glm::vec3& position) {
    HabitatCell& cell = cells_[keyAt(position.x, position.z)];
    cell.logging = std::min(1.0f, cell.logging + params_.loggingPerTree);
    cell.disturbance = std::min(1.0f, cell.disturbance + params_.loggingNoise);
    ++cell.treesFelled;

    // Volontairement PAS de debordement sur les cellules voisines, contrairement
    // au bruit de presence : un arbre tombe a un endroit precis, et c'est cet
    // endroit-la qui perd son couvert. L'interpolation bilineaire de
    // groundCover() suffit a ce que la clairiere ne soit pas un carre net a
    // l'oeil ; etaler en plus le compteur reviendrait a lisser deux fois.
}

void Habitat::update(float deltaTime) {
    const float dt = std::clamp(deltaTime, 0.0f, kMaxStep);
    if (dt <= 0.0f) {
        return;
    }

    const float disturbanceStep = params_.disturbanceDecay * dt;
    const float loggingStep = params_.loggingDecay * dt;

    for (auto it = cells_.begin(); it != cells_.end();) {
        HabitatCell& cell = it->second;

        // La frequentation ne redescend QUE dans les cellules que le joueur ne
        // derange plus. La deforestation, elle, se resorbe toujours : une coupe
        // ne se remet pas a couper parce qu'on est reste sur place.
        const bool occupied = cell.occupied;
        cell.occupied = false;
        if (!occupied) {
            cell.disturbance = std::max(0.0f, cell.disturbance - disturbanceStep);
        }
        cell.logging = std::max(0.0f, cell.logging - loggingStep);

        // OUBLI. Sans lui, traverser les 25 km2 finirait par garder en memoire
        // toutes les cellules traversees, dont l'immense majorite est revenue a
        // l'etat neuf : c'est exactement l'eviction du cache de Vegetation,
        // appliquee a la memoire du milieu.
        //
        // treesFelled n'est PAS une raison de garder la cellule : l'interet du
        // compte brut est le journal du moment, pas un cadastre des coupes. Une
        // cellule qui a repousse est une cellule ordinaire.
        // Une cellule OCCUPEE n'est jamais oubliee, meme si son compteur est
        // encore sous le seuil. Defaut reel, et il annulait toute la mecanique :
        // au premier pas de temps, une cellule ne contient que 0,0003 - donc
        // moins que l'epsilon d'oubli - et elle etait effacee a l'image meme ou
        // elle venait d'etre creee. La frequentation restait a zero quel que
        // soit le temps passe, sans que rien ne le signale. Mesure apres
        // correction : une minute de marche sur place donne bien 1,0.
        if (!occupied && cell.disturbance <= kForgetEpsilon &&
            cell.logging <= kForgetEpsilon) {
            it = cells_.erase(it);
        } else {
            ++it;
        }
    }
}

float Habitat::disturbanceAt(float worldX, float worldZ) const {
    const HabitatCell* cell = find(keyAt(worldX, worldZ));
    return cell ? cell->disturbance : 0.0f;
}

float Habitat::loggingAt(float worldX, float worldZ) const {
    const HabitatCell* cell = find(keyAt(worldX, worldZ));
    return cell ? cell->logging : 0.0f;
}

uint32_t Habitat::treesFelledAt(float worldX, float worldZ) const {
    const HabitatCell* cell = find(keyAt(worldX, worldZ));
    return cell ? cell->treesFelled : 0u;
}

float Habitat::coverFromLogging(float logging) const {
    const float bareness = ramp(logging, params_.barenessBegin, params_.barenessFull);
    return glm::mix(1.0f, params_.minGroundCover, bareness);
}

float Habitat::clearing(float worldX, float worldZ) const {
    // Ecretage explicite : loggingAt vit deja dans [0, 1] par construction, mais
    // le contrat de ClearingSource l'exige et un jour ou l'autre quelqu'un
    // changera loggingPerTree sans relire ce fichier.
    const float logging = loggingAt(worldX, worldZ);
    return (logging < 0.0f) ? 0.0f : ((logging > 1.0f) ? 1.0f : logging);
}

float Habitat::groundCover(float worldX, float worldZ) const {
    // Interpolation bilineaire entre les QUATRE centres de cellule qui
    // encadrent le point. L'astuce d'indice est la meme que pour une texture :
    // on decale d'une demi-cellule avant de prendre la partie entiere, ce qui
    // fait que le repere des centres coincide avec celui des coins.
    const float u = worldX / params_.cellSize - 0.5f;
    const float v = worldZ / params_.cellSize - 0.5f;
    const float u0 = std::floor(u);
    const float v0 = std::floor(v);
    const float fu = u - u0;
    const float fv = v - v0;

    const auto ix = static_cast<int32_t>(u0);
    const auto iz = static_cast<int32_t>(v0);

    const auto coverOf = [this](int32_t cx, int32_t cz) {
        const HabitatCell* cell = find(CellKey{ cx, cz });
        // Cellule absente = milieu intact : c'est le contrat TOTAL de
        // GroundCoverSource. Un monde neuf rend donc 1 partout, et le semis
        // d'herbe est bit a bit celui d'avant la Phase 17.
        return cell ? coverFromLogging(cell->logging) : 1.0f;
    };

    const float c00 = coverOf(ix, iz);
    const float c10 = coverOf(ix + 1, iz);
    const float c01 = coverOf(ix, iz + 1);
    const float c11 = coverOf(ix + 1, iz + 1);

    return glm::mix(glm::mix(c00, c10, fu), glm::mix(c01, c11, fu), fv);
}

bool Habitat::allowsWildlife(float worldX, float worldZ) const {
    const HabitatCell* cell = find(keyAt(worldX, worldZ));
    if (cell == nullptr) {
        return true; // zone vierge de toute memoire : la faune y est chez elle
    }
    return cell->disturbance < params_.respawnThreshold &&
           cell->logging < params_.loggingBlocksWildlife;
}

bool Habitat::wildlifeFlees(float worldX, float worldZ) const {
    const HabitatCell* cell = find(keyAt(worldX, worldZ));
    return cell != nullptr && cell->disturbance > params_.fleeThreshold;
}

float Habitat::peakDisturbance() const {
    float peak = 0.0f;
    for (const auto& [key, cell] : cells_) {
        (void)key;
        peak = std::max(peak, cell.disturbance);
    }
    return peak;
}

float Habitat::peakLogging() const {
    float peak = 0.0f;
    for (const auto& [key, cell] : cells_) {
        (void)key;
        peak = std::max(peak, cell.logging);
    }
    return peak;
}

} // namespace vercora
