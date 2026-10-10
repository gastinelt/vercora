#include "world/Wildlife.hpp"

#include "core/HeightSampler.hpp"
// PHASE 20 : kAnimalHeight / kAnimalLength. La collision doit etre deduite des
// dimensions du MESH et non les redeclarer : un cervide qui grandirait sans que
// son collider suive redeviendrait traversable par le haut.
#include "world/AnimalMesh.hpp"
#include "world/Habitat.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace vercora {
namespace {

// Meme garde-fou que Forest::update, Needs::update et Habitat::update : une
// image anormalement longue ne doit pas telporter les animaux.
constexpr float kMaxStep = 0.25f;

// Demi-base de la lecture de pente, en m. 2,0, donc une base de 4 m : MEME
// valeur et MEME raison que BikeParams::slopeSampleStep et
// WaterSurveyParams::slopeSampleStep - au pas de 1 m, le bruit decimetrique du
// LiDAR fait lire des contre-pentes qui n'existent pas.
constexpr float kSlopeStep = 2.0f;

float horizontalDistance(const glm::vec3& a, const glm::vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

glm::mat4 Animal::modelMatrix() const {
    glm::mat4 model = glm::translate(glm::mat4(1.0f), position);
    model = glm::rotate(model, glm::radians(-headingDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
    return glm::scale(model, glm::vec3(scale));
}

Wildlife::Wildlife(const HeightSampler& terrain, const HeightSampler* cover,
                   float terrainExtent, const WildlifeParams& params)
    : terrain_(terrain),
      cover_(cover),
      terrainExtent_(terrainExtent),
      params_(params),
      rng_(params.seed) {
    animals_.reserve(params_.maxAnimals);
}

float Wildlife::slopeAt(float worldX, float worldZ) const {
    const float hx1 = terrain_.heightAt(worldX - kSlopeStep, worldZ);
    const float hx2 = terrain_.heightAt(worldX + kSlopeStep, worldZ);
    const float hz1 = terrain_.heightAt(worldX, worldZ - kSlopeStep);
    const float hz2 = terrain_.heightAt(worldX, worldZ + kSlopeStep);

    const float dx = (hx2 - hx1) / (2.0f * kSlopeStep);
    const float dz = (hz2 - hz1) / (2.0f * kSlopeStep);
    return std::sqrt(dx * dx + dz * dz);
}

float Wildlife::coverAt(float worldX, float worldZ) const {
    if (cover_ == nullptr) {
        return params_.coverWithoutMap;
    }
    // La carte de couvert est une Heightmap dont la "hauteur" est une part de
    // retours de vegetation dans [0, 1] - c'est la convention posee par
    // tools/lidar_to_canopy.py et deja consommee par Vegetation::coverAt. Le
    // clamp protege d'une carte hors norme plutot que de propager un couvert
    // negatif dans les filtres.
    return std::clamp(cover_->heightAt(worldX, worldZ), 0.0f, 1.0f);
}

bool Wildlife::isPlaceable(float worldX, float worldZ, const Habitat& habitat) const {
    // Emprise, avec une marge : un animal pile au bord verrait ses sondes de
    // pente sortir des donnees.
    const float usable = std::max(0.0f, terrainExtent_ - 2.0f * kSlopeStep);
    if (std::abs(worldX) > usable || std::abs(worldZ) > usable) {
        return false;
    }
    if (slopeAt(worldX, worldZ) > params_.maxSlope) {
        return false;
    }
    if (coverAt(worldX, worldZ) < params_.minCover) {
        return false;
    }
    // LE filtre de la phase : le milieu se souvient, et il refuse.
    if (!habitat.allowsWildlife(worldX, worldZ)) {
        return false;
    }
    for (const Animal& other : animals_) {
        const float dx = other.position.x - worldX;
        const float dz = other.position.z - worldZ;
        if (dx * dx + dz * dz < params_.minSpacing * params_.minSpacing) {
            return false;
        }
    }
    return true;
}

void Wildlife::spawnWave(const glm::vec3& playerPosition, const Habitat& habitat) {
    if (animals_.size() >= params_.maxAnimals) {
        return;
    }

    std::uniform_real_distribution<float> angle(0.0f, 6.28318530718f);
    // Tirage du rayon en RACINE : un tirage uniforme sur [min, max] concentrerait
    // les animaux vers le bord interieur de l'anneau, ou l'aire est plus petite.
    // C'est le meme correctif que pour un semis en disque.
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::uniform_real_distribution<float> sizeDist(0.88f, 1.12f);

    const float rmin2 = params_.spawnRadiusMin * params_.spawnRadiusMin;
    const float rmax2 = params_.spawnRadiusMax * params_.spawnRadiusMax;

    for (uint32_t attempt = 0; attempt < params_.spawnAttempts; ++attempt) {
        if (animals_.size() >= params_.maxAnimals) {
            return;
        }
        const float theta = angle(rng_);
        const float radius = std::sqrt(glm::mix(rmin2, rmax2, unit(rng_)));
        const float x = playerPosition.x + radius * std::cos(theta);
        const float z = playerPosition.z + radius * std::sin(theta);

        if (!isPlaceable(x, z, habitat)) {
            continue;
        }

        Animal animal;
        animal.position = { x, terrain_.heightAt(x, z), z };
        animal.headingDegrees = glm::degrees(angle(rng_));
        animal.scale = sizeDist(rng_);
        animal.state = AnimalState::Calm;
        animal.id = nextId_++;
        animals_.push_back(animal);
        ++spawnCount_;
    }
}

void Wildlife::populate(const glm::vec3& playerPosition, const Habitat& habitat,
                        uint32_t waves) {
    for (uint32_t i = 0; i < waves; ++i) {
        spawnWave(playerPosition, habitat);
    }
}

void Wildlife::update(float deltaTime, const glm::vec3& playerPosition,
                      const Habitat& habitat) {
    const float dt = std::clamp(deltaTime, 0.0f, kMaxStep);

    // --- 1. Oubli des animaux trop lointains ------------------------------
    const std::size_t before = animals_.size();
    animals_.erase(
        std::remove_if(animals_.begin(), animals_.end(),
                       [&](const Animal& animal) {
                           return horizontalDistance(animal.position, playerPosition) >
                                  params_.despawnRadius;
                       }),
        animals_.end());
    despawnCount_ += static_cast<uint32_t>(before - animals_.size());

    // --- 2. Comportement ---------------------------------------------------
    std::uniform_real_distribution<float> turn(-1.0f, 1.0f);

    for (Animal& animal : animals_) {
        const float distance = horizontalDistance(animal.position, playerPosition);

        // DEUX declencheurs de fuite, et ce sont les deux demandes du cahier des
        // charges :
        //   - le joueur s'approche TROP PRES (distance) ;
        //   - le joueur est reste TROP LONGTEMPS dans le secteur, meme sans
        //     jamais s'approcher (habitat.wildlifeFlees, qui lit la memoire de
        //     frequentation remplie par Habitat::notePresence).
        // Le second est le plus important des deux : c'est lui qui fait qu'un
        // camp de base finit par vider son vallon.
        const bool startled =
            distance < params_.fleeRadius ||
            habitat.wildlifeFlees(animal.position.x, animal.position.z);

        if (startled && animal.state == AnimalState::Calm) {
            animal.state = AnimalState::Fleeing;
            animal.fleeTimer = params_.fleeDuration;
            // Cap DIRECTEMENT oppose au joueur. Pas d'evitement d'obstacle, pas
            // de recherche de couvert : c'est la simplification annoncee dans
            // l'en-tete. Le cas degenere (animal exactement sur le joueur) garde
            // le cap courant plutot que de produire un atan2(0,0).
            const float dx = animal.position.x - playerPosition.x;
            const float dz = animal.position.z - playerPosition.z;
            if (dx * dx + dz * dz > 1e-6f) {
                animal.headingDegrees = glm::degrees(std::atan2(dz, dx));
            }
        }

        float speed = params_.wanderSpeed;
        if (animal.state == AnimalState::Fleeing) {
            speed = params_.fleeSpeed;
            animal.fleeTimer -= dt;
            if (animal.fleeTimer <= 0.0f) {
                if (distance > params_.fleeSafeDistance && !startled) {
                    animal.state = AnimalState::Calm;
                } else {
                    // Toujours trop pres (ou toujours derange) : reparti pour
                    // une pointe. C'est ce qui fait qu'un joueur qui poursuit
                    // n'attrape jamais rien - et c'est voulu.
                    animal.fleeTimer = params_.fleeDuration;
                }
            }
        } else {
            // Errance : le cap derive doucement. Sans elle, un animal paisible
            // avancerait indefiniment en ligne droite et sortirait du decor.
            animal.headingDegrees += params_.wanderTurn * turn(rng_) * dt;
        }

        const float heading = glm::radians(animal.headingDegrees);
        animal.position.x += std::cos(heading) * speed * dt;
        animal.position.z += std::sin(heading) * speed * dt;

        // Recollage au relief a chaque image : c'est le meme contrat que la
        // camera en mode marche. Un animal ne "tombe" pas, il suit la surface.
        animal.position.y = terrain_.heightAt(animal.position.x, animal.position.z);
    }

    // --- 3. Apparitions ----------------------------------------------------
    spawnAccumulator_ += dt;
    if (spawnAccumulator_ >= params_.spawnInterval) {
        spawnAccumulator_ = 0.0f;
        spawnWave(playerPosition, habitat);
    }
}

std::size_t Wildlife::countWithin(const glm::vec3& position, float radius) const {
    return static_cast<std::size_t>(
        std::count_if(animals_.begin(), animals_.end(), [&](const Animal& animal) {
            return horizontalDistance(animal.position, position) <= radius;
        }));
}

std::size_t Wildlife::fleeingCount() const {
    return static_cast<std::size_t>(
        std::count_if(animals_.begin(), animals_.end(), [](const Animal& animal) {
            return animal.state == AnimalState::Fleeing;
        }));
}

// ---------------------------------------------------------------------------
// PHASE 20 : collision. Le joueur ne traverse plus un animal.
// ---------------------------------------------------------------------------
// Rien d'original ici, et c'est l'intention : la geometrie est celle de
// Forest::resolve (cercles dans le plan XZ, recouvrement d'intervalles en Y,
// repoussement jusqu'au contact) parce que le monde doit se cogner PARTOUT de la
// meme facon. Ecrire un second modele de collision pour la faune aurait donne
// deux comportements a regler, a tester et a garder d'accord.
//
// Une seule chose distingue vraiment un animal d'un tronc, et elle ne se voit
// pas dans ce code : il BOUGE. Consequences assumees, toutes les deux :
//   - un animal peut venir se poser SUR le joueur immobile (le joueur ne bouge
//     pas, donc aucun moveBody() n'est appele, donc rien ne l'ecarte). Il sera
//     repousse au premier pas : c'est exactement le cas degenere que
//     moveBody() traite deja en dernier recours pour "un arbre plante sur le
//     joueur" ;
//   - la position "precedente" sur laquelle moveBody() se replie a pu devenir
//     occupee entre deux images. Le repli le teste, il n'y a donc rien de
//     special a faire - le repoussement reprend la main.
namespace {

// Le rayon par defaut est la DEMI-LONGUEUR du cervide, et ce lien doit tenir
// tout seul : WildlifeParams ne peut pas inclure AnimalMesh.hpp (elle serait
// alors trainee par tout ce qui parametre la faune), donc la valeur y est
// ecrite en clair. Cette assertion est ce qui empeche les deux de diverger le
// jour ou le mesh changera de taille - elle echoue a la COMPILATION.
static_assert(WildlifeParams{}.colliderRadius == 0.5f * kAnimalLength,
              "Le rayon de collision par defaut doit rester la demi-longueur "
              "du mesh de cervide (kAnimalLength).");

// Un animal mord-il la bande verticale du corps ? Meme forme que
// Forest::blocksBody. animal.position est aux SABOTS (voir AnimalMesh.hpp).
bool blocksBody(const Animal& animal, float height, const BodyCylinder& body) {
    return body.feetY <= animal.position.y + height && body.headY >= animal.position.y;
}

} // namespace

bool Wildlife::overlaps(const BodyCylinder& body) const {
    if (!params_.solid) {
        return false;
    }

    for (const Animal& animal : animals_) {
        const float height = kAnimalHeight * animal.scale;
        if (!blocksBody(animal, height, body)) {
            continue;
        }

        const float reach = colliderRadiusOf(animal) + body.radius;
        const float dx = body.position.x - animal.position.x;
        const float dz = body.position.z - animal.position.z;

        // Meme marge de tolerance, et pour la meme raison, que Forest::overlaps
        // et EditorScene::overlaps : resolve() repousse EXACTEMENT au contact,
        // et un test strict sur la meme valeur declarerait le joueur bloque sur
        // la sortie de resolve(). moveBody() refuserait alors tout deplacement
        // le long d'un animal au lieu de laisser glisser.
        constexpr float kContactEpsilon = 1e-3f;
        if (dx * dx + dz * dz < (reach - kContactEpsilon) * (reach - kContactEpsilon)) {
            return true;
        }
    }
    return false;
}

glm::vec3 Wildlife::resolve(const BodyCylinder& body) const {
    if (!params_.solid) {
        return body.position;
    }

    glm::vec3 corrected = body.position;

    // Meme nombre de passes que Forest, meme sortie anticipee : le cas courant
    // (aucun animal au contact) ne coute qu'un balayage.
    constexpr int kCollisionPasses = 4;
    for (int pass = 0; pass < kCollisionPasses; ++pass) {
        bool touched = false;

        for (const Animal& animal : animals_) {
            const float height = kAnimalHeight * animal.scale;
            // Bande verticale evaluee sur le corps SOUMIS, comme Forest : la
            // resolution ne deplace jamais qu'a l'horizontale, donc la bande ne
            // peut pas changer d'une passe a l'autre.
            if (!blocksBody(animal, height, body)) {
                continue;
            }

            const float reach = colliderRadiusOf(animal) + body.radius;
            const float dx = corrected.x - animal.position.x;
            const float dz = corrected.z - animal.position.z;
            const float distanceSq = dx * dx + dz * dz;

            if (distanceSq >= reach * reach) {
                continue;
            }

            const float distance = std::sqrt(distanceSq);
            // Cas degenere : le joueur est exactement sur l'axe de l'animal.
            // Direction arbitraire mais DETERMINISTE, plutot qu'une division
            // par zero qui propagerait un NaN jusqu'a la camera.
            const glm::vec2 outward = (distance > 1e-4f)
                ? glm::vec2(dx / distance, dz / distance)
                : glm::vec2(1.0f, 0.0f);

            corrected.x = animal.position.x + outward.x * reach;
            corrected.z = animal.position.z + outward.y * reach;
            touched = true;
        }

        if (!touched) {
            break;
        }
    }

    return corrected;
}

} // namespace vercora
