#pragma once

#include "world/GroundItem.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vercora {

class Ground;
class Inventory;

// ===========================================================================
// PHASE 28 : le FACONNAGE generique et diegetique.
// ===========================================================================
// Generalisation directe de world/Knapping (knapStones), sans Vulkan, sans
// menu, sans fichier de donnees. Une seule touche (C, celle de la percussion) :
// le joueur choisit la recette EN CHOISISSANT CE QU'IL POSE autour de lui.
//
//  - Un ingredient est soit PORTE (compte dans Inventory), soit POSE (objet du
//    Ground a moins de kCraftReach du joueur, a l'horizontale, meme critere que
//    le ramassage). Un ingredient porte qui traine au sol ne compte pas, et
//    inversement : c'est la difference entre "tordre des fibres dans ses mains"
//    et "assembler une hache sur une souche".
//  - Un ingredient peut etre un OUTIL : requis, non consomme (regle du
//    percuteur, Knapping.hpp).
//  - Une recette peut exiger un POSTE : un foyer allume (ou froid, pour le
//    rallumer) a moins de kStationReach du joueur.
//  - La table est parcourue dans l'ordre (complexite decroissante) ; la
//    PREMIERE recette satisfaite est executee. Aucune autre regle.
//  - Tout est verifie AVANT le moindre retrait : un refus ne touche ni a
//    l'inventaire ni au sol (meme discipline qu'Inventory::remove).
//  - Le produit nait AU SOL : au barycentre des ingredients poses consommes
//    (l'objet nait la ou on l'a assemble), ou a 1,1 m devant le joueur si la
//    recette ne consomme que du porte (meme depot que la percussion).
//
// knapStones (Knapping.cpp) reste en place, INCHANGE, comme facade testee
// (testStoneKnapping) ; la recette "knap" ci-dessous en reproduit exactement le
// resultat, ce que verifie testCraftingKnapUnchanged.

enum class CraftSource : uint8_t { Carried, Placed };

enum class CraftStation : uint8_t {
    None,
    LitHearth,  // un foyer EN COMBUSTION a portee
    ColdHearth, // un foyer ETEINT a portee (pour le rallumer)
};

// Effet sur un foyer, en plus (ou a la place) d'un produit.
enum class CraftEffect : uint8_t {
    None,
    LightNewHearth, // le produit (Hearth) nait allume : l'eclat porte fait l'etincelle
    RelightHearth,  // le foyer froid a portee repart pour une duree de combustion
    FeedHearth,     // le foyer allume a portee brule plus longtemps
};

struct CraftIngredient {
    GroundItemKind kind = GroundItemKind::Stone;
    uint8_t count = 0;
    CraftSource source = CraftSource::Carried;
    bool consumed = true; // false = outil (requis, non consomme)
};

struct Recipe {
    const char* id;    // stable : journal, tests, future sauvegarde
    const char* label; // francais, pour les messages ("un foyer")
    std::array<CraftIngredient, 3> in;
    uint8_t inCount;
    bool hasOutput;
    GroundItemKind out;
    CraftStation station;
    CraftEffect effect;
    const char* successMessage; // francais, pret a journaliser
};

// Portee des objets POSES comptes par une recette : la meme que le ramassage
// (kPickupReach de main.cpp, 2,5 m) - ce qu'on peut ramasser, on peut
// l'assembler.
inline constexpr float kCraftReach = 2.5f;
// Portee d'un POSTE (foyer), mesuree depuis le joueur (NOTES-PHASE28-DESIGN §2 P4).
inline constexpr float kStationReach = 2.0f;
// Distance du depot devant le joueur : celle de la percussion (Knapping.cpp).
inline constexpr float kCraftDropDistance = 1.1f;

// La table. Ordre = priorite (complexite decroissante). Voir NOTES-PHASE28.md.
inline constexpr std::array<Recipe, 10> kRecipes = { {
    // PHASE 28 (Agent 3), conception P6 : l'abri, la plus grosse recette, EN
    // TETE (complexite decroissante). Objet FIXE : effet NeedsContext::sheltered.
    { "shelter", "un abri",
      { { { GroundItemKind::Log, 4, CraftSource::Placed, true },
          { GroundItemKind::Branch, 6, CraftSource::Placed, true },
          { GroundItemKind::Cord, 2, CraftSource::Placed, true } } },
      3, true, GroundItemKind::Shelter, CraftStation::None, CraftEffect::None,
      "Abri : deux buches en montants, une en faitiere, une a la base ; les branches "
      "couchees contre la faitiere, les ligatures serrees aux noeuds." },
    { "hearth", "un foyer",
      { { { GroundItemKind::Stone, 3, CraftSource::Placed, true },
          { GroundItemKind::Branch, 2, CraftSource::Placed, true },
          { GroundItemKind::Flake, 1, CraftSource::Carried, false } } },
      3, true, GroundItemKind::Hearth, CraftStation::None, CraftEffect::LightNewHearth,
      "Foyer : les pierres en cercle, le petit bois croise ; l'eclat frappe la pierre, "
      "le feu prend." },
    { "hearth_relight", "rallumer le foyer",
      { { { GroundItemKind::Branch, 2, CraftSource::Placed, true },
          { GroundItemKind::Flake, 1, CraftSource::Carried, false },
          {} } },
      2, false, GroundItemKind::Hearth, CraftStation::ColdHearth, CraftEffect::RelightHearth,
      "Foyer : du petit bois neuf, une etincelle, le feu repart." },
    // PHASE 28 (Agent 3), conception P5 : hache + poix, posees. Avant "axe" :
    // plus complexe (elle consomme une hache).
    { "pitched_axe", "une hache renforcee",
      { { { GroundItemKind::Axe, 1, CraftSource::Placed, true },
          { GroundItemKind::Pitch, 1, CraftSource::Placed, true },
          {} } },
      2, true, GroundItemKind::PitchedAxe, CraftStation::None, CraftEffect::None,
      "Hache renforcee : la poix noie la ligature et colle l'eclat au manche." },
    { "axe", "une hache",
      { { { GroundItemKind::Flake, 1, CraftSource::Placed, true },
          { GroundItemKind::Branch, 1, CraftSource::Placed, true },
          { GroundItemKind::Cord, 1, CraftSource::Placed, true } } },
      3, true, GroundItemKind::Axe, CraftStation::None, CraftEffect::None,
      "Hache : l'eclat est cale dans la fente de la branche et serre par la ligature." },
    { "pitch", "de la poix",
      { { { GroundItemKind::Resin, 2, CraftSource::Placed, true },
          { GroundItemKind::Ash, 1, CraftSource::Placed, true },
          {} } },
      2, true, GroundItemKind::Pitch, CraftStation::LitHearth, CraftEffect::None,
      "Poix : la resine fond pres du feu, la cendre s'y melange ; une colle sombre fige." },
    { "cord", "une ligature",
      { { { GroundItemKind::Fiber, 3, CraftSource::Carried, true }, {}, {} } },
      1, true, GroundItemKind::Cord, CraftStation::None, CraftEffect::None,
      "Ligature : trois tiges torsadees ensemble donnent un lien solide, pose a vos pieds." },
    { "hearth_feed_log", "alimenter le foyer",
      { { { GroundItemKind::Log, 1, CraftSource::Placed, true }, {}, {} } },
      1, false, GroundItemKind::Hearth, CraftStation::LitHearth, CraftEffect::FeedHearth,
      "Foyer : la buche est posee sur les braises, le feu durera plus longtemps." },
    { "hearth_feed_branch", "alimenter le foyer",
      { { { GroundItemKind::Branch, 1, CraftSource::Placed, true }, {}, {} } },
      1, false, GroundItemKind::Hearth, CraftStation::LitHearth, CraftEffect::FeedHearth,
      "Foyer : la branche est jetee au feu, il durera plus longtemps." },
    // Recette "numero 0" historique (Phase 6), placee EN DERNIER : c'est la plus simple.
    { "knap", "un eclat",
      { { { GroundItemKind::Stone, 1, CraftSource::Carried, true },   // nucleus
          { GroundItemKind::Stone, 1, CraftSource::Carried, false },  // percuteur
          {} } },
      2, true, GroundItemKind::Flake, CraftStation::None, CraftEffect::None,
      "Percussion : un eclat tranchant se detache et tombe a vos pieds." },
} };

// ---------------------------------------------------------------------------
// PHASE 28 (Agent 3) : les EFFETS des objets faconnes, en fonctions pures.
// ---------------------------------------------------------------------------
// Rendement de la hache (conception P3/P5) : buches SUPPLEMENTAIRES par arbre
// debite, lues par Forest::scheduleBucking via Forest::setBuckingBonusLogs.
// La hache n'est PAS une condition d'abattage (scenario Vulkan A intact).
inline constexpr int kAxeBonusLogs = 1;
inline constexpr int kPitchedAxeBonusLogs = 2;
// Hache renforcee portee -> 2 ; sinon hache portee -> 1 ; sinon 0. Pas de
// cumul : on abat avec UN outil, le meilleur.
int axeBonusLogs(const Inventory& inventory);

// Abri (conception P6) : le joueur est abrite a moins de kShelterReach
// (horizontal) d'un abri pose, avec la meme tolerance verticale que le
// ramassage (3 m) - un parapente qui survole l'abri n'est pas abrite.
inline constexpr float kShelterReach = 2.5f;
inline constexpr float kShelterVerticalTolerance = 3.0f;
bool shelterNear(const Ground& ground, const glm::vec3& position);

// Indice d'une recette par son id, ou -1.
int findRecipe(const char* id);

// ---------------------------------------------------------------------------
// Foyers : l'etat de combustion, tenu A PART du Ground (qui reste un sac de
// POD sans etat), sur le modele de ResinStand qui garde la memoire des arbres
// saignes hors de la Forest. Un foyer est identifie par sa POSITION : les
// indices du Ground ne sont pas stables (pickUp echange avec le dernier), un
// foyer, lui, ne bouge jamais.
//
// Le temps est celui du JEU (GameClock::totalHours), pas le temps reel : un
// feu dure une heure de jeu, que l'horloge soit acceleree ou gelee.
// ---------------------------------------------------------------------------
struct HearthParams {
    float burnHours = 1.0f;     // duree d'un allumage (proposition du document)
    float feedHours = 1.0f;     // gagne par branche ou buche ajoutee
    // --- POINTS D'ACCROCHE (Agent 3), NON BRANCHES en Phase 28 --------------
    // Chaleur au centre et rayon ou elle s'annule (decroissance lineaire),
    // proposes par NOTES-PHASE28-DESIGN §2 P1. Rien ne lit warmthAt dans
    // main.cpp a ce stade.
    float warmthC = 8.0f;
    float warmthRadius = 6.0f;
};

struct HearthState {
    glm::vec3 position{0.0f};  // celle de l'objet Hearth du Ground
    bool lit = false;
    float burnsUntilHours = 0.0f; // heure de jeu d'extinction (si lit)
};

class HearthSet {
public:
    explicit HearthSet(const HearthParams& params = {}) : params_(params) {}

    // Enregistre un foyer neuf, ALLUME a nowHours.
    void addLit(const glm::vec3& position, float nowHours);

    // Foyer le plus proche (horizontal) a portee, allume ou non selon wantLit ;
    // -1 si aucun.
    int nearest(const glm::vec3& position, float reach, bool wantLit) const;

    void relight(int index, float nowHours);
    void feed(int index);

    // Fait avancer les combustions : tout foyer allume dont l'heure est passee
    // s'eteint et laisse UNE cendre au sol, a son centre. Renvoie le nombre de
    // foyers eteints a cet appel.
    int update(float nowHours, Ground& ground);

    // Heures de combustion restantes (0 si eteint ou indice invalide).
    float remainingHours(int index, float nowHours) const;

    // POINT D'ACCROCHE pour l'Agent 3 (chaleur -> NeedsContext::ambientTemperatureC).
    // Somme des foyers allumes, chacun warmthC au centre, 0 a warmthRadius.
    // Fonction pure ; NON appelee par le jeu en Phase 28.
    float warmthAt(const glm::vec3& position) const;

    const std::vector<HearthState>& hearths() const { return hearths_; }
    const HearthParams& params() const { return params_; }

private:
    HearthParams params_;
    std::vector<HearthState> hearths_;
};

// ---------------------------------------------------------------------------
// Le geste
// ---------------------------------------------------------------------------
enum class CraftResult {
    Success,
    MissingIngredients, // aucune recette satisfaite ; le message dit ce qui manque
};

struct CraftOutcome {
    CraftResult result = CraftResult::MissingIngredients;
    int recipe = -1;          // recette executee, ou la plus proche en cas de refus
    std::string message;      // francais, pret a journaliser
};

// facing : direction du regard (projetee et normalisee ici, avec repli si
// verticale, exactement comme knapStones). nowHours : GameClock::totalHours.
CraftOutcome craftAt(Inventory& inventory, Ground& ground, HearthSet& hearths,
                     const glm::vec3& playerPosition, const glm::vec3& facing,
                     float nowHours);

// ---------------------------------------------------------------------------
// Reposer un objet porte (PHASE 28) : le geste manquant sans lequel le foyer
// et la hache - assembles AU SOL - seraient injouables.
// ---------------------------------------------------------------------------
// Nature a reposer : `preferred` (le dernier objet ramasse) si le joueur en
// porte encore, sinon la premiere nature portee dans l'ordre de l'enum. false
// si l'inventaire est vide.
bool chooseKindToPutDown(const Inventory& inventory, GroundItemKind preferred,
                         GroundItemKind* kindOut);

// Pose un exemplaire de `kind` a kPutDownDistance devant le joueur. Si des
// objets y sont deja, le point est decale en spirale (angle d'or) pour que les
// pierres d'un futur foyer ne se superposent pas exactement. false (et rien ne
// change) si le joueur n'en porte pas ou si l'objet n'est pas portable.
inline constexpr float kPutDownDistance = 0.9f;
bool putDown(Inventory& inventory, Ground& ground, GroundItemKind kind,
             const glm::vec3& playerPosition, const glm::vec3& facing);

} // namespace vercora
