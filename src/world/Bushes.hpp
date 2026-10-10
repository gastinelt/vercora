#pragma once

#include "renderer/InstanceData.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vercora {

class HeightSampler;
struct Frustum;

// PHASE 29 (Agent 3) : buissons et petits arbres, pilotes UNIQUEMENT par la
// classe 3 LiDAR (vegetation basse) rasterisee par l'Agent 1 au pas de 5 m
// (NOTES-PHASE29-DESIGN.md, section 2) :
//
//   BUISSON  =  F3 >= 0,10  ET  F45 = couvert - F3 < 0,20
//   F3       =  retours classe 3 / retours valides (vercora_zone_canopy_lowveg.vchm)
//   couvert  =  (c3 + c4 + c5) / valides (vercora_zone_canopy.vchm, deja charge)
//
// La carte des grands arbres (couvert) ne sert QU'A exclure : elle ne cree
// aucun buisson (pas de double emploi du meme signal). Densite : lineaire en F3
// de 0,10 (rare) a 0,30 (~p99, plein). Hauteur : tiree SOUS la hauteur max de
// la classe 3 de la cellule (`_lowveg_height`), bornee a [heightMin, heightMax].
//
// Exclusions :
//  - rochers (Rocks, Agent 2) : leur masque exige couvert < 0,10 ; on exige ici
//    couvert >= 0,10, donc les deux semis sont DISJOINTS par construction (la
//    donnee a 6 cellules sur 1 M ou F3 depasse le couvert de < 0,03 : ce test
//    les ecarte aussi) ;
//  - arbres abattables de la Forest : emprise du houppier (setTreeFootprints).
//    Les arbres de decor de Vegetation ne sont pas consultes (semis prive) ;
//    F45 < 0,20 les evite deja en grande partie (leur seuil coverMin = 0,20).
//
// Sans l'une des trois cartes : AUCUN buisson (aucun substitut).
// Collision : AUCUNE, les buissons se traversent (voir NOTES-PHASE29.md).
struct BushParams {
    float lowVegMin = 0.10f;    // F3 minimal
    float lowVegFull = 0.30f;   // F3 a densite pleine
    float highVegMax = 0.20f;   // F45 maximal
    float rockCoverMax = 0.10f; // couvert sous lequel Rocks peut semer -> exclu ici
    float minDensity = 0.20f;   // part des emplacements retenue a F3 = lowVegMin

    float cell = 8.0f;
    uint32_t slots = 6; // au plus 6 sujets / 64 m2 (1 / 10,7 m2)

    float heightMin = 0.5f, heightMax = 3.0f; // m
    float heightMapMin = 0.3f; // h classe 3 de la cellule sous ce seuil : pas de sujet
    // Fraction de h max tiree : [0,35 ; 0,85], moyenne 0,60 = rapport mesure par
    // l'Agent 1 entre hauteur MOYENNE (p50 1,5 m) et MAX (p50 2,5 m) de la classe 3.
    float heightDrawMin = 0.35f, heightDrawMax = 0.85f;
    float smallTreeFrom = 1.8f; // hauteur a partir de laquelle c'est un petit arbre

    // Portees : un premier reglage 220/380 m coutait 276 us de semis par image
    // (Debug) au coeur d'une zone de buissons ; ramene a 150/260 m.
    float shrubDistanceFar = 150.0f, shrubDistanceNear = 55.0f;
    float treeDistanceFar = 260.0f, treeDistanceNear = 100.0f;

    uint32_t seed = 20260928u;
    uint32_t blockBudget = 4; // blocs de 8x8 cellules engendres au plus par appel
};

enum class BushKind : uint8_t { Shrub = 0, SmallTree = 1 };

struct BushInstance {
    glm::vec3 position{0.0f};
    float height = 1.0f; // = echelle du mesh (unite = 1 m de haut)
    float yaw = 0.0f;
    float tint = 0.0f;
    float maxDistance = 0.0f;
    BushKind kind = BushKind::Shrub;
};

class Bushes {
public:
    // lowVeg = F3, lowVegHeight = h max classe 3 - sol (m), cover = couvert total.
    // Toutes non possedees ; une seule nulle -> aucun buisson.
    Bushes(const HeightSampler& terrain, const HeightSampler* lowVeg,
           const HeightSampler* lowVegHeight, const HeightSampler* cover, float terrainExtent,
           const BushParams& params = {});

    bool enabled() const { return lowVeg_ && lowVegHeight_ && cover_; }

    // Emprises (x, z, rayon) des arbres existants : aucun buisson dedans.
    // A appeler avant le premier gather (vide le cache).
    void setTreeFootprints(std::vector<glm::vec3> footprints);

    void gather(const glm::vec3& eye, const Frustum& frustum, std::vector<InstanceData>& shrubs,
                std::vector<InstanceData>& smallTrees);

    // --- Critere et semis exposes (tests, diagnostic) ------------------------
    bool maskAt(float worldX, float worldZ) const;
    float densityAt(float worldX, float worldZ) const; // [0,1], 0 hors masque
    std::vector<BushInstance> cellBushes(int32_t cx, int32_t cz) const;

    struct Stats {
        double maskFraction = 0.0;
        double shrubsPerHa = 0.0;     // par ha de ZONE
        double smallTreesPerHa = 0.0;
    };
    Stats estimate(uint32_t step = 7) const;

    uint32_t lastShrubCount() const { return lastShrubs_; }
    uint32_t lastSmallTreeCount() const { return lastTrees_; }
    const BushParams& params() const { return params_; }

private:
    struct Key {
        int32_t x = 0, z = 0;
        bool operator==(const Key& o) const { return x == o.x && z == o.z; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return (static_cast<std::size_t>(static_cast<uint32_t>(k.x)) * 0x9E3779B1ull) ^
                   (static_cast<std::size_t>(static_cast<uint32_t>(k.z)) * 0x85EBCA77ull);
        }
    };
    struct Block {
        std::vector<BushInstance> bushes;
        glm::vec3 boxMin{0.0f}, boxMax{0.0f};
    };
    static constexpr int32_t kBlockCells = 8;
    Block buildBlock(int32_t bx, int32_t bz) const;
    bool inTreeFootprint(float x, float z) const;
    float coverAt(float x, float z) const;
    float lowVegAt(float x, float z) const;

    const HeightSampler& terrain_;
    const HeightSampler* lowVeg_ = nullptr;
    const HeightSampler* lowVegHeight_ = nullptr;
    const HeightSampler* cover_ = nullptr;
    float terrainExtent_ = 0.0f;
    BushParams params_;

    // Emprises d'arbres rangees par case de 32 m.
    std::unordered_map<Key, std::vector<glm::vec3>, KeyHash> treeGrid_;
    float treeGridMaxRadius_ = 0.0f;

    std::unordered_map<Key, Block, KeyHash> cache_;
    uint32_t lastShrubs_ = 0, lastTrees_ = 0;
};

} // namespace vercora
