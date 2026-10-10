#pragma once

#include "world/GroundItem.hpp"

#include <array>
#include <string>

namespace vercora {

// Ce que le joueur porte. Volontairement reduit a des COMPTEURS : la Phase 6 a
// besoin de savoir "combien de pierres ai-je" pour autoriser la percussion, pas
// de modeliser des emplacements, des masses ou de l'encombrement.
//
// Aucun rendu, aucun menu : conformement a la philosophie "zero menu virtuel" du
// jeu, l'inventaire ne s'affiche que dans le titre de la fenetre et les logs. Le
// jour ou le portage a dos sera un vrai sujet, c'est cette classe qui grossira,
// sans que le reste du monde ait a changer.
class Inventory {
public:
    void add(GroundItemKind kind, int quantity = 1) {
        counts_[static_cast<std::size_t>(kind)] += quantity;
    }

    int count(GroundItemKind kind) const {
        return counts_[static_cast<std::size_t>(kind)];
    }

    // Retire quantity exemplaires. Renvoie false et ne touche a RIEN si le
    // joueur n'en a pas assez : un retrait partiel silencieux serait la
    // meilleure facon de faire disparaitre de la matiere sans trace.
    bool remove(GroundItemKind kind, int quantity = 1) {
        const auto slot = static_cast<std::size_t>(kind);
        if (quantity <= 0 || counts_[slot] < quantity) {
            return false;
        }
        counts_[slot] -= quantity;
        return true;
    }

    int total() const {
        int sum = 0;
        for (const int value : counts_) {
            sum += value;
        }
        return sum;
    }

    // Resume compact, du type "buches 3 | branches 2 | pierres 1 | eclats 0".
    //
    // PHASE 28 : les huit MATIERES (Log..Fiber) sont toujours affichees, meme a
    // zero, comme avant ; les objets FACONNES (Cord et suivants) ne le sont que
    // s'ils sont portes, pour ne pas allonger le titre de la fenetre de cinq
    // compteurs a zero.
    std::string summary() const {
        std::string text;
        for (uint32_t i = 0; i < kGroundItemKindCount; ++i) {
            const auto kind = static_cast<GroundItemKind>(i);
            if (i >= static_cast<uint32_t>(GroundItemKind::Cord) && counts_[i] == 0) {
                continue;
            }
            if (!text.empty()) {
                text += " | ";
            }
            text += groundItemLabelPlural(kind);
            text += " ";
            text += std::to_string(counts_[i]);
        }
        return text;
    }

private:
    std::array<int, kGroundItemKindCount> counts_{};
};

} // namespace vercora
