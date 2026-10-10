#include "editor/EditorScene.hpp"

#include "editor/AssetCatalog.hpp"

#include <algorithm>
#include <cmath>

namespace vercora {

const PlacedObject* EditorScene::find(uint32_t id) const {
    const int index = indexOf(id);
    return index < 0 ? nullptr : &objects_[static_cast<std::size_t>(index)];
}

int EditorScene::indexOf(uint32_t id) const {
    if (id == kNoPlacedObject) {
        return -1;
    }
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        if (objects_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void EditorScene::pushAction(Action action) {
    undoStack_.push_back(std::move(action));
    if (undoStack_.size() > kMaxUndoDepth) {
        // On jette la PLUS ANCIENNE, pas la plus recente : une pile bornee doit
        // continuer d'annuler ce qu'on vient de faire.
        undoStack_.erase(undoStack_.begin());
    }
}

uint32_t EditorScene::place(PlacedObject object) {
    object.id = nextId_++;
    const uint32_t id = object.id;

    Action action;
    action.kind = ActionKind::Place;
    action.before = object; // sert au libelle, pas a la restauration
    action.index = objects_.size();
    pushAction(std::move(action));

    objects_.push_back(std::move(object));
    return id;
}

bool EditorScene::erase(uint32_t id) {
    const int index = indexOf(id);
    if (index < 0) {
        return false;
    }
    const auto position = static_cast<std::size_t>(index);

    Action action;
    action.kind = ActionKind::Erase;
    action.before = objects_[position];
    action.index = position;
    pushAction(std::move(action));

    objects_.erase(objects_.begin() + index);
    return true;
}

bool EditorScene::transform(uint32_t id, const PlacedObject& updated, float now) {
    const int index = indexOf(id);
    if (index < 0) {
        return false;
    }
    PlacedObject& target = objects_[static_cast<std::size_t>(index)];

    // Regroupement : si la derniere action est deja une transformation du MEME
    // objet, assez recente, on ne l'empile pas une seconde fois - on prolonge la
    // precedente, dont le `before` reste l'etat d'avant la manipulation
    // complete. Voir l'en-tete du header.
    const bool grouped = !undoStack_.empty() &&
                         undoStack_.back().kind == ActionKind::Transform &&
                         undoStack_.back().before.id == id &&
                         (now - undoStack_.back().lastTouch) <= transformGroupSeconds_;

    if (grouped) {
        undoStack_.back().lastTouch = now;
    } else {
        Action action;
        action.kind = ActionKind::Transform;
        action.before = target;
        action.index = static_cast<std::size_t>(index);
        action.lastTouch = now;
        pushAction(std::move(action));
    }

    // L'identite et l'asset NE CHANGENT JAMAIS par cette voie : `updated` est
    // fourni par l'appelant, qui a pu le construire a partir d'une copie
    // maladroite. Les reimposer ici rend l'invariant impossible a violer depuis
    // l'exterieur.
    const std::string asset = target.asset;
    target = updated;
    target.id = id;
    target.asset = asset;
    if (!(target.scale > 0.0f) || !std::isfinite(target.scale)) {
        target.scale = 1.0f;
    }
    return true;
}

bool EditorScene::undo() {
    if (undoStack_.empty()) {
        return false;
    }
    const Action action = undoStack_.back();
    undoStack_.pop_back();

    switch (action.kind) {
        case ActionKind::Place: {
            const int index = indexOf(action.before.id);
            if (index >= 0) {
                objects_.erase(objects_.begin() + index);
            }
            return true;
        }
        case ActionKind::Erase: {
            // Reinsertion a sa place d'origine. L'indice peut avoir depasse la
            // taille courante si d'autres suppressions ont eu lieu depuis ; on
            // ecrete plutot que de refuser l'annulation.
            const std::size_t index = std::min(action.index, objects_.size());
            objects_.insert(objects_.begin() + static_cast<std::ptrdiff_t>(index),
                            action.before);
            return true;
        }
        case ActionKind::Transform: {
            const int index = indexOf(action.before.id);
            if (index < 0) {
                // L'objet a ete supprime depuis. Rien a restaurer : la
                // suppression, elle, a sa propre entree plus bas dans la pile.
                return true;
            }
            objects_[static_cast<std::size_t>(index)] = action.before;
            return true;
        }
    }
    return false;
}

std::string EditorScene::lastActionLabel() const {
    if (undoStack_.empty()) {
        return {};
    }
    const Action& action = undoStack_.back();
    const std::string& asset = action.before.asset;
    switch (action.kind) {
        case ActionKind::Place:     return "pose de \"" + asset + "\"";
        case ActionKind::Erase:     return "suppression de \"" + asset + "\"";
        case ActionKind::Transform: return "reglage de \"" + asset + "\"";
    }
    return {};
}

void EditorScene::replaceAll(std::vector<PlacedObject> objects) {
    objects_ = std::move(objects);
    undoStack_.clear();
    // Renumerotation complete : les identifiants sont une notion de SESSION, le
    // fichier n'en porte pas (voir SceneFile.hpp). Repartir de 1 garantit qu'ils
    // sont uniques et croissants dans l'ordre du fichier, donc lisibles.
    nextId_ = 1;
    for (PlacedObject& object : objects_) {
        object.id = nextId_++;
    }
}

bool EditorScene::footprint(const PlacedObject& object, float* radius,
                            float* height) const {
    if (catalog_ == nullptr) {
        return false;
    }
    const int index = catalog_->indexOf(object.asset);
    if (index < 0) {
        return false;
    }
    const AssetEntry& entry = catalog_->entries()[static_cast<std::size_t>(index)];
    if (!(entry.collisionRadius > 0.0f) || !(entry.height > 0.0f)) {
        return false;
    }
    if (radius) { *radius = entry.collisionRadius * object.scale; }
    if (height) { *height = entry.height * object.scale; }
    return true;
}

uint32_t EditorScene::pick(const glm::vec3& origin, const glm::vec3& direction,
                           float maxDistance) const {
    uint32_t closestId = kNoPlacedObject;
    float closestT = maxDistance;

    for (const PlacedObject& object : objects_) {
        float radius = 0.0f;
        float height = 0.0f;
        if (!footprint(object, &radius, &height)) {
            // Asset inconnu : l'objet n'est pas dessine non plus (voir main.cpp),
            // il ne serait pas honnete de le rendre selectionnable a l'aveugle.
            continue;
        }
        // La VISEE est plus genereuse que la collision : on veut pouvoir
        // reprendre un poteau de 9 cm de large a dix metres sans devoir poser le
        // curseur au pixel. 0,35 m, c'est le rayon du corps du joueur - donc
        // "gros comme un joueur", ce qui reste discriminant entre deux objets
        // voisins.
        const float pickRadius = std::max(radius, 0.35f);

        const glm::vec2 d2(direction.x, direction.z);
        const glm::vec2 o2(origin.x - object.position.x, origin.z - object.position.z);

        const float a = glm::dot(d2, d2);
        if (a < 1e-6f) {
            continue; // rayon quasi vertical : voir Forest::pickTree
        }
        const float b = 2.0f * glm::dot(o2, d2);
        const float c = glm::dot(o2, o2) - pickRadius * pickRadius;
        const float discriminant = b * b - 4.0f * a * c;
        if (discriminant < 0.0f) {
            continue;
        }

        const float sqrtDisc = std::sqrt(discriminant);
        const float roots[2] = { (-b - sqrtDisc) / (2.0f * a),
                                 (-b + sqrtDisc) / (2.0f * a) };
        for (const float t : roots) {
            if (t <= 0.05f || t >= closestT) {
                continue;
            }
            const float y = origin.y + direction.y * t;
            if (y < object.position.y || y > object.position.y + height) {
                continue;
            }
            closestT = t;
            closestId = object.id;
            break;
        }
    }
    return closestId;
}

glm::vec3 EditorScene::resolve(const BodyCylinder& body) const {
    glm::vec3 position = body.position;

    for (const PlacedObject& object : objects_) {
        if (!object.solid) {
            continue;
        }
        float radius = 0.0f;
        float height = 0.0f;
        if (!footprint(object, &radius, &height)) {
            continue;
        }
        // Recouvrement vertical : un objet entierement sous les pieds ou
        // au-dessus de la tete ne bloque pas. Meme regle que pour les souches
        // depuis la Phase 6.
        if (object.position.y + height < body.feetY || object.position.y > body.headY) {
            continue;
        }

        const float minDistance = radius + body.radius;
        const glm::vec2 offset(position.x - object.position.x,
                               position.z - object.position.z);
        const float distanceSquared = glm::dot(offset, offset);
        if (distanceSquared >= minDistance * minDistance) {
            continue;
        }

        const float distance = std::sqrt(distanceSquared);
        // Exactement au centre : aucune direction de repoussement n'existe. On
        // en choisit une arbitraire mais DETERMINISTE (+X), sinon la position
        // rendue dependrait du bruit numerique.
        const glm::vec2 push = (distance > 1e-4f) ? offset / distance
                                                  : glm::vec2(1.0f, 0.0f);
        position.x = object.position.x + push.x * minDistance;
        position.z = object.position.z + push.y * minDistance;
    }
    return position;
}

bool EditorScene::overlaps(const BodyCylinder& body) const {
    for (const PlacedObject& object : objects_) {
        if (!object.solid) {
            continue;
        }
        float radius = 0.0f;
        float height = 0.0f;
        if (!footprint(object, &radius, &height)) {
            continue;
        }
        if (object.position.y + height < body.feetY || object.position.y > body.headY) {
            continue;
        }
        const float minDistance = radius + body.radius;
        const glm::vec2 offset(body.position.x - object.position.x,
                               body.position.z - object.position.z);
        // Marge d'un dixieme de millimetre : resolve() repousse EXACTEMENT a
        // minDistance, et un test strict sur la meme valeur rendrait "encore
        // dedans" a cause de l'arrondi flottant - donc moveBody() refuserait
        // tout deplacement le long d'un rocher au lieu de laisser glisser.
        if (glm::dot(offset, offset) < (minDistance - 1e-4f) * (minDistance - 1e-4f)) {
            return true;
        }
    }
    return false;
}

std::size_t EditorScene::solidCount() const {
    std::size_t count = 0;
    for (const PlacedObject& object : objects_) {
        if (object.solid) {
            ++count;
        }
    }
    return count;
}

} // namespace vercora
