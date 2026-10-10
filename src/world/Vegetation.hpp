#pragma once

#include "renderer/InstanceData.hpp"
#include "world/GroundCover.hpp"
#include "world/TreeSpecies.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vercora {

class HeightSampler;
struct Frustum;

// Reglages du semis de vegetation de decor. Tous les seuils sont nommes ici
// plutot que disperses dans le .cpp : c'est le tableau de bord de la Phase 11,
// et NOTES-PHASE11.md renvoie a ces noms.
struct VegetationParams {
    // --- Arbres -------------------------------------------------------------
    // Cote d'une cellule de semis. C'est l'unite de travail : une cellule est
    // engendree d'un bloc, mise en cache, et cullee d'un bloc.
    float treeCell = 24.0f;

    // Nombre d'emplacements d'arbre engendres par cellule. Le nombre REELLEMENT
    // retenu depend du couvert local et de la distance (voir plus bas).
    // 8 emplacements sur 24 m de cote = au plus un arbre pour 72 m2, soit une
    // futaie dense mais pas un mur.
    uint32_t treeSlots = 8;

    // Portee de rendu de l'emplacement 0 (le plus persistant) et du dernier.
    // C'est ICI que se joue le niveau de detail de DENSITE : chaque emplacement
    // a sa PROPRE portee, interpolee entre ces deux bornes. Les arbres
    // disparaissent donc un par un et progressivement quand on s'eloigne, au
    // lieu que toute une cellule change de compte d'un coup - ce qui serait
    // visible comme un clignotement de bosquet.
    float treeDistanceFar = 1100.0f;
    float treeDistanceNear = 320.0f;

    // Couvert (part de retours de vegetation, voir tools/lidar_to_canopy.py)
    // en dessous duquel aucune cellule n'est boisee, et au-dessus duquel elle
    // l'est au maximum. Entre les deux, la densite croit lineairement.
    float coverMin = 0.20f;
    float coverFull = 0.75f;

    // Bornes de hauteur d'arbre, en metres. La carte de canopee donne la hauteur
    // du plus grand arbre de sa cellule ; les emplacements d'une meme cellule se
    // repartissent en dessous, pour que la futaie ne soit pas une brosse.
    float treeMinHeight = 4.5f;
    float treeMaxHeight = 32.0f;

    // Pente maximale (en tangente) sur laquelle un arbre de decor accepte de
    // pousser. Meme role que ForestParams::maxSlope, mais plus permissif : un
    // arbre de decor n'a pas a etre abattable, seulement a ne pas flotter.
    float treeMaxSlope = 1.1f;

    // --- Herbe --------------------------------------------------------------
    // Cellule de semis de l'herbe et emplacements par cellule. Au maximum
    // 18 touffes par cellule de 2 m, soit 4,5 touffes/m2 ; la densite reellement
    // retenue vaut la moitie environ (voir gatherGrass). Les premiers reglages
    // (cellule de 3 m, 6 emplacements) donnaient moins d'une touffe par 9 m2 :
    // des brins isoles au milieu du vide, pas un pre.
    float grassCell = 1.0f;

    // PHASE 18 : 24 -> 56 emplacements. Au maximum 14 touffes/m2 sur une cellule
    // de 1 m, et la densite REELLEMENT retenue vaut `density * grassSlots`, donc
    // environ 5 touffes/m2 sur un alpage ordinaire de cette zone (2 avant).
    //
    // POURQUOI AGIR ICI ET PAS SUR LES SEUILS DE SELECTION. Baisser
    // grassClusterThreshold ou grassMaxSlope aurait aussi produit plus de
    // touffes - mais en en mettant LA OU IL N'Y EN A PAS : sur le rocher, sous
    // la futaie dense, sur les versants que la carte LiDAR declare nus. Le
    // reproche de la Phase 11 n'etait pas que l'herbe manquait d'emprise, il
    // etait qu'elle formait un semis clairseme LA OU ELLE POUSSE. Le nombre
    // d'emplacements est le seul reglage qui densifie sans deplacer une seule
    // frontiere : la carte de repartition de la Phase 11 est litteralement
    // inchangee, seul son remplissage l'est.
    uint32_t grassSlots = 56;

    // PHASE 18 : force de la DECOUPE ALPHA des brins, entre 0 et 1 (voir
    // shaders/foliage.frag). 0 rend exactement les quads opaques de la Phase 11 :
    // c'est le TEMOIN qui permet de mesurer ce que la decoupe coute reellement,
    // sur le meme binaire et la meme vue (option de jeu --no-grass-cutout).
    float grassCutout = 1.0f;

    // Portee volontairement COURTE : l'herbe n'a pas a etre visible a des
    // kilometres, contrairement au terrain et aux arbres, et c'est de loin le
    // poste le plus couteux en nombre d'instances. Le fondu occupe toute la
    // bande entre les deux valeurs.
    float grassDistanceFar = 34.0f;
    float grassDistanceNear = 14.0f;

    // Taille d'une touffe au plus bas et au plus haut de la zone, en metres.
    // C'est la traduction directe de "vegetation alpine rase en altitude".
    float grassSizeLow = 0.30f;
    float grassSizeHigh = 0.11f;

    // Altitudes, en metres, entre lesquelles l'herbe se rarefie et rapetisse.
    // Reglees sur la zone reelle (1029 a 2285 m) : dense en dessous de la
    // premiere, absente au-dessus de la seconde.
    // Reglees sur CE massif : la limite des arbres y est vers 1800-1900 m et
    // l'alpage reste dense bien au-dessus. Les premieres valeurs (1500 / 2150)
    // etaient calquees sur les bornes d'altitude de la zone plutot que sur sa
    // vegetation reelle, et supprimaient l'herbe precisement la ou le massif est
    // ouvert - les alpages et les pistes de ski du nord-ouest.
    float grassAltitudeLush = 1750.0f;
    float grassAltitudeBarren = 2250.0f;

    // Pente maximale portant de l'herbe. 1,3 en tangente ~ 52 degres : une
    // pelouse d'alpage tient sur des versants bien plus raides qu'on ne le
    // croit, et le premier reglage (0,9) rejetait 42 % des cellules d'un
    // versant herbeux ordinaire de ce massif.
    float grassMaxSlope = 1.3f;

    // Part de la densite d'herbe qui subsiste SOUS UNE CANOPEE HAUTE. Zero
    // voudrait dire qu'il ne pousse rien en foret, ce qui est faux : une futaie
    // de montagne est claire et son sol porte de l'herbe. C'etait pourtant le
    // premier reglage, et il vidait 79 % des cellules d'un versant boise.
    float grassUnderCanopy = 0.35f;

    // Frequence spatiale du bruit qui module la densite d'herbe, en cycles par
    // metre. 1/14 m : les taches d'herbe font une quinzaine de metres. Le premier
    // reglage (1/45 m) produisait des taches si larges qu'a hauteur d'oeil on
    // n'en voyait qu'une seule : l'impression n'etait plus celle de touffes mais
    // d'une moitie de paysage herbeuse et d'une moitie nue.
    float grassClusterFrequency = 1.0f / 14.0f;

    // Seuil du bruit au-dessus duquel une zone est herbeuse. Plus il est haut,
    // plus les touffes sont groupees et separees par du sol nu.
    float grassClusterThreshold = 0.25f;

    // Largeur de la rampe au-dessus du seuil : c'est elle qui decide si le bord
    // d'une tache d'herbe est franc (valeur petite) ou degrade (valeur grande).
    float grassClusterSoftness = 0.35f;

    // Amplitude d'ondulation, en metres, a la pointe d'un brin.
    float grassSway = 0.06f;

    // --- Commun -------------------------------------------------------------
    // Deux Vegetation de meme graine sont identiques : le decor ne bouge pas
    // d'une execution a l'autre, ce qui rend les captures comparables.
    uint32_t seed = 20260821u;

    // PHASE 26 : reglages des ESSENCES d'arbres. Le semis de decor ne decide pas
    // lui-meme de la part de resineux : il delegue a world/TreeSpecies.hpp, qui
    // est aussi ce que consultent les arbres abattables et la recolte de resine.
    // Une seule loi pour les trois.
    TreeSpeciesParams species;

    // Cellules d'arbres engendrees au plus par appel. Un budget, comme celui du
    // TerrainStreamer : sans lui, le premier passage dans une zone vierge
    // engendrerait des milliers de cellules d'un coup et ferait un a-coup
    // visible. Les cellules manquantes arrivent aux images suivantes.
    uint32_t treeCellBudget = 96;
};

// Vegetation de DECOR : les dizaines de milliers d'arbres et les centaines de
// milliers de touffes d'herbe qui habillent les 25 km2, par opposition aux
// quelques arbres ABATTABLES de world/Forest.hpp.
//
// POURQUOI DEUX SYSTEMES ET NON UN SEUL. Forest tient un vecteur d'arbres, et
// resolve()/pickTree() le parcourent en entier a chaque appel : c'est parfait
// pour seize arbres avec lesquels on interagit, et intenable pour cinquante
// mille. Surtout, un arbre de Forest porte un ETAT (debout, en chute, en
// debitage) qu'il faut conserver, alors qu'un arbre de decor n'en a aucun. Les
// fusionner obligerait a payer le prix du plus exigeant des deux partout. La
// promotion d'un arbre de decor en arbre abattable quand le joueur s'en approche
// est le prolongement naturel de cette architecture ; elle n'est PAS faite dans
// cette phase (voir NOTES-PHASE11.md).
//
// AUCUN ARBRE N'EST STOCKE POUR LES 25 KM2. Le semis est une FONCTION
// DETERMINISTE de la cellule : engendrer la cellule (17, -42) donne toujours les
// memes arbres, quelle que soit la facon dont on y est arrive. La memoire ne
// contient donc que les cellules proches du joueur, et il n'y a rien a
// sauvegarder ni a charger. C'est le meme principe que le quadtree du terrain,
// applique au semis plutot qu'au maillage.
//
// Ne connait RIEN de Vulkan : produit des InstanceData, c'est main.cpp qui les
// remet au Renderer. Testable sans GPU.
class Vegetation {
public:
    // `terrain` fournit l'altitude et la pente ; il n'est pas possede et doit
    // vivre plus longtemps.
    // `cover` et `canopyHeight` sont les cartes produites par
    // tools/lidar_to_canopy.py, echantillonnees en coordonnees MONDE comme le
    // terrain (elles partagent son origine et son emprise). Toutes deux peuvent
    // etre nulles : le semis retombe alors sur un substitut par pente et
    // altitude, documente dans NOTES-PHASE11.md.
    // `terrainExtent` = demi-cote du terrain, pour ne pas semer hors des donnees.
    Vegetation(const HeightSampler& terrain, const HeightSampler* cover,
               const HeightSampler* canopyHeight, float terrainExtent,
               const VegetationParams& params = {});

    // Remplit `out` (vide d'abord) des arbres visibles depuis `eye`. Les
    // cellules hors du frustum ne sont pas engendrees du tout : c'est le
    // frustum culling qui evite le gros du travail, exactement comme pour le
    // terrain.
    void gatherTrees(const glm::vec3& eye, const Frustum& frustum,
                     std::vector<InstanceData>& out);

    // PHASE 26 : meme chose, mais TRIEE PAR ESSENCE. Les deux essences ont des
    // geometries distinctes (world/TreeMesh.hpp), donc deux appels de dessin
    // instancies au lieu d'un : le rendu instancie tient UNE geometrie pour N
    // instances, c'est sa definition meme, et il n'y a pas de facon de melanger
    // deux formes dans un seul appel sans reintroduire une matrice par arbre -
    // c'est-a-dire sans defaire la Phase 11.
    //
    // Les deux vecteurs sont vides d'abord. La surcharge a un seul vecteur
    // ci-dessus reste EXACTE et sert encore (tests, estimation de densite) :
    // elle rend l'union des deux, dans le meme ordre qu'avant cette phase.
    void gatherTrees(const glm::vec3& eye, const Frustum& frustum,
                     std::vector<InstanceData>& conifers,
                     std::vector<InstanceData>& broadleaves);

    // Idem pour l'herbe. PAS de cache : la portee etant courte (une soixantaine
    // de metres), le nombre de cellules concernees est petit et l'engendrement
    // direct coute moins cher que la gestion d'un cache et son eviction.
    void gatherGrass(const glm::vec3& eye, const Frustum& frustum,
                     std::vector<InstanceData>& out) const;

    // PHASE 17 : etat du sol. Branche l'HERBE - et elle seule - sur une source
    // de couvert (world/Habitat.hpp en pratique), qui la rarefie la ou le
    // milieu a ete degrade. Nul par defaut, et un Vegetation sans source se
    // comporte EXACTEMENT comme avant la Phase 17 (voir le contrat total de
    // GroundCoverSource) : c'est ce qui rend la non-regression verifiable.
    //
    // Pourquoi l'herbe et pas les arbres : les arbres de decor sont la FORET,
    // et la foret ne disparait pas parce qu'on en a coupe trois pieds - ce sont
    // les arbres ABATTABLES de Forest qui tombent, et ils tombent un par un,
    // visiblement. Ce qui change apres une coupe, c'est le sol : pietine,
    // debarde, mis a nu. Faire disparaitre les arbres de decor aurait efface la
    // coupe au lieu de la montrer.
    //
    // La source n'est PAS possedee et doit vivre plus longtemps que la
    // Vegetation.
    void setGroundCover(const GroundCoverSource* source) { groundCover_ = source; }
    const GroundCoverSource* groundCoverSource() const { return groundCover_; }

    // PHASE 26 : source d'exposition adret/ubac, qui decide de l'ESSENCE d'un
    // arbre - et d'elle seule.
    //
    // CE QU'ELLE NE TOUCHE PAS, ET C'EST LE POINT. La PRESENCE d'un arbre reste
    // pilotee exactement comme depuis la Phase 11 : par la carte de couvert
    // LiDAR (coverAt), la pente et l'altitude. L'exposition n'entre nulle part
    // dans buildTreeCell avant que l'emplacement ne soit DEJA retenu ; elle ne
    // peut donc ni ajouter ni retirer un seul arbre. Un versant boise le reste,
    // seule la composition de son peuplement change.
    //
    // Nulle par defaut : le semis rend alors la part de resineux neutre partout,
    // donc la meme repartition qu'un tirage sans exposition. La source n'est PAS
    // possedee et doit vivre plus longtemps que la Vegetation.
    //
    // A APPELER AVANT le premier gatherTrees : l'essence est mise en cache avec
    // la cellule, comme le reste du semis. Changer la source ensuite purge le
    // cache, pour qu'aucune cellule ne garde une essence calculee sous l'ancienne.
    void setAspectExposure(const AspectExposureSource* source);
    const AspectExposureSource* aspectExposureSource() const { return aspectExposure_; }

    // --- Diagnostic ---------------------------------------------------------
    std::size_t cachedCellCount() const { return cache_.size(); }
    uint32_t lastTreeCount() const { return lastTreeCount_; }
    uint32_t lastConiferCount() const { return lastConiferCount_; }     // PHASE 26
    uint32_t lastBroadleafCount() const { return lastBroadleafCount_; } // PHASE 26
    uint32_t lastGrassCount() const { return lastGrassCount_; }

    // Densite d'arbres estimee sur toute la zone, en arbres par hectare, obtenue
    // en engendrant un echantillon regulier de cellules. Sert au journal de
    // demarrage et aux tests : c'est le seul chiffre qui dit si la carte de
    // couvert a reellement ete prise en compte.
    // `step` = une cellule sondee sur `step` dans chaque direction.
    double estimateTreeDensityPerHectare(uint32_t step = 7) const;

    const VegetationParams& params() const { return params_; }

private:
    struct CellKey {
        int32_t x = 0;
        int32_t z = 0;
        bool operator==(const CellKey& other) const { return x == other.x && z == other.z; }
    };
    struct CellKeyHash {
        std::size_t operator()(const CellKey& key) const {
            // Melange simple : les deux moities occupent des bits distincts.
            return (static_cast<std::size_t>(static_cast<uint32_t>(key.x)) * 0x9E3779B1ull)
                 ^ (static_cast<std::size_t>(static_cast<uint32_t>(key.z)) * 0x85EBCA77ull);
        }
    };

    // Emplacements CANDIDATS d'une cellule, avec leur portee individuelle. Ce
    // qui est mis en cache est le resultat du semis, pas ce qui est visible :
    // la distance et le frustum sont reevalues a chaque image.
    struct Candidate {
        glm::vec3 position{0.0f};
        float height = 0.0f;
        float yaw = 0.0f;
        float variation = 0.0f;
        float maxDistance = 0.0f; // portee propre a l'emplacement
        TreeSpecies species = TreeSpecies::Conifer; // PHASE 26
    };
    struct Cell {
        std::vector<Candidate> candidates;
        glm::vec3 boxMin{0.0f};
        glm::vec3 boxMax{0.0f};
    };

    // Engendre (ou retrouve) une cellule d'arbres.
    const Cell& treeCell(int32_t cx, int32_t cz);
    Cell buildTreeCell(int32_t cx, int32_t cz) const;

    // Pente du terrain en un point, par differences finies. Meme definition que
    // Forest::slopeAt : les deux filtres doivent parler de la meme grandeur.
    float slopeAt(float worldX, float worldZ) const;

    // Couvert et hauteur de canopee en un point, avec leur substitut quand les
    // cartes LiDAR ne sont pas fournies.
    float coverAt(float worldX, float worldZ) const;
    float canopyHeightAt(float worldX, float worldZ) const;

    const HeightSampler& terrain_;
    const HeightSampler* cover_ = nullptr;
    const HeightSampler* canopyHeight_ = nullptr;
    const GroundCoverSource* groundCover_ = nullptr; // PHASE 17, optionnel
    const AspectExposureSource* aspectExposure_ = nullptr; // PHASE 26, optionnel
    float terrainExtent_ = 0.0f;
    VegetationParams params_;

    // Cache des cellules d'arbres. Purge des cellules trop lointaines a chaque
    // appel de gatherTrees : sans elle, traverser la zone entiere finirait par
    // tout garder en memoire, ce qui annulerait l'interet du semis a la demande.
    std::unordered_map<CellKey, Cell, CellKeyHash> cache_;

    uint32_t lastTreeCount_ = 0;
    // Comptes par essence du dernier gatherTrees, pour le journal et les tests :
    // sans eux, "il y a plus de resineux en ubac" ne serait qu'une impression.
    uint32_t lastConiferCount_ = 0;
    uint32_t lastBroadleafCount_ = 0;
    mutable uint32_t lastGrassCount_ = 0;
};

} // namespace vercora
