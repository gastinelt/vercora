#include "world/Forest.hpp"

#include "core/HeightSampler.hpp"
#include "core/Logger.hpp"
#include "world/Ground.hpp"
#include "world/TreeMesh.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vercora {
namespace {

constexpr float kPi = 3.14159265359f;
constexpr float kHalfPi = 1.57079632679f;

// Nombre de rayons lances a l'horizontale pour chercher la zone degagee.
// 24 rayons = un tous les 15 degres : assez fin pour trouver une trouee entre
// deux arbres voisins, assez grossier pour rester negligeable en cout (on
// l'appelle une fois par abattage, pas une fois par frame).
constexpr int kClearanceRayCount = 24;

// Pas d'echantillonnage du relief le long d'un rayon, en unites monde.
constexpr float kTerrainStep = 1.0f;

// On ignore le relief tout pres du pied : sur une pente, le sol juste derriere
// l'arbre est forcement plus haut que son pied sans que ca gene sa chute.
constexpr float kTerrainProbeStart = 2.0f;

// Acceleration de la pesanteur. 1 unite monde = 1 m (convention posee par la
// hauteur d'oeil de la camera), donc la vraie valeur a un sens ici.
constexpr float kGravity = 9.81f;

// Un pas de temps aberrant (fenetre deplacee, point d'arret dans le debugger)
// ne doit pas faire exploser l'integration : au-dela, on avance au ralenti
// plutot que de telescoper l'arbre a travers le sol.
constexpr float kMaxStep = 0.05f;

// --- Amortissement apres l'impact (etat Settling) --------------------------
// Duree pendant laquelle l'arbre reste dessine, COUCHE au sol, avant de se
// debiter. C'est le temps qu'il faut pour lire "l'arbre est tombe la" ; en
// dessous de ~0.4 s on ne voit toujours rien, au-dela d'une seconde l'attente
// devient perceptible.
constexpr float kSettleDuration = 0.65f;

// Rebond : l'arbre remonte legerement puis se repose, en s'amortissant. On reste
// TOUJOURS sous l'horizontale (l'angle est retranche a pi/2), sans quoi le tronc
// s'enfoncerait dans le terrain a chaque oscillation.
constexpr float kSettleAmplitude = 0.055f; // rad, ~3 degres au premier rebond
constexpr float kSettlePulsation = 15.0f;  // rad/s
constexpr float kSettleDamping   = 7.0f;   // 1/s

// --- Debitage (passage a Fallen) -------------------------------------------
// Intervalle entre deux pieces de bois. Le tronc se debite du pied vers la cime,
// puis les branches suivent : l'oeil suit une vague de fragmentation le long de
// l'arbre au lieu de voir tout le bois surgir d'un coup.
constexpr float kBuckingInterval = 0.16f;

// Nombre de passes de resolution des collisions (voir resolve()).
//
// La resolution est SEQUENTIELLE : chaque obstacle repousse le joueur a son
// tour. C'est ce qui donne le glissement le long d'un tronc, mais quand
// plusieurs obstacles se chevauchent, sortir du premier peut replonger dans le
// second - il faut alors plusieurs passes pour converger. Quatre suffisent
// largement a la densite de foret visee ici ; les passes s'arretent des qu'une
// d'elles ne touche plus rien, donc le cas courant (aucun obstacle) n'en coute
// qu'une.
//
// Limite assumee : deux troncs plus rapproches que la largeur du joueur forment
// un passage geometriquement infranchissable. La resolution le traite en le
// repoussant d'un cote ou de l'autre sans jamais diverger ni produire de NaN,
// mais elle ne garantit pas de trouver l'echappatoire laterale. En pratique le
// joueur ne peut pas s'y retrouver enferme : il est arrete par le premier tronc
// bien avant d'atteindre l'entre-deux (c'est ce que verifie le test de marche
// dans tests/logic_tests.cpp).
constexpr int kCollisionPasses = 4;

// Produit vectoriel 2D (composante Y) entre deux vecteurs du plan XZ. Son signe
// donne le cote, sa valeur absolue la distance a la droite quand b est unitaire.
float cross2D(const glm::vec3& a, const glm::vec3& b) {
    return a.x * b.z - a.z * b.x;
}

// Vecteur horizontal unitaire perpendiculaire a d (lui-meme horizontal).
glm::vec3 horizontalPerp(const glm::vec3& d) {
    return glm::vec3(d.z, 0.0f, -d.x);
}

// Le volume de collision de cet arbre mord-il la tranche verticale occupee par
// le corps ? C'est ce test qui distingue un tronc entier d'une souche : la
// souche d'un arbre de 7 m culmine a ~0.3 m, donc elle n'accroche que les pieds,
// alors que le tronc debout barre toute la hauteur du joueur.
//
// Un arbre EN TRAIN DE TOMBER n'expose deja plus que le volume de sa souche
// (voir Tree::colliderTopY) : son tronc est en l'air, quelque part au-dessus du
// joueur ou a cote de lui. Le suivre pendant la bascule demanderait un obstacle
// mobile - c'est hors sujet a ce stade, et la fenetre d'une seconde et demie ou
// on pourrait traverser un tronc en vol est un defaut acceptable.
bool blocksBody(const Tree& tree, const BodyCylinder& body) {
    return body.feetY <= tree.colliderTopY() && body.headY >= tree.base.y;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction : semis des arbres
// ---------------------------------------------------------------------------
Forest::Forest(const HeightSampler& terrain, Ground& ground, float terrainExtent,
               const ForestParams& params,
               const AspectExposureSource* aspectExposure,
               const TreeSpeciesParams& speciesParams)
    : terrain_(terrain), ground_(ground), terrainExtent_(terrainExtent),
      params_(params), rng_(params.seed) {
    // Renseignes AVANT le semis qui suit : plantTree les lit pour chaque arbre.
    aspectExposure_ = aspectExposure;
    speciesParams_ = speciesParams;
    if (params_.minHeight <= 0.0f || params_.maxHeight < params_.minHeight) {
        throw std::runtime_error("Forest : bornes de hauteur d'arbre incoherentes.");
    }
    if (params_.areaRadius <= params_.minDistanceFromStart) {
        throw std::runtime_error(
            "Forest : la zone de semis est vide (areaRadius <= minDistanceFromStart).");
    }

    // Semis par tirage-rejet : on tire une position au hasard et on la garde si
    // elle passe les deux filtres (pente praticable, espacement suffisant).
    // Plus simple qu'un echantillonnage de Poisson et parfaitement suffisant
    // pour quelques dizaines d'arbres ; la limite d'essais garantit qu'on sort
    // meme si les contraintes sont trop serrees pour la surface disponible.
    std::uniform_real_distribution<float> angleDist(0.0f, 2.0f * kPi);
    std::uniform_real_distribution<float> unitDist(0.0f, 1.0f);
    std::uniform_real_distribution<float> heightDist(params_.minHeight, params_.maxHeight);

    // Marge par rapport au bord du terrain : un arbre pile sur le bord tomberait
    // hors des donnees, ou heightAt ne renvoie plus que la valeur ecretee.
    const float usableExtent = std::max(0.0f, terrainExtent_ - 3.0f);

    const uint32_t maxAttempts = params_.treeCount * 60u;
    trees_.reserve(params_.treeCount);

    for (uint32_t attempt = 0; attempt < maxAttempts && trees_.size() < params_.treeCount;
         ++attempt) {
        // Racine carree sur le rayon : sans elle, le tirage se concentre au
        // centre du disque (la surface d'une couronne croit avec le rayon).
        const float r0 = params_.minDistanceFromStart;
        const float r1 = params_.areaRadius;
        const float u = unitDist(rng_);
        const float radius = std::sqrt(r0 * r0 + u * (r1 * r1 - r0 * r0));
        const float angle = angleDist(rng_);

        const float x = radius * std::cos(angle);
        const float z = radius * std::sin(angle);

        if (std::abs(x) > usableExtent || std::abs(z) > usableExtent) {
            continue;
        }
        if (slopeAt(x, z) > params_.maxSlope) {
            continue;
        }

        bool tooClose = false;
        for (const Tree& other : trees_) {
            const float dx = other.base.x - x;
            const float dz = other.base.z - z;
            if (dx * dx + dz * dz < params_.minSpacing * params_.minSpacing) {
                tooClose = true;
                break;
            }
        }
        if (tooClose) {
            continue;
        }

        // Les deux tirages sont nommes AVANT l'appel, et non passes directement
        // en arguments : l'ordre d'evaluation des arguments d'un appel n'est pas
        // specifie en C++, donc les ecrire dans l'appel laisserait le compilateur
        // decider laquelle des deux valeurs du generateur devient la hauteur et
        // laquelle devient l'orientation. La foret cesserait d'etre reproductible
        // d'un compilateur a l'autre, ce qui est precisement la garantie que
        // ForestParams::seed est cense offrir.
        const float height = heightDist(rng_);
        const float yaw = angleDist(rng_);
        plantTree(x, z, height, yaw);
    }

    logInfo("Foret : " + std::to_string(trees_.size()) + " arbres semes sur " +
            std::to_string(params_.treeCount) + " demandes (rejet par pente ou espacement).");
}

int Forest::plantTree(float worldX, float worldZ, float height, float yaw) {
    if (height <= 0.0f) {
        throw std::runtime_error("Forest : hauteur d'arbre nulle ou negative.");
    }

    Tree tree;
    tree.base = { worldX, terrain_.heightAt(worldX, worldZ), worldZ };
    tree.height = height;
    tree.yaw = yaw;
    // PHASE 26 : l'essence, decidee par le MEME appel que le semis de decor. Un
    // arbre abattable et un arbre de decor plantes au meme endroit sont donc de
    // la meme essence - la fonction ne depend que de la position et de la graine.
    // Source d'exposition nulle (cas des tests et du terrain synthetique) : la
    // part de resineux vaut alors la valeur neutre partout.
    tree.species = treeSpeciesAt(worldX, worldZ, aspectExposure_, speciesParams_);
    trees_.push_back(tree);
    return static_cast<int>(trees_.size()) - 1;
}

std::size_t Forest::coniferCount() const {
    std::size_t count = 0;
    for (const Tree& tree : trees_) {
        if (tree.species == TreeSpecies::Conifer) { ++count; }
    }
    return count;
}

float Forest::slopeAt(float worldX, float worldZ) const {
    // Difference centree, comme les normales de TerrainMesh. L'ecart est
    // volontairement large (0.5 unite) : on veut la pente a l'echelle du pied
    // d'un arbre, pas la micro-rugosite d'une cellule de la grille.
    constexpr float kProbe = 0.5f;
    const float dhdx = (terrain_.heightAt(worldX + kProbe, worldZ) -
                        terrain_.heightAt(worldX - kProbe, worldZ)) / (2.0f * kProbe);
    const float dhdz = (terrain_.heightAt(worldX, worldZ + kProbe) -
                        terrain_.heightAt(worldX, worldZ - kProbe)) / (2.0f * kProbe);
    return std::sqrt(dhdx * dhdx + dhdz * dhdz);
}

// ---------------------------------------------------------------------------
// Visee : rayon du joueur -> arbre
// ---------------------------------------------------------------------------
int Forest::pickTree(const glm::vec3& origin, const glm::vec3& direction,
                     float maxDistance) const {
    int closestIndex = kNoTree;
    float closestT = maxDistance;

    for (std::size_t i = 0; i < trees_.size(); ++i) {
        const Tree& tree = trees_[i];
        if (tree.state != TreeState::Standing) {
            continue;
        }

        // Volume de visee : un cylindre vertical englobant, de rayon celui du
        // houppier, sur toute la hauteur de l'arbre. C'est un PROXY assume et
        // non la geometrie exacte - tester le tronc reel rendrait la visee
        // penible a 30 m, et rien dans le gameplay ne depend de savoir si le
        // joueur a touche une branche ou le tronc.
        const float pickRadius = tree.crownRadius() * 0.9f;

        // Intersection rayon / cylindre : le probleme se resout dans le plan XZ
        // (le cylindre y est un cercle), la coordonnee Y ne servant ensuite qu'a
        // verifier que le point touche est bien entre le pied et la cime.
        const glm::vec2 d2(direction.x, direction.z);
        const glm::vec2 o2(origin.x - tree.base.x, origin.z - tree.base.z);

        const float a = glm::dot(d2, d2);
        if (a < 1e-6f) {
            // Rayon quasi vertical : il ne peut toucher un tronc que si la
            // camera est deja dedans, cas sans interet.
            continue;
        }

        const float b = 2.0f * glm::dot(o2, d2);
        const float c = glm::dot(o2, o2) - pickRadius * pickRadius;
        const float discriminant = b * b - 4.0f * a * c;
        if (discriminant < 0.0f) {
            continue;
        }

        const float sqrtDisc = std::sqrt(discriminant);
        // Les deux racines : entree puis sortie du cylindre. On teste les deux,
        // pour que viser depuis l'interieur d'un houppier fonctionne aussi.
        const float roots[2] = { (-b - sqrtDisc) / (2.0f * a),
                                 (-b + sqrtDisc) / (2.0f * a) };

        for (const float t : roots) {
            if (t <= 0.05f || t >= closestT) {
                continue;
            }
            const float y = origin.y + direction.y * t;
            if (y < tree.base.y || y > tree.base.y + tree.height) {
                continue;
            }
            closestT = t;
            closestIndex = static_cast<int>(i);
            break;
        }
    }

    return closestIndex;
}

// ---------------------------------------------------------------------------
// Zone degagee : ou l'arbre peut-il tomber ?
// ---------------------------------------------------------------------------
Clearance Forest::evaluateClearance(int treeIndex) const {
    Clearance result;
    if (treeIndex < 0 || static_cast<std::size_t>(treeIndex) >= trees_.size()) {
        return result;
    }

    const Tree& tree = trees_[static_cast<std::size_t>(treeIndex)];

    // Ce qu'il faut de place pour coucher l'arbre entier : sa hauteur.
    const float needed = tree.height;

    // Demi-largeur du couloir balaye par le houppier pendant la chute. On ne
    // simule pas le volume exact : un couloir de largeur constante suffit a
    // repondre a la seule question qui compte ici ("y a-t-il un tronc sur la
    // trajectoire ?").
    const float swath = tree.crownRadius() * 0.8f;

    // Un relief qui monte de plus de ca sur le trajet arrete la chute : l'arbre
    // s'appuierait sur la pente au lieu de se coucher.
    const float uphillTolerance = std::max(1.0f, tree.height * 0.18f);

    struct RayResult {
        glm::vec3 direction;
        float freeDistance;
        float score;
        const char* obstacle;
    };
    std::vector<RayResult> rays;
    rays.reserve(kClearanceRayCount);

    for (int k = 0; k < kClearanceRayCount; ++k) {
        const float angle = 2.0f * kPi * static_cast<float>(k) /
                            static_cast<float>(kClearanceRayCount);
        const glm::vec3 dir(std::cos(angle), 0.0f, std::sin(angle));

        float freeDistance = needed;
        const char* obstacle = "aucun";

        // --- Obstacle 1 : les autres arbres encore debout -------------------
        // Test analytique plutot qu'un echantillonnage le long du rayon : la
        // distance d'un pied d'arbre a la demi-droite se lit directement dans
        // la projection (le long) et le produit vectoriel 2D (le travers).
        for (std::size_t j = 0; j < trees_.size(); ++j) {
            if (static_cast<int>(j) == treeIndex || trees_[j].state != TreeState::Standing) {
                continue;
            }
            const glm::vec3 toOther = trees_[j].base - tree.base;
            const float along = toOther.x * dir.x + toOther.z * dir.z;
            if (along <= 0.0f || along > freeDistance) {
                continue; // derriere, ou plus loin que ce qui nous interesse
            }
            const float lateral = std::abs(cross2D(toOther, dir));
            if (lateral < swath + trees_[j].crownRadius() * 0.7f) {
                freeDistance = std::max(0.0f, along - trees_[j].trunkRadius());
                obstacle = "arbre voisin";
            }
        }

        // --- Obstacle 2 : le relief qui monte -------------------------------
        for (float s = kTerrainProbeStart; s <= freeDistance; s += kTerrainStep) {
            const float h = terrain_.heightAt(tree.base.x + dir.x * s,
                                              tree.base.z + dir.z * s);
            if (h - tree.base.y > uphillTolerance) {
                freeDistance = s;
                obstacle = "relief montant";
                break;
            }
        }

        // --- Score ----------------------------------------------------------
        // D'abord la place disponible, c'est le critere qui compte. A place
        // egale, un bonus pour la pente descendante : c'est la ou un bucheron
        // ferait effectivement tomber l'arbre, et ca departage naturellement
        // les directions en terrain ouvert (sans ce terme, toutes les
        // directions d'une clairiere sont ex aequo).
        const float endH = terrain_.heightAt(tree.base.x + dir.x * freeDistance,
                                             tree.base.z + dir.z * freeDistance);
        const float drop = std::clamp(tree.base.y - endH, 0.0f, 6.0f);
        const float score = freeDistance + 0.35f * drop;

        rays.push_back({ dir, freeDistance, score, obstacle });
    }

    // Meilleur score, puis tirage au sort parmi les quasi ex aequo : deux
    // abattages dans la meme clairiere ne donnent pas mecaniquement la meme
    // direction, sans pour autant sacrifier le choix de la zone la plus degagee.
    float bestScore = rays.front().score;
    for (const RayResult& ray : rays) {
        bestScore = std::max(bestScore, ray.score);
    }

    std::vector<const RayResult*> candidates;
    for (const RayResult& ray : rays) {
        if (ray.score >= bestScore - 0.5f) {
            candidates.push_back(&ray);
        }
    }

    std::uniform_int_distribution<int> pick(0, static_cast<int>(candidates.size()) - 1);
    const RayResult& chosen = *candidates[static_cast<std::size_t>(pick(rng_))];

    result.direction = chosen.direction;
    result.distance = chosen.freeDistance;
    result.obstacle = chosen.obstacle;
    result.cramped = chosen.freeDistance < needed * 0.9f;
    return result;
}

// ---------------------------------------------------------------------------
// Abattage et chute
// ---------------------------------------------------------------------------
bool Forest::fell(int treeIndex, Clearance* clearanceOut) {
    if (treeIndex < 0 || static_cast<std::size_t>(treeIndex) >= trees_.size()) {
        return false;
    }
    Tree& tree = trees_[static_cast<std::size_t>(treeIndex)];
    if (tree.state != TreeState::Standing) {
        return false;
    }

    const Clearance clearance = evaluateClearance(treeIndex);
    if (clearanceOut != nullptr) {
        *clearanceOut = clearance;
    }

    tree.fallDirection = clearance.direction;
    tree.state = TreeState::Falling;

    // Conditions initiales = le coup d'epaule du bucheron sur un arbre deja
    // entaille. Elles ne sont pas cosmetiques : le couple de gravite vaut
    // sin(angle), donc un arbre parfaitement vertical et immobile ne tomberait
    // JAMAIS. Il faut donc un ecart initial non nul.
    //
    // Le choix de la vitesse initiale, lui, est un choix de jeu. Avec un simple
    // ecart et une vitesse nulle, la solution est en cosh(t/tau) : mesure sur
    // cette scene, l'arbre mettait ~3 s a tomber dont les deux premieres
    // secondes visuellement immobiles, ce qui se lit comme un bug. Une vitesse
    // initiale ajoute un terme en sinh(t/tau) qui rend la bascule visible des
    // la premiere demi-seconde, pour une chute complete en ~1,4 s - sans rien
    // changer a l'equation du mouvement, qui reste celle du pendule.
    tree.fallAngle = 0.10f; // rad, ~6 degres : l'arbre "part" sur son entaille
    tree.fallRate = 0.45f;  // rad/s
    return true;
}

void Forest::update(float deltaTime) {
    const float dt = std::clamp(deltaTime, 0.0f, kMaxStep);
    if (dt <= 0.0f) {
        return;
    }

    for (Tree& tree : trees_) {
        if (tree.state == TreeState::Falling) {
            // Pendule inverse : barre homogene de longueur L pivotant autour de
            // son pied. Son moment d'inertie vaut mL²/3 et le couple de la
            // gravite (mgL/2)sin(a), d'ou une acceleration angulaire
            // (3g/2L)sin(a). Consequence physique correcte et bien visible : un
            // grand arbre tombe plus LENTEMENT qu'un petit, parce que L est au
            // denominateur.
            const float angularAccel = (3.0f * kGravity / (2.0f * tree.height)) *
                                       std::sin(tree.fallAngle);
            tree.fallRate += angularAccel * dt;
            tree.fallAngle += tree.fallRate * dt;

            if (tree.fallAngle >= kHalfPi) {
                // Impact. L'arbre ne disparait PAS ici (c'etait le defaut de la
                // Phase 5) : il se pose et reste visible le temps de s'amortir.
                tree.fallAngle = kHalfPi;
                tree.fallRate = 0.0f;
                tree.settleTime = 0.0f;
                tree.state = TreeState::Settling;
                logInfo("Arbre a terre.");
            }
            continue;
        }

        if (tree.state == TreeState::Settling) {
            tree.settleTime += dt;

            // Oscillation amortie SOUS l'horizontale : la cime rebondit d'un
            // poil et retombe. La valeur absolue du sinus evite que l'arbre
            // repasse au-dessus de pi/2, ce qui l'enfoncerait dans le terrain.
            const float decay = std::exp(-kSettleDamping * tree.settleTime);
            const float swing = std::abs(std::sin(kSettlePulsation * tree.settleTime));
            tree.fallAngle = kHalfPi - kSettleAmplitude * decay * swing;

            if (tree.settleTime >= kSettleDuration) {
                tree.fallAngle = kHalfPi;
                tree.state = TreeState::Fallen;
                scheduleBucking(tree);
            }
        }
    }

    advancePendingDrops(dt);
}

void Forest::advancePendingDrops(float deltaTime) {
    if (pendingDrops_.empty()) {
        return;
    }

    // Parcours en place, en preservant l'ordre : les pieces ont ete programmees
    // du pied vers la cime, et c'est dans cet ordre qu'elles doivent apparaitre.
    std::size_t keep = 0;
    for (std::size_t i = 0; i < pendingDrops_.size(); ++i) {
        PendingDrop drop = pendingDrops_[i];
        drop.delay -= deltaTime;

        if (drop.delay <= 0.0f) {
            ground_.drop(drop.kind, drop.worldX, drop.worldZ, drop.yaw, drop.roll);
        } else {
            pendingDrops_[keep] = drop;
            ++keep;
        }
    }
    pendingDrops_.resize(keep);
}

// ---------------------------------------------------------------------------
// Collision : le joueur ne traverse ni les troncs debout ni les souches
// ---------------------------------------------------------------------------
bool Forest::overlaps(const BodyCylinder& body) const {
    for (const Tree& tree : trees_) {
        if (!blocksBody(tree, body)) {
            continue;
        }

        const float reach = tree.colliderRadius() + body.radius;
        const float dx = body.position.x - tree.base.x;
        const float dz = body.position.z - tree.base.z;

        // Marge de tolerance : resolve() repousse le joueur EXACTEMENT au
        // contact, et l'arrondi flottant peut alors laisser la distance un
        // cheveu sous le seuil. Sans cette marge, overlaps() contredirait
        // resolve() sur sa propre sortie et le joueur serait declare bloque
        // partout ou il longe un tronc.
        constexpr float kContactEpsilon = 1e-3f;
        if (dx * dx + dz * dz < (reach - kContactEpsilon) * (reach - kContactEpsilon)) {
            return true;
        }
    }
    return false;
}

glm::vec3 Forest::resolve(const BodyCylinder& body) const {
    glm::vec3 corrected = body.position;

    for (int pass = 0; pass < kCollisionPasses; ++pass) {
        bool touched = false;

        for (const Tree& tree : trees_) {
            if (!blocksBody(tree, body)) {
                continue;
            }

            const float reach = tree.colliderRadius() + body.radius;
            const float dx = corrected.x - tree.base.x;
            const float dz = corrected.z - tree.base.z;
            const float distanceSq = dx * dx + dz * dz;

            if (distanceSq >= reach * reach) {
                continue;
            }

            const float distance = std::sqrt(distanceSq);
            // Cas degenere : le joueur est exactement sur l'axe du tronc (par
            // exemple parce qu'un arbre vient d'etre plante sous ses pieds). Il
            // n'y a alors pas de direction de repoussement privilegiee ; on en
            // choisit une arbitraire mais DETERMINISTE, plutot que de diviser
            // par zero et de propager un NaN dans la position de la camera.
            const glm::vec2 outward = (distance > 1e-4f)
                ? glm::vec2(dx / distance, dz / distance)
                : glm::vec2(1.0f, 0.0f);

            // Repoussement jusqu'au contact, et non annulation du deplacement :
            // c'est ce qui donne gratuitement le glissement le long du tronc.
            corrected.x = tree.base.x + outward.x * reach;
            corrected.z = tree.base.z + outward.y * reach;
            touched = true;
        }

        // Sortir des la premiere passe sans contact : sur le cas courant (aucun
        // obstacle), la seconde passe ne couterait qu'une boucle pour rien.
        if (!touched) {
            break;
        }
    }

    return corrected;
}

int Forest::buckingLogCount(float height, int bonusLogs) {
    return std::clamp(static_cast<int>(height / 2.4f), 2, 4) + std::max(0, bonusLogs);
}

void Forest::scheduleBucking(const Tree& tree) {
    const glm::vec3 f = tree.fallDirection;
    const glm::vec3 perp = horizontalPerp(f);

    // Le mesh de buche est couche le long de +X ; une rotation d'angle a autour
    // de Y envoie +X sur (cos a, 0, -sin a). Aligner sur f impose donc cet
    // atan2 (voir GroundItem::modelMatrix).
    const float alignYaw = std::atan2(-f.z, f.x);

    std::uniform_real_distribution<float> jitter(-0.22f, 0.22f);
    std::uniform_real_distribution<float> lateral(-0.4f, 0.4f);
    std::uniform_real_distribution<float> spin(0.0f, 2.0f * kPi);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    // Un tronc de 7 m debite donne 3 buches : on tronconne la longueur reelle
    // plutot que de sortir un nombre fixe, pour que la quantite de bois suive la
    // taille de l'arbre.
    const int logCount = buckingLogCount(tree.height, 0);

    // On demarre au-dela de la souche : les buches sont debitees dans la partie
    // du tronc qui est effectivement tombee.
    float cursor = tree.stumpHeight() + 0.7f;

    // Delai courant. La PREMIERE buche sort a delai nul, c'est-a-dire dans la
    // frame meme ou l'arbre cesse d'etre dessine : sans ca, le tronc laisserait
    // un trou visible avant que le bois n'arrive.
    float delay = 0.0f;

    for (int k = 0; k < logCount; ++k) {
        cursor += kLogLength * 0.5f;

        const float off = lateral(rng_);
        const glm::vec3 position = tree.base + f * cursor + perp * off;

        PendingDrop drop;
        drop.delay = delay;
        drop.kind = GroundItemKind::Log;
        drop.worldX = position.x;
        drop.worldZ = position.z;
        drop.yaw = alignYaw + jitter(rng_);
        drop.roll = spin(rng_);
        pendingDrops_.push_back(drop);

        cursor += kLogLength * 0.5f + 0.3f;
        delay += kBuckingInterval;
    }

    // PHASE 28 (Agent 3) : buches de la HACHE. Une hache tronconne plus court
    // et plus net : les buches en plus sont rangees A COTE du tronc (decalees
    // d'un metre sur le cote), du pied vers la cime, pour ne jamais depasser la
    // longueur reelle de l'arbre tombe. Aucun tirage aleatoire si bonus = 0 :
    // sans hache, la suite du RNG est celle d'avant (scenario A intact).
    for (int k = 0; k < buckingBonusLogs_; ++k) {
        const float along = tree.stumpHeight() + 0.7f + kLogLength * (0.5f + static_cast<float>(k));
        const float side = (k % 2 == 0) ? 1.0f : -1.0f;
        const glm::vec3 position = tree.base + f * std::min(along, tree.height) + perp * side;

        PendingDrop drop;
        drop.delay = delay;
        drop.kind = GroundItemKind::Log;
        drop.worldX = position.x;
        drop.worldZ = position.z;
        drop.yaw = alignYaw + jitter(rng_);
        drop.roll = spin(rng_);
        pendingDrops_.push_back(drop);

        delay += kBuckingInterval;
    }

    // Les branches se dispersent autour de la partie haute de l'arbre, la ou
    // etait le houppier. Elles suivent le tronc : on ebranche apres avoir
    // tronconne.
    constexpr int kBranchCount = 4;
    for (int k = 0; k < kBranchCount; ++k) {
        const float distance = tree.height * (0.45f + 0.5f * unit(rng_));
        const float off = (unit(rng_) * 2.0f - 1.0f) * tree.crownRadius();

        const glm::vec3 position = tree.base + f * distance + perp * off;

        PendingDrop drop;
        drop.delay = delay;
        drop.kind = GroundItemKind::Branch;
        drop.worldX = position.x;
        drop.worldZ = position.z;
        drop.yaw = spin(rng_);
        drop.roll = spin(rng_);
        pendingDrops_.push_back(drop);

        delay += kBuckingInterval;
    }

    logInfo("Debitage en cours : " + std::to_string(logCount + buckingBonusLogs_) + " buches et " +
            std::to_string(kBranchCount) + " branches a degager.");
}

} // namespace vercora
