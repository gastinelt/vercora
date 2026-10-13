#pragma once

#include "core/Frustum.hpp"
#include "renderer/DrawItem.hpp"
#include "renderer/MeshData.hpp"
#include "world/TerrainQuadtree.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vercora {

class Heightmap;
class Renderer;

// Fait le lien entre le quadtree (qui decide QUOI dessiner) et le Renderer (qui
// possede les geometries GPU) : cache de tuiles residentes, construction a la
// demande sous budget, eviction LRU, production de la liste de DrawItem.
//
// PHASE 10. Le point important est la GESTION DE LA RESIDENCE. Le quadtree
// complet de la zone de 25 km2 au pas de 1 m compte ~8300 noeuds ; les mailler
// tous couterait plus cher que le maillage d'un seul bloc de la Phase 9. Seules
// les tuiles reellement selectionnees sont donc construites, et elles sont
// gardees tant que la place le permet.
//
// Trois garde-fous, dans cet ordre d'importance :
//
//  1. BUDGET PAR FRAME (maxUploadsPerFrame). PHASE 12 : ce budget ne borne plus
//     une synchronisation GPU - il n'y en a plus - mais le seul cout restant,
//     qui est le MAILLAGE CPU d'une tuile (65 x 65 sommets a calculer, ~385 Ko a
//     recopier). Il est passe de 4 a 64 pour cette raison. Les tuiles au-dela du
//     budget sont servies dans les frames suivantes, les plus PROCHES d'abord.
//
//  2. PLAFOND DE RESIDENCE (maxResidentNodes). Chaque tuile consomme deux
//     allocations Vulkan (vertex + index), et vkAllocateMemory est plafonne par
//     maxMemoryAllocationCount (typiquement 4096). Le plafond garde une marge
//     confortable en plus de borner la RAM.
//
//  3. EVICTION LRU, jamais sur une tuile utilisee dans la frame courante, ni
//     sur une tuile du sommet epingle (point 4).
//
//  4. REPLI SUR L'ANCETRE (PHASE 12). Une tuile selectionnee mais pas encore
//     construite n'est plus laissee en trou : on remonte sa chaine de peres
//     jusqu'a une tuile DEJA RESIDENTE et c'est celle-la qu'on dessine. Elle
//     couvre exactement la meme surface, en resolution inferieure.
//
//     Le repli REMPLACE tout le sous-arbre, il ne s'y superpose pas : les autres
//     tuiles selectionnees situees sous l'ancetre retenu sont retirees de la
//     frame, meme si elles etaient residentes. Sans cela deux surfaces de
//     niveaux differents se recouvriraient et se disputeraient le z-buffer, ce
//     qui remplacerait un trou par un mouchetage clignotant. L'ensemble dessine
//     reste ainsi une COUPE de l'arbre : chaque metre carre visible est couvert
//     une fois et une seule.
//
//     Pourquoi un ancetre est-il resident ? En partie parce qu'on l'a dessine
//     il y a peu (en s'approchant d'une zone on la voit d'abord en L5, puis L4,
//     puis L3...), mais cette raison-la ne suffit PAS, et c'est une mesure qui
//     l'a montre : les tuiles restees non couvertes lors d'un survol a 900 m/s
//     etaient de niveau 3, 4 et 5, et leurs ancetres de niveau 6 et 7 n'avaient
//     JAMAIS ete dessines - un noeud proche de la racine ne se dessine que vu de
//     plus loin que le terrain n'est grand, ce qui n'arrive jamais.
//
//     D'ou le SOMMET EPINGLE (params_.pinnedTopLevels) : les tuiles des
//     quelques niveaux les plus grossiers sont construites au demarrage et ne
//     sont jamais evincees. Elles sont peu nombreuses (1 + 4 + 16 = 21 tuiles
//     pour trois niveaux, ~8 Mo) et elles rendent le repli STRUCTURELLEMENT
//     toujours disponible : tout noeud selectionne est soit epingle lui-meme,
//     soit descendant d'un noeud epingle. uncoveredNodes() ne peut alors plus
//     valoir autre chose que zero - et il reste mesure pour le prouver.
class TerrainStreamer {
public:
    struct Params {
        TerrainQuadtree::Params quadtree{};

        // Tuiles construites au maximum par frame.
        //
        // PHASE 10 : 4, parce que chaque tuile coutait deux vkQueueWaitIdle.
        // PHASE 12 : 64, une fois ce verrou leve. Ce qui reste est du travail
        // CPU (maillage + memcpy vers le staging), qui ne vide plus la queue
        // graphique. Le budget existe encore pour deux raisons : lisser les
        // a-coups d'une teleportation ou d'un demi-tour brutal, et borner le
        // staging vivant simultanement (64 tuiles ~ 25 Mo).
        uint32_t maxUploadsPerFrame = 64;

        // Plafond de tuiles gardees sur le GPU. 512 tuiles = 1024 allocations
        // Vulkan et ~200 Mo au format de sommet de la Phase 10.
        uint32_t maxResidentNodes = 512;

        // Frames pendant lesquelles le budget par frame est LEVE, au demarrage :
        // le temps de construire la vue initiale d'un coup plutot que de montrer
        // un terrain qui se remplit par morceaux pendant deux secondes.
        uint32_t primingFrames = 1;

        // PHASE 12. Nombre de niveaux les plus grossiers dont TOUTES les tuiles
        // sont construites au demarrage et jamais evincees, racine comprise.
        // 3 -> racine + deux niveaux en dessous, soit au plus 1 + 4 + 16 = 21
        // tuiles (~8 Mo). C'est ce qui garantit qu'un ancetre resident existe
        // toujours, donc qu'un trou est impossible. 0 desactive l'epinglage.
        uint32_t pinnedTopLevels = 3;

        // PHASE 12. Repli sur l'ancetre resident quand une tuile manque.
        // Desactivable pour mesurer l'avant/apres sur le MEME binaire, comme
        // --no-lod le fait pour le quadtree entier (option --no-tile-fallback).
        bool ancestorFallback = true;

        // PHASE 18. false remet la construction d'une tuile dans l'etat
        // d'avant la phase : topologie reconstruite a chaque tuile et cible de
        // morphing recalculee sommet par sommet. TEMOIN de mesure - le
        // maillage produit est identique, seul le travail depense change
        // (option de jeu --no-tile-cache).
        bool cacheTileWork = true;

        // PHASE 19. Degre de parallelisme du MAILLAGE CPU des tuiles, thread
        // appelant compris. 1 = tout sequentiel, c'est-a-dire exactement le
        // comportement d'avant la Phase 19 (temoin --tile-threads=1).
        //
        // Ce qui est parallelise, et ce qui ne l'est PAS :
        //
        //   - terrainNodeMeshData est PURE : elle ne lit que la heightmap et le
        //     quadtree, tous deux const et immuables pendant toute la session,
        //     et elle ecrit dans un MeshData qui lui appartient. Deux appels sur
        //     deux tuiles differentes ne partagent STRICTEMENT rien. C'est elle,
        //     et elle seule, qui part sur les threads ;
        //
        //   - Renderer::addMesh reste sur le THREAD DE RENDU, appele en sequence
        //     et dans le meme ordre qu'avant. Aucun appel Vulkan ne change de
        //     thread dans cette phase. Voir NOTES-PHASE19.md pour le detail de
        //     ce qui empecherait addMesh d'etre appele depuis plusieurs threads,
        //     et pourquoi la Phase 19 n'y a pas touche.
        //
        // POURQUOI LE DEFAUT EST 1, ET NON 4 - c'est le resultat le plus
        // important de la Phase 19, et il contredit la piste ouverte par la
        // Phase 18 :
        //
        //   La Phase 18 a mesure que la geometrie faisait 83 % du cout d'une
        //   tuile et a designe la parallelisation comme le gain suivant. Ce
        //   chiffre a ete releve en DEBUG, la seule configuration ou tournent
        //   les couches de validation et donc toutes les mesures du projet. En
        //   RELEASE, le meme releve donne un tout autre partage :
        //
        //     Debug   : geometrie 1344 us/tuile, indices 16, televersement 250
        //     Release : geometrie  140 us/tuile, indices 15, televersement 262
        //
        //   La geometrie est 9,6 fois moins chere une fois optimisee ; ce n'est
        //   plus elle le poste dominant d'une tuile, c'est le TELEVERSEMENT
        //   (63 %). Paralleliser la geometrie revenait donc a decouper le petit
        //   morceau.
        //
        //   Mesure directe, survol a 900 m/s, meme binaire Release, deux
        //   passages chacun (option --tile-threads) :
        //     1 thread  : 819 / 811 ms de temps de mur, 32206 / 32327 images
        //     4 threads : 1227 / 870 ms de temps de mur, 29776 / 32104 images
        //   La creation des threads (environ 3900 sur le survol) coute plus que
        //   le travail qu'elle repartit, parce qu'une tuile Release ne dure que
        //   140 us. Le parallelisme est neutre au mieux, perdant au pire.
        //
        // Le chemin parallele est CONSERVE, teste et documente, parce qu'il
        // reste un gain net la ou une tuile est chere : en Debug il donne un
        // facteur 2,4 a 2,6 et +14 % d'images, ce qui accelere reellement les
        // campagnes de mesure et de validation du projet. Il redeviendra
        // interessant en Release le jour ou une tuile coutera plus cher (format
        // de sommet plus lourd, pas LiDAR plus fin). D'ici la, le defaut est
        // celui que la mesure designe.
        //
        // Une valeur superieure a 1 n'ouvre AUCUN nouveau chemin Vulkan : voir
        // meshBatch. Le risque de ce reglage est donc un risque de performance,
        // pas un risque de correction.
        uint32_t meshThreads = 1;
    };

    // Ni la Heightmap ni le Renderer ne sont possedes ; les deux doivent vivre
    // plus longtemps que le streamer.
    TerrainStreamer(const Heightmap& heightmap, Renderer& renderer,
                    const Params& params);
    // Parametres par defaut : surcharge et non `= {}`, voir Heightmap() (refus
    // de GCC/Clang, CWG 1397).
    TerrainStreamer(const Heightmap& heightmap, Renderer& renderer)
        : TerrainStreamer(heightmap, renderer, Params{}) {}
    ~TerrainStreamer();

    TerrainStreamer(const TerrainStreamer&) = delete;
    TerrainStreamer& operator=(const TerrainStreamer&) = delete;

    // Selectionne, construit ce qui manque (sous budget) et AJOUTE les DrawItem
    // des tuiles dessinables a `out`. `out` n'est pas vide au prealable : le
    // terrain n'est qu'une partie de la scene.
    void appendDrawItems(const glm::vec3& cameraPosition, const Frustum& frustum,
                         std::vector<DrawItem>& out);

    const TerrainQuadtree& quadtree() const { return quadtree_; }

    // --- Statistiques de la derniere frame, pour les logs et la mesure -------
    uint32_t selectedNodes() const { return selectedCount_; }
    uint32_t drawnNodes() const { return drawnCount_; }
    // Tuiles selectionnees qui n'etaient pas encore construites a cette frame.
    uint32_t pendingNodes() const { return pendingCount_; }
    // Tuiles d'ancetre dessinees a la place d'un sous-arbre incomplet (PHASE 12).
    uint32_t fallbackNodes() const { return fallbackCount_; }
    // LE chiffre qui compte : tuiles selectionnees que RIEN ne couvre, ni
    // elles-memes ni un ancetre. C'est la mesure objective d'un trou visuel.
    uint32_t uncoveredNodes() const { return uncoveredCount_; }
    uint32_t residentNodes() const { return static_cast<uint32_t>(resident_.size()); }
    uint32_t drawnTriangles() const {
        return drawnCount_ * quadtree_.trianglesPerNode();
    }
    // Octets de geometrie residente sur le GPU (sommets + indices).
    std::size_t residentBytes() const { return residentBytes_; }

    // Ligne de statistiques lisible, journalisee periodiquement.
    std::string statsLine() const;

    // PHASE 18. Bilan du COUT de construction d'une tuile, journalise a la
    // destruction. Separe volontairement en trois postes, parce que la Phase 12
    // a conclu que le maillage CPU etait devenu le goulot sans jamais le mesurer
    // poste par poste :
    //
    //   - GEOMETRIE  : terrainNodeMeshData, c'est-a-dire l'echantillonnage de la
    //                  heightmap et le calcul des normales et des cibles de
    //                  morphing ;
    //   - INDICES    : la topologie de la tuile, identique pour TOUTES les
    //                  tuiles de tous les niveaux ;
    //   - TELEVERSEMENT : Renderer::addMesh, donc le chemin GPU deja traite en
    //                  Phase 12.
    //
    // Sans cette separation, "la construction CPU coute X ms" ne dit pas quoi
    // optimiser - et la reponse s'est revelee contre-intuitive (voir
    // NOTES-PHASE18.md).
    std::string buildProfile() const;

    // PHASE 12. Bilan des trous sur toute la session, journalise a la
    // destruction : c'est le critere de reussite du chantier.
    std::string holeReport() const;

private:
    struct ResidentNode {
        MeshHandle mesh = kInvalidMesh;
        uint64_t lastUsedFrame = 0;
        std::size_t bytes = 0;
        // PHASE 12 : tuile du sommet epingle, jamais candidate a l'eviction.
        bool pinned = false;
    };

    // PHASE 19. Maille les tuiles `nodes` dans `batchMeshes_`, en parallele
    // au-dela de deux tuiles. AUCUN appel Vulkan : c'est toute la raison pour
    // laquelle cette etape peut quitter le thread de rendu sans rien changer a
    // la synchronisation du moteur.
    //
    // Chaque thread ecrit dans des ELEMENTS DISJOINTS de vecteurs deja
    // dimensionnes : il n'y a ni verrou, ni file, ni compteur partage - donc
    // aucun etat sur lequel une course serait possible. Une tuile qui echoue
    // depose son message dans `batchErrors_[k]`, jamais dans un journal (les
    // fonctions de journalisation ne sont pas prevues pour plusieurs threads) :
    // c'est l'appelant qui journalise, apres la jonction.
    void meshBatch(const std::vector<uint32_t>& nodes);

    // true si un lot de `count` tuiles vaut la peine de partir sur des threads.
    bool wantsParallel(std::size_t count) const;

    // Pose la topologie commune puis televerse. SEQUENTIEL, thread de rendu.
    // Renvoie kInvalidMesh en cas d'echec (la tuile est alors simplement
    // sautee : un trou d'une frame vaut mieux qu'un arret du jeu).
    MeshHandle uploadNode(uint32_t nodeIndex, MeshData& data);

    // Maille ET televerse une tuile, en sequence, sans jamais garder plus d'un
    // MeshData en vie. Chemin par DEFAUT depuis que meshThreads vaut 1, et
    // chemin du sommet epingle.
    MeshHandle buildNode(uint32_t nodeIndex);

    // PHASE 18 : la topologie d'une tuile, commune a toutes.
    const std::vector<uint32_t>& sharedIndices() const;

    // Libere la tuile la moins recemment utilisee qui n'a pas servi dans la
    // frame courante. Renvoie false s'il n'y a rien a liberer.
    bool evictOne();

    // PHASE 12. Construit et epingle les tuiles des params_.pinnedTopLevels
    // niveaux les plus grossiers. Appele une fois, a la construction.
    void pinTopLevels();

    // PHASE 12. Premier ancetre STRICT de `nodeIndex` qui soit resident, ou -1.
    int32_t residentAncestor(uint32_t nodeIndex) const;

    // PHASE 12. true si un ancetre STRICT de `nodeIndex` figure parmi les
    // ancetres de repli retenus cette frame (donc si le noeud est deja couvert).
    bool coveredByFallback(uint32_t nodeIndex) const;

    const Heightmap& heightmap_;
    Renderer& renderer_;
    Params params_;

    TerrainQuadtree quadtree_;

    // Tuiles residentes, indexees par indice de noeud du quadtree.
    std::unordered_map<uint32_t, ResidentNode> resident_;

    // Reutilisee d'une frame a l'autre pour ne pas reallouer 60 fois par seconde.
    std::vector<TerrainQuadtree::SelectedNode> selection_;

    // PHASE 12. Ancetres retenus pour le repli a cette frame. Membre, et non
    // variable locale, uniquement pour garder sa capacite d'une frame a l'autre.
    std::unordered_set<uint32_t> fallbackAncestors_;

    uint64_t frameCounter_ = 0;
    std::size_t residentBytes_ = 0;

    uint32_t selectedCount_ = 0;
    uint32_t drawnCount_ = 0;
    uint32_t pendingCount_ = 0;
    uint32_t fallbackCount_ = 0;
    uint32_t uncoveredCount_ = 0;
    uint32_t peakSelected_ = 0;
    uint32_t peakResident_ = 0;
    std::size_t peakResidentBytes_ = 0;

    // Bilan de session (PHASE 12).
    uint64_t frameCount_ = 0;
    uint64_t framesWithHoles_ = 0;     // frames comptant au moins un noeud non couvert
    uint64_t framesWithFallback_ = 0;  // frames ayant utilise au moins un repli
    uint32_t peakUncovered_ = 0;
    uint32_t peakFallback_ = 0;
    uint32_t peakBuiltPerFrame_ = 0;

    // PHASE 18 : la topologie commune, construite a la premiere tuile.
    // `mutable` parce que sharedIndices() est const : c'est un cache, il ne
    // change rien a l'etat observable du streamer.
    mutable std::vector<uint32_t> indexTemplate_;

    // PHASE 18 : profilage de la construction, en microsecondes cumulees.
    uint64_t builtNodes_ = 0;
    uint64_t geometryMicros_ = 0;
    uint64_t indicesMicros_ = 0;
    uint64_t uploadMicros_ = 0;
    uint32_t pinnedCount_ = 0;

    // PHASE 19 : lot de tuiles a mailler cette frame. Membres, et non variables
    // locales, uniquement pour garder leur capacite d'une frame a l'autre -
    // meme raison que selection_ et fallbackAncestors_.
    std::vector<uint32_t>    batchNodes_;
    std::vector<MeshData>    batchMeshes_;   // rempli seulement sur le chemin parallele
    std::vector<std::string> batchErrors_;

    // Maillage de travail du chemin sequentiel. Membre pour garder sa capacite
    // d'une tuile a l'autre ; il n'en vit jamais qu'un.
    MeshData scratchMesh_;

    // PHASE 19 : temps de MUR de l'etape de maillage, a comparer a
    // geometryMicros_ qui reste, lui, la somme du travail par tuile. Les deux
    // sont necessaires pour une mesure honnete : le travail par tuile ne doit
    // PAS changer (la fonction est la meme), seul le temps ecoule doit baisser.
    uint64_t meshWallMicros_ = 0;
    uint64_t parallelBatches_ = 0;   // lots reellement partis sur des threads
    uint32_t peakBatchSize_ = 0;
};

} // namespace vercora
