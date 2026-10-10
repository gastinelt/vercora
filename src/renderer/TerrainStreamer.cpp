#include "renderer/TerrainStreamer.hpp"

#include "assets/Heightmap.hpp"
#include "core/Logger.hpp"
#include "renderer/MeshData.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/TerrainNodeMesh.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <thread>

namespace vercora {

TerrainStreamer::TerrainStreamer(const Heightmap& heightmap, Renderer& renderer,
                                 const Params& params)
    : heightmap_(heightmap), renderer_(renderer), params_(params),
      quadtree_(heightmap, params.quadtree) {
    logInfo(quadtree_.describe());

    const std::size_t bytesPerNode = static_cast<std::size_t>(quadtree_.nodeCells() + 1) *
                                     (quadtree_.nodeCells() + 1) * sizeof(Vertex) +
                                     static_cast<std::size_t>(quadtree_.trianglesPerNode()) *
                                     3 * sizeof(uint32_t);
    logInfo("Streaming terrain : au plus " + std::to_string(params_.maxResidentNodes) +
            " tuiles residentes (" +
            std::to_string(params_.maxResidentNodes * bytesPerNode / (1024 * 1024)) +
            " Mo de geometrie GPU), " + std::to_string(params_.maxUploadsPerFrame) +
            " construites par frame.");

    pinTopLevels();
}

void TerrainStreamer::pinTopLevels() {
    if (params_.pinnedTopLevels == 0) {
        logInfo("Sommet du quadtree NON epingle : un trou redevient possible si "
                "une tuile grossiere n'est pas prete a temps.");
        return;
    }

    // Niveau le plus fin a epingler. La soustraction est faite en entier signe :
    // sur un terrain minuscule, rootLevel peut etre inferieur au nombre de
    // niveaux demandes, et on epingle alors simplement tout l'arbre.
    const int lowest = std::max(0, static_cast<int>(quadtree_.rootLevel()) -
                                       static_cast<int>(params_.pinnedTopLevels) + 1);

    const std::size_t bytesPerNode =
        static_cast<std::size_t>(quadtree_.nodeCells() + 1) *
            (quadtree_.nodeCells() + 1) * sizeof(Vertex) +
        static_cast<std::size_t>(quadtree_.trianglesPerNode()) * 3 * sizeof(uint32_t);

    for (uint32_t index = 0; index < quadtree_.nodeCount(); ++index) {
        if (static_cast<int>(quadtree_.node(index).level) < lowest) {
            continue;
        }
        const MeshHandle mesh = buildNode(index);
        if (mesh == kInvalidMesh) {
            continue; // buildNode a deja journalise ; le repli sera juste moins bon
        }
        ResidentNode node;
        node.mesh = mesh;
        node.bytes = bytesPerNode;
        node.pinned = true;
        // Jamais rajeunie ensuite : elle n'est de toute facon pas evincable.
        node.lastUsedFrame = 0;
        resident_.emplace(index, node);
        residentBytes_ += bytesPerNode;
        ++pinnedCount_;
    }

    logInfo("Sommet du quadtree epingle : " + std::to_string(pinnedCount_) +
            " tuiles de niveau " + std::to_string(lowest) + " a " +
            std::to_string(static_cast<int>(quadtree_.rootLevel())) + " (" +
            std::to_string(pinnedCount_ * bytesPerNode / (1024 * 1024)) +
            " Mo), jamais evincees. C'est ce qui rend le repli sur l'ancetre "
            "toujours disponible.");
}

TerrainStreamer::~TerrainStreamer() {
    // Les tuiles sont rendues au Renderer, qui differe leur destruction reelle.
    // Le Renderer attend le GPU avant de se detruire lui-meme, donc l'ordre est
    // sur quel que soit celui des deux qui part en premier.
    for (const auto& entry : resident_) {
        renderer_.removeMesh(entry.second.mesh);
    }
    logInfo("Streaming terrain : pics de la session - " + std::to_string(peakSelected_) +
            " tuiles selectionnees, " + std::to_string(peakResident_) +
            " residentes, " + std::to_string(peakResidentBytes_ / (1024 * 1024)) +
            " Mo de geometrie GPU, au plus " + std::to_string(peakBuiltPerFrame_) +
            " construites en une frame.");
    logInfo(holeReport());
    logInfo(buildProfile());
}

const std::vector<uint32_t>& TerrainStreamer::sharedIndices() const {
    // La topologie d'une tuile - 24 576 indices pour 64x64 cellules - est
    // IDENTIQUE pour toutes les tuiles de tous les niveaux. L'en-tete de
    // TerrainNodeMesh.hpp le disait depuis la Phase 10 (« calcules une fois et
    // recopies ») mais le code, lui, les reconstruisait a chaque tuile.
    //
    // Le profilage de la Phase 18 a chiffre ce travail redondant a
    // 636 us/tuile, soit 23 % du cout total de construction - devant le
    // televersement GPU (307 us), qui avait pourtant mobilise toute la Phase 12.
    // C'est le genre de poste qu'on ne trouve qu'en mesurant : personne
    // n'aurait parie sur la boucle d'indices.
    //
    // Construite a la premiere tuile puis simplement RECOPIEE dans le MeshData.
    // Ne pas la recopier du tout - partager un unique buffer d'indices GPU entre
    // toutes les tuiles - economiserait en plus 96 Ko par tuile residente, mais
    // demanderait de dissocier sommets et indices dans Renderer::addMesh : c'est
    // un chantier de renderer, pas de streaming (voir NOTES-PHASE18.md).
    if (indexTemplate_.empty() || !params_.cacheTileWork) {
        indexTemplate_ = terrainNodeIndices(quadtree_.nodeCells());
    }
    return indexTemplate_;
}

namespace {

// Duree en microsecondes entre deux points d'horloge. steady_clock, et non
// system_clock : une mesure de duree ne doit pas dependre d'un ajustement
// d'horloge. Le cout du chronometre lui-meme (quelques dizaines de
// nanosecondes) est negligeable devant une tuile, qui se compte en centaines de
// microsecondes.
using Clock = std::chrono::steady_clock;

uint64_t microsBetween(Clock::time_point a, Clock::time_point b) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
}

// PHASE 19. En deca de ce nombre de tuiles, le lot est maille en sequence sur
// le thread appelant, quel que soit meshThreads. Le REGIME ETABLI (une ou deux
// tuiles par image, la marche a pied) reste ainsi sur le chemin de code exact
// d'avant la Phase 19, threads compris.
//
// Ce seuil n'a d'effet que si l'on a demande des threads (--tile-threads=N) ;
// par defaut le maillage est entierement sequentiel. Voir le commentaire de
// Params::meshThreads pour la mesure qui a conduit a ce defaut.
constexpr std::size_t kParallelBatchThreshold = 3;

} // namespace

bool TerrainStreamer::wantsParallel(std::size_t count) const {
    return params_.meshThreads > 1 && count >= kParallelBatchThreshold;
}

void TerrainStreamer::meshBatch(const std::vector<uint32_t>& nodes) {
    batchMeshes_.clear();
    batchErrors_.clear();
    batchMeshes_.resize(nodes.size());
    batchErrors_.resize(nodes.size());
    if (nodes.empty()) {
        return;
    }

    // Le corps d'un travailleur. Il ne touche QUE batchMeshes_[k] et
    // batchErrors_[k] pour k dans [begin, end) - des elements deja alloues, donc
    // ni le vecteur ni sa capacite ne bougent pendant l'execution. Tout le reste
    // de ce qu'il lit (heightmap_, quadtree_, params_) est const pour la duree
    // de la session.
    //
    // Le chronometrage par tuile est fait ICI, mais il n'est PAS accumule dans
    // geometryMicros_ depuis un thread : chaque tuile depose sa duree dans son
    // propre element d'un vecteur local, somme apres la jonction. Un `+=` sur
    // un membre depuis plusieurs threads serait exactement la course que cette
    // fonction s'emploie a ne pas avoir - et une course sur un compteur de
    // MESURE serait la pire de toutes, puisqu'elle fausserait le chiffre cense
    // prouver que le changement est bon.
    std::vector<uint64_t> perTileMicros(nodes.size(), 0);

    const auto worker = [&](std::size_t begin, std::size_t end) {
        for (std::size_t k = begin; k < end; ++k) {
            const auto t0 = Clock::now();
            try {
                batchMeshes_[k] = terrainNodeMeshData(heightmap_, quadtree_, nodes[k],
                                                      false, params_.cacheTileWork);
            } catch (const std::exception& e) {
                // Une exception ne doit JAMAIS traverser la frontiere d'un
                // std::thread : elle appellerait std::terminate. Elle est donc
                // capturee ici et rendue a l'appelant comme une donnee.
                batchErrors_[k] = e.what();
                batchMeshes_[k] = MeshData{};
            } catch (...) {
                batchErrors_[k] = "exception inconnue";
                batchMeshes_[k] = MeshData{};
            }
            perTileMicros[k] = microsBetween(t0, Clock::now());
        }
    };

    const auto wall0 = Clock::now();

    const uint32_t wanted = std::max(1u, params_.meshThreads);
    if (nodes.size() < kParallelBatchThreshold || wanted == 1) {
        worker(0, nodes.size());
    } else {
        // Decoupage en tranches CONTIGUES et disjointes. Une tuile coute
        // toujours la meme chose - 65 x 65 sommets, quel que soit son niveau -
        // donc un decoupage statique equilibre naturellement la charge : pas
        // besoin d'une file de travail, qui demanderait un verrou.
        const std::size_t total = nodes.size();
        const std::size_t slices =
            std::min(static_cast<std::size_t>(wanted), total);
        std::vector<std::thread> threads;
        threads.reserve(slices - 1);

        const auto sliceBegin = [total, slices](std::size_t s) {
            return (total * s) / slices;
        };
        // Le thread APPELANT prend la premiere tranche : creer `slices` threads
        // pour ensuite attendre sans rien faire gacherait un coeur entier.
        for (std::size_t s = 1; s < slices; ++s) {
            threads.emplace_back(worker, sliceBegin(s), sliceBegin(s + 1));
        }
        worker(sliceBegin(0), sliceBegin(1));
        for (std::thread& t : threads) {
            t.join();
        }
        ++parallelBatches_;
    }

    meshWallMicros_ += microsBetween(wall0, Clock::now());
    for (uint64_t micros : perTileMicros) {
        geometryMicros_ += micros;
    }
}

MeshHandle TerrainStreamer::uploadNode(uint32_t nodeIndex, MeshData& data) {
    try {
        const auto t1 = Clock::now();
        // Les indices sont poses ICI et non dans terrainNodeMeshData, pour que
        // les deux postes soient chronometres separement (voir buildProfile).
        data.indices = sharedIndices();
        const auto t2 = Clock::now();
        // log = false : plusieurs tuiles par frame, une ligne chacune rendrait la
        // sortie inexploitable (et couterait plus que l'upload).
        const MeshHandle mesh = renderer_.addMesh(data, false);
        const auto t3 = Clock::now();

        ++builtNodes_;
        indicesMicros_ += microsBetween(t1, t2);
        uploadMicros_  += microsBetween(t2, t3);
        return mesh;
    } catch (const std::exception& e) {
        // Une tuile qu'on ne sait pas construire ne doit pas arreter le jeu :
        // elle sera simplement absente, et le message dit laquelle. Le cas
        // realiste est un manque de memoire GPU, pas une donnee invalide.
        logWarn("Streaming terrain : tuile " + std::to_string(nodeIndex) +
                " non televersee (" + e.what() + ").");
        return kInvalidMesh;
    }
}

MeshHandle TerrainStreamer::buildNode(uint32_t nodeIndex) {
    // Chemin SEQUENTIEL : maille une tuile puis la televerse, sans jamais garder
    // plus d'un MeshData en vie. C'est le chemin du sommet epingle (construit
    // avant la premiere frame) et, depuis que le defaut est meshThreads = 1,
    // celui de la construction courante.
    //
    // Ne pas passer par meshBatch ici n'est pas une duplication gratuite :
    // meshBatch dimensionne un vecteur de MeshData pour TOUT le lot, ce qui fait
    // vivre jusqu'a 53 tuiles simultanement (~15 Mo) la ou ce chemin-ci n'en
    // tient qu'une. Mesure : le pic memoire du processus montait de 5,6 Mo quand
    // tout passait par le lot. Le chemin par defaut retrouve ainsi exactement le
    // profil memoire d'avant la Phase 19.
    const auto t0 = Clock::now();
    try {
        scratchMesh_ = terrainNodeMeshData(heightmap_, quadtree_, nodeIndex, false,
                                           params_.cacheTileWork);
    } catch (const std::exception& e) {
        geometryMicros_ += microsBetween(t0, Clock::now());
        logWarn("Streaming terrain : tuile " + std::to_string(nodeIndex) +
                " non maillee (" + e.what() + ").");
        return kInvalidMesh;
    }
    const uint64_t micros = microsBetween(t0, Clock::now());
    geometryMicros_ += micros;
    meshWallMicros_ += micros;
    return uploadNode(nodeIndex, scratchMesh_);
}

bool TerrainStreamer::evictOne() {
    uint32_t victim = 0;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    bool found = false;

    for (const auto& entry : resident_) {
        // Jamais une tuile deja utilisee dans la frame courante : l'evincer
        // reviendrait a la reconstruire immediatement (thrashing pur).
        if (entry.second.lastUsedFrame >= frameCounter_) {
            continue;
        }
        // PHASE 12 : le sommet epingle n'est jamais candidat. Il est la
        // precisement pour les frames ou tout le reste manque.
        if (entry.second.pinned) {
            continue;
        }

        if (entry.second.lastUsedFrame < oldest) {
            oldest = entry.second.lastUsedFrame;
            victim = entry.first;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    const auto it = resident_.find(victim);
    renderer_.removeMesh(it->second.mesh);
    residentBytes_ -= it->second.bytes;
    resident_.erase(it);
    return true;
}

int32_t TerrainStreamer::residentAncestor(uint32_t nodeIndex) const {
    // Remontee STRICTE : on part du pere, jamais du noeud lui-meme. La chaine
    // fait au plus rootLevel_ maillons (8 sur la zone de 25 km2 au pas de 1 m),
    // donc cette boucle est bornee et courte.
    int32_t current = quadtree_.parentIndex(nodeIndex);
    while (current >= 0) {
        if (resident_.find(static_cast<uint32_t>(current)) != resident_.end()) {
            return current;
        }
        current = quadtree_.parentIndex(static_cast<uint32_t>(current));
    }
    return -1;
}

bool TerrainStreamer::coveredByFallback(uint32_t nodeIndex) const {
    int32_t current = quadtree_.parentIndex(nodeIndex);
    while (current >= 0) {
        if (fallbackAncestors_.count(static_cast<uint32_t>(current)) != 0) {
            return true;
        }
        current = quadtree_.parentIndex(static_cast<uint32_t>(current));
    }
    return false;
}

void TerrainStreamer::appendDrawItems(const glm::vec3& cameraPosition,
                                      const Frustum& frustum,
                                      std::vector<DrawItem>& out) {
    ++frameCounter_;

    quadtree_.select(cameraPosition, frustum, selection_);
    selectedCount_ = static_cast<uint32_t>(selection_.size());
    drawnCount_ = 0;
    pendingCount_ = 0;
    fallbackCount_ = 0;
    uncoveredCount_ = 0;

    const std::size_t bytesPerNode =
        static_cast<std::size_t>(quadtree_.nodeCells() + 1) *
            (quadtree_.nodeCells() + 1) * sizeof(Vertex) +
        static_cast<std::size_t>(quadtree_.trianglesPerNode()) * 3 * sizeof(uint32_t);

    const bool priming = (frameCounter_ <= params_.primingFrames);
    uint32_t uploadsLeft = priming ? std::numeric_limits<uint32_t>::max()
                                   : params_.maxUploadsPerFrame;

    // --- 1a. Recensement : QUELLES tuiles construire cette frame ------------
    // selection_ est deja triee du plus proche au plus lointain : le lot retient
    // donc les tuiles sous les pieds du joueur en premier quand le budget est
    // serre.
    //
    // PHASE 19 : cette passe ne fait plus que RECENSER. Elle etait auparavant
    // fusionnee avec le maillage et le televersement ; les separer est ce qui
    // permet de mailler tout le lot d'un coup, donc en parallele.
    uint32_t builtThisFrame = 0;
    batchNodes_.clear();
    for (const TerrainQuadtree::SelectedNode& selected : selection_) {
        auto it = resident_.find(selected.nodeIndex);
        if (it != resident_.end()) {
            // Marquee utilisee MEME si le repli finit par la remplacer cette
            // frame (voir passe 3) : sans cela le LRU evincerait une tuile que
            // la selection redemande, ce qui la ferait reconstruire aussitot.
            it->second.lastUsedFrame = frameCounter_;
            continue;
        }

        ++pendingCount_;

        if (uploadsLeft == 0) {
            continue; // servie a une frame suivante ; le repli la couvre en attendant
        }
        // Le budget est decompte a l'INSCRIPTION et non plus a la reussite.
        // Difference observable seulement quand une tuile echoue a se construire
        // (en pratique : plus de memoire GPU) : cette frame-la en sert une de
        // moins, et la tuile part a la frame suivante. Le repli sur l'ancetre la
        // couvre entre-temps, donc cela ne peut pas faire de trou.
        if (uploadsLeft != std::numeric_limits<uint32_t>::max()) {
            --uploadsLeft;
        }
        batchNodes_.push_back(selected.nodeIndex);
    }

    // --- 1b/1c. Maillage puis televersement ---------------------------------
    // Deux chemins, et le choix se fait sur la TAILLE DU LOT :
    //
    //   - lot parallelisable : tout le lot est maille d'abord (meshBatch, aucun
    //     appel Vulkan, plusieurs threads), puis televerse en sequence ;
    //   - sinon : chaque tuile est maillee et televersee a la suite, sans jamais
    //     garder plus d'un MeshData en vie. C'est le chemin par DEFAUT, et il
    //     est identique a celui d'avant la Phase 19, cout memoire compris.
    //
    // Dans les deux cas les addMesh ont lieu sur le thread de rendu, en
    // sequence, et dans l'ordre de selection - donc du plus proche au plus
    // lointain.

    // Suivi du plus gros lot : il doit compter les DEUX chemins, sinon le bilan
    // afficherait « plus gros lot 0 tuiles » sur le chemin par defaut.
    peakBatchSize_ = std::max(peakBatchSize_,
                              static_cast<uint32_t>(batchNodes_.size()));

    const bool parallel = wantsParallel(batchNodes_.size());
    if (parallel) {
        meshBatch(batchNodes_);
    }

    for (std::size_t k = 0; k < batchNodes_.size(); ++k) {
        const uint32_t nodeIndex = batchNodes_[k];
        if (parallel && !batchErrors_[k].empty()) {
            logWarn("Streaming terrain : tuile " + std::to_string(nodeIndex) +
                    " non maillee (" + batchErrors_[k] + ").");
            continue;
        }
        // Faire de la place AVANT de televerser, sinon le pic de residence
        // depasserait le plafond le temps d'un upload. (Le maillage, lui,
        // n'alloue rien sur le GPU : c'est bien l'upload qui doit etre couvert.)
        while (resident_.size() >= params_.maxResidentNodes) {
            if (!evictOne()) {
                break; // tout est utilise cette frame : on laisse deborder
            }
        }

        const MeshHandle mesh = parallel ? uploadNode(nodeIndex, batchMeshes_[k])
                                         : buildNode(nodeIndex);
        if (mesh == kInvalidMesh) {
            continue;
        }
        ++builtThisFrame;

        ResidentNode node;
        node.mesh = mesh;
        node.bytes = bytesPerNode;
        node.lastUsedFrame = frameCounter_;
        resident_.emplace(nodeIndex, node);
        residentBytes_ += bytesPerNode;
        --pendingCount_; // finalement servie dans la frame meme
    }

    // Le lot ne doit pas garder ses maillages en vie jusqu'a la frame suivante :
    // ce sont ~287 Ko par tuile.
    if (parallel) {
        batchMeshes_.clear();
        batchMeshes_.shrink_to_fit();
    }

    // --- 2. Choix des ancetres de repli (PHASE 12) --------------------------
    // Rien a faire dans le cas courant : en regime etabli toute la selection est
    // residente, l'ensemble reste vide et les passes suivantes ne coutent rien.
    fallbackAncestors_.clear();

    if (pendingCount_ > 0) {
        for (const TerrainQuadtree::SelectedNode& selected : selection_) {
            if (resident_.find(selected.nodeIndex) != resident_.end()) {
                continue;
            }
            if (!params_.ancestorFallback) {
                ++uncoveredCount_; // comportement Phase 10 : la tuile est un trou
                continue;
            }
            const int32_t ancestor = residentAncestor(selected.nodeIndex);
            if (ancestor >= 0) {
                fallbackAncestors_.insert(static_cast<uint32_t>(ancestor));
            } else {
                // Aucun ancetre resident : la surface n'est reellement couverte
                // par rien. C'est le seul cas qui produit encore un trou.
                ++uncoveredCount_;
            }
        }
    }

    // --- 3. Emission ---------------------------------------------------------
    const bool hasFallback = !fallbackAncestors_.empty();

    auto emit = [&](const TerrainQuadtree::SelectedNode& node, MeshHandle mesh) {
        DrawItem item;
        item.mesh = mesh;
        // Les sommets d'une tuile sont deja en coordonnees monde, comme ceux du
        // terrain d'un seul bloc : matrice identite.
        item.model = glm::mat4(1.0f);
        item.material = Material::Terrain;
        item.params = glm::vec4(node.morphStart, node.morphEnd, 0.0f, 0.0f);
        out.push_back(item);
        ++drawnCount_;
    };

    // 3a. Les ancetres de repli eux-memes, une seule fois chacun, et seulement
    //     les plus HAUTS : si un ancetre de repli est lui-meme sous un autre
    //     ancetre de repli, le second couvre deja le premier.
    fallbackCount_ = 0;
    if (hasFallback) {
        for (uint32_t ancestor : fallbackAncestors_) {
            if (coveredByFallback(ancestor)) {
                continue;
            }
            const auto it = resident_.find(ancestor);
            if (it == resident_.end()) {
                continue; // ne peut pas arriver : il a ete retenu parce qu'il l'etait
            }
            it->second.lastUsedFrame = frameCounter_;
            emit(quadtree_.describeNode(ancestor, cameraPosition), it->second.mesh);
            ++fallbackCount_;
        }
    }

    // 3b. Les tuiles selectionnees residentes, SAUF celles qu'un ancetre de
    //     repli recouvre deja - deux surfaces superposees se disputeraient le
    //     z-buffer.
    for (const TerrainQuadtree::SelectedNode& selected : selection_) {
        const auto it = resident_.find(selected.nodeIndex);
        if (it == resident_.end()) {
            continue; // pas encore construite : couverte par un repli, ou trou
        }
        if (hasFallback && coveredByFallback(selected.nodeIndex)) {
            continue;
        }
        emit(selected, it->second.mesh);
    }

    peakSelected_ = std::max(peakSelected_, selectedCount_);
    peakResident_ = std::max(peakResident_, static_cast<uint32_t>(resident_.size()));
    peakResidentBytes_ = std::max(peakResidentBytes_, residentBytes_);
    peakUncovered_ = std::max(peakUncovered_, uncoveredCount_);
    peakFallback_ = std::max(peakFallback_, fallbackCount_);
    peakBuiltPerFrame_ = std::max(peakBuiltPerFrame_, builtThisFrame);
    ++frameCount_;
    if (uncoveredCount_ > 0) { ++framesWithHoles_; }
    if (fallbackCount_ > 0) { ++framesWithFallback_; }
}

std::string TerrainStreamer::statsLine() const {
    std::string text =
        "Terrain LOD : " + std::to_string(drawnCount_) + " tuiles dessinees sur " +
        std::to_string(selectedCount_) + " selectionnees (" +
        std::to_string(drawnTriangles()) + " triangles), " +
        std::to_string(resident_.size()) + " residentes, " +
        std::to_string(residentBytes_ / (1024 * 1024)) + " Mo de geometrie GPU.";
    if (pendingCount_ > 0 || fallbackCount_ > 0 || uncoveredCount_ > 0) {
        text += " En attente " + std::to_string(pendingCount_) + ", repli sur " +
                std::to_string(fallbackCount_) + " ancetre(s), " +
                std::to_string(uncoveredCount_) + " non couverte(s).";
    }
    return text;
}

std::string TerrainStreamer::holeReport() const {
    if (frameCount_ == 0) {
        return "Trous de streaming : aucune frame mesuree.";
    }
    std::string text =
        "Trous de streaming : " + std::to_string(framesWithHoles_) + " frame(s) sur " +
        std::to_string(frameCount_) + " avec au moins une tuile NON COUVERTE (pic " +
        std::to_string(peakUncovered_) + " tuiles) ; repli sur ancetre utilise dans " +
        std::to_string(framesWithFallback_) + " frame(s) (pic " +
        std::to_string(peakFallback_) + " ancetres).";
    if (!params_.ancestorFallback) {
        text += " [repli DESACTIVE : --no-tile-fallback]";
    }
    return text;
}

std::string TerrainStreamer::buildProfile() const {
    if (builtNodes_ == 0) {
        return "Construction de tuiles : aucune tuile construite.";
    }
    const auto per = [this](uint64_t total) {
        // Moyenne en microsecondes par tuile, arrondie au dixieme.
        const double value = static_cast<double>(total) / static_cast<double>(builtNodes_);
        return std::to_string(static_cast<int>(value * 10.0 + 0.5) / 10.0);
    };
    const uint64_t total = geometryMicros_ + indicesMicros_ + uploadMicros_;
    std::string text =
           "Construction de tuiles : " + std::to_string(builtNodes_) + " tuiles, " +
           std::to_string(total / 1000) + " ms au total - geometrie " +
           std::to_string(geometryMicros_ / 1000) + " ms (" + per(geometryMicros_) +
           " us/tuile), indices " + std::to_string(indicesMicros_ / 1000) + " ms (" +
           per(indicesMicros_) + " us/tuile), televersement " +
           std::to_string(uploadMicros_ / 1000) + " ms (" + per(uploadMicros_) +
           " us/tuile).";

    // PHASE 19. Le chiffre precedent est le TRAVAIL par tuile ; il ne doit PAS
    // baisser avec la parallelisation, puisque c'est la meme fonction qui fait
    // le meme calcul. Ce qui doit baisser, c'est le temps que le thread de rendu
    // passe BLOQUE dessus - donc le temps de mur. Publier les deux est la seule
    // facon honnete de presenter un gain de parallelisme : un seul des deux
    // chiffres laisserait croire soit qu'il n'y a pas de gain, soit que le
    // travail a diminue.
    text += " Maillage parallele : " + std::to_string(meshWallMicros_ / 1000) +
            " ms de temps de mur pour " + std::to_string(geometryMicros_ / 1000) +
            " ms de travail (facteur " +
            std::to_string(meshWallMicros_ > 0
                ? static_cast<int>(static_cast<double>(geometryMicros_) * 100.0 /
                                   static_cast<double>(meshWallMicros_) + 0.5) / 100.0
                : 0.0) +
            "), " + std::to_string(parallelBatches_) + " lot(s) sur threads, plus gros lot " +
            std::to_string(peakBatchSize_) + " tuiles, " +
            std::to_string(std::max(1u, params_.meshThreads)) + " thread(s) demandes.";
    return text;
}

} // namespace vercora
