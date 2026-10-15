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
    // Gros blocs : loi de taille de la Phase 29, INCHANGEE en Phase 30. C'est
    // elle qui porte la collision (collideMinScale, rayon x echelle) : la garder
    // telle quelle garantit une collision identique au bit pres.
    float boulderSizeMin = 1.1f, boulderSizeMax = 2.6f; // echelle (m)
    // PHASE 30 (Agent B) : rochers moyens 0,35-0,95 -> 0,6-1,4, puis x le
    // facteur de rugosite (voir rockRoughnessScale) et plafonnes a stoneSizeCap.
    // Mesure (build-phase30gentA\DIAGNOSTIC.md) : a 0,36 m hors sol ils
    // n'apportaient que 2 % des pixels de rochers, 3 px de mediane a 35 m.
    float stoneSizeMin = 0.60f, stoneSizeMax = 1.40f;
    // Plafond STRICTEMENT sous collideMinScale : un rocher moyen n'a pas de
    // collision ; il ne doit donc jamais atteindre la taille d'un gros bloc qui
    // en a une (sinon on traverserait un bloc d'aspect bloquant).
    float stoneSizeCap = 1.29f;
    // PHASE 30 : facteur de taille = clamp(rugosite / ref, min, max). Un DTM de
    // 0,10 m RMS ne porte pas de blocs de 2 m ; une zone a 0,30 m RMS, si. Ne
    // s'applique qu'aux rochers MOYENS (les gros blocs gardent leur loi, voir
    // plus haut). Mediane de la rugosite au pied des rochers : 0,16-0,18 m,
    // donc un facteur median ~1,1.
    float roughScaleRef = 0.15f;
    float roughScaleMin = 0.80f, roughScaleMax = 2.00f;
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

    // --- PHASE 31 (Agent 2) : emplacements de PROXIMITE ---------------------
    // Mesure (build-phase30\agentA\DIAGNOSTIC.md, NOTES-PHASE30.md) : le premier
    // plan (0-15 m) n'avait AUCUN rocher, faute de densite. On ajoute, dans les
    // MEMES cellules et sous le MEME masque (pente, couvert, rugosite -> classe,
    // trainees), des emplacements supplementaires a PORTEE COURTE : c'est le LOD
    // de densite existant (chaque emplacement a sa portee) prolonge vers le
    // joueur. Au-dela de stone/gravelNearDistanceFar ils ne sont ni parcourus
    // ni dessines : aucun surcout lointain.
    //  - rochers moyens seulement (jamais de gros bloc : la collision ne change
    //    pas), meme loi de taille, donc plafonnes a stoneSizeCap < 1,3 ;
    //  - pierraille : memes plaques.
    // Flux de hachage DISJOINTS de ceux de la Phase 29/30 : les instances
    // d'origine sont identiques au bit (empreinte dans les tests).
    // Valeurs MESUREES (build-phase31\agent2\RAPPORT.md) : 40 emplacements a
    // 48 m ou 60 a 80 m donnent le meme gain d'image au pierrier (6,87 / 6,93 %
    // de pixels) ; on garde 60 emplacements (densite proche) sur 60 m.
    uint32_t stoneNearSlots = 60;  // en plus des 20 : au plus 80 rochers / 1024 m2 pres du joueur
    float stoneNearDistanceFar = 60.0f, stoneNearDistanceNear = 18.0f;
    uint32_t gravelNearSlots = 24; // en plus des 12 : au plus 36 plaques / 64 m2 pres du joueur
    float gravelNearDistanceFar = 26.0f, gravelNearDistanceNear = 12.0f;
    // Fondu par retrecissement des emplacements de proximite : sur les 40 %
    // derniers de leur portee (0,82 -> 1 pour les autres). Portees courtes,
    // donc bande plus large en proportion : une plaque ne doit pas "pousser"
    // en 2 m de marche.
    float nearFadeStart = 0.60f;
    // PLAFOND d'instances par BLOC de cache (8 x 8 cellules), toutes classes.
    // Seules les instances de PROXIMITE sont eclaircies pour tenir dessous
    // (tirage `rank` uniforme, independant de l'ordre) : les instances
    // d'origine, gros blocs compris, ne sont JAMAIS retirees. Le constructeur
    // refuse un plafond sous le maximum d'origine (64 x emplacements d'origine).
    // Sans plafond, le pire cas theorique serait 64 x 88 = 5 632 (blocs) et
    // 64 x 36 = 2 304 (pierraille). Mesure sur la zone : max 1 080 et 1 315.
    uint32_t blockInstanceCap = 1792;  // = maximum d'origine 64 x 28 (le plafond le plus bas permis)
    uint32_t gravelInstanceCap = 1024; // origine : au plus 64 x 12 = 768

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

// PHASE 30 (Agent B) : facteur de taille des rochers moyens selon la rugosite
// locale du DTM (m). Borne, monotone, sans alea : il ne consomme AUCUN tirage du
// hachage, donc ne change ni la presence ni la position d'aucun rocher.
inline float rockRoughnessScale(float roughness, const RockParams& p) {
    const float k = roughness / p.roughScaleRef;
    return std::fmin(std::fmax(k, p.roughScaleMin), p.roughScaleMax);
}

// PHASE 31 (Agent 2) : ENFONCEMENT d'une plaque de pierraille pendant son fondu
// (m, a retrancher a y). Le fondu retrecit la plaque vers son origine ; l'assise
// (Rocks::makeCandidate) n'est sondee qu'a pleine echelle, sur deux anneaux. Mesure
// (sonde, 150 m autour des deux sites) : a 30 % d'echelle, 1 a 4 cailloux sur des
// dizaines de milliers ont leur point bas jusqu'a 1 cm AU-DESSUS du sol - defaut
// anterieur a la Phase 31, plus frequent avec la densite. On enfonce donc la plaque
// de 4 cm x echelle x (1 - fondu) : nul a pleine echelle (assise inchangee), et
// la plaque s'enterre en meme temps qu'elle rapetisse. Blocs et rochers : 0 (pied
// au sol, ils ne flottent a aucune echelle, mesure).
constexpr float kGravelFadeSink = 0.04f;
inline float rockFadeSink(RockKind kind, float scale, float fade) {
    return kind == RockKind::Gravel ? kGravelFadeSink * scale * (1.0f - fade) : 0.0f;
}

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
    // PHASE 31 : emplacement de PROXIMITE (portee courte, fondu large, eclairci
    // par le plafond du bloc selon `rank`). false = emplacement d'origine.
    bool nearSlot = false;
    uint32_t rank = 0;
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

    // Tous les candidats RETENUS par le semis d'une cellule (tests) : origine
    // puis proximite, AVANT le plafond par bloc.
    std::vector<RockInstance> blockCellRocks(int32_t cx, int32_t cz) const;
    std::vector<RockInstance> gravelCellRocks(int32_t cx, int32_t cz) const;
    // PHASE 31 : contenu d'un BLOC de cache (kBlockCells x kBlockCells
    // cellules), APRES le plafond - exactement ce que gather et la collision
    // lisent. Construit a neuf (fonction pure du bloc ; tests, diagnostic).
    std::vector<RockInstance> blockRocks(int32_t bx, int32_t bz, bool gravelGrid) const;
    static constexpr int32_t kBlockCells = 8;

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
        std::vector<RockInstance> rocks;     // emplacements d'origine (Phase 29/30)
        std::vector<RockInstance> nearRocks; // PHASE 31 : emplacements de proximite
        glm::vec3 boxMin{0.0f}, boxMax{0.0f};
    };
    using Cache = std::unordered_map<CellKey, Cell, CellKeyHash>;

    Cell buildBlockCell(int32_t cx, int32_t cz) const;
    Cell buildGravelCell(int32_t cx, int32_t cz) const;
    // Cache par BLOC de kBlockCells x kBlockCells cellules (voir buildBlock).
    Cell buildBlock(int32_t bx, int32_t bz, bool gravelGrid) const;
    const Cell& cachedBlock(int32_t bx, int32_t bz, bool gravelGrid) const;

    // Parcours commun aux deux grilles (culling par blocs 8x8, budget, LOD).
    void gatherGrid(const glm::vec3& eye, const Frustum& frustum, bool gravelGrid,
                    std::vector<InstanceData>* outs[3]);

    // Candidat a (x, z) : filtre dur + construction. Renvoie false si rejete.
    // `roughness` : rugosite deja calculee au point par le semis (blocs) ; sert
    // a la taille des rochers moyens (PHASE 30), ignoree pour les autres types.
    // `nearSlot` (PHASE 31) : portee tiree de stone/gravelNearDistance*.
    bool makeCandidate(float x, float z, RockKind kind, uint32_t slot, uint32_t slots,
                       uint32_t hSize, uint32_t hYaw, uint32_t hTint, float roughness,
                       RockInstance& out, bool nearSlot = false) const;
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
