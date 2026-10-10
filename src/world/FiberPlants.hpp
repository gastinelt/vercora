#pragma once

#include "renderer/InstanceData.hpp"
#include "world/Clearing.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vercora {

class HeightSampler;
struct Frustum;

// ===========================================================================
// PHASE 22 : les PLANTES A FIBRES (ronces, orties).
// ===========================================================================
// Ce sont des plantes de RUDERALIE : elles ne poussent pas dans la futaie
// fermee, elles poussent la ou la lumiere arrive au sol et ou personne ne les
// concurrence. Le document de cadrage dit "clairieres, zones perturbees", et
// c'est litteralement le critere code ici, par DEUX voies independantes qui
// s'additionnent :
//
//   1. LA LISIERE, lue dans la carte de couvert LiDAR. Ni la futaie dense
//      (l'ombre les tue) ni l'alpage nu (rien ne les abrite) : la BANDE DE
//      TRANSITION entre les deux. C'est une bordure, donc une forme que la
//      carte de couvert donne gratuitement - il suffit de retenir les cellules
//      dont le couvert tombe entre deux seuils. Rien de nouveau n'est charge :
//      c'est la MEME carte que celle qui decide ou poussent les arbres de decor
//      (voir Vegetation::coverAt), lue avec d'autres seuils.
//
//   2. LE SOL RECEMMENT OUVERT, lu dans world/Clearing.hpp - en pratique le
//      compteur de deforestation de world/Habitat. Une zone ou le joueur a
//      abattu des arbres se met a porter des ronces. C'est la coherence que le
//      cahier des charges suggerait et elle est exploitee ici PARCE QU'ELLE EST
//      VRAIE : la recolonisation d'une coupe par les ronces est le fait le plus
//      banal de la sylviculture temperee. Elle a en prime une vertu de jeu que
//      rien d'autre dans le projet n'avait : c'est la PREMIERE fois qu'une
//      action du joueur FAIT APPARAITRE quelque chose au lieu de le retirer.
//      La Phase 17 avait construit la boucle "je coupe, le milieu s'appauvrit" ;
//      celle-ci lui donne son revers.
//
// La densite retenue est le MAXIMUM des deux et non leur somme : une coupe faite
// en pleine lisiere ne doit pas porter deux fois plus de ronces qu'ailleurs.
//
// LES LIMITES, EN CLAIR. Le couvert LiDAR ne distingue pas une vraie lisiere
// d'un simple trou dans le couvert (un affleurement rocheux au milieu des
// arbres passe le test). Et rien ne distingue une ronce d'une ortie : le mesh
// est unique, seule l'echelle varie. Ce sont des limites assumees, du meme
// ordre que celles de world/Deposits.hpp et de world/Resin.hpp.
//
// ARCHITECTURE : le semis est une FONCTION DETERMINISTE de la cellule, comme
// celui de world/Vegetation - engendrer la cellule (17, -42) donne toujours les
// memes plantes, quelle que soit la facon dont on y est arrive, et il n'y a
// donc RIEN a stocker pour les 25 km2. Seules les plantes ARRACHEES sont
// memorisees, dans une table creuse, et seulement le temps de leur repousse :
// meme principe que la grille creuse de world/Habitat.
//
// Ne connait RIEN de Vulkan : produit des InstanceData, c'est main.cpp qui les
// remet au Renderer. Testable sans GPU.
// ===========================================================================

struct FiberPlantParams {
    // Cote d'une cellule de semis, en m. 4 m : entre la cellule d'herbe (1 m) et
    // celle des arbres (24 m). Une tache de ronces fait quelques metres, pas
    // quelques decimetres.
    float cell = 4.0f;

    // Emplacements engendres par cellule. Le nombre REELLEMENT retenu depend de
    // la densite locale (voir plus bas). 3 sur 4 m de cote = au plus une plante
    // pour 5,3 m2 au maximum de densite : un roncier est dense, mais il reste
    // franchissable a pied.
    uint32_t slots = 3;

    // Portee de rendu, en m, et debut du fondu. Court, comme l'herbe : ces
    // plantes sont basses et c'est le poste d'instances le plus cher apres
    // elle.
    float distanceFar = 46.0f;
    float distanceNear = 22.0f;

    // --- Voie 1 : la LISIERE -------------------------------------------------
    // Bande de couvert retenue. En dessous de coverOpen c'est de l'alpage nu, au
    // dessus de coverClosed c'est de la futaie fermee ; la densite est maximale
    // au MILIEU de la bande et retombe a zero a ses deux bords.
    //
    // Les bornes encadrent volontairement VegetationParams::coverMin (0,20), qui
    // est le seuil a partir duquel une cellule commence a porter des arbres de
    // decor : la lisiere est, par construction, l'endroit ou les arbres
    // apparaissent. Les deux fichiers parlent donc bien de la meme frontiere.
    float coverOpen = 0.06f;
    float coverClosed = 0.42f;

    // --- Voie 2 : le SOL OUVERT ---------------------------------------------
    // Ouverture (ClearingSource) en dessous de laquelle rien ne recolonise, et
    // au-dessus de laquelle la recolonisation est maximale.
    //
    // 0,30 correspond a UN SEUL arbre abattu dans la cellule d'Habitat
    // (HabitatParams::loggingPerTree vaut 0,34). C'est plus bas que le seuil qui
    // fait fuir la faune (0,40) et que celui qui denude le sol (0,25 a 1,00), et
    // c'est voulu : couper un arbre isole ne vide pas la zone de ses animaux,
    // mais il ouvre bel et bien une trouee de lumiere, et c'est tout ce qu'il
    // faut a une ronce.
    float clearingBegin = 0.30f;
    float clearingFull = 0.85f;

    // --- Filtres de terrain --------------------------------------------------
    // Pente maximale (tangente) portant des plantes a fibres. 1,0 (45 degres) :
    // plus severe que l'herbe (1,3) - un roncier a besoin d'un peu de sol, pas
    // seulement d'une fissure.
    float maxSlope = 1.0f;

    // Altitude, en m, au-dessus de laquelle ces plantes disparaissent. 1900 m :
    // c'est l'ordre de grandeur de la limite des arbres sur ce massif (voir
    // VegetationParams::grassAltitudeLush et son commentaire). Ronces et orties
    // sont des plantes de l'etage montagnard ; au-dessus, il n'y a plus de
    // lisiere puisqu'il n'y a plus de foret.
    float maxAltitude = 1900.0f;

    // Taille d'une plante, en m, aux deux bouts de la variation d'instance.
    float sizeMin = 0.45f;
    float sizeMax = 0.95f;

    // --- Recolte -------------------------------------------------------------
    // Delai de repousse d'une plante arrachee, en secondes de jeu. 420 s = sept
    // minutes. Entre la resine (600 s) et rien du tout : une ronce repousse vite,
    // c'est meme sa definition.
    float regrowthDelay = 420.0f;

    // --- Commun --------------------------------------------------------------
    // Deux FiberPlants de meme graine sont identiques. Distincte de celle de
    // Vegetation : changer l'une ne doit pas rebattre l'autre.
    uint32_t seed = 20260822u;
};

// Une plante identifiee de facon STABLE : sa cellule et son emplacement dans la
// cellule. C'est ce couple, et non un indice dans un vecteur, qui sert de cle a
// la memoire des plantes arrachees - un indice de vecteur n'aurait aucun sens
// pour un semis qui n'est jamais stocke.
struct FiberPlantId {
    int32_t cellX = 0;
    int32_t cellZ = 0;
    uint32_t slot = 0;

    bool operator==(const FiberPlantId& other) const {
        return cellX == other.cellX && cellZ == other.cellZ && slot == other.slot;
    }
};

// Une plante sur pied, telle que le semis la rend.
struct FiberPlant {
    FiberPlantId id;
    glm::vec3 position{0.0f}; // pied, sur la surface
    float size = 0.0f;        // hauteur en m
    float yaw = 0.0f;
    float variation = 0.0f;
};

// Portee d'arrachage, en m, a l'HORIZONTALE. 2,5 m : exactement celle du
// ramassage d'objet au sol, parce que c'est le meme geste - on se penche et on
// prend.
inline constexpr float kPullReach = 2.5f;

class FiberPlants {
public:
    // `terrain` fournit altitude et pente ; `cover` est la carte de couvert
    // LiDAR (celle de tools/lidar_to_canopy.py, echantillonnee en coordonnees
    // MONDE comme le terrain). Aucun des deux n'est possede et tous deux doivent
    // vivre plus longtemps.
    //
    // `cover` peut etre NUL : la voie "lisiere" est alors entierement
    // desactivee, et seule la voie "sol ouvert" produit des plantes. C'est le
    // comportement voulu et non un repli degrade - sans carte de couvert, on ne
    // sait tout simplement pas ou est la lisiere, et l'inventer par un
    // substitut de pente donnerait des ronces sur des criteres qui n'ont rien a
    // voir avec la lumiere. Un TEMOIN naturel, en prime : un monde sans carte de
    // couvert ne porte QUE les plantes que le joueur a fait pousser.
    //
    // `terrainExtent` = demi-cote du terrain, pour ne pas semer hors des donnees.
    FiberPlants(const HeightSampler& terrain, const HeightSampler* cover,
                float terrainExtent, const FiberPlantParams& params = {});

    // Branche le semis sur l'etat du milieu (world/Habitat en pratique). Nul par
    // defaut, et une FiberPlants sans source se comporte comme un monde jamais
    // coupe - le contrat TOTAL de ClearingSource garantit qu'il n'y a pas de cas
    // particulier a ecrire ici.
    // La source n'est PAS possedee et doit vivre plus longtemps.
    void setClearingSource(const ClearingSource* source) { clearing_ = source; }
    const ClearingSource* clearingSource() const { return clearing_; }

    // Fait avancer l'horloge de repousse et OUBLIE les plantes revenues. A
    // appeler une fois par image, comme Habitat::update.
    void update(float deltaTime);

    // Remplit `out` (vide d'abord) des plantes visibles depuis `eye`.
    void gather(const glm::vec3& eye, const Frustum& frustum,
                std::vector<InstanceData>& out) const;

    // Plantes SUR PIED a moins de `reach` (a l'horizontale) de `position`,
    // engendrees a la demande. Sert a l'arrachage et aux tests ; ne met rien en
    // cache.
    std::vector<FiberPlant> plantsNear(const glm::vec3& position, float reach) const;

    // Arrache la plante sur pied la plus proche, dans la portee donnee. Rend
    // true et remplit `pulled` (s'il n'est pas nul) quand une plante a bien ete
    // arrachee. La plante disparait du semis jusqu'a sa repousse.
    bool pullNearest(const glm::vec3& position, float reach, FiberPlant* pulled = nullptr);

    // Cette plante est-elle encore sur pied ? (false pendant sa repousse)
    bool isStanding(const FiberPlantId& id) const;

    // --- Diagnostic ---------------------------------------------------------
    std::size_t pulledPlantCount() const { return pulled_.size(); }
    uint32_t lastPlantCount() const { return lastPlantCount_; }

    // Densite locale retenue en un point, dans [0, 1], et les deux voies qui la
    // composent. Expose parce que c'est le SEUL chiffre qui dit laquelle des
    // deux a decide - un test qui ne pourrait pas les lire devrait redemontrer
    // le semis a l'envers pour distinguer une lisiere d'une coupe.
    float densityAt(float worldX, float worldZ) const;
    float edgeDensityAt(float worldX, float worldZ) const;
    float clearingDensityAt(float worldX, float worldZ) const;

    const FiberPlantParams& params() const { return params_; }

private:
    struct IdHash {
        std::size_t operator()(const FiberPlantId& id) const {
            // Meme melange que Vegetation::CellKeyHash et Habitat::CellKeyHash :
            // les moities occupent des bits distincts.
            return (static_cast<std::size_t>(static_cast<uint32_t>(id.cellX)) * 0x9E3779B1ull)
                 ^ (static_cast<std::size_t>(static_cast<uint32_t>(id.cellZ)) * 0x85EBCA77ull)
                 ^ (static_cast<std::size_t>(id.slot) * 0x27D4EB2Full);
        }
    };

    // Engendre les plantes SUR PIED d'une cellule (les arrachees sont omises).
    void buildCell(int32_t cellX, int32_t cellZ, std::vector<FiberPlant>& out) const;

    float slopeAt(float worldX, float worldZ) const;
    float coverAt(float worldX, float worldZ) const;

    const HeightSampler& terrain_;
    const HeightSampler* cover_ = nullptr;
    const ClearingSource* clearing_ = nullptr;
    float terrainExtent_ = 0.0f;
    FiberPlantParams params_;

    // Plantes arrachees : secondes de repousse restantes. Creuse, et videe des
    // qu'une plante est revenue.
    std::unordered_map<FiberPlantId, float, IdHash> pulled_;

    mutable uint32_t lastPlantCount_ = 0;
};

} // namespace vercora
