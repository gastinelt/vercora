#include "world/Resin.hpp"

#include <algorithm>

namespace vercora {

const char* resinResultMessage(ResinResult result) {
    switch (result) {
        case ResinResult::Success:
            return "Resine : l'entaille pleure, une perle tombe au pied de l'arbre. "
                   "L'arbre reste debout.";
        case ResinResult::NotConifer:
            return "Resine : cet arbre n'est pas un resineux, l'entaille ne donne rien.";
        case ResinResult::StillDry:
            return "Resine : cet arbre a deja ete saigne, il faut lui laisser le "
                   "temps de couler a nouveau.";
        case ResinResult::NoTree:
            return "Resine : aucun arbre en vue a saigner.";
    }
    return "Resine : issue inconnue.";
}

ResinStand::ResinStand(const ResinParams& params) : params_(params) {}

ResinResult ResinStand::tap(int treeIndex, TreeSpecies species, float now) {
    if (treeIndex < 0) {
        return ResinResult::NoTree;
    }
    // L'essence est DONNEE, plus decidee ici : voir l'en-tete de Resin.hpp.
    if (species != TreeSpecies::Conifer) {
        return ResinResult::NotConifer;
    }

    const auto found = taps_.find(treeIndex);
    if (found != taps_.end()) {
        const Tap& tap = found->second;
        if (params_.maxTapsPerTree != 0 && tap.count >= params_.maxTapsPerTree) {
            return ResinResult::StillDry;
        }
        if (now - tap.lastTapTime < params_.flowDelay) {
            return ResinResult::StillDry;
        }
    }

    // L'arbre n'entre dans la memoire QU'ICI, c'est-a-dire au premier
    // prelevement reel. Viser mille arbres sans en saigner un seul laisse la
    // table vide - meme principe que la grille creuse de world/Habitat.
    Tap& tap = taps_[treeIndex];
    tap.count += 1;
    tap.lastTapTime = now;
    return ResinResult::Success;
}

uint32_t ResinStand::tapCount(int treeIndex) const {
    const auto found = taps_.find(treeIndex);
    return (found == taps_.end()) ? 0u : found->second.count;
}

float ResinStand::secondsUntilFlow(int treeIndex, float now) const {
    const auto found = taps_.find(treeIndex);
    if (found == taps_.end()) {
        return 0.0f;
    }
    const Tap& tap = found->second;
    if (params_.maxTapsPerTree != 0 && tap.count >= params_.maxTapsPerTree) {
        return 0.0f; // epuise : ce n'est plus une question de delai
    }
    return std::max(0.0f, params_.flowDelay - (now - tap.lastTapTime));
}

} // namespace vercora
