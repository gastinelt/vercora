#pragma once

#include "world/GroundItem.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <random>
#include <vector>

namespace vercora {

class HeightSampler;

// Indice d'objet invalide, renvoye par findNearest quand rien n'est a portee.
inline constexpr int kNoGroundItem = -1;

// Tout ce qui traine sur le sol : buches et branches laissees par un abattage,
// pierres semees par le monde, eclats produits par le taillage.
//
// Separee de Forest, qui gere les arbres DEBOUT et leur chute. La frontiere est
// nette : des qu'un objet est couche au sol et ramassable, il appartient au
// Ground ; tant qu'il pousse, il appartient a la Forest. Sans cette separation,
// Forest devrait connaitre les pierres, avec lesquelles elle n'a rien a voir.
//
// Ne connait RIEN de Vulkan : elle produit des positions et des natures, c'est
// main.cpp qui les traduit en DrawItem.
class Ground {
public:
    // Le terrain n'est PAS possede : il doit vivre plus longtemps que le Ground.
    Ground(const HeightSampler& terrain, uint32_t seed = 20260814u);

    // --- Depot --------------------------------------------------------------
    // Pose un objet a la verticale de (worldX, worldZ), exactement sur la
    // surface : l'appelant donne une position horizontale, le Ground se charge
    // du Y pour qu'aucun objet ne flotte ni ne s'enfonce.
    void drop(GroundItemKind kind, float worldX, float worldZ, float yaw, float roll);

    // Seme des pierres dans une couronne autour de l'origine, la ou demarre le
    // joueur. Meme tirage-rejet que le semis d'arbres : position au hasard,
    // gardee si elle respecte l'espacement minimal. Renvoie le nombre reellement
    // depose (la limite d'essais garantit qu'on sort meme si la zone est saturee).
    uint32_t scatterStones(uint32_t count, float areaRadius,
                           float minDistanceFromStart, float terrainExtent);

    // --- Ramassage ----------------------------------------------------------
    // Objet ramassable le plus proche du joueur, ou kNoGroundItem.
    //
    // La portee se mesure a l'HORIZONTALE seulement : un objet en contrebas d'un
    // talus reste attrapable, et surtout la portee ne depend pas de la taille du
    // joueur ni du fait que la position de reference soit son oeil et non sa
    // main. Le critere vertical est traite a part, avec une tolerance large.
    int findNearest(const glm::vec3& playerPosition, float reach,
                    float verticalTolerance = 3.0f) const;

    // Retire l'objet d'indice donne et renvoie sa nature par kindOut (peut etre
    // nullptr). false si l'indice ne designe rien.
    bool pickUp(int index, GroundItemKind* kindOut = nullptr);

    // PHASE 28 : TOUS les objets a portee (meme critere horizontal + tolerance
    // verticale que findNearest), portables ou non, tries par indice croissant.
    // Sert au faconnage (world/Crafting) : une recette compte ce qui est POSE.
    // Memes reserves que findNearest : indices invalides apres un pickUp.
    std::vector<int> indicesWithin(const glm::vec3& position, float reach,
                                   float verticalTolerance = 3.0f) const;

    const std::vector<GroundItem>& items() const { return items_; }
    std::size_t itemCount() const { return items_.size(); }

private:
    const HeightSampler& terrain_;
    std::vector<GroundItem> items_;

    // RNG seede : une meme graine rejoue exactement le meme semis de pierres.
    std::mt19937 rng_;
};

} // namespace vercora
