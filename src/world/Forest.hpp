#pragma once

#include "core/CollisionSolver.hpp"
#include "world/GroundItem.hpp"
#include "world/Tree.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace vercora {

class Ground;
class HeightSampler;

// Indice d'arbre invalide, renvoye par pickTree quand la visee ne touche rien.
inline constexpr int kNoTree = -1;

struct ForestParams {
    // Nombre d'arbres de test. Volontairement modeste : le but de cette phase
    // est la logique d'abattage, pas une vraie foret.
    uint32_t treeCount = 16;

    // Les arbres sont semes dans un disque de ce rayon autour de l'origine
    // (ou demarre la camera), mais pas trop pres d'elle.
    float areaRadius = 34.0f;
    float minDistanceFromStart = 7.0f;

    // Distance minimale entre deux pieds d'arbre. Sous cette valeur, deux
    // houppiers s'interpenetrent et la detection de zone degagee n'a plus de
    // sens (tout arbre bloquerait tout arbre).
    float minSpacing = 5.5f;

    // Bornes de hauteur en unites monde (= metres, cf. la convention 1 unite =
    // 1 m posee par la hauteur d'oeil de la camera).
    float minHeight = 5.5f;
    float maxHeight = 9.5f;

    // Pente maximale du terrain (en tangente : 0.6 ~ 31 degres) sur laquelle un
    // arbre accepte de pousser. Au-dela, le pied de l'arbre flotterait d'un cote.
    float maxSlope = 0.6f;

    // Graine : deux Forest de meme graine sont identiques, donc les tests sont
    // reproductibles (meme principe que Heightmap).
    uint32_t seed = 20260724u;
};

// Resultat de l'analyse de la zone degagee autour d'un arbre.
struct Clearance {
    // Direction horizontale normalisee retenue pour la chute.
    glm::vec3 direction{1.0f, 0.0f, 0.0f};

    // Distance libre trouvee dans cette direction, en unites monde, ecretee a la
    // hauteur de l'arbre (au-dela, ce qui s'y trouve ne le gene plus).
    float distance = 0.0f;

    // true si AUCUNE direction n'offre la place de coucher l'arbre entier.
    // L'abattage reste possible - c'est au gameplay de decider s'il l'autorise -
    // mais le signaler evite de croire que le systeme a trouve un bon degagement.
    bool cramped = true;

    // Ce qui a arrete le rayon retenu, pour les logs.
    std::string obstacle = "aucun";
};

// Les arbres du monde et la logique d'abattage : placement sur le terrain,
// visee au rayon, choix de la direction de chute, animation, debitage.
//
// Ne connait RIEN de Vulkan : elle produit des positions et des etats, c'est
// main.cpp qui les traduit en DrawItem. C'est ce qui permet de tester toute la
// logique sans GPU (voir tests/logic_tests.cpp).
//
// Implemente CollisionSolver : la Forest est la seule a savoir ou sont ses
// troncs et ses souches, c'est donc a elle de dire au joueur qu'il ne peut pas
// passer au travers. La camera, elle, ne voit que l'interface.
class Forest : public CollisionSolver {
public:
    // Ni le terrain ni le sol ne sont possedes : tous deux doivent vivre plus
    // longtemps que la Forest. Le Ground recoit les buches et les branches
    // produites par le debitage des arbres abattus.
    // terrainExtent = demi-cote du terrain, pour ne pas semer hors des donnees.
    // Un treeCount nul donne une foret vide, a peupler par plantTree.
    // `aspectExposure` (PHASE 26) decide de l'ESSENCE de chaque arbre seme.
    // Il est PARAMETRE DU CONSTRUCTEUR et non un simple setter, parce que le
    // constructeur seme immediatement : un setter aurait cree un ordre a
    // respecter, donc un piege - « regler la source apres coup ne fait rien »
    // est exactement le genre de silence dont ce projet a deja paye le prix.
    // Nul = exposition nulle partout, donc la part de resineux neutre : c'est
    // le comportement d'avant la Phase 26, et le TEMOIN de non-regression.
    // La source n'est PAS possedee et doit vivre plus longtemps que la Forest.
    Forest(const HeightSampler& terrain, Ground& ground, float terrainExtent,
           const ForestParams& params = {},
           const AspectExposureSource* aspectExposure = nullptr,
           const TreeSpeciesParams& speciesParams = {});

    // Plante un arbre a une position CHOISIE, pose sur le terrain. Le semis
    // aleatoire du constructeur n'est qu'un appelant parmi d'autres : c'est par
    // ici que passeront un placement decide a la main et, plus tard, la couche
    // de vegetation issue de la classification LiDAR. Aucun filtre de pente ni
    // d'espacement n'est applique - l'appelant demande explicitement cet arbre.
    // Renvoie son indice.
    int plantTree(float worldX, float worldZ, float height, float yaw = 0.0f);

    // --- ESSENCES (PHASE 26) ------------------------------------------------
    //
    // Ne sert QU'AUX ARBRES PLANTES ENSUITE (plantTree) : les arbres deja semes
    // gardent leur essence. C'est pour cela que la source normale passe par le
    // constructeur.
    void setAspectExposure(const AspectExposureSource* source) { aspectExposure_ = source; }

    const TreeSpeciesParams& speciesParams() const { return speciesParams_; }

    // Nombre d'arbres de chaque essence, pour le journal et les tests.
    std::size_t coniferCount() const;

    // --- Visee --------------------------------------------------------------
    // Premier arbre DEBOUT touche par le rayon (origine, direction normalisee),
    // ou kNoTree. Le volume teste est un cylindre vertical englobant, pas la
    // geometrie exacte : viser un arbre a 30 m doit rester jouable.
    int pickTree(const glm::vec3& origin, const glm::vec3& direction,
                 float maxDistance) const;

    // --- Zone degagee -------------------------------------------------------
    // Balaye l'horizon autour de l'arbre par une serie de rayons et retient la
    // direction ou il peut tomber le plus loin sans rien heurter. Sans effet de
    // bord : appelable pour previsualiser avant d'abattre.
    Clearance evaluateClearance(int treeIndex) const;

    // --- Abattage -----------------------------------------------------------
    // Passe l'arbre en chute vers sa zone degagee. Renvoie false si l'indice ne
    // designe pas un arbre debout. Le Clearance retenu ressort par clearanceOut
    // (peut etre nullptr) pour que l'appelant puisse le journaliser.
    bool fell(int treeIndex, Clearance* clearanceOut = nullptr);

    // Fait avancer les chutes, les amortissements et les debitages en cours.
    // deltaTime en secondes.
    void update(float deltaTime);

    // --- CollisionSolver ----------------------------------------------------
    glm::vec3 resolve(const BodyCylinder& body) const override;
    bool overlaps(const BodyCylinder& body) const override;

    const std::vector<Tree>& trees() const { return trees_; }

    // Nombre d'arbres reellement semes (le rejet par pente/espacement peut en
    // produire moins que demande).
    std::size_t treeCount() const { return trees_.size(); }

    // Objets encore en attente d'apparition, le temps que le debitage progresse.
    // Expose pour les tests : un debitage qui n'atteint jamais zero passerait
    // sinon inapercu.
    std::size_t pendingDropCount() const { return pendingDrops_.size(); }

    // --- PHASE 28 (Agent 3) : rendement de la hache ---------------------------
    // Buches SUPPLEMENTAIRES donnees par chaque debitage programme a partir de
    // maintenant (world/Crafting axeBonusLogs : 0 sans hache, 1 hache, 2 hache
    // renforcee). Lu au moment ou l'arbre finit de s'amortir, donc c'est l'outil
    // PORTE A CE MOMENT qui compte. 0 par defaut = comportement anterieur au
    // tirage aleatoire pres (aucun tirage supplementaire). N'affecte PAS fell().
    void setBuckingBonusLogs(int bonus) { buckingBonusLogs_ = bonus < 0 ? 0 : bonus; }
    int buckingBonusLogs() const { return buckingBonusLogs_; }

    // Nombre total de buches d'un arbre de hauteur height : 2 a 4 selon la
    // longueur du tronc (Phase 5), plus le bonus.
    static int buckingLogCount(float height, int bonusLogs);

private:
    // Un objet de sol dont l'apparition est DIFFEREE. C'est ce qui remplace le
    // depot en bloc de la Phase 5 : au lieu que tout le bois surgisse a la
    // milliseconde ou l'arbre touche terre, chaque piece attend son tour et le
    // tronc se debite visiblement du pied vers la cime.
    struct PendingDrop {
        float delay = 0.0f; // secondes restantes
        GroundItemKind kind = GroundItemKind::Log;
        float worldX = 0.0f;
        float worldZ = 0.0f;
        float yaw = 0.0f;
        float roll = 0.0f;
    };

    // Sonde la pente du terrain autour d'un point, par differences finies.
    float slopeAt(float worldX, float worldZ) const;

    // PHASE 26 : essences. Ni l'un ni l'autre n'influe sur OU un arbre pousse -
    // ils ne sont lus que dans plantTree, apres que la position est fixee.
    const AspectExposureSource* aspectExposure_ = nullptr;
    TreeSpeciesParams speciesParams_{};

    // Programme buches et branches sur la trace de l'arbre tombe, avec leurs
    // delais d'apparition respectifs.
    void scheduleBucking(const Tree& tree);

    // Libere les objets dont le delai est ecoule.
    void advancePendingDrops(float deltaTime);

    const HeightSampler& terrain_;
    Ground& ground_;
    float terrainExtent_;
    ForestParams params_;

    std::vector<Tree> trees_;
    std::vector<PendingDrop> pendingDrops_;
    int buckingBonusLogs_ = 0; // PHASE 28 (Agent 3)

    // RNG unique et seede : le placement ET les tirages de la chute en
    // dependent, donc une meme graine rejoue exactement la meme partie.
    // Mutable : evaluateClearance departage les directions ex aequo par tirage
    // tout en restant logiquement une observation (const).
    mutable std::mt19937 rng_;
};

} // namespace vercora
