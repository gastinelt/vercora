#pragma once

#include "core/CollisionSolver.hpp"
#include "core/SlopeLimits.hpp"
#include "renderer/InstanceData.hpp"
#include "world/Climber.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vercora {

class HeightSampler;
struct Frustum;

// PHASE 29 (Agent 2) : rochers et pierriers PROCEDURAUX, pilotes par les donnees
// reelles selon les criteres valides dans NOTES-PHASE29-DESIGN.md (Agent 1) :
//
//   PIERRIER  =  kWalkComfortSlope <= pente(Climber::slopeAt, base 4 m) < kWalkMaxSlope
//                ET couvert LiDAR < 0,10  (ET hors bati/eau : non expose au jeu,
//                effet mesure 2 272 m2 sur 25 km2 - voir NOTES-PHASE29.md).
//   MESH      =  rugosite (RMS du residu DTM - moyenne 5x5, sur 5x5) >= 0,10 m
//                -> gros blocs + rochers moyens ; < 0,10 m -> pierraille fine.
//
// Le masque est un filtre DUR : aucune position hors masque n'est jamais
// retenue, quel que soit le bruit. Le bruit (basse frequence, etire dans le sens
// de la pente) ne fait que MODULER la densite a l'interieur du masque, pour
// dessiner des trainees d'eboulis au lieu d'un semis uniforme.
//
// Sans carte de couvert (terrain synthetique, dalle sans carte) : AUCUN rocher.
// C'est la regle "aucun rocher sans lien avec une donnee reelle" - on ne
// retombe PAS sur un substitut par pente seule, contrairement a Vegetation.
//
// Meme architecture que Vegetation : semis = fonction deterministe (hachage) de
// la cellule, cache des cellules proches, culling par blocs 8x8, LOD de DENSITE
// par emplacement (chaque emplacement a sa portee), InstanceData de 28 o.
struct RockParams {
    // --- Criteres du masque (NOTES-PHASE29-DESIGN.md, section 3) ------------
    float slopeMin = kWalkComfortSlope; // 0,60
    float slopeMax = kWalkMaxSlope;     // 0,84 (au-dela : paroi, Climber)
    float coverMax = 0.10f;
    float roughBlocks = 0.10f; // m : >= -> blocs, < -> pierraille

    // --- Blocs (gros blocs + rochers moyens), grille grossiere --------------
    // Cellule de 32 m (et non 16) : MESURE, le semis a 16 m / 900 m parcourait
    // ~10 000 cellules par image, 1,6 ms en Debug. Meme densite surfacique.
    float blockCell = 32.0f;
    uint32_t boulderSlots = 8; // au plus 8 gros blocs / 1024 m2 (1 / 128 m2)
    uint32_t stoneSlots = 20;  // au plus 20 rochers moyens / 1024 m2
    float boulderSizeMin = 1.1f, boulderSizeMax = 2.6f; // echelle (m)
    float stoneSizeMin = 0.35f, stoneSizeMax = 0.95f;
    float boulderDistanceFar = 700.0f, boulderDistanceNear = 350.0f;
    float stoneDistanceFar = 320.0f, stoneDistanceNear = 140.0f;

    // --- Pierraille fine, grille fine ----------------------------------------
    float gravelCell = 8.0f;
    uint32_t gravelSlots = 12; // plaques de ~11 cailloux, au plus 12 / 64 m2 (3 / 16 m2)
    float gravelSizeMin = 0.7f, gravelSizeMax = 1.5f;
    float gravelDistanceFar = 70.0f, gravelDistanceNear = 30.0f;

    // --- Trainees : bruit de valeur a basse frequence, ANISOTROPE -----------
    // Frequences en cycles/m dans le repere local de la pente : lent dans le
    // sens de la pente (trainees de ~110 m), rapide en travers (~28 m).
    float streakAlong = 1.0f / 110.0f;
    float streakAcross = 1.0f / 28.0f;
    float streakThreshold = 0.35f; // densite nulle sous ce seuil de bruit
    float streakSoftness = 0.30f;  // largeur de la rampe au-dessus
    // Pas des differences centrees pour la DIRECTION des trainees (pas pour le
    // masque) : une base large evite que la trainee se torde a chaque bosse.
    float streakDirectionStep = 16.0f;
    // Tuile du repere des trainees (voir Rocks::streakDensityAt) : la direction
    // de la pente est lue au centre de chaque tuile de 96 m.
    float streakTile = 96.0f;

    // Part de la pente suivie par l'inclinaison du rocher (1 = normale au sol).
    float tiltFollow = 0.85f;

    // --- Collision ------------------------------------------------------------
    // Seuls les gros blocs d'au moins cette echelle bloquent le joueur ; les
    // rochers moyens et la pierraille se traversent (voir NOTES-PHASE29.md).
    bool collide = true;
    float collideMinScale = 1.3f;
    float colliderRadiusFactor = 0.85f; // x kRockFootprintRadius x echelle
    float stepOver = 0.40f;             // un bloc dont le sommet est sous les pieds + ca ne bloque pas

    uint32_t seed = 20260927u;
    // BLOCS de 8 x 8 cellules engendres au plus par appel (a-coup borne).
    uint32_t blockCellBudget = 3;
    uint32_t gravelCellBudget = 4;
};

enum class RockKind : uint8_t { Boulder = 0, Stone = 1, Gravel = 2 };

// Encodage de l'inclinaison dans InstanceData::variation (voir shaders/
// foliage.vert, mode rocher = params.w < 0). La struct d'instance est pleine
// (28 o, stride fige) ; plutot que d'en creer une seconde et un second format
// de sommet, la partie ENTIERE de `variation` porte le gradient quantifie sur
// 64 x 64 niveaux dans [-1, 1], la partie fractionnaire garde l'alea de teinte.
// 4095 + fraction tient dans les 24 bits de mantisse avec 12 bits de fraction.
inline float packRockVariation(float tint, glm::vec2 gradient) {
    const auto q = [](float g) {
        const float c = std::fmin(std::fmax(g, -1.0f), 1.0f);
        return std::floor((c + 1.0f) * 0.5f * 63.0f + 0.5f);
    };
    const float t = std::fmin(std::fmax(tint, 0.0f), 0.999f);
    return q(gradient.x) * 64.0f + q(gradient.y) + t;
}
inline glm::vec2 unpackRockGradient(float packed) {
    const float index = std::floor(packed);
    const float qx = std::floor(index / 64.0f);
    const float qz = index - qx * 64.0f;
    return glm::vec2(qx, qz) / 63.0f * 2.0f - 1.0f;
}
inline float unpackRockTint(float packed) { return packed - std::floor(packed); }

struct RockInstance {
    glm::vec3 position{0.0f}; // pied (origine du mesh), sur le terrain
    float scale = 1.0f;
    float yaw = 0.0f;
    float variation = 0.0f;   // deja encode (packRockVariation)
    float maxDistance = 0.0f; // portee propre a l'emplacement (LOD de densite)
    RockKind kind = RockKind::Gravel;
};

class Rocks : public CollisionSolver {
public:
    // `cover` = carte de couvert LiDAR (vercora_zone_canopy.vchm), comme
    // Vegetation. Nulle -> aucun rocher (voir plus haut). Non possedes.
    Rocks(const HeightSampler& terrain, const HeightSampler* cover, float terrainExtent,
          const RockParams& params = {});

    bool enabled() const { return cover_ != nullptr; }

    // Remplit les trois vecteurs (vides d'abord) des rochers visibles.
    void gather(const glm::vec3& eye, const Frustum& frustum,
                std::vector<InstanceData>& boulders, std::vector<InstanceData>& stones,
                std::vector<InstanceData>& gravel);

    // --- Critere, expose pour les tests et le diagnostic --------------------
    bool maskAt(float worldX, float worldZ) const;       // filtre dur
    float roughnessAt(float worldX, float worldZ) const; // m
    float streakDensityAt(float worldX, float worldZ) const; // [0,1]

    // Tous les candidats RETENUS par le semis d'une cellule (tests).
    std::vector<RockInstance> blockCellRocks(int32_t cx, int32_t cz) const;
    std::vector<RockInstance> gravelCellRocks(int32_t cx, int32_t cz) const;

    // Estimation sur toute la zone (une cellule de blocs sur `step`) : part de
    // la surface sous masque et nombre de rochers par hectare de masque.
    struct Stats {
        double maskFraction = 0.0;
        double bouldersPerHa = 0.0; // par hectare de ZONE
        double stonesPerHa = 0.0;
        double gravelPerHa = 0.0;   // plaques
    };
    Stats estimate(uint32_t step = 9) const;

    // --- CollisionSolver : gros blocs seulement -----------------------------
    glm::vec3 resolve(const BodyCylinder& body) const override;
    bool overlaps(const BodyCylinder& body) const override;

    // --- Diagnostic -------------------------------------------------------
    uint32_t lastBoulderCount() const { return lastBoulders_; }
    uint32_t lastStoneCount() const { return lastStones_; }
    uint32_t lastGravelCount() const { return lastGravel_; }
    std::size_t cachedBlockCount() const { return blockCache_.size() + gravelCache_.size(); }
    const RockParams& params() const { return params_; }

private:
    struct CellKey {
        int32_t x = 0, z = 0;
        bool operator==(const CellKey& o) const { return x == o.x && z == o.z; }
    };
    struct CellKeyHash {
        std::size_t operator()(const CellKey& k) const {
            return (static_cast<std::size_t>(static_cast<uint32_t>(k.x)) * 0x9E3779B1ull) ^
                   (static_cast<std::size_t>(static_cast<uint32_t>(k.z)) * 0x85EBCA77ull);
        }
    };
    struct Cell {
        std::vector<RockInstance> rocks;
        glm::vec3 boxMin{0.0f}, boxMax{0.0f};
    };
    using Cache = std::unordered_map<CellKey, Cell, CellKeyHash>;

    Cell buildBlockCell(int32_t cx, int32_t cz) const;
    Cell buildGravelCell(int32_t cx, int32_t cz) const;
    // Cache par BLOC de kBlockCells x kBlockCells cellules (voir buildBlock).
    static constexpr int32_t kBlockCells = 8;
    Cell buildBlock(int32_t bx, int32_t bz, bool gravelGrid) const;
    const Cell& cachedBlock(int32_t bx, int32_t bz, bool gravelGrid) const;

    // Parcours commun aux deux grilles (culling par blocs 8x8, budget, LOD).
    void gatherGrid(const glm::vec3& eye, const Frustum& frustum, bool gravelGrid,
                    std::vector<InstanceData>* outs[3]);

    // Candidat a (x, z) : filtre dur + construction. Renvoie false si rejete.
    bool makeCandidate(float x, float z, RockKind kind, uint32_t slot, uint32_t slots,
                       uint32_t hSize, uint32_t hYaw, uint32_t hTint,
                       RockInstance& out) const;
    float coverAt(float worldX, float worldZ) const;
    void fitBox(Cell& cell, float baseX, float baseZ, float size) const;

    const HeightSampler& terrain_;
    const HeightSampler* cover_ = nullptr;
    float terrainExtent_ = 0.0f;
    RockParams params_;
    Climber slopeProbe_; // Climber::slopeAt = LA pente du critere (base 4 m)

    // Mutables : la collision (const, interface CollisionSolver) engendre a la
    // demande les cellules de blocs sous le joueur. Le contenu est une fonction
    // pure de la cellule : le cache ne change aucun resultat, seulement le cout.
    mutable Cache blockCache_;
    mutable Cache gravelCache_;

    uint32_t lastBoulders_ = 0, lastStones_ = 0, lastGravel_ = 0;
};

} // namespace vercora
