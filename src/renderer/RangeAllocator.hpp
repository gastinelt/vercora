#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace vercora {

// PHASE 32. Gestion des plages libres d'UN bloc de memoire : premier
// ajustement, alignement, fusion des voisins a la liberation.
//
// C'est toute la logique de sous-allocation de GpuBufferPool, isolee ici sans
// la moindre dependance Vulkan pour etre testee headless (tests/logic_tests.cpp)
// : un decalage faux dans un allocateur ne se voit pas a l'ecran avant d'avoir
// ecrase la geometrie d'un autre maillage.
//
// Les tailles en jeu ne justifient pas mieux qu'une liste triee : quelques
// dizaines de plages par bloc au pire (les tuiles de terrain ont toutes la meme
// taille, donc un trou laisse par une tuile evincee est exactement celui que la
// suivante remplit).
class RangeAllocator {
public:
    static constexpr uint64_t kInvalid = std::numeric_limits<uint64_t>::max();

    explicit RangeAllocator(uint64_t capacity) : capacity_(capacity) {
        if (capacity > 0) {
            free_.push_back({ 0, capacity });
        }
    }

    uint64_t capacity() const { return capacity_; }
    uint64_t used() const { return used_; }
    bool empty() const { return used_ == 0; }

    // Plus grande plage libre : sert a decider s'il faut un nouveau bloc.
    uint64_t largestFree() const {
        uint64_t best = 0;
        for (const Span& span : free_) {
            best = (span.size > best) ? span.size : best;
        }
        return best;
    }

    // Reserve `size` octets alignes sur `alignment` (puissance de deux).
    // Renvoie le decalage, ou kInvalid si aucune plage ne convient.
    uint64_t allocate(uint64_t size, uint64_t alignment) {
        if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return kInvalid;
        }
        for (std::size_t k = 0; k < free_.size(); ++k) {
            const Span span = free_[k];
            const uint64_t aligned = (span.offset + alignment - 1) & ~(alignment - 1);
            const uint64_t padding = aligned - span.offset;
            if (padding > span.size || span.size - padding < size) {
                continue;
            }
            // La plage est decoupee en (rembourrage avant) + (reserve) + (reste).
            // Le rembourrage reste LIBRE : il n'est perdu que tant qu'aucune
            // demande plus petite ne le reclame.
            const uint64_t tailOffset = aligned + size;
            const uint64_t tailSize = span.offset + span.size - tailOffset;
            free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(k));
            std::size_t insertAt = k;
            if (padding > 0) {
                free_.insert(free_.begin() + static_cast<std::ptrdiff_t>(insertAt),
                             Span{ span.offset, padding });
                ++insertAt;
            }
            if (tailSize > 0) {
                free_.insert(free_.begin() + static_cast<std::ptrdiff_t>(insertAt),
                             Span{ tailOffset, tailSize });
            }
            used_ += size;
            return aligned;
        }
        return kInvalid;
    }

    // Rend une plage obtenue par allocate. Fusionne avec les plages libres
    // contigues, pour qu'une serie d'evictions reconstitue de grands trous.
    void release(uint64_t offset, uint64_t size) {
        if (size == 0) {
            return;
        }
        std::size_t k = 0;
        while (k < free_.size() && free_[k].offset < offset) {
            ++k;
        }
        free_.insert(free_.begin() + static_cast<std::ptrdiff_t>(k), Span{ offset, size });
        used_ -= size;
        // Fusion avec le suivant, puis avec le precedent.
        if (k + 1 < free_.size() && free_[k].offset + free_[k].size == free_[k + 1].offset) {
            free_[k].size += free_[k + 1].size;
            free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(k + 1));
        }
        if (k > 0 && free_[k - 1].offset + free_[k - 1].size == free_[k].offset) {
            free_[k - 1].size += free_[k].size;
            free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(k));
        }
    }

    // Nombre de plages libres disjointes : 1 pour un bloc vide, et c'est
    // l'invariant que les tests verifient apres une liberation complete.
    std::size_t freeSpanCount() const { return free_.size(); }

private:
    struct Span {
        uint64_t offset;
        uint64_t size;
    };

    uint64_t capacity_ = 0;
    uint64_t used_ = 0;
    std::vector<Span> free_; // trie par decalage croissant, jamais deux contigues
};

} // namespace vercora
