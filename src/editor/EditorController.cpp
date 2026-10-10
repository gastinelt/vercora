#include "editor/EditorController.hpp"

#include "core/HeightSampler.hpp"
#include "core/Logger.hpp"
#include "core/TerrainRay.hpp"
#include "editor/AssetCatalog.hpp"
#include "editor/EditorScene.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace vercora {

EditorController::EditorController(const AssetCatalog& catalog, EditorScene& scene,
                                   const EditorControllerParams& params)
    : catalog_(catalog), scene_(scene), params_(params) {
    if (!catalog_.empty()) {
        brushIndex_ = 0;
    }
}

void EditorController::selectBrush(int index) {
    if (catalog_.empty()) {
        brushIndex_ = -1;
        return;
    }
    const int count = static_cast<int>(catalog_.size());
    // Modulo POSITIF : reculer depuis le premier asset doit ramener au dernier,
    // et l'operateur % du C++ rendrait un indice negatif.
    brushIndex_ = ((index % count) + count) % count;
    page_ = brushIndex_ / kPageSize;
    statusChanged_ = true;
}

void EditorController::applyAdjustments(const EditorInput& input, PlacedObject& target,
                                        float* liftOut) const {
    const float divider = input.holdFineStep ? std::max(1.0f, params_.fineStepDivider)
                                             : 1.0f;
    const float dt = input.deltaTime;

    const float rotate = (input.holdRotateRight ? 1.0f : 0.0f) -
                         (input.holdRotateLeft ? 1.0f : 0.0f);
    if (rotate != 0.0f) {
        target.yawDegrees += rotate * params_.rotateDegreesPerSecond * dt / divider;
        // Repli dans [-180, 180] : la valeur part dans le fichier de scene, et
        // un cap de 3 240 degres y serait juste illisible.
        while (target.yawDegrees > 180.0f)  { target.yawDegrees -= 360.0f; }
        while (target.yawDegrees < -180.0f) { target.yawDegrees += 360.0f; }
    }

    const float scaleInput = (input.holdScaleUp ? 1.0f : 0.0f) -
                             (input.holdScaleDown ? 1.0f : 0.0f);
    if (scaleInput != 0.0f) {
        // Reglage MULTIPLICATIF et non additif : passer de 0,2 a 0,3 et de 10 a
        // 15 sont le meme geste a l'oeil, alors qu'un pas additif rendrait le
        // premier impossible a doser et le second interminable.
        const float rate = std::pow(params_.scalePerSecond, scaleInput * dt / divider);
        target.scale = std::clamp(target.scale * rate, params_.minScale, params_.maxScale);
    }

    if (liftOut != nullptr) {
        const float lift = (input.holdRaise ? 1.0f : 0.0f) -
                           (input.holdLower ? 1.0f : 0.0f);
        if (lift != 0.0f) {
            *liftOut = std::clamp(
                *liftOut + lift * params_.liftMetresPerSecond * dt / divider,
                params_.minLift, params_.maxLift);
        }
    }
}

void EditorController::update(const EditorInput& input, const HeightSampler& terrain,
                              float halfExtent) {
    hasPreview_ = false;

    // --- Catalogue ----------------------------------------------------------
    if (input.nextAssetPressed) { selectBrush(brushIndex_ + 1); }
    if (input.prevAssetPressed) { selectBrush(brushIndex_ - 1); }
    if (input.quickPick >= 0) {
        const int index = page_ * kPageSize + input.quickPick;
        if (index < static_cast<int>(catalog_.size())) {
            selectBrush(index);
        } else {
            logInfo("Editeur : rien au rang " + std::to_string(input.quickPick + 1) +
                    " de la page " + std::to_string(page_ + 1) + ".");
        }
    }

    if (input.listPressed)   { listRequested_ = true; }
    if (input.savePressed)   { saveRequested_ = true; }
    if (input.reloadPressed) { reloadRequested_ = true; }

    if (input.undoPressed) {
        // Une annulation pendant une saisie serait ambigue (annule-t-on la
        // saisie ou l'action d'avant ?). On repose d'abord, sans rien empiler :
        // l'objet retourne exactement d'ou il vient.
        if (grabbedId_ != kNoPlacedObject) {
            grabbedId_ = kNoPlacedObject;
            logInfo("Editeur : saisie abandonnee avant l'annulation.");
        }
        const std::string label = scene_.lastActionLabel();
        if (scene_.undo()) {
            logInfo("Editeur : annule (" + label + "). Reste " +
                    std::to_string(scene_.undoDepth()) + " annulation(s), " +
                    std::to_string(scene_.size()) + " objet(s).");
            if (scene_.find(selectedId_) == nullptr) {
                selectedId_ = kNoPlacedObject;
            }
        } else {
            logInfo("Editeur : rien a annuler.");
        }
        statusChanged_ = true;
    }

    if (input.cancelPressed) {
        if (grabbedId_ != kNoPlacedObject) {
            grabbedId_ = kNoPlacedObject;
            logInfo("Editeur : saisie abandonnee, l'objet reste ou il etait.");
        } else if (selectedId_ != kNoPlacedObject) {
            selectedId_ = kNoPlacedObject;
            logInfo("Editeur : selection abandonnee.");
        }
        statusChanged_ = true;
    }

    // --- Visee --------------------------------------------------------------
    const glm::vec3 direction = glm::length(input.forward) > 1e-6f
                                    ? glm::normalize(input.forward)
                                    : glm::vec3(0.0f, 0.0f, -1.0f);
    const TerrainRayHit aim =
        raycastTerrain(terrain, input.eye, direction, params_.aimReach, halfExtent);

    // --- Selection ----------------------------------------------------------
    if (input.pickPressed && grabbedId_ == kNoPlacedObject) {
        // La portee de selection s'arrete AU TERRAIN quand celui-ci est touche :
        // sans ca, viser le sol au pied d'un rocher selectionnerait un objet
        // situe derriere la colline. `aimReach` sinon.
        const float reach = aim.hit ? aim.distance + 1.0f : params_.aimReach;
        const uint32_t picked = scene_.pick(input.eye, direction, reach);
        if (picked != kNoPlacedObject) {
            selectedId_ = picked;
            const PlacedObject* object = scene_.find(picked);
            logInfo("Editeur : selection de l'objet " + std::to_string(picked) + " (" +
                    (object ? object->asset : std::string("?")) + ").");
        } else {
            selectedId_ = kNoPlacedObject;
            logInfo("Editeur : aucun objet dans la ligne de visee, selection videe.");
        }
        statusChanged_ = true;
    }

    // --- Cycle parmi les objets poses (F3/F4) --------------------------------
    // Meme garde que la selection a la visee : pas pendant une saisie, sinon on
    // changerait l'objet EN MAIN sous les pieds du joueur. Contrairement a
    // pickPressed, celle-ci ne regarde jamais le terrain - c'est tout son
    // interet : retrouver un objet pose hors du champ de visee actuel sans
    // revenir dessus a la souris.
    if ((input.cycleNextPressed || input.cyclePrevPressed) &&
        grabbedId_ == kNoPlacedObject) {
        const auto& objects = scene_.objects();
        if (objects.empty()) {
            selectedId_ = kNoPlacedObject;
            logInfo("Editeur : aucun objet pose a selectionner.");
        } else {
            const int count = static_cast<int>(objects.size());
            const int current = scene_.indexOf(selectedId_);
            const int base = (current >= 0) ? current : 0;
            // Rien de selectionne + "suivant" pointe sur le premier objet ;
            // rien de selectionne + "precedent" sur le dernier - le meme choix
            // que selectBrush(-1) depuis l'indice 0.
            const int step = (current < 0) ? (input.cyclePrevPressed ? -1 : 0)
                                           : (input.cycleNextPressed ? 1 : -1);
            const int index = ((base + step) % count + count) % count;
            selectedId_ = objects[static_cast<std::size_t>(index)].id;
            logInfo("Editeur : selection de l'objet " + std::to_string(selectedId_) +
                    " (" + objects[static_cast<std::size_t>(index)].asset +
                    ") [F3/F4].");
        }
        statusChanged_ = true;
    }

    // --- Saisie : DEBUT seulement -------------------------------------------
    //
    // La REPOSE est traitee plus bas, APRES le calcul de la previsualisation de
    // cette image. Les deux etaient ensemble ici au depart, et c'etait un
    // defaut : reposer a la touche de saisie ecrivait la previsualisation de
    // l'image PRECEDENTE, alors que reposer au clic - traite plus bas -
    // ecrivait celle de l'image courante. Deux gestes censes faire la meme
    // chose n'avaient donc pas exactement le meme resultat.
    bool grabStartedThisFrame = false;
    if (input.grabPressed && grabbedId_ == kNoPlacedObject) {
        if (selectedId_ != kNoPlacedObject) {
            const PlacedObject* object = scene_.find(selectedId_);
            if (object != nullptr) {
                grabbedId_ = selectedId_;
                grabState_ = *object;
                grabLift_ = object->position.y -
                            terrain.heightAt(object->position.x, object->position.z);
                grabStartedThisFrame = true;
                logInfo("Editeur : objet " + std::to_string(grabbedId_) +
                        " en main, il suit la visee.");
                statusChanged_ = true;
            }
        } else {
            logInfo("Editeur : rien de selectionne a saisir.");
        }
    }

    // --- Reglages : saisie > selection > pinceau ----------------------------
    if (grabbedId_ != kNoPlacedObject) {
        // Les reglages portent sur `grabState_`, une COPIE : l'objet stocke dans
        // la scene ne bouge pas tant qu'il est en main. C'est ce qui garantit
        // que l'unique annulation empilee a la repose retienne bien l'etat
        // d'avant la saisie, rotation comprise.
        applyAdjustments(input, grabState_, &grabLift_);
        if (input.toggleSolidPressed) {
            grabState_.solid = !grabState_.solid;
            logInfo(std::string("Editeur : objet en main rendu ") +
                    (grabState_.solid ? "SOLIDE." : "TRAVERSABLE."));
            statusChanged_ = true;
        }
    } else if (selectedId_ != kNoPlacedObject) {
        const PlacedObject* current = scene_.find(selectedId_);
        if (current == nullptr) {
            selectedId_ = kNoPlacedObject;
        } else {
            PlacedObject updated = *current;
            float lift = updated.position.y -
                         terrain.heightAt(updated.position.x, updated.position.z);
            const float beforeYaw = updated.yawDegrees;
            const float beforeScale = updated.scale;
            const float beforeLift = lift;

            applyAdjustments(input, updated, &lift);
            bool changed = (updated.yawDegrees != beforeYaw) ||
                           (updated.scale != beforeScale) || (lift != beforeLift);

            if (input.toggleSolidPressed) {
                updated.solid = !updated.solid;
                changed = true;
                logInfo(std::string("Editeur : objet ") + std::to_string(selectedId_) +
                        (updated.solid ? " rendu SOLIDE." : " rendu TRAVERSABLE."));
            }

            if (changed) {
                updated.position.y =
                    terrain.heightAt(updated.position.x, updated.position.z) + lift;
                scene_.transform(selectedId_, updated, input.now);
                statusChanged_ = true;
            }
        }
    } else {
        PlacedObject brush;
        brush.yawDegrees = brushYaw_;
        brush.scale = brushScale_;
        const float beforeYaw = brushYaw_;
        const float beforeScale = brushScale_;
        const float beforeLift = brushLift_;
        applyAdjustments(input, brush, &brushLift_);
        brushYaw_ = brush.yawDegrees;
        brushScale_ = brush.scale;
        if (input.toggleSolidPressed) {
            brushSolid_ = !brushSolid_;
            logInfo(std::string("Editeur : le pinceau posera des objets ") +
                    (brushSolid_ ? "SOLIDES." : "TRAVERSABLES."));
            statusChanged_ = true;
        }
        if (brushYaw_ != beforeYaw || brushScale_ != beforeScale ||
            brushLift_ != beforeLift) {
            statusChanged_ = true;
        }
    }

    // --- Previsualisation ---------------------------------------------------
    if (grabbedId_ != kNoPlacedObject) {
        preview_ = grabState_;
        preview_.id = grabbedId_;
        if (aim.hit) {
            preview_.position.x = aim.position.x;
            preview_.position.z = aim.position.z;
        }
        preview_.position.y =
            terrain.heightAt(preview_.position.x, preview_.position.z) + grabLift_;
        hasPreview_ = true;
    } else if (aim.hit && brushIndex_ >= 0) {
        preview_ = PlacedObject{};
        preview_.id = kNoPlacedObject;
        preview_.asset = catalog_.entries()[static_cast<std::size_t>(brushIndex_)].id;
        preview_.position = { aim.position.x,
                              aim.position.y + brushLift_,
                              aim.position.z };
        preview_.yawDegrees = brushYaw_;
        preview_.scale = brushScale_;
        preview_.solid = brushSolid_;
        hasPreview_ = true;
    }

    // --- Repose et pose -----------------------------------------------------
    // Un objet en main se repose AUSSI BIEN a la touche de saisie qu'a celle de
    // pose : c'est le geste naturel dans les deux cas, et ca evite d'avoir a se
    // souvenir laquelle des deux termine un deplacement. Les deux passent
    // desormais par le meme code et ecrivent la MEME previsualisation, celle de
    // l'image courante, calculee juste au-dessus.
    const bool dropRequested = grabbedId_ != kNoPlacedObject &&
                               ((input.grabPressed && !grabStartedThisFrame) ||
                                input.placePressed);
    if (dropRequested) {
        const uint32_t id = grabbedId_;
        grabbedId_ = kNoPlacedObject;
        PlacedObject dropped = preview_;
        dropped.id = id;
        // UNE seule ecriture dans la scene pour tout le deplacement, donc UNE
        // seule annulation - dont le `before` est l'etat d'avant la saisie,
        // puisque rien n'a touche l'objet stocke entre-temps.
        scene_.transform(id, dropped, input.now);
        logInfo("Editeur : objet " + std::to_string(id) + " repose.");
        statusChanged_ = true;
    } else if (input.placePressed) {
        if (!hasPreview_) {
            logInfo("Editeur : la visee ne touche pas le terrain, rien a poser.");
        } else if (brushIndex_ < 0) {
            logInfo("Editeur : catalogue vide, rien a poser.");
        } else {
            const uint32_t id = scene_.place(preview_);
            // L'objet pose n'est VOLONTAIREMENT PAS selectionne.
            //
            // C'etait le cas dans la premiere version, par analogie avec les
            // editeurs 3D, et un essai sous Vulkan a montre que c'etait le
            // mauvais choix ici : les reglages visent la selection en priorite
            // (SAISIE > SELECTION > PINCEAU), donc poser un objet detournait
            // silencieusement les fleches de rotation du PINCEAU vers l'objet
            // qu'on venait de poser. Regler le pinceau puis poser dix blocs
            // alignes - le geste le plus courant de l'outil - devenait
            // impossible sans deselectionner entre chaque.
            //
            // Pour retoucher ce qu'on vient de poser, il suffit de le viser et
            // d'appuyer sur la touche de selection.
            logInfo("Editeur : pose de \"" + preview_.asset + "\" (objet " +
                    std::to_string(id) + ") a " + std::to_string(aim.distance) +
                    " m, cap " + std::to_string(preview_.yawDegrees) +
                    " deg, echelle " + std::to_string(preview_.scale) +
                    (preview_.solid ? ", solide." : ", traversable."));
            statusChanged_ = true;
        }
    }

    // --- Suppression --------------------------------------------------------
    if (input.deletePressed) {
        if (grabbedId_ != kNoPlacedObject) {
            grabbedId_ = kNoPlacedObject; // on repose avant de supprimer
        }
        if (selectedId_ == kNoPlacedObject) {
            logInfo("Editeur : rien de selectionne a supprimer.");
        } else {
            const PlacedObject* object = scene_.find(selectedId_);
            const std::string asset = object ? object->asset : std::string("?");
            if (scene_.erase(selectedId_)) {
                logInfo("Editeur : objet " + std::to_string(selectedId_) + " (" + asset +
                        ") supprime. Reste " + std::to_string(scene_.size()) +
                        " objet(s).");
            }
            selectedId_ = kNoPlacedObject;
            statusChanged_ = true;
        }
    }
}

bool EditorController::consumeSaveRequest() {
    const bool requested = saveRequested_;
    saveRequested_ = false;
    return requested;
}

bool EditorController::consumeReloadRequest() {
    const bool requested = reloadRequested_;
    reloadRequested_ = false;
    return requested;
}

bool EditorController::consumeListRequest() {
    const bool requested = listRequested_;
    listRequested_ = false;
    return requested;
}

bool EditorController::consumeStatusChanged() {
    const std::string current = statusLine();
    const bool changed = statusChanged_ || (current != lastStatus_);
    statusChanged_ = false;
    lastStatus_ = current;
    return changed;
}

void EditorController::resetInteraction() {
    grabbedId_ = kNoPlacedObject;
    selectedId_ = kNoPlacedObject;
    hasPreview_ = false;
    statusChanged_ = true;
}

std::string EditorController::statusLine() const {
    std::ostringstream out;
    out << "Editeur : pinceau ";
    if (brushIndex_ >= 0) {
        out << '[' << (brushIndex_ + 1) << '/' << catalog_.size() << "] "
            << catalog_.entries()[static_cast<std::size_t>(brushIndex_)].id;
    } else {
        out << "(catalogue vide)";
    }
    out << ", cap " << brushYaw_ << " deg, echelle " << brushScale_
        << ", dz " << brushLift_ << " m, "
        << (brushSolid_ ? "solide" : "traversable");

    // La ligne d'etat doit montrer CE QUE LES REGLAGES VONT TOUCHER, et non
    // seulement le pinceau. Sans ca, tourner un objet selectionne affichait
    // "cap 0 deg" - celui du pinceau, inchange - pendant que l'objet tournait :
    // on ne pouvait pas lire a l'ecran ce qu'on etait en train de regler.
    const auto describe = [&out](const PlacedObject& object) {
        out << " (" << object.asset << ", cap " << object.yawDegrees
            << " deg, echelle " << object.scale << ", "
            << (object.solid ? "solide" : "traversable") << ')';
    };

    if (grabbedId_ != kNoPlacedObject) {
        out << " | EN MAIN : objet " << grabbedId_;
        describe(grabState_);
    } else if (selectedId_ != kNoPlacedObject) {
        out << " | selection : objet " << selectedId_;
        const PlacedObject* selected = scene_.find(selectedId_);
        if (selected != nullptr) {
            describe(*selected);
        }
    } else {
        out << " | aucune selection";
    }

    out << " | " << scene_.size() << " objet(s), " << scene_.solidCount()
        << " solide(s), " << scene_.undoDepth() << " annulation(s)";
    return out.str();
}

} // namespace vercora
