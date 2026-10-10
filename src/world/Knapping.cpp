#include "world/Knapping.hpp"

#include "world/Ground.hpp"
#include "world/GroundItem.hpp"
#include "world/Inventory.hpp"

#include <cmath>

namespace vercora {
namespace {

// A quelle distance devant le joueur l'eclat retombe. Assez loin pour ne pas
// etre cache par le corps du joueur, assez pres pour etre ramasse sans bouger.
constexpr float kDropDistance = 1.1f;

// Repli quand le joueur regarde exactement a la verticale : la projection
// horizontale du regard est alors nulle et ne donne aucune direction.
constexpr glm::vec3 kFallbackFacing{ 1.0f, 0.0f, 0.0f };

} // namespace

const char* knapResultMessage(KnapResult result) {
    switch (result) {
        case KnapResult::Success:
            return "Percussion : un eclat tranchant se detache et tombe a vos pieds.";
        case KnapResult::NotEnoughStones:
            return "Percussion impossible : il faut deux pierres, un percuteur et un nucleus.";
    }
    return "Percussion : issue inconnue.";
}

KnapResult knapStones(Inventory& inventory, Ground& ground,
                      const glm::vec3& playerPosition, const glm::vec3& facing) {
    if (inventory.count(GroundItemKind::Stone) < 2) {
        return KnapResult::NotEnoughStones;
    }

    // Direction horizontale du regard. On teste la longueur AVANT de normaliser :
    // normaliser un vecteur nul propagerait des NaN jusque dans la position de
    // l'objet depose, qui disparaitrait alors du monde sans le moindre message.
    glm::vec3 flat(facing.x, 0.0f, facing.z);
    const float flatLength = std::sqrt(flat.x * flat.x + flat.z * flat.z);
    flat = (flatLength > 1e-4f) ? flat / flatLength : kFallbackFacing;

    // Le nucleus est consomme, le percuteur reste. Le retrait est teste : si
    // l'inventaire refusait, on ne voudrait surtout pas creer l'eclat quand meme.
    if (!inventory.remove(GroundItemKind::Stone, 1)) {
        return KnapResult::NotEnoughStones;
    }

    const float dropX = playerPosition.x + flat.x * kDropDistance;
    const float dropZ = playerPosition.z + flat.z * kDropDistance;

    // L'eclat se pose a plat, aligne sur la direction du regard : meme convention
    // de yaw que la buche (mesh couche le long de +X, cf. GroundItem).
    const float yaw = std::atan2(-flat.z, flat.x);
    ground.drop(GroundItemKind::Flake, dropX, dropZ, yaw, /*roll=*/0.0f);

    return KnapResult::Success;
}

} // namespace vercora
