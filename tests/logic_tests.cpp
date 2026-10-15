// ===========================================================================
// Tests HEADLESS de la logique de jeu de Vercora.
// ===========================================================================
// Aucun GPU, aucune fenetre, aucun Vulkan : ce fichier ne compile que des unites
// qui ne dependent que de GLM (src/world, src/assets, src/core hors Window).
// Lancement : tools\run_logic_tests.bat  (voir l'en-tete de ce script pour la
// raison d'etre d'un harnais separe du CMakeLists).
//
// PHASE 15 : s'y ajoute tout l'EDITEUR sauf le dessin - assets generes,
// catalogue et scan de dossier, raycast terrain, scene editable, pile
// d'annulation, collision des objets poses, solveur composite, et le format
// de scene .vcsc (aller-retour, robustesse, independance vis-a-vis de
// l'emprise du terrain charge).
//
// Ce qui est couvert : collision joueur/troncs/souches, ramassage et inventaire,
// debitage progressif d'un arbre abattu, taillage de pierre par percussion, le
// chargement d'une heightmap LiDAR .vchm (Phase 7) avec son georeferencement et
// le refus des fichiers casses, et la non-regression des phases precedentes
// (terrain, semis, visee, chute).
//
// Ce qui n'est PAS couvert et ne peut pas l'etre ici : le rendu, les couches de
// validation Vulkan, le resize, la fermeture propre.
// ===========================================================================

#include "assets/Heightmap.hpp"
#include "assets/TerrainFile.hpp"
#include "core/CollisionSolver.hpp"
#include "core/CompositeCollisionSolver.hpp"
#include "core/Frustum.hpp"
#include "core/HeightSampler.hpp"
#include "core/SlopeLimits.hpp"
#include "core/TerrainRay.hpp"
#include "editor/AssetCatalog.hpp"
#include "editor/EditorAssets.hpp"
#include "editor/EditorController.hpp"
#include "editor/EditorScene.hpp"
#include "editor/PlacedObject.hpp"
#include "editor/SceneFile.hpp"
#include "renderer/DrawItem.hpp"
#include "renderer/TerrainMesh.hpp"
#include "renderer/TerrainNodeMesh.hpp"
#include "world/AnimalMesh.hpp"
#include "world/Forest.hpp"
#include "world/Habitat.hpp"
#include "world/Ground.hpp"
#include "world/GroundItem.hpp"
#include "world/Inventory.hpp"
#include "world/Knapping.hpp"
#include "world/Crafting.hpp"
#include "world/CraftMesh.hpp"
#include "world/StoneMesh.hpp"
#include "world/Bike.hpp"
#include "world/Climber.hpp"
#include "world/Needs.hpp"
#include "world/Weather.hpp"
#include "world/WaterSource.hpp"
#include "world/Deposits.hpp"
#include "world/Resin.hpp"
#include "world/FiberPlants.hpp"
#include "world/RockMesh.hpp"
#include "world/Rocks.hpp"
#include "world/BushMesh.hpp"
#include "world/Bushes.hpp"
#include "world/HarvestMesh.hpp"
#include "world/Paraglider.hpp"
#include "assets/TextureFile.hpp"
#include "assets/DetailNoise.hpp"
#include "renderer/InstanceData.hpp"
#include "world/GrassMesh.hpp"
#include "world/TerrainQuadtree.hpp"
#include "world/Vegetation.hpp"
#include "world/Wildlife.hpp"
#include "world/Tree.hpp"
#include "world/TreeMesh.hpp"
#include "world/TreeSpecies.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace vercora;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& label) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  [ECHEC] %s\n", label.c_str());
    }
}

void checkNear(float actual, float expected, float tolerance, const std::string& label) {
    const bool ok = std::abs(actual - expected) <= tolerance;
    if (!ok) {
        std::printf("  (attendu %.4f +/- %.4f, obtenu %.4f)\n", expected, tolerance, actual);
    }
    check(ok, label);
}

void section(const char* name) {
    std::printf("\n== %s ==\n", name);
}

// Terrain plat et parfaitement previsible : les tests de collision et de
// ramassage doivent verifier LEUR logique, pas se battre avec un relief bruite.
class FlatTerrain : public HeightSampler {
public:
    explicit FlatTerrain(float height = 0.0f) : height_(height) {}
    float heightAt(float, float) const override { return height_; }

private:
    float height_;
};

// Terrain en PENTE constante : l'altitude ne depend que de x. Deux usages, tous
// deux impossibles sur un terrain plat :
//  - Phase 11 : exercer d'un coup le filtre de pente et les seuils d'altitude du
//    semis de vegetation ;
//  - Phase 13 : imposer EXACTEMENT la pente sous les roues du velo, dont toute
//    la dynamique longitudinale depend - la deviner sur du relief bruite ne
//    prouverait rien.
// slope est un RISE PAR METRE horizontal (0,20 = 20 %), positif vers +X.
class SlopedTerrain : public HeightSampler {
public:
    SlopedTerrain(float base, float slope) : base_(base), slope_(slope) {}
    float heightAt(float x, float) const override { return base_ + slope_ * x; }

private:
    float base_;
    float slope_;
};

// Fait rouler le velo jusqu'a ce que sa vitesse se stabilise (variation sous
// 1 mm/s d'un pas a l'autre), ou jusqu'a expiration. Renvoie la vitesse finale
// en m/s. C'est ce qui permet de verifier les vitesses D'EQUILIBRE annoncees
// dans world/Bike.hpp sans les recalculer a la main dans le test.
float settleSpeed(Bike& bike, const HeightSampler& terrain, float pedal, float brake,
                  float maxSeconds = 120.0f) {
    constexpr float dt = 1.0f / 60.0f;
    float previous = -1.0f;
    for (float t = 0.0f; t < maxSeconds; t += dt) {
        bike.update(dt, 0.0f, pedal, brake, terrain, nullptr);
        if (t > 1.0f && std::abs(bike.speed() - previous) < 1e-3f * dt * 60.0f) {
            break;
        }
        previous = bike.speed();
    }
    return bike.speed();
}

// Distance horizontale entre deux points.
float horizontalDistance(const glm::vec3& a, const glm::vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// Corps de joueur debout sur un terrain a l'altitude groundY.
BodyCylinder standingBody(float x, float z, float groundY = 0.0f) {
    constexpr float kEyeHeight = 1.7f;
    BodyCylinder body;
    body.position = { x, groundY + kEyeHeight, z };
    body.radius = 0.35f;
    body.feetY = groundY;
    body.headY = groundY + kEyeHeight + 0.15f;
    return body;
}

// Fait tourner le monde jusqu'a ce que l'arbre soit entierement debite, ou
// jusqu'a expiration. Renvoie le temps simule.
float runUntilFallen(Forest& forest, float maxSeconds, float step = 1.0f / 60.0f) {
    float elapsed = 0.0f;
    while (elapsed < maxSeconds) {
        forest.update(step);
        elapsed += step;
        const bool stillMoving =
            forest.trees()[0].state != TreeState::Fallen || forest.pendingDropCount() > 0;
        if (!stillMoving) {
            break;
        }
    }
    return elapsed;
}

// ---------------------------------------------------------------------------
// Non-regression des phases 1 a 5
// ---------------------------------------------------------------------------
void testExistingBehaviourStillWorks() {
    section("Non-regression : terrain, meshes, semis, visee, chute");

    Heightmap heightmap;
    check(heightmap.resolution() == 128, "Heightmap : resolution par defaut");
    check(heightmap.maxHeight() > heightmap.minHeight(), "Heightmap : relief non plat");

    // Reproductibilite : deux terrains de meme graine sont identiques.
    Heightmap twin;
    check(heightmap.heightAt(3.5f, -7.25f) == twin.heightAt(3.5f, -7.25f),
          "Heightmap : meme graine, meme altitude");

    const MeshData tree = treeMeshData(kTreeProportions);
    const MeshData stump = stumpMeshData(kTreeProportions);
    const MeshData log = logMeshData();
    const MeshData branch = branchMeshData();
    check(!tree.empty() && !stump.empty(), "Meshes arbre et souche non vides");
    check(!log.empty() && !branch.empty(), "Meshes buche et branche non vides");

    // Tout indice doit designer un sommet existant, sur TOUS les meshes generes.
    // Un depassement ici ne planterait pas le CPU mais ferait lire au GPU de la
    // memoire arbitraire - exactement le genre d'erreur que les couches de
    // validation signalent et qu'on ne peut pas verifier sur cette machine.
    for (const MeshData* mesh : { &tree, &stump, &log, &branch }) {
        bool indicesValid = true;
        for (const uint32_t index : mesh->indices) {
            if (index >= mesh->vertices.size()) {
                indicesValid = false;
                break;
            }
        }
        check(indicesValid, "Indices dans les bornes : " + mesh->name);
        check(mesh->indices.size() % 3 == 0, "Indices multiples de 3 : " + mesh->name);
    }

    // Semis, visee, abattage.
    FlatTerrain flat;
    Ground ground(flat);
    ForestParams params;
    params.treeCount = 0; // on plante a la main pour un test deterministe
    Forest forest(flat, ground, 50.0f, params);

    const int index = forest.plantTree(0.0f, -10.0f, 7.0f);
    check(index == 0, "plantTree renvoie l'indice du nouvel arbre");
    check(forest.treeCount() == 1, "Un seul arbre plante");

    // Visee depuis l'origine, regard vers -Z : l'arbre est droit devant.
    const int aimed = forest.pickTree({ 0.0f, 1.7f, 0.0f }, { 0.0f, 0.0f, -1.0f }, 40.0f);
    check(aimed == 0, "pickTree touche l'arbre vise");

    const int missed = forest.pickTree({ 0.0f, 1.7f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 40.0f);
    check(missed == kNoTree, "pickTree ne touche rien dans le dos");

    Clearance clearance;
    check(forest.fell(0, &clearance), "fell reussit sur un arbre debout");
    check(!forest.fell(0), "fell refuse un arbre deja coupe");
    checkNear(glm::length(clearance.direction), 1.0f, 1e-3f,
              "Direction de chute normalisee");
}

// ---------------------------------------------------------------------------
// TACHE 1.1 : collision avec les troncs et les souches
// ---------------------------------------------------------------------------
void testCollision() {
    section("Tache 1.1 : collision troncs et souches");

    FlatTerrain flat;
    Ground ground(flat);
    ForestParams params;
    params.treeCount = 0;
    Forest forest(flat, ground, 50.0f, params);

    forest.plantTree(0.0f, 0.0f, 7.0f);
    const Tree& tree = forest.trees()[0];
    const float trunkRadius = tree.trunkRadius();
    const float bodyRadius = 0.35f;
    const float contactDistance = trunkRadius + bodyRadius;

    // 1. Loin du tronc : aucune correction.
    {
        const BodyCylinder body = standingBody(10.0f, 10.0f);
        const glm::vec3 out = forest.resolve(body);
        check(out == body.position, "Loin de tout : position inchangee");
    }

    // 2. Dans le tronc : repousse exactement au contact.
    {
        const BodyCylinder body = standingBody(0.05f, 0.0f);
        const glm::vec3 out = forest.resolve(body);
        const float distance = horizontalDistance(out, tree.base);
        checkNear(distance, contactDistance, 1e-3f,
                  "Dans le tronc : repousse pile au contact");
        check(out.y == body.position.y, "Collision : la verticale n'est pas touchee");
    }

    // 3. Pile sur l'axe du tronc : cas degenere, pas de NaN, repousse quand meme.
    {
        const BodyCylinder body = standingBody(0.0f, 0.0f);
        const glm::vec3 out = forest.resolve(body);
        check(!std::isnan(out.x) && !std::isnan(out.z), "Cas degenere : aucun NaN");
        checkNear(horizontalDistance(out, tree.base), contactDistance, 1e-3f,
                  "Cas degenere : repousse au contact");
    }

    // 4. Juste en dehors du rayon de contact : rien ne bouge.
    {
        const BodyCylinder body = standingBody(contactDistance + 0.01f, 0.0f);
        const glm::vec3 out = forest.resolve(body);
        checkNear(out.x, contactDistance + 0.01f, 1e-4f,
                  "Juste en dehors : position inchangee");
    }

    // 5. Le houppier ne bloque pas : un corps place tres haut, au-dessus de la
    //    cime, ne doit plus rien rencontrer.
    {
        BodyCylinder body = standingBody(0.0f, 0.0f);
        body.feetY = tree.base.y + tree.height + 1.0f;
        body.headY = body.feetY + 1.85f;
        const glm::vec3 out = forest.resolve(body);
        check(out == body.position, "Au-dessus de la cime : pas de collision");
    }

    // 6. Souche : une fois l'arbre entierement debite, l'obstacle se reduit a la
    //    souche. Les pieds sont bloques...
    {
        forest.fell(0);
        runUntilFallen(forest, 12.0f);
        check(forest.trees()[0].state == TreeState::Fallen,
              "L'arbre finit par atteindre l'etat Fallen");

        const BodyCylinder body = standingBody(0.05f, 0.0f);
        const glm::vec3 out = forest.resolve(body);
        checkNear(horizontalDistance(out, tree.base), contactDistance, 1e-3f,
                  "Souche : le joueur ne la traverse pas");
    }

    // 7. ... mais le volume au-dessus de la souche est libre : un corps dont les
    //    pieds sont plus haut que la souche passe.
    {
        const Tree& felled = forest.trees()[0];
        BodyCylinder body = standingBody(0.05f, 0.0f);
        body.feetY = felled.base.y + felled.stumpHeight() + 0.05f;
        body.headY = body.feetY + 1.85f;
        const glm::vec3 out = forest.resolve(body);
        check(out == body.position, "Au-dessus de la souche : plus d'obstacle");
    }

    // 8. Glissement : un joueur pousse contre le tronc mais decale lateralement
    //    doit ressortir decale du meme cote, pas rejete a l'oppose.
    {
        Ground ground2(flat);
        Forest forest2(flat, ground2, 50.0f, params);
        forest2.plantTree(0.0f, 0.0f, 7.0f);

        const BodyCylinder body = standingBody(0.10f, 0.20f);
        const glm::vec3 out = forest2.resolve(body);
        check(out.x > 0.0f && out.z > 0.0f, "Glissement : repousse du bon cote");
        checkNear(horizontalDistance(out, glm::vec3(0.0f)), contactDistance, 1e-3f,
                  "Glissement : ressort au contact");
    }

    // 9. MARCHE reelle vers un passage trop etroit. C'est le vrai scenario de
    //    jeu, et le seul qui compte : le joueur avance pas a pas, la collision
    //    etant resolue a chaque pas. Il ne doit JAMAIS finir a l'interieur d'un
    //    tronc, meme en poussant contre un entre-deux infranchissable.
    //
    //    Deux troncs distants de 1.2 m et larges de 0.63 m laissent un passage
    //    de 0.57 m, alors que le joueur en fait 0.70 : le passage est
    //    geometriquement infranchissable, et c'est voulu - on veut verifier que
    //    la collision refuse proprement, pas qu'elle laisse passer.
    {
        Ground ground3(flat);
        Forest forest3(flat, ground3, 50.0f, params);
        forest3.plantTree(-0.6f, 0.0f, 7.0f);
        forest3.plantTree(0.6f, 0.0f, 7.0f);

        glm::vec3 position{ 0.0f, 1.7f, 4.0f };
        bool everInsideATrunk = false;
        bool everNaN = false;

        // 200 pas de 5 cm droit vers -Z : de quoi traverser largement, si la
        // collision ne faisait pas son travail. On passe par moveBody, comme la
        // camera : c'est lui qui garantit qu'on ne franchit pas un passage plus
        // etroit que soi.
        for (int step = 0; step < 200; ++step) {
            const glm::vec3 previous = position;
            position.z -= 0.05f;

            BodyCylinder body = standingBody(position.x, position.z);
            body.position = position;
            position = moveBody(forest3, body, previous);

            if (std::isnan(position.x) || std::isnan(position.z)) {
                everNaN = true;
                break;
            }
            for (const Tree& other : forest3.trees()) {
                if (horizontalDistance(position, other.base) <
                    other.trunkRadius() + 0.35f - 1e-3f) {
                    everInsideATrunk = true;
                }
            }
        }

        check(!everNaN, "Marche : aucun NaN sur 200 pas");
        check(!everInsideATrunk, "Marche : jamais a l'interieur d'un tronc");
        check(position.z > -0.6f,
              "Marche : le passage trop etroit n'est pas franchi");
    }

    // 10. Le meme passage, mais assez large pour passer : la collision ne doit
    //     pas bloquer un joueur qui a la place de se faufiler.
    {
        Ground ground4(flat);
        Forest forest4(flat, ground4, 50.0f, params);
        forest4.plantTree(-1.6f, 0.0f, 7.0f);
        forest4.plantTree(1.6f, 0.0f, 7.0f);

        glm::vec3 position{ 0.0f, 1.7f, 4.0f };
        for (int step = 0; step < 200; ++step) {
            const glm::vec3 previous = position;
            position.z -= 0.05f;
            BodyCylinder body = standingBody(position.x, position.z);
            body.position = position;
            position = moveBody(forest4, body, previous);
        }
        check(position.z < -4.0f, "Marche : un passage assez large se franchit");
    }

    // 11. L'exigence de base, de bout en bout : marcher droit sur un tronc doit
    //     s'arreter dessus. C'est litteralement ce que la Phase 5 ne faisait pas.
    {
        Ground ground5(flat);
        Forest forest5(flat, ground5, 50.0f, params);
        forest5.plantTree(0.0f, 0.0f, 7.0f);
        const float stopDistance = forest5.trees()[0].trunkRadius() + 0.35f;

        glm::vec3 position{ 0.0f, 1.7f, 4.0f };
        for (int step = 0; step < 200; ++step) {
            const glm::vec3 previous = position;
            position.z -= 0.05f;
            BodyCylinder body = standingBody(position.x, position.z);
            body.position = position;
            position = moveBody(forest5, body, previous);
        }
        check(position.z > 0.0f, "Marche : le tronc arrete le joueur");
        checkNear(horizontalDistance(position, glm::vec3(0.0f)), stopDistance, 0.06f,
                  "Marche : le joueur s'arrete au contact du tronc");
    }

    // 12. Une souche, elle, arrete aussi le joueur (exigence explicite), et
    //     resolve()/overlaps() doivent etre d'accord sur leur propre sortie.
    {
        Ground ground6(flat);
        Forest forest6(flat, ground6, 50.0f, params);
        forest6.plantTree(0.0f, 0.0f, 7.0f);
        forest6.fell(0);
        runUntilFallen(forest6, 12.0f);

        glm::vec3 position{ 0.0f, 1.7f, 4.0f };
        for (int step = 0; step < 200; ++step) {
            const glm::vec3 previous = position;
            position.z -= 0.05f;
            BodyCylinder body = standingBody(position.x, position.z);
            body.position = position;
            position = moveBody(forest6, body, previous);
        }
        check(position.z > 0.0f, "Marche : la souche arrete le joueur");

        BodyCylinder settled = standingBody(position.x, position.z);
        settled.position = position;
        check(!forest6.overlaps(settled),
              "overlaps() et resolve() sont d'accord sur la position finale");
    }
}

// ---------------------------------------------------------------------------
// TACHE 1.2 : ramassage reel des buches et branches
// ---------------------------------------------------------------------------
void testPickup() {
    section("Tache 1.2 : ramassage et inventaire");

    FlatTerrain flat(12.0f);
    Ground ground(flat);

    check(ground.itemCount() == 0, "Sol vide au depart");

    ground.drop(GroundItemKind::Log, 2.0f, 0.0f, 0.0f, 0.0f);
    ground.drop(GroundItemKind::Branch, 20.0f, 0.0f, 0.0f, 0.0f);
    check(ground.itemCount() == 2, "Deux objets deposes");

    // Depose sur le terrain, pas dedans ni au-dessus.
    checkNear(ground.items()[0].position.y, 12.0f + kLogRadius, 1e-4f,
              "Buche posee sur la surface du terrain");

    const glm::vec3 player{ 0.0f, 13.7f, 0.0f };

    // Hors de portee : rien.
    check(ground.findNearest(player, 1.0f) == kNoGroundItem,
          "Rien a portee de 1 m");

    // A portee : la buche, pas la branche lointaine.
    const int nearest = ground.findNearest(player, 3.0f);
    check(nearest != kNoGroundItem, "Buche trouvee a portee de 3 m");

    GroundItemKind picked = GroundItemKind::Stone;
    check(ground.pickUp(nearest, &picked), "Ramassage reussi");
    check(picked == GroundItemKind::Log, "L'objet ramasse est bien la buche");
    check(ground.itemCount() == 1, "L'objet a disparu de la scene");

    // Le meme indice ne doit plus rien donner de valide au-dela de la taille.
    check(!ground.pickUp(5), "Indice hors bornes refuse");
    check(!ground.pickUp(-1), "Indice negatif refuse");

    // Inventaire.
    Inventory inventory;
    check(inventory.total() == 0, "Inventaire vide au depart");
    inventory.add(picked);
    check(inventory.count(GroundItemKind::Log) == 1, "Une buche comptee");
    check(inventory.total() == 1, "Total d'inventaire coherent");
    check(!inventory.remove(GroundItemKind::Log, 2), "Retrait impossible refuse");
    check(inventory.count(GroundItemKind::Log) == 1,
          "Un retrait refuse ne modifie rien");
    check(inventory.remove(GroundItemKind::Log), "Retrait possible accepte");
    check(inventory.total() == 0, "Inventaire vide apres retrait");

    // Le resume doit nommer les quatre natures : c'est ce qui s'affiche dans le
    // titre de la fenetre.
    const std::string summary = inventory.summary();
    check(summary.find("buches") != std::string::npos, "Resume : buches");
    check(summary.find("branches") != std::string::npos, "Resume : branches");
    check(summary.find("pierres") != std::string::npos, "Resume : pierres");
    check(summary.find("eclats") != std::string::npos, "Resume : eclats");

    // Ramassage a repetition : tout doit pouvoir etre vide sans trou ni doublon
    // (le pickUp par echange avec le dernier element est un piege classique).
    {
        Ground bulk(flat);
        for (int i = 0; i < 10; ++i) {
            bulk.drop(GroundItemKind::Stone, static_cast<float>(i) * 0.1f, 0.0f, 0.0f, 0.0f);
        }
        int collected = 0;
        while (true) {
            const int index = bulk.findNearest({ 0.0f, 13.7f, 0.0f }, 50.0f);
            if (index == kNoGroundItem) {
                break;
            }
            if (!bulk.pickUp(index)) {
                break;
            }
            ++collected;
        }
        check(collected == 10, "Les 10 objets sont tous ramassables");
        check(bulk.itemCount() == 0, "Sol vide apres ramassage complet");
    }
}

// ---------------------------------------------------------------------------
// TACHE 1.3 : transition de chute et debitage progressif
// ---------------------------------------------------------------------------
void testProgressiveFall() {
    section("Tache 1.3 : chute, amortissement, debitage progressif");

    FlatTerrain flat;
    Ground ground(flat);
    ForestParams params;
    params.treeCount = 0;
    Forest forest(flat, ground, 50.0f, params);
    forest.plantTree(0.0f, 0.0f, 7.0f);

    check(forest.trees()[0].state == TreeState::Standing, "Arbre debout au depart");
    forest.fell(0);
    check(forest.trees()[0].state == TreeState::Falling, "Arbre en chute apres fell");

    constexpr float kStep = 1.0f / 60.0f;

    // Phase 1 : la bascule. Rien ne doit tomber au sol pendant ce temps.
    float elapsed = 0.0f;
    while (forest.trees()[0].state == TreeState::Falling && elapsed < 10.0f) {
        forest.update(kStep);
        elapsed += kStep;
    }
    check(forest.trees()[0].state == TreeState::Settling,
          "La chute debouche sur l'amortissement, pas sur la disparition");
    check(ground.itemCount() == 0, "Aucun bois au sol pendant la chute");
    check(elapsed < 4.0f, "La chute dure un temps raisonnable");

    // Phase 2 : l'amortissement. L'arbre est TOUJOURS dessine (state != Fallen)
    // et reste couche, jamais au-dessus de l'horizontale.
    constexpr float kHalfPi = 1.57079632679f;
    float settlingFrames = 0.0f;
    bool stayedBelowHorizontal = true;
    while (forest.trees()[0].state == TreeState::Settling && settlingFrames < 5.0f) {
        forest.update(kStep);
        settlingFrames += kStep;
        if (forest.trees()[0].fallAngle > kHalfPi + 1e-4f) {
            stayedBelowHorizontal = false;
        }
    }
    check(settlingFrames > 0.3f, "L'arbre reste visible au sol un temps lisible");
    check(stayedBelowHorizontal,
          "L'amortissement ne fait jamais repasser l'arbre sous terre");
    check(forest.trees()[0].state == TreeState::Fallen,
          "L'amortissement debouche sur le debitage");

    // Phase 3 : le debitage. Les pieces arrivent une par une, pas toutes d'un
    // coup - c'est precisement le defaut de la Phase 5 qu'on corrige ici.
    const std::size_t afterFirstFrame = ground.itemCount();
    check(afterFirstFrame >= 1, "La premiere buche sort des la disparition du tronc");
    check(forest.pendingDropCount() > 0, "Il reste du bois a degager");
    check(afterFirstFrame < 7, "Le bois n'apparait PAS tout d'un coup");

    // On laisse le debitage se terminer.
    int distinctCounts = 0;
    std::size_t previous = afterFirstFrame;
    float buckingTime = 0.0f;
    while (forest.pendingDropCount() > 0 && buckingTime < 10.0f) {
        forest.update(kStep);
        buckingTime += kStep;
        if (ground.itemCount() != previous) {
            ++distinctCounts;
            previous = ground.itemCount();
        }
    }

    check(forest.pendingDropCount() == 0, "Tout le bois a fini par tomber");
    check(distinctCounts >= 3, "Le bois est apparu en plusieurs fois");
    check(buckingTime > 0.3f, "Le debitage s'etale dans le temps");

    // Un arbre de 7 m : 2 buches (7/2.4 = 2) et 4 branches.
    const int logs = static_cast<int>(7.0f / 2.4f);
    check(ground.itemCount() == static_cast<std::size_t>(logs + 4),
          "Nombre total de pieces conforme");

    int logCount = 0;
    int branchCount = 0;
    for (const GroundItem& item : ground.items()) {
        if (item.kind == GroundItemKind::Log) ++logCount;
        if (item.kind == GroundItemKind::Branch) ++branchCount;
    }
    check(logCount == logs, "Le bon nombre de buches");
    check(branchCount == 4, "Le bon nombre de branches");

    // Plus rien ne doit bouger une fois tout retombe.
    const std::size_t settled = ground.itemCount();
    for (int i = 0; i < 120; ++i) {
        forest.update(kStep);
    }
    check(ground.itemCount() == settled, "Etat stable apres debitage");
}

// ---------------------------------------------------------------------------
// TACHE 2 : pierres et taillage par percussion
// ---------------------------------------------------------------------------
void testStoneKnapping() {
    section("Tache 2 : pierres ramassables et percussion");

    // Meshes generes en code.
    const MeshData stone = stoneMeshData();
    const MeshData flake = flakeMeshData();
    check(!stone.empty(), "Mesh de pierre non vide");
    check(!flake.empty(), "Mesh d'eclat non vide");
    check(stone.indices.size() % 3 == 0, "Pierre : indices multiples de 3");
    check(flake.indices.size() % 3 == 0, "Eclat : indices multiples de 3");

    for (const MeshData* mesh : { &stone, &flake }) {
        bool indicesValid = true;
        bool normalsUnit = true;
        bool withinBounds = true;
        for (const uint32_t index : mesh->indices) {
            if (index >= mesh->vertices.size()) {
                indicesValid = false;
                break;
            }
        }
        for (const Vertex& vertex : mesh->vertices) {
            if (std::abs(glm::length(vertex.normal) - 1.0f) > 1e-3f) {
                normalsUnit = false;
            }
            // Un objet ramassable doit tenir dans un volume de la taille d'une
            // main : une erreur d'echelle donnerait un rocher de 3 m qu'on
            // ramasserait quand meme, sans que rien ne le signale.
            if (std::abs(vertex.pos.x) > 0.4f || std::abs(vertex.pos.y) > 0.4f ||
                std::abs(vertex.pos.z) > 0.4f) {
                withinBounds = false;
            }
        }
        check(indicesValid, "Indices dans les bornes : " + mesh->name);
        check(normalsUnit, "Normales unitaires : " + mesh->name);
        check(withinBounds, "Dimensions plausibles : " + mesh->name);
    }

    // Semis de pierres.
    FlatTerrain flat;
    Ground ground(flat);
    const uint32_t placed = ground.scatterStones(2, 20.0f, 4.0f, 50.0f);
    check(placed == 2, "Les deux pierres demandees sont semees");
    check(ground.itemCount() == 2, "Deux objets au sol");

    bool allStones = true;
    bool allReachable = true;
    for (const GroundItem& item : ground.items()) {
        if (item.kind != GroundItemKind::Stone) allStones = false;
        const float distance = std::sqrt(item.position.x * item.position.x +
                                         item.position.z * item.position.z);
        if (distance < 4.0f || distance > 20.0f) allReachable = false;
    }
    check(allStones, "Les objets semes sont bien des pierres");
    check(allReachable, "Pierres semees dans la couronne demandee");
    checkNear(ground.items()[0].position.y, kStoneRadius, 1e-4f,
              "Pierre posee sur la surface du terrain");

    // Percussion.
    Inventory inventory;
    const glm::vec3 player{ 0.0f, 1.7f, 0.0f };
    const glm::vec3 facing{ 0.0f, -0.2f, -1.0f }; // regard vers -Z, legerement baisse

    check(knapStones(inventory, ground, player, facing) == KnapResult::NotEnoughStones,
          "Percussion refusee sans pierre");

    inventory.add(GroundItemKind::Stone);
    check(knapStones(inventory, ground, player, facing) == KnapResult::NotEnoughStones,
          "Percussion refusee avec une seule pierre");
    check(inventory.count(GroundItemKind::Stone) == 1,
          "Un echec ne consomme aucune pierre");

    inventory.add(GroundItemKind::Stone);
    const std::size_t beforeKnap = ground.itemCount();
    check(knapStones(inventory, ground, player, facing) == KnapResult::Success,
          "Percussion reussie avec deux pierres");
    check(inventory.count(GroundItemKind::Stone) == 1,
          "La percussion consomme exactement une pierre");
    check(ground.itemCount() == beforeKnap + 1, "Un objet est apparu dans le monde");

    // L'eclat existe dans le MONDE (et non dans l'inventaire), devant le joueur.
    const GroundItem& produced = ground.items().back();
    check(produced.kind == GroundItemKind::Flake, "L'objet produit est un eclat");
    check(inventory.count(GroundItemKind::Flake) == 0,
          "L'eclat n'atterrit pas directement dans l'inventaire");
    check(produced.position.z < player.z, "L'eclat tombe devant le joueur (vers -Z)");
    checkNear(horizontalDistance(produced.position, player), 1.1f, 1e-3f,
              "L'eclat tombe a portee de bras");

    // ... et il est ramassable comme n'importe quoi d'autre.
    const int index = ground.findNearest(player, 2.0f);
    check(index != kNoGroundItem, "L'eclat est a portee de ramassage");
    GroundItemKind kind = GroundItemKind::Log;
    check(ground.pickUp(index, &kind) && kind == GroundItemKind::Flake,
          "L'eclat se ramasse");

    // Regard parfaitement vertical : cas degenere, aucun NaN dans la position.
    {
        Inventory vertical;
        vertical.add(GroundItemKind::Stone, 2);
        Ground groundVertical(flat);
        check(knapStones(vertical, groundVertical, player, { 0.0f, -1.0f, 0.0f }) ==
                  KnapResult::Success,
              "Percussion possible en regardant ses pieds");
        const GroundItem& item = groundVertical.items().back();
        check(!std::isnan(item.position.x) && !std::isnan(item.position.z),
              "Regard vertical : aucun NaN dans la position de l'eclat");
    }
}

// ---------------------------------------------------------------------------
// Construction de la liste d'objets a dessiner
// ---------------------------------------------------------------------------
// main.cpp ne peut PAS etre compile par ce harnais : il tire le Renderer, donc
// Vulkan. Mais DrawItem, lui, ne depend que de GLM - on peut donc rejouer ici la
// partie la plus facile a casser de la boucle de rendu (quel objet est dessine
// dans quel etat, et avec quel mesh) et verifier qu'elle tient.
//
// Ce test ne prouve pas que main.cpp compile. Il prouve que la LOGIQUE qu'il
// applique est correcte, ce qui est le principal risque a cet endroit.
void testDrawListConstruction() {
    section("Rendu : composition de la liste d'objets");

    FlatTerrain flat;
    Ground ground(flat);
    ForestParams params;
    params.treeCount = 0;
    Forest forest(flat, ground, 50.0f, params);
    forest.plantTree(0.0f, -8.0f, 7.0f);

    // Table mesh-par-nature, exactement comme dans main.cpp : c'est le passage
    // ou un oubli donnerait un kInvalidMesh envoye au Renderer.
    std::array<MeshHandle, kGroundItemKindCount> groundMeshes{};
    groundMeshes.fill(kInvalidMesh);
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Log)] = 10;
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Branch)] = 11;
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Stone)] = 12;
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Flake)] = 13;
    // PHASE 22 : les quatre matieres ajoutees doivent elles aussi avoir leur
    // mesh. C'est CE test qui a signale leur oubli quand l'enum a grandi.
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Clay)] = 14;
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Sand)] = 15;
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Resin)] = 16;
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Fiber)] = 17;
    // PHASE 28 : les cinq objets faconnes, comme dans main.cpp.
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Cord)] = 18;
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Axe)] = 19;
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Hearth)] = 20;
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Ash)] = 21;
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Pitch)] = 22;
    // PHASE 28 (Agent 3) : hache renforcee et abri.
    groundMeshes[static_cast<std::size_t>(GroundItemKind::PitchedAxe)] = 23;
    groundMeshes[static_cast<std::size_t>(GroundItemKind::Shelter)] = 24;

    bool everyKindHasAMesh = true;
    for (const MeshHandle handle : groundMeshes) {
        if (handle == kInvalidMesh) {
            everyKindHasAMesh = false;
        }
    }
    check(everyKindHasAMesh,
          "Chaque nature d'objet au sol a son mesh (aucun trou dans la table)");

    constexpr MeshHandle kTreeMesh = 1;
    constexpr MeshHandle kStumpMesh = 2;

    // Reproduit la boucle de main.cpp.
    const auto buildDrawItems = [&]() {
        std::vector<DrawItem> items;
        for (const Tree& tree : forest.trees()) {
            if (tree.state != TreeState::Standing) {
                items.push_back({ kStumpMesh, tree.stumpMatrix(), Material::Object });
            }
            if (tree.state != TreeState::Fallen) {
                items.push_back({ kTreeMesh, tree.modelMatrix(), Material::Object });
            }
        }
        for (const GroundItem& item : ground.items()) {
            const MeshHandle mesh = groundMeshes[static_cast<std::size_t>(item.kind)];
            if (mesh != kInvalidMesh) {
                items.push_back({ mesh, item.modelMatrix(), Material::Object });
            }
        }
        return items;
    };

    // Debout : l'arbre seul, pas de souche.
    {
        const std::vector<DrawItem> items = buildDrawItems();
        check(items.size() == 1, "Arbre debout : un seul objet dessine");
        check(items[0].mesh == kTreeMesh, "Arbre debout : c'est le mesh d'arbre");
    }

    constexpr float kStep = 1.0f / 60.0f;
    forest.fell(0);

    // En chute : souche + arbre incline. C'est ce couple qui rend le trait de
    // coupe lisible.
    forest.update(kStep);
    {
        const std::vector<DrawItem> items = buildDrawItems();
        check(items.size() == 2, "Arbre en chute : souche + arbre");
    }

    // Amortissement : l'arbre est TOUJOURS dessine. C'est tout l'objet de la
    // Tache 1.3 - en Phase 5 il avait deja disparu a cet instant.
    while (forest.trees()[0].state == TreeState::Falling) {
        forest.update(kStep);
    }
    {
        const std::vector<DrawItem> items = buildDrawItems();
        check(forest.trees()[0].state == TreeState::Settling, "Etat Settling atteint");
        check(items.size() == 2, "Arbre couche en amortissement : encore dessine");
        check(ground.items().empty(), "Amortissement : rien encore au sol");
    }

    // Debitage termine : plus d'arbre, une souche, et tout le bois.
    runUntilFallen(forest, 12.0f);
    {
        const std::vector<DrawItem> items = buildDrawItems();
        const std::size_t expected = 1 + ground.items().size(); // souche + bois

        check(items.size() == expected, "Arbre debite : souche + tout le bois");

        bool anyTreeMesh = false;
        bool allHandlesValid = true;
        for (const DrawItem& item : items) {
            if (item.mesh == kTreeMesh) anyTreeMesh = true;
            if (item.mesh == kInvalidMesh) allHandlesValid = false;
        }
        check(!anyTreeMesh, "Arbre debite : le mesh d'arbre n'est plus dessine");
        check(allHandlesValid, "Aucun mesh invalide envoye au rendu");
    }

    // Les matrices d'instance doivent rester finies : un NaN dans une model
    // matrix ne fait pas planter le CPU, il fait disparaitre l'objet a l'ecran
    // (ou pire, tout l'ecran) sans le moindre message.
    {
        ground.drop(GroundItemKind::Stone, 3.0f, 4.0f, 1.2f, 0.4f);
        const std::vector<DrawItem> items = buildDrawItems();
        bool allFinite = true;
        for (const DrawItem& item : items) {
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    if (std::isnan(item.model[column][row]) ||
                        std::isinf(item.model[column][row])) {
                        allFinite = false;
                    }
                }
            }
        }
        check(allFinite, "Toutes les matrices d'instance sont finies");
    }
}

// ---------------------------------------------------------------------------
// PHASE 7 : heightmap chargee depuis un fichier .vchm (donnees LiDAR reelles)
// ---------------------------------------------------------------------------
// La grille de test est construite ICI, octet par octet, plutot que lue depuis
// assets/ : le harnais reste ainsi executable sur un depot frais, ou les 4 Mo de
// heightmap et les 167 Mo de nuage de points ne sont pas versionnes. Le vrai
// fichier, s'il est present, est verifie en plus a la fin.

constexpr std::size_t kVchmHeaderSize = 120;

// Ecrit un .vchm minimal. Reproduit le format decrit dans TerrainFile.hpp, sans
// rien partager avec le lecteur : une erreur de decalage dans l'un ne peut pas
// se compenser dans l'autre.
void writeVchm(const std::filesystem::path& path, uint32_t nx, uint32_t nz,
               float spacing, const std::vector<float>& heights,
               const char* label = "grille de test",
               uint32_t version = 1, const char* magic = "VCHM",
               double originEast = 907000.0, double originNorth = 6440000.0,
               uint32_t epsg = 2154, uint32_t flags = 1,
               bool lieAboutBounds = false) {
    unsigned char header[kVchmHeaderSize] = {};
    std::memcpy(header + 0, magic, 4);
    std::memcpy(header + 4, &version, 4);
    std::memcpy(header + 8, &nx, 4);
    std::memcpy(header + 12, &nz, 4);
    std::memcpy(header + 16, &spacing, 4);

    std::memcpy(header + 24, &originEast, 8);
    std::memcpy(header + 32, &originNorth, 8);

    float lo = heights.empty() ? 0.0f : heights[0];
    float hi = lo;
    for (float h : heights) { lo = std::min(lo, h); hi = std::max(hi, h); }
    if (lieAboutBounds) { lo -= 100.0f; hi += 100.0f; }
    std::memcpy(header + 40, &lo, 4);
    std::memcpy(header + 44, &hi, 4);
    std::memcpy(header + 48, &epsg, 4);
    std::memcpy(header + 52, &flags, 4);
    std::memcpy(header + 56, label, std::min<std::size_t>(std::strlen(label), 63));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(header), kVchmHeaderSize);
    out.write(reinterpret_cast<const char*>(heights.data()),
              static_cast<std::streamsize>(heights.size() * sizeof(float)));
}

// true si loadTerrainFile refuse le fichier. Un fichier casse DOIT lever :
// charger un terrain a moitie faux et s'en apercevoir a l'ecran est exactement
// ce que ces gardes-fous evitent.
bool rejects(const std::filesystem::path& path) {
    try {
        loadTerrainFile(path.string());
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

void testTerrainFile() {
    section("Phase 7 : heightmap LiDAR chargee depuis un fichier .vchm");

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "vercora_tests";
    std::filesystem::create_directories(dir);

    // Grille ASYMETRIQUE dans les deux directions, et avec des coefficients
    // differents sur chaque axe : c'est ce qui rend un axe inverse ou deux axes
    // echanges detectables. Une grille symetrique passerait tous les tests
    // ci-dessous meme lue de travers.
    const uint32_t n = 5;
    const float spacing = 2.0f;
    std::vector<float> heights(static_cast<std::size_t>(n) * n);
    for (uint32_t iz = 0; iz < n; ++iz) {
        for (uint32_t ix = 0; ix < n; ++ix) {
            heights[static_cast<std::size_t>(iz) * n + ix] =
                400.0f + static_cast<float>(ix) + 10.0f * static_cast<float>(iz);
        }
    }

    const std::filesystem::path good = dir / "good.vchm";
    writeVchm(good, n, n, spacing, heights, "dalle de test");

    {
        const Heightmap loaded = loadTerrainFile(good.string());

        check(loaded.resolution() == n, "Fichier : resolution relue");
        checkNear(loaded.spacing(), spacing, 1e-6f, "Fichier : pas relu");
        // worldSize DECOULE du pas : (n - 1) cellules de 2 m = 8 m.
        checkNear(loaded.worldSize(), 8.0f, 1e-6f, "Fichier : emprise deduite du pas");
        checkNear(loaded.minHeight(), 400.0f, 1e-4f, "Fichier : altitude minimale");
        checkNear(loaded.maxHeight(), 444.0f, 1e-4f, "Fichier : altitude maximale");
        check(loaded.describe() == "dalle de test", "Fichier : label repris dans describe()");

        // Indexation : at(ix, iz), donc l'altitude doit croitre de 1 vers l'est
        // et de 10 vers le sud. Inverser les deux boucles de lecture donnerait
        // ici 10 et 1 - le test discrimine.
        checkNear(loaded.at(0, 0), 400.0f, 1e-4f, "Fichier : coin (0,0)");
        checkNear(loaded.at(4, 0), 404.0f, 1e-4f, "Fichier : +1 par pas vers l'est");
        checkNear(loaded.at(0, 4), 440.0f, 1e-4f, "Fichier : +10 par pas vers le sud");
        checkNear(loaded.at(4, 4), 444.0f, 1e-4f, "Fichier : coin oppose");

        // Le terrain est centre sur l'origine : l'echantillon (0,0) est donc en
        // (-4, -4) et le dernier en (+4, +4).
        checkNear(loaded.worldCoord(0), -4.0f, 1e-6f, "Fichier : bord ouest a -worldSize/2");
        checkNear(loaded.worldCoord(4), 4.0f, 1e-6f, "Fichier : bord est a +worldSize/2");
        checkNear(loaded.heightAt(-4.0f, -4.0f), 400.0f, 1e-4f,
                  "Fichier : heightAt au coin nord-ouest");
        checkNear(loaded.heightAt(4.0f, 4.0f), 444.0f, 1e-4f,
                  "Fichier : heightAt au coin sud-est");
        // Milieu d'une cellule : l'interpolation affine doit donner la moyenne
        // exacte des deux extremites sur cette grille lineaire.
        checkNear(loaded.heightAt(-3.0f, -4.0f), 400.5f, 1e-4f,
                  "Fichier : heightAt interpole entre deux echantillons");

        // Georeferencement.
        check(loaded.geo().isGeoreferenced(), "Fichier : grille georeferencee");
        check(loaded.geo().epsg == 2154, "Fichier : EPSG relu");
        check(loaded.geo().absoluteAltitude, "Fichier : drapeau d'altitude absolue");

        // toProjected : le centre du monde tombe au centre de la dalle, et les
        // deux axes ont des SIGNES OPPOSES en projection (est croissant, nord
        // decroissant). C'est la seule verification ecrite de cette regle.
        double east = 0.0, north = 0.0;
        loaded.toProjected(0.0f, 0.0f, &east, &north);
        checkNear(static_cast<float>(east), 907004.0f, 1e-2f,
                  "toProjected : centre, coordonnee est");
        checkNear(static_cast<float>(north), 6439996.0f, 1e-2f,
                  "toProjected : centre, coordonnee nord");

        loaded.toProjected(-4.0f, -4.0f, &east, &north);
        checkNear(static_cast<float>(east), 907000.0f, 1e-2f,
                  "toProjected : coin nord-ouest = origine du fichier (est)");
        checkNear(static_cast<float>(north), 6440000.0f, 1e-2f,
                  "toProjected : coin nord-ouest = origine du fichier (nord)");

        loaded.toProjected(4.0f, 4.0f, &east, &north);
        checkNear(static_cast<float>(east), 907008.0f, 1e-2f,
                  "toProjected : aller a l'est AUGMENTE l'abscisse projetee");
        checkNear(static_cast<float>(north), 6439992.0f, 1e-2f,
                  "toProjected : aller au sud (+Z) DIMINUE l'ordonnee projetee");

        // Le maillage reste celui de la Phase 4, sans adaptation : c'etait tout
        // l'interet de faire passer les donnees par Heightmap.
        const MeshData mesh = terrainMeshData(loaded);
        check(mesh.vertices.size() == static_cast<std::size_t>(n) * n,
              "TerrainMesh : un sommet par echantillon");
        check(mesh.triangleCount() == (n - 1) * (n - 1) * 2,
              "TerrainMesh : deux triangles par cellule");
        check(mesh.name == "dalle de test", "TerrainMesh : nom repris de la heightmap");
        bool posMatches = true;
        for (uint32_t iz = 0; iz < n; ++iz) {
            for (uint32_t ix = 0; ix < n; ++ix) {
                const Vertex& v = mesh.vertices[static_cast<std::size_t>(iz) * n + ix];
                if (std::abs(v.pos.y - loaded.at(ix, iz)) > 1e-4f) { posMatches = false; }
            }
        }
        check(posMatches, "TerrainMesh : altitude de chaque sommet = celle de la grille");
    }

    // --- Fichiers refuses ---------------------------------------------------
    {
        const std::filesystem::path bad = dir / "bad.vchm";

        writeVchm(bad, n, n, spacing, heights, "x", 1, "NOPE");
        check(rejects(bad), "Refus : magie incorrecte");

        writeVchm(bad, n, n, spacing, heights, "x", 99);
        check(rejects(bad), "Refus : version inconnue");

        // Grille rectangulaire : limite assumee, mais elle doit etre SIGNALEE.
        std::vector<float> rect(static_cast<std::size_t>(n) * (n + 1), 1.0f);
        writeVchm(bad, n, n + 1, spacing, rect);
        check(rejects(bad), "Refus : grille non carree");

        writeVchm(bad, n, n, 0.0f, heights);
        check(rejects(bad), "Refus : pas de grille nul");

        writeVchm(bad, 1, 1, spacing, std::vector<float>{1.0f});
        check(rejects(bad), "Refus : grille d'un seul echantillon");

        // Tronque : l'en-tete annonce 25 altitudes, le fichier n'en contient
        // que 10. Sans le controle de taille, on lirait des zeros en silence.
        writeVchm(bad, n, n, spacing, heights);
        std::filesystem::resize_file(bad, kVchmHeaderSize + 10 * sizeof(float));
        check(rejects(bad), "Refus : fichier tronque");

        // NaN : le cas typique d'une cellule sans point de retour mal traitee en
        // amont. Un seul suffit a produire un triangle degenere.
        std::vector<float> withNan = heights;
        withNan[7] = std::numeric_limits<float>::quiet_NaN();
        writeVchm(bad, n, n, spacing, withNan);
        check(rejects(bad), "Refus : altitude NaN");

        check(rejects(dir / "ce-fichier-n-existe-pas.vchm"), "Refus : fichier absent");

        // En-tete qui ment sur les bornes : accepte (les donnees font foi) mais
        // doit produire un avertissement. On verifie au moins que ca ne casse pas.
        writeVchm(bad, n, n, spacing, heights, "bornes fausses", 1, "VCHM",
                  907000.0, 6440000.0, 2154, 1, /*lieAboutBounds=*/true);
        bool loadedDespiteBadBounds = false;
        try {
            const Heightmap h = loadTerrainFile(bad.string());
            loadedDespiteBadBounds = (std::abs(h.minHeight() - 400.0f) < 1e-4f);
        } catch (const std::exception&) {}
        check(loadedDespiteBadBounds,
              "Bornes d'en-tete fausses : les DONNEES font foi, le chargement passe");
    }

    std::filesystem::remove_all(dir);

    // --- La vraie dalle, si elle a ete produite ------------------------------
    // Conditionnel a dessein : le depot ne versionne pas les 4 Mo de heightmap.
    // Quand elle est la, ce bloc est le seul test qui touche de VRAIES donnees.
    const std::filesystem::path realTile =
        std::filesystem::path(ASSETS_DIR) / "terrain" / "LHD_0907_6440_1m.vchm";
    if (!std::filesystem::exists(realTile)) {
        std::printf("  (dalle reelle absente, bloc ignore : %s)\n",
                    realTile.string().c_str());
        return;
    }

    const Heightmap tile = loadTerrainFile(realTile.string());
    check(tile.resolution() == 1001, "Dalle reelle : 1001 echantillons par cote");
    checkNear(tile.spacing(), 1.0f, 1e-6f, "Dalle reelle : pas de 1 m");
    checkNear(tile.worldSize(), 1000.0f, 1e-3f, "Dalle reelle : emprise de 1 km");
    check(tile.geo().epsg == 2154, "Dalle reelle : Lambert 93");
    // Altitudes plausibles pour un relief de moyenne montagne, et surtout un
    // denivele qui exclut un terrain plat par erreur de lecture.
    check(tile.minHeight() > 400.0f && tile.maxHeight() < 1000.0f,
          "Dalle reelle : altitudes dans une plage plausible");
    check(tile.maxHeight() - tile.minHeight() > 500.0f,
          "Dalle reelle : denivele reel de plus de 500 m");

    double east = 0.0, north = 0.0;
    tile.toProjected(0.0f, 0.0f, &east, &north);
    checkNear(static_cast<float>(east - 907000.0), 500.0f, 1e-2f,
              "Dalle reelle : l'origine du monde est au centre de la dalle (est)");
    checkNear(static_cast<float>(north - 6439000.0), 500.0f, 1e-2f,
              "Dalle reelle : l'origine du monde est au centre de la dalle (nord)");
    // Valeur relevee independamment par tools\check_terrain_file.py sur le nuage
    // brut : la mediane des points sol a E 907500 / N 6439500 vaut 605.38 m.
    checkNear(tile.heightAt(0.0f, 0.0f), 605.38f, 0.01f,
              "Dalle reelle : altitude au point de depart conforme au nuage brut");

    const MeshData mesh = terrainMeshData(tile);
    check(mesh.vertices.size() == 1001u * 1001u, "Dalle reelle : 1 002 001 sommets");
    check(mesh.triangleCount() == 1000u * 1000u * 2u, "Dalle reelle : 2 000 000 triangles");
}

// ---------------------------------------------------------------------------
// Phase 8 : physique de base du parapente
// ---------------------------------------------------------------------------
void testParaglider() {
    section("Phase 8 : physique de base du parapente");

    // --- Vol droit devant, air calme -----------------------------------------
    {
        Paraglider glider;
        glider.launch(glm::vec3(0.0f, 100.0f, 0.0f), 0.0f); // cap = +X
        check(glider.flying(), "launch() met le vol en cours");
        checkNear(glm::length(glider.forward()), 1.0f, 1e-4f,
                  "forward() est unitaire au decollage");

        constexpr float dt = 1.0f / 60.0f;
        for (int i = 0; i < 120; ++i) {
            glider.update(dt, /*turnInput=*/0.0f, /*brakeInput=*/0.0f);
        }
        // 2 s en ligne droite, cap +X : Z ne doit pas avoir bouge, Y doit avoir
        // baisse d'a peu pres cruiseSinkRate * 2 s des que la reponse
        // exponentielle du taux de chute (partant deja au regime de croisiere,
        // donc sans transitoire ici) s'est stabilisee.
        checkNear(glider.position().z, 0.0f, 1e-3f,
                  "Vol tout droit (cap +X) : aucune derive laterale");
        check(glider.position().x > 0.0f, "Vol tout droit : avance bien vers +X");
        checkNear(glider.position().y, 100.0f - ParagliderParams{}.cruiseSinkRate * 2.0f,
                  0.05f, "Taux de chute de croisiere respecte sur 2 s");
    }

    // --- Virage : rayon fini, pas une rotation instantanee --------------------
    {
        Paraglider glider;
        glider.launch(glm::vec3(0.0f, 200.0f, 0.0f), 0.0f);

        constexpr float dt = 1.0f / 60.0f;
        for (int i = 0; i < 60; ++i) {
            glider.update(dt, /*turnInput=*/1.0f, /*brakeInput=*/0.0f);
        }
        // Une seconde de virage plein a droite : le cap a change d'exactement
        // turnRateDegPerSec degres (integration a vitesse angulaire constante),
        // ni plus (pas de rotation instantanee sur la premiere frame) ni moins.
        checkNear(glider.headingDegrees(), ParagliderParams{}.turnRateDegPerSec, 0.5f,
                  "Le cap tourne a vitesse angulaire constante, pas instantanement");
        check(glider.position().z > 0.1f,
              "Le virage courbe bien la trajectoire (Z s'est ecarte de 0)");
    }

    // --- Frein : vitesse plus faible, taux de chute plus fort, progressivement -
    {
        Paraglider glider;
        glider.launch(glm::vec3(0.0f, 200.0f, 0.0f), 0.0f);

        const float yAfterOneFrameNoBrake = [&] {
            Paraglider probe;
            probe.launch(glm::vec3(0.0f, 200.0f, 0.0f), 0.0f);
            probe.update(1.0f / 60.0f, 0.0f, 0.0f);
            return probe.position().y;
        }();

        glider.update(1.0f / 60.0f, 0.0f, /*brakeInput=*/1.0f);
        check(glider.position().y < yAfterOneFrameNoBrake,
              "Freiner degrade le taux de chute des la premiere frame (mais peu : "
              "reponse progressive, pas un saut)");

        constexpr float dt = 1.0f / 60.0f;
        for (int i = 0; i < 180; ++i) {
            glider.update(dt, 0.0f, 1.0f);
        }
        // Apres 3 s de frein a fond, la vitesse doit s'etre stabilisee pres de
        // brakeSpeed : on le verifie indirectement via la distance parcourue au
        // pas suivant plutot que d'exposer un accesseur de vitesse rien que pour
        // le test.
        const glm::vec3 beforeStep = glider.position();
        glider.update(dt, 0.0f, 1.0f);
        const float stepDistance = horizontalDistance(glider.position(), beforeStep);
        checkNear(stepDistance, ParagliderParams{}.brakeSpeed * dt, 0.02f,
                  "Plein frein maintenu : la vitesse s'est stabilisee pres de brakeSpeed");
    }

    // --- Atterrissage : le vol s'arrete pile au niveau du terrain ------------
    {
        FlatTerrain terrain(50.0f);
        constexpr float kEyeHeight = 1.7f;

        Paraglider glider;
        glider.launch(glm::vec3(0.0f, 52.0f, 0.0f), 0.0f);
        check(!glider.checkLanding(terrain, kEyeHeight),
              "checkLanding() ne declenche rien tant qu'on est au-dessus du sol");

        bool landed = false;
        for (int i = 0; i < 6000 && !landed; ++i) {
            glider.update(1.0f / 60.0f, 0.0f, 0.0f);
            landed = glider.checkLanding(terrain, kEyeHeight);
        }
        check(landed, "Le vol finit par atterrir sur un terrain plat");
        check(!glider.flying(), "flying() redevient faux apres l'atterrissage");
        checkNear(glider.position().y, terrain.heightAt(0.0f, 0.0f) + kEyeHeight, 1e-3f,
                  "Position posee EXACTEMENT au niveau du terrain (hauteur d'oeil incluse)");
    }

    // --- abort() : filet de securite, independant de l'altitude --------------
    {
        Paraglider glider;
        glider.launch(glm::vec3(0.0f, 500.0f, 0.0f), 0.0f);
        glider.abort();
        check(!glider.flying(), "abort() termine le vol immediatement, quelle que soit l'altitude");
    }

    // --- update()/checkLanding() sans vol en cours : aucun effet -------------
    {
        FlatTerrain terrain(10.0f);
        Paraglider glider;
        const glm::vec3 before = glider.position();
        glider.update(1.0f / 60.0f, 1.0f, 1.0f);
        check(glider.position() == before, "update() sans launch() ne deplace rien");
        check(!glider.checkLanding(terrain, 1.7f),
              "checkLanding() sans vol en cours ne fait rien");
    }
}



// ---------------------------------------------------------------------------
// Phase 13 : physique de base du velo
// ---------------------------------------------------------------------------
// Ce que ces tests verifient, et pourquoi ils sont ecrits ainsi : la valeur d'un
// modele simplifie ne tient pas dans ses formules mais dans les VITESSES qu'il
// produit. Chaque bloc ci-dessous mesure donc une vitesse d'equilibre reelle et
// la confronte a la fourchette annoncee dans world/Bike.hpp et
// NOTES-PHASE-VELO.md - au lieu de re-deriver l'equation, ce qui ne prouverait
// que l'accord du test avec lui-meme.
void testBike() {
    section("Phase 13 : physique de base du velo");

    const BikeParams defaults;

    // --- Contact permanent avec le sol ---------------------------------------
    {
        SlopedTerrain slope(0.0f, 0.10f); // 10 %, monte vers +X
        Bike bike;
        // On enfourche a une altitude VOLONTAIREMENT fausse : le velo doit se
        // recoller au terrain de lui-meme, des mount().
        bike.mount(glm::vec3(20.0f, 999.0f, 0.0f), 0.0f, slope);
        check(bike.riding(), "mount() met la sortie en cours");
        checkNear(bike.position().y, slope.heightAt(20.0f, 0.0f) + defaults.eyeHeight,
                  1e-3f, "mount() recolle le velo au terrain, quelle que soit "
                         "l'altitude demandee");
        checkNear(bike.speed(), 0.0f, 1e-6f, "On part a l'arret, pas lance");

        // Cap +X = plein dans la montee : la pente lue doit etre celle du plan.
        checkNear(bike.slopeAlongHeading(slope), 0.10f, 1e-4f,
                  "La pente le long du cap est bien celle du terrain (montee)");
        // Demi-tour : la meme pente, en descente.
        bike.mount(glm::vec3(20.0f, 0.0f, 0.0f), 180.0f, slope);
        checkNear(bike.slopeAlongHeading(slope), -0.10f, 1e-4f,
                  "Cap inverse : meme pente, signe oppose");
        // En travers : pente nulle le long du cap.
        bike.mount(glm::vec3(20.0f, 0.0f, 0.0f), 90.0f, slope);
        checkNear(bike.slopeAlongHeading(slope), 0.0f, 1e-4f,
                  "Cap en travers de la pente : pente longitudinale nulle");

        // Et le contact tient pendant qu'on roule : une seule assertion pour les
        // 300 pas, sans quoi un test isole pesserait 300 fois dans le compte.
        bike.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, slope);
        bool alwaysOnGround = true;
        for (int i = 0; i < 300; ++i) {
            bike.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, slope, nullptr);
            const float expected =
                slope.heightAt(bike.position().x, bike.position().z) + defaults.eyeHeight;
            if (std::abs(bike.position().y - expected) > 1e-3f) {
                alwaysOnGround = false;
            }
        }
        check(alwaysOnGround,
              "Le velo reste colle a la surface a chacun des 300 pas (jamais en l'air)");
    }

    // --- Inertie : ni acceleration ni freinage instantanes -------------------
    {
        FlatTerrain flat;
        Bike bike;
        bike.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);

        bike.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, flat, nullptr);
        const float afterOneFrame = bike.speed();
        check(afterOneFrame > 0.0f, "Pedaler fait avancer");
        // Une frame de pedalage a fond ne peut pas donner plus que
        // pedalAcceleration * dt, soit ~3,7 cm/s : c'est ca, l'inertie, et c'est
        // verifiable sans connaitre la vitesse finale.
        check(afterOneFrame < defaults.pedalAcceleration / 60.0f + 1e-4f,
              "Une frame de pedalage ne donne qu'une fraction de m/s (inertie)");

        // Vitesse de croisiere sur le plat, puis freinage.
        const float cruise = settleSpeed(bike, flat, 1.0f, 0.0f);
        bike.update(1.0f / 60.0f, 0.0f, 0.0f, 1.0f, flat, nullptr);
        check(bike.speed() > 0.0f,
              "Le frein a fond ne met pas a l'arret en une frame (inertie)");
        check(bike.speed() < cruise, "Le frein ralentit bien des la premiere frame");

        // Distance et duree d'arret depuis la croisiere.
        const glm::vec3 beforeStop = bike.position();
        float stopTime = 0.0f;
        for (int i = 0; i < 600 && bike.speed() > 0.0f; ++i) {
            bike.update(1.0f / 60.0f, 0.0f, 0.0f, 1.0f, flat, nullptr);
            stopTime += 1.0f / 60.0f;
        }
        checkNear(bike.speed(), 0.0f, 1e-6f,
                  "Le frein maintenu finit par arreter le velo");
        const float stopDistance = horizontalDistance(bike.position(), beforeStop);
        check(stopTime > 3.0f / 60.0f, "L'arret prend plusieurs frames, pas une seule");
        check(stopDistance > 0.3f && stopDistance < 3.0f,
              "Distance d'arret plausible depuis la croisiere (entre 0,3 et 3 m)");
    }

    // --- Vitesses d'equilibre : LE chiffre qui compte ------------------------
    {
        // Plat, pedalage a fond.
        FlatTerrain flat;
        Bike bike;
        bike.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        const float flatKmh = settleSpeed(bike, flat, 1.0f, 0.0f) * 3.6f;
        check(flatKmh > 12.0f && flatKmh < 22.0f,
              "Plat, pedalage a fond : 12 a 22 km/h (VTT sur chemin)");

        // Montee a 10 % : nettement plus lent, mais on monte.
        SlopedTerrain up10(0.0f, 0.10f);
        Bike climber;
        climber.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, up10);
        const float up10Kmh = settleSpeed(climber, up10, 1.0f, 0.0f) * 3.6f;
        check(up10Kmh > 4.0f && up10Kmh < 12.0f,
              "Montee a 10 %, pedalage a fond : 4 a 12 km/h");
        check(up10Kmh < flatKmh - 3.0f,
              "La montee ralentit franchement par rapport au plat");

        // Montee a 25 % : le velo cale. C'est VOULU (25 % est du portage), et
        // c'est aussi la garantie qu'il ne recule jamais.
        SlopedTerrain up25(0.0f, 0.25f);
        Bike staller;
        staller.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, up25);
        const float up25Speed = settleSpeed(staller, up25, 1.0f, 0.0f, 20.0f);
        checkNear(up25Speed, 0.0f, 1e-6f,
                  "Montee a 25 % : le velo cale (pas de portage simule) ...");
        check(staller.position().x >= -1e-3f,
              "... et il ne RECULE pas : la vitesse est ecretee a zero");

        // Descente a 20 %, en roue libre : la fourchette VTT demandee.
        SlopedTerrain down20(0.0f, -0.20f); // descend vers +X
        Bike descender;
        descender.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, down20);
        const float down20Kmh = settleSpeed(descender, down20, 0.0f, 0.0f) * 3.6f;
        check(down20Kmh > 30.0f && down20Kmh <= 50.0f,
              "Descente a 20 % en roue libre : 30 a 50 km/h (fourchette VTT visee)");

        // Descente a 30 % : plus vite, mais toujours sous le plafond.
        SlopedTerrain down30(0.0f, -0.30f);
        Bike descender30;
        descender30.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, down30);
        const float down30Kmh = settleSpeed(descender30, down30, 0.0f, 0.0f) * 3.6f;
        check(down30Kmh > down20Kmh,
              "Une descente plus raide donne une vitesse plus elevee");
        check(down30Kmh <= defaults.maxSpeed * 3.6f + 1e-3f,
              "Le plafond de 50 km/h n'est jamais depasse");

        // Falaise LiDAR : meme a 200 % de pente, la vitesse reste bornee. C'est
        // le cas que la zone de jeu contient reellement (parois verticales, cf.
        // NOTES-PHASE9.md), et celui ou une formule en tan() divergerait.
        SlopedTerrain cliff(0.0f, -2.0f);
        Bike faller;
        faller.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, cliff);
        const float cliffKmh = settleSpeed(faller, cliff, 1.0f, 0.0f, 60.0f) * 3.6f;
        check(cliffKmh <= defaults.maxSpeed * 3.6f + 1e-3f,
              "Pente de 200 % : la vitesse reste bornee par maxSpeed, sans divergence");
        check(!std::isnan(cliffKmh), "Pente extreme : aucun NaN");

        // Le frein bat la gravite : on doit pouvoir descendre au pas.
        Bike braker;
        braker.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, down30);
        const float brakedKmh = settleSpeed(braker, down30, 0.0f, 1.0f, 20.0f) * 3.6f;
        checkNear(brakedKmh, 0.0f, 1e-3f,
                  "Frein a fond en descente a 30 % : le velo s'arrete (le frein "
                  "l'emporte sur la gravite)");
    }

    // --- Roue libre sur pente tres faible : le velo ne part pas tout seul ----
    {
        SlopedTerrain gentle(0.0f, -0.02f); // 2 % de descente
        Bike bike;
        bike.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, gentle);
        const float speed = settleSpeed(bike, gentle, 0.0f, 0.0f, 20.0f);
        checkNear(speed, 0.0f, 1e-6f,
                  "Descente a 2 %, sans pedaler : la resistance au roulement "
                  "empeche le velo de partir seul");
    }

    // --- Direction : rayon de braquage fini, et croissant avec la vitesse ----
    {
        FlatTerrain flat;

        // Rayon mesure a deux vitesses differentes, par la vitesse angulaire
        // effectivement obtenue (R = v / omega).
        auto measureRadius = [&](float targetSpeed) {
            Bike bike;
            bike.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
            // On amene le velo a la vitesse voulue en pedalant en ligne droite,
            // puis on mesure UN pas de virage a fond.
            for (int i = 0; i < 4000 && bike.speed() < targetSpeed; ++i) {
                bike.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, flat, nullptr);
            }
            const float v = bike.speed();
            const float before = bike.headingDegrees();
            bike.update(1.0f / 60.0f, 1.0f, 0.0f, 0.0f, flat, nullptr);
            const float omega = glm::radians(bike.headingDegrees() - before) * 60.0f;
            return std::make_pair(v, v / omega);
        };

        const std::pair<float, float> slow = measureRadius(3.0f);
        const std::pair<float, float> fast = measureRadius(4.5f);
        check(slow.second > 0.0f && fast.second > 0.0f,
              "Le rayon de braquage est fini et positif : pas de rotation sur place "
              "en roulant");
        check(fast.second > slow.second + 0.5f,
              "Le rayon de braquage GRANDIT avec la vitesse (pas de virage a angle "
              "droit a pleine vitesse)");
        // R = v^2 / maxLateralAccel : c'est la formule annoncee, on la confronte
        // a la mesure.
        checkNear(fast.second, fast.first * fast.first / defaults.maxLateralAccel, 0.2f,
                  "Le rayon mesure vaut bien v^2 / maxLateralAccel");

        // A l'arret, le plafond de vitesse angulaire prend le relais : on peut
        // replacer la machine, mais pas plus vite que maxTurnRateDegPerSec.
        Bike still;
        still.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        const float beforeHeading = still.headingDegrees();
        still.update(1.0f / 60.0f, 1.0f, 0.0f, 0.0f, flat, nullptr);
        checkNear((still.headingDegrees() - beforeHeading) * 60.0f,
                  defaults.maxTurnRateDegPerSec, 1e-2f,
                  "A l'arret : rotation plafonnee a maxTurnRateDegPerSec, jamais "
                  "infinie");

        // Sens de rotation : +1 tourne a droite (vers +Z depuis le cap +X, meme
        // convention que Camera et Paraglider).
        //
        // On lance d'abord le velo EN LIGNE DROITE avant de tourner. Tourner des
        // l'arret ne prouverait rien : a vitesse nulle le braquage est plafonne a
        // 140 deg/s, donc deux secondes de virage font trois quarts de tour et la
        // trajectoire revient sur elle-meme - le signe de z ne dirait plus rien
        // du sens de rotation.
        Bike turner;
        turner.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        for (int i = 0; i < 600; ++i) {
            turner.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, flat, nullptr);
        }
        const float zBeforeTurn = turner.position().z;
        const float headingBeforeTurn = turner.headingDegrees();
        for (int i = 0; i < 30; ++i) { // une demi-seconde de virage a droite
            turner.update(1.0f / 60.0f, 1.0f, 1.0f, 0.0f, flat, nullptr);
        }
        check(turner.headingDegrees() > headingBeforeTurn,
              "steerInput > 0 fait croitre le cap");
        check(turner.position().z > zBeforeTurn,
              "steerInput > 0 courbe la trajectoire vers +Z (a droite du cap +X)");

        // Et le miroir, pour ecarter un test qui passerait avec n'importe quel
        // signe.
        Bike turnerLeft;
        turnerLeft.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        for (int i = 0; i < 600; ++i) {
            turnerLeft.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, flat, nullptr);
        }
        const float zBeforeLeft = turnerLeft.position().z;
        for (int i = 0; i < 30; ++i) {
            turnerLeft.update(1.0f / 60.0f, -1.0f, 1.0f, 0.0f, flat, nullptr);
        }
        check(turnerLeft.position().z < zBeforeLeft,
              "steerInput < 0 courbe la trajectoire dans l'autre sens");
    }

    // --- Assiette visuelle : suit la pente locale ----------------------------
    {
        SlopedTerrain up20(0.0f, 0.20f);
        Bike bike;
        bike.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, up20);
        bike.update(1.0f / 60.0f, 0.0f, 0.0f, 0.0f, up20, nullptr);
        checkNear(bike.pitchDegrees(), glm::degrees(std::atan(0.20f)), 0.1f,
                  "Tangage : le nez se leve de l'angle de la pente en montee");
        checkNear(bike.rollDegrees(), 0.0f, 0.1f,
                  "Roulis nul quand on monte droit dans la pente");

        // Meme plan, cap en travers : plus de tangage, mais du devers.
        Bike crossing;
        crossing.mount(glm::vec3(0.0f, 0.0f, 0.0f), 90.0f, up20); // cap +Z
        crossing.update(1.0f / 60.0f, 0.0f, 0.0f, 0.0f, up20, nullptr);
        checkNear(crossing.pitchDegrees(), 0.0f, 0.1f,
                  "Cap en travers : plus de tangage");
        check(std::abs(crossing.rollDegrees()) > 5.0f,
              "Cap en travers : le velo epouse le devers (roulis non nul)");

        // Descente : le nez pique.
        SlopedTerrain down20(0.0f, -0.20f);
        Bike descender;
        descender.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, down20);
        descender.update(1.0f / 60.0f, 0.0f, 0.0f, 0.0f, down20, nullptr);
        check(descender.pitchDegrees() < -5.0f, "Tangage negatif en descente");

        // Inclinaison en virage : a vitesse non nulle, tourner penche la machine
        // vers l'interieur, en plus du devers.
        FlatTerrain flat;
        Bike leaner;
        leaner.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        for (int i = 0; i < 600; ++i) {
            leaner.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, flat, nullptr);
        }
        leaner.update(1.0f / 60.0f, 1.0f, 1.0f, 0.0f, flat, nullptr);
        check(leaner.rollDegrees() > 2.0f,
              "Virage a droite sur le plat : le velo s'incline a droite");
        leaner.update(1.0f / 60.0f, -1.0f, 1.0f, 0.0f, flat, nullptr);
        check(leaner.rollDegrees() < -2.0f, "Virage a gauche : inclinaison opposee");

        // L'ecretage tient sur une paroi.
        SlopedTerrain wall(0.0f, 3.0f);
        Bike onWall;
        onWall.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, wall);
        onWall.update(1.0f / 60.0f, 0.0f, 0.0f, 0.0f, wall, nullptr);
        check(std::abs(onWall.pitchDegrees()) <= defaults.maxVisualTiltDegrees + 1e-3f,
              "L'assiette visuelle est ecretee sur une paroi (pas de marqueur a plat)");
    }

    // --- Obstacles : LE MEME systeme de collision que le joueur a pied -------
    {
        FlatTerrain flat;
        Ground ground(flat);
        ForestParams noAutoSeed;
        noAutoSeed.treeCount = 0;
        Forest forest(flat, ground, 50.0f, noAutoSeed);
        forest.plantTree(10.0f, 0.0f, 7.0f); // droit devant un velo cap +X

        Bike bike;
        bike.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);

        // On roule droit sur le tronc pendant largement de quoi le depasser.
        for (int i = 0; i < 1200; ++i) {
            bike.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, flat, &forest);
        }

        const float trunkRadius = forest.trees()[0].trunkRadius();
        const float contact = trunkRadius + BikeParams{}.bodyRadius;
        check(bike.position().x < 10.0f,
              "Le velo n'a PAS traverse le tronc (il est reste en deca)");
        check(horizontalDistance(bike.position(), forest.trees()[0].base) >=
                  contact - 1e-2f,
              "Le velo est arrete au contact du tronc, pas dedans");
        check(bike.speed() < 1.0f,
              "Le choc coute sa vitesse au velo (il ne pousse pas l'arbre a 16 km/h)");

        // Temoin : sans solveur, le meme trajet TRAVERSE. Sans ce controle, un
        // test qui bloque parce que le velo n'avance pas du tout passerait aussi.
        Bike ghost;
        ghost.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        for (int i = 0; i < 1200; ++i) {
            ghost.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, flat, nullptr);
        }
        check(ghost.position().x > 12.0f,
              "TEMOIN : sans solveur de collision, le meme trajet depasse l'arbre "
              "(donc le blocage ci-dessus vient bien de la collision)");

        // Une souche basse arrete aussi le velo : c'est la bande verticale du
        // BodyCylinder qui le garantit, exactement comme pour le joueur a pied.
        Forest stumpOnly(flat, ground, 50.0f, noAutoSeed);
        stumpOnly.plantTree(10.0f, 0.0f, 7.0f);
        stumpOnly.fell(0);
        runUntilFallen(stumpOnly, 12.0f);
        check(stumpOnly.trees()[0].state == TreeState::Fallen,
              "L'arbre du test de souche est bien entierement debite");

        Bike overStump;
        overStump.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        for (int i = 0; i < 1200; ++i) {
            overStump.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, flat, &stumpOnly);
        }
        check(overStump.position().x < 10.0f,
              "Une souche basse arrete le velo comme elle arrete les jambes");
    }

    // --- Sans mount() : aucun effet -----------------------------------------
    {
        FlatTerrain flat;
        Bike bike;
        const glm::vec3 before = bike.position();
        bike.update(1.0f / 60.0f, 1.0f, 1.0f, 1.0f, flat, nullptr);
        check(bike.position() == before, "update() sans mount() ne deplace rien");
        check(!bike.riding(), "Un velo neuf n'est pas en cours d'utilisation");

        bike.mount(glm::vec3(5.0f, 0.0f, 5.0f), 0.0f, flat);
        bike.dismount();
        check(!bike.riding(), "dismount() termine la sortie");
        const glm::vec3 parked = bike.position();
        bike.update(1.0f / 60.0f, 1.0f, 1.0f, 0.0f, flat, nullptr);
        check(bike.position() == parked,
              "Apres dismount(), plus aucun pas n'est simule");

        // deltaTime nul : pas d'integration, pas de NaN.
        bike.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        const glm::vec3 atMount = bike.position();
        bike.update(0.0f, 1.0f, 1.0f, 0.0f, flat, nullptr);
        check(bike.position() == atMount, "deltaTime nul : aucun pas simule");
    }

    // --- Ecretage des entrees : des valeurs brutes hors bornes sont sures ----
    {
        FlatTerrain flat;
        Bike wild;
        wild.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        Bike tame;
        tame.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        for (int i = 0; i < 300; ++i) {
            wild.update(1.0f / 60.0f, 50.0f, 50.0f, 0.0f, flat, nullptr);
            tame.update(1.0f / 60.0f, 1.0f, 1.0f, 0.0f, flat, nullptr);
        }
        checkNear(wild.speed(), tame.speed(), 1e-5f,
                  "Les entrees sont ecretees : 50 se comporte comme 1");
        checkNear(wild.headingDegrees(), tame.headingDegrees(), 1e-3f,
                  "Ecretage aussi sur la direction");
    }
}

// ---------------------------------------------------------------------------
// Phase 14 : physique de base de l'escalade
// ---------------------------------------------------------------------------
// Deux terrains de test que les precedents ne fournissaient pas. SlopedTerrain
// est un plan infini : parfait pour mesurer une vitesse ou un cout par metre sur
// une pente EXACTE, mais il n'a ni pied ni sommet, donc il ne peut prouver
// aucune des transitions de mode. D'ou les deux ci-dessous.

// Falaise minimale : plat en bas, paroi, plat en haut. C'est le plus petit
// terrain qui contienne les trois situations de l'escalade (s'engager, monter,
// topper), et il est PARFAITEMENT previsible - contrairement a du relief LiDAR,
// ou un test qui echoue laisse toujours le doute entre le modele et la donnee.
class CliffTerrain : public HeightSampler {
public:
    CliffTerrain(float baseHeight, float slope, float footX, float topX)
        : base_(baseHeight), slope_(slope), footX_(footX), topX_(topX) {}

    float heightAt(float x, float) const override {
        if (x <= footX_) return base_;
        if (x >= topX_) return base_ + slope_ * (topX_ - footX_);
        return base_ + slope_ * (x - footX_);
    }

private:
    float base_, slope_, footX_, topX_;
};

// Meme falaise, mais avec une VIRE horizontale a mi-hauteur, de largeur
// reglable. C'est le terrain qui met a l'epreuve la confirmation du sommet :
// une vire d'un metre ne doit pas terminer l'escalade (on serait debout en plein
// vide), une vire de six metres si (on y tient reellement debout).
class LedgedCliffTerrain : public HeightSampler {
public:
    LedgedCliffTerrain(float slope, float topX, float ledgeWidth)
        : slope_(slope), topX_(topX), ledgeWidth_(ledgeWidth) {
        ledgeStart_ = 0.5f * topX_;
    }

    float heightAt(float x, float) const override {
        if (x <= 0.0f) return 0.0f;
        if (x <= ledgeStart_) return slope_ * x;
        const float ledgeY = slope_ * ledgeStart_;
        if (x <= ledgeStart_ + ledgeWidth_) return ledgeY;
        const float afterLedge = x - (ledgeStart_ + ledgeWidth_);
        const float wallLeft = topX_ - ledgeStart_;
        if (afterLedge >= wallLeft) return ledgeY + slope_ * wallLeft;
        return ledgeY + slope_ * afterLedge;
    }

private:
    float slope_, topX_, ledgeWidth_, ledgeStart_;
};

// Ce qui fait la valeur d'un modele simplifie n'est pas sa formule mais les
// GRANDEURS qu'il produit - meme intention que les tests du velo. Chaque bloc
// mesure donc un comportement reel (une vitesse d'ascension, une hauteur
// maximale, une position de repli) et le confronte a ce qu'annonce
// world/Climber.hpp, au lieu de re-deriver l'equation, ce qui ne prouverait que
// l'accord du test avec lui-meme.
void testClimber() {
    section("Phase 14 : physique de base de l'escalade");

    const ClimberParams defaults;
    constexpr float dt = 1.0f / 60.0f;

    // --- Detection de paroi --------------------------------------------------
    {
        Climber climber;
        FlatTerrain flat(100.0f);
        checkNear(climber.slopeAt(flat, 3.0f, -7.0f), 0.0f, 1e-6f,
                  "Terrain plat : pente nulle");
        check(!climber.isWall(flat, 3.0f, -7.0f),
              "Terrain plat : ce n'est pas une paroi");

        // Un versant raide MAIS marchable : 60 %, la valeur type de ce massif.
        // C'est le cas qui compte - si celui-la passait pour une paroi, le seuil
        // ne servirait a rien, la moitie du terrain etant au-dessus de 50 %.
        SlopedTerrain steep(0.0f, 0.60f);
        checkNear(climber.slopeAt(steep, 12.0f, 5.0f), 0.60f, 1e-4f,
                  "Versant a 60 % : la pente lue est exacte");
        check(!climber.isWall(steep, 12.0f, 5.0f),
              "Versant a 60 % : ce n'est PAS une paroi (sous le seuil de paroi)");

        SlopedTerrain wall(0.0f, 1.05f);
        check(climber.isWall(wall, 12.0f, 5.0f), "Pente a 105 % : c'est une paroi");

        SlopedTerrain exactly(0.0f, defaults.wallSlope);
        check(climber.isWall(exactly, 0.0f, 0.0f),
              "Pente exactement au seuil : c'est une paroi (comparaison >=)");

        // La pente est une MAGNITUDE : une paroi qui descend vers +X en est une
        // aussi. Un signe oublie se verrait ici et nulle part ailleurs.
        SlopedTerrain descending(0.0f, -1.50f);
        check(climber.isWall(descending, 0.0f, 0.0f),
              "Paroi de pente negative : c'est une paroi (magnitude, pas signe)");

        // Ce n'est pas un detail de reglage : si walkableSlope rattrapait
        // wallSlope, l'hysteresis disparaitrait et le mode escalade se mettrait
        // a clignoter d'une frame a l'autre sur une paroi reelle.
        check(defaults.walkableSlope < defaults.wallSlope,
              "Hysteresis : le seuil de sortie est STRICTEMENT sous celui d'entree");
    }

    // --- Engagement : tryStart ------------------------------------------------
    {
        CliffTerrain cliff(0.0f, 3.0f, 0.0f, 10.0f);

        {
            Climber climber;
            const glm::vec3 eye(-1.5f, 1.7f, 0.0f);
            check(climber.tryStart(cliff, eye, glm::vec3(1.0f, 0.0f, 0.0f)),
                  "Engagement : une paroi a portee, on s'accroche");
            check(climber.climbing(), "Engagement : l'escalade est en cours");
            checkNear(climber.anchor().y,
                      cliff.heightAt(climber.anchor().x, climber.anchor().z), 1e-4f,
                      "Engagement : le point d'accroche est SUR la surface");
            check(climber.isWall(cliff, climber.anchor().x, climber.anchor().z),
                  "Engagement : le point d'accroche est bien sur une paroi");
        }
        {
            Climber climber;
            check(!climber.tryStart(cliff, glm::vec3(-1.5f, 1.7f, 0.0f),
                                    glm::vec3(-1.0f, 0.0f, 0.0f)),
                  "Engagement refuse : la paroi est dans le dos");
            check(!climber.climbing(), "Engagement refuse : rien n'a change");
        }
        {
            Climber climber;
            check(!climber.tryStart(cliff, glm::vec3(-5.0f, 1.7f, 0.0f),
                                    glm::vec3(1.0f, 0.0f, 0.0f)),
                  "Engagement refuse : la paroi est hors de portee");
        }
        {
            Climber climber;
            FlatTerrain flat;
            check(!climber.tryStart(flat, glm::vec3(0.0f, 1.7f, 0.0f),
                                    glm::vec3(1.0f, 0.0f, 0.0f)),
                  "Engagement refuse : il n'y a pas de paroi du tout");
        }
        {
            // Regard strictement vertical : aucune direction au sol, donc aucune
            // cible. Le cas degenere doit REFUSER, pas normaliser un vecteur nul.
            Climber climber;
            check(!climber.tryStart(cliff, glm::vec3(-1.5f, 1.7f, 0.0f),
                                    glm::vec3(0.0f, 1.0f, 0.0f)),
                  "Engagement refuse : regard strictement vertical (cas degenere)");
        }
    }

    // --- Contact permanent avec la surface ------------------------------------
    {
        // Le meme invariant que le velo, et pour la meme raison : la verticale
        // n'est jamais une variable d'etat, elle est relue de heightAt().
        SlopedTerrain wall(0.0f, 3.0f);
        Climber climber;
        check(climber.tryStart(wall, glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(1.0f, 0.0f, 0.0f)),
              "Contact : engagement sur une paroi uniforme a 300 %");

        bool alwaysOnSurface = true;
        bool everNaN = false;
        for (int i = 0; i < 400; ++i) {
            climber.update(dt, 1.0f, 0.3f, wall, nullptr);
            const float expected = wall.heightAt(climber.anchor().x, climber.anchor().z);
            if (std::abs(climber.anchor().y - expected) > 1e-3f) {
                alwaysOnSurface = false;
            }
            if (std::isnan(climber.anchor().x) || std::isnan(climber.anchor().y) ||
                std::isnan(climber.anchor().z) || std::isnan(climber.stamina())) {
                everNaN = true;
            }
        }
        check(alwaysOnSurface, "Le grimpeur reste sur la surface a chacun des 400 pas");
        check(!everNaN, "Escalade : aucun NaN sur 400 pas");
    }

    // --- Vitesse le long de la surface ----------------------------------------
    {
        // Ce que le modele promet : on avance a climbSpeed LE LONG DE LA
        // SURFACE. La montee verticale qui en resulte vaut donc climbSpeed x
        // sin(theta), pas climbSpeed. Le verifier sur une pente connue est la
        // seule facon de prouver que la projection est reellement faite.
        const float slope = 3.0f; // 300 %, 71,6 degres
        SlopedTerrain wall(0.0f, slope);
        Climber climber;
        climber.tryStart(wall, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

        const glm::vec3 before = climber.anchor();
        const int steps = 60; // une seconde
        for (int i = 0; i < steps; ++i) {
            climber.update(dt, 1.0f, 0.0f, wall, nullptr);
        }
        const glm::vec3 after = climber.anchor();
        const float dx = after.x - before.x;
        const float dz = after.z - before.z;
        const float dy = after.y - before.y;
        const float alongSurface = std::sqrt(dx * dx + dz * dz + dy * dy);
        const float elapsed = steps * dt;

        checkNear(alongSurface / elapsed, defaults.climbSpeed, 1e-2f,
                  "Vitesse le long de la SURFACE : c'est bien climbSpeed");
        checkNear(dy / elapsed,
                  defaults.climbSpeed * slope / std::sqrt(1.0f + slope * slope), 1e-2f,
                  "Vitesse VERTICALE = climbSpeed x sin(pente)");
        check(dy / elapsed < defaults.climbSpeed,
              "La montee verticale est plus lente que la progression sur la paroi");
        check(defaults.climbSpeed < 14.0f / 10.0f,
              "Grimper est au moins dix fois plus lent que marcher (14 m/s)");
    }

    // --- Une paroi quasi verticale se monte quasi verticalement ---------------
    {
        // Aucun cas particulier n'est ecrit pour ca dans Climber : c'est la
        // projection 1/sqrt(1+pente^2) qui le produit toute seule. Ce test fige
        // ce comportement emergent, qui est le coeur du modele.
        SlopedTerrain sheer(0.0f, 20.0f); // 2000 %, 87,1 degres
        Climber climber;
        climber.tryStart(sheer, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

        const glm::vec3 before = climber.anchor();
        for (int i = 0; i < 60; ++i) {
            climber.update(dt, 1.0f, 0.0f, sheer, nullptr);
        }
        const glm::vec3 after = climber.anchor();
        const float dHoriz = horizontalDistance(after, before);
        const float dAlt = after.y - before.y;
        check(dAlt > 0.99f * defaults.climbSpeed,
              "Paroi a 87 degres : la montee est quasi integralement verticale");
        check(dHoriz < 0.06f * defaults.climbSpeed,
              "Paroi a 87 degres : l'avance horizontale est negligeable");
    }

    // --- Traversee : le long de la LIGNE DE NIVEAU ----------------------------
    {
        // Sur un terrain dont l'altitude ne depend que de x, la ligne de niveau
        // est l'axe z. Traverser doit donc deplacer en z et laisser l'altitude ou
        // elle est - c'est ce qui distingue une traversee d'une diagonale.
        SlopedTerrain wall(0.0f, 2.0f);
        Climber climber;
        climber.tryStart(wall, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

        const glm::vec3 before = climber.anchor();
        for (int i = 0; i < 60; ++i) {
            climber.update(dt, 0.0f, 1.0f, wall, nullptr);
        }
        const glm::vec3 after = climber.anchor();
        checkNear(after.y, before.y, 1e-3f,
                  "Traversee : l'altitude ne change pas (ligne de niveau)");
        checkNear(std::abs(after.z - before.z), defaults.traverseSpeed, 1e-2f,
                  "Traversee : on parcourt traverseSpeed en une seconde");
        checkNear(after.x, before.x, 1e-3f,
                  "Traversee : aucune avance dans la ligne de plus grande pente");

        const float rightward = after.z - before.z;
        Climber other;
        other.tryStart(wall, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 b2 = other.anchor();
        for (int i = 0; i < 60; ++i) {
            other.update(dt, 0.0f, -1.0f, wall, nullptr);
        }
        checkNear(other.anchor().z - b2.z, -rightward, 1e-3f,
                  "Traversee : gauche et droite sont exactement symetriques");
    }

    // --- L'oeil est HORS du relief --------------------------------------------
    {
        // Une camera a la premiere personne collee a une falaise est le piege
        // evident de cette phase : un oeil sous la surface montre l'interieur du
        // terrain. Le decalage vers l'AVAL doit le rendre impossible, sur toute
        // une ascension et pas seulement au depart.
        SlopedTerrain wall(0.0f, 4.0f);
        Climber climber;
        climber.tryStart(wall, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

        bool alwaysOutside = true;
        for (int i = 0; i < 200; ++i) {
            climber.update(dt, 1.0f, 0.0f, wall, nullptr);
            const glm::vec3 eye = climber.position();
            if (eye.y <= wall.heightAt(eye.x, eye.z)) {
                alwaysOutside = false;
            }
        }
        check(alwaysOutside,
              "L'oeil du grimpeur reste au-dessus de la surface sur 200 pas");
        check(climber.position().y > climber.anchor().y,
              "L'oeil est au-dessus du point d'accroche (eyeRise)");
        check(horizontalDistance(climber.position(), climber.anchor()) > 0.5f,
              "L'oeil est ecarte de la paroi (wallOffset)");
    }

    // --- Energie : les couts sont PAR METRE, pas par seconde ------------------
    {
        SlopedTerrain sheer(0.0f, 20.0f);
        Climber climber;
        climber.tryStart(sheer, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        checkNear(climber.stamina(), 1.0f, 1e-6f,
                  "Energie : on part avec la jauge pleine");

        const float startY = climber.anchor().y;
        for (int i = 0; i < 300; ++i) {
            climber.update(dt, 1.0f, 0.0f, sheer, nullptr);
        }
        const float climbed = climber.anchor().y - startY;
        const float spent = 1.0f - climber.stamina();
        check(climbed > 4.0f, "Energie : l'ascension de reference a bien eu lieu");
        // Sur une paroi a 2000 % la composante horizontale ne vaut que 5 % : le
        // cout par metre monte doit donc etre celui annonce, a 2 % pres.
        checkNear(spent / climbed, defaults.staminaPerMeterUp,
                  0.02f * defaults.staminaPerMeterUp,
                  "Energie : le cout par metre monte est bien staminaPerMeterUp");

        // Le meme trajet en DEUX FOIS PLUS DE TEMPS coute pareil : c'est la
        // consequence d'un cout par metre, et ca rend la jauge independante du
        // nombre d'images par seconde.
        Climber slow;
        slow.tryStart(sheer, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        const float slowStart = slow.anchor().y;
        for (int i = 0; i < 600; ++i) {
            slow.update(dt, 0.5f, 0.0f, sheer, nullptr);
        }
        checkNear(slow.anchor().y - slowStart, climbed, 1e-2f,
                  "Meme ascension, moitie moins vite, deux fois plus longtemps");
        checkNear(1.0f - slow.stamina(), spent, 0.02f * spent,
                  "Energie : monter lentement coute la MEME chose (cout par metre)");
    }

    // --- Energie : suspendu, desescalader -------------------------------------
    {
        SlopedTerrain sheer(0.0f, 20.0f);

        Climber hanging;
        hanging.tryStart(sheer, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        for (int i = 0; i < 300; ++i) { // 5 secondes
            hanging.update(dt, 0.0f, 0.0f, sheer, nullptr);
        }
        checkNear(1.0f - hanging.stamina(), defaults.staminaHangPerSecond * 5.0f, 1e-4f,
                  "Energie : suspendu immobile, cout par SECONDE");
        check(hanging.stamina() > 0.97f,
              "Energie : rester suspendu 5 s ne vide pas la jauge");

        // Descendre coute moins cher que monter - le choix de JEU qui garde la
        // desescalade comme porte de sortie.
        Climber up;
        up.tryStart(sheer, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        for (int i = 0; i < 120; ++i) up.update(dt, 1.0f, 0.0f, sheer, nullptr);
        const float upCost = 1.0f - up.stamina();

        Climber down;
        down.tryStart(sheer, glm::vec3(50.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        const float downStart = down.anchor().y;
        for (int i = 0; i < 120; ++i) down.update(dt, -1.0f, 0.0f, sheer, nullptr);
        const float downCost = 1.0f - down.stamina();
        check(down.anchor().y < downStart, "Desescalade : on descend bien");
        check(downCost < upCost, "Energie : desescalader coute moins cher que monter");
        checkNear(downCost / upCost,
                  defaults.staminaPerMeterDown / defaults.staminaPerMeterUp, 0.05f,
                  "Energie : le rapport descente/montee est celui annonce");
    }

    // --- Recuperation : au sol seulement --------------------------------------
    {
        SlopedTerrain sheer(0.0f, 20.0f);
        Climber climber;
        climber.tryStart(sheer, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        for (int i = 0; i < 600; ++i) climber.update(dt, 1.0f, 0.0f, sheer, nullptr);
        const float inWall = climber.stamina();
        check(inWall < 1.0f, "Recuperation : la jauge a bien baisse");

        // En paroi, restOnGround ne doit RIEN faire - sans quoi l'appelant, qui
        // l'appelle a chaque frame, rechargerait le grimpeur pendant sa montee.
        climber.restOnGround(1.0f);
        checkNear(climber.stamina(), inWall, 1e-6f,
                  "Recuperation : aucune recuperation en paroi");

        climber.release();
        climber.restOnGround(2.0f);
        checkNear(climber.stamina(), inWall + 2.0f * defaults.staminaRecoveryPerSecond,
                  1e-4f, "Recuperation : au sol, staminaRecoveryPerSecond par seconde");

        climber.restOnGround(1000.0f);
        checkNear(climber.stamina(), 1.0f, 1e-6f,
                  "Recuperation : la jauge ne depasse jamais 1");
    }

    // --- LA hauteur qui compte : ce qu'une jauge pleine permet ----------------
    {
        // Le chiffre annonce dans world/Climber.hpp (62,5 m de paroi strictement
        // verticale) est une limite theorique. Sur une paroi a 87 degres - la
        // plus raide qu'on trouve reellement dans la zone de jeu - la composante
        // horizontale en retire un peu. C'est CE chiffre-la qu'il faut mesurer,
        // parce que c'est lui qui decide quelles parois du massif sont faisables.
        SlopedTerrain sheer(0.0f, 20.0f);
        Climber climber;
        climber.tryStart(sheer, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

        float highest = climber.anchor().y;
        ClimbExit exit = ClimbExit::None;
        int steps = 0;
        while (exit == ClimbExit::None && steps < 200000) {
            exit = climber.update(dt, 1.0f, 0.0f, sheer, nullptr);
            if (exit == ClimbExit::None) {
                highest = climber.anchor().y;
            }
            ++steps;
        }
        check(exit == ClimbExit::Exhausted,
              "Jauge pleine : sur une paroi sans sommet, on finit par tomber");
        std::printf("  [mesure] jauge pleine sur une paroi a 87 deg : %.1f m atteints\n",
                    static_cast<double>(highest));
        check(highest > 58.0f && highest < 63.0f,
              "Jauge pleine : entre 58 et 63 m de paroi a 87 degres");

        // Corollaire qui donne son sens a l'activite : les grandes falaises de
        // ce massif (250 a 276 m, mesurees par tools/find_climb_walls.py) sont
        // HORS DE PORTEE d'une seule jauge. C'est voulu, pas un reglage rate.
        check(highest < 250.0f,
              "Jauge pleine : les falaises de 250 m du massif restent hors de portee");
    }

    // --- Les trois sorties ----------------------------------------------------
    {
        CliffTerrain cliff(0.0f, 3.0f, 0.0f, 10.0f); // 30 m de paroi a 300 %

        // 1. SOMMET.
        {
            Climber climber;
            check(climber.tryStart(cliff, glm::vec3(-1.0f, 0.0f, 0.0f),
                                   glm::vec3(1.0f, 0.0f, 0.0f)),
                  "Sortie sommet : engagement");
            ClimbExit exit = ClimbExit::None;
            int steps = 0;
            while (exit == ClimbExit::None && steps < 200000) {
                exit = climber.update(dt, 1.0f, 0.0f, cliff, nullptr);
                ++steps;
            }
            check(exit == ClimbExit::ToppedOut,
                  "Sortie sommet : une paroi de 30 m se termine par un SOMMET");
            check(!climber.climbing(), "Sortie sommet : l'escalade est terminee");
            // Au sommet on reste OU L'ON EST : c'est la seule des trois sorties
            // qui ne ramene pas au pied, et c'est toute la recompense.
            check(climber.anchor().x > 9.0f,
                  "Sortie sommet : on reste sur le plateau, pas au pied");
            check(climber.heightGained() > 28.0f,
                  "Sortie sommet : la trentaine de metres a bien ete gagnee");
            check(climber.stamina() > 0.0f, "Sortie sommet : il restait de l'energie");
            checkNear(climber.anchor().y,
                      cliff.heightAt(climber.anchor().x, climber.anchor().z), 1e-3f,
                      "Sortie sommet : la position finale est sur la surface");
        }

        // 2. LACHAGE VOLONTAIRE.
        {
            Climber climber;
            climber.tryStart(cliff, glm::vec3(-1.0f, 0.0f, 0.0f),
                             glm::vec3(1.0f, 0.0f, 0.0f));
            const glm::vec3 foot = climber.groundAnchor();
            for (int i = 0; i < 300; ++i) climber.update(dt, 1.0f, 0.0f, cliff, nullptr);
            check(climber.heightGained() > 3.0f, "Lachage : on etait bien monte");

            const ClimbExit exit = climber.release();
            check(exit == ClimbExit::Released, "Lachage : la sortie est Released");
            check(!climber.climbing(), "Lachage : l'escalade est terminee");
            check(climber.anchor() == foot,
                  "Lachage : retour EXACT au point d'engagement");
            checkNear(climber.heightGained(), 0.0f, 1e-6f,
                      "Lachage : le gain d'altitude est annule");
            check(climber.release() == ClimbExit::None,
                  "Lachage : lacher deux fois ne fait rien la seconde fois");
        }

        // 3. EPUISEMENT.
        {
            Climber climber;
            climber.tryStart(cliff, glm::vec3(-1.0f, 0.0f, 0.0f),
                             glm::vec3(1.0f, 0.0f, 0.0f));
            const glm::vec3 foot = climber.groundAnchor();

            ClimbExit exit = ClimbExit::None;
            int steps = 0;
            // On monte puis on redescend en boucle : l'altitude ne progresse pas,
            // mais l'energie, elle, se consomme. C'est le scenario "je tourne en
            // rond sur la paroi", celui ou la chute doit finir par arriver.
            while (exit == ClimbExit::None && steps < 400000) {
                const float input = ((steps / 120) % 2 == 0) ? 1.0f : -1.0f;
                exit = climber.update(dt, input, 0.0f, cliff, nullptr);
                ++steps;
            }
            check(exit == ClimbExit::Exhausted,
                  "Epuisement : tourner en rond sur la paroi finit par une CHUTE");
            check(!climber.climbing(), "Epuisement : l'escalade est terminee");
            checkNear(climber.stamina(), 0.0f, 1e-6f,
                      "Epuisement : la jauge est bien a zero");
            check(climber.anchor() == foot,
                  "Epuisement : reinitialisation EXACTE au point d'engagement");
            check(!std::isnan(climber.anchor().x) && !std::isnan(climber.anchor().y),
                  "Epuisement : la position de repli est valide");
        }
    }

    // --- Le sommet l'emporte sur l'epuisement ---------------------------------
    {
        // L'ordre des deux tests de sortie dans update() est un choix, pas un
        // hasard : poser la main sur la vire avec sa derniere goutte d'energie,
        // c'est topper, pas tomber. Le figer ici evite qu'un remaniement
        // n'inverse les deux sans que personne ne s'en apercoive.
        CliffTerrain cliff(0.0f, 3.0f, 0.0f, 3.0f); // paroi courte : 9 m
        Climber climber;
        climber.tryStart(cliff, glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

        ClimbExit exit = ClimbExit::None;
        int steps = 0;
        while (exit == ClimbExit::None && steps < 200000) {
            exit = climber.update(dt, 1.0f, 0.0f, cliff, nullptr);
            ++steps;
        }
        check(exit == ClimbExit::ToppedOut,
              "Paroi courte : on toppe, la jauge n'a pas le temps de se vider");
    }

    // --- Hysteresis : pas de sortie au milieu d'une paroi ---------------------
    {
        // Le piege de cette phase : une vire etroite au milieu de la face ne doit
        // PAS terminer l'escalade, sinon le joueur se retrouve debout en plein
        // vide. Climber confirme donc le terrain marchable 3 m plus loin.
        LedgedCliffTerrain narrow(3.0f, 15.0f, 1.0f);
        Climber climber;
        climber.tryStart(narrow, glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        ClimbExit exit = ClimbExit::None;
        int steps = 0;
        while (exit == ClimbExit::None && steps < 200000) {
            exit = climber.update(dt, 1.0f, 0.0f, narrow, nullptr);
            ++steps;
        }
        check(exit == ClimbExit::ToppedOut,
              "Vire etroite : l'escalade se poursuit jusqu'au VRAI sommet");
        check(climber.anchor().x > 15.0f,
              "Vire etroite de 1 m : on ne s'y arrete PAS (pas de sortie en plein vide)");

        // Vire de 6 m : assez profonde pour s'y tenir debout. S'y arreter est le
        // comportement CORRECT et non un pis-aller - 6 m de terrain marchable,
        // on y est reellement.
        LedgedCliffTerrain wide(3.0f, 15.0f, 6.0f);
        Climber other;
        other.tryStart(wide, glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        exit = ClimbExit::None;
        steps = 0;
        while (exit == ClimbExit::None && steps < 200000) {
            exit = other.update(dt, 1.0f, 0.0f, wide, nullptr);
            ++steps;
        }
        check(exit == ClimbExit::ToppedOut, "Vire large : sortie par le sommet");
        check(other.anchor().x < 15.0f,
              "Vire large de 6 m : on s'y arrete, c'est du terrain ou l'on tient debout");
    }

    // --- Seuil d'energie a l'engagement ---------------------------------------
    {
        SlopedTerrain wall(0.0f, 3.0f);
        Climber climber;
        climber.tryStart(wall, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        while (climber.climbing()) {
            climber.update(dt, 1.0f, 0.0f, wall, nullptr);
        }
        checkNear(climber.stamina(), 0.0f, 1e-6f, "Seuil d'engagement : jauge videe");
        check(!climber.tryStart(wall, glm::vec3(0.0f, 0.0f, 0.0f),
                                glm::vec3(1.0f, 0.0f, 0.0f)),
              "Seuil d'engagement : jauge vide, on refuse de s'engager");

        climber.restOnGround(Climber::minStaminaToStart() /
                                 defaults.staminaRecoveryPerSecond + 1.0f);
        check(climber.stamina() >= Climber::minStaminaToStart(),
              "Seuil d'engagement : la jauge est remontee au-dessus du seuil");
        check(climber.tryStart(wall, glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(1.0f, 0.0f, 0.0f)),
              "Seuil d'engagement : au-dessus du seuil, on peut repartir");
    }

    // --- Collision : le meme moveBody que le joueur a pied --------------------
    {
        // Rien de la collision n'est reecrit dans Climber : si ce test passe,
        // c'est que le grimpeur emprunte bien le point d'entree unique du projet.
        SlopedTerrain wall(0.0f, 3.0f);
        Ground ground(wall);
        ForestParams params;
        params.treeCount = 0; // on plante a la main : test deterministe
        Forest forest(wall, ground, 200.0f, params);

        Climber climber;
        climber.tryStart(wall, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

        const glm::vec3 start = climber.anchor();
        forest.plantTree(start.x, start.z + 1.5f, 8.0f);

        for (int i = 0; i < 300; ++i) {
            climber.update(dt, 0.0f, 1.0f, wall, &forest);
        }
        // Sans obstacle, 5 s de traversee a 0,6 m/s feraient 3 m. Le tronc est a
        // 1,5 m : on ne doit pas l'avoir traverse.
        const float travelled = climber.anchor().z - start.z;
        check(travelled < 1.5f,
              "Collision : le grimpeur bute sur le tronc au lieu de le traverser");
        check(travelled > 0.0f, "Collision : il a quand meme progresse jusqu'au tronc");

        // Rester coince contre un tronc ne doit PAS etre un repos gratuit. Une
        // premiere version de ce test attendait l'inverse ("buter coute moins
        // que traverser") ; la mesure a montre que c'est faux, et que c'est tant
        // mieux : un grimpeur bloque est facture au tarif SUSPENDU (par seconde),
        // et sur ce modele suspendre 5 s coute 0,020 quand traverser 3 m n'en
        // coute que 0,015. Pousser contre un obstacle est donc plus couteux que
        // d'avancer, ce qui est le bon comportement - on ne se repose pas en
        // s'appuyant sur un arbre.
        const float pushedCost = 1.0f - climber.stamina();
        check(pushedCost > 0.0f,
              "Collision : rester bloque contre un tronc consomme quand meme de "
              "l'energie (pas de repos gratuit)");

        Climber unobstructed;
        unobstructed.tryStart(wall, glm::vec3(0.0f, 0.0f, 0.0f),
                              glm::vec3(1.0f, 0.0f, 0.0f));
        for (int i = 0; i < 300; ++i) {
            unobstructed.update(dt, 0.0f, 1.0f, wall, nullptr);
        }
        check(horizontalDistance(unobstructed.anchor(),
                                 glm::vec3(0.0f, 0.0f, 0.0f)) > travelled,
              "Collision : sans obstacle, la meme duree fait parcourir plus de "
              "chemin - c'est bien le tronc qui a arrete le grimpeur");
    }

    // --- LA VRAIE PAROI DU TERRAIN LIDAR --------------------------------------
    // Conditionnel a dessein, comme le bloc de dalle reelle de testTerrainFile :
    // le depot ne versionne pas les heightmaps. Quand la dalle est la, c'est le
    // SEUL test d'escalade qui touche du relief mesure - et c'est celui qui
    // compte, parce qu'un plan incline synthetique ne porte aucun bruit et n'a
    // jamais prouve grand-chose (lecon de la Phase 13).
    {
        const std::filesystem::path realTile =
            std::filesystem::path(ASSETS_DIR) / "terrain" / "LHD_0899_6436_1m.vchm";
        if (!std::filesystem::exists(realTile)) {
            std::printf("  (dalle reelle absente, bloc ignore : %s)\n",
                        realTile.string().c_str());
        } else {
            const Heightmap tile = loadTerrainFile(realTile.string());

            // VOIE de reference de la Phase 14, trouvee par
            // `find_climb_walls.py --routes` : depart E 899900 / N 6435550, soit
            // x = +400 / z = -50 dans le monde moteur de cette dalle, cap 0 degre
            // (plein est). 21,4 m gagnes sur 27 m de trajet en ligne droite, dont
            // 63 % au-dessus du seuil de paroi, et un depart a 57 % de pente -
            // donc du terrain ou l'on tient debout.
            //
            // POURQUOI UNE "VOIE" ET PAS UNE "PAROI" : le premier essai de ce
            // test visait le centre d'une paroi reperee par le mode par defaut de
            // l'outil (E 899850 / N 6435230) et l'abordait plein est. Il a echoue,
            // et l'echec etait instructif : cette paroi DESCEND vers l'est. Le
            // test approchait donc par le SOMMET, marchait au bord de la falaise,
            // et le grimpeur remontait correctement les 3,9 m qu'il venait de
            // descendre avant de topper. Le modele avait raison, c'est la
            // geometrie du test qui etait fausse.
            //
            // C'est exactement la lecon du scenario E de la Phase 13 : savoir
            // qu'il y a de la pente quelque part ne dit pas PAR OU l'aborder, et
            // le terrain de l'autre cote d'un point est un AUTRE terrain. D'ou le
            // mode --routes de l'outil, qui evalue le relief le long de chaque
            // cap depuis un pied marchable.
            const float footX = 400.0f;
            const float footZ = -50.0f;
            const float headingRad = glm::radians(0.0f);
            const glm::vec3 lookDir(std::cos(headingRad), 0.0f, std::sin(headingRad));

            Climber climber;
            const float measured = climber.slopeAt(tile, footX, footZ);
            std::printf("  [mesure] voie de reference E 899900 N 6435550 : "
                        "pente au depart %.1f %%\n",
                        static_cast<double>(100.0f * measured));
            check(!climber.isWall(tile, footX, footZ),
                  "Voie LiDAR reelle : le pied n'est PAS une paroi (on y marche)");

            // Engagement : on marche vers la paroi le long du cap jusqu'a
            // l'avoir a portee - exactement ce que fait le joueur.
            float approach = 0.0f;
            bool engaged = false;
            for (int step = 0; step < 160 && !engaged; ++step) {
                const float ex = footX + lookDir.x * approach;
                const float ez = footZ + lookDir.z * approach;
                const glm::vec3 eye(ex, tile.heightAt(ex, ez) + 1.7f, ez);
                engaged = climber.tryStart(tile, eye, lookDir);
                approach += 0.25f;
            }
            check(engaged,
                  "Voie LiDAR reelle : on s'engage en marchant vers la paroi");
            check(climber.isWall(tile, climber.anchor().x, climber.anchor().z),
                  "Voie LiDAR reelle : le point d'accroche est bien sur une paroi");

            if (engaged) {
                const float startY = climber.anchor().y;
                ClimbExit exit = ClimbExit::None;
                int steps = 0;
                float peak = startY;
                bool everNaN = false;
                bool alwaysOnSurface = true;
                bool eyeAlwaysOutside = true;
                while (exit == ClimbExit::None && steps < 200000) {
                    exit = climber.update(dt, 1.0f, 0.0f, tile, nullptr);
                    const glm::vec3 a = climber.anchor();
                    if (std::isnan(a.x) || std::isnan(a.y) || std::isnan(a.z) ||
                        std::isnan(climber.stamina())) {
                        everNaN = true;
                    }
                    if (exit == ClimbExit::None) {
                        peak = std::max(peak, a.y);
                        if (std::abs(a.y - tile.heightAt(a.x, a.z)) > 1e-2f) {
                            alwaysOnSurface = false;
                        }
                        const glm::vec3 eye = climber.position();
                        if (eye.y <= tile.heightAt(eye.x, eye.z)) {
                            eyeAlwaysOutside = false;
                        }
                    }
                    ++steps;
                }

                std::printf("  [mesure] ascension sur relief LiDAR reel : %.1f m "
                            "gagnes en %.1f s, sortie %s, energie restante %.0f %%\n",
                            static_cast<double>(peak - startY),
                            static_cast<double>(steps * dt),
                            exit == ClimbExit::ToppedOut ? "SOMMET"
                                                         : "epuisement",
                            static_cast<double>(100.0f * climber.stamina()));

                check(!everNaN,
                      "Voie LiDAR reelle : aucun NaN sur toute l'ascension");
                check(alwaysOnSurface,
                      "Voie LiDAR reelle : le grimpeur ne quitte jamais la surface");
                check(eyeAlwaysOutside,
                      "Voie LiDAR reelle : l'oeil ne passe jamais sous le relief");
                // 15 m et non 21,4 : le grimpeur suit la ligne de PLUS GRANDE
                // PENTE, qui s'incurve, alors que l'outil mesure le relief le
                // long d'un cap FIXE. Les deux trajets divergent, et le gain
                // reellement obtenu (~19 m) est donc inferieur a la projection en
                // ligne droite. C'est un resultat, pas un defaut - mais ca
                // interdit de recopier le chiffre de l'outil dans une assertion.
                check(peak - startY > 15.0f,
                      "Voie LiDAR reelle : plus de 15 m reellement gagnes");
                check(exit == ClimbExit::ToppedOut,
                      "Voie LiDAR reelle : la voie de reference se TOPPE");
                check(climber.stamina() > 0.0f,
                      "Voie LiDAR reelle : il restait de l'energie au sommet");
                check(!climber.isWall(tile, climber.anchor().x, climber.anchor().z),
                      "Voie LiDAR reelle : on termine sur du terrain non-paroi");
            }
        }
    }
}


// ---------------------------------------------------------------------------
// PHASE 24 : LA MARCHE A UNE LIMITE DE PENTE, ET ELLE REJOINT L'ESCALADE
// ---------------------------------------------------------------------------
// Ce qui est teste ici est core/SlopeLimits.hpp, pas core/Camera : la camera se
// contente d'appeler resolveWalkStep() une fois par frame avec le pas que le
// clavier demandait. Tester la fonction plutot que la camera n'est pas un
// contournement du fait que Camera::update() exige une Window - c'est le bon
// niveau : toute la decision est dans le filtre, la camera n'y ajoute que la
// lecture des touches.
//
// L'assertion qui compte le plus est celle du premier bloc : celle qui verifie
// qu'il n'existe aucune pente ou l'on ne puisse ni marcher ni grimper. C'etait
// le vrai defaut de coherence, et c'est la seule chose ici qu'un reglage
// distrait pourrait reintroduire sans rien casser d'autre.
void testWalkSlopeLimit() {
    section("Phase 24 : limite de pente a la marche, et sa jonction avec l'escalade");

    const WalkSlopeParams walkParams;
    const ClimberParams climbing;
    constexpr float dt = 1.0f / 60.0f;
    constexpr float walkSpeed = 14.0f; // la vitesse de marche du moteur
    const float step = walkSpeed * dt;

    // --- LA PROPRIETE STRUCTURELLE : AUCUN TROU ENTRE LES DEUX ACTIVITES -----
    // Elle se verifie sans terrain du tout, parce qu'elle ne porte que sur les
    // seuils. Deux nombres regles separement finissent toujours par diverger ;
    // cette assertion est ce qui l'empeche.
    {
        check(climbing.wallSlope <= walkParams.maxSlope,
              "Coherence : l'escalade s'engage AU PLUS TARD la ou la marche "
              "s'arrete (aucune bande interdite aux deux)");
        check(climbing.walkableSlope < walkParams.maxSlope,
              "Coherence : on sort de l'escalade sur du terrain reellement "
              "marchable, pas sur du terrain deja trop raide");
        check(walkParams.comfortSlope < walkParams.maxSlope,
              "Coherence : le ralentissement precede le blocage (pas de mur sec)");

        // Le balayage explicite, parce que l'inegalite ci-dessus se lit vite
        // mais se croit mal : pour 300 pentes reparties de 0 a 300 %, il y a
        // TOUJOURS au moins une des deux activites qui accepte.
        bool everStuck = false;
        float worstSlope = -1.0f;
        for (int i = 0; i <= 300; ++i) {
            const float slope = 0.01f * static_cast<float>(i);
            const bool canWalk = walkSpeedFactor(slope, walkParams) > 0.0f;
            const bool canClimb = slope >= climbing.wallSlope;
            if (!canWalk && !canClimb) {
                everStuck = true;
                worstSlope = slope;
            }
        }
        if (everStuck) {
            std::printf("  [mesure] pente sans issue trouvee : %.0f %%\n",
                        static_cast<double>(100.0f * worstSlope));
        }
        check(!everStuck,
              "Aucune pente de 0 a 300 % n'est a la fois trop raide pour "
              "marcher et trop douce pour grimper");
    }

    // --- LE FACTEUR DE VITESSE ----------------------------------------------
    {
        checkNear(walkSpeedFactor(0.0f, walkParams), 1.0f, 1e-6f,
                  "A plat : pleine vitesse");
        checkNear(walkSpeedFactor(walkParams.comfortSlope, walkParams), 1.0f, 1e-6f,
                  "Au seuil de confort (60 %) : encore pleine vitesse");
        checkNear(walkSpeedFactor(0.5f * (walkParams.comfortSlope + walkParams.maxSlope),
                                  walkParams),
                  0.5f, 1e-4f, "A mi-chemin (72 %) : demi-vitesse");
        checkNear(walkSpeedFactor(walkParams.maxSlope, walkParams), 0.0f, 1e-6f,
                  "Au seuil maximal (84 %) : plus aucune progression");
        checkNear(walkSpeedFactor(30.0f, walkParams), 0.0f, 1e-6f,
                  "Sur une paroi a 3000 % : toujours zero, jamais de negatif");

        // La rampe doit etre MONOTONE : un joueur qui aborde une pente de plus
        // en plus raide ne doit jamais reaccelerer.
        bool monotone = true;
        float previous = 1.0f;
        for (int i = 0; i <= 200; ++i) {
            const float f = walkSpeedFactor(0.01f * static_cast<float>(i), walkParams);
            if (f > previous + 1e-6f) monotone = false;
            previous = f;
        }
        check(monotone, "Le ralentissement est monotone : jamais de reprise de "
                        "vitesse quand ca se redresse");
    }

    // --- UN VERSANT DE RANDONNEE NORMALE N'EST PAS TOUCHE --------------------
    // C'est la contrainte explicite de la consigne : ne pas casser la traversee
    // des pentes raides de randonnee. 30 %, c'est un sentier de montagne
    // soutenu ; il doit se marcher exactement comme avant la Phase 24.
    {
        SlopedTerrain hike(0.0f, 0.30f);
        const glm::vec3 from(0.0f, 0.0f, 0.0f);
        const WalkStepResult up =
            resolveWalkStep(hike, from, glm::vec3(step, 0.0f, 0.0f), dt, walkParams);
        checkNear(up.speedFactor, 1.0f, 1e-6f,
                  "Sentier a 30 % : aucun ralentissement");
        checkNear(up.position.x - from.x, step, 1e-4f,
                  "Sentier a 30 % : le pas complet est accorde en montee");
        check(!up.sliding, "Sentier a 30 % : on ne glisse pas");

        // Et le raidillon ponctuel a 80 % passe encore, ralenti. S'il etait
        // refuse, la limite serait mal placee.
        SlopedTerrain hard(0.0f, 0.80f);
        const WalkStepResult tough =
            resolveWalkStep(hard, from, glm::vec3(step, 0.0f, 0.0f), dt, walkParams);
        check(tough.position.x - from.x > 0.0f,
              "Raidillon a 80 % : ca monte encore, meme ralenti");
        std::printf("  [mesure] vitesse de marche accordee : 30 %% -> %.0f %%, "
                    "60 %% -> %.0f %%, 72 %% -> %.0f %%, 80 %% -> %.0f %%, "
                    "84 %% -> %.0f %%\n",
                    static_cast<double>(100.0f * walkSpeedFactor(0.30f, walkParams)),
                    static_cast<double>(100.0f * walkSpeedFactor(0.60f, walkParams)),
                    static_cast<double>(100.0f * walkSpeedFactor(0.72f, walkParams)),
                    static_cast<double>(100.0f * walkSpeedFactor(0.80f, walkParams)),
                    static_cast<double>(100.0f * walkSpeedFactor(0.84f, walkParams)));
    }

    // --- LA DISSYMETRIE AMONT / AVAL ----------------------------------------
    // Point de conception : on refuse de MONTER, jamais de redescendre. Sans
    // cette dissymetrie, un joueur pose sur du trop raide serait fige, ce qui
    // est exactement le blocage frustrant qu'il fallait eviter.
    {
        SlopedTerrain wall(0.0f, 1.00f); // 100 %, au-dessus du maximum
        const glm::vec3 from(0.0f, 0.0f, 0.0f);

        const WalkStepResult up =
            resolveWalkStep(wall, from, glm::vec3(step, 0.0f, 0.0f), dt, walkParams);
        check(up.speedFactor <= 0.0f,
              "Pente a 100 % : la montee a pied n'est plus accordee du tout");

        const WalkStepResult down =
            resolveWalkStep(wall, from, glm::vec3(-step, 0.0f, 0.0f), dt, walkParams);
        check(down.position.x - from.x < -step * 0.99f,
              "Pente a 100 % : REDESCENDRE reste possible, a pleine vitesse");

        check(up.sliding && down.sliding,
              "Pente a 100 % : on glisse, qu'on pousse la touche ou non");

        // La glissade descend, et elle est douce a 100 % (0,16 d'exces).
        const WalkStepResult idle =
            resolveWalkStep(wall, from, glm::vec3(0.0f), dt, walkParams);
        check(idle.position.x < from.x,
              "Pente a 100 %, sans aucune touche : le joueur glisse vers l'AVAL");
        const float slideSpeed = (from.x - idle.position.x) / dt;
        std::printf("  [mesure] glissade : %.2f m/s a 100 %% de pente\n",
                    static_cast<double>(slideSpeed));
        check(slideSpeed > 0.0f && slideSpeed < 1.0f,
              "Glissade a 100 % : un decrochage lent (<1 m/s), pas une ejection");

        // Et sur une vraie paroi elle est plafonnee : sans plafond, les 5018 %
        // mesures sur cette dalle ejecteraient le joueur a 150 m/s.
        SlopedTerrain cliff(0.0f, 30.0f);
        const WalkStepResult ejected =
            resolveWalkStep(cliff, from, glm::vec3(0.0f), dt, walkParams);
        const float cliffSpeed = (from.x - ejected.position.x) / dt;
        check(cliffSpeed <= walkParams.slideSpeedMax + 1e-3f,
              "Glissade sur une paroi a 3000 % : plafonnee, pas d'ejection");
    }

    // --- AUCUNE POSITION N'EST UN PIEGE -------------------------------------
    // La garantie de fond du chantier : ou qu'on depose le joueur, il finit sur
    // du terrain marchable sans rien faire. C'est ce qui rend le blocage
    // acceptable - il n'immobilise jamais, il renvoie en arriere.
    {
        CliffTerrain cliff(0.0f, 1.60f, 0.0f, 40.0f); // plat / paroi 160 % / plat
        glm::vec3 player(20.0f, 0.0f, 0.0f);          // depose en pleine paroi
        int frames = 0;
        while (frames < 60 * 60) { // une minute de jeu au maximum
            const WalkStepResult r =
                resolveWalkStep(cliff, player, glm::vec3(0.0f), dt, walkParams);
            player = r.position;
            if (!r.sliding) break;
            ++frames;
        }
        const float finalSlope =
            walkTerrainSlope(cliff, player.x, player.z, walkParams.sampleStep);
        std::printf("  [mesure] depose en pleine paroi a 160 %% : ramene sur du "
                    "%.0f %% en %.1f s\n",
                    static_cast<double>(100.0f * finalSlope),
                    static_cast<double>(frames * dt));
        check(finalSlope <= walkParams.maxSlope + 1e-3f,
              "Depose sur du trop raide : la glissade ramene sur du marchable");
        check(player.x < 20.0f, "La glissade va bien vers l'AVAL de la paroi");
    }

    // --- LE ZIGZAG NE CONTOURNE PAS LA LIMITE -------------------------------
    // Controle negatif de la decision de facturer aussi le deplacement LATERAL.
    // Si le lateral etait gratuit, monter en travers a 45 degres rendrait 71 %
    // du gain d'altitude a pleine vitesse et la limite ne servirait a rien.
    {
        SlopedTerrain slope(0.0f, 0.80f);
        const glm::vec3 from(0.0f, 0.0f, 0.0f);

        const WalkStepResult direct =
            resolveWalkStep(slope, from, glm::vec3(step, 0.0f, 0.0f), dt, walkParams);
        const float diagonal = step * 0.70710678f;
        const WalkStepResult zigzag = resolveWalkStep(
            slope, from, glm::vec3(diagonal, 0.0f, diagonal), dt, walkParams);

        const float riseDirect = 0.80f * (direct.position.x - from.x);
        const float riseZigzag = 0.80f * (zigzag.position.x - from.x);
        check(riseZigzag <= riseDirect + 1e-5f,
              "Zigzag : monter en travers ne fait pas gagner plus de hauteur "
              "que monter droit");
    }

    // --- LA JONCTION, SUR UNE FALAISE SYNTHETIQUE ---------------------------
    // Le scenario du joueur : il marche vers une paroi, il est arrete par la
    // pente. La question qui decide de tout le chantier est celle-ci - a
    // l'endroit exact ou la marche s'arrete, est-ce que l'escalade accepte ?
    {
        CliffTerrain cliff(0.0f, 1.60f, 30.0f, 70.0f);
        const glm::vec3 lookDir(1.0f, 0.0f, 0.0f);

        glm::vec3 player(0.0f, 0.0f, 0.0f);
        int frames = 0;
        float lastAdvance = step;
        while (frames < 60 * 60 && lastAdvance > 1e-4f) {
            const WalkStepResult r =
                resolveWalkStep(cliff, player, glm::vec3(step, 0.0f, 0.0f), dt, walkParams);
            lastAdvance = r.position.x - player.x;
            player = r.position;
            ++frames;
        }

        const float stopSlope =
            walkTerrainSlope(cliff, player.x, player.z, walkParams.sampleStep);
        std::printf("  [mesure] falaise synthetique : la marche s'arrete a "
                    "x = %.2f m, pente sous les pieds %.0f %%\n",
                    static_cast<double>(player.x),
                    static_cast<double>(100.0f * stopSlope));

        check(player.x < 70.0f,
              "Falaise : la marche ne franchit plus la paroi (elle passait avant "
              "la Phase 24)");
        check(stopSlope <= walkParams.maxSlope + 1e-3f,
              "Falaise : le joueur s'arrete AU PIED, sur du terrain ou il tient "
              "encore debout - pas coince en pleine paroi");

        // ET C'EST ICI QUE LE CHANTIER SE JOUE : depuis ce point d'arret,
        // l'escalade doit accepter de s'engager.
        Climber climber;
        const glm::vec3 eye(player.x, cliff.heightAt(player.x, player.z) + 1.7f,
                            player.z);
        check(climber.tryStart(cliff, eye, lookDir),
              "Falaise : la ou la marche s'arrete, l'ESCALADE PREND LE RELAIS");
    }

    // --- LA MEME JONCTION, SUR LE VRAI RELIEF LIDAR -------------------------
    // Meme raison d'etre que le bloc LiDAR de testClimber() : un plan incline
    // synthetique n'a jamais rien prouve sur ce projet. La voie employee est
    // celle de reference de la Phase 14 - pied marchable a E 899900 /
    // N 6435550, cap plein est - precisement parce qu'elle est deja connue
    // comme une VRAIE transition marche / paroi.
    {
        const std::filesystem::path realTile =
            std::filesystem::path(ASSETS_DIR) / "terrain" / "LHD_0899_6436_1m.vchm";
        if (!std::filesystem::exists(realTile)) {
            std::printf("  (dalle reelle absente, bloc ignore : %s)\n",
                        realTile.string().c_str());
        } else {
            const Heightmap tile = loadTerrainFile(realTile.string());
            const glm::vec3 lookDir(1.0f, 0.0f, 0.0f);

            glm::vec3 player(400.0f, 0.0f, -50.0f);
            const float startSlope =
                walkTerrainSlope(tile, player.x, player.z, walkParams.sampleStep);
            check(startSlope < walkParams.maxSlope,
                  "Voie LiDAR reelle : le pied de la voie se marche encore");

            float travelled = 0.0f;
            int frames = 0;
            float lastAdvance = step;
            while (frames < 60 * 120 && lastAdvance > 1e-4f) {
                const WalkStepResult r = resolveWalkStep(
                    tile, player, glm::vec3(step, 0.0f, 0.0f), dt, walkParams);
                lastAdvance = r.position.x - player.x;
                travelled += lastAdvance;
                player = r.position;
                ++frames;
            }

            const float stopSlope =
                walkTerrainSlope(tile, player.x, player.z, walkParams.sampleStep);
            std::printf("  [mesure] relief LiDAR reel : la marche progresse de "
                        "%.1f m puis s'arrete, pente au point d'arret %.0f %%, "
                        "altitude gagnee %.1f m\n",
                        static_cast<double>(travelled),
                        static_cast<double>(100.0f * stopSlope),
                        static_cast<double>(tile.heightAt(player.x, player.z) -
                                            tile.heightAt(400.0f, -50.0f)));

            check(travelled > 1.0f,
                  "Voie LiDAR reelle : le joueur avance reellement avant d'etre "
                  "arrete (ce n'est pas un blocage des le premier pas)");
            check(stopSlope <= walkParams.maxSlope + 1e-3f,
                  "Voie LiDAR reelle : l'arret se fait sur du terrain ou l'on "
                  "tient debout");

            Climber climber;
            const glm::vec3 eye(player.x, tile.heightAt(player.x, player.z) + 1.7f,
                                player.z);
            const bool engaged = climber.tryStart(tile, eye, lookDir);
            check(engaged,
                  "Voie LiDAR reelle : l'escalade prend le relais LA OU la "
                  "marche s'est arretee");
            if (!engaged) {
                // Diagnostic plutot qu'une simple croix : sur ce projet, savoir
                // POURQUOI un engagement echoue a deja fait gagner des phases
                // entieres (pente insuffisante ? ou ressaut sans relief ?).
                std::printf("  [diagnostic] pente devant = %.0f %%, seuil de "
                            "paroi = %.0f %%\n",
                            static_cast<double>(
                                100.0f * climber.slopeAt(tile, player.x + 1.0f, player.z)),
                            static_cast<double>(100.0f * climber.params().wallSlope));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 10 : quadtree de terrain, selection de LOD, frustum culling
// ---------------------------------------------------------------------------
void testTerrainQuadtree() {
    section("Phase 10 : quadtree, LOD et frustum culling");

    // Matrice view-projection reutilisee par les tests de culling : camera a
    // l'origine regardant vers -Z (la convention du moteur : Camera part avec un
    // yaw de -90 degres), 60 degres de champ, exactement comme le Renderer.
    const auto viewProjectionLookingNorth = [](float farPlane) {
        glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, farPlane);
        proj[1][1] *= -1.0f; // convention Vulkan, comme Renderer::projectionMatrix
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f),
                                           glm::vec3(0.0f, 1.0f, 0.0f));
        return proj * view;
    };

    // --- Frustum : ce qui est devant est vu, ce qui est derriere ne l'est pas -
    {
        const Frustum frustum = Frustum::fromViewProjection(viewProjectionLookingNorth(1000.0f));

        check(frustum.intersectsAABB(glm::vec3(-1.0f, -1.0f, -51.0f),
                                     glm::vec3(1.0f, 1.0f, -49.0f)),
              "Frustum : une boite droit devant est visible");
        check(!frustum.intersectsAABB(glm::vec3(-1.0f, -1.0f, 49.0f),
                                      glm::vec3(1.0f, 1.0f, 51.0f)),
              "Frustum : une boite DERRIERE la camera est ecartee");
        check(!frustum.intersectsAABB(glm::vec3(499.0f, -1.0f, -51.0f),
                                      glm::vec3(501.0f, 1.0f, -49.0f)),
              "Frustum : une boite loin sur le cote est ecartee");
        check(!frustum.intersectsAABB(glm::vec3(-1.0f, -1.0f, -2001.0f),
                                      glm::vec3(1.0f, 1.0f, -1999.0f)),
              "Frustum : une boite au-dela du plan lointain est ecartee");
        check(frustum.intersectsAABB(glm::vec3(-10000.0f, -10000.0f, -10000.0f),
                                     glm::vec3(10000.0f, 10000.0f, 10000.0f)),
              "Frustum : une boite qui englobe la camera est visible");
    }

    // --- distanceToAABB -----------------------------------------------------
    {
        const glm::vec3 lo(-1.0f, -1.0f, -1.0f);
        const glm::vec3 hi(1.0f, 1.0f, 1.0f);
        checkNear(distanceToAABB(glm::vec3(0.0f), lo, hi), 0.0f, 1e-5f,
                  "distanceToAABB : nulle a l'interieur de la boite");
        checkNear(distanceToAABB(glm::vec3(4.0f, 0.0f, 0.0f), lo, hi), 3.0f, 1e-5f,
                  "distanceToAABB : distance a la FACE la plus proche, pas au centre");
    }

    // --- Structure de l'arbre ----------------------------------------------
    Heightmap::Params params;
    params.resolution = 257;   // 256 cellules = 4 x 4 feuilles de 64
    params.worldSize = 256.0f; // pas de 1 unite, pour raisonner en metres ronds
    const Heightmap heightmap(params);

    TerrainQuadtree::Params quadtreeParams;
    quadtreeParams.nodeCells = 64;
    const TerrainQuadtree quadtree(heightmap, quadtreeParams);

    check(quadtree.rootLevel() == 2,
          "Quadtree : 256 cellules / 64 = 4 x 4 feuilles -> racine au niveau 2");
    check(quadtree.nodeCount() == 21, "Quadtree : 16 feuilles + 4 noeuds + 1 racine = 21");
    check(quadtree.trianglesPerNode() == 64 * 64 * 2,
          "Quadtree : un noeud dessine le meme nombre de triangles a tous les niveaux");
    check(quadtree.range(1) > quadtree.range(0),
          "Quadtree : la portee croit avec le niveau");
    checkNear(quadtree.range(1), 2.0f * quadtree.range(0), 1e-3f,
              "Quadtree : la portee DOUBLE d'un niveau au suivant");

    // La racine couvre tout le terrain, et ses bornes verticales encadrent
    // reellement le relief : une boite trop petite ferait disparaitre du terrain
    // au culling.
    {
        const TerrainQuadtree::Node& root = quadtree.node(0);
        check(root.level == quadtree.rootLevel(), "Quadtree : le noeud 0 est la racine");
        checkNear(root.boundsMin.x, -128.0f, 1e-3f, "Quadtree : la racine part du bord ouest");
        checkNear(root.boundsMax.x, 128.0f, 1e-3f, "Quadtree : la racine va au bord est");
        check(root.boundsMin.y <= heightmap.minHeight() + 1e-3f &&
              root.boundsMax.y >= heightmap.maxHeight() - 1e-3f,
              "Quadtree : la boite de la racine englobe TOUTE la plage d'altitudes");
    }

    // --- Selection : plus on est pres, plus le niveau est fin ---------------
    std::vector<TerrainQuadtree::SelectedNode> selection;
    {
        // Frustum permissif (tout est visible) : on isole ici le critere de
        // DISTANCE, le culling est teste separement juste apres.
        const Frustum everything = Frustum::fromViewProjection(glm::mat4(0.0f));

        const glm::vec3 corner(-120.0f, heightmap.maxHeight(), -120.0f);
        quadtree.select(corner, everything, selection);
        check(!selection.empty(), "Selection : au moins un noeud retenu");

        // Le noeud le plus proche doit etre au niveau le plus fin, et le plus
        // lointain a un niveau plus grossier : c'est exactement la promesse du
        // LOD par distance.
        check(selection.front().level == 0,
              "Selection : la tuile la plus proche est a la PLEINE resolution");
        check(selection.back().level >= selection.front().level,
              "Selection : la tuile la plus lointaine n'est pas plus fine que la plus proche");

        bool sorted = true;
        for (std::size_t i = 1; i < selection.size(); ++i) {
            sorted = sorted && (selection[i - 1].distance <= selection[i].distance + 1e-4f);
        }
        check(sorted, "Selection : triee du plus proche au plus lointain");

        // Bande de morphing : condition ANTI-FISSURE. Un noeud de niveau L doit
        // etre entierement morphe sur la grille L+1 avant range[L], sans quoi son
        // voisin plus grossier ne le rejoint pas.
        bool bandsValid = true;
        for (const TerrainQuadtree::SelectedNode& node : selection) {
            bandsValid = bandsValid &&
                         node.morphStart > 0.5f * quadtree.range(node.level) &&
                         node.morphEnd > node.morphStart &&
                         node.morphEnd <= quadtree.range(node.level);
        }
        check(bandsValid,
              "Selection : bande de morphing dans (range/2, range] pour chaque tuile");

        // COUVERTURE : les tuiles retenues doivent paver tout le terrain, sans
        // trou ni recouvrement. On le verifie en surface : la somme des emprises
        // horizontales doit valoir exactement celle du terrain.
        double coveredArea = 0.0;
        for (const TerrainQuadtree::SelectedNode& selected : selection) {
            const TerrainQuadtree::Node& node = quadtree.node(selected.nodeIndex);
            coveredArea += static_cast<double>(node.boundsMax.x - node.boundsMin.x) *
                           static_cast<double>(node.boundsMax.z - node.boundsMin.z);
        }
        checkNear(static_cast<float>(coveredArea), 256.0f * 256.0f, 1.0f,
                  "Selection : les tuiles retenues pavent le terrain, sans trou ni recouvrement");
    }

    // --- Frustum culling : regarder ailleurs retire des tuiles ---------------
    {
        const Frustum everything = Frustum::fromViewProjection(glm::mat4(0.0f));
        const glm::vec3 eye(0.0f, heightmap.maxHeight() + 50.0f, 0.0f);

        quadtree.select(eye, everything, selection);
        const std::size_t withoutCulling = selection.size();

        const Frustum frustum = Frustum::fromViewProjection(viewProjectionLookingNorth(2000.0f));
        quadtree.select(eye, frustum, selection);
        const std::size_t withCulling = selection.size();

        check(withCulling < withoutCulling,
              "Culling : regarder dans une direction retient MOINS de tuiles que tout voir");
        check(withCulling > 0, "Culling : il reste des tuiles devant la camera");
    }

    // --- Maillage d'un noeud ------------------------------------------------
    {
        // Une feuille : pas de 1 echantillon, donc les sommets doivent tomber
        // EXACTEMENT sur la grille - c'est ce qui garantit qu'au contact du
        // joueur la surface affichee est celle sur laquelle il marche.
        uint32_t leafIndex = 0;
        for (uint32_t i = 0; i < quadtree.nodeCount(); ++i) {
            if (quadtree.node(i).level == 0) {
                leafIndex = i;
                break;
            }
        }
        const TerrainQuadtree::Node& leaf = quadtree.node(leafIndex);
        check(leaf.stride == 1, "Tuile : une feuille echantillonne au pas de la donnee");

        const MeshData mesh = terrainNodeMeshData(heightmap, quadtree, leafIndex);
        check(mesh.vertices.size() == 65u * 65u, "Tuile : 65 x 65 sommets");
        check(mesh.triangleCount() == 64u * 64u * 2u, "Tuile : 8192 triangles");

        // --- PHASE 18 -----------------------------------------------------
        // La cible de morphing est desormais lue dans une grille grossiere
        // calculee une fois par tuile, au lieu d'etre recalculee par sommet.
        // Cela n'est legitime QUE si les quatre sommets d'un bloc 2x2 partagent
        // bien la meme cible - c'est l'hypothese sur laquelle repose toute
        // l'optimisation, et elle merite d'etre verifiee plutot que crue.
        bool blockShared = true;
        for (uint32_t j = 0; j + 1 <= 64 && blockShared; j += 2) {
            for (uint32_t i = 0; i + 1 <= 64 && blockShared; i += 2) {
                const Vertex& reference = mesh.vertices[j * 65u + i];
                for (uint32_t dj = 0; dj < 2; ++dj) {
                    for (uint32_t di = 0; di < 2; ++di) {
                        const Vertex& other = mesh.vertices[(j + dj) * 65u + (i + di)];
                        if (other.morphPos != reference.morphPos ||
                            other.morphNormal != reference.morphNormal) {
                            blockShared = false;
                        }
                    }
                }
            }
        }
        check(blockShared,
              "Tuile : les 4 sommets d'un bloc 2x2 visent la MEME cible de morphing");

        // `withIndices = false` (le chemin du TerrainStreamer depuis la
        // Phase 18) doit produire exactement les memes SOMMETS et laisser la
        // topologie a l'appelant. Sans cette verification, une tuile pourrait
        // etre construite differemment selon l'appelant.
        const MeshData bare = terrainNodeMeshData(heightmap, quadtree, leafIndex, false);
        check(bare.indices.empty(), "Tuile : withIndices=false laisse la topologie vide");
        bool sameVertices = (bare.vertices.size() == mesh.vertices.size());
        for (std::size_t v = 0; v < bare.vertices.size() && sameVertices; ++v) {
            sameVertices = (bare.vertices[v].pos == mesh.vertices[v].pos) &&
                           (bare.vertices[v].normal == mesh.vertices[v].normal) &&
                           (bare.vertices[v].morphPos == mesh.vertices[v].morphPos);
        }
        check(sameVertices, "Tuile : memes sommets avec et sans la topologie");
        check(terrainNodeIndices(64u).size() == 64u * 64u * 6u &&
                  terrainNodeIndices(64u) == mesh.indices,
              "Tuile : la topologie posee par l'appelant est bien la meme");

        bool onGrid = true;
        for (uint32_t j = 0; j <= 64 && onGrid; ++j) {
            for (uint32_t i = 0; i <= 64 && onGrid; ++i) {
                const Vertex& vertex = mesh.vertices[j * 65 + i];
                onGrid = std::abs(vertex.pos.y -
                                  heightmap.at(leaf.ix0 + i, leaf.iz0 + j)) < 1e-4f;
            }
        }
        check(onGrid, "Tuile de niveau 0 : chaque sommet porte l'altitude EXACTE de la grille");

        // Geomorphing : un sommet d'indice local PAIR est deja sur la grille du
        // niveau superieur et ne doit pas bouger ; un sommet IMPAIR doit venir se
        // confondre avec son voisin pair. C'est cette propriete qui fait qu'a
        // morph = 1 la tuile a exactement la forme de son parent.
        bool evenStaysPut = true;
        bool oddCollapses = true;
        for (uint32_t j = 0; j <= 64; j += 2) {
            for (uint32_t i = 0; i <= 64; ++i) {
                const Vertex& vertex = mesh.vertices[j * 65 + i];
                if (i % 2 == 0) {
                    evenStaysPut = evenStaysPut &&
                                   glm::length(vertex.morphPos - vertex.pos) < 1e-4f;
                } else {
                    const Vertex& evenNeighbour = mesh.vertices[j * 65 + (i - 1)];
                    oddCollapses = oddCollapses &&
                                   glm::length(vertex.morphPos - evenNeighbour.pos) < 1e-4f;
                }
            }
        }
        check(evenStaysPut, "Geomorphing : un sommet d'indice pair ne bouge pas");
        check(oddCollapses,
              "Geomorphing : un sommet impair rejoint son voisin pair (grille du parent)");

        // Les indices sont les MEMES pour toutes les tuiles : c'est ce qui
        // permettra de partager un seul index buffer le jour ou on le fera.
        const std::vector<uint32_t> shared = terrainNodeIndices(quadtree.nodeCells());
        check(shared == mesh.indices, "Tuile : topologie identique pour tous les noeuds");
    }

    // --- Une tuile grossiere reste DANS les bornes de son noeud -------------
    {
        const MeshData rootMesh = terrainNodeMeshData(heightmap, quadtree, 0);
        const TerrainQuadtree::Node& root = quadtree.node(0);
        bool inside = true;
        for (const Vertex& vertex : rootMesh.vertices) {
            inside = inside &&
                     vertex.pos.x >= root.boundsMin.x - 1e-3f &&
                     vertex.pos.x <= root.boundsMax.x + 1e-3f &&
                     vertex.pos.z >= root.boundsMin.z - 1e-3f &&
                     vertex.pos.z <= root.boundsMax.z + 1e-3f &&
                     vertex.pos.y >= root.boundsMin.y - 1e-3f &&
                     vertex.pos.y <= root.boundsMax.y + 1e-3f;
        }
        check(inside, "Tuile grossiere : tous ses sommets tiennent dans la boite du noeud");
    }

    // --- DECOUPLAGE RENDU / COLLISION ---------------------------------------
    // Le point le plus important de la Phase 10 cote gameplay : la physique ne
    // doit RIEN devoir au niveau de detail affiche. La Heightmap reste la source
    // unique et pleine resolution de heightAt() ; construire, selectionner ou
    // mailler le quadtree ne la touche pas.
    {
        const float before = heightmap.heightAt(12.5f, -33.25f);

        const Frustum everything = Frustum::fromViewProjection(glm::mat4(0.0f));
        quadtree.select(glm::vec3(0.0f, 5000.0f, 0.0f), everything, selection); // tout au loin
        const float farAway = heightmap.heightAt(12.5f, -33.25f);
        quadtree.select(glm::vec3(12.5f, 0.0f, -33.25f), everything, selection); // au contact
        const float closeUp = heightmap.heightAt(12.5f, -33.25f);

        check(before == farAway && before == closeUp,
              "Collision : heightAt() est insensible au niveau de detail selectionne");

        // Et la surface de collision est bien celle de la donnee, pas celle
        // d'une tuile grossiere : au noeud de grille, heightAt() vaut at().
        checkNear(heightmap.heightAt(heightmap.worldCoord(70), heightmap.worldCoord(70)),
                  heightmap.at(70, 70), 1e-3f,
                  "Collision : heightAt() reste la surface pleine resolution");
    }

    // --- Grilles non multiples de la taille de tuile -------------------------
    // Le terrain procedural (128 echantillons, donc 127 cellules) et la zone
    // assemblee (5001, donc 5000) ne tombent PAS sur un multiple de 64. Le
    // quadtree doit les accepter en ecretant, sans deborder ni laisser de trou.
    {
        Heightmap::Params odd;
        odd.resolution = 128;
        odd.worldSize = 127.0f;
        const Heightmap oddMap(odd);
        const TerrainQuadtree oddTree(oddMap, quadtreeParams);
        check(oddTree.rootLevel() == 1, "Grille de 127 cellules : racine au niveau 1");

        const MeshData mesh = terrainNodeMeshData(oddMap, oddTree, 0);
        const float edge = oddMap.worldCoord(oddMap.resolution() - 1);
        bool clamped = true;
        for (const Vertex& vertex : mesh.vertices) {
            clamped = clamped && vertex.pos.x <= edge + 1e-3f && vertex.pos.z <= edge + 1e-3f;
        }
        check(clamped,
              "Grille non multiple : aucun sommet ne depasse le bord (ecretage, pas d'etagere)");
    }

    // --- Parametres invalides refuses ---------------------------------------
    {
        const auto rejected = [&](const TerrainQuadtree::Params& bad) {
            try {
                TerrainQuadtree tree(heightmap, bad);
                return false;
            } catch (const std::runtime_error&) {
                return true;
            }
        };

        TerrainQuadtree::Params notPowerOfTwo = quadtreeParams;
        notPowerOfTwo.nodeCells = 60;
        check(rejected(notPowerOfTwo), "Refus : nodeCells qui n'est pas une puissance de deux");

        // morphStartRatio <= 0.5 rouvrirait les fissures entre niveaux : le
        // constructeur doit le refuser plutot que de produire un terrain fendu.
        TerrainQuadtree::Params tooEarly = quadtreeParams;
        tooEarly.morphStartRatio = 0.4f;
        check(rejected(tooEarly), "Refus : bande de morphing qui commence trop tot");
    }
}

// ===========================================================================
// PHASE 11 : orthophoto, vegetation de decor, herbe
// ===========================================================================

// Carte constante : sert a verifier que le semis SUIT bien la carte, en
// comparant deux mondes qui ne different QUE par elle.
class ConstantMap : public HeightSampler {
public:
    explicit ConstantMap(float value) : value_(value) {}
    float heightAt(float, float) const override { return value_; }

private:
    float value_;
};

// Couvert coupe en deux : boise a l'est de x = 0, nu a l'ouest. C'est le test
// decisif de la tache 2 - le semis doit reproduire la frontiere.
class HalfCoveredMap : public HeightSampler {
public:
    float heightAt(float x, float) const override { return x > 0.0f ? 1.0f : 0.0f; }
};

// Frustum qui accepte tout : les tests de SEMIS ne doivent pas dependre du
// cadrage. Les six plans delimitent un volume immense.
Frustum permissiveFrustum() {
    Frustum frustum;
    const float far = 1.0e6f;
    frustum.planes[0] = { 1.0f, 0.0f, 0.0f, far };
    frustum.planes[1] = { -1.0f, 0.0f, 0.0f, far };
    frustum.planes[2] = { 0.0f, 1.0f, 0.0f, far };
    frustum.planes[3] = { 0.0f, -1.0f, 0.0f, far };
    frustum.planes[4] = { 0.0f, 0.0f, 1.0f, far };
    frustum.planes[5] = { 0.0f, 0.0f, -1.0f, far };
    return frustum;
}


// Le semis d'arbres est BUDGETE : gatherTrees ne construit qu'un nombre limite
// de cellules neuves par appel (VegetationParams::treeCellBudget), pour qu'une
// arrivee dans une zone vierge n'engendre pas des milliers de cellules d'un coup
// et ne fasse pas un a-coup visible. En jeu, les cellules manquantes arrivent aux
// images suivantes ; dans un test, il faut donc appeler jusqu'a ce que le semis
// se stabilise, sinon on ne juge que du premier lot construit - c'est-a-dire du
// coin le plus au sud-ouest de la zone balayee, ce qui n'a aucun sens.
void gatherTreesUntilStable(Vegetation& vegetation, const glm::vec3& eye,
                            const Frustum& frustum, std::vector<InstanceData>& out) {
    std::size_t previous = 0;
    int stable = 0;
    for (int pass = 0; pass < 400 && stable < 3; ++pass) {
        vegetation.gatherTrees(eye, frustum, out);
        stable = (out.size() == previous) ? stable + 1 : 0;
        previous = out.size();
    }
}

void testTextureFile() {
    section("Phase 11 : format de texture .vcto");

    // BC1 : blocs de 4x4 texels sur 8 octets.
    check(textureLevelSize(TextureFormat::BC1, 8, 8) == 4ull * 8ull,
          "BC1 8x8 = 4 blocs de 8 octets");
    check(textureLevelSize(TextureFormat::BC1, 8192, 8192) == 33554432ull,
          "BC1 8192x8192 = 32 Mo");
    // Les derniers niveaux de mipmap ne sont PAS multiples de 4 : ils occupent
    // quand meme un bloc entier. Se tromper ici decale toute la fin de la chaine
    // et le fichier ne correspond plus a son en-tete.
    check(textureLevelSize(TextureFormat::BC1, 2, 2) == 8ull,
          "BC1 2x2 occupe un bloc entier");
    check(textureLevelSize(TextureFormat::BC1, 1, 1) == 8ull,
          "BC1 1x1 occupe un bloc entier");
    check(textureLevelSize(TextureFormat::RGBA8, 16, 8) == 512ull,
          "RGBA8 16x8 = 512 octets");

    // La chaine complete d'une texture de 8192, telle que la calcule le
    // chargeur, doit valoir exactement la taille produite par l'outil Python.
    uint64_t total = 0;
    for (uint32_t level = 0; level < 14; ++level) {
        const uint32_t side = std::max(1u, 8192u >> level);
        total += textureLevelSize(TextureFormat::BC1, side, side);
    }
    check(total == 44739256ull,
          "Chaine de 14 mipmaps BC1 8192 = 44 739 256 octets");

    TextureData data;
    data.width = 8192;
    data.height = 8192;
    data.eastMin = 899000.0;
    data.eastMax = 904000.0;
    data.northMin = 6435000.0;
    data.northMax = 6440000.0;
    checkNear(static_cast<float>(data.metresPerTexel()), 0.6104f, 0.001f,
              "0,61 m par texel sur 5 km en 8192 texels");

    // Un fichier absent doit LEVER, pas renvoyer une texture vide qui serait
    // ensuite drapee en silence sur le terrain.
    bool threw = false;
    try {
        loadTextureFile("chemin/qui/n/existe/pas.vcto");
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "Refus : fichier .vcto absent");
}

void testGrassMesh() {
    section("Phase 11 : maillage de touffe d'herbe");

    const MeshData tuft = grassTuftMeshData(3);
    check(tuft.vertices.size() == 12, "3 quads croises = 12 sommets");
    check(tuft.triangleCount() == 6, "3 quads croises = 6 triangles");

    // CONVENTION UNITE, la meme que l'arbre : pied a l'origine, hauteur 1. Une
    // touffe deja a l'echelle obligerait a une echelle non uniforme, donc a
    // transporter une vraie normal matrix (voir InstanceData.hpp).
    float lowest = 1.0e9f;
    float highest = -1.0e9f;
    for (const Vertex& vertex : tuft.vertices) {
        lowest = std::min(lowest, vertex.pos.y);
        highest = std::max(highest, vertex.pos.y);
    }
    checkNear(lowest, 0.0f, 1e-5f, "Pied de la touffe a l'altitude zero");
    checkNear(highest, 1.0f, 1e-5f, "Sommet de la touffe a l'altitude un");

    // L'UV.y porte la hauteur dans le brin : c'est ce que lit le vertex shader
    // pour plier la touffe SANS decoller son pied du sol. Un uv.y non nul au
    // pied ferait glisser la touffe hors de terre a chaque coup de vent.
    bool feetPinned = true;
    bool normalsRaised = true;
    for (const Vertex& vertex : tuft.vertices) {
        if (vertex.pos.y < 0.5f && std::abs(vertex.uv.y) > 1e-5f) {
            feetPinned = false;
        }
        if (vertex.normal.y <= 0.5f) {
            normalsRaised = false;
        }
    }
    check(feetPinned, "uv.y nul au pied de chaque brin");
    // Normale relevee vers le haut : un quad vertical dont la normale serait
    // horizontale ne recevrait presque rien d'une lumiere zenithale.
    check(normalsRaised, "Normales de brin relevees vers le haut");
}

// ---------------------------------------------------------------------------
// PHASE 19 : le maillage d'une tuile est-il vraiment insensible au thread ?
// ---------------------------------------------------------------------------
// La Phase 19 deplace terrainNodeMeshData du thread de rendu vers un petit
// groupe de threads. Tout repose sur UNE affirmation : cette fonction est PURE,
// donc N tuiles maillees en parallele donnent exactement les memes octets que
// les memes N tuiles maillees en sequence.
//
// C'est une affirmation qu'on ne peut PAS verifier en regardant l'image. Deux
// captures du jeu different deja entre deux executions identiques (l'herbe et
// la faune s'animent avec le temps ecoule, et le temps ecoule depend du debit
// d'images, que la phase change justement) : la comparaison de pixels a un
// plancher de bruit de l'ordre du pour-cent, largement au-dessus de ce qu'on
// cherche a detecter. Le seul controle qui tranche est celui-ci, deterministe
// et bit a bit.
//
// Le test compare les SOMMETS octet par octet, et non a une tolerance pres :
// une course sur une donnee partagee ne produirait pas un petit ecart
// numerique, elle produirait un sommet pris a la mauvaise tuile.
void testTileMeshingIsThreadSafe() {
    section("Phase 19 : maillage de tuiles en parallele");

    Heightmap::Params hparams;
    hparams.resolution = 513;   // 512 cellules = 8 x 8 feuilles de 64
    hparams.worldSize = 512.0f;
    hparams.seed = 1907u;
    const Heightmap heightmap(hparams);

    TerrainQuadtree::Params qparams;
    qparams.nodeCells = 64;
    const TerrainQuadtree quadtree(heightmap, qparams);

    // Un lot volontairement plus GROS que le nombre de threads, et melangeant
    // les niveaux : une tuile grossiere lit la heightmap au pas du noeud et
    // touche donc une tout autre partie de la donnee qu'une feuille. Si un etat
    // etait partage quelque part dans terrainNodeMeshData, c'est ce melange qui
    // le ferait sortir.
    std::vector<uint32_t> nodes;
    for (uint32_t index = 0; index < quadtree.nodeCount() && nodes.size() < 40; ++index) {
        nodes.push_back(index);
    }
    check(nodes.size() >= 8, "Phase 19 : assez de tuiles pour un lot de test");

    // --- Reference : tout en sequence, sur ce thread -----------------------
    std::vector<MeshData> reference(nodes.size());
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        reference[k] = terrainNodeMeshData(heightmap, quadtree, nodes[k], false);
    }

    // --- Le meme lot, sur 4 threads, decoupe en tranches contigues ----------
    // Meme decoupage que TerrainStreamer::meshBatch : chaque thread ecrit dans
    // des elements disjoints d'un vecteur deja dimensionne.
    const std::size_t slices = 4;
    std::vector<MeshData> parallel(nodes.size());
    {
        std::vector<std::thread> threads;
        const std::size_t total = nodes.size();
        const auto sliceBegin = [total, slices](std::size_t s) {
            return (total * s) / slices;
        };
        const auto worker = [&](std::size_t begin, std::size_t end) {
            for (std::size_t k = begin; k < end; ++k) {
                parallel[k] = terrainNodeMeshData(heightmap, quadtree, nodes[k], false);
            }
        };
        for (std::size_t s = 1; s < slices; ++s) {
            threads.emplace_back(worker, sliceBegin(s), sliceBegin(s + 1));
        }
        worker(sliceBegin(0), sliceBegin(1));
        for (std::thread& t : threads) {
            t.join();
        }
    }

    bool sameCount = true;
    bool sameBytes = true;
    bool sameNames = true;
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        if (reference[k].vertices.size() != parallel[k].vertices.size()) {
            sameCount = false;
            continue;
        }
        sameNames = sameNames && (reference[k].name == parallel[k].name);
        // memcmp, et non une comparaison champ a champ : on veut que le test
        // echoue aussi sur un champ qu'on aurait oublie d'ajouter ici le jour
        // ou Vertex grandira.
        if (std::memcmp(reference[k].vertices.data(), parallel[k].vertices.data(),
                        reference[k].vertices.size() * sizeof(Vertex)) != 0) {
            sameBytes = false;
        }
    }
    check(sameCount, "Phase 19 : meme nombre de sommets par tuile en parallele");
    check(sameNames, "Phase 19 : meme nom de tuile en parallele");
    check(sameBytes,
          "Phase 19 : sommets BIT A BIT identiques, en sequence et sur 4 threads");

    // Le temoin du test lui-meme : si le lot etait constitue de tuiles toutes
    // identiques, la verification ci-dessus passerait sans rien prouver. On
    // s'assure donc que les tuiles comparees sont bien DIFFERENTES entre elles.
    bool tilesDiffer = false;
    for (std::size_t k = 1; k < reference.size() && !tilesDiffer; ++k) {
        tilesDiffer = std::memcmp(reference[0].vertices.data(),
                                  reference[k].vertices.data(),
                                  reference[0].vertices.size() * sizeof(Vertex)) != 0;
    }
    check(tilesDiffer,
          "Phase 19 : le lot compare contient bien des tuiles distinctes (temoin)");

    // Repetitions : une course se manifeste rarement. Un seul passage qui
    // reussit ne dit presque rien ; c'est la raison d'etre de cette boucle.
    bool stableOverRuns = true;
    for (int attempt = 0; attempt < 12 && stableOverRuns; ++attempt) {
        std::vector<MeshData> again(nodes.size());
        std::vector<std::thread> threads;
        const std::size_t total = nodes.size();
        const auto sliceBegin = [total, slices](std::size_t s) {
            return (total * s) / slices;
        };
        const auto worker = [&](std::size_t begin, std::size_t end) {
            for (std::size_t k = begin; k < end; ++k) {
                again[k] = terrainNodeMeshData(heightmap, quadtree, nodes[k], false);
            }
        };
        for (std::size_t s = 1; s < slices; ++s) {
            threads.emplace_back(worker, sliceBegin(s), sliceBegin(s + 1));
        }
        worker(sliceBegin(0), sliceBegin(1));
        for (std::thread& t : threads) {
            t.join();
        }
        for (std::size_t k = 0; k < nodes.size() && stableOverRuns; ++k) {
            stableOverRuns =
                (again[k].vertices.size() == reference[k].vertices.size()) &&
                std::memcmp(again[k].vertices.data(), reference[k].vertices.data(),
                            reference[k].vertices.size() * sizeof(Vertex)) == 0;
        }
    }
    check(stableOverRuns,
          "Phase 19 : 12 passages paralleles de plus, tous identiques a la reference");
}

void testGrassPhase18() {
    section("Phase 18 : herbe plus dense et brins decoupes");

    // --- Le quad est devenu une PLANCHE a decouper -------------------------
    // La silhouette n'est plus portee par le contour du polygone mais par le
    // masque de shaders/foliage.frag. Le quad doit donc etre large et peu
    // effile, faute de quoi les brins decoupes seraient plus maigres que le
    // ruban de la Phase 11 - c'est-a-dire un recul.
    const MeshData tuft = grassTuftMeshData(3);
    float baseHalfWidth = 0.0f;
    float tipHalfWidth = 0.0f;
    for (const Vertex& vertex : tuft.vertices) {
        const float halfWidth = std::sqrt(vertex.pos.x * vertex.pos.x +
                                          vertex.pos.z * vertex.pos.z);
        if (vertex.pos.y < 0.5f) {
            baseHalfWidth = std::max(baseHalfWidth, halfWidth);
        } else {
            tipHalfWidth = std::max(tipHalfWidth, halfWidth);
        }
    }
    check(baseHalfWidth > 0.35f, "Quad large au pied : le masque a la place de decouper");
    check(tipHalfWidth > 0.25f,
          "Quad a peine effile : c'est le masque, et non le polygone, qui effile le brin");

    // --- Densite ----------------------------------------------------------
    SlopedTerrain terrain(1200.0f, 0.0f);
    const Frustum frustum = permissiveFrustum();
    const glm::vec3 eye(0.0f, 1200.0f, 0.0f);
    ConstantMap dense(1.0f);
    ConstantMap lowCanopy(0.0f);

    VegetationParams phase18;
    VegetationParams phase11 = phase18;
    phase11.grassSlots = 24; // le reglage d'avant la Phase 18

    std::vector<InstanceData> now;
    std::vector<InstanceData> before;
    Vegetation(terrain, &dense, &lowCanopy, 2000.0f, phase18)
        .gatherGrass(eye, frustum, now);
    Vegetation(terrain, &dense, &lowCanopy, 2000.0f, phase11)
        .gatherGrass(eye, frustum, before);
    check(!before.empty(), "Temoin Phase 11 : de l'herbe est bien semee");
    check(now.size() > before.size() * 2,
          "Phase 18 : plus du DOUBLE de touffes sur la meme vue et la meme carte");

    // --- La decoupe alpha ne touche pas au SEMIS --------------------------
    // grassCutout est un parametre de RENDU. S'il changeait le nombre de
    // touffes, le temoin --no-grass-cutout ne mesurerait plus la decoupe seule
    // mais un melange des deux, et la mesure de la phase n'aurait aucune valeur.
    VegetationParams opaque = phase18;
    opaque.grassCutout = 0.0f;
    std::vector<InstanceData> opaqueGrass;
    Vegetation(terrain, &dense, &lowCanopy, 2000.0f, opaque)
        .gatherGrass(eye, frustum, opaqueGrass);
    check(opaqueGrass.size() == now.size(),
          "grassCutout ne change pas d'une touffe le semis (temoin honnete)");

    // --- Le pied des touffes reste SUR le sol ------------------------------
    // La hauteur n'est plus echantillonnee par touffe mais interpolee entre les
    // quatre coins de la cellule. Sur un plan incline - que les deux triangles
    // reproduisent exactement - l'ecart doit etre nul : c'est ce qui prouve que
    // l'optimisation n'a pas decolle l'herbe du terrain.
    SlopedTerrain ramp(1200.0f, 0.35f);
    std::vector<InstanceData> onRamp;
    Vegetation(ramp, &dense, &lowCanopy, 2000.0f, phase18)
        .gatherGrass(glm::vec3(0.0f, 1200.0f, 0.0f), frustum, onRamp);
    check(!onRamp.empty(), "De l'herbe pousse aussi sur le versant");
    float worstGap = 0.0f;
    for (const InstanceData& blade : onRamp) {
        worstGap = std::max(worstGap,
                            std::abs(blade.position.y -
                                     ramp.heightAt(blade.position.x, blade.position.z)));
    }
    checkNear(worstGap, 0.0f, 1e-3f,
              "Pied pose au sol malgre l'interpolation par cellule (versant a 35 %)");
}

void testVegetationPlacement() {
    section("Phase 11 : semis de la vegetation de decor");

    SlopedTerrain terrain(1200.0f, 0.0f); // plat a 1200 m : pente nulle
    const Frustum frustum = permissiveFrustum();
    const glm::vec3 eye(0.0f, 1200.0f, 0.0f);

    VegetationParams params;
    ConstantMap dense(1.0f);
    ConstantMap canopy(20.0f);

    // --- Le couvert commande bien le semis ---------------------------------
    {
        ConstantMap bare(0.0f);
        Vegetation wooded(terrain, &dense, &canopy, 2000.0f, params);
        Vegetation empty(terrain, &bare, &canopy, 2000.0f, params);

        std::vector<InstanceData> woodedTrees;
        std::vector<InstanceData> emptyTrees;
        gatherTreesUntilStable(wooded, eye, frustum, woodedTrees);
        gatherTreesUntilStable(empty, eye, frustum, emptyTrees);

        check(!woodedTrees.empty(), "Couvert a 1 : des arbres sont semes");
        check(emptyTrees.empty(),
              "Couvert a 0 : AUCUN arbre - c'est ce qui garde les lapiaz nus");
    }

    // --- Frontiere de foret : le semis reproduit la carte -------------------
    {
        HalfCoveredMap half;
        Vegetation vegetation(terrain, &half, &canopy, 2000.0f, params);
        std::vector<InstanceData> trees;
        gatherTreesUntilStable(vegetation, eye, frustum, trees);

        check(!trees.empty(), "Carte a deux moities : des arbres sont semes");
        std::size_t west = 0;
        for (const InstanceData& tree : trees) {
            if (tree.position.x < -params.treeCell) {
                ++west;
            }
        }
        check(west == 0, "Aucun arbre du cote NU de la frontiere de couvert");
    }

    // --- Determinisme : meme graine, meme foret ----------------------------
    {
        Vegetation a(terrain, &dense, &canopy, 2000.0f, params);
        Vegetation b(terrain, &dense, &canopy, 2000.0f, params);
        std::vector<InstanceData> first;
        std::vector<InstanceData> second;
        gatherTreesUntilStable(a, eye, frustum, first);
        gatherTreesUntilStable(b, eye, frustum, second);

        check(first.size() == second.size(), "Meme graine : meme nombre d'arbres");
        bool identical = first.size() == second.size();
        for (std::size_t i = 0; identical && i < first.size(); ++i) {
            identical = first[i].position == second[i].position &&
                        first[i].scale == second[i].scale &&
                        first[i].yaw == second[i].yaw;
        }
        check(identical, "Meme graine : arbres identiques au bit pres");
    }

    // --- Le semis ne depend PAS du chemin parcouru -------------------------
    // C'est la propriete qui autorise a ne rien stocker pour les 25 km2 : une
    // cellule decouverte de loin puis approchee doit donner les memes arbres
    // qu'une cellule decouverte directement au contact. Un generateur A ETAT
    // (mt19937) echouerait ici ; un hachage passe.
    {
        Vegetation direct(terrain, &dense, &canopy, 2000.0f, params);
        Vegetation wandered(terrain, &dense, &canopy, 2000.0f, params);

        std::vector<InstanceData> scratch;
        gatherTreesUntilStable(wandered, { 700.0f, 1200.0f, -400.0f }, frustum, scratch);
        gatherTreesUntilStable(wandered, { -900.0f, 1200.0f, 850.0f }, frustum, scratch);
        gatherTreesUntilStable(wandered, { 120.0f, 1200.0f, 60.0f }, frustum, scratch);

        std::vector<InstanceData> a;
        std::vector<InstanceData> b;
        gatherTreesUntilStable(direct, eye, frustum, a);
        gatherTreesUntilStable(wandered, eye, frustum, b);

        check(a.size() == b.size(),
              "Semis independant du chemin : meme nombre d'arbres");
        bool identical = a.size() == b.size();
        for (std::size_t i = 0; identical && i < a.size(); ++i) {
            identical = a[i].position == b[i].position && a[i].scale == b[i].scale;
        }
        check(identical, "Semis independant du chemin : arbres identiques");
    }

    // --- Rien hors des donnees ---------------------------------------------
    {
        const float extent = 300.0f;
        Vegetation vegetation(terrain, &dense, &canopy, extent, params);
        std::vector<InstanceData> trees;
        gatherTreesUntilStable(vegetation, eye, frustum, trees);
        bool inside = true;
        for (const InstanceData& tree : trees) {
            inside = inside && std::abs(tree.position.x) <= extent &&
                     std::abs(tree.position.z) <= extent;
        }
        check(inside, "Aucun arbre au-dela de l'emprise du terrain");
    }

    // --- Niveau de detail de DENSITE ---------------------------------------
    // Les arbres proches doivent etre plus nombreux, PAR UNITE DE SURFACE, que
    // les arbres lointains : c'est tout l'objet de la portee par emplacement.
    {
        Vegetation vegetation(terrain, &dense, &canopy, 4000.0f, params);
        std::vector<InstanceData> trees;
        gatherTreesUntilStable(vegetation, eye, frustum, trees);

        std::size_t near = 0;
        std::size_t far = 0;
        for (const InstanceData& tree : trees) {
            const float d = glm::length(tree.position - eye);
            if (d < 200.0f) {
                ++near;
            } else if (d > 600.0f && d < 1000.0f) {
                ++far;
            }
        }
        // On compare des DENSITES et non des comptes bruts : la couronne
        // lointaine est bien plus vaste que le disque proche.
        const double nearArea = 3.14159 * 200.0 * 200.0;
        const double farArea = 3.14159 * (1000.0 * 1000.0 - 600.0 * 600.0);
        const double nearDensity = static_cast<double>(near) / nearArea;
        const double farDensity = static_cast<double>(far) / farArea;
        check(near > 0 && far > 0, "Des arbres proches ET lointains");
        check(nearDensity > farDensity * 1.5,
              "Niveau de detail de densite : la foret s'eclaircit avec la distance");
    }

    // --- Attenuation : les plus lointains retrecissent ---------------------
    {
        Vegetation vegetation(terrain, &dense, &canopy, 4000.0f, params);
        std::vector<InstanceData> trees;
        gatherTreesUntilStable(vegetation, eye, frustum, trees);
        bool fadeInRange = true;
        bool someFaded = false;
        for (const InstanceData& tree : trees) {
            fadeInRange = fadeInRange && tree.fade >= 0.0f && tree.fade <= 1.0f;
            if (tree.fade < 0.999f) {
                someFaded = true;
            }
        }
        check(fadeInRange, "Attenuation toujours dans [0,1]");
        check(someFaded, "Certains arbres lointains sont attenues");
    }

    // --- Substitut sans carte LiDAR ----------------------------------------
    // Sans carte, le semis doit encore produire quelque chose de plausible et
    // refuser les hautes altitudes (limite des arbres).
    {
        SlopedTerrain low(1200.0f, 0.0f);
        SlopedTerrain high(2100.0f, 0.0f);
        Vegetation belowTreeline(low, nullptr, nullptr, 2000.0f, params);
        Vegetation aboveTreeline(high, nullptr, nullptr, 2000.0f, params);

        std::vector<InstanceData> lowTrees;
        std::vector<InstanceData> highTrees;
        gatherTreesUntilStable(belowTreeline, { 0.0f, 1200.0f, 0.0f }, frustum, lowTrees);
        gatherTreesUntilStable(aboveTreeline, { 0.0f, 2100.0f, 0.0f }, frustum, highTrees);

        check(!lowTrees.empty(), "Substitut : des arbres sous la limite des arbres");
        check(highTrees.empty(), "Substitut : aucun arbre au-dessus de 1950 m");
    }

    // --- Pente : pas d'arbre sur une paroi ---------------------------------
    {
        SlopedTerrain cliff(1200.0f, 3.0f); // 3 m d'altitude par metre parcouru
        Vegetation vegetation(cliff, &dense, &canopy, 2000.0f, params);
        std::vector<InstanceData> trees;
        gatherTreesUntilStable(vegetation, { 0.0f, 1200.0f, 0.0f }, frustum, trees);
        check(trees.empty(), "Aucun arbre sur une pente de 3 pour 1");
    }
}

void testGrassPlacement() {
    section("Phase 11 : semis de l'herbe");

    const Frustum frustum = permissiveFrustum();
    VegetationParams params;
    ConstantMap cover(0.5f);
    ConstantMap noCanopy(0.0f);

    // --- L'altitude commande la TAILLE et la DENSITE ------------------------
    // C'est la demande explicite de la tache 3 : plus haut = plus petit et plus
    // rare (pelouse alpine rase).
    auto sampleAt = [&](float altitude, std::vector<InstanceData>& out) {
        SlopedTerrain terrain(altitude, 0.0f);
        Vegetation vegetation(terrain, &cover, &noCanopy, 4000.0f, params);
        vegetation.gatherGrass({ 0.0f, altitude, 0.0f }, frustum, out);
    };

    std::vector<InstanceData> low;
    std::vector<InstanceData> mid;
    std::vector<InstanceData> high;
    sampleAt(1200.0f, low);   // sous grassAltitudeLush : pleine densite
    sampleAt(2050.0f, mid);   // entre les deux seuils
    sampleAt(2400.0f, high);  // au-dessus de grassAltitudeBarren

    check(!low.empty(), "De l'herbe en dessous de la limite d'alpage");
    check(high.empty(), "Aucune herbe au-dessus de l'altitude sterile");
    check(mid.size() < low.size(), "L'herbe se rarefie avec l'altitude");

    auto meanScale = [](const std::vector<InstanceData>& tufts) {
        double total = 0.0;
        for (const InstanceData& tuft : tufts) {
            total += tuft.scale;
        }
        return tufts.empty() ? 0.0 : total / static_cast<double>(tufts.size());
    };
    check(meanScale(mid) < meanScale(low), "Les touffes rapetissent avec l'altitude");
    check(meanScale(low) < params.grassSizeLow * 1.3,
          "Taille des touffes coherente avec grassSizeLow");

    // --- Portee COURTE ------------------------------------------------------
    {
        SlopedTerrain terrain(1200.0f, 0.0f);
        Vegetation vegetation(terrain, &cover, &noCanopy, 4000.0f, params);
        std::vector<InstanceData> tufts;
        const glm::vec3 eye(0.0f, 1200.0f, 0.0f);
        vegetation.gatherGrass(eye, frustum, tufts);
        float furthest = 0.0f;
        for (const InstanceData& tuft : tufts) {
            furthest = std::max(furthest, glm::length(tuft.position - eye));
        }
        check(furthest <= params.grassDistanceFar + 0.001f,
              "Aucune touffe au-dela de la portee de rendu");
        check(furthest > 0.5f * params.grassDistanceFar,
              "L'herbe occupe bien sa portee");
    }

    // --- Le placement n'est PAS une grille reguliere ------------------------
    // Le risque explicitement a eviter. Un semis regulier donnerait des
    // distances au plus proche voisin toutes egales ; un semis groupe donne une
    // large dispersion. On compare l'ecart-type a la moyenne.
    {
        SlopedTerrain terrain(1200.0f, 0.0f);
        Vegetation vegetation(terrain, &cover, &noCanopy, 4000.0f, params);
        std::vector<InstanceData> tufts;
        vegetation.gatherGrass({ 0.0f, 1200.0f, 0.0f }, frustum, tufts);

        check(tufts.size() > 200, "Assez de touffes pour juger de la repartition");

        // Distance au plus proche voisin, sur un echantillon (le calcul est
        // quadratique).
        const std::size_t sampleCount =
            std::min<std::size_t>(150, tufts.size());
        std::vector<float> nearest;
        nearest.reserve(sampleCount);
        for (std::size_t i = 0; i < sampleCount; ++i) {
            float best = 1.0e9f;
            for (std::size_t j = 0; j < tufts.size(); ++j) {
                if (i == j) {
                    continue;
                }
                best = std::min(best, horizontalDistance(tufts[i].position,
                                                         tufts[j].position));
            }
            nearest.push_back(best);
        }
        double mean = 0.0;
        for (float value : nearest) {
            mean += value;
        }
        mean /= static_cast<double>(nearest.size());
        double variance = 0.0;
        for (float value : nearest) {
            variance += (value - mean) * (value - mean);
        }
        variance /= static_cast<double>(nearest.size());
        const double deviation = std::sqrt(variance);

        // Sur une grille PARFAITE toutes les distances seraient egales et
        // l'ecart-type nul. On exige une dispersion d'au moins 25 % de la
        // moyenne : c'est la signature d'un placement groupe.
        check(deviation > 0.25 * mean,
              "Placement groupe et non regulier (dispersion du plus proche voisin)");
    }

    // --- Determinisme -------------------------------------------------------
    {
        SlopedTerrain terrain(1200.0f, 0.0f);
        Vegetation a(terrain, &cover, &noCanopy, 4000.0f, params);
        Vegetation b(terrain, &cover, &noCanopy, 4000.0f, params);
        std::vector<InstanceData> first;
        std::vector<InstanceData> second;
        a.gatherGrass({ 12.0f, 1200.0f, -7.0f }, frustum, first);
        b.gatherGrass({ 12.0f, 1200.0f, -7.0f }, frustum, second);
        check(first.size() == second.size() && !first.empty(),
              "Meme graine : meme herbe");
    }
}

void testInstanceLayout() {
    section("Phase 11 : format d'instance");

    // Ces proprietes sont SUPPOSEES par VertexFormat.hpp, qui lit (scale, yaw)
    // et (variation, fade) comme des vec2. Un membre insere entre les deux
    // ferait lire n'importe quoi par le GPU, en silence.
    check(sizeof(InstanceData) == 28, "InstanceData tient en 28 octets");
    check(offsetof(InstanceData, yaw) == offsetof(InstanceData, scale) + sizeof(float),
          "scale et yaw contigus");
    check(offsetof(InstanceData, fade) == offsetof(InstanceData, variation) + sizeof(float),
          "variation et fade contigus");

    // Valeurs par defaut NEUTRES : une instance non renseignee doit etre visible
    // a sa taille normale, pas invisible.
    const InstanceData fresh;
    checkNear(fresh.scale, 1.0f, 1e-6f, "Echelle par defaut a 1");
    checkNear(fresh.fade, 1.0f, 1e-6f, "Attenuation par defaut a 1");
}

} // namespace

// ---------------------------------------------------------------------------
// Phase 12 : chaine de peres du quadtree et repli sur l'ancetre
// ---------------------------------------------------------------------------
// Le repli sur l'ancetre vit dans TerrainStreamer, qui pilote le Renderer donc
// Vulkan : il n'est pas testable ici. Ce qui l'EST, et qui est ce dont il
// depend entierement, c'est la structure sur laquelle il s'appuie - la chaine de
// peres et la description d'un noeud hors selection. Une erreur a ce niveau
// (chainage casse par une reallocation de nodes_, boucle, morphing different de
// celui de la selection) donnerait un repli silencieusement faux.
void testQuadtreeAncestry() {
    section("Phase 12 : chaine de peres et repli sur l'ancetre");

    Heightmap::Params params;
    params.resolution = 513;
    params.worldSize = 512.0f;
    params.seed = 4242u;
    Heightmap heightmap(params);

    TerrainQuadtree quadtree(heightmap);

    // --- La chaine de peres est complete et coherente ------------------------
    {
        uint32_t roots = 0;
        bool levelsIncrease = true;
        bool childOfItsParent = true;
        uint32_t maxDepth = 0;

        for (uint32_t index = 0; index < quadtree.nodeCount(); ++index) {
            const int32_t parent = quadtree.parentIndex(index);
            if (parent < 0) {
                ++roots;
                continue;
            }
            const TerrainQuadtree::Node& node = quadtree.node(index);
            const TerrainQuadtree::Node& up = quadtree.node(static_cast<uint32_t>(parent));

            // Le pere est exactement un niveau au-dessus.
            if (up.level != node.level + 1) {
                levelsIncrease = false;
            }
            // ... et il reconnait bien ce fils. C'est ce test qui attraperait un
            // chainage pose sur une reference devenue pendante apres
            // reallocation de nodes_ pendant la construction recursive.
            bool found = false;
            for (int i = 0; i < 4; ++i) {
                if (up.children[i] == static_cast<int32_t>(index)) {
                    found = true;
                }
            }
            if (!found) {
                childOfItsParent = false;
            }

            // Remontee : elle doit se terminer, et sur la racine.
            uint32_t depth = 0;
            int32_t current = parent;
            while (current >= 0 && depth <= quadtree.rootLevel() + 1u) {
                current = quadtree.parentIndex(static_cast<uint32_t>(current));
                ++depth;
            }
            maxDepth = std::max(maxDepth, depth);
        }

        check(roots == 1, "Chaine de peres : un seul noeud sans pere (la racine)");
        check(levelsIncrease, "Chaine de peres : le pere est toujours au niveau du dessus");
        check(childOfItsParent, "Chaine de peres : chaque noeud est bien un fils de son pere");
        check(maxDepth <= quadtree.rootLevel(),
              "Chaine de peres : la remontee se termine, en au plus rootLevel etapes");
    }

    // --- La boite d'un ancetre contient bien celle de ses descendants --------
    // C'est LA propriete qui rend le repli correct : dessiner l'ancetre a la
    // place d'un descendant couvre au moins la meme surface.
    {
        bool contains = true;
        for (uint32_t index = 0; index < quadtree.nodeCount(); ++index) {
            const int32_t parent = quadtree.parentIndex(index);
            if (parent < 0) {
                continue;
            }
            const TerrainQuadtree::Node& node = quadtree.node(index);
            const TerrainQuadtree::Node& up = quadtree.node(static_cast<uint32_t>(parent));
            // Emprise horizontale seulement : c'est la couverture du sol qui
            // compte pour un trou, et les bornes verticales remontent deja des
            // fils par construction.
            if (node.boundsMin.x < up.boundsMin.x || node.boundsMin.z < up.boundsMin.z ||
                node.boundsMax.x > up.boundsMax.x || node.boundsMax.z > up.boundsMax.z) {
                contains = false;
            }
        }
        check(contains,
              "Repli : l'emprise d'un pere contient celle de chacun de ses fils");
    }

    // --- describeNode redit exactement ce que dit la selection ---------------
    // Un ancetre dessine par repli doit recevoir la MEME bande de geomorphing
    // que s'il avait ete selectionne normalement, sinon la geometrie sauterait
    // au moment ou le repli cesse.
    {
        const glm::vec3 eye(3.0f, 40.0f, 7.0f);
        const Frustum everything = Frustum::fromViewProjection(glm::mat4(0.0f));
        std::vector<TerrainQuadtree::SelectedNode> selection;
        quadtree.select(eye, everything, selection);

        check(!selection.empty(), "Repli : la selection de reference n'est pas vide");

        bool identical = true;
        for (const TerrainQuadtree::SelectedNode& selected : selection) {
            const TerrainQuadtree::SelectedNode described =
                quadtree.describeNode(selected.nodeIndex, eye);
            if (described.level != selected.level ||
                described.morphStart != selected.morphStart ||
                described.morphEnd != selected.morphEnd ||
                described.distance != selected.distance) {
                identical = false;
            }
        }
        check(identical,
              "Repli : describeNode redonne au bit pres ce que produit select()");

        // Et pour un noeud que la selection n'a PAS retenu (un ancetre, justement),
        // la bande de morphing suit bien la portee de SON niveau.
        const uint32_t leaf = selection.front().nodeIndex;
        const int32_t up = quadtree.parentIndex(leaf);
        check(up >= 0, "Repli : la tuile la plus proche a bien un pere");
        if (up >= 0) {
            const TerrainQuadtree::SelectedNode described =
                quadtree.describeNode(static_cast<uint32_t>(up), eye);
            checkNear(described.morphStart,
                      quadtree.range(described.level) * quadtree.params().morphStartRatio,
                      1e-3f, "Repli : la bande de morphing de l'ancetre suit SON niveau");
            check(described.level == quadtree.node(leaf).level + 1,
                  "Repli : l'ancetre immediat est bien un niveau plus grossier");
        }
    }

    // --- Le sommet du quadtree est peu peuple --------------------------------
    // Le TerrainStreamer epingle les trois niveaux les plus grossiers pour que le
    // repli soit toujours disponible. Ce n'est defendable que si ces niveaux
    // comptent une poignee de tuiles : ce test fige cette hypothese.
    {
        uint32_t top = 0;
        const int lowest = std::max(0, static_cast<int>(quadtree.rootLevel()) - 2);
        for (uint32_t index = 0; index < quadtree.nodeCount(); ++index) {
            if (static_cast<int>(quadtree.node(index).level) >= lowest) {
                ++top;
            }
        }
        check(top <= 21,
              "Sommet epingle : au plus 21 tuiles sur les trois niveaux les plus grossiers");
        check(top >= 1, "Sommet epingle : au moins la racine");
    }
}

// ===========================================================================
// PHASE 15 : editeur de niveau
// ===========================================================================
// Ce que ce bloc couvre, et pourquoi il peut le couvrir sans GPU : TOUT
// l'editeur sauf le dessin. Le catalogue produit des MeshData, la scene et le
// controleur ne dependent que de GLM, le format de scene ne depend que de la
// bibliotheque standard, et le raycast terrain ne connait que HeightSampler.
// C'est le meme decoupage qui avait permis de tester la physique du parapente,
// du velo et de l'escalade sans fenetre.
//
// Ce qui n'est PAS couvert ici et ne peut pas l'etre : le fantome REELLEMENT
// dessine a l'ecran, la teinte de temoin, et la lecture du clavier.

// Terrain georeference et parfaitement previsible : l'altitude ne depend que de
// X et vaut 400 + (x + 50), donc 400 au bord ouest et 500 au bord est.
//
// Construit par le constructeur "grille toute faite" - celui par lequel entrent
// les dalles LiDAR - et non par le generateur de bruit : les tests de scene
// verifient des CONVERSIONS DE COORDONNEES au millimetre, ce qui exige de savoir
// exactement quelle altitude repond le terrain, y compris apres un changement de
// resolution.
Heightmap makeGeoTerrain(uint32_t resolution, float spacing) {
    std::vector<float> heights(static_cast<std::size_t>(resolution) * resolution, 0.0f);
    for (uint32_t iz = 0; iz < resolution; ++iz) {
        for (uint32_t ix = 0; ix < resolution; ++ix) {
            heights[static_cast<std::size_t>(iz) * resolution + ix] =
                400.0f + static_cast<float>(ix) * spacing;
        }
    }
    GeoReference geo;
    geo.originEast = 907000.0;
    geo.originNorth = 6440000.0;
    geo.epsg = 2154;
    geo.absoluteAltitude = true;
    return Heightmap(std::move(heights), resolution, spacing, "terrain de test", geo);
}

// Ecrit un .obj minimal (tetraedre), pour exercer le SCAN du catalogue par le
// vrai loader de la Phase 3 plutot que par un substitut.
void writeTestObj(const std::filesystem::path& path, float scale) {
    std::ofstream file(path);
    file << "# asset de test\n"
         << "v 0 0 0\n"
         << "v " << scale << " 0 0\n"
         << "v 0 0 " << scale << "\n"
         << "v 0 " << scale << " 0\n"
         << "f 1 2 3\n"
         << "f 1 2 4\n"
         << "f 2 3 4\n"
         << "f 1 3 4\n";
}

// Boite englobante d'un MeshData, pour verifier la convention d'ancrage.
void meshBounds(const MeshData& mesh, glm::vec3* minOut, glm::vec3* maxOut) {
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (const Vertex& vertex : mesh.vertices) {
        lo = glm::min(lo, vertex.pos);
        hi = glm::max(hi, vertex.pos);
    }
    *minOut = lo;
    *maxOut = hi;
}

void testEditorAssets() {
    section("Phase 15 : assets generes et convention d'ancrage");

    check(builtinEditorAssets().size() >= 5,
          "Assets : au moins cinq modeles generes en code");

    for (const BuiltinAsset& builtin : builtinEditorAssets()) {
        const MeshData mesh = builtin.build();
        check(!mesh.empty(), "Asset \"" + builtin.id + "\" : geometrie non vide");
        check(mesh.indices.size() % 3 == 0,
              "Asset \"" + builtin.id + "\" : indices multiples de 3");

        glm::vec3 lo(0.0f);
        glm::vec3 hi(0.0f);
        meshBounds(mesh, &lo, &hi);

        // Le PIED est a zero : c'est ce qui rend "poser au point vise" exact.
        checkNear(lo.y, 0.0f, 1e-4f, "Asset \"" + builtin.id + "\" : pied a y = 0");
        check(hi.y > 0.05f, "Asset \"" + builtin.id + "\" : hauteur non nulle");

        // L'ancrage tombe DANS l'empreinte horizontale, pas a cote : un objet
        // pose entoure le point vise.
        //
        // Volontairement plus faible qu'un centrage exact sur la boite
        // englobante. Le poteau de balisage a son MAT a l'origine et sa plaque
        // qui deborde vers +X : sa boite n'est pas centree, et elle ne doit pas
        // l'etre - planter un panneau, c'est planter le mat au point vise, pas y
        // placer le milieu de l'ensemble mat + panneau. Voir la note
        // "ancrage n'est pas centrage" dans editor/EditorAssets.hpp.
        check(lo.x <= 1e-4f && hi.x >= -1e-4f,
              "Asset \"" + builtin.id + "\" : ancrage dans l'empreinte en X");
        check(lo.z <= 1e-4f && hi.z >= -1e-4f,
              "Asset \"" + builtin.id + "\" : ancrage dans l'empreinte en Z");
        // Et pas au bord non plus : l'ancrage reste dans la moitie centrale de
        // l'empreinte, sinon l'objet se poserait visiblement de travers.
        check(std::abs(0.5f * (lo.x + hi.x)) <= 0.5f * std::max(std::abs(lo.x), hi.x),
              "Asset \"" + builtin.id + "\" : ancrage pas au bord en X");
        check(std::abs(0.5f * (lo.z + hi.z)) <= 0.5f * std::max(std::abs(lo.z), hi.z),
              "Asset \"" + builtin.id + "\" : ancrage pas au bord en Z");

        // Toutes les normales sont unitaires : une normale nulle donnerait un
        // triangle noir, et le facettage en produit une par triangle.
        bool normalsOk = true;
        for (const Vertex& vertex : mesh.vertices) {
            normalsOk = normalsOk && std::abs(glm::length(vertex.normal) - 1.0f) < 1e-3f;
        }
        check(normalsOk, "Asset \"" + builtin.id + "\" : normales unitaires");
    }

    // DETERMINISME. Les rochers sont deformes par HACHAGE de l'indice de sommet
    // et non par un generateur a etat (meme lecon qu'en Phase 11) : deux appels
    // doivent donner la meme geometrie au bit pres. Ca compte parce que
    // l'empreinte de COLLISION est deduite de la boite englobante du mesh - une
    // geometrie qui varierait donnerait une collision qui varie, et une scene
    // sauvegardee ne se rechargerait pas a l'identique.
    const MeshData first = editorBoulderMeshData();
    const MeshData second = editorBoulderMeshData();
    bool identical = first.vertices.size() == second.vertices.size();
    for (std::size_t i = 0; identical && i < first.vertices.size(); ++i) {
        identical = std::memcmp(&first.vertices[i].pos, &second.vertices[i].pos,
                                sizeof(glm::vec3)) == 0;
    }
    check(identical, "Assets : le rocher est identique au bit pres a chaque appel");
}

void testAssetCatalog() {
    section("Phase 15 : catalogue d'assets");

    {
        AssetCatalog catalog;
        catalog.addBuiltins();
        check(catalog.size() == builtinEditorAssets().size(),
              "Catalogue : tous les builtins ajoutes");
        check(catalog.indexOf("rocher") >= 0, "Catalogue : \"rocher\" retrouve par nom");
        check(catalog.indexOf("inexistant") < 0, "Catalogue : nom inconnu -> -1");

        const AssetEntry& boulder =
            catalog.entries()[static_cast<std::size_t>(catalog.indexOf("rocher"))];
        check(boulder.height > 0.5f && boulder.height < 4.0f,
              "Catalogue : hauteur du rocher plausible");
        // Cylindre CONTENANT : le rayon est la demi-diagonale horizontale, donc
        // au moins aussi grand que la plus grande demi-etendue.
        check(boulder.collisionRadius >=
                  std::max(boulder.halfExtentX, boulder.halfExtentZ) - 1e-4f,
              "Catalogue : le rayon de collision contient la boite");
        check(boulder.sourcePath.empty(), "Catalogue : un builtin n'a pas de fichier source");

        // Deuxieme appel : les identifiants sont deja pris, rien ne doit doubler.
        catalog.addBuiltins();
        check(catalog.size() == builtinEditorAssets().size(),
              "Catalogue : les doublons d'identifiant sont refuses");
    }

    // --- sanitizeAssetId ----------------------------------------------------
    check(sanitizeAssetId("Gros Rocher") == "gros_rocher",
          "Catalogue : espaces et majuscules normalises");
    check(sanitizeAssetId("mur-de-pierre.v2") == "mur-de-pierre.v2",
          "Catalogue : tiret et point conserves");
    check(sanitizeAssetId("") == "asset", "Catalogue : identifiant vide remplace");

    // --- normalizeAssetMesh -------------------------------------------------
    {
        MeshData mesh;
        for (const glm::vec3 position : { glm::vec3(10.0f, 5.0f, -3.0f),
                                          glm::vec3(12.0f, 9.0f, -1.0f),
                                          glm::vec3(11.0f, 5.0f, -2.0f) }) {
            Vertex vertex{};
            vertex.pos = position;
            vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            mesh.vertices.push_back(vertex);
        }
        mesh.indices = { 0, 1, 2 };
        normalizeAssetMesh(mesh);

        glm::vec3 lo(0.0f);
        glm::vec3 hi(0.0f);
        meshBounds(mesh, &lo, &hi);
        checkNear(lo.y, 0.0f, 1e-5f, "Normalisation : pied ramene a y = 0");
        checkNear(0.5f * (lo.x + hi.x), 0.0f, 1e-5f, "Normalisation : recentre en X");
        checkNear(0.5f * (lo.z + hi.z), 0.0f, 1e-5f, "Normalisation : recentre en Z");
        // La FORME ne bouge pas : seule l'origine change.
        checkNear(hi.y - lo.y, 4.0f, 1e-5f, "Normalisation : hauteur inchangee");
    }

    // --- scanDirectory ------------------------------------------------------
    {
        namespace fs = std::filesystem;
        const fs::path directory =
            fs::temp_directory_path() / "vercora_p15_catalogue";
        std::error_code ec;
        fs::remove_all(directory, ec);
        fs::create_directories(directory, ec);

        writeTestObj(directory / "b_deuxieme.obj", 2.0f);
        writeTestObj(directory / "a_premier.obj", 1.0f);
        // Un fichier illisible ne doit priver l'editeur d'AUCUN autre modele.
        {
            std::ofstream broken(directory / "c_casse.obj");
            broken << "ceci n'est pas un maillage\nf 1 2 3\n";
        }
        // Une extension non reconnue est simplement ignoree.
        {
            std::ofstream note(directory / "notes.txt");
            note << "rien a voir";
        }

        AssetCatalog catalog;
        catalog.addBuiltins();
        const std::size_t builtinCount = catalog.size();
        const uint32_t added = catalog.scanDirectory(directory.string());

        check(added == 2, "Catalogue : deux modeles valides charges sur trois .obj");
        check(catalog.size() == builtinCount + 2,
              "Catalogue : le fichier casse n'a pas interrompu le scan");
        check(catalog.indexOf("a_premier") >= 0, "Catalogue : identifiant tire du nom de fichier");
        check(catalog.indexOf("notes") < 0, "Catalogue : extension non reconnue ignoree");
        // ORDRE : les fichiers sont tries par nom. Les touches 1..9 de l'editeur
        // designent des rangs dans cette liste, et directory_iterator ne garantit
        // aucun ordre - sans le tri, la meme touche poserait un modele different
        // d'une execution a l'autre.
        check(catalog.indexOf("a_premier") < catalog.indexOf("b_deuxieme"),
              "Catalogue : les fichiers scannes sont tries par nom");
        // Le scan applique la convention d'ancrage a un fichier qui ne la
        // respectait pas (le tetraedre a son origine dans un coin).
        const AssetEntry& scanned =
            catalog.entries()[static_cast<std::size_t>(catalog.indexOf("b_deuxieme"))];
        glm::vec3 lo(0.0f);
        glm::vec3 hi(0.0f);
        meshBounds(scanned.mesh, &lo, &hi);
        checkNear(lo.y, 0.0f, 1e-4f, "Catalogue : modele scanne repose sur y = 0");
        checkNear(0.5f * (lo.x + hi.x), 0.0f, 1e-4f, "Catalogue : modele scanne recentre");
        check(!scanned.sourcePath.empty(), "Catalogue : chemin d'origine conserve");

        // Un dossier absent n'est pas une erreur : l'editeur doit demarrer avec
        // les seuls builtins.
        AssetCatalog empty;
        empty.addBuiltins();
        check(empty.scanDirectory((directory / "nexiste_pas").string()) == 0,
              "Catalogue : dossier absent -> zero modele, pas d'erreur");
        check(empty.size() == builtinCount, "Catalogue : builtins intacts");

        fs::remove_all(directory, ec);
    }
}

void testTerrainRaycast() {
    section("Phase 15 : intersection rayon / terrain");

    {
        const FlatTerrain flat(0.0f);
        // Oeil a 10 m, regard a 45 degres vers le bas : la surface est atteinte
        // a 10 m devant, donc a 10*sqrt(2) le long du rayon.
        const glm::vec3 eye(0.0f, 10.0f, 0.0f);
        const glm::vec3 direction = glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f));
        const TerrainRayHit hit = raycastTerrain(flat, eye, direction, 400.0f, 1000.0f);
        check(hit.hit, "Raycast : la surface plate est touchee");
        checkNear(hit.distance, 14.1421f, 0.01f, "Raycast : distance le long du rayon");
        checkNear(hit.position.z, -10.0f, 0.01f, "Raycast : point d'impact en Z");
        checkNear(hit.position.y, 0.0f, 1e-3f, "Raycast : le point est SUR la surface");
    }

    {
        // Regard vers le HAUT : rien a toucher. C'est ce qui fait que viser le
        // ciel ne pose aucun objet, au lieu d'en poser un a distance arbitraire.
        const FlatTerrain flat(0.0f);
        const TerrainRayHit hit =
            raycastTerrain(flat, glm::vec3(0.0f, 10.0f, 0.0f),
                           glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f)), 400.0f, 1000.0f);
        check(!hit.hit, "Raycast : un regard vers le ciel ne touche rien");
    }

    {
        // L'EMPRISE compte. heightAt est TOTALE par contrat (hors donnees elle
        // prolonge le bord), donc sans halfExtent un rayon partant vers
        // l'exterieur accrocherait une etagere plate infinie.
        const FlatTerrain flat(0.0f);
        const glm::vec3 eye(0.0f, 10.0f, 0.0f);
        const glm::vec3 direction = glm::normalize(glm::vec3(0.0f, -0.05f, -1.0f));
        const TerrainRayHit bounded =
            raycastTerrain(flat, eye, direction, 400.0f, 50.0f);
        const TerrainRayHit unbounded =
            raycastTerrain(flat, eye, direction, 400.0f, 0.0f);
        check(!bounded.hit, "Raycast : hors emprise -> aucun impact");
        check(unbounded.hit, "Raycast : sans emprise declaree, le bord prolonge touche");
    }

    {
        // Oeil DEJA sous la surface (vol libre dans le relief) : on rend le point
        // de depart projete sur la surface plutot que rien du tout.
        const FlatTerrain flat(100.0f);
        const TerrainRayHit hit =
            raycastTerrain(flat, glm::vec3(3.0f, 90.0f, 4.0f),
                           glm::vec3(0.0f, 0.0f, -1.0f), 400.0f, 1000.0f);
        check(hit.hit, "Raycast : oeil sous la surface -> impact");
        checkNear(hit.distance, 0.0f, 1e-5f, "Raycast : impact a distance nulle");
        checkNear(hit.position.y, 100.0f, 1e-4f, "Raycast : projete sur la surface");
    }

    {
        // Pente reelle : le point rendu doit etre sur la surface, quelle que
        // soit son inclinaison.
        const SlopedTerrain slope(0.0f, 0.40f); // 40 % vers +X
        const glm::vec3 eye(-30.0f, 20.0f, 0.0f);
        const glm::vec3 direction = glm::normalize(glm::vec3(1.0f, -0.35f, 0.0f));
        const TerrainRayHit hit = raycastTerrain(slope, eye, direction, 400.0f, 1000.0f);
        check(hit.hit, "Raycast : la pente est touchee");
        checkNear(hit.position.y, slope.heightAt(hit.position.x, hit.position.z), 1e-2f,
                  "Raycast : le point d'impact est sur la pente");
    }

    {
        // Normale de surface : sur une pente de 40 % vers +X, elle penche vers
        // l'aval (composante X negative) et reste unitaire.
        const SlopedTerrain slope(0.0f, 0.40f);
        const glm::vec3 normal = terrainNormalAt(slope, 0.0f, 0.0f);
        checkNear(glm::length(normal), 1.0f, 1e-4f, "Normale de terrain : unitaire");
        check(normal.x < -0.3f, "Normale de terrain : penchee vers l'aval");
        checkNear(normal.z, 0.0f, 1e-4f, "Normale de terrain : rien selon Z sur cette pente");
    }
}

void testEditorScene() {
    section("Phase 15 : scene editable, selection et annulation");

    AssetCatalog catalog;
    catalog.addBuiltins();
    EditorScene scene(&catalog);

    const auto makeObject = [](const char* asset, float x, float z) {
        PlacedObject object;
        object.asset = asset;
        object.position = { x, 0.0f, z };
        return object;
    };

    const uint32_t a = scene.place(makeObject("rocher", 0.0f, 0.0f));
    const uint32_t b = scene.place(makeObject("cairn", 10.0f, 0.0f));
    const uint32_t c = scene.place(makeObject("poteau", 20.0f, 0.0f));
    check(scene.size() == 3, "Scene : trois objets poses");
    check(a != b && b != c, "Scene : les identifiants sont distincts");
    check(scene.find(b) != nullptr && scene.find(b)->asset == "cairn",
          "Scene : retrouve un objet par identifiant");
    check(scene.find(9999) == nullptr, "Scene : identifiant inconnu -> nullptr");

    // --- Suppression au MILIEU, puis annulation --------------------------
    // Le cas qui distingue une pile d'annulation correcte d'une pile naive :
    // l'objet doit reprendre SA PLACE dans la liste, pas atterrir a la fin.
    check(scene.erase(b), "Scene : suppression d'un objet du milieu");
    check(scene.size() == 2, "Scene : il en reste deux");
    check(scene.undo(), "Scene : annulation de la suppression");
    check(scene.size() == 3, "Scene : l'objet est revenu");
    check(scene.objects()[1].id == b, "Scene : il a repris SON indice, pas la fin");

    // --- Annulation d'une pose ------------------------------------------
    check(scene.undo(), "Scene : annulation de la pose du poteau");
    check(scene.size() == 2 && scene.find(c) == nullptr,
          "Scene : le poteau a disparu");

    // --- Regroupement des retouches -------------------------------------
    // Vingt petites rotations dans la meme seconde ne doivent laisser QU'UNE
    // annulation : sans ce regroupement, "annuler" ne rendrait jamais l'etat
    // d'avant la manipulation, qui est pourtant la seule chose qu'on veut.
    const std::size_t depthBefore = scene.undoDepth();
    for (int i = 0; i < 20; ++i) {
        PlacedObject updated = *scene.find(a);
        updated.yawDegrees += 5.0f;
        scene.transform(a, updated, 100.0f + 0.01f * static_cast<float>(i));
    }
    checkNear(scene.find(a)->yawDegrees, 100.0f, 1e-3f, "Scene : 20 x 5 degres appliques");
    check(scene.undoDepth() == depthBefore + 1,
          "Scene : les retouches rapprochees ne font qu'UNE annulation");
    check(scene.undo(), "Scene : annulation du reglage");
    checkNear(scene.find(a)->yawDegrees, 0.0f, 1e-3f,
              "Scene : le cap d'AVANT toute la manipulation est rendu");

    // Deux retouches ESPACEES restent deux annulations distinctes : le
    // regroupement ne doit pas avaler un geste separe.
    {
        const std::size_t before = scene.undoDepth();
        PlacedObject updated = *scene.find(a);
        updated.scale = 2.0f;
        scene.transform(a, updated, 200.0f);
        updated.scale = 3.0f;
        scene.transform(a, updated, 260.0f); // une minute plus tard
        check(scene.undoDepth() == before + 2,
              "Scene : deux retouches espacees = deux annulations");
        scene.undo();
        checkNear(scene.find(a)->scale, 2.0f, 1e-4f, "Scene : retour a l'echelle intermediaire");
        scene.undo();
        checkNear(scene.find(a)->scale, 1.0f, 1e-4f, "Scene : retour a l'echelle d'origine");
    }

    // transform() ne peut PAS changer l'asset ni l'identite, meme si l'appelant
    // le lui demande : c'est l'invariant qui rend la scene incassable de
    // l'exterieur.
    {
        PlacedObject hostile = *scene.find(a);
        hostile.asset = "abri";
        hostile.id = 4321;
        hostile.scale = -5.0f; // echelle invalide
        scene.transform(a, hostile, 400.0f);
        check(scene.find(a) != nullptr && scene.find(a)->asset == "rocher",
              "Scene : transform() ne change ni l'asset ni l'identifiant");
        check(scene.find(a)->scale > 0.0f, "Scene : une echelle invalide est corrigee");
    }

    // --- replaceAll : renumerotation et pile videe -----------------------
    {
        std::vector<PlacedObject> loaded;
        loaded.push_back(makeObject("rocher", 1.0f, 1.0f));
        loaded.push_back(makeObject("bloc", 2.0f, 2.0f));
        scene.replaceAll(loaded);
        check(scene.size() == 2, "Scene : contenu remplace");
        check(scene.undoDepth() == 0,
              "Scene : la pile d'annulation est videe par un chargement");
        check(scene.objects()[0].id == 1 && scene.objects()[1].id == 2,
              "Scene : identifiants renumerotes dans l'ordre du fichier");
        check(!scene.undo(), "Scene : plus rien a annuler");
    }

    // --- Profondeur bornee ------------------------------------------------
    // On jette la PLUS ANCIENNE, jamais la plus recente : une pile bornee doit
    // continuer d'annuler ce qu'on vient de faire.
    {
        EditorScene deep(&catalog);
        for (std::size_t i = 0; i < EditorScene::kMaxUndoDepth + 20; ++i) {
            deep.place(makeObject("rocher", static_cast<float>(i), 0.0f));
        }
        check(deep.undoDepth() == EditorScene::kMaxUndoDepth,
              "Scene : la pile d'annulation est bornee");
        const std::size_t sizeBefore = deep.size();
        check(deep.undo() && deep.size() == sizeBefore - 1,
              "Scene : la derniere action reste annulable malgre la borne");
    }
}

void testEditorSceneCollision() {
    section("Phase 15 : collision des objets poses");

    AssetCatalog catalog;
    catalog.addBuiltins();
    const AssetEntry& boulder =
        catalog.entries()[static_cast<std::size_t>(catalog.indexOf("rocher"))];

    EditorScene scene(&catalog);
    PlacedObject rock;
    rock.asset = "rocher";
    rock.position = { 0.0f, 0.0f, 0.0f };
    rock.solid = true;
    scene.place(rock);

    BodyCylinder body;
    body.radius = 0.35f;
    body.feetY = 0.0f;
    body.headY = 1.7f;

    // Le joueur veut traverser le rocher : il doit etre repousse HORS du
    // cylindre, exactement comme contre un tronc depuis la Phase 6.
    body.position = { 0.1f, 1.7f, 0.0f };
    check(scene.overlaps(body), "Collision : le joueur chevauche le rocher");
    const glm::vec3 pushed = scene.resolve(body);
    const float distance = std::sqrt(pushed.x * pushed.x + pushed.z * pushed.z);
    check(distance >= boulder.collisionRadius + body.radius - 1e-3f,
          "Collision : repousse juste en dehors du cylindre");

    // Le resultat du repoussement doit etre LIBRE, sinon moveBody() refuserait
    // tout deplacement le long du rocher au lieu de laisser glisser. C'est la
    // raison de la marge d'un dixieme de millimetre dans overlaps().
    BodyCylinder probe = body;
    probe.position = pushed;
    check(!scene.overlaps(probe), "Collision : la position repoussee est libre");

    // Loin : rien ne bouge.
    body.position = { 40.0f, 1.7f, 0.0f };
    check(!scene.overlaps(body), "Collision : pas de contact a 40 m");
    checkNear(scene.resolve(body).x, 40.0f, 1e-5f, "Collision : position intacte au loin");

    // Au-DESSUS de l'objet : pas de contact. Meme regle que pour les souches -
    // c'est ce qui permettra de passer sur un rocher plutot qu'a travers.
    body.position = { 0.0f, 60.0f, 0.0f };
    body.feetY = 58.3f;
    body.headY = 60.15f;
    check(!scene.overlaps(body), "Collision : rien a 58 m au-dessus du rocher");

    // TRAVERSABLE : le drapeau par objet est la seule reponse immediate au cas
    // ou le cylindre englobant est faux (un abri a un interieur).
    {
        EditorScene ghostly(&catalog);
        PlacedObject shelter;
        shelter.asset = "abri";
        shelter.solid = false;
        ghostly.place(shelter);
        BodyCylinder inside;
        inside.radius = 0.35f;
        inside.feetY = 0.0f;
        inside.headY = 1.7f;
        inside.position = { 0.0f, 1.7f, 0.0f };
        check(!ghostly.overlaps(inside), "Collision : un objet traversable ne bloque pas");
        check(ghostly.solidCount() == 0, "Collision : compte des objets solides");
    }

    // ASSET INCONNU : sans dimensions on ne peut pas inventer une empreinte.
    {
        EditorScene orphan(&catalog);
        PlacedObject unknown;
        unknown.asset = "modele_disparu";
        unknown.solid = true;
        orphan.place(unknown);
        BodyCylinder here;
        here.radius = 0.35f;
        here.feetY = 0.0f;
        here.headY = 1.7f;
        here.position = { 0.0f, 1.7f, 0.0f };
        check(!orphan.overlaps(here),
              "Collision : un asset absent du catalogue ne bloque pas");
    }

    // --- Solveur COMPOSITE : foret + editeur --------------------------------
    // Le vrai chemin du jeu depuis la Phase 15. On verifie que les deux sources
    // d'obstacles bloquent toutes les deux, par le meme moveBody().
    {
        FlatTerrain flat(0.0f);
        Ground ground(flat);
        ForestParams noTrees;
        noTrees.treeCount = 0;
        Forest forest(flat, ground, 100.0f, noTrees);
        forest.plantTree(6.0f, 0.0f, 8.0f);

        CompositeCollisionSolver composite;
        composite.add(&forest);
        composite.add(&scene);
        check(composite.size() == 2, "Composite : deux solveurs enregistres");

        BodyCylinder walker;
        walker.radius = 0.35f;
        walker.feetY = 0.0f;
        walker.headY = 1.7f;

        // Contre le ROCHER de l'editeur.
        walker.position = { 0.05f, 1.7f, 0.0f };
        check(composite.overlaps(walker), "Composite : le rocher de l'editeur bloque");
        const glm::vec3 outOfRock =
            moveBody(composite, walker, glm::vec3(-8.0f, 1.7f, 0.0f));
        BodyCylinder after = walker;
        after.position = outOfRock;
        check(!composite.overlaps(after), "Composite : moveBody sort du rocher");

        // Contre le TRONC de la foret, inchange depuis la Phase 6.
        walker.position = { 6.0f, 1.7f, 0.0f };
        check(composite.overlaps(walker), "Composite : le tronc bloque toujours");

        // Entre les deux : libre.
        walker.position = { 3.0f, 1.7f, 0.0f };
        check(!composite.overlaps(walker), "Composite : passage libre entre les deux");

        // Un composite VIDE est totalement transparent : c'est le contrat de
        // CollisionSolver ("une implementation qui n'a rien a dire renvoie la
        // position telle quelle").
        CompositeCollisionSolver nothing;
        check(!nothing.overlaps(walker), "Composite : vide -> aucun obstacle");
        checkNear(nothing.resolve(walker).x, 3.0f, 1e-6f,
                  "Composite : vide -> position inchangee");
    }
}

void testEditorScenePick() {
    section("Phase 15 : visee des objets poses");

    AssetCatalog catalog;
    catalog.addBuiltins();
    EditorScene scene(&catalog);

    PlacedObject near;
    near.asset = "rocher";
    near.position = { 0.0f, 0.0f, -10.0f };
    const uint32_t nearId = scene.place(near);

    PlacedObject far;
    far.asset = "rocher";
    far.position = { 0.0f, 0.0f, -30.0f };
    const uint32_t farId = scene.place(far);

    const glm::vec3 eye(0.0f, 0.9f, 0.0f);
    const glm::vec3 ahead(0.0f, 0.0f, -1.0f);

    check(scene.pick(eye, ahead, 100.0f) == nearId,
          "Visee : le PLUS PROCHE des deux est touche");
    check(scene.pick(eye, ahead, 5.0f) == kNoPlacedObject,
          "Visee : hors de portee -> rien");
    check(scene.pick(glm::vec3(30.0f, 0.9f, 0.0f), ahead, 100.0f) == kNoPlacedObject,
          "Visee : a cote -> rien");

    // En supprimant le proche, c'est le lointain qui vient.
    scene.erase(nearId);
    check(scene.pick(eye, ahead, 100.0f) == farId,
          "Visee : le suivant apres suppression du premier");

    // Trop HAUT : le rayon passe au-dessus. C'est le test de la borne verticale,
    // sans laquelle un objet au sol serait selectionnable depuis le ciel.
    check(scene.pick(glm::vec3(0.0f, 50.0f, 0.0f), ahead, 100.0f) == kNoPlacedObject,
          "Visee : un rayon au-dessus de l'objet ne le touche pas");
}

void testSceneFile() {
    section("Phase 15 : format de scene .vcsc");

    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / "vercora_p15_scene";
    std::error_code ec;
    fs::remove_all(directory, ec);
    fs::create_directories(directory, ec);
    const std::string path = (directory / "test.vcsc").string();

    const Heightmap terrain = makeGeoTerrain(101, 1.0f);

    // --- Fichier absent : ce n'est PAS une erreur -------------------------
    {
        const SceneLoadResult missing = loadSceneFile(path, terrain);
        check(!missing.fileExisted, "Scene : fichier absent signale comme tel");
        check(!missing.ok && missing.objects.empty(),
              "Scene : rien charge, aucune exception");
    }

    // --- Aller-retour exact ----------------------------------------------
    std::vector<PlacedObject> original;
    {
        PlacedObject rock;
        rock.asset = "rocher";
        rock.position = { 12.5f, 0.0f, -7.25f };
        rock.position.y = terrain.heightAt(rock.position.x, rock.position.z);
        rock.yawDegrees = 137.5f;
        rock.scale = 1.75f;
        rock.solid = true;
        original.push_back(rock);

        PlacedObject post;
        post.asset = "poteau";
        post.position = { -30.0f, 0.0f, 41.0f };
        // Volontairement SURELEVE de 2,5 m : le dz doit survivre a l'aller-retour.
        post.position.y = terrain.heightAt(post.position.x, post.position.z) + 2.5f;
        post.yawDegrees = -45.0f;
        post.scale = 0.5f;
        post.solid = false;
        original.push_back(post);
    }

    check(saveSceneFile(path, original, terrain, "terrain_de_test.vchm"),
          "Scene : ecriture reussie");
    check(fs::exists(path), "Scene : le fichier existe");
    check(!fs::exists(path + ".tmp"),
          "Scene : le temporaire d'ecriture atomique a disparu");

    {
        const SceneLoadResult loaded = loadSceneFile(path, terrain);
        check(loaded.fileExisted && loaded.ok, "Scene : relecture reussie");
        check(loaded.version == 1, "Scene : version 1");
        check(loaded.epsg == 2154, "Scene : projection conservee");
        check(loaded.terrainLabel == "terrain_de_test.vchm", "Scene : terrain d'origine note");
        check(loaded.objects.size() == 2, "Scene : deux objets relus");
        check(loaded.skippedObjects == 0 && loaded.unknownDirectives == 0,
              "Scene : aucune ligne perdue");

        for (std::size_t i = 0; i < loaded.objects.size(); ++i) {
            const PlacedObject& before = original[i];
            const PlacedObject& after = loaded.objects[i];
            check(after.asset == before.asset, "Scene : asset conserve");
            // Tolerance d'un demi-millimetre : le fichier ecrit trois decimales,
            // en metres, sur des coordonnees Lambert 93 de sept chiffres.
            checkNear(after.position.x, before.position.x, 5e-4f, "Scene : X conserve");
            checkNear(after.position.z, before.position.z, 5e-4f, "Scene : Z conserve");
            checkNear(after.position.y, before.position.y, 5e-3f, "Scene : altitude conservee");
            checkNear(after.yawDegrees, before.yawDegrees, 1e-2f, "Scene : cap conserve");
            checkNear(after.scale, before.scale, 1e-3f, "Scene : echelle conservee");
            check(after.solid == before.solid, "Scene : solidite conservee");
        }
    }

    // --- LE point du format : independance vis-a-vis de l'emprise ---------
    // La meme scene rechargee sur le MEME endroit echantillonne au pas de 2 m.
    // Les coordonnees sont projetees et l'altitude est relative, donc les objets
    // doivent retomber au meme endroit du massif et rester poses au sol.
    {
        const Heightmap coarse = makeGeoTerrain(51, 2.0f);
        const SceneLoadResult loaded = loadSceneFile(path, coarse);
        check(loaded.ok && loaded.objects.size() == 2,
              "Scene : rechargee sur le terrain au pas de 2 m");
        checkNear(loaded.objects[0].position.x, original[0].position.x, 5e-4f,
                  "Scene : X identique malgre le changement de resolution");
        // Pose au sol : l'altitude est celle de la SURFACE du terrain courant.
        checkNear(loaded.objects[0].position.y,
                  coarse.heightAt(loaded.objects[0].position.x, loaded.objects[0].position.z),
                  5e-3f, "Scene : l'objet pose au sol y reste");
        // Souleve de 2,5 m : il reste souleve de 2,5 m.
        checkNear(loaded.objects[1].position.y -
                      coarse.heightAt(loaded.objects[1].position.x,
                                      loaded.objects[1].position.z),
                  2.5f, 5e-3f, "Scene : l'objet souleve garde sa hauteur relative");
    }

    // --- Robustesse : le fichier s'edite aussi a la main ------------------
    {
        const std::string messy = (directory / "messy.vcsc").string();
        {
            std::ofstream file(messy);
            file << "# un commentaire\n"
                    "version 1\n"
                    "terrain autre.vchm\n"
                    "crs 2154\n"
                    "brouette 42\n"                       // directive inconnue
                    "object rocher 907050.000 6439950.000 0.000 0.000 1.000 1\n"
                    "object incomplet 907050.000\n"       // champs manquants
                    "object cairn 907060.000 6439950.000 0.000 90.000 0.000 1\n" // echelle nulle
                    "\n"
                    "object bloc 907070.000 6439950.000 1.500 10.000 2.000 0 # avec commentaire\n";
        }
        const SceneLoadResult loaded = loadSceneFile(messy, terrain);
        check(loaded.ok, "Scene : un fichier partiellement fautif se charge quand meme");
        check(loaded.objects.size() == 2, "Scene : les deux lignes valides sont gardees");
        check(loaded.skippedObjects == 2, "Scene : les deux lignes fautives sont comptees");
        check(loaded.unknownDirectives == 1, "Scene : la directive inconnue est comptee");
        check(loaded.objects[1].asset == "bloc" && !loaded.objects[1].solid,
              "Scene : commentaire de fin de ligne ignore, solidite lue");
    }

    // --- En-tete obligatoire ---------------------------------------------
    {
        const std::string headless = (directory / "sans_entete.vcsc").string();
        {
            std::ofstream file(headless);
            file << "object rocher 907050.000 6439950.000 0.000 0.000 1.000 1\n";
        }
        const SceneLoadResult loaded = loadSceneFile(headless, terrain);
        check(loaded.fileExisted && !loaded.ok,
              "Scene : sans directive version, le fichier est refuse");
        check(!loaded.error.empty(), "Scene : le refus est explique");
    }

    // --- Terrain SANS georeferencement ------------------------------------
    // Le terrain synthetique n'a pas de projection : les coordonnees sont alors
    // locales, et l'aller-retour doit rester exact.
    {
        Heightmap::Params params;
        params.resolution = 64;
        params.worldSize = 128.0f;
        const Heightmap procedural(params);
        const std::string localPath = (directory / "local.vcsc").string();

        std::vector<PlacedObject> objects;
        PlacedObject object;
        object.asset = "cairn";
        object.position = { -12.0f, 0.0f, 33.0f };
        object.position.y = procedural.heightAt(object.position.x, object.position.z);
        objects.push_back(object);

        check(saveSceneFile(localPath, objects, procedural, "procedural"),
              "Scene : ecriture sur terrain non georeference");
        const SceneLoadResult loaded = loadSceneFile(localPath, procedural);
        check(loaded.ok && loaded.epsg == 0, "Scene : crs 0 pour un terrain local");
        check(loaded.objects.size() == 1, "Scene : objet relu");
        checkNear(loaded.objects[0].position.x, -12.0f, 1e-3f, "Scene : X local conserve");
        checkNear(loaded.objects[0].position.z, 33.0f, 1e-3f, "Scene : Z local conserve");
    }

    // --- Objets hors emprise : conserves, mais comptes ---------------------
    {
        const std::string outside = (directory / "dehors.vcsc").string();
        {
            std::ofstream file(outside);
            file << "version 1\ncrs 2154\n"
                    "object rocher 912000.000 6439950.000 0.000 0.000 1.000 1\n";
        }
        const SceneLoadResult loaded = loadSceneFile(outside, terrain);
        check(loaded.ok && loaded.objects.size() == 1,
              "Scene : un objet hors emprise est CONSERVE");
        check(loaded.outsideExtent == 1, "Scene : il est signale");
    }

    // --- Convention de chemin ---------------------------------------------
    {
        const std::string derived = scenePathFor("assets/terrain/vercora_zone_1m.vchm",
                                                 "assets/scenes");
        check(derived.find("vercora_zone.vcsc") != std::string::npos,
              "Scene : le suffixe de resolution est retire du nom de scene");
        const std::string coarse = scenePathFor("assets/terrain/vercora_zone_2m.vchm",
                                                "assets/scenes");
        check(derived == coarse,
              "Scene : 1 m et 2 m partagent la MEME scene (meme endroit)");
    }

    // --- Conversion projetee : les deux sens sont inverses l'un de l'autre -
    {
        double east = 0.0;
        double north = 0.0;
        terrain.toProjected(17.5f, -23.25f, &east, &north);
        float backX = 0.0f;
        float backZ = 0.0f;
        terrain.fromProjected(east, north, &backX, &backZ);
        checkNear(backX, 17.5f, 1e-4f, "Georeferencement : aller-retour exact en X");
        checkNear(backZ, -23.25f, 1e-4f, "Georeferencement : aller-retour exact en Z");
    }

    fs::remove_all(directory, ec);
}

void testEditorController() {
    section("Phase 15 : controleur d'editeur");

    AssetCatalog catalog;
    catalog.addBuiltins();
    EditorScene scene(&catalog);
    EditorController controller(catalog, scene);
    const FlatTerrain terrain(0.0f);
    constexpr float kHalfExtent = 1000.0f;

    check(controller.brushIndex() == 0, "Controleur : premier asset arme au demarrage");

    // Visee : oeil a 10 m, regard a 45 degres vers -Z. Le point vise est donc
    // (0, 0, -10) sur ce terrain plat.
    EditorInput input;
    input.deltaTime = 1.0f / 60.0f;
    input.now = 0.0f;
    input.eye = { 0.0f, 10.0f, 0.0f };
    input.forward = glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f));

    controller.update(input, terrain, kHalfExtent);
    check(controller.hasPreview(), "Controleur : un fantome suit la visee");
    checkNear(controller.preview().position.z, -10.0f, 0.01f,
              "Controleur : le fantome est au point vise");
    check(controller.preview().id == kNoPlacedObject,
          "Controleur : un fantome de pose n'a pas encore d'identifiant");
    check(scene.empty(), "Controleur : previsualiser ne pose rien");

    // Regard vers le CIEL : pas de fantome, et un clic ne pose rien.
    {
        EditorInput sky = input;
        sky.forward = glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f));
        sky.placePressed = true;
        controller.update(sky, terrain, kHalfExtent);
        check(!controller.hasPreview(), "Controleur : rien a previsualiser vers le ciel");
        check(scene.empty(), "Controleur : viser le ciel ne pose rien");
    }

    // --- Pose --------------------------------------------------------------
    {
        EditorInput place = input;
        place.placePressed = true;
        controller.update(place, terrain, kHalfExtent);
        check(scene.size() == 1, "Controleur : un objet pose au clic");
        checkNear(scene.objects()[0].position.z, -10.0f, 0.01f,
                  "Controleur : pose au point exactement vise");
        // La pose ne SELECTIONNE PAS. Sinon les reglages, qui visent la
        // selection en priorite, seraient silencieusement detournes du pinceau
        // des la premiere pose - et "regler une fois, poser dix fois" deviendrait
        // impossible. Voir le commentaire du bloc de pose dans
        // editor/EditorController.cpp.
        check(controller.selection() == kNoPlacedObject,
              "Controleur : poser ne selectionne pas, le pinceau reste la cible");
    }

    // --- Le PINCEAU garde ses reglages d'une pose a l'autre ----------------
    // C'est le comportement utile quand on aligne une serie de blocs : on regle
    // une fois, on pose dix fois.
    {
        EditorInput turn = input;
        turn.holdRotateRight = true;
        turn.deltaTime = 0.5f; // 90 deg/s x 0,5 s = 45 degres
        controller.update(turn, terrain, kHalfExtent);
        checkNear(controller.preview().yawDegrees, 45.0f, 1e-3f,
                  "Controleur : le pinceau a tourne de 45 degres");

        EditorInput grow = input;
        grow.holdScaleUp = true;
        grow.deltaTime = 1.0f; // facteur 1,8 par seconde
        controller.update(grow, terrain, kHalfExtent);
        checkNear(controller.preview().scale, 1.8f, 1e-3f,
                  "Controleur : echelle multiplicative");

        EditorInput lift = input;
        lift.holdRaise = true;
        lift.deltaTime = 1.0f; // 0,6 m/s
        controller.update(lift, terrain, kHalfExtent);
        checkNear(controller.preview().position.y, 0.6f, 1e-3f,
                  "Controleur : decalage vertical par rapport au sol");

        EditorInput place = input;
        place.placePressed = true;
        controller.update(place, terrain, kHalfExtent);
        check(scene.size() == 2, "Controleur : deuxieme objet pose");
        checkNear(scene.objects()[1].yawDegrees, 45.0f, 1e-3f,
                  "Controleur : le cap du pinceau est repris a la pose");
        checkNear(scene.objects()[1].scale, 1.8f, 1e-3f,
                  "Controleur : l'echelle du pinceau est reprise");
        checkNear(scene.objects()[1].position.y, 0.6f, 1e-3f,
                  "Controleur : la hauteur du pinceau est reprise");
    }

    // --- Pas fin ------------------------------------------------------------
    {
        EditorController fine(catalog, scene);
        EditorInput slow = input;
        slow.holdRotateRight = true;
        slow.holdFineStep = true;
        slow.deltaTime = 0.5f; // 45 degres / 8
        fine.update(slow, terrain, kHalfExtent);
        checkNear(fine.preview().yawDegrees, 45.0f / 8.0f, 1e-3f,
                  "Controleur : la touche de pas fin divise le reglage");
    }

    // --- Selection puis SAISIE ---------------------------------------------
    {
        EditorScene moving(&catalog);
        EditorController tool(catalog, moving);

        EditorInput place = input;
        place.placePressed = true;
        tool.update(place, terrain, kHalfExtent);
        const uint32_t id = moving.objects()[0].id;
        const std::size_t depthAfterPlace = moving.undoDepth();

        // On SELECTIONNE d'abord : la saisie porte sur la selection, et poser ne
        // selectionne plus rien.
        EditorInput pick = input;
        pick.eye = { 0.0f, 0.9f, 0.0f };
        pick.forward = { 0.0f, 0.0f, -1.0f };
        pick.pickPressed = true;
        tool.update(pick, terrain, kHalfExtent);
        check(tool.selection() == id, "Controleur : objet vise puis selectionne");

        // Saisie, puis 60 images de deplacement du regard : la scene ne doit PAS
        // bouger tant que l'objet est en main. Sans ca, deplacer un rocher de
        // trente metres empilerait une annulation par image.
        EditorInput grab = input;
        grab.grabPressed = true;
        tool.update(grab, terrain, kHalfExtent);
        check(tool.grabbing(), "Controleur : objet en main");
        check(tool.hiddenObjectId() == id,
              "Controleur : l'objet en main n'est plus dessine a sa place");

        EditorInput move = input;
        move.eye = { 25.0f, 10.0f, 0.0f };
        for (int frame = 0; frame < 60; ++frame) {
            move.now = 1.0f + 0.016f * static_cast<float>(frame);
            tool.update(move, terrain, kHalfExtent);
        }
        checkNear(moving.objects()[0].position.x, 0.0f, 1e-3f,
                  "Controleur : la scene ne bouge pas tant que l'objet est en main");
        check(moving.undoDepth() == depthAfterPlace,
              "Controleur : aucune annulation empilee pendant le deplacement");
        checkNear(tool.preview().position.x, 25.0f, 0.05f,
                  "Controleur : la previsualisation, elle, suit la visee");

        // Repose : UNE seule annulation pour tout le deplacement.
        EditorInput drop = move;
        drop.now = 3.0f;
        drop.grabPressed = true;
        tool.update(drop, terrain, kHalfExtent);
        check(!tool.grabbing(), "Controleur : objet repose");
        checkNear(moving.objects()[0].position.x, 25.0f, 0.05f,
                  "Controleur : l'objet a bien ete deplace");
        check(moving.undoDepth() == depthAfterPlace + 1,
              "Controleur : UNE annulation pour tout le deplacement");

        EditorInput undo = input;
        undo.now = 4.0f;
        undo.undoPressed = true;
        tool.update(undo, terrain, kHalfExtent);
        checkNear(moving.objects()[0].position.x, 0.0f, 1e-3f,
                  "Controleur : l'annulation ramene l'objet d'ou il venait");
    }

    // --- Selection par la visee, puis suppression --------------------------
    {
        EditorScene target(&catalog);
        EditorController tool(catalog, target);

        EditorInput place = input;
        place.placePressed = true;
        tool.update(place, terrain, kHalfExtent);
        const uint32_t id = target.objects()[0].id;

        check(tool.selection() == kNoPlacedObject,
              "Controleur : rien de selectionne juste apres la pose");

        // On vise l'objet depuis le sol, a l'horizontale.
        EditorInput pick = input;
        pick.eye = { 0.0f, 0.9f, 0.0f };
        pick.forward = { 0.0f, 0.0f, -1.0f };
        pick.pickPressed = true;
        tool.update(pick, terrain, kHalfExtent);
        check(tool.selection() == id, "Controleur : l'objet vise est selectionne");

        EditorInput erase = input;
        erase.deletePressed = true;
        tool.update(erase, terrain, kHalfExtent);
        check(target.empty(), "Controleur : la selection est supprimee");
        check(tool.selection() == kNoPlacedObject, "Controleur : plus rien de selectionne");

        EditorInput undo = input;
        undo.undoPressed = true;
        undo.now = 10.0f;
        tool.update(undo, terrain, kHalfExtent);
        check(target.size() == 1, "Controleur : la suppression est annulable");
    }

    // --- Cycle F3/F4 parmi les objets poses, SANS viser ---------------------
    // Ajoute apres la session initiale de la Phase 15 (voir NOTES-PHASE15.md
    // ss13) : pickPressed exige que la visee touche l'objet, ce qui n'aide en
    // rien a retrouver un objet pose hors du champ courant. F3/F4 parcourent la
    // liste de la scene independamment du regard.
    {
        EditorScene noneScene(&catalog);
        EditorController none(catalog, noneScene);
        EditorInput nextOnEmpty = input;
        nextOnEmpty.cycleNextPressed = true;
        none.update(nextOnEmpty, terrain, kHalfExtent);
        check(none.selection() == kNoPlacedObject,
              "Cycle : rien a selectionner dans une scene vide");
    }
    {
        EditorScene many(&catalog);
        EditorController tool(catalog, many);
        PlacedObject a; a.asset = catalog.entries()[0].id; a.position = { 0.0f, 0.0f, 0.0f };
        PlacedObject b; b.asset = catalog.entries()[0].id; b.position = { 5.0f, 0.0f, 0.0f };
        PlacedObject c; c.asset = catalog.entries()[0].id; c.position = { 10.0f, 0.0f, 0.0f };
        const uint32_t idA = many.place(a);
        const uint32_t idB = many.place(b);
        const uint32_t idC = many.place(c);

        EditorInput next = input;
        next.cycleNextPressed = true;
        tool.update(next, terrain, kHalfExtent);
        check(tool.selection() == idA, "Cycle : F3 depuis rien selectionne prend le premier");
        tool.update(next, terrain, kHalfExtent);
        check(tool.selection() == idB, "Cycle : F3 avance d'un objet");
        tool.update(next, terrain, kHalfExtent);
        check(tool.selection() == idC, "Cycle : F3 avance encore");
        tool.update(next, terrain, kHalfExtent);
        check(tool.selection() == idA, "Cycle : F3 boucle apres le dernier");

        EditorInput prev = input;
        prev.cyclePrevPressed = true;
        tool.update(prev, terrain, kHalfExtent);
        check(tool.selection() == idC, "Cycle : F4 recule et boucle avant le premier");

        // Pendant une saisie, le cycle ne doit RIEN faire - changer la selection
        // sous un objet en main serait incoherent (quel objet suit alors la
        // visee ?).
        EditorInput grab = input;
        grab.grabPressed = true;
        tool.update(grab, terrain, kHalfExtent);
        check(tool.grabbing(), "Cycle : objet en main avant le test d'ignorance");
        EditorInput cycleWhileGrabbing = input;
        cycleWhileGrabbing.cycleNextPressed = true;
        tool.update(cycleWhileGrabbing, terrain, kHalfExtent);
        check(tool.selection() == idC, "Cycle : ignore pendant une saisie");
        check(tool.grabbing(), "Cycle : la saisie en cours n'est pas interrompue");
    }

    // --- Catalogue : parcours et rang direct -------------------------------
    {
        EditorScene empty(&catalog);
        EditorController tool(catalog, empty);

        EditorInput next = input;
        next.nextAssetPressed = true;
        tool.update(next, terrain, kHalfExtent);
        check(tool.brushIndex() == 1, "Controleur : asset suivant");

        EditorInput prev = input;
        prev.prevAssetPressed = true;
        tool.update(prev, terrain, kHalfExtent);
        check(tool.brushIndex() == 0, "Controleur : asset precedent");

        // Reculer depuis le premier doit ramener au DERNIER (modulo positif).
        tool.update(prev, terrain, kHalfExtent);
        check(tool.brushIndex() == static_cast<int>(catalog.size()) - 1,
              "Controleur : le parcours est cyclique dans les deux sens");

        EditorInput quick = input;
        quick.quickPick = 2; // troisieme rang de la page courante
        tool.update(quick, terrain, kHalfExtent);
        check(tool.brushIndex() == 2, "Controleur : rang direct au clavier");
        check(tool.preview().asset == catalog.entries()[2].id,
              "Controleur : le fantome suit l'asset arme");
    }

    // --- Les requetes d'entree/sortie sont des drapeaux, consommes une fois -
    {
        EditorScene empty(&catalog);
        EditorController tool(catalog, empty);
        EditorInput ask = input;
        ask.savePressed = true;
        ask.reloadPressed = true;
        ask.listPressed = true;
        tool.update(ask, terrain, kHalfExtent);
        check(tool.consumeSaveRequest(), "Controleur : sauvegarde demandee");
        check(!tool.consumeSaveRequest(), "Controleur : la demande n'est consommee qu'une fois");
        check(tool.consumeReloadRequest(), "Controleur : rechargement demande");
        check(tool.consumeListRequest(), "Controleur : liste demandee");
    }

    // --- resetInteraction ---------------------------------------------------
    // Appele en sortant du mode edition : laisser un objet "en main" pendant une
    // partie normale le figerait sur le reticule pour toujours.
    {
        EditorScene held(&catalog);
        EditorController tool(catalog, held);
        EditorInput place = input;
        place.placePressed = true;
        tool.update(place, terrain, kHalfExtent);
        EditorInput pick = input;
        pick.eye = { 0.0f, 0.9f, 0.0f };
        pick.forward = { 0.0f, 0.0f, -1.0f };
        pick.pickPressed = true;
        tool.update(pick, terrain, kHalfExtent);
        EditorInput grab = input;
        grab.grabPressed = true;
        tool.update(grab, terrain, kHalfExtent);
        check(tool.grabbing(), "Controleur : objet en main avant la sortie");
        tool.resetInteraction();
        check(!tool.grabbing() && tool.selection() == kNoPlacedObject,
              "Controleur : sortie du mode -> ni saisie ni selection");
        check(held.size() == 1, "Controleur : l'objet reste dans la scene");
    }
}

// ===========================================================================
// PHASE 16 : besoins primaires (faim, soif, fatigue, temperature)
// ===========================================================================

// Relief qui ne varie que selon Z. Indispensable ici et nulle part ailleurs :
// SlopedTerrain penche selon X, or l'exposition adret/ubac ne depend QUE de
// l'axe nord-sud, c'est-a-dire de Z (convention Heightmap : +Z va vers le sud).
// Un test d'exposition sur SlopedTerrain lirait donc 0,5 quelle que soit la
// pente, et passerait meme avec un signe inverse.
class NorthSouthSlope : public HeightSampler {
public:
    explicit NorthSouthSlope(float slope) : slope_(slope) {}
    // slope > 0 : le terrain MONTE vers le sud, donc il descend vers le nord.
    // C'est un UBAC.
    float heightAt(float, float z) const override { return slope_ * z; }

private:
    float slope_;
};

// Cuvette parabolique centree, posee dans un plateau plat. C'est le seul relief
// ou l'on sache a l'avance ou est le creux, donc le seul qui permette de dire
// si le detecteur de points d'eau trouve LE bon endroit et pas seulement un
// endroit.
class BowlTerrain : public HeightSampler {
public:
    BowlTerrain(float centerX, float centerZ, float curvature, float rim)
        : cx_(centerX), cz_(centerZ), k_(curvature), rim_(rim) {}

    float heightAt(float x, float z) const override {
        const float dx = x - cx_;
        const float dz = z - cz_;
        return std::min(rim_, k_ * (dx * dx + dz * dz));
    }

private:
    float cx_, cz_, k_, rim_;
};

// La meme, retournee : une bosse. Elle doit etre rejetee - c'est le controle
// negatif du critere de concavite, sans lequel un detecteur qui ne regarderait
// que la platitude passerait le test de la cuvette par accident.
class DomeTerrain : public HeightSampler {
public:
    DomeTerrain(float centerX, float centerZ, float curvature, float floor)
        : cx_(centerX), cz_(centerZ), k_(curvature), floor_(floor) {}

    float heightAt(float x, float z) const override {
        const float dx = x - cx_;
        const float dz = z - cz_;
        return std::max(floor_, 100.0f - k_ * (dx * dx + dz * dz));
    }

private:
    float cx_, cz_, k_, floor_;
};

// Plusieurs cuvettes creusees dans un meme plateau : la hauteur est celle de la
// cuvette la plus proche. C'est le seul relief qui produise plus d'un point
// d'eau, donc le seul sur lequel l'ecartement minimal veuille dire quelque
// chose - une cuvette unique ne rend qu'un point, et le test passerait a vide.
class MultiBowlTerrain : public HeightSampler {
public:
    MultiBowlTerrain(std::vector<glm::vec2> centers, float curvature, float rim)
        : centers_(std::move(centers)), k_(curvature), rim_(rim) {}

    float heightAt(float x, float z) const override {
        float best = rim_;
        for (const glm::vec2& c : centers_) {
            const float dx = x - c.x;
            const float dz = z - c.y;
            best = std::min(best, k_ * (dx * dx + dz * dz));
        }
        return best;
    }

private:
    std::vector<glm::vec2> centers_;
    float k_, rim_;
};

// Fait tourner les jauges pendant `seconds` avec un contexte constant.
// keepFedAndWatered remet faim et soif a plein a chaque pas : c'est ce qui
// permet de mesurer la fatigue SEULE, sans l'aggravation par les autres manques
// (NeedsParams::deficitFatigueGain). Sans cette option, un test de fatigue
// mesurerait en realite le couplage.
void runNeeds(Needs& needs, const NeedsContext& context, float seconds,
              bool keepFedAndWatered = false, float dt = 1.0f) {
    for (float t = 0.0f; t < seconds; t += dt) {
        needs.update(dt, context);
        if (keepFedAndWatered) {
            needs.setHunger(1.0f);
            needs.setThirst(1.0f);
        }
    }
}

// Distance parcourue par le velo en `seconds` a pedalage donne, sur terrain
// plat. Sert a mesurer une CONSEQUENCE de la fatigue plutot qu'un coefficient :
// le cahier des charges demande "moins de distance en velo", pas "un facteur
// plus petit".
float bikeDistance(float pedalInput, float seconds) {
    FlatTerrain flat(100.0f);
    Bike bike;
    bike.mount(glm::vec3(0.0f, 100.0f, 0.0f), 0.0f, flat);
    const glm::vec3 start = bike.position();
    constexpr float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += dt) {
        bike.update(dt, 0.0f, pedalInput, 0.0f, flat, nullptr);
    }
    return horizontalDistance(bike.position(), start);
}

void testNeeds() {
    section("Phase 16 : jauges de besoins primaires");

    const NeedsParams defaults;

    // --- Etat de depart ------------------------------------------------------
    {
        Needs needs;
        checkNear(needs.hunger(), 1.0f, 1e-6f, "Depart : faim pleine");
        checkNear(needs.thirst(), 1.0f, 1e-6f, "Depart : soif pleine");
        checkNear(needs.fatigue(), 1.0f, 1e-6f, "Depart : forme pleine");
        checkNear(needs.temperature(), 1.0f, 1e-6f, "Depart : temperature pleine");
        checkNear(needs.movementFactor(), 1.0f, 1e-6f,
                  "Depart : aucune penalite de deplacement");
        check(!needs.inTrouble(), "Depart : aucun besoin critique");

        // Un pas de temps nul ou negatif ne doit RIEN faire : la premiere image
        // du jeu a un deltaTime issu d'une soustraction d'horloges, et une
        // valeur nulle y est normale.
        needs.update(0.0f, NeedsContext{});
        needs.update(-1.0f, NeedsContext{});
        checkNear(needs.fatigue(), 1.0f, 1e-6f, "deltaTime <= 0 : jauges intactes");
    }

    // --- Rythme : rien de perceptible en quelques minutes ---------------------
    // C'est la contrainte explicite du cahier des charges, et c'est aussi celle
    // qu'un reglage trop nerveux casserait en premier.
    {
        NeedsContext walking;
        walking.activity = Activity::Walk;
        // PHASE 23 : une ambiante douce et posee, la ou ce bloc reglait avant
        // une altitude et une heure pour l'obtenir. Ce test parle du RYTHME des
        // jauges, pas du climat : il ne doit dependre d'aucun modele meteo.
        walking.ambientTemperatureC = 14.0f;

        Needs needs;
        runNeeds(needs, walking, 5.0f * 60.0f);
        check(needs.hunger() > 0.90f, "5 min de marche : la faim se voit a peine");
        check(needs.thirst() > 0.90f, "5 min de marche : la soif se voit a peine");
        check(needs.fatigue() > 0.90f, "5 min de marche : la forme se voit a peine");
        checkNear(needs.movementFactor(), 1.0f, 1e-6f,
                  "5 min de marche : toujours aucune penalite");
    }

    // --- Rythme : un vrai enjeu sur une session de 30 a 60 min ---------------
    {
        NeedsContext walking;
        walking.activity = Activity::Walk;
        // PHASE 23 : une ambiante douce et posee, la ou ce bloc reglait avant
        // une altitude et une heure pour l'obtenir. Ce test parle du RYTHME des
        // jauges, pas du climat : il ne doit dependre d'aucun modele meteo.
        walking.ambientTemperatureC = 14.0f;

        Needs needs;
        runNeeds(needs, walking, 30.0f * 60.0f);
        check(needs.thirst() < 0.60f, "30 min de marche : la soif est bien entamee");
        check(needs.fatigue() < 0.75f, "30 min de marche : la forme est entamee");
        check(needs.hunger() > 0.70f,
              "30 min de marche : la faim reste le besoin le plus lent");
        check(needs.hunger() > needs.thirst(),
              "On a soif AVANT d'avoir faim (ordre voulu des besoins)");
    }

    // --- Fatigue : les taux annonces dans world/Needs.hpp ---------------------
    {
        NeedsContext walking;
        walking.activity = Activity::Walk;

        Needs half;
        runNeeds(half, walking, 45.0f * 60.0f, /*keepFedAndWatered=*/true);
        checkNear(half.fatigue(), 0.5f, 0.01f,
                  "45 min de marche : la moitie de la forme (1/(90 min) annonce)");
        checkNear(half.fatigueFactor(), 1.0f, 1e-3f,
                  "Forme a la moitie : la penalite commence tout juste");

        Needs empty;
        runNeeds(empty, walking, 90.0f * 60.0f, /*keepFedAndWatered=*/true);
        checkNear(empty.fatigue(), 0.0f, 0.01f, "90 min de marche : forme a plat");
        checkNear(empty.fatigueFactor(), defaults.fatigueFloorFactor, 1e-3f,
                  "Forme a plat : le coefficient tombe a son plancher");
    }

    // --- Chaque activite coute selon son effort ------------------------------
    {
        checkNear(activityEffort(Activity::Rest), 0.0f, 1e-6f, "Effort : le repos ne coute rien");
        check(activityEffort(Activity::Climb) > activityEffort(Activity::Bike),
              "Effort : l'escalade coute plus que le velo");
        check(activityEffort(Activity::Bike) > activityEffort(Activity::Walk),
              "Effort : le velo coute plus que la marche");
        check(activityEffort(Activity::Paraglide) < activityEffort(Activity::Walk),
              "Effort : le parapente coute moins que la marche");

        // Meme duree, quatre activites : la fatigue restante doit se classer
        // dans l'ordre inverse des efforts. C'est la verification demandee -
        // "l'escalade et le velo fatiguent plus vite que la marche".
        auto fatigueAfter = [](Activity activity, float seconds) {
            NeedsContext context;
            context.activity = activity;
            Needs needs;
            runNeeds(needs, context, seconds, /*keepFedAndWatered=*/true);
            return needs.fatigue();
        };
        const float minutes10 = 10.0f * 60.0f;
        const float rest = fatigueAfter(Activity::Rest, minutes10);
        const float walk = fatigueAfter(Activity::Walk, minutes10);
        const float bike = fatigueAfter(Activity::Bike, minutes10);
        const float climb = fatigueAfter(Activity::Climb, minutes10);
        checkNear(rest, 1.0f, 1e-6f, "10 min a l'arret : la forme reste pleine");
        check(walk > bike, "10 min : le velo fatigue plus que la marche");
        check(bike > climb, "10 min : l'escalade fatigue plus que le velo");
        checkNear(1.0f - climb, 4.0f * (1.0f - walk), 1e-3f,
                  "L'escalade fatigue exactement 4 fois plus vite que la marche");
    }

    // --- Repos : la seule source de recuperation de la forme ------------------
    {
        NeedsContext resting;
        resting.activity = Activity::Rest;

        Needs needs;
        needs.setFatigue(0.0f);
        runNeeds(needs, resting, 10.0f * 60.0f, /*keepFedAndWatered=*/true);
        checkNear(needs.fatigue(), 0.5f, 0.01f,
                  "10 min de repos : la moitie de la forme (1/(20 min) annonce)");
        runNeeds(needs, resting, 10.0f * 60.0f, /*keepFedAndWatered=*/true);
        checkNear(needs.fatigue(), 1.0f, 1e-3f, "20 min de repos : forme refaite");

        // Le repos ne rend NI faim NI soif : sans ca, s'arreter reglerait tout
        // et le point d'eau ne servirait a rien.
        Needs other;
        other.setHunger(0.5f);
        other.setThirst(0.5f);
        runNeeds(other, resting, 5.0f * 60.0f);
        check(other.hunger() < 0.5f, "Repos : la faim continue de descendre");
        check(other.thirst() < 0.5f, "Repos : la soif continue de descendre");
    }

    // --- Aggravation croisee -------------------------------------------------
    {
        NeedsContext walking;
        walking.activity = Activity::Walk;

        Needs healthy;
        runNeeds(healthy, walking, 10.0f * 60.0f, /*keepFedAndWatered=*/true);

        Needs parched;
        parched.setThirst(0.0f);
        // On maintient la soif a zero pour isoler l'effet : sinon elle
        // remonterait d'elle-meme, ce qu'aucune jauge ne fait, mais le test
        // doit tenir meme si les taux changent.
        for (float t = 0.0f; t < 10.0f * 60.0f; t += 1.0f) {
            parched.setThirst(0.0f);
            parched.update(1.0f, walking);
        }
        check(parched.fatigue() < healthy.fatigue(),
              "Un joueur assoiffe se fatigue plus vite");
        checkNear(1.0f - parched.fatigue(),
                  (1.0f + defaults.deficitFatigueGain) * (1.0f - healthy.fatigue()),
                  1e-3f, "Manque total : la fatigue double exactement");
    }

    // --- Consequences : les coefficients -------------------------------------
    {
        Needs needs;
        needs.setFatigue(defaults.fatiguePenaltyThreshold);
        checkNear(needs.fatigueFactor(), 1.0f, 1e-6f,
                  "Forme au seuil : aucune penalite encore");
        needs.setFatigue(0.5f * defaults.fatiguePenaltyThreshold);
        checkNear(needs.fatigueFactor(),
                  0.5f * (1.0f + defaults.fatigueFloorFactor), 1e-6f,
                  "Forme a mi-seuil : penalite a mi-chemin du plancher");

        Needs thirsty;
        thirsty.setThirst(defaults.criticalThreshold);
        checkNear(thirsty.thirstFactor(), 1.0f, 1e-6f,
                  "Soif au seuil critique : aucune penalite encore");
        thirsty.setThirst(0.0f);
        checkNear(thirsty.thirstFactor(), defaults.thirstFloorFactor, 1e-6f,
                  "Soif a zero : le coefficient tombe a son plancher");
        check(thirsty.inTrouble(), "Soif a zero : besoin critique signale");

        Needs starving;
        starving.setHunger(0.0f);
        checkNear(starving.hungerFactor(), defaults.hungerFloorFactor, 1e-6f,
                  "Faim a zero : penalite progressive, pas de mort");

        Needs frozen;
        frozen.setTemperature(0.0f);
        checkNear(frozen.coldFactor(), defaults.coldFloorFactor, 1e-6f,
                  "Froid extreme : penalite progressive, pas de mort");
    }

    // --- Aucune jauge ne tue, jamais -----------------------------------------
    {
        Needs dead;
        dead.setHunger(0.0f);
        dead.setThirst(0.0f);
        dead.setFatigue(0.0f);
        dead.setTemperature(0.0f);
        checkNear(dead.movementFactor(), defaults.minimumMovementFactor, 1e-6f,
                  "Les quatre jauges a zero : le plancher de deplacement s'applique");
        check(dead.movementFactor() > 0.3f,
              "Les quatre jauges a zero : le joueur avance encore (pas de punition brutale)");

        // Le plancher MORD reellement : sans lui, le produit des quatre
        // penalites serait bien plus bas. C'est le controle negatif du choix.
        const float rawProduct = dead.fatigueFactor() * dead.hungerFactor() *
                                 dead.thirstFactor() * dead.coldFactor();
        check(rawProduct < defaults.minimumMovementFactor,
              "Le plancher n'est pas decoratif : le produit brut est sous lui");
    }

    // --- Regeneration : boire, manger ----------------------------------------
    {
        Needs needs;
        needs.setThirst(0.2f);
        check(needs.drink(), "Boire : la gorgee compte quand on avait soif");
        checkNear(needs.thirst(), 1.0f, 1e-6f, "Boire : la soif est etanchee");
        check(!needs.drink(), "Boire a soif pleine : rien a signaler");

        needs.setHunger(0.5f);
        check(needs.eat(0.3f), "Manger : la faim remonte");
        checkNear(needs.hunger(), 0.8f, 1e-6f, "Manger : de la valeur nutritive exacte");
        check(!needs.eat(0.0f), "Manger du vide : refuse");
        check(!needs.eat(-1.0f), "Manger une valeur negative : refuse");
        needs.eat(5.0f);
        checkNear(needs.hunger(), 1.0f, 1e-6f, "Manger : la jauge ne depasse pas 1");

        needs.reset();
        checkNear(needs.hunger(), 1.0f, 1e-6f, "reset : tout est plein");
        checkNear(needs.temperature(), 1.0f, 1e-6f, "reset : temperature pleine");
    }

    // --- Climat --------------------------------------------------------------
    // PHASE 23 : le bloc qui verifiait ici le gradient adiabatique, le cycle
    // diurne et l'adret/ubac a DEMENAGE dans testWeather(). Ce n'est pas une
    // perte de couverture, c'est le deplacement du calcul : Needs ne calcule
    // plus aucune ambiante, il en recoit une. Ce qui se verifie desormais ici
    // est ce que Needs fait de cette ambiante, et rien d'autre.

    // --- Exposition lue sur le relief ----------------------------------------
    {
        FlatTerrain flat(300.0f);
        checkNear(terrainExposure(flat, 10.0f, -5.0f), 0.5f, 1e-6f,
                  "Exposition : du plat n'est ni adret ni ubac");

        // SlopedTerrain penche selon X (est-ouest) : aucune exposition non plus.
        SlopedTerrain eastWest(0.0f, 0.80f);
        checkNear(terrainExposure(eastWest, 10.0f, 0.0f), 0.5f, 1e-6f,
                  "Exposition : un versant est-ouest n'est ni adret ni ubac");

        // +Z va vers le SUD : un terrain qui DESCEND vers +Z est un adret.
        NorthSouthSlope adret(-0.50f);
        checkNear(terrainExposure(adret, 0.0f, 0.0f), 1.0f, 1e-4f,
                  "Exposition : versant descendant vers le sud = adret franc");

        NorthSouthSlope ubac(0.50f);
        checkNear(terrainExposure(ubac, 0.0f, 0.0f), 0.0f, 1e-4f,
                  "Exposition : versant descendant vers le nord = ubac franc");

        // Pente douce : l'exposition est ponderee, elle n'est pas franche.
        NorthSouthSlope gentle(kFullExposureSlope * 0.5f);
        checkNear(terrainExposure(gentle, 0.0f, 0.0f), 0.25f, 1e-4f,
                  "Exposition : une pente a mi-seuil ne compte qu'a moitie");
    }

    // --- Temperature : la jauge elle-meme ------------------------------------
    {
        // Parametres depouilles : pas de cycle diurne, pas d'exposition, une
        // ambiante fixee a 10 degres SOUS le confort. C'est le seul moyen de
        // verifier une vitesse de refroidissement sans qu'elle depende de
        // l'heure choisie par le test.
        // PHASE 23 : plus rien a neutraliser dans les parametres - l'ambiante
        // arrive du contexte, on la POSE. Le test est du meme coup plus direct
        // qu'avant, ou il fallait annuler trois termes de climat pour obtenir
        // une temperature stable.
        NeedsParams cold = defaults;

        NeedsContext still;
        still.activity = Activity::Rest;
        still.ambientTemperatureC = cold.comfortTemperatureC - 10.0f;

        Needs needs(cold);
        checkNear(needs.temperature(), 1.0f, 1e-6f, "Froid : on part au chaud");
        runNeeds(needs, still, 15.0f * 60.0f);
        checkNear(needs.temperature(), 0.5f, 0.01f,
                  "10 degres sous le confort : moitie de jauge en 15 min");
        checkNear(needs.ambientTemperatureC(), cold.comfortTemperatureC - 10.0f, 1e-3f,
                  "Froid : l'ambiante relevee est celle attendue");
        runNeeds(needs, still, 15.0f * 60.0f);
        checkNear(needs.temperature(), 0.0f, 0.01f,
                  "10 degres sous le confort : jauge vide en 30 min");
        checkNear(needs.movementFactor(), defaults.coldFloorFactor, 1e-3f,
                  "Gele : la seule penalite active est celle du froid");

        // L'effort rechauffe : meme climat, mais en marchant.
        Needs walker(cold);
        NeedsContext walking = still;
        walking.activity = Activity::Walk;
        runNeeds(walker, walking, 15.0f * 60.0f, /*keepFedAndWatered=*/true);
        check(walker.temperature() > needs.temperature() + 0.05f ||
                  walker.temperature() > 0.5f,
              "Marcher rechauffe : on a moins froid qu'a l'arret");
        check(walker.feltTemperatureC() > walker.ambientTemperatureC(),
              "Marcher : le ressenti est au-dessus de l'ambiante");

        // Le vol refroidit malgre l'effort : c'est le vent. PHASE 23 : ce n'est
        // plus un forfait de -6 degres reserve au parapente, c'est la formule
        // JAG/TI appliquee au vent que l'appelant declare - ici les 10,5 m/s de
        // vitesse propre de l'aile. La MEME mecanique refroidit desormais un
        // marcheur en crete par vent fort, ce qu'un forfait ne pouvait pas
        // faire.
        Needs pilot(cold);
        NeedsContext flying = still;
        flying.activity = Activity::Paraglide;
        flying.windSpeedMps = 10.5f;
        pilot.update(1.0f, flying);
        check(pilot.feltTemperatureC() < pilot.ambientTemperatureC(),
              "Parapente : le ressenti est SOUS l'ambiante (vent relatif)");

        Needs windyWalker(cold);
        NeedsContext exposedRidge = still;
        exposedRidge.windSpeedMps = 14.0f;
        windyWalker.update(1.0f, exposedRidge);
        Needs calmWalker(cold);
        calmWalker.update(1.0f, still);
        check(windyWalker.feltTemperatureC() < calmWalker.feltTemperatureC() - 1.0f,
              "Vent : a l'arret aussi, le vent refroidit le ressenti");
        check(windyWalker.ambientTemperatureC() == calmWalker.ambientTemperatureC(),
              "Vent : il change le RESSENTI, jamais l'ambiante");

        // Un abri annule le vent, pas seulement le froid.
        Needs hutted(cold);
        NeedsContext shelteredGale = exposedRidge;
        shelteredGale.sheltered = true;
        hutted.update(1.0f, shelteredGale);
        check(hutted.feltTemperatureC() > windyWalker.feltTemperatureC(),
              "Abri : le refroidissement eolien ne s'y applique pas");

        // L'abri est le point d'ancrage prevu pour l'artisanat a venir.
        Needs sheltered(cold);
        NeedsContext hut = still;
        hut.sheltered = true;
        runNeeds(sheltered, hut, 15.0f * 60.0f);
        check(sheltered.temperature() > needs.temperature(),
              "Abri : on se refroidit moins vite qu'a decouvert");

        // Rechauffement : au-dessus du confort, la jauge remonte.
        Needs thawing(cold);
        NeedsContext mild = still;
        mild.ambientTemperatureC = cold.comfortTemperatureC + 10.0f;
        thawing.setTemperature(0.0f);
        runNeeds(thawing, mild, 10.0f * 60.0f);
        checkNear(thawing.temperature(), 1.0f, 1e-3f,
                  "Au chaud : la jauge se refait en 10 min");
    }

    // --- L'altitude mord vraiment sur la jauge -------------------------------
    // C'est la demande explicite du cahier des charges : "lien avec l'altitude
    // deja disponible (heightAt())". Deux joueurs, meme heure, meme activite,
    // 1000 m d'ecart -> celui d'en haut a plus froid.
    {
        // PHASE 23 : les deux ambiantes sont desormais PRODUITES par le systeme
        // meteo, a la meme heure et au meme instant, et Needs ne fait que les
        // consommer. Le test verifie donc la chaine COMPLETE - Weather calcule,
        // Needs subit - au lieu de la seule formule interne d'avant.
        const WeatherSystem sky;
        const float nightHours = 24.0f + 2.0f; // 2 h du matin, jour 1
        const WeatherSample lowAir =
            sky.sampleAt(glm::vec3(0.0f, 300.0f, 0.0f), nightHours, nullptr);
        const WeatherSample highAir =
            sky.sampleAt(glm::vec3(0.0f, 1300.0f, 0.0f), nightHours, nullptr);

        NeedsContext valley;
        valley.activity = Activity::Walk;
        valley.ambientTemperatureC = lowAir.temperatureC;
        NeedsContext summit = valley;
        summit.ambientTemperatureC = highAir.temperatureC;

        Needs low;
        Needs high;
        runNeeds(low, valley, 20.0f * 60.0f, /*keepFedAndWatered=*/true);
        runNeeds(high, summit, 20.0f * 60.0f, /*keepFedAndWatered=*/true);
        check(high.temperature() < low.temperature(),
              "Altitude : a 1000 m plus haut, on se refroidit plus vite");
        checkNear(low.ambientTemperatureC() - high.ambientTemperatureC(),
                  WeatherParams{}.lapseRateCPerKm, 1e-2f,
                  "Altitude : l'ecart d'ambiante vaut le gradient annonce");
    }

    // --- Horloge de jeu ------------------------------------------------------
    {
        GameClock clock(9.0f, 12.0f);
        checkNear(clock.hours(), 9.0f, 1e-6f, "Horloge : heure de depart");
        check(clock.clockText() == "09:00", "Horloge : texte a deux chiffres");

        // 300 s reelles x 12 = 3600 s de jeu = une heure.
        clock.advance(300.0f);
        checkNear(clock.hours(), 10.0f, 1e-3f,
                  "Horloge : 5 min reelles = 1 h de jeu au facteur 12");
        check(clock.clockText() == "10:00", "Horloge : texte apres avance");

        clock.advance(150.0f);
        check(clock.clockText() == "10:30", "Horloge : demi-heure lisible");

        // Une session d'une heure reelle traverse bien une journee.
        GameClock day(9.0f, 12.0f);
        day.advance(60.0f * 60.0f);
        checkNear(day.hours(), 21.0f, 1e-2f,
                  "Horloge : 1 h reelle = 12 h de jeu, la nuit tombe dans la session");

        // Repli au tour suivant, sans heure negative ni heure >= 24.
        GameClock wrap(23.5f, 12.0f);
        wrap.advance(300.0f);
        checkNear(wrap.hours(), 0.5f, 1e-3f, "Horloge : passage de minuit");
        wrap.setHours(-2.0f);
        checkNear(wrap.hours(), 22.0f, 1e-3f, "Horloge : reglage negatif ramene dans [0,24)");
        wrap.setHours(26.0f);
        checkNear(wrap.hours(), 2.0f, 1e-3f, "Horloge : reglage au-dela de 24 h");
    }

    // --- Affichage -----------------------------------------------------------
    {
        Needs needs;
        const std::string summary = needs.summary();
        check(summary.find("faim 100%") != std::string::npos,
              "Titre : la faim y figure en pourcentage entier");
        check(summary.find("forme 100%") != std::string::npos,
              "Titre : la forme y figure (jauge a l'endroit, pas 'fatigue')");
        const std::string status = needs.statusLine();
        check(status.find("ambiante") != std::string::npos,
              "Journal : la temperature ambiante y figure");
        check(status.find("deplacement") != std::string::npos,
              "Journal : le coefficient de deplacement y figure");
    }

    // --- CONSEQUENCE REELLE : moins de distance a velo ------------------------
    // Le cahier des charges ne demande pas un coefficient, il demande un effet.
    // On mesure donc la distance parcourue en une minute, pedale au plancher,
    // avec et sans la penalite d'un joueur epuise.
    {
        Needs spent;
        spent.setFatigue(0.0f);
        const float factor = spent.movementFactor();
        check(factor < 0.6f, "Joueur epuise : le coefficient mord");

        const float fresh = bikeDistance(1.0f, 60.0f);
        const float tired = bikeDistance(factor, 60.0f);
        check(tired < fresh, "Velo : un joueur epuise va moins loin en une minute");
        check(tired < 0.85f * fresh,
              "Velo : la perte de distance est franche, pas symbolique");
        check(tired > 0.30f * fresh,
              "Velo : le velo avance encore (pas de blocage)");
    }

    // --- CONSEQUENCE REELLE : escalade plus lente ----------------------------
    {
        CliffTerrain cliff(0.0f, 3.0f, 0.0f, 40.0f);
        constexpr float dt = 1.0f / 60.0f;

        auto climbGain = [&](float climbInput) {
            Climber climber;
            climber.tryStart(cliff, glm::vec3(-1.5f, 1.7f, 0.0f),
                             glm::vec3(1.0f, 0.0f, 0.0f));
            check(climber.climbing(), "Escalade : engagement pour la mesure");
            for (float t = 0.0f; t < 20.0f; t += dt) {
                if (climber.update(dt, climbInput, 0.0f, cliff, nullptr) !=
                    ClimbExit::None) {
                    break;
                }
            }
            return climber.heightGained();
        };

        Needs spent;
        spent.setFatigue(0.0f);
        const float full = climbGain(1.0f);
        const float weak = climbGain(spent.movementFactor());
        check(full > 0.0f, "Escalade : un grimpeur frais gagne de la hauteur");
        check(weak < full, "Escalade : un joueur epuise grimpe moins haut en 20 s");
        check(weak > 0.0f, "Escalade : un joueur epuise grimpe encore");
    }
}

void testWaterSources() {
    section("Phase 16 : detection des points d'eau");

    const WaterSurveyParams defaults;

    // --- Une cuvette est trouvee, et au bon endroit --------------------------
    {
        // Courbure 0,004 : le pourtour a 25 m est 2,5 m plus haut que le fond,
        // au-dessus des 1,5 m exiges. Levre a 40 m : au-dela, plateau plat, qui
        // ne doit rien produire.
        BowlTerrain bowl(120.0f, -80.0f, 0.004f, 40.0f);
        const auto sources = findWaterSources(bowl, -500.0f, 500.0f, -500.0f, 500.0f);
        check(!sources.empty(), "Cuvette : au moins un point d'eau trouve");
        if (!sources.empty()) {
            // Le plus creux est le premier : c'est l'ordre annonce.
            const float distance =
                horizontalDistance(sources.front().position, glm::vec3(120.0f, 0.0f, -80.0f));
            check(distance <= defaults.scanStep,
                  "Cuvette : le meilleur point est a moins d'un pas de balayage du fond");
            checkNear(sources.front().position.y,
                      bowl.heightAt(sources.front().position.x, sources.front().position.z),
                      1e-3f, "Cuvette : le point est SUR la surface");
            check(sources.front().depth >= defaults.minDepth,
                  "Cuvette : la profondeur retenue atteint le minimum exige");

            for (std::size_t i = 1; i < sources.size(); ++i) {
                check(sources[i].depth <= sources[i - 1].depth,
                      "Points d'eau : classes du plus creux au moins creux");
            }
        }
    }

    // --- Controles negatifs : ce qui ne doit RIEN produire -------------------
    {
        FlatTerrain flat(500.0f);
        check(findWaterSources(flat, -500.0f, 500.0f, -500.0f, 500.0f).empty(),
              "Terrain plat : aucun point d'eau (plat n'est pas creux)");

        // Un versant a 20 % : trop pentu, l'eau s'ecoule.
        SlopedTerrain steep(0.0f, 0.20f);
        check(findWaterSources(steep, -500.0f, 500.0f, -500.0f, 500.0f).empty(),
              "Versant a 20 % : aucun point d'eau");

        // Un versant DOUX a 5 % : la pente passe le filtre, mais il n'y a
        // aucune concavite. C'est le controle qui separe les deux criteres -
        // sans lui, un detecteur qui ne regarderait que la platitude passerait.
        SlopedTerrain gentle(0.0f, 0.05f);
        check(findWaterSources(gentle, -500.0f, 500.0f, -500.0f, 500.0f).empty(),
              "Versant doux a 5 % : plat ne suffit pas, il faut du creux");

        // Une bosse : plate au sommet, mais convexe.
        DomeTerrain dome(0.0f, 0.0f, 0.004f, 0.0f);
        check(findWaterSources(dome, -500.0f, 500.0f, -500.0f, 500.0f).empty(),
              "Sommet de bosse : plat mais convexe, aucun point d'eau");
    }

    // --- Bords : les sondes ne doivent pas fabriquer de fausses cuvettes ------
    // heightAt est TOTALE (elle prolonge la hauteur du bord), donc sans le
    // retrait d'une portee de sonde, tout le perimetre du terrain passerait
    // pour un creux.
    {
        BowlTerrain bowl(0.0f, 0.0f, 0.004f, 40.0f);
        const auto sources = findWaterSources(bowl, -300.0f, 300.0f, -300.0f, 300.0f);
        for (const WaterSource& source : sources) {
            check(source.position.x >= -300.0f + defaults.probeDistance - 1e-3f &&
                      source.position.x <= 300.0f - defaults.probeDistance + 1e-3f &&
                      source.position.z >= -300.0f + defaults.probeDistance - 1e-3f &&
                      source.position.z <= 300.0f - defaults.probeDistance + 1e-3f,
                  "Points d'eau : aucun a portee de sonde du bord du terrain");
        }

        // Un rectangle plus petit que deux portees de sonde ne rend rien plutot
        // que de sonder hors emprise.
        check(findWaterSources(bowl, -10.0f, 10.0f, -10.0f, 10.0f).empty(),
              "Emprise minuscule : aucun point d'eau, aucune sonde hors bornes");
        check(findWaterSources(bowl, 100.0f, -100.0f, -100.0f, 100.0f).empty(),
              "Emprise inversee : rien, et pas de boucle infinie");
    }

    // --- Ecartement et bornage -----------------------------------------------
    {
        // Centres poses SUR les points du balayage (x0 = -575, pas de 20) et
        // ecartes de 200 m, donc au-dessus de la separation minimale de 150 m :
        // les trois doivent ressortir, tous les trois.
        MultiBowlTerrain bowl({{25.0f, 25.0f}, {225.0f, 25.0f}, {25.0f, 225.0f}},
                              0.004f, 40.0f);
        const auto sources = findWaterSources(bowl, -600.0f, 600.0f, -600.0f, 600.0f);
        check(sources.size() == 3,
              "Trois cuvettes ecartees : trois points d'eau, un par cuvette");
        check(sources.size() <= defaults.maxSources,
              "Points d'eau : la liste reste bornee");
        for (std::size_t i = 0; i < sources.size(); ++i) {
            for (std::size_t j = i + 1; j < sources.size(); ++j) {
                check(horizontalDistance(sources[i].position, sources[j].position) >=
                          defaults.minSeparation - 1e-3f,
                      "Points d'eau : deux points retenus sont toujours ecartes");
            }
        }

        // Reproductible : deux balayages du meme relief donnent la meme liste.
        // C'est la condition pour qu'un scenario Vulkan puisse aller boire au
        // meme endroit deux fois de suite.
        const auto again = findWaterSources(bowl, -600.0f, 600.0f, -600.0f, 600.0f);
        check(again.size() == sources.size(), "Points d'eau : meme nombre a chaque balayage");
        bool identical = again.size() == sources.size();
        for (std::size_t i = 0; identical && i < sources.size(); ++i) {
            identical = horizontalDistance(again[i].position, sources[i].position) < 1e-3f;
        }
        check(identical, "Points d'eau : balayage deterministe");
    }

    // --- Recherche du plus proche --------------------------------------------
    {
        std::vector<WaterSource> empty;
        float distance = -1.0f;
        check(nearestWaterSource(empty, glm::vec3(0.0f), &distance) == nullptr,
              "Plus proche : rien a trouver dans une liste vide");

        std::vector<WaterSource> sources = {
            WaterSource{glm::vec3(100.0f, 10.0f, 0.0f), 2.0f},
            WaterSource{glm::vec3(0.0f, 90.0f, 20.0f), 3.0f},
        };
        const WaterSource* best =
            nearestWaterSource(sources, glm::vec3(5.0f, 0.0f, 5.0f), &distance);
        check(best == &sources[1], "Plus proche : le bon point est retenu");
        // La distance est HORIZONTALE : les 90 m d'altitude du point ne doivent
        // pas compter, sans quoi on ne pourrait jamais boire.
        checkNear(distance, std::sqrt(25.0f + 225.0f), 1e-3f,
                  "Plus proche : la distance rendue est horizontale");
        check(distance < kDrinkReach * 10.0f, "Plus proche : ordre de grandeur coherent");
    }

    // --- Boire au point d'eau : la chaine complete ----------------------------
    // C'est le point d'ancrage demande : une jauge qui ne se remplit QUE si le
    // joueur est reellement au bord d'un creux detecte sur le relief.
    {
        // Centre pose sur un point du balayage. Ce n'est pas un arrangement de
        // confort : le balayage a un pas de 20 m, et le rayon utile d'une
        // cuvette assez creuse pour etre retenue vaut une vingtaine de metres
        // (au-dela, la pente depasse maxSlope) - c'est d'ailleurs pour ca que le
        // pas du balayage vaut 20 m et non 50. Une cuvette placee entre deux
        // points de grille peut malgre tout passer inapercue : c'est une limite
        // REELLE de la methode, notee dans NOTES-PHASE16.md, pas un artefact de
        // ce test.
        BowlTerrain bowl(25.0f, 25.0f, 0.004f, 40.0f);
        const auto sources = findWaterSources(bowl, -400.0f, 400.0f, -400.0f, 400.0f);
        check(!sources.empty(), "Chaine complete : un point d'eau existe");
        if (!sources.empty()) {
            const glm::vec3 atSource = sources.front().position;
            const glm::vec3 farAway = atSource + glm::vec3(500.0f, 0.0f, 0.0f);

            float distance = 0.0f;
            nearestWaterSource(sources, atSource, &distance);
            check(distance <= kDrinkReach, "Chaine complete : au bord, on est a portee");

            nearestWaterSource(sources, farAway, &distance);
            check(distance > kDrinkReach,
                  "Chaine complete : a 500 m, on n'est pas a portee");

            Needs needs;
            needs.setThirst(0.1f);
            nearestWaterSource(sources, atSource, &distance);
            if (distance <= kDrinkReach) {
                check(needs.drink(), "Chaine complete : on boit au point d'eau");
            }
            checkNear(needs.thirst(), 1.0f, 1e-6f,
                      "Chaine complete : la soif est etanchee sur place");
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 17 : empreinte environnementale (memoire du milieu, faune, degradation)
// ---------------------------------------------------------------------------
// Ce que ces tests couvrent : les deux memoires de l'Habitat et leur asymetrie,
// les seuils de faune et leur hysteresis, la mise a nu du sol et son effet REEL
// sur le semis d'herbe, le placement de la faune par tirage-rejet et ses
// filtres, les deux declencheurs de fuite, la disparition et le retour.
//
// Ce qu'ils ne couvrent pas : le rendu des animaux (aucun GPU ici) et
// l'impression PRODUITE sur un joueur, qui ne se teste pas - seulement le
// scenario Vulkan et l'oeil peuvent dire si "la vallee s'est videe" se voit.

namespace {

// Fait tourner l'Habitat pendant `seconds` SANS presence du joueur, au pas de
// 1/60 s. Passer un seul grand deltaTime ne testerait rien : Habitat::update
// ecrete a kMaxStep (0,25 s), donc un appel unique de 240 s ne ferait
// decroitre que d'un quart de seconde. C'est precisement le genre de piege que
// le test doit exercer, pas contourner.
void idleHabitat(Habitat& habitat, float seconds) {
    constexpr float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += dt) {
        habitat.update(dt);
    }
}

// Presence continue du joueur au meme endroit pendant `seconds`.
void occupyHabitat(Habitat& habitat, const glm::vec3& position, float seconds,
                   float noise = 1.0f) {
    constexpr float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += dt) {
        habitat.notePresence(position, dt, noise);
        habitat.update(dt);
    }
}

} // namespace

void testHabitatMemory() {
    section("Phase 17 : memoire locale du milieu");

    const glm::vec3 here(10.0f, 0.0f, 10.0f);
    const glm::vec3 faraway(600.0f, 0.0f, 600.0f);

    // --- Un monde neuf ne se souvient de rien -------------------------------
    // C'est le contrat TOTAL de GroundCoverSource, et c'est ce qui garantit la
    // non-regression : sans presence ni abattage, tout le reste du jeu doit se
    // comporter exactement comme avant la Phase 17.
    {
        Habitat habitat;
        check(habitat.trackedCellCount() == 0, "Neuf : aucune cellule memorisee");
        checkNear(habitat.disturbanceAt(0.0f, 0.0f), 0.0f, 1e-6f,
                  "Neuf : aucune frequentation");
        checkNear(habitat.loggingAt(0.0f, 0.0f), 0.0f, 1e-6f,
                  "Neuf : aucune deforestation");
        checkNear(habitat.groundCover(0.0f, 0.0f), 1.0f, 1e-6f,
                  "Neuf : le sol est intact");
        check(habitat.allowsWildlife(0.0f, 0.0f), "Neuf : la faune est chez elle");
        check(!habitat.wildlifeFlees(0.0f, 0.0f), "Neuf : rien ne fait fuir");
    }

    // --- La presence monte, et seulement la ou l'on est ---------------------
    {
        Habitat habitat;
        occupyHabitat(habitat, here, 60.0f);
        check(habitat.disturbanceAt(here.x, here.z) > 0.9f,
              "Une minute de marche sur place sature la cellule");
        checkNear(habitat.disturbanceAt(faraway.x, faraway.z), 0.0f, 1e-6f,
                  "A 600 m, le milieu n'a rien remarque");
        check(habitat.trackedCellCount() > 1,
              "Le derangement deborde sur les cellules voisines");

        // Le debordement DECROIT avec la distance : c'est ce qui empeche la
        // grille de 32 m de se lire comme une grille.
        const float atPlayer = habitat.disturbanceAt(here.x, here.z);
        const float aside = habitat.disturbanceAt(here.x + 40.0f, here.z);
        check(aside > 0.0f, "La cellule voisine se souvient aussi");
        check(aside < atPlayer, "Mais moins que celle ou l'on se tient");
    }

    // --- L'oubli, et son ASYMETRIE ------------------------------------------
    {
        Habitat habitat;
        occupyHabitat(habitat, here, 60.0f);
        const float peak = habitat.disturbanceAt(here.x, here.z);

        idleHabitat(habitat, 60.0f);
        const float afterOneMinute = habitat.disturbanceAt(here.x, here.z);
        check(afterOneMinute < peak, "La frequentation redescend quand on part");
        check(afterOneMinute > 0.5f * peak,
              "On derange plus vite qu'on ne se fait oublier (asymetrie 4x)");

        idleHabitat(habitat, 300.0f);
        checkNear(habitat.disturbanceAt(here.x, here.z), 0.0f, 1e-6f,
                  "Cinq minutes de calme effacent le derangement");
        check(habitat.trackedCellCount() == 0,
              "Une cellule revenue a neuf est OUBLIEE (la table ne gonfle pas)");
    }

    // --- Le bruit depend de l'activite --------------------------------------
    // La table activityNoise n'est PAS celle des efforts de la Phase 16, et
    // l'escalade est le cas qui le prouve : effort maximal du joueur, et l'une
    // des activites qui derange le moins.
    {
        check(activityNoise(Activity::Bike) > activityNoise(Activity::Walk),
              "Le velo derange plus que la marche");
        check(activityNoise(Activity::Climb) < activityNoise(Activity::Walk),
              "L'escalade derange moins que la marche");
        check(activityNoise(Activity::Climb) > activityNoise(Activity::Paraglide),
              "Le parapente est ce qui derange le moins");
        check(activityNoise(Activity::Rest) > 0.0f,
              "Meme immobile, un homme derange encore");
        check(activityNoise(Activity::Paraglide) > 0.0f,
              "Aucune activite n'est parfaitement transparente");

        Habitat bikeHabitat;
        Habitat walkHabitat;
        occupyHabitat(bikeHabitat, here, 20.0f, activityNoise(Activity::Bike));
        occupyHabitat(walkHabitat, here, 20.0f, activityNoise(Activity::Walk));
        check(bikeHabitat.disturbanceAt(here.x, here.z) >
                  walkHabitat.disturbanceAt(here.x, here.z),
              "A duree egale, le velo laisse plus de trace que la marche");
    }

    // --- Les deux seuils de faune, et l'hysteresis --------------------------
    {
        Habitat habitat;
        occupyHabitat(habitat, here, 25.0f);
        check(!habitat.wildlifeFlees(here.x, here.z),
              "Vingt-cinq secondes ne suffisent pas a vider la zone");

        occupyHabitat(habitat, here, 40.0f);
        check(habitat.wildlifeFlees(here.x, here.z),
              "Rester trop longtemps finit par faire fuir la faune");
        check(!habitat.allowsWildlife(here.x, here.z),
              "Et elle ne revient pas tant qu'on est la");

        // La bande d'hysteresis : entre les deux seuils, la faune ne fuit plus
        // mais ne revient pas encore. Sans elle, une zone posee sur le seuil
        // ferait clignoter les animaux a chaque image.
        idleHabitat(habitat, 60.0f);
        const float mid = habitat.disturbanceAt(here.x, here.z);
        if (mid < habitat.params().fleeThreshold &&
            mid > habitat.params().respawnThreshold) {
            check(!habitat.wildlifeFlees(here.x, here.z),
                  "Hysteresis : sous le seuil haut, on ne fuit plus");
            check(!habitat.allowsWildlife(here.x, here.z),
                  "Hysteresis : mais pas encore de retour");
        }

        idleHabitat(habitat, 240.0f);
        check(habitat.allowsWildlife(here.x, here.z),
              "Une zone laissee tranquille redevient accueillante");
    }

    // --- La deforestation : un compteur SEPARE, bien plus lent --------------
    {
        Habitat habitat;
        const glm::vec3 stump(16.0f, 0.0f, 16.0f); // centre de la cellule (0,0)

        habitat.recordFelling(stump);
        check(habitat.treesFelledAt(stump.x, stump.z) == 1u,
              "Un arbre abattu est compte");
        check(habitat.allowsWildlife(stump.x, stump.z),
              "Un seul arbre ne suffit pas a interdire la faune");
        check(habitat.groundCover(stump.x, stump.z) < 1.0f,
              "Il laisse tout de meme une marque au sol");
        check(habitat.groundCover(stump.x, stump.z) > 0.8f,
              "Marque MODESTE : couper un arbre n'est pas une coupe rase");

        habitat.recordFelling(stump);
        check(!habitat.allowsWildlife(stump.x, stump.z),
              "Deux arbres dans la meme cellule : la faune n'y revient plus");

        habitat.recordFelling(stump);
        checkNear(habitat.loggingAt(stump.x, stump.z), 1.0f, 1e-3f,
                  "Trois arbres saturent la cellule");
        checkNear(habitat.groundCover(stump.x, stump.z),
                  habitat.params().minGroundCover, 1e-3f,
                  "Le sol est aussi nu qu'il peut l'etre");

        for (int i = 0; i < 10; ++i) {
            habitat.recordFelling(stump);
        }
        check(habitat.loggingAt(stump.x, stump.z) <= 1.0f,
              "Le compteur reste ecrete a 1");
        check(habitat.treesFelledAt(stump.x, stump.z) == 13u,
              "Mais le COMPTE brut d'arbres, lui, continue de monter");

        // Une coupe n'est PAS un derangement : elle survit tres largement au
        // temps qu'il faut pour oublier un passage.
        idleHabitat(habitat, 240.0f);
        checkNear(habitat.disturbanceAt(stump.x, stump.z), 0.0f, 1e-6f,
                  "Quatre minutes plus tard, le bruit de la coupe est oublie");
        check(habitat.loggingAt(stump.x, stump.z) > 0.6f,
              "La coupe, elle, est toujours la");
        check(habitat.groundCover(stump.x, stump.z) < 0.5f,
              "Et le sol est toujours nu");

        idleHabitat(habitat, 900.0f);
        checkNear(habitat.groundCover(stump.x, stump.z), 1.0f, 1e-3f,
                  "Un quart d'heure de plus et le sous-bois est revenu");
        check(habitat.allowsWildlife(stump.x, stump.z),
              "La faune peut y revenir");
    }

    // --- La coupe est LOCALE, et son bord est adouci ------------------------
    {
        Habitat habitat;
        const glm::vec3 stump(16.0f, 0.0f, 16.0f);
        for (int i = 0; i < 3; ++i) {
            habitat.recordFelling(stump);
        }

        checkNear(habitat.groundCover(200.0f, 200.0f), 1.0f, 1e-6f,
                  "Deux cents metres plus loin, le sol est intact");

        // Interpolation bilineaire : le couvert doit remonter PROGRESSIVEMENT en
        // s'eloignant du centre de la coupe, sans marche d'escalier au bord de la
        // cellule. Une valeur constante par cellule dessinerait un damier.
        const float atCentre = habitat.groundCover(stump.x, stump.z);
        const float at20 = habitat.groundCover(stump.x + 20.0f, stump.z);
        const float at40 = habitat.groundCover(stump.x + 40.0f, stump.z);
        check(atCentre < at20, "Le couvert remonte des qu'on s'ecarte du centre");
        check(at20 < at40, "Et continue de remonter : le bord est degrade");
        check(at40 <= 1.0f && atCentre >= habitat.params().minGroundCover,
              "Le couvert interpole reste dans ses bornes");
    }

    // --- Robustesse du pas de temps -----------------------------------------
    // Meme garde-fou que Forest et Needs : une image anormalement longue ne doit
    // pas faire un bond dans la memoire, et un pas negatif ne doit rien faire.
    {
        Habitat habitat;
        habitat.notePresence(here, -1.0f, 1.0f);
        check(habitat.trackedCellCount() == 0, "Un pas negatif n'ecrit rien");

        habitat.notePresence(here, 1000.0f, 1.0f);
        const float capped = habitat.disturbanceAt(here.x, here.z);
        check(capped > 0.0f && capped < 0.1f,
              "Une image de mille secondes est ecretee comme ailleurs");
    }
}

void testAnimalMesh() {
    section("Phase 17 : geometrie de la faune");

    const MeshData animal = animalMeshData();
    check(!animal.empty(), "Le mesh d'animal n'est pas vide");
    check(animal.indices.size() % 3 == 0, "Indices par multiples de trois");
    check(animal.triangleCount() < 200u,
          "Bas-poly assume : moins de 200 triangles");
    check(!animal.name.empty(), "Le mesh porte un nom lisible");

    float minY = 1e9f, maxY = -1e9f, maxX = -1e9f, minX = 1e9f, maxAbsZ = 0.0f;
    bool normalsUnit = true;
    for (const Vertex& vertex : animal.vertices) {
        minY = std::min(minY, vertex.pos.y);
        maxY = std::max(maxY, vertex.pos.y);
        minX = std::min(minX, vertex.pos.x);
        maxX = std::max(maxX, vertex.pos.x);
        maxAbsZ = std::max(maxAbsZ, std::abs(vertex.pos.z));
        normalsUnit = normalsUnit && std::abs(glm::length(vertex.normal) - 1.0f) < 1e-3f;
    }

    // Convention de world/AnimalMesh.hpp : sabots a y = 0, regard vers +X. Un
    // mesh centre sur l'origine s'enfoncerait de moitie dans le terrain, et un
    // mesh regardant vers +Z tournerait le dos a son cap.
    checkNear(minY, 0.0f, 1e-4f, "Les sabots sont a y = 0");
    checkNear(maxY, kAnimalHeight, 0.02f, "La hauteur annoncee est la bonne");
    check(maxX > 0.0f && std::abs(maxX) > std::abs(minX),
          "L'animal regarde vers +X (la tete depasse la croupe)");
    check(maxX - minX <= kAnimalLength + 0.1f, "La longueur annoncee est tenue");
    check(2.0f * maxAbsZ <= kAnimalWidth + 0.1f, "La largeur annoncee est tenue");
    check(normalsUnit, "Toutes les normales sont unitaires");

    // La matrice d'instance : meme convention de cap que le velo.
    {
        Animal deer;
        deer.position = { 5.0f, 100.0f, -3.0f };
        deer.headingDegrees = 0.0f;
        const glm::vec3 nose =
            glm::vec3(deer.modelMatrix() * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)) -
            deer.position;
        checkNear(nose.x, 1.0f, 1e-4f, "Cap 0 : l'animal regarde vers +X");
        checkNear(nose.z, 0.0f, 1e-4f, "Cap 0 : rien sur Z");

        deer.headingDegrees = 90.0f;
        const glm::vec3 turned =
            glm::vec3(deer.modelMatrix() * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)) -
            deer.position;
        checkNear(turned.z, 1.0f, 1e-4f, "Cap 90 : l'animal regarde vers +Z");
        checkNear(turned.x, 0.0f, 1e-4f, "Cap 90 : plus rien sur X");

        // Le pied de l'animal reste ou on l'a mis, quelle que soit l'echelle :
        // c'est ce qui permet de le poser sur le terrain sans correction.
        deer.scale = 1.2f;
        const glm::vec3 foot =
            glm::vec3(deer.modelMatrix() * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        checkNear(foot.y, deer.position.y, 1e-4f,
                  "L'echelle ne decolle pas l'animal du sol");
    }
}

void testWildlife() {
    section("Phase 17 : faune, placement et fuite");

    FlatTerrain flat(1200.0f);
    ConstantMap wooded(0.6f);   // au-dessus de WildlifeParams::minCover
    ConstantMap bare(0.05f);    // en dessous
    const glm::vec3 player(0.0f, 1200.0f, 0.0f);

    // --- Placement par tirage-rejet -----------------------------------------
    {
        Habitat habitat;
        Wildlife wildlife(flat, &wooded, 4000.0f);
        wildlife.populate(player, habitat);

        check(wildlife.animalCount() > 0, "Un bois calme se peuple");
        check(wildlife.animalCount() <= wildlife.params().maxAnimals,
              "La population reste sous son plafond");
        check(wildlife.spawnCount() == wildlife.animalCount(),
              "Le compteur d'apparitions suit la population");

        bool inRing = true;
        bool spaced = true;
        bool onGround = true;
        const auto& animals = wildlife.animals();
        for (std::size_t i = 0; i < animals.size(); ++i) {
            const glm::vec3 delta = animals[i].position - player;
            const float distance = std::sqrt(delta.x * delta.x + delta.z * delta.z);
            inRing = inRing && distance >= wildlife.params().spawnRadiusMin - 0.01f &&
                     distance <= wildlife.params().spawnRadiusMax + 0.01f;
            onGround = onGround && std::abs(animals[i].position.y - 1200.0f) < 1e-3f;
            for (std::size_t j = i + 1; j < animals.size(); ++j) {
                const float dx = animals[i].position.x - animals[j].position.x;
                const float dz = animals[i].position.z - animals[j].position.z;
                spaced = spaced && (dx * dx + dz * dz) >=
                                       wildlife.params().minSpacing *
                                           wildlife.params().minSpacing - 0.01f;
            }
        }
        // Le minimum de l'anneau EST le point important : un animal qui
        // apparaitrait dans le champ proche montrerait la mecanique.
        check(inRing, "Aucun animal n'apparait dans le champ proche du joueur");
        check(spaced, "Les animaux ne s'entassent pas");
        check(onGround, "Les animaux sont poses sur la surface du terrain");
    }

    // --- Determinisme (meme graine, meme monde) -----------------------------
    {
        Habitat habitat;
        Wildlife a(flat, &wooded, 4000.0f);
        Wildlife b(flat, &wooded, 4000.0f);
        a.populate(player, habitat);
        b.populate(player, habitat);
        bool identical = a.animalCount() == b.animalCount();
        for (std::size_t i = 0; identical && i < a.animals().size(); ++i) {
            identical = glm::length(a.animals()[i].position - b.animals()[i].position) < 1e-5f;
        }
        check(identical, "Deux Wildlife de meme graine placent les memes animaux");
    }

    // --- Les filtres refusent -----------------------------------------------
    {
        Habitat habitat;
        Wildlife onBareRock(flat, &bare, 4000.0f);
        onBareRock.populate(player, habitat);
        check(onBareRock.animalCount() == 0,
              "Aucun animal sur du lapiaz nu (carte de couvert)");

        SlopedTerrain cliff(1200.0f, 1.6f); // 160 %, bien au-dela de maxSlope
        Wildlife onCliff(cliff, &wooded, 4000.0f);
        onCliff.populate(player, habitat);
        check(onCliff.animalCount() == 0, "Aucun animal sur une paroi");

        // Sans carte LiDAR, on ne refuse pas tout : c'est le cas du terrain
        // synthetique, et un monde vide y serait un faux negatif permanent.
        Wildlife noMap(flat, nullptr, 4000.0f);
        noMap.populate(player, habitat);
        check(noMap.animalCount() > 0, "Sans carte de couvert, la faune existe quand meme");

        // Emprise : rien hors des donnees, meme si tout le reste est bon.
        Wildlife tiny(flat, &wooded, 40.0f);
        tiny.populate(player, habitat);
        check(tiny.animalCount() == 0,
              "Rien n'est place hors de l'emprise du terrain");
    }

    // --- Fuite 1 : le joueur s'approche trop pres ---------------------------
    {
        Habitat habitat;
        Wildlife wildlife(flat, &wooded, 4000.0f);
        wildlife.populate(player, habitat);
        check(wildlife.fleeingCount() == 0, "Au repos, personne ne fuit");

        const Animal target = wildlife.animals().front();
        // Le joueur se teleporte a cinq metres de cet animal.
        const glm::vec3 intruder(target.position.x + 5.0f, target.position.y,
                                 target.position.z);
        wildlife.update(1.0f / 60.0f, intruder, habitat);

        check(wildlife.fleeingCount() >= 1, "S'approcher fait fuir");
        const Animal* moved = nullptr;
        for (const Animal& animal : wildlife.animals()) {
            if (animal.id == target.id) {
                moved = &animal;
            }
        }
        check(moved != nullptr, "L'animal derange est toujours la (il fuit, il ne disparait pas)");
        if (moved != nullptr) {
            check(moved->state == AnimalState::Fleeing, "Il est en fuite");
            const float before = 5.0f;
            for (int i = 0; i < 60; ++i) {
                wildlife.update(1.0f / 60.0f, intruder, habitat);
            }
            const Animal* after = nullptr;
            for (const Animal& animal : wildlife.animals()) {
                if (animal.id == target.id) {
                    after = &animal;
                }
            }
            if (after != nullptr) {
                const float dx = after->position.x - intruder.x;
                const float dz = after->position.z - intruder.z;
                const float distance = std::sqrt(dx * dx + dz * dz);
                check(distance > before, "Il s'ELOIGNE du joueur");
                check(distance > before + 0.5f * wildlife.params().fleeSpeed,
                      "Et il s'eloigne vite : poursuivre ne sert a rien");
            }
        }
    }

    // --- Fuite 2 : le joueur reste trop longtemps (LE point de la phase) ----
    // Le joueur ne s'est jamais approche a moins de 30 m. C'est la MEMOIRE du
    // milieu, remplie par sa presence prolongee, qui vide la zone.
    {
        Habitat habitat;
        Wildlife wildlife(flat, &wooded, 4000.0f);
        wildlife.populate(player, habitat);
        check(wildlife.animalCount() > 0, "Depart : le bois est habite");

        const Animal target = wildlife.animals().front();
        // Camp installe a 40 m de l'animal : au-dela de fleeRadius (30 m), donc
        // AUCUNE fuite par approche possible.
        const glm::vec3 camp(target.position.x + 40.0f, target.position.y,
                             target.position.z);
        check(glm::length(glm::vec2(camp.x - target.position.x,
                                    camp.z - target.position.z)) >
                  wildlife.params().fleeRadius,
              "Le camp est hors de la distance de fuite par approche");

        // Quatre-vingt-dix secondes de camp. C'est ce que produit le modele a
        // cette distance (ponderation 0,89 a 40 m sur un rayon de 120 m, voir
        // Habitat::notePresence), et c'est le bon ordre de grandeur de jeu : on
        // ne vide pas un vallon en le traversant, on le vide en s'y installant.
        occupyHabitat(habitat, camp, 90.0f);
        check(habitat.wildlifeFlees(target.position.x, target.position.z),
              "Le milieu, lui, a retenu la presence prolongee");

        wildlife.update(1.0f / 60.0f, camp, habitat);
        bool stillCalm = false;
        for (const Animal& animal : wildlife.animals()) {
            if (animal.id == target.id) {
                stillCalm = animal.state == AnimalState::Calm;
            }
        }
        check(!stillCalm, "Rester trop longtemps vide la zone sans s'etre approche");
    }

    // --- Le milieu derange refuse les nouvelles apparitions ------------------
    {
        Habitat saturated;
        // On sature tout l'anneau d'apparition : le joueur a passe une minute
        // partout. C'est artificiel - un vrai joueur mettrait une heure - mais
        // c'est exactement l'etat que le systeme doit savoir produire.
        for (float x = -320.0f; x <= 320.0f; x += 30.0f) {
            for (float z = -320.0f; z <= 320.0f; z += 30.0f) {
                saturated.notePresence({ x, 1200.0f, z }, 0.25f, 1.0f);
            }
        }
        for (int i = 0; i < 300; ++i) {
            for (float x = -320.0f; x <= 320.0f; x += 30.0f) {
                for (float z = -320.0f; z <= 320.0f; z += 30.0f) {
                    saturated.notePresence({ x, 1200.0f, z }, 0.25f, 1.0f);
                }
            }
        }
        check(!saturated.allowsWildlife(150.0f, 0.0f),
              "L'anneau entier est sature de frequentation");

        Wildlife wildlife(flat, &wooded, 4000.0f);
        wildlife.populate(player, saturated);
        check(wildlife.animalCount() == 0,
              "Une zone sur-frequentee ne se repeuple pas");

        // ... ET LA FAUNE PEUT REVENIR. C'est la moitie du systeme : une
        // consequence qu'on ne peut pas defaire serait une punition, pas un
        // milieu vivant.
        idleHabitat(saturated, 400.0f);
        check(saturated.allowsWildlife(150.0f, 0.0f),
              "Laissee tranquille, la zone redevient accueillante");
        wildlife.populate(player, saturated);
        check(wildlife.animalCount() > 0, "Et la faune y revient");
    }

    // --- Une coupe interdit la faune bien plus longtemps qu'un passage ------
    {
        Habitat habitat;
        Wildlife wildlife(flat, &wooded, 4000.0f);

        // Deux arbres abattus dans chaque cellule de l'anneau.
        for (float x = -320.0f; x <= 320.0f; x += 30.0f) {
            for (float z = -320.0f; z <= 320.0f; z += 30.0f) {
                habitat.recordFelling({ x, 1200.0f, z });
                habitat.recordFelling({ x, 1200.0f, z });
            }
        }
        wildlife.populate(player, habitat);
        check(wildlife.animalCount() == 0, "Un versant deforeste reste vide");

        // Quatre minutes suffisent a effacer un passage (test precedent) ; elles
        // ne suffisent PAS a effacer une coupe.
        idleHabitat(habitat, 240.0f);
        wildlife.populate(player, habitat);
        check(wildlife.animalCount() == 0,
              "Quatre minutes plus tard, toujours vide : une coupe n'est pas un passage");
    }

    // --- Disparition au loin -------------------------------------------------
    {
        Habitat habitat;
        Wildlife wildlife(flat, &wooded, 4000.0f);
        wildlife.populate(player, habitat);
        const std::size_t before = wildlife.animalCount();
        check(before > 0, "Des animaux au depart");

        const glm::vec3 elsewhere(3000.0f, 1200.0f, 3000.0f);
        wildlife.update(1.0f / 60.0f, elsewhere, habitat);
        check(wildlife.animalCount() == 0,
              "A trois kilometres, la population precedente est oubliee");
        check(wildlife.despawnCount() == static_cast<uint32_t>(before),
              "Et les disparitions sont comptees");
    }
}

void testEnvironmentalFootprint() {
    section("Phase 17 : la coupe se VOIT au sol");

    // Le bout de chaine qui compte : Habitat -> GroundCoverSource -> semis
    // d'herbe. C'est la seule consequence VISUELLE de la degradation dans cette
    // premiere version (voir NOTES-PHASE17.md pour ce qui a ete ecarte).
    const Frustum frustum = permissiveFrustum();
    SlopedTerrain terrain(1200.0f, 0.0f); // plat, sous grassAltitudeLush
    ConstantMap cover(0.5f);
    ConstantMap noCanopy(0.0f);
    const glm::vec3 eye(16.0f, 1200.0f, 16.0f); // centre de la cellule (0,0)

    std::vector<InstanceData> reference;
    {
        Vegetation vegetation(terrain, &cover, &noCanopy, 4000.0f);
        vegetation.gatherGrass(eye, frustum, reference);
        check(!reference.empty(), "Sans degradation, le pre est herbeux");
    }

    // NON-REGRESSION LITTERALE : un Habitat vierge branche ne doit RIEN changer,
    // pas "presque rien". C'est ce que garantit le contrat total de
    // GroundCoverSource, et c'est ce qui permet d'affirmer que la Phase 17 ne
    // touche pas au decor d'un monde qu'on n'a pas abime.
    {
        Habitat habitat;
        Vegetation vegetation(terrain, &cover, &noCanopy, 4000.0f);
        vegetation.setGroundCover(&habitat);
        std::vector<InstanceData> tufts;
        vegetation.gatherGrass(eye, frustum, tufts);
        check(tufts.size() == reference.size(),
              "Habitat vierge : le semis d'herbe est INCHANGE");
    }

    // Trois arbres abattus dans la cellule sous les pieds.
    {
        Habitat habitat;
        for (int i = 0; i < 3; ++i) {
            habitat.recordFelling(eye);
        }
        Vegetation vegetation(terrain, &cover, &noCanopy, 4000.0f);
        vegetation.setGroundCover(&habitat);
        std::vector<InstanceData> tufts;
        vegetation.gatherGrass(eye, frustum, tufts);

        check(tufts.size() < reference.size(),
              "Apres la coupe, le sol se denude");
        check(!tufts.empty(),
              "Mais il reste des touffes : une coupe rase n'est pas de la roche");

        // La mise a nu est LOCALE : les touffes qui subsistent sont surtout
        // celles des cellules voisines, epargnees par la coupe.
        std::size_t nearStump = 0;
        std::size_t nearStumpBefore = 0;
        for (const InstanceData& tuft : tufts) {
            if (std::abs(tuft.position.x - eye.x) < 12.0f &&
                std::abs(tuft.position.z - eye.z) < 12.0f) {
                ++nearStump;
            }
        }
        for (const InstanceData& tuft : reference) {
            if (std::abs(tuft.position.x - eye.x) < 12.0f &&
                std::abs(tuft.position.z - eye.z) < 12.0f) {
                ++nearStumpBefore;
            }
        }
        check(nearStump < nearStumpBefore,
              "Le manque se voit AU PIED de la coupe");
        check(static_cast<double>(nearStump) / std::max<std::size_t>(1, nearStumpBefore) <
                  0.5,
              "Et il s'y voit franchement (plus de la moitie de l'herbe en moins)");
    }

    // La repousse rend son herbe au sol : le systeme est reversible.
    {
        Habitat habitat;
        for (int i = 0; i < 3; ++i) {
            habitat.recordFelling(eye);
        }
        idleHabitat(habitat, 1200.0f);
        Vegetation vegetation(terrain, &cover, &noCanopy, 4000.0f);
        vegetation.setGroundCover(&habitat);
        std::vector<InstanceData> tufts;
        vegetation.gatherGrass(eye, frustum, tufts);
        check(tufts.size() == reference.size(),
              "Vingt minutes plus tard, l'herbe est integralement revenue");
    }
}

// ===========================================================================
// PHASE 20 : la collision s'etend a la faune
// ===========================================================================
// Ce qui est verifie ici n'est pas "il y a du code de collision" mais "sans le
// solveur, on TRAVERSE ; avec lui, on s'arrete la ou le modele le dit". C'est
// la meme methode qu'en Phase 15 pour les objets d'editeur : un temoin negatif
// et un chiffre PREDIT, pas une impression.
//
// Le temoin est obtenu en rejouant EXACTEMENT le meme monde avec
// WildlifeParams::solid a false - c'est-a-dire l'option --no-animal-collision
// du jeu. Meme graine, donc meme semis, donc meme animal a la meme place : la
// seule difference entre les deux marches est la collision elle-meme.

namespace {

// Marche en ligne droite, pas par pas, par le MEME moveBody que la camera, le
// velo et le grimpeur. Renvoie la position finale ; remplit les temoins passes
// en pointeur (peuvent etre nuls).
glm::vec3 walkThrough(const CollisionSolver& solver, const glm::vec3& start,
                      const glm::vec3& direction, int steps, float stepLength,
                      const glm::vec3& landmark, float* closestApproachOut,
                      bool* sawNaNOut) {
    glm::vec3 position = start;
    float closest = horizontalDistance(start, landmark);
    bool sawNaN = false;

    for (int step = 0; step < steps; ++step) {
        const glm::vec3 previous = position;
        const glm::vec3 desired = position + direction * stepLength;

        BodyCylinder body = standingBody(desired.x, desired.z);
        position = moveBody(solver, body, previous);

        if (std::isnan(position.x) || std::isnan(position.z)) {
            sawNaN = true;
            break;
        }
        closest = std::min(closest, horizontalDistance(position, landmark));
    }

    if (closestApproachOut != nullptr) {
        *closestApproachOut = closest;
    }
    if (sawNaNOut != nullptr) {
        *sawNaNOut = sawNaN;
    }
    return position;
}

} // namespace

void testWildlifeCollision() {
    section("Phase 20 : collision de la faune");

    FlatTerrain flat(0.0f);
    ConstantMap wooded(0.6f);
    const glm::vec3 player(0.0f, 1.7f, 0.0f);

    // Deux mondes IDENTIQUES a une option pres. La graine par defaut est la
    // meme, le terrain est le meme, le semis est donc le meme animal a la meme
    // place - c'est ce qui autorise a comparer les deux marches.
    //
    // UN SEUL animal, et c'est ce qui rend la mesure interpretable : avec la
    // population par defaut (24 animaux espaces d'au moins 14 m), un trajet de
    // 20 m peut en croiser un SECOND, et la marche s'arrete alors sur celui-la.
    // Le premier jet de ce test echouait exactement la-dessus : la collision
    // fonctionnait, c'est la distance predite qui parlait du mauvais animal.
    WildlifeParams soloParams;
    soloParams.maxAnimals = 1;
    WildlifeParams passableParams = soloParams;
    passableParams.solid = false; // == --no-animal-collision

    Habitat habitat;
    Wildlife solidFauna(flat, &wooded, 4000.0f, soloParams);
    Wildlife passableFauna(flat, &wooded, 4000.0f, passableParams);
    solidFauna.populate(player, habitat);
    passableFauna.populate(player, habitat);

    check(solidFauna.animalCount() > 0, "Un animal est seme pour la mesure");
    check(solidFauna.animalCount() == passableFauna.animalCount(),
          "Temoin et mesure ont la meme population");

    const Animal& animal = solidFauna.animals()[0];
    const Animal& twin = passableFauna.animals()[0];
    check(horizontalDistance(animal.position, twin.position) < 1e-4f,
          "Temoin et mesure ont le meme animal au meme endroit");

    // Rayon d'arret ATTENDU, calcule ici et pas lu dans le solveur : rayon de
    // collision de cet animal + demi-largeur du joueur.
    const float reach = solidFauna.colliderRadiusOf(animal) + 0.35f;

    // Trajet : on part a 10 m de l'animal et on marche DROIT dessus sur 20 m,
    // donc de quoi ressortir largement de l'autre cote si rien n'arrete.
    const glm::vec3 toPlayer =
        glm::normalize(glm::vec3(player.x - animal.position.x, 0.0f,
                                 player.z - animal.position.z));
    const glm::vec3 start = animal.position + toPlayer * 10.0f + glm::vec3(0.0f, 1.7f, 0.0f);
    const glm::vec3 heading = -toPlayer;

    float closestWithSolver = 0.0f;
    bool sawNaN = false;
    const glm::vec3 stopped = walkThrough(solidFauna, start, heading, 400, 0.05f,
                                          animal.position, &closestWithSolver, &sawNaN);

    float closestWitness = 0.0f;
    bool witnessNaN = false;
    const glm::vec3 crossed = walkThrough(passableFauna, start, heading, 400, 0.05f,
                                          animal.position, &closestWitness, &witnessNaN);

    const float travelled = horizontalDistance(stopped, start);
    const float travelledWitness = horizontalDistance(crossed, start);

    check(!sawNaN && !witnessNaN, "Aucun NaN sur les 400 pas des deux marches");

    // --- LE TEMOIN : sans collision, on passe AU TRAVERS ---------------------
    // Mesure : approche la plus proche 0,0010 m, soit le centre de l'animal, et
    // les 20 m parcourus en entier. C'est le comportement d'avant la Phase 20.
    check(closestWitness < 0.05f,
          "Temoin (--no-animal-collision) : la marche passe par le centre de l'animal");
    check(travelledWitness > 19.9f,
          "Temoin : les 20 m sont parcourus, l'animal ne freine rien");

    // --- LA MESURE : le contact a lieu au rayon PREDIT -----------------------
    // La premiere version de ce test attendait un ARRET NET a `10 - reach`, et
    // elle avait tort - la mesure l'a dit tout de suite : 13,20 m parcourus au
    // lieu des 8,96 m attendus, pour une approche la plus proche de 1,0418 m
    // EXACTEMENT egale au rayon predit.
    //
    // Ce n'est pas un defaut, c'est le GLISSEMENT que le solveur par
    // repoussement produit exprès (voir CollisionSolver.hpp : "on longe un tronc
    // au lieu de s'y coller net"). Contre un obstacle convexe ISOLE, une poussee
    // frontale n'est jamais parfaitement frontale a l'arrondi pres, et la
    // composante laterale accumulee fait contourner l'animal. Le joueur en sort
    // par le cote, comme il contournerait un tronc.
    //
    // La grandeur qui est reellement PREDITE par le modele, et qu'on verifie
    // donc, est le rayon de CONTACT : rayon de collision de l'animal + demi-
    // largeur du joueur. Elle est atteinte au millimetre pres.
    check(closestWithSolver >= reach - 1e-3f,
          "Avec collision : jamais a l'interieur de l'animal");
    check(std::abs(closestWithSolver - reach) < 1e-3f,
          "Avec collision : le contact a lieu exactement au rayon predit");
    check(travelled < travelledWitness - 5.0f,
          "Avec collision : contourner l'animal coute de la distance (13,2 m contre 20,0 m)");
    check(horizontalDistance(stopped, animal.position) > reach - 1e-3f,
          "Avec collision : la marche finit hors de l'animal, pas dedans");

    // --- Coherence resolve / overlaps ---------------------------------------
    // Le contrat de CollisionSolver : la sortie de resolve() doit etre LIBRE,
    // sinon moveBody() refuserait tout deplacement le long d'un animal.
    {
        BodyCylinder inside = standingBody(animal.position.x + 0.1f,
                                           animal.position.z + 0.1f);
        check(solidFauna.overlaps(inside), "Un corps dans l'animal est declare en contact");

        BodyCylinder pushed = inside;
        pushed.position = solidFauna.resolve(inside);
        check(!solidFauna.overlaps(pushed), "La sortie de resolve() est libre");
        check(std::abs(horizontalDistance(pushed.position, animal.position) - reach) < 1e-3f,
              "resolve() repousse exactement au contact");
    }

    // --- Cas degenere : le joueur exactement sur l'axe de l'animal -----------
    // Un animal peut venir se poser sur un joueur immobile, puisqu'il bouge et
    // que le joueur n'appelle moveBody() qu'en se deplacant.
    {
        BodyCylinder centred = standingBody(animal.position.x, animal.position.z);
        const glm::vec3 out = solidFauna.resolve(centred);
        check(!std::isnan(out.x) && !std::isnan(out.z),
              "Joueur exactement sur l'animal : aucun NaN");
        check(std::abs(horizontalDistance(out, animal.position) - reach) < 1e-3f,
              "Joueur exactement sur l'animal : ecarte au contact");
    }

    // --- La bande verticale : un animal n'est pas un mur -------------------
    // C'est ce qui rend l'extension sans danger pour l'ESCALADE. Le grimpeur
    // soumet une bande centree sur son point d'ancrage : des qu'il est plus
    // haut qu'un cervide, la faune du pied de la paroi ne peut plus le
    // concerner. Aucune ligne de Climber n'a ete ecrite pour ca.
    {
        BodyCylinder onTheWall;
        onTheWall.radius = 0.35f;
        onTheWall.position = { animal.position.x, animal.position.y + 3.0f,
                               animal.position.z };
        onTheWall.feetY = animal.position.y + 2.4f;
        onTheWall.headY = animal.position.y + 3.6f;
        check(!solidFauna.overlaps(onTheWall),
              "Un grimpeur au-dessus du cervide n'est pas gene par lui");

        BodyCylinder atItsFeet = standingBody(animal.position.x, animal.position.z);
        check(solidFauna.overlaps(atItsFeet),
              "Le meme point, au sol, est bien bloque");
    }

    // --- Le rayon suit l'echelle du mesh ------------------------------------
    {
        bool radiusFollowsScale = true;
        for (const Animal& each : solidFauna.animals()) {
            radiusFollowsScale =
                radiusFollowsScale &&
                std::abs(solidFauna.colliderRadiusOf(each) -
                         solidFauna.params().colliderRadius * each.scale) < 1e-6f;
        }
        check(radiusFollowsScale, "Le rayon de collision suit l'echelle de l'animal");
        check(std::abs(solidFauna.params().colliderRadius - 0.5f * kAnimalLength) < 1e-6f,
              "Le rayon par defaut est la demi-longueur du mesh");
    }

    // --- Le monde entier, par le solveur composite --------------------------
    // Le chemin REEL du jeu : les trois sources d'obstacles dans le meme
    // CompositeCollisionSolver, comme main.cpp les y met.
    {
        Ground ground(flat);
        ForestParams noTrees;
        noTrees.treeCount = 0;
        Forest forest(flat, ground, 100.0f, noTrees);
        forest.plantTree(6.0f, 0.0f, 8.0f);

        AssetCatalog catalog;
        catalog.addBuiltins();
        EditorScene scene(&catalog);
        PlacedObject rock;
        rock.asset = "rocher";
        rock.position = { 12.0f, 0.0f, 0.0f };
        rock.solid = true;
        scene.place(rock);

        CompositeCollisionSolver world;
        world.add(&forest);
        world.add(&scene);
        world.add(&solidFauna);
        check(world.size() == 3, "Composite : trois sources d'obstacles");

        BodyCylinder walker = standingBody(6.0f, 0.0f);
        check(world.overlaps(walker), "Composite : le tronc bloque toujours (Phase 6)");
        walker = standingBody(12.0f, 0.0f);
        check(world.overlaps(walker), "Composite : le rocher pose bloque toujours (Phase 15)");
        walker = standingBody(animal.position.x, animal.position.z);
        check(world.overlaps(walker), "Composite : l'animal bloque desormais (Phase 20)");
        walker = standingBody(3.0f, 0.0f);
        check(!world.overlaps(walker), "Composite : le passage libre le reste");

        // --- DECISION DOCUMENTEE : les objets au sol ne bloquent PAS --------
        // Ground n'est pas un CollisionSolver et n'est pas dans ce composite.
        // Ce n'est pas un oubli : buches, branches, pierres et eclats sont des
        // cibles de RAMASSAGE (Ground::findNearest exige de s'en approcher), et
        // le plus haut d'entre eux - la pierre - culmine a 0,40 m, sous le
        // genou. Un debitage depose 3 a 4 buches autour du joueur : les rendre
        // solides pourrait l'emmurer dans le bois qu'il vient de produire.
        ground.drop(GroundItemKind::Stone, 3.0f, 0.0f, 0.0f, 0.0f);
        ground.drop(GroundItemKind::Log, 3.4f, 0.0f, 0.0f, 0.0f);
        check(ground.itemCount() >= 2, "Deux objets sont bien poses sur le trajet");
        check(2.0f * kStoneRadius < 0.5f,
              "Le plus haut objet au sol reste sous le genou");
        walker = standingBody(3.0f, 0.0f);
        check(!world.overlaps(walker),
              "Un objet ramassable au sol ne bloque pas (decision de la Phase 20)");
    }

    // --- LE VELO : la meme collision, sans une ligne ecrite dans Bike --------
    // La consigne de la phase demandait explicitement que l'extension vaille
    // pour TOUTES les activites qui se deplacent, pas seulement la marche. Le
    // velo est le cas qui compte le plus : il roule a 30 km/h, c'est-a-dire a
    // la vitesse de FUITE de l'animal (8 m/s), donc c'est la seule activite qui
    // peut reellement rattraper un cervide et lui rentrer dedans. A pied, on
    // n'en approche jamais - il part a 30 m.
    {
        // On enfourche a 15 m de l'animal, cap droit dessus. Convention de cap
        // du velo : 0 = +X, croissant vers +Z (voir Bike.hpp).
        const glm::vec3 mountPoint = animal.position + toPlayer * 15.0f;
        const glm::vec3 toAnimal = -toPlayer;
        const float headingDegrees = glm::degrees(std::atan2(toAnimal.z, toAnimal.x));

        auto rideAt = [&](const Wildlife& fauna) {
            Bike bike;
            bike.mount(mountPoint, headingDegrees, flat);
            float closest = horizontalDistance(bike.position(), animal.position);
            for (int i = 0; i < 900; ++i) {
                bike.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, flat, &fauna);
                closest = std::min(closest, horizontalDistance(bike.position(),
                                                              animal.position));
            }
            return closest;
        };

        const float closestRiding = rideAt(solidFauna);
        const float closestRidingWitness = rideAt(passableFauna);

        check(closestRidingWitness < 0.5f,
              "Velo, temoin : sans collision le velo passe sur l'animal");
        check(closestRiding >= reach - 1e-3f,
              "Velo : le cycliste bute sur l'animal au lieu de le traverser");
        check(closestRiding > closestRidingWitness + 0.4f,
              "Velo : l'ecart avec le temoin est celui d'un obstacle reel");
    }

    // --- L'ESCALADE : concernee au PIED de la paroi, jamais dessus -----------
    // Le grimpeur recoit le meme `worldSolver` que les deux autres, donc il
    // herite de la collision sans qu'une ligne de Climber change (le test de la
    // Phase 14 le prouve deja pour les troncs). Mais il y a mieux, et c'est une
    // propriete du MODELE, pas du code : un animal ne peut pas se tenir sur une
    // surface assez raide pour etre grimpable.
    //
    // C'est le meme genre de rapport entre deux constantes reglees separement
    // que la lecon de la Phase 17 (noiseRadius vs spawnRadiusMin) - sauf qu'ici
    // le rapport est celui qu'on veut, et cette assertion est ce qui empeche de
    // le perdre par inadvertance en retouchant l'une des deux.
    {
        const ClimberParams climbing;
        // PHASE 24 : les deux nombres ont bouge ensemble (1,00 -> 0,84 pour la
        // paroi, 0,90 -> 0,80 pour l'animal). L'assertion, elle, n'a pas
        // change - et c'est exactement ce qu'on lui demandait : elle porte le
        // RAPPORT, pas les valeurs, donc elle survit au reglage et continue
        // d'interdire de le perdre.
        check(WildlifeParams{}.maxSlope < climbing.wallSlope,
              "Aucun animal ne peut se tenir sur une pente grimpable "
              "(maxSlope < wallSlope)");

        // Et en paroi, la bande verticale suffit de toute facon : verifie plus
        // haut ("Un grimpeur au-dessus du cervide n'est pas gene par lui").
    }

    // --- Le temoin, une derniere fois, cote interface ------------------------
    {
        BodyCylinder centred = standingBody(twin.position.x, twin.position.z);
        check(!passableFauna.overlaps(centred),
              "--no-animal-collision : aucun contact declare");
        check(passableFauna.resolve(centred) == centred.position,
              "--no-animal-collision : resolve() rend la position telle quelle");
    }
}

// ===========================================================================
// PHASE 22 : les gisements d'argile et de sable
// ===========================================================================
// Ce que ces tests peuvent prouver et ce qu'ils ne peuvent PAS prouver merite
// d'etre dit une fois : ils verifient que le detecteur applique bien la regle
// annoncee dans world/Deposits.hpp (creux + versant amont, puis partage par la
// distance a l'eau). Ils ne verifient AUCUNEMENT qu'il y a de l'argile la ou il
// en place - c'est un proxy morphologique, pas une carte geologique, et aucun
// test ne peut valider une vraisemblance.
void testMineralDeposits() {
    section("Phase 22 : gisements d'argile et de sable");

    const DepositSurveyParams defaults;
    const std::vector<WaterSource> noWater;

    // --- Un creux alimente par un versant est trouve ------------------------
    {
        // Cuvette de courbure 0,006 : le pourtour a 25 m est 3,75 m plus haut
        // que le fond, donc au-dessus de minUpslope (3,0 m). Levre a 60 m.
        BowlTerrain bowl(160.0f, -120.0f, 0.006f, 60.0f);
        const auto deposits =
            findMineralDeposits(bowl, -600.0f, 600.0f, -600.0f, 600.0f, noWater);

        check(!deposits.empty(), "Cuvette : au moins un gisement trouve");
        if (!deposits.empty()) {
            const float distance = horizontalDistance(
                deposits.front().position, glm::vec3(160.0f, 0.0f, -120.0f));
            check(distance <= defaults.scanStep,
                  "Cuvette : le meilleur gisement est a moins d'un pas du fond");
            checkNear(deposits.front().position.y,
                      bowl.heightAt(deposits.front().position.x,
                                    deposits.front().position.z),
                      1e-3f, "Cuvette : le gisement est SUR la surface");
            check(deposits.front().upslope >= defaults.minUpslope,
                  "Cuvette : le denivele amont retenu atteint le minimum exige");

            for (std::size_t i = 1; i < deposits.size(); ++i) {
                check(deposits[i].depth <= deposits[i - 1].depth,
                      "Gisements : classes du plus creux au moins creux");
            }
            // Ecartement : c'est la seule garantie qui empeche une grande
            // cuvette de rendre quarante gisements colles.
            bool spaced = true;
            for (std::size_t i = 0; i < deposits.size(); ++i) {
                for (std::size_t j = i + 1; j < deposits.size(); ++j) {
                    if (horizontalDistance(deposits[i].position, deposits[j].position)
                        < defaults.minSeparation - 1e-3f) {
                        spaced = false;
                    }
                }
            }
            check(spaced,
                  "Gisements : deux retenus sont toujours separes de minSeparation");
        }
    }

    // --- Controles negatifs : ce qui ne doit RIEN produire ------------------
    {
        FlatTerrain flat(500.0f);
        check(findMineralDeposits(flat, -600.0f, 600.0f, -600.0f, 600.0f, noWater).empty(),
              "TEMOIN terrain plat : aucun gisement (plat n'est ni creux ni alimente)");

        // Bosse PURE : convexe PARTOUT, sans palier. Courbure choisie faible
        // (0,00002) pour que la pente reste sous maxSlope sur tout le
        // rectangle - sans quoi le rejet viendrait de la pente et non de la
        // convexite, et le temoin ne prouverait pas ce qu'il annonce.
        DomeTerrain dome(0.0f, 0.0f, 0.00002f, -1.0e6f);
        check(findMineralDeposits(dome, -600.0f, 600.0f, -600.0f, 600.0f, noWater).empty(),
              "TEMOIN bosse convexe partout : aucun gisement");

        // Versant regulier a 40 % : pente trop forte ET aucune concavite.
        SlopedTerrain steep(0.0f, 0.40f);
        check(findMineralDeposits(steep, -600.0f, 600.0f, -600.0f, 600.0f, noWater).empty(),
              "TEMOIN versant a 40 % : aucun gisement");

        // MAIS le PIED d'une bosse, lui, doit etre retenu - et c'est une
        // propriete voulue, pas un accident. La rupture de pente au bas d'un
        // versant est CONCAVE et a tout le versant au-dessus d'elle : c'est
        // exactement la definition d'un cone de dejection, l'endroit le plus
        // credible de tout un relief pour trouver du sediment.
        //
        // Ce cas a d'abord ete ecrit comme un controle NEGATIF, et la mesure a
        // montre qu'il ne devait pas l'etre : la bosse a palier rend deux
        // gisements a 160 m du centre, avec 8,1 m de concavite et 26,8 m de
        // versant au-dessus. C'est le detecteur qui avait raison.
        DomeTerrain footed(0.0f, 0.0f, 0.004f, 0.0f);
        const auto atFoot =
            findMineralDeposits(footed, -600.0f, 600.0f, -600.0f, 600.0f, noWater);
        check(!atFoot.empty(), "Pied de bosse : des gisements y sont retenus");
        for (const MineralDeposit& deposit : atFoot) {
            const float radius = horizontalDistance(deposit.position, glm::vec3(0.0f));
            check(radius > 100.0f,
                  "Pied de bosse : les gisements sont au PIED, pas au sommet");
            check(deposit.upslope > deposit.depth,
                  "Pied de bosse : le versant amont domine largement la concavite");
        }
    }

    // --- LE controle qui distingue ce detecteur de celui des points d'eau ----
    // Une cuvette TRES PLATE : concave, mais dont le pourtour ne domine le fond
    // que de 1,0 m. Elle passe le critere de concavite et doit etre rejetee par
    // le seul critere de denivele amont. Sans ce test, minUpslope pourrait etre
    // supprime sans qu'aucune verification ne bronche.
    {
        // Une cuvette dont le RELIEF TOTAL est plafonne a 2 m : aucun point du
        // terrain ne peut donc en dominer un autre de plus de 2 m, et le
        // critere de denivele amont (3 m) est le SEUL qui puisse la rejeter.
        //
        // Le premier jet de ce temoin plafonnait a 40 m et il etait faux : la
        // mesure a montre que le detecteur retenait alors trois points sur un
        // anneau a 60 m du centre, ou le flanc de la cuvette fournit 5,5 m
        // d'amont. Une cuvette n'est "sans amont" qu'a son centre, pas partout.
        BowlTerrain shallow(0.0f, 0.0f, 0.0016f, 2.0f);
        const auto deposits =
            findMineralDeposits(shallow, -600.0f, 600.0f, -600.0f, 600.0f, noWater);
        check(deposits.empty(),
              "TEMOIN cuvette sans amont : rejetee par minUpslope, pas par la concavite");

        // La MEME cuvette avec minUpslope abaisse : elle passe. C'est ce qui
        // prouve que c'est bien ce seuil-la qui l'avait ecartee, et non un
        // autre filtre.
        DepositSurveyParams permissive;
        permissive.minUpslope = 0.5f;
        check(!findMineralDeposits(shallow, -600.0f, 600.0f, -600.0f, 600.0f,
                                   noWater, permissive).empty(),
              "Cuvette sans amont : retenue des que minUpslope est abaisse");
    }

    // --- Le partage argile / sable ------------------------------------------
    {
        BowlTerrain bowl(160.0f, -120.0f, 0.006f, 60.0f);

        // Sans aucun point d'eau connu : TOUT est sable. C'est le comportement
        // annonce - sans eau, on n'affirme rien sur l'argile.
        const auto dry =
            findMineralDeposits(bowl, -600.0f, 600.0f, -600.0f, 600.0f, noWater);
        bool allSand = !dry.empty();
        for (const MineralDeposit& deposit : dry) {
            if (deposit.kind != DepositKind::Sand) {
                allSand = false;
            }
        }
        check(allSand, "TEMOIN sans point d'eau : tous les gisements sont du sable");

        // Avec un point d'eau POSE AU FOND de la cuvette : le gisement du fond
        // devient de l'argile. Meme terrain, meme balayage : la SEULE chose qui
        // a change est la liste des points d'eau.
        std::vector<WaterSource> water;
        water.push_back(WaterSource{glm::vec3(160.0f, 0.0f, -120.0f), 3.0f});
        const auto wet =
            findMineralDeposits(bowl, -600.0f, 600.0f, -600.0f, 600.0f, water);
        check(dry.size() == wet.size(),
              "Le point d'eau ne change PAS quels creux sont retenus, seulement leur nature");
        check(!wet.empty() && wet.front().kind == DepositKind::Clay,
              "Creux au bord de l'eau : classe ARGILE");
        check(!wet.empty() && wet.front().waterDistance <= defaults.clayWaterRadius,
              "Creux classe argile : sa distance a l'eau est bien sous le rayon");

        // Un gisement au-dela du rayon reste du sable, dans le MEME balayage :
        // c'est ce qui prouve que le partage est local et non global.
        for (const MineralDeposit& deposit : wet) {
            if (deposit.waterDistance > defaults.clayWaterRadius) {
                check(deposit.kind == DepositKind::Sand,
                      "Creux loin de l'eau : classe SABLE, dans le meme balayage");
            }
        }
    }

    // --- Recherche du plus proche, et son temoin ----------------------------
    {
        std::vector<MineralDeposit> deposits;
        MineralDeposit a;
        a.position = glm::vec3(0.0f, 10.0f, 0.0f);
        a.kind = DepositKind::Clay;
        MineralDeposit b;
        b.position = glm::vec3(50.0f, 90.0f, 0.0f);
        b.kind = DepositKind::Sand;
        deposits.push_back(a);
        deposits.push_back(b);

        float distance = -1.0f;
        const MineralDeposit* found =
            nearestDeposit(deposits, glm::vec3(4.0f, 0.0f, 3.0f), &distance);
        check(found != nullptr && found->kind == DepositKind::Clay,
              "nearestDeposit : rend bien le plus proche a l'horizontale");
        checkNear(distance, 5.0f, 1e-3f,
                  "nearestDeposit : la distance rendue est horizontale");

        // L'altitude ne doit PAS entrer dans le calcul : b est 80 m plus haut,
        // et pourtant c'est lui le plus proche depuis un point proche de lui.
        found = nearestDeposit(deposits, glm::vec3(48.0f, 0.0f, 0.0f), &distance);
        check(found != nullptr && found->kind == DepositKind::Sand,
              "nearestDeposit : l'altitude n'intervient pas");

        const std::vector<MineralDeposit> empty;
        check(nearestDeposit(empty, glm::vec3(0.0f)) == nullptr,
              "TEMOIN liste vide : nearestDeposit rend nullptr");
    }

    // --- Rectangles degeneres : aucune sortie, aucun plantage ---------------
    {
        FlatTerrain flat;
        check(findMineralDeposits(flat, 10.0f, 10.0f, -100.0f, 100.0f, noWater).empty(),
              "Rectangle de largeur nulle : aucun gisement, aucun plantage");
        check(findMineralDeposits(flat, -10.0f, 10.0f, -10.0f, 10.0f, noWater).empty(),
              "Rectangle plus petit qu'une portee de sonde : aucun gisement");
    }

    // --- Les deux natures ont un libelle ------------------------------------
    {
        check(std::strcmp(depositLabel(DepositKind::Clay), "gisement") != 0,
              "L'argile a son libelle propre");
        check(std::strcmp(depositLabel(DepositKind::Sand), "gisement") != 0,
              "Le sable a son libelle propre");
    }
}

// ===========================================================================
// PHASE 22 / PHASE 26 : essences d'arbres et recolte de resine
// ===========================================================================
void testResin() {
    section("Phase 22 / 26 : essences d'arbres et resine de pin");

    // --- TreeSpecies : Le tirage d'essence est DETERMINISTE et reparti ------
    {
        bool identical = true;
        int coniferCount = 0;
        for (int i = 0; i < 2000; ++i) {
            const float x = static_cast<float>(i % 45) * 12.0f;
            const float z = static_cast<float>(i / 45) * 12.0f;
            const TreeSpecies a = treeSpeciesAt(x, z, 0.0f);
            const TreeSpecies b = treeSpeciesAt(x, z, 0.0f);
            if (a != b) {
                identical = false;
            }
            if (a == TreeSpecies::Conifer) {
                ++coniferCount;
            }
        }
        check(identical,
              "Essences : deux appels de meme position designent la MEME essence");

        // La part observee doit tomber pres de coniferShareNeutral (0.55).
        const double share = static_cast<double>(coniferCount) / 2000.0;
        check(std::fabs(share - 0.55) < 0.04,
              "Essences : la part de resineux observee est bien celle demandee");
    }

    // --- Les deux temoins de l'essence --------------------------------------
    {
        TreeSpeciesParams none;
        none.coniferShareNeutral = 0.0f;
        none.aspectSwing = 0.0f;
        none.coniferShareMin = 0.0f;
        none.coniferShareMax = 0.0f;

        TreeSpeciesParams all;
        all.coniferShareNeutral = 1.0f;
        all.aspectSwing = 0.0f;
        all.coniferShareMin = 1.0f;
        all.coniferShareMax = 1.0f;

        bool noneTagged = true;
        bool allTagged = true;
        for (int i = 0; i < 500; ++i) {
            const float x = static_cast<float>(i) * 7.5f;
            const float z = static_cast<float>(i) * 3.2f;
            if (treeSpeciesAt(x, z, 0.0f, none) == TreeSpecies::Conifer) {
                noneTagged = false;
            }
            if (treeSpeciesAt(x, z, 0.0f, all) != TreeSpecies::Conifer) {
                allTagged = false;
            }
        }
        check(noneTagged, "TEMOIN part nulle : aucun arbre n'est resineux");
        check(allTagged, "TEMOIN part pleine : tous les arbres sont resineux");
    }

    // --- Une graine differente donne un AUTRE tirage ------------------------
    {
        TreeSpeciesParams other;
        other.seed = 12345u;
        int differences = 0;
        for (int i = 0; i < 500; ++i) {
            const float x = static_cast<float>(i) * 5.0f;
            const float z = static_cast<float>(i) * 8.0f;
            if (treeSpeciesAt(x, z, 0.0f) != treeSpeciesAt(x, z, 0.0f, other)) {
                ++differences;
            }
        }
        check(differences > 50,
              "Graine differente : le tirage change reellement (pas un no-op)");
    }

    // --- Exposition adret / ubac : deplace la repartition -------------------
    {
        int ubacConifers = 0;
        int adretConifers = 0;
        for (int i = 0; i < 1000; ++i) {
            const float x = static_cast<float>(i % 30) * 10.0f;
            const float z = static_cast<float>(i / 30) * 10.0f;
            if (treeSpeciesAt(x, z, -1.0f) == TreeSpecies::Conifer) {
                ++ubacConifers;
            }
            if (treeSpeciesAt(x, z, +1.0f) == TreeSpecies::Conifer) {
                ++adretConifers;
            }
        }
        check(ubacConifers > adretConifers,
              "Adret / ubac : plus de resineux en ubac (-1) qu'en adret (+1)");
    }

    // --- La recolte de resine elle-meme (ResinStand) -------------------------
    {
        ResinStand stand;
        const int conifer = 10;
        const int broadleaf = 20;

        check(stand.tap(-1, TreeSpecies::Conifer, 0.0f) == ResinResult::NoTree,
              "TEMOIN rien de vise : la recolte refuse");
        check(stand.tap(broadleaf, TreeSpecies::Broadleaf, 0.0f) == ResinResult::NotConifer,
              "TEMOIN feuillu : la recolte refuse, l'arbre n'est pas resineux");
        check(stand.trackedTreeCount() == 0,
              "Un refus ne memorise RIEN : la table reste vide");

        check(stand.tap(conifer, TreeSpecies::Conifer, 0.0f) == ResinResult::Success,
              "Resineux : la premiere recolte reussit");
        check(stand.tapCount(conifer) == 1, "Resineux : la recolte est comptee");
        check(stand.trackedTreeCount() == 1,
              "Seul l'arbre reellement saigne entre dans la memoire");

        // Aussitot apres : sec.
        check(stand.tap(conifer, TreeSpecies::Conifer, 1.0f) == ResinResult::StillDry,
              "Resineux : deuxieme recolte immediate refusee (il faut du temps)");
        check(stand.secondsUntilFlow(conifer, 1.0f) > 0.0f,
              "Resineux : un delai restant est annonce");

        // Apres le delai : de nouveau.
        const float delay = stand.params().flowDelay;
        check(stand.tap(conifer, TreeSpecies::Conifer, 1.0f + delay) == ResinResult::Success,
              "Resineux : la resine recoule apres flowDelay");
        checkNear(stand.secondsUntilFlow(conifer, 1.0f + delay), delay, 1e-3f,
                  "Resineux : le delai repart a plein apres une recolte");

        // Epuisement au bout de maxTapsPerTree.
        check(stand.tap(conifer, TreeSpecies::Conifer, 1.0f + 2.0f * delay) == ResinResult::Success,
              "Resineux : troisieme recolte encore possible");
        check(stand.tapCount(conifer) == stand.params().maxTapsPerTree,
              "Resineux : le compte atteint maxTapsPerTree");
        check(stand.tap(conifer, TreeSpecies::Conifer, 1.0f + 10.0f * delay) == ResinResult::StillDry,
              "Resineux epuise : refuse meme apres un temps tres long");
        checkNear(stand.secondsUntilFlow(conifer, 1.0f + 10.0f * delay), 0.0f, 1e-3f,
                  "Resineux epuise : plus de delai annonce (c'est tapCount qui le dit)");

        // Un AUTRE resineux reste intact : l'epuisement est par arbre.
        const int otherConifer = 11;
        check(stand.tap(otherConifer, TreeSpecies::Conifer, 0.0f) == ResinResult::Success,
              "L'epuisement est PAR ARBRE : un autre resineux donne toujours");
    }

    // --- Les messages existent pour toutes les issues ------------------------
    {
        const ResinResult results[] = { ResinResult::Success, ResinResult::NotConifer,
                                        ResinResult::StillDry, ResinResult::NoTree };
        for (ResinResult result : results) {
            const char* message = resinResultMessage(result);
            check(message != nullptr && message[0] != '\0',
                  "Chaque issue de recolte a un message journalisable");
        }
    }
}

// ===========================================================================
// PHASE 22 : les plantes a fibres
// ===========================================================================

// Carte de couvert CONSTANTE : le seul moyen de placer le semis exactement au
// milieu, au bord ou hors de la bande de lisiere, sans dependre d'une vraie
// carte LiDAR.
class ConstantCover : public HeightSampler {
public:
    explicit ConstantCover(float value) : value_(value) {}
    float heightAt(float, float) const override { return value_; }

private:
    float value_;
};

// Source d'ouverture CONSTANTE : le pendant du precedent pour la voie 2.
class ConstantClearing : public ClearingSource {
public:
    explicit ConstantClearing(float value) : value_(value) {}
    float clearing(float, float) const override { return value_; }

private:
    float value_;
};

void testFiberPlants() {
    section("Phase 22 : plantes a fibres (ronces, orties)");

    FlatTerrain flat(800.0f);
    const FiberPlantParams defaults;
    const float middleCover = 0.5f * (defaults.coverOpen + defaults.coverClosed);

    // --- Voie 1 : la LISIERE -------------------------------------------------
    {
        ConstantCover edge(middleCover);
        FiberPlants plants(flat, &edge, 500.0f);
        checkNear(plants.edgeDensityAt(0.0f, 0.0f), 1.0f, 1e-3f,
                  "Lisiere : densite maximale au MILIEU de la bande de couvert");
        check(!plants.plantsNear(glm::vec3(0.0f), 30.0f).empty(),
              "Lisiere : des plantes sont bien semees");

        // Les deux bords de la bande, et au-dela : rien. C'est ce qui fait de ce
        // critere une bordure et non un intervalle.
        ConstantCover openGround(defaults.coverOpen - 0.01f);
        ConstantCover deepForest(defaults.coverClosed + 0.01f);
        FiberPlants onOpen(flat, &openGround, 500.0f);
        FiberPlants inForest(flat, &deepForest, 500.0f);
        checkNear(onOpen.edgeDensityAt(0.0f, 0.0f), 0.0f, 1e-3f,
                  "TEMOIN alpage nu : densite de lisiere nulle");
        checkNear(inForest.edgeDensityAt(0.0f, 0.0f), 0.0f, 1e-3f,
                  "TEMOIN futaie fermee : densite de lisiere nulle");
        check(onOpen.plantsNear(glm::vec3(0.0f), 30.0f).empty(),
              "TEMOIN alpage nu : aucune plante semee");
        check(inForest.plantsNear(glm::vec3(0.0f), 30.0f).empty(),
              "TEMOIN futaie fermee : aucune plante semee");

        // Le toit triangulaire : a mi-chemin du bord, la densite doit valoir la
        // moitie. Sans ca, un creneau passerait les tests ci-dessus.
        const float quarter = 0.5f * (defaults.coverOpen + middleCover);
        ConstantCover slope(quarter);
        FiberPlants half(flat, &slope, 500.0f);
        checkNear(half.edgeDensityAt(0.0f, 0.0f), 0.5f, 0.02f,
                  "Lisiere : la densite decroit LINEAIREMENT vers les bords");
    }

    // --- Sans carte de couvert : la voie 1 est muette ------------------------
    {
        FiberPlants plants(flat, nullptr, 500.0f);
        checkNear(plants.edgeDensityAt(0.0f, 0.0f), 0.0f, 1e-3f,
                  "TEMOIN sans carte de couvert : aucune lisiere connue");
        check(plants.plantsNear(glm::vec3(0.0f), 30.0f).empty(),
              "TEMOIN sans carte ni coupe : aucune plante a fibres dans le monde");
    }

    // --- Voie 2 : le SOL OUVERT, et c'est le joueur qui l'ouvre --------------
    {
        // Sans carte de couvert, pour que la voie 2 soit SEULE en cause : tout
        // ce qui pousse ici pousse parce qu'on a coupe.
        FiberPlants plants(flat, nullptr, 500.0f);
        check(plants.plantsNear(glm::vec3(0.0f), 30.0f).empty(), "Avant la coupe : rien");

        ConstantClearing cleared(1.0f);
        plants.setClearingSource(&cleared);
        checkNear(plants.clearingDensityAt(0.0f, 0.0f), 1.0f, 1e-3f,
                  "Sol entierement ouvert : densite de recolonisation maximale");
        check(!plants.plantsNear(glm::vec3(0.0f), 30.0f).empty(),
              "Apres la coupe : des plantes a fibres apparaissent");

        // Le seuil bas est bien un seuil.
        ConstantClearing untouched(defaults.clearingBegin - 0.01f);
        FiberPlants below(flat, nullptr, 500.0f);
        below.setClearingSource(&untouched);
        checkNear(below.clearingDensityAt(0.0f, 0.0f), 0.0f, 1e-3f,
                  "TEMOIN sous clearingBegin : aucune recolonisation");
        check(below.plantsNear(glm::vec3(0.0f), 30.0f).empty(),
              "TEMOIN sous clearingBegin : aucune plante");
    }

    // --- La regle de combinaison est un MAXIMUM, pas une somme ---------------
    {
        ConstantCover edge(middleCover);
        ConstantClearing cleared(1.0f);
        FiberPlants both(flat, &edge, 500.0f);
        both.setClearingSource(&cleared);
        checkNear(both.densityAt(0.0f, 0.0f), 1.0f, 1e-3f,
                  "Lisiere ET coupe : la densite reste bornee a 1 (maximum, pas somme)");

        // Une lisiere a mi-densite plus une coupe pleine doit donner la coupe.
        const float quarter = 0.5f * (defaults.coverOpen + middleCover);
        ConstantCover half(quarter);
        FiberPlants mixed(flat, &half, 500.0f);
        mixed.setClearingSource(&cleared);
        checkNear(mixed.densityAt(0.0f, 0.0f), 1.0f, 1e-3f,
                  "La voie la plus forte l'emporte");
    }

    // --- Les filtres de terrain INTERDISENT, ils ne choisissent pas ----------
    {
        ConstantCover edge(middleCover);

        // Un versant a 150 % : au-dela de maxSlope (1,0).
        SlopedTerrain cliff(800.0f, 1.5f);
        FiberPlants onCliff(cliff, &edge, 500.0f);
        checkNear(onCliff.densityAt(0.0f, 0.0f), 0.0f, 1e-3f,
                  "TEMOIN paroi : aucune plante malgre un couvert de lisiere");

        // Au-dessus de la limite d'altitude.
        FlatTerrain high(defaults.maxAltitude + 50.0f);
        FiberPlants tooHigh(high, &edge, 500.0f);
        checkNear(tooHigh.densityAt(0.0f, 0.0f), 0.0f, 1e-3f,
                  "TEMOIN au-dessus de la limite des arbres : aucune plante");
        // Et juste en dessous, la MEME configuration en produit : c'est ce qui
        // prouve que c'est bien l'altitude qui a decide.
        FlatTerrain justBelow(defaults.maxAltitude - 50.0f);
        FiberPlants allowed(justBelow, &edge, 500.0f);
        check(allowed.densityAt(0.0f, 0.0f) > 0.0f,
              "Juste sous la limite : la meme configuration produit des plantes");
    }

    // --- Le semis est DETERMINISTE -------------------------------------------
    {
        ConstantCover edge(middleCover);
        FiberPlants a(flat, &edge, 500.0f);
        FiberPlants b(flat, &edge, 500.0f);
        const auto pa = a.plantsNear(glm::vec3(20.0f, 0.0f, -13.0f), 25.0f);
        const auto pb = b.plantsNear(glm::vec3(20.0f, 0.0f, -13.0f), 25.0f);
        check(pa.size() == pb.size() && !pa.empty(),
              "Semis : deux FiberPlants de meme graine rendent le meme nombre de plantes");
        bool same = (pa.size() == pb.size());
        for (std::size_t i = 0; i < pa.size() && same; ++i) {
            same = (pa[i].id == pb[i].id)
                && std::fabs(pa[i].position.x - pb[i].position.x) < 1e-4f
                && std::fabs(pa[i].position.z - pb[i].position.z) < 1e-4f
                && std::fabs(pa[i].size - pb[i].size) < 1e-4f;
        }
        check(same, "Semis : les memes plantes, aux memes places, aux memes tailles");

        // Les tailles restent dans les bornes annoncees.
        bool sized = true;
        for (const FiberPlant& plant : pa) {
            if (plant.size < defaults.sizeMin - 1e-4f
                || plant.size > defaults.sizeMax + 1e-4f) {
                sized = false;
            }
        }
        check(sized, "Semis : toutes les tailles tiennent dans [sizeMin, sizeMax]");
    }

    // --- Arrachage et repousse ------------------------------------------------
    {
        ConstantCover edge(middleCover);
        FiberPlants plants(flat, &edge, 500.0f);

        const glm::vec3 player(0.0f, 800.0f, 0.0f);
        const std::size_t before = plants.plantsNear(player, kPullReach).size();
        check(before > 0, "Arrachage : il y a bien une plante a portee pour ce test");

        FiberPlant pulled;
        check(plants.pullNearest(player, kPullReach, &pulled),
              "Arrachage : la plante la plus proche est arrachee");
        check(!plants.isStanding(pulled.id), "Arrachage : la plante n'est plus sur pied");
        check(plants.pulledPlantCount() == 1,
              "Arrachage : une seule plante entre dans la memoire de repousse");
        check(plants.plantsNear(player, kPullReach).size() == before - 1,
              "Arrachage : le semis en rend exactement une de moins");

        // Elle ne repousse PAS avant l'heure.
        plants.update(defaults.regrowthDelay - 1.0f);
        check(!plants.isStanding(pulled.id), "Repousse : rien avant regrowthDelay");
        check(plants.pulledPlantCount() == 1,
              "Repousse : la plante est toujours memorisee");

        // Et elle repousse apres, a l'identique - c'est la promesse du semis
        // deterministe : rien n'est stocke, donc rien ne peut se perdre.
        plants.update(2.0f);
        check(plants.isStanding(pulled.id), "Repousse : la plante revient apres le delai");
        check(plants.pulledPlantCount() == 0,
              "Repousse : la plante SORT de la memoire (grille creuse)");
        check(plants.plantsNear(player, kPullReach).size() == before,
              "Repousse : le semis retrouve exactement son compte");

        bool identical = false;
        for (const FiberPlant& plant : plants.plantsNear(player, kPullReach)) {
            if (plant.id == pulled.id) {
                identical = std::fabs(plant.position.x - pulled.position.x) < 1e-4f
                         && std::fabs(plant.position.z - pulled.position.z) < 1e-4f
                         && std::fabs(plant.size - pulled.size) < 1e-4f;
            }
        }
        check(identical, "Repousse : la MEME plante, a la meme place, a la meme taille");
    }

    // --- Le temoin de l'arrachage : rien a portee ----------------------------
    {
        FiberPlants empty(flat, nullptr, 500.0f);
        check(!empty.pullNearest(glm::vec3(0.0f), kPullReach),
              "TEMOIN monde sans plantes : l'arrachage echoue proprement");
        check(empty.pulledPlantCount() == 0,
              "TEMOIN : un arrachage rate ne memorise rien");

        ConstantCover edge(middleCover);
        FiberPlants plants(flat, &edge, 500.0f);
        check(!plants.pullNearest(glm::vec3(0.0f), 0.0f),
              "TEMOIN portee nulle : l'arrachage echoue proprement");
    }

    // --- La portee est HORIZONTALE -------------------------------------------
    {
        ConstantCover edge(middleCover);
        FiberPlants plants(flat, &edge, 500.0f);
        const std::size_t atFeet =
            plants.plantsNear(glm::vec3(0.0f, 800.0f, 0.0f), 10.0f).size();
        const std::size_t highAbove =
            plants.plantsNear(glm::vec3(0.0f, 3000.0f, 0.0f), 10.0f).size();
        check(atFeet == highAbove && atFeet > 0,
              "Portee : l'altitude du joueur n'intervient pas (portee horizontale)");
    }
}

// ===========================================================================
// PHASE 22 : les geometries des matieres recoltees
// ===========================================================================
void testHarvestMeshes() {
    section("Phase 22 : geometries des matieres recoltees");

    struct MeshCase {
        const char* label;
        MeshData mesh;
        float halfHeight; // 0 = pas de contrainte de centrage vertical
    };
    const MeshCase cases[] = {
        { "motte d'argile", clayLumpMeshData(), kClayRadius },
        { "poignee de sable", sandPileMeshData(), kSandRadius },
        { "perle de resine", resinBlobMeshData(), 0.0f },
        { "fagot de tiges", fiberBundleMeshData(), 0.0f },
        { "plante a fibres sur pied", fiberPlantMeshData(), 0.0f },
    };

    for (const MeshCase& item : cases) {
        check(!item.mesh.empty(), "Mesh non vide");
        check(item.mesh.indices.size() % 3 == 0, "Mesh : indices multiples de 3");
        check(!item.mesh.name.empty(), "Mesh : porte un nom journalisable");

        bool finite = true;
        bool normalized = true;
        float minY = 1e9f;
        float maxY = -1e9f;
        for (const Vertex& vertex : item.mesh.vertices) {
            if (!std::isfinite(vertex.pos.x) || !std::isfinite(vertex.pos.y)
                || !std::isfinite(vertex.pos.z)) {
                finite = false;
            }
            // Toute normale doit etre unitaire : un triangle degenere qui aurait
            // echappe au filtre d'appendFacet donnerait une normale nulle, et
            // l'eclairage s'y verrait comme un trou noir.
            if (std::fabs(glm::length(vertex.normal) - 1.0f) > 1e-3f) {
                normalized = false;
            }
            minY = std::min(minY, vertex.pos.y);
            maxY = std::max(maxY, vertex.pos.y);
        }
        check(finite, "Mesh : aucune coordonnee non finie");
        check(normalized, "Mesh : toutes les normales sont unitaires");

        if (item.halfHeight > 0.0f) {
            // Les objets poses au sol sont CENTRES verticalement et TIENNENT
            // dans leur demi-hauteur : c'est ce qui permet a Ground::drop de les
            // poser en ajoutant simplement groundItemRestRadius a l'altitude du
            // terrain, sans qu'ils s'enfoncent ni qu'ils flottent.
            //
            // On encadre au lieu d'exiger l'egalite : ces meshes sont BRUITES
            // (voir HarvestMesh.cpp), donc leur sommet extreme ne tombe pas
            // pile sur la demi-hauteur. Exiger l'egalite testerait le tirage
            // aleatoire et non le contrat - c'est d'ailleurs ce que le premier
            // jet de ce test faisait, et il echouait sur la motte d'argile.
            check(minY >= -item.halfHeight - 1e-3f && maxY <= item.halfHeight + 1e-3f,
                  "Mesh pose au sol : il TIENT dans sa demi-hauteur");
            check(minY < 0.0f && maxY > 0.0f,
                  "Mesh pose au sol : il est CENTRE (il deborde des deux cotes)");
            check(-minY > 0.7f * item.halfHeight && maxY > 0.7f * item.halfHeight,
                  "Mesh pose au sol : il REMPLIT sa demi-hauteur (pas un objet plat)");
        }
    }

    // La plante SUR PIED, elle, part du SOL et monte : elle n'est pas centree,
    // parce qu'elle est instanciee par son pied comme la touffe d'herbe.
    {
        const MeshData plant = fiberPlantMeshData();
        float minY = 1e9f;
        float maxY = -1e9f;
        for (const Vertex& vertex : plant.vertices) {
            minY = std::min(minY, vertex.pos.y);
            maxY = std::max(maxY, vertex.pos.y);
        }
        checkNear(minY, 0.0f, 1e-3f, "Plante sur pied : son pied est a y = 0");
        check(maxY > 0.4f && maxY <= 1.0f + 1e-3f,
              "Plante sur pied : a l'echelle UNITE (au plus 1 m de haut)");
    }

    // --- Les quatre nouvelles natures ont tout ce qu'un objet au sol doit
    //     avoir. Sans ce test, ajouter une valeur a l'enum sans l'ajouter aux
    //     quatre switch de Ground.cpp passerait inapercu : ces switch ont un
    //     `return` par defaut, donc le compilateur ne dirait rien.
    {
        for (uint32_t i = 0; i < kGroundItemKindCount; ++i) {
            const auto kind = static_cast<GroundItemKind>(i);
            check(groundItemRestRadius(kind) > 0.0f,
                  "Chaque nature d'objet au sol a un rayon de repos non nul");
            check(std::strcmp(groundItemLabel(kind), "objet") != 0,
                  "Chaque nature d'objet au sol a son libelle propre");
            check(std::strcmp(groundItemLabelIndefinite(kind), "un objet") != 0,
                  "Chaque nature a son libelle avec article");
            check(std::strcmp(groundItemLabelPlural(kind), "objets") != 0,
                  "Chaque nature a son libelle au pluriel");
        }
    }

    // --- Elles se posent et se ramassent comme les autres --------------------
    {
        FlatTerrain flat(40.0f);
        Ground ground(flat);
        Inventory inventory;

        const GroundItemKind kinds[] = { GroundItemKind::Clay, GroundItemKind::Sand,
                                         GroundItemKind::Resin, GroundItemKind::Fiber };
        for (GroundItemKind kind : kinds) {
            ground.drop(kind, 3.0f, -2.0f, 0.0f, 0.0f);
        }
        check(ground.itemCount() == 4, "Les quatre matieres se posent au sol");

        const int index = ground.findNearest(glm::vec3(3.0f, 41.7f, -2.0f), 2.5f);
        check(index != kNoGroundItem, "Une matiere posee se retrouve a portee");

        GroundItemKind picked = GroundItemKind::Log;
        check(ground.pickUp(index, &picked), "Une matiere posee se ramasse");
        inventory.add(picked);
        check(inventory.count(picked) == 1, "La matiere ramassee entre dans l'inventaire");
        check(inventory.summary().find("argile") != std::string::npos,
              "Le resume d'inventaire mentionne les nouvelles matieres");
    }
}


// ===========================================================================
// PHASE 23 - Systeme meteo deterministe
// ===========================================================================
// CE QUE CES TESTS VERIFIENT, ET CE QU'ILS NE VERIFIENT PAS.
//
// Ils verifient des PROPRIETES : un determinisme, un signe, un ordre, une
// monotonie. "Un adret est plus chaud qu'un ubac au meme instant". "Un col vente
// plus qu'un plateau". "La meme position au meme instant rend le meme resultat".
//
// Ils ne verifient PRESQUE AUCUNE valeur absolue, et c'est delibere : aucune des
// constantes de WeatherParams n'a ete validee par un climatologue (voir
// l'avertissement en tete de world/Weather.hpp). Un test qui exigerait
// "l'inversion vaut -6 degres" devrait etre reecrit le jour du recalibrage, et
// il ne dirait rien de plus que "la constante vaut ce qu'elle vaut". Les rares
// egalites numeriques ci-dessous portent sur des grandeurs qui NE SONT PAS
// provisoires : le gradient adiabatique compare a son propre parametre, et la
// formule de refroidissement eolien, qui est publiee.
// ===========================================================================

// Chaine de montagnes orientee NORD-SUD, avec un COL creuse dedans a z = 0.
//
// C'est le seul relief de ce fichier ou l'on sache a l'avance ou est le col,
// donc le seul qui permette de dire si le sondage amont trouve LE bon endroit et
// pas seulement un endroit - exactement le role que joue BowlTerrain pour les
// points d'eau depuis la Phase 16.
//
// Un vent d'ouest (+X pour le sens du flux, puisque +X va vers l'est) doit donc
// franchir la crete, et le franchir par le col.
class RidgeWithColTerrain : public HeightSampler {
public:
    RidgeWithColTerrain(float base, float ridgeHeight, float ridgeWidth,
                        float colDepth, float colWidth)
        : base_(base), height_(ridgeHeight), width_(ridgeWidth),
          colDepth_(colDepth), colWidth_(colWidth) {}

    float heightAt(float x, float z) const override {
        // Profil de crete : une gaussienne le long de X, donc un versant au vent
        // (x < 0), un sommet (x = 0) et un versant sous le vent (x > 0).
        const float across = std::exp(-(x / width_) * (x / width_));
        // Entaille : la crete perd colDepth_ de sa hauteur au voisinage de z = 0.
        const float notch = 1.0f - colDepth_ * std::exp(-(z / colWidth_) * (z / colWidth_));
        return base_ + height_ * across * notch;
    }

private:
    float base_, height_, width_, colDepth_, colWidth_;
};

void testWeather() {
    std::printf("\n--- Meteo deterministe (Phase 23) ---\n");

    const WeatherParams defaults;
    const WeatherSystem sky;

    // =======================================================================
    // 1. DETERMINISME - la propriete fondatrice du module
    // =======================================================================
    {
        // Purete stricte : deux appels identiques rendent BIT A BIT le meme
        // resultat. Pas "a epsilon pres" - le module n'a aucun etat, il ne peut
        // pas deriver.
        const SynopticState a = sky.synopticAt(137.25f);
        const SynopticState b = sky.synopticAt(137.25f);
        check(a.seaLevelTemperatureC == b.seaLevelTemperatureC &&
                  a.cloudCover == b.cloudCover && a.seaLevelWindMps == b.seaLevelWindMps &&
                  a.windFromDegrees == b.windFromDegrees,
              "Determinisme : deux appels au meme instant sont identiques");

        RidgeWithColTerrain terrain(800.0f, 400.0f, 300.0f, 0.60f, 400.0f);
        const glm::vec3 spot(120.0f, 1050.0f, -60.0f);
        const WeatherSample s1 = sky.sampleAt(spot, 137.25f, &terrain);
        const WeatherSample s2 = sky.sampleAt(spot, 137.25f, &terrain);
        check(s1.temperatureC == s2.temperatureC && s1.windMps == s2.windMps &&
                  s1.verticalMps == s2.verticalMps && s1.wetness == s2.wetness,
              "Determinisme : deux echantillons au meme point sont identiques");

        // INDEPENDANCE DU CHEMIN, qui est la vraie demande du cahier des charges.
        // Deux horloges couvrent la meme duree de jeu par des pas DIFFERENTS -
        // 60 images par seconde contre 10 - puis on echantillonne. Un modele qui
        // integrerait quoi que ce soit divergerait ici ; celui-ci ne peut pas.
        GameClock fast(9.0f, 12.0f);
        GameClock slow(9.0f, 12.0f);
        for (int i = 0; i < 6000; ++i) { fast.advance(1.0f / 60.0f); }
        for (int i = 0; i < 1000; ++i) { slow.advance(1.0f / 10.0f); }
        checkNear(fast.totalHours(), slow.totalHours(), 1e-2f,
                  "Horloge : deux pas de temps differents, meme temps de jeu");

        const WeatherSample viaFast = sky.sampleAt(spot, fast.totalHours(), &terrain);
        const WeatherSample viaSlow = sky.sampleAt(spot, slow.totalHours(), &terrain);
        checkNear(viaFast.temperatureC, viaSlow.temperatureC, 0.05f,
                  "Chemin : la meteo ne depend pas de la facon dont on est arrive");
        checkNear(viaFast.windMps, viaSlow.windMps, 0.05f,
                  "Chemin : le vent non plus");

        // Et l'inverse : deux instants ECARTES doivent, eux, differer. Sans ce
        // controle negatif, une fonction constante passerait tous les tests
        // ci-dessus.
        bool varies = false;
        const SynopticState reference = sky.synopticAt(0.0f);
        for (int day = 1; day <= 20 && !varies; ++day) {
            const SynopticState later = sky.synopticAt(static_cast<float>(day) * 24.0f);
            if (std::abs(later.cloudCover - reference.cloudCover) > 0.10f ||
                std::abs(later.seaLevelTemperatureC - reference.seaLevelTemperatureC) > 1.0f) {
                varies = true;
            }
        }
        check(varies, "Perturbations : le temps qu'il fait change d'un jour a l'autre");
    }

    // =======================================================================
    // 2. COUCHE 1 - cycles, soleil, perturbations
    // =======================================================================
    {
        // Cycle diurne : il fait plus chaud a l'heure de pointe qu'au petit
        // matin, le meme jour. Compare a NEBULOSITE EGALE, ce qui n'est possible
        // qu'en restant dans la meme journee - c'est pour ca que le test prend
        // deux heures du meme jour et non deux instants au hasard.
        const float day = 3.0f * 24.0f;
        const SynopticState afternoon = sky.synopticAt(day + defaults.diurnalPeakHour);
        const SynopticState dawn = sky.synopticAt(day + defaults.diurnalPeakHour - 12.0f);
        check(afternoon.seaLevelTemperatureC > dawn.seaLevelTemperatureC,
              "Cycle diurne : l'apres-midi est plus chaude que la fin de nuit");

        // Soleil : leve a l'est, couche a l'ouest, culmine au sud.
        const SynopticState noon = sky.synopticAt(12.0f);
        check(noon.sunElevationDegrees > 0.0f, "Soleil : haut a midi");
        check(noon.sunDirection.z > 0.9f * std::cos(glm::radians(noon.sunElevationDegrees)) - 1e-3f,
              "Soleil : plein SUD a midi (+Z dans la convention du projet)");
        check(noon.daylight > 0.8f, "Soleil : plein jour a midi");

        const SynopticState morning = sky.synopticAt(8.0f);
        check(morning.sunDirection.x > 0.0f, "Soleil : a l'EST le matin (+X)");
        const SynopticState evening = sky.synopticAt(18.0f);
        check(evening.sunDirection.x < 0.0f, "Soleil : a l'OUEST le soir (-X)");

        const SynopticState midnight = sky.synopticAt(0.0f);
        check(midnight.sunElevationDegrees < 0.0f, "Soleil : sous l'horizon a minuit");
        checkNear(midnight.daylight, 0.0f, 1e-6f, "Soleil : aucun ensoleillement de nuit");

        // Saison : le soleil monte plus haut a midi en juillet qu'en janvier.
        // Le jour de depart etant mi-mai, on saute de six mois pour comparer.
        WeatherParams winterParams = defaults;
        winterParams.startDayOfYear = 15.0f; // mi-janvier
        const WeatherSystem winter(winterParams);
        const SynopticState winterNoon = winter.synopticAt(12.0f);
        check(winterNoon.sunElevationDegrees < noon.sunElevationDegrees - 10.0f,
              "Saison : le soleil de janvier est bien plus bas que celui de mai");

        // Perturbations : sur un mois de jeu, il doit exister des journees
        // claires ET des journees de pluie. Un modele qui ne produirait jamais
        // de pluie - ou qui en produirait tout le temps - serait inutile.
        int rainySamples = 0;
        int clearSamples = 0;
        float maxWind = 0.0f;
        for (int h = 0; h < 30 * 24; ++h) {
            const SynopticState state = sky.synopticAt(static_cast<float>(h));
            if (state.precipitationMmPerHour > 0.1f) { ++rainySamples; }
            if (state.cloudCover < 0.20f) { ++clearSamples; }
            maxWind = std::max(maxWind, state.seaLevelWindMps);
            check(state.cloudCover >= 0.0f && state.cloudCover <= 1.0f,
                  "Nebulosite : toujours dans [0, 1]");
            check(state.seaLevelWindMps >= 0.0f, "Vent synoptique : jamais negatif");
        }
        check(rainySamples > 0, "Un mois de jeu contient des heures de pluie");
        check(clearSamples > 0, "Un mois de jeu contient des heures de grand beau");
        check(rainySamples < 30 * 24 / 2, "Il ne pleut pas la moitie du temps");
        check(maxWind > defaults.baseWindMps + 1.0f,
              "Le vent forcit reellement au passage des perturbations");
        std::printf("    [meteo] sur 30 jours simules : %d h de pluie, %d h de grand beau, "
                    "vent max %.1f m/s\n",
                    rainySamples, clearSamples, static_cast<double>(maxWind));
    }

    // =======================================================================
    // 3. COUCHE 2 - temperature locale
    // =======================================================================
    {
        // --- Gradient altitudinal ------------------------------------------
        // La seule egalite numerique du fichier, et elle est legitime : elle
        // compare le resultat a SON PROPRE parametre. Le jour ou le climatologue
        // changera 6,5, ce test continuera de passer - et c'est exactement ce
        // qu'on veut d'un test de mecanisme.
        const float when = 5.0f * 24.0f + 11.0f;
        const WeatherSample low = sky.sampleAt(glm::vec3(0.0f, 300.0f, 0.0f), when, nullptr);
        const WeatherSample high = sky.sampleAt(glm::vec3(0.0f, 1300.0f, 0.0f), when, nullptr);
        checkNear(low.temperatureC - high.temperatureC, defaults.lapseRateCPerKm, 1e-2f,
                  "Altitude : l'ecart vaut exactement le gradient annonce");
        check(high.lapseC < low.lapseC, "Altitude : le terme de gradient est bien celui qui mord");

        // --- Adret / ubac ---------------------------------------------------
        // MEME INSTANT, MEME ALTITUDE, seule l'orientation change. C'est la
        // propriete demandee par le cahier des charges, et elle est verifiee a
        // une heure ou le soleil est franchement au sud.
        NorthSouthSlope adretSlope(-0.60f); // descend vers le sud
        NorthSouthSlope ubacSlope(0.60f);   // descend vers le nord
        const float noonHours = 4.0f * 24.0f + 12.0f;
        const WeatherSample adret =
            sky.sampleAt(glm::vec3(0.0f, 1000.0f, 0.0f), noonHours, &adretSlope);
        const WeatherSample ubac =
            sky.sampleAt(glm::vec3(0.0f, 1000.0f, 0.0f), noonHours, &ubacSlope);
        check(adret.temperatureC > ubac.temperatureC,
              "Adret / ubac : le versant sud est MESURABLEMENT plus chaud a midi");
        check(adret.aspectC > ubac.aspectC,
              "Adret / ubac : l'ecart vient bien du terme d'exposition");
        std::printf("    [meteo] a midi, 1000 m : adret %.1f C, ubac %.1f C (ecart %.1f C)\n",
                    static_cast<double>(adret.temperatureC),
                    static_cast<double>(ubac.temperatureC),
                    static_cast<double>(adret.temperatureC - ubac.temperatureC));

        // Controle negatif : la nuit, l'orientation ne doit plus rien changer.
        const float nightHours = 4.0f * 24.0f + 2.0f;
        const WeatherSample adretNight =
            sky.sampleAt(glm::vec3(0.0f, 1000.0f, 0.0f), nightHours, &adretSlope);
        const WeatherSample ubacNight =
            sky.sampleAt(glm::vec3(0.0f, 1000.0f, 0.0f), nightHours, &ubacSlope);
        checkNear(adretNight.temperatureC, ubacNight.temperatureC, 1e-3f,
                  "La nuit : un versant sud n'a plus aucun avantage");

        // Le matin, c'est le versant EST qui prend le soleil - ce qu'un modele
        // purement nord/sud (celui de la Phase 16) ne pouvait pas produire.
        SlopedTerrain eastFacing(1000.0f, -0.60f); // descend vers l'est (+X)
        SlopedTerrain westFacing(1000.0f, 0.60f);
        const float morningHours = 4.0f * 24.0f + 7.5f;
        const WeatherSample east =
            sky.sampleAt(glm::vec3(0.0f, 1000.0f, 0.0f), morningHours, &eastFacing);
        const WeatherSample west =
            sky.sampleAt(glm::vec3(0.0f, 1000.0f, 0.0f), morningHours, &westFacing);
        check(east.temperatureC > west.temperatureC,
              "Le matin : le versant EST est plus chaud que le versant ouest");

        // --- Terrain absent : repli explicite -------------------------------
        const WeatherSample freeAir = sky.sampleAt(glm::vec3(0.0f, 1000.0f, 0.0f), noonHours, nullptr);
        checkNear(freeAir.aspectC, 0.0f, 1e-6f,
                  "Sans terrain : aucun effet de versant, et aucun plantage");
        check(freeAir.windMps > 0.0f, "Sans terrain : le vent synoptique reste disponible");
    }

    // =======================================================================
    // 4. CARTE DE PIEGEAGE D'AIR FROID ET INVERSION
    // =======================================================================
    {
        // Une cuvette de 600 m de rayon creusee dans un plateau : le creux doit
        // etre note comme piegeant, la bosse identique NON. C'est le meme
        // controle negatif que la Phase 16 employait pour les points d'eau, et
        // pour la meme raison - sans lui, une carte qui noterait "bas = piege"
        // passerait par accident.
        // Rebord a 150 m au-dessus du fond, atteint a 500 m du centre : les
        // proportions d'une vraie combe de moyenne montagne. La premiere
        // redaction employait 400 m de profondeur, et le test echouait pour une
        // raison qui vaut d'etre notee - a 400 m de denivele, le seul gradient
        // adiabatique rend deja le fond 2,6 degres PLUS CHAUD que le rebord, et
        // l'inversion, pourtant bien calculee, ne suffisait pas tout a fait a
        // renverser ce handicap. Ce n'etait pas un defaut du modele mais un
        // relief invraisemblable : une cuvette de 400 m de creux pour 500 m de
        // rayon est une falaise circulaire, pas une combe.
        BowlTerrain bowl(0.0f, 0.0f, 0.0006f, 150.0f);
        DomeTerrain dome(0.0f, 0.0f, 0.0006f, 0.0f);

        const ColdAirTrapMap bowlMap(bowl, 2000.0f, 48);
        const ColdAirTrapMap domeMap(dome, 2000.0f, 48);

        check(!bowlMap.empty(), "Carte de piegeage : construite");
        check(bowlMap.at(0.0f, 0.0f) > 0.5f,
              "Piegeage : le fond de la cuvette est fortement enferme");
        check(domeMap.at(0.0f, 0.0f) < 0.05f,
              "Piegeage : le sommet d'une bosse ne piege rien (controle negatif)");
        check(bowlMap.at(0.0f, 0.0f) > bowlMap.at(1800.0f, 1800.0f) + 0.3f,
              "Piegeage : le fond piege bien plus que le plateau qui l'entoure");
        std::printf("    [meteo] carte de piegeage : creux %.2f, plateau %.2f, bosse %.2f\n",
                    static_cast<double>(bowlMap.at(0.0f, 0.0f)),
                    static_cast<double>(bowlMap.at(1800.0f, 1800.0f)),
                    static_cast<double>(domeMap.at(0.0f, 0.0f)));

        // Carte degeneree : ne doit ni planter ni mentir.
        const ColdAirTrapMap tiny(bowl, 0.0f, 64);
        check(tiny.empty() && tiny.at(0.0f, 0.0f) == 0.0f,
              "Carte de piegeage : une emprise nulle rend une carte vide, pas un plantage");

        // --- L'inversion elle-meme ------------------------------------------
        // Le fond de cuvette doit etre plus froid que le plateau LA NUIT, alors
        // qu'il est PLUS BAS - donc plus chaud par le seul gradient. C'est la
        // seule facon de prouver que l'inversion existe : elle doit RENVERSER le
        // signe du gradient, pas simplement s'y ajouter.
        WeatherSystem inverted;
        inverted.setTrapMap(&bowlMap);

        // On cherche une nuit calme et claire dans les dix premiers jours : le
        // phenomene ne s'installe que par ciel degage et sans vent, et exiger
        // qu'il se produise a une date arbitraire serait exiger que la meteo
        // soit celle qui arrange le test.
        // Le critere ne porte QUE sur la nebulosite : le vent, lui, est deja
        // traite par le modele au bon endroit - un fond de cuvette est abrite
        // par construction, donc le vent y est faible meme quand le vent
        // synoptique ne l'est pas. Une premiere version de ce test exigeait
        // aussi un vent synoptique faible et ne trouvait jamais de nuit
        // propice : le vent de base ne descend jamais sous baseWindMps.
        bool inversionSeen = false;
        float bestGap = 0.0f;
        for (int day = 0; day < 20 && !inversionSeen; ++day) {
            const float t = static_cast<float>(day) * 24.0f + 3.0f;
            const SynopticState state = inverted.synopticAt(t);
            if (state.cloudCover > 0.25f) {
                continue;
            }
            const WeatherSample hollow =
                inverted.sampleAt(glm::vec3(0.0f, bowl.heightAt(0.0f, 0.0f), 0.0f), t, &bowl);
            const WeatherSample plateau = inverted.sampleAt(
                glm::vec3(1800.0f, bowl.heightAt(1800.0f, 1800.0f), 1800.0f), t, &bowl);
            if (hollow.inversionC < -0.5f) {
                inversionSeen = true;
                bestGap = plateau.temperatureC - hollow.temperatureC;
                check(hollow.temperatureC < plateau.temperatureC,
                      "Inversion : par nuit claire et calme, le fond est plus FROID que le plateau");
                std::printf("    [meteo] inversion nocturne : fond %.1f C, plateau %.1f C "
                            "(le fond est pourtant %.0f m plus bas)\n",
                            static_cast<double>(hollow.temperatureC),
                            static_cast<double>(plateau.temperatureC),
                            static_cast<double>(bowl.heightAt(1800.0f, 1800.0f) -
                                                bowl.heightAt(0.0f, 0.0f)));
            }
        }
        check(inversionSeen, "Inversion : une nuit propice existe dans les vingt premiers jours");
        check(bestGap > 0.0f || !inversionSeen, "Inversion : l'ecart est bien dans le bon sens");

        // De JOUR, au meme endroit, l'inversion doit avoir disparu.
        const WeatherSample daytime =
            inverted.sampleAt(glm::vec3(0.0f, bowl.heightAt(0.0f, 0.0f), 0.0f), 13.0f, &bowl);
        checkNear(daytime.inversionC, 0.0f, 1e-3f,
                  "Inversion : rien en plein jour (controle negatif)");

        // Sans carte reliee, pas d'inversion - et aucun plantage.
        const WeatherSample noMap =
            sky.sampleAt(glm::vec3(0.0f, 400.0f, 0.0f), 3.0f, &bowl);
        checkNear(noMap.inversionC, 0.0f, 1e-6f,
                  "Sans carte de piegeage : l'inversion vaut zero, proprement");
    }

    // =======================================================================
    // 5. VENT LOCAL - SONDAGE AMONT
    // =======================================================================
    {
        RidgeWithColTerrain ridge(800.0f, 400.0f, 300.0f, 0.60f, 400.0f);

        // Vent d'OUEST impose : le flux traverse la crete d'ouest en est. On fixe
        // la direction au lieu de la subir, parce qu'un test sur la reponse du
        // relief ne doit pas dependre de l'humeur du bruit synoptique ce jour-la.
        WeatherParams westerly;
        westerly.meanWindFromDegrees = 270.0f;
        westerly.windDirectionSwingDegrees = 0.0f;
        const WeatherSystem west(westerly);

        const float t = 6.0f * 24.0f + 10.0f;
        const SynopticState state = west.synopticAt(t);
        checkNear(state.windFromDegrees, 270.0f, 1e-3f, "Vent : direction d'ouest imposee");

        // Le vecteur vent doit POINTER vers l'est : le vent d'ouest va vers l'est.
        const WeatherSample anywhere =
            west.sampleAt(glm::vec3(-2500.0f, 800.0f, 0.0f), state, &ridge);
        check(anywhere.windVector.x > 0.0f,
              "Vent d'ouest : l'air se deplace vers l'EST (+X)");

        // --- COL contre PLATEAU : la propriete centrale de cette section ------
        const float colY = ridge.heightAt(0.0f, 0.0f);
        const WeatherSample col = west.sampleAt(glm::vec3(0.0f, colY, 0.0f), state, &ridge);

        const float plateauY = ridge.heightAt(-2500.0f, 0.0f);
        const WeatherSample plateau =
            west.sampleAt(glm::vec3(-2500.0f, plateauY, 0.0f), state, &ridge);

        check(col.windMps > 1.5f * plateau.windMps,
              "Col : le vent y est nettement plus fort que sur le plateau adjacent");
        std::printf("    [meteo] vent : col %.1f m/s, plateau %.1f m/s (x%.2f)\n",
                    static_cast<double>(col.windMps), static_cast<double>(plateau.windMps),
                    static_cast<double>(col.windMps / std::max(0.01f, plateau.windMps)));

        // Le col doit aussi venter plus que la crete PLEINE, a vent de reference
        // egal. La crete pleine etant plus HAUTE que le col, comparer les
        // vitesses brutes ne dirait rien : le vent croit deja avec l'altitude.
        // On compare donc les FACTEURS - le vent local divise par le vent en air
        // libre a la meme altitude. C'est la seule facon d'isoler l'effet
        // d'entonnoir de l'effet d'altitude, et la premiere redaction de ce test
        // ne le faisait pas : elle cherchait un point de crete a altitude egale,
        // n'en trouvait aucun (il n'y en a pas, par construction du relief) et se
        // sautait donc en silence.
        const auto freeAirWind = [&](float y) {
            return state.seaLevelWindMps + westerly.windAltitudeGainMpsPerKm * y / 1000.0f;
        };
        const float crestY = ridge.heightAt(0.0f, 1600.0f);
        const WeatherSample onRidge =
            west.sampleAt(glm::vec3(0.0f, crestY, 1600.0f), state, &ridge);
        const float colFactor = col.windMps / freeAirWind(colY);
        const float crestFactor = onRidge.windMps / freeAirWind(crestY);
        check(colFactor > crestFactor,
              "Venturi : a vent de reference egal, le col accelere plus que la crete pleine");
        std::printf("    [meteo] facteur de relief : col x%.2f, crete pleine x%.2f\n",
                    static_cast<double>(colFactor), static_cast<double>(crestFactor));

        // --- Versant au vent contre versant sous le vent ---------------------
        // Meme altitude, meme instant, de part et d'autre du sommet.
        const float flankY = ridge.heightAt(-250.0f, 1500.0f);
        const WeatherSample windward =
            west.sampleAt(glm::vec3(-250.0f, flankY, 1500.0f), state, &ridge);
        const WeatherSample lee =
            west.sampleAt(glm::vec3(250.0f, flankY, 1500.0f), state, &ridge);

        check(windward.ridgeLiftMps > 0.5f,
              "Ridge lift : le versant AU VENT porte reellement");
        checkNear(lee.ridgeLiftMps, 0.0f, 1e-6f,
                  "Ridge lift : le versant SOUS LE VENT ne porte pas");
        check(lee.subsidenceMps < 0.0f,
              "Sous le vent : l'air redescend (subsidence)");
        check(lee.turbulence > windward.turbulence,
              "Sous le vent : l'air est plus turbulent qu'au vent");
        std::printf("    [meteo] versants a %.0f m : au vent +%.2f m/s (turb %.2f), "
                    "sous le vent %.2f m/s (turb %.2f)\n",
                    static_cast<double>(flankY),
                    static_cast<double>(windward.ridgeLiftMps),
                    static_cast<double>(windward.turbulence),
                    static_cast<double>(lee.subsidenceMps),
                    static_cast<double>(lee.turbulence));

        // Sans vent, pas de turbulence et pas de portance : le controle negatif
        // qui prouve que ces deux termes sont bien pilotes par le vent.
        WeatherParams calmParams = westerly;
        calmParams.baseWindMps = 0.0f;
        calmParams.frontWindAmplitudeMps = 0.0f;
        calmParams.windAltitudeGainMpsPerKm = 0.0f;
        const WeatherSystem calm(calmParams);
        const WeatherSample stillAir =
            calm.sampleAt(glm::vec3(250.0f, flankY, 1500.0f), t, &ridge);
        checkNear(stillAir.turbulence, 0.0f, 1e-6f, "Air calme : aucune turbulence de relief");
        checkNear(stillAir.ridgeLiftMps, 0.0f, 1e-6f, "Air calme : aucune portance de pente");
        checkNear(stillAir.subsidenceMps, 0.0f, 1e-6f, "Air calme : aucune subsidence");

        // Ecretage : meme dans le pire cumul, le vent reste sous le garde-fou.
        for (int h = 0; h < 15 * 24; ++h) {
            const WeatherSample s =
                west.sampleAt(glm::vec3(0.0f, colY, 0.0f), static_cast<float>(h), &ridge);
            check(s.windMps <= westerly.maxLocalWindMps + 1e-3f,
                  "Vent local : jamais au-dessus du garde-fou");
        }

        // --- Thermiques ------------------------------------------------------
        // Sur un adret ensoleille en debut d'apres-midi, ca doit monter ; la
        // meme pente a 3 h du matin, non.
        NorthSouthSlope adretSlope(-0.60f);
        WeatherParams clearParams;
        clearParams.meanCloudCover = 0.0f;
        clearParams.cloudAmplitude = 0.0f;
        const WeatherSystem clearSky(clearParams);

        const WeatherSample thermalNoon = clearSky.sampleAt(
            glm::vec3(0.0f, 1200.0f, 0.0f), clearParams.thermalPeakHour, &adretSlope);
        const WeatherSample thermalNight =
            clearSky.sampleAt(glm::vec3(0.0f, 1200.0f, 0.0f), 3.0f, &adretSlope);
        check(thermalNoon.thermalMps > 1.0f,
              "Thermiques : un adret ensoleille produit une vraie ascendance");
        checkNear(thermalNight.thermalMps, 0.0f, 1e-6f,
                  "Thermiques : rien la nuit (controle negatif)");

        // Sous un ciel couvert, la meme pente au meme instant doit produire
        // beaucoup moins - c'est le mecanisme reel, et c'est aussi ce qui relie
        // les thermiques a la nebulosite plutot qu'a la seule heure.
        WeatherParams overcastParams = clearParams;
        overcastParams.meanCloudCover = 1.0f;
        const WeatherSystem overcast(overcastParams);
        const WeatherSample thermalDull = overcast.sampleAt(
            glm::vec3(0.0f, 1200.0f, 0.0f), overcastParams.thermalPeakHour, &adretSlope);
        check(thermalDull.thermalMps < 0.5f * thermalNoon.thermalMps,
              "Thermiques : un ciel couvert les etouffe");

        // Et l'ubac, au meme instant, doit produire moins que l'adret.
        NorthSouthSlope ubacSlope(0.60f);
        const WeatherSample thermalUbac = clearSky.sampleAt(
            glm::vec3(0.0f, 1200.0f, 0.0f), clearParams.thermalPeakHour, &ubacSlope);
        check(thermalUbac.thermalMps < thermalNoon.thermalMps,
              "Thermiques : l'ubac declenche moins que l'adret");
    }

    // =======================================================================
    // 6. PRECIPITATIONS ET MOUILLURE
    // =======================================================================
    {
        RidgeWithColTerrain ridge(800.0f, 400.0f, 300.0f, 0.60f, 400.0f);

        // Ciel forcement bouche : on veut mesurer la MODULATION par le relief,
        // pas attendre qu'il pleuve.
        WeatherParams wetParams;
        wetParams.meanWindFromDegrees = 270.0f;
        wetParams.windDirectionSwingDegrees = 0.0f;
        wetParams.meanCloudCover = 1.0f;
        wetParams.cloudAmplitude = 0.0f;
        const WeatherSystem storm(wetParams);

        const float t = 2.0f * 24.0f + 15.0f;
        const float flankY = ridge.heightAt(-250.0f, 1500.0f);
        const WeatherSample windward =
            storm.sampleAt(glm::vec3(-250.0f, flankY, 1500.0f), t, &ridge);
        const WeatherSample lee =
            storm.sampleAt(glm::vec3(250.0f, flankY, 1500.0f), t, &ridge);

        check(windward.precipitationMmPerHour > 0.0f, "Il pleut sous un ciel entierement couvert");
        check(windward.precipitationMmPerHour > lee.precipitationMmPerHour,
              "Barriere orographique : il pleut plus sur le versant AU VENT");
        check(windward.cloudCover >= lee.cloudCover,
              "Le versant au vent accroche aussi plus de nuages");

        // Mouillure : monotone et bornee, seche quand il ne pleut plus.
        check(windward.wetness > 0.5f, "Mouillure : le sol est trempe sous la pluie");
        for (int h = 0; h < 200; ++h) {
            const WeatherSample s =
                storm.sampleAt(glm::vec3(-250.0f, flankY, 1500.0f), static_cast<float>(h), &ridge);
            check(s.wetness >= 0.0f && s.wetness <= 1.0f, "Mouillure : toujours dans [0, 1]");
        }

        // Un ciel qui ne pleut jamais laisse un sol sec - controle negatif.
        WeatherParams dryParams = wetParams;
        dryParams.meanCloudCover = 0.0f;
        const WeatherSystem drought(dryParams);
        const WeatherSample dry =
            drought.sampleAt(glm::vec3(-250.0f, flankY, 1500.0f), t, &ridge);
        checkNear(dry.wetness, 0.0f, 1e-6f, "Mouillure : sol sec quand il ne pleut jamais");

        // La mouillure ne depend PAS du chemin : la relire a un instant donne
        // rend toujours la meme chose, qu'on y arrive en une fois ou pas du tout.
        const WeatherSample again =
            storm.sampleAt(glm::vec3(-250.0f, flankY, 1500.0f), t, &ridge);
        check(again.wetness == windward.wetness,
              "Mouillure : relue, pas accumulee - donc identique a l'identique");

        // Neige : au-dessus d'une certaine altitude, la meme precipitation tombe
        // en neige. On monte tres haut pour que ce soit vrai quelle que soit la
        // saison tiree.
        const WeatherSample summit =
            storm.sampleAt(glm::vec3(0.0f, 4000.0f, 0.0f), t, nullptr);
        check(summit.snowing, "Neige : a 4000 m, la precipitation tombe en neige");
        check(!windward.snowing || windward.temperatureC <= wetParams.snowThresholdC,
              "Neige : jamais annoncee au-dessus du seuil de temperature");
    }

    // =======================================================================
    // 7. REFROIDISSEMENT EOLIEN (formule publiee, PAS provisoire)
    // =======================================================================
    {
        // Valeur de reference publiee : -5 degres par 30 km/h de vent donne
        // environ -13. On verifie a un degre pres, la formule etant exacte.
        checkNear(windChillC(-5.0f, 30.0f / 3.6f), -13.0f, 1.0f,
                  "Refroidissement eolien : -5 C par 30 km/h donne environ -13 C");

        check(windChillC(0.0f, 10.0f) < 0.0f, "Vent : le ressenti descend sous l'ambiante");
        check(windChillC(0.0f, 20.0f) < windChillC(0.0f, 10.0f),
              "Vent : plus il souffle, plus il refroidit");
        check(windChillC(-10.0f, 10.0f) < windChillC(0.0f, 10.0f),
              "Vent : a vent egal, plus il fait froid plus le ressenti est bas");

        // Hors domaine : la fonction rend la temperature INCHANGEE plutot qu'une
        // extrapolation fausse. C'est le point le plus facile a rater dans une
        // implementation pressee - au-dessus de 10 degres la formule brute
        // rendrait un vent RECHAUFFANT.
        checkNear(windChillC(25.0f, 15.0f), 25.0f, 1e-6f,
                  "Hors domaine : au-dessus de 10 C, le vent ne rechauffe pas");
        checkNear(windChillC(0.0f, 0.5f), 0.0f, 1e-6f,
                  "Hors domaine : un souffle d'air ne refroidit pas");
        checkNear(windChillC(0.0f, 0.0f), 0.0f, 1e-6f, "Vent nul : aucun effet");
    }

    // =======================================================================
    // 8. ROBUSTESSE ET COUT
    // =======================================================================
    {
        RidgeWithColTerrain ridge(800.0f, 400.0f, 300.0f, 0.60f, 400.0f);

        // Entrees degenerees : le module ne doit jamais rendre de NaN, parce
        // qu'un NaN de temperature contaminerait les quatre jauges de Needs puis
        // la vitesse du joueur (voir le commentaire de clamp01 dans Needs.cpp).
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        const glm::vec3 broken(nan, inf, -nan);
        const WeatherSample wrecked = sky.sampleAt(broken, nan, &ridge);
        check(std::isfinite(wrecked.temperatureC) && std::isfinite(wrecked.windMps) &&
                  std::isfinite(wrecked.verticalMps) && std::isfinite(wrecked.wetness),
              "Robustesse : des entrees non finies ne produisent aucun NaN");

        const WeatherSample past = sky.sampleAt(glm::vec3(0.0f, 900.0f, 0.0f), -500.0f, &ridge);
        check(std::isfinite(past.temperatureC),
              "Robustesse : un instant negatif reste calculable");

        // --- COUT ------------------------------------------------------------
        // Budget de reference de la Phase 19 : le frame est domine par le GPU
        // (81 % dans drawFrame, en Release). Le seuil ci-dessous est TRES large
        // et volontairement : ce test protege contre un facteur mille - un
        // sondage qui deviendrait une recherche, une carte recalculee par image -
        // et non contre une variation de 20 %, que le bruit de mesure de cette
        // machine ne permet de toute facon pas de distinguer (voir la lecon de
        // la Phase 17 sur les FPS bruites).
        constexpr int kEvaluations = 20000;
        const auto start = std::chrono::steady_clock::now();
        double sink = 0.0;
        for (int i = 0; i < kEvaluations; ++i) {
            const float t = static_cast<float>(i) * 0.01f;
            const glm::vec3 p(static_cast<float>(i % 500) * 4.0f, 900.0f,
                              static_cast<float>(i % 337) * 6.0f);
            sink += sky.sampleAt(p, t, &ridge).temperatureC;
        }
        const double micros = std::chrono::duration<double, std::micro>(
                                  std::chrono::steady_clock::now() - start).count();
        const double perCall = micros / kEvaluations;
        std::printf("    [meteo] cout : %.2f us par evaluation complete "
                    "(%d evaluations, terrain analytique, somme %.0f)\n",
                    perCall, kEvaluations, sink);
        check(perCall < 50.0,
              "Cout : une evaluation meteo complete reste tres loin d'un budget d'image");

        // La couche 1 seule doit etre nettement moins chere que la couche 2 :
        // c'est ce qui justifie la surcharge sampleAt(position, SynopticState,
        // terrain), celle qu'il faut appeler pour plusieurs points au meme
        // instant.
        const auto startSky = std::chrono::steady_clock::now();
        double skySink = 0.0;
        for (int i = 0; i < kEvaluations; ++i) {
            skySink += sky.synopticAt(static_cast<float>(i) * 0.01f).seaLevelTemperatureC;
        }
        const double skyMicros = std::chrono::duration<double, std::micro>(
                                     std::chrono::steady_clock::now() - startSky).count();
        std::printf("    [meteo] cout : %.2f us pour la couche synoptique seule (somme %.0f)\n",
                    skyMicros / kEvaluations, skySink);
    }
}

// ===========================================================================
// PHASE 23 - Ce que la meteo change dans les quatre activites
// ===========================================================================
// Meme regle que testWeather() : on verifie des PROPRIETES et des SIGNES, pas
// des valeurs. Et systematiquement par COMPARAISON de deux simulations
// identiques a un parametre pres - c'est la seule facon de mesurer un effet
// sans dependre du reglage exact des constantes provisoires.
//
// Chaque bloc porte aussi son CONTROLE NEGATIF : sans meteo, le comportement
// doit etre exactement celui d'avant la Phase 23. C'est ce qui prouve que les
// valeurs par defaut des nouveaux parametres sont bien neutres, et donc que la
// non-regression des Phases 8, 13 et 14 n'est pas un hasard.
void testWeatherGameplay() {
    std::printf("\n--- Phase 23 : effets meteo sur les activites ---\n");

    // =======================================================================
    // PARAPENTE
    // =======================================================================
    {
        constexpr float dt = 1.0f / 60.0f;
        constexpr int kSteps = 60 * 30; // 30 s de vol

        // --- Controle negatif : air calme = vol de la Phase 8 ---------------
        Paraglider reference;
        Paraglider withDefaultAir;
        reference.launch(glm::vec3(0.0f, 1500.0f, 0.0f), 0.0f);
        withDefaultAir.launch(glm::vec3(0.0f, 1500.0f, 0.0f), 0.0f);
        for (int i = 0; i < kSteps; ++i) {
            reference.update(dt, 0.0f, 0.0f);
            withDefaultAir.update(dt, 0.0f, 0.0f, AirState{});
        }
        check(reference.position() == withDefaultAir.position(),
              "Parapente : AirState par defaut = air calme = vol d'avant la Phase 23");

        // --- Thermique : l'aile MONTE ---------------------------------------
        // Une ascendance plus forte que le taux de chute doit faire gagner de
        // l'altitude. C'est la definition du vol thermique, et rien dans le
        // modele ne la code explicitement : elle sort de l'addition.
        Paraglider soaring;
        soaring.launch(glm::vec3(0.0f, 1500.0f, 0.0f), 0.0f);
        AirState thermal;
        thermal.vertical = 3.0f; // > cruiseSinkRate (1,25)
        for (int i = 0; i < kSteps; ++i) {
            soaring.update(dt, 0.0f, 0.0f, thermal);
        }
        check(soaring.position().y > 1500.0f,
              "Parapente : dans une thermique, l'aile gagne de l'altitude");
        check(soaring.climbRateMps() > 0.0f, "Parapente : le vario est positif en thermique");
        check(soaring.position().y > reference.position().y + 50.0f,
              "Parapente : la thermique change vraiment le vol, elle ne le nuance pas");
        std::printf("    [vol] 30 s : air calme %.0f m, thermique 3 m/s %.0f m\n",
                    static_cast<double>(reference.position().y),
                    static_cast<double>(soaring.position().y));

        // --- Subsidence : l'aile descend PLUS vite --------------------------
        Paraglider sinking;
        sinking.launch(glm::vec3(0.0f, 1500.0f, 0.0f), 0.0f);
        AirState lee;
        lee.vertical = -2.5f;
        for (int i = 0; i < kSteps; ++i) {
            sinking.update(dt, 0.0f, 0.0f, lee);
        }
        check(sinking.position().y < reference.position().y - 50.0f,
              "Parapente : sous le vent, la subsidence coute cher");

        // --- Vent : la derive ------------------------------------------------
        // Cap 0 = +X. Un vent d'ouest (qui pousse vers +X) doit allonger le vol
        // vers l'est ; le meme vent, cap oppose, doit le raccourcir. Les deux
        // sens sont testes : un modele qui ajouterait la NORME du vent passerait
        // le premier test et echouerait le second.
        AirState westerly;
        westerly.wind = glm::vec3(6.0f, 0.0f, 0.0f);

        Paraglider downwind;
        Paraglider upwind;
        downwind.launch(glm::vec3(0.0f, 1500.0f, 0.0f), 0.0f);   // vers +X
        upwind.launch(glm::vec3(0.0f, 1500.0f, 0.0f), 180.0f);   // vers -X
        for (int i = 0; i < kSteps; ++i) {
            downwind.update(dt, 0.0f, 0.0f, westerly);
            upwind.update(dt, 0.0f, 0.0f, westerly);
        }
        check(downwind.position().x > reference.position().x + 100.0f,
              "Parapente : vent dans le dos, on va nettement plus loin");
        check(upwind.position().x > -reference.position().x,
              "Parapente : vent de face, on avance nettement moins");
        std::printf("    [vol] 30 s, vent 6 m/s : vent arriere %.0f m, vent de face %.0f m, "
                    "air calme %.0f m\n",
                    static_cast<double>(downwind.position().x),
                    static_cast<double>(std::abs(upwind.position().x)),
                    static_cast<double>(reference.position().x));

        // Le vent ne doit PAS changer l'altitude : c'est un vent horizontal.
        checkNear(downwind.position().y, reference.position().y, 1e-2f,
                  "Parapente : un vent horizontal ne fait ni monter ni descendre");

        // --- Turbulence ------------------------------------------------------
        Paraglider tossed;
        tossed.launch(glm::vec3(0.0f, 1500.0f, 0.0f), 0.0f);
        AirState rough;
        rough.turbulence = 1.0f;
        float maxHeadingDrift = 0.0f;
        for (int i = 0; i < kSteps; ++i) {
            tossed.update(dt, 0.0f, 0.0f, rough);
            maxHeadingDrift = std::max(maxHeadingDrift, std::abs(tossed.headingDegrees()));
        }
        check(maxHeadingDrift > 1.0f,
              "Parapente : la turbulence fait reellement bouger le cap");
        check(maxHeadingDrift < 90.0f,
              "Parapente : la turbulence chahute, elle ne met pas en vrille");
        check(tossed.position().y < reference.position().y,
              "Parapente : l'air brasse coute de la finesse");

        // Determinisme du vol turbulent : deux vols identiques doivent donner
        // exactement le meme resultat. Sans ca, un vol ne serait pas rejouable,
        // et toute la Phase 23 perdrait sa propriete centrale.
        Paraglider twin;
        twin.launch(glm::vec3(0.0f, 1500.0f, 0.0f), 0.0f);
        for (int i = 0; i < kSteps; ++i) {
            twin.update(dt, 0.0f, 0.0f, rough);
        }
        check(twin.position() == tossed.position() &&
                  twin.headingDegrees() == tossed.headingDegrees(),
              "Parapente : meme vol turbulent, meme trajectoire (deterministe)");
    }

    // =======================================================================
    // ESCALADE - adherence de la paroi
    // =======================================================================
    {
        CliffTerrain cliff(0.0f, 3.0f, 0.0f, 10.0f);
        constexpr float dt = 1.0f / 60.0f;
        constexpr int kSteps = 60 * 20; // 20 s de montee

        const auto climbFor = [&](float grip, float* stamina) {
            Climber climber;
            climber.tryStart(cliff, glm::vec3(-1.5f, 1.7f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            for (int i = 0; i < kSteps && climber.climbing(); ++i) {
                climber.update(dt, 1.0f, 0.0f, cliff, nullptr, grip);
            }
            if (stamina) { *stamina = climber.stamina(); }
            return climber.heightGained();
        };

        float dryStamina = 0.0f;
        float wetStamina = 0.0f;
        const float dryRise = climbFor(1.0f, &dryStamina);
        const float wetRise = climbFor(Climber{}.params().wetGrip, &wetStamina);

        check(dryRise > 0.0f, "Escalade : on monte bien sur rocher sec");
        check(wetRise < dryRise,
              "Escalade : sur paroi mouillee, on progresse MESURABLEMENT moins vite");

        // Le cout par METRE monte, ce qui est la partie qui compte : une paroi
        // mouillee n'est pas seulement plus lente, elle est plus chere. Un
        // modele qui ne ferait que ralentir laisserait la jauge dire le
        // contraire - moins de metres, donc moins d'energie depensee.
        const float dryCost = (1.0f - dryStamina) / std::max(0.01f, dryRise);
        const float wetCost = (1.0f - wetStamina) / std::max(0.01f, wetRise);
        check(wetCost > dryCost,
              "Escalade : chaque metre de paroi mouillee coute plus cher que sur du sec");
        std::printf("    [escalade] 20 s : sec %.1f m (%.3f de jauge/m), "
                    "mouille %.1f m (%.3f de jauge/m)\n",
                    static_cast<double>(dryRise), static_cast<double>(dryCost),
                    static_cast<double>(wetRise), static_cast<double>(wetCost));

        // Controle negatif : sans argument de grip, l'escalade est celle de la
        // Phase 14.
        Climber withGrip;
        Climber withoutGrip;
        withGrip.tryStart(cliff, glm::vec3(-1.5f, 1.7f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        withoutGrip.tryStart(cliff, glm::vec3(-1.5f, 1.7f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        for (int i = 0; i < 600; ++i) {
            withGrip.update(dt, 1.0f, 0.0f, cliff, nullptr, 1.0f);
            withoutGrip.update(dt, 1.0f, 0.0f, cliff, nullptr);
        }
        check(withGrip.anchor() == withoutGrip.anchor() &&
                  withGrip.stamina() == withoutGrip.stamina(),
              "Escalade : grip par defaut = rocher sec = escalade d'avant la Phase 23");

        // La traduction humidite -> adherence est monotone et bornee.
        Climber sample;
        check(sample.gripForWetness(0.0f) == 1.0f, "Adherence : sec = 1");
        check(sample.gripForWetness(1.0f) < 1.0f, "Adherence : trempe < sec");
        check(sample.gripForWetness(0.5f) < sample.gripForWetness(0.0f) &&
                  sample.gripForWetness(0.5f) > sample.gripForWetness(1.0f),
              "Adherence : monotone entre les deux");
        check(sample.gripForWetness(2.0f) == sample.gripForWetness(1.0f) &&
                  sample.gripForWetness(-1.0f) == sample.gripForWetness(0.0f),
              "Adherence : une humidite hors bornes est ecretee, pas extrapolee");

        // Une paroi ruisselante reste GRIMPABLE : le plancher d'adherence existe
        // pour qu'un joueur ne se retrouve jamais bloque en paroi, ni montant ni
        // descendant.
        check(wetRise > 0.0f, "Escalade : une paroi trempee se grimpe encore, lentement");
    }

    // =======================================================================
    // VELO - sol detrempe
    // =======================================================================
    {
        FlatTerrain flat;

        // Vitesse d'equilibre en pedalant a fond, sec contre trempe.
        const auto settleWet = [&](float wetness) {
            Bike bike;
            bike.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
            float previous = -1.0f;
            for (int i = 0; i < 60 * 120; ++i) {
                bike.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, flat, nullptr, wetness);
                if (std::abs(bike.speed() - previous) < 1e-3f && i > 60) { break; }
                previous = bike.speed();
            }
            return bike.speed();
        };

        const float drySpeed = settleWet(0.0f);
        const float wetSpeed = settleWet(1.0f);
        check(wetSpeed < drySpeed,
              "Velo : sur sol detrempe, la vitesse d'equilibre est plus basse");
        check(wetSpeed > 0.5f * drySpeed,
              "Velo : le sol mouille freine, il n'immobilise pas");
        std::printf("    [velo] vitesse d'equilibre a plat : sec %.1f km/h, trempe %.1f km/h\n",
                    static_cast<double>(drySpeed * 3.6f),
                    static_cast<double>(wetSpeed * 3.6f));

        // Freinage : depuis la meme vitesse, la meme commande de frein pendant la
        // meme duree doit laisser PLUS de vitesse sur sol mouille.
        const auto brakeFrom = [&](float wetness) {
            Bike bike;
            bike.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
            for (int i = 0; i < 60 * 60; ++i) {
                bike.update(1.0f / 60.0f, 0.0f, 1.0f, 0.0f, flat, nullptr, 0.0f);
            }
            const float entry = bike.speed();
            for (int i = 0; i < 30; ++i) { // une demi-seconde de frein
                bike.update(1.0f / 60.0f, 0.0f, 0.0f, 1.0f, flat, nullptr, wetness);
            }
            return entry - bike.speed();
        };
        const float dryLoss = brakeFrom(0.0f);
        const float wetLoss = brakeFrom(1.0f);
        check(wetLoss < dryLoss,
              "Velo : sur sol detrempe, une demi-seconde de frein enleve moins de vitesse");
        std::printf("    [velo] 0,5 s de frein : sec -%.1f km/h, trempe -%.1f km/h\n",
                    static_cast<double>(dryLoss * 3.6f), static_cast<double>(wetLoss * 3.6f));

        // Controle negatif : sans argument, la physique est celle de la Phase 13.
        Bike explicitDry;
        Bike implicitDry;
        explicitDry.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        implicitDry.mount(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, flat);
        for (int i = 0; i < 600; ++i) {
            explicitDry.update(1.0f / 60.0f, 0.2f, 1.0f, 0.0f, flat, nullptr, 0.0f);
            implicitDry.update(1.0f / 60.0f, 0.2f, 1.0f, 0.0f, flat, nullptr);
        }
        check(explicitDry.position() == implicitDry.position() &&
                  explicitDry.speed() == implicitDry.speed(),
              "Velo : humidite par defaut = sol sec = physique d'avant la Phase 23");
    }

    // =======================================================================
    // LUMIERE DU JOUR - le cycle jour/nuit visuel
    // =======================================================================
    {
        const WeatherSystem sky;
        const WeatherParams params;

        const SunLight noon = sunLightFor(sky.synopticAt(12.0f), params);
        const SunLight midnight = sunLightFor(sky.synopticAt(0.0f), params);
        check(noon.intensity > 0.9f, "Lumiere : pleine intensite a midi");
        checkNear(midnight.intensity, 0.0f, 1e-6f, "Lumiere : aucune lumiere directe a minuit");

        // L'AUBE EST UNE TRANSITION, pas un interrupteur : il doit exister des
        // instants ou la lumiere directe est partielle. On les CHERCHE au lieu
        // de parier sur une heure : l'heure du lever depend de la saison, et une
        // premiere redaction de ce test interrogeait 6 h 30 - qui est deja le
        // plein jour a la mi-mai au 45e parallele.
        int partialQuarters = 0;
        for (int i = 0; i < 24 * 12; ++i) {
            const float intensity =
                sunLightFor(sky.synopticAt(static_cast<float>(i) / 12.0f), params).intensity;
            if (intensity > 0.02f && intensity < 0.98f) { ++partialQuarters; }
        }
        check(partialQuarters >= 4,
              "Lumiere : l'aube et le crepuscule durent, ce n'est pas un interrupteur");
        std::printf("    [lumiere] %d intervalles de 5 min en lumiere partielle sur 24 h\n",
                    partialQuarters);

        check(noon.direction.y > 0.5f, "Lumiere : le soleil de midi vient d'en haut");
        check(midnight.direction.y > 0.9f,
              "Lumiere : la nuit, la direction bascule a la verticale (rien n'est eclaire "
              "par en dessous)");

        check(midnight.ambient > 0.0f,
              "Lumiere : la nuit n'est pas noire - choix de jouabilite assume");
        check(midnight.ambient < noon.ambient,
              "Lumiere : la nuit est tout de meme nettement plus sombre que le jour");
        checkNear(noon.ambient, 1.0f, 1e-3f,
                  "Lumiere : en plein jour, l'ambiante des shaders est inchangee (x1)");

        // La direction de la lumiere DOIT etre celle du soleil de la meteo : un
        // second soleil qui divergerait est exactement ce que cette phase
        // corrige.
        const SynopticState afternoon = sky.synopticAt(15.0f);
        const SunLight afternoonLight = sunLightFor(afternoon, params);
        checkNear(glm::dot(afternoonLight.direction, afternoon.sunDirection), 1.0f, 1e-3f,
                  "Lumiere : c'est LE soleil de la meteo, pas un second soleil");

        // Le soleil traverse bien le ciel : est le matin, ouest le soir.
        const SunLight morningLight = sunLightFor(sky.synopticAt(8.0f), params);
        const SunLight eveningLight = sunLightFor(sky.synopticAt(18.0f), params);
        check(morningLight.direction.x > 0.0f && eveningLight.direction.x < 0.0f,
              "Lumiere : la direction traverse le ciel d'est en ouest dans la journee");

        // Determinisme : deux captures au meme instant de jeu doivent etre
        // comparables, ce qui est la condition de la verification visuelle du
        // projet depuis la Phase 12.
        const SunLight again = sunLightFor(sky.synopticAt(15.0f), params);
        check(again.direction == afternoonLight.direction &&
                  again.intensity == afternoonLight.intensity,
              "Lumiere : deterministe, donc deux captures au meme instant sont comparables");

        // Toutes les valeurs restent dans leurs bornes, a toute heure.
        for (int i = 0; i < 24 * 4; ++i) {
            const SunLight light = sunLightFor(sky.synopticAt(static_cast<float>(i) * 0.25f), params);
            check(light.intensity >= 0.0f && light.intensity <= 1.0f,
                  "Lumiere : intensite toujours dans [0, 1]");
            check(light.ambient >= 0.0f && light.ambient <= 1.0f,
                  "Lumiere : multiplicateur d'ambiante toujours dans [0, 1]");
            checkNear(glm::length(light.direction), 1.0f, 1e-3f,
                      "Lumiere : la direction est toujours unitaire");
            check(light.direction.y >= 0.0f,
                  "Lumiere : jamais eclaire par en dessous, a aucune heure");
        }
    }
}

// ===========================================================================
// PHASE 27 - Meteo visible
// ===========================================================================
// Meme regle que testWeather() : des PROPRIETES et des SIGNES. Et une regle en
// plus, propre a cette phase : chaque effet visuel doit etre NUL quand la
// grandeur meteo qui le pilote est nulle. C'est la preuve executable qu'aucun
// effet n'a ete invente - un effet qui existerait sans sa grandeur serait par
// definition une mecanique nouvelle.
// PHASE 27 - texture de bruit du micro-relief (assets/DetailNoise.hpp).
void testDetailNoise() {
    std::printf("\n[Phase 27] Texture de bruit de detail\n");
    const TextureData t = makeDetailNoiseTexture(256, 32);
    check(t.width == 256 && t.mips.size() == 9 && t.mips.back().width == 1,
          "Bruit de detail : 256x256 et chaine complete de 9 mipmaps");
    const auto px = [&](uint32_t x, uint32_t y, int ch) {
        return static_cast<int>(t.pixels[(static_cast<size_t>(y) * 256 + x) * 4 + ch]);
    };
    // Periodicite : la colonne 255 doit se raccorder a la colonne 0 aussi bien
    // que deux colonnes voisines quelconques (sinon une couture en repetition).
    double seam = 0.0, inner = 0.0;
    for (uint32_t y = 0; y < 256; ++y) {
        seam += std::abs(px(255, y, 0) - px(0, y, 0));
        inner += std::abs(px(128, y, 0) - px(127, y, 0));
    }
    check(seam < 2.0 * inner + 256.0, "Bruit de detail : raccord periodique sans couture");
    // Derivee codee coherente avec la variation reelle de la valeur :
    // correlation positive entre G et (R[x+1] - R[x-1]).
    double corr = 0.0;
    for (uint32_t y = 0; y < 256; y += 3) {
        for (uint32_t x = 1; x < 255; x += 3) {
            corr += (px(x, y, 1) - 127.5) * (px(x + 1, y, 0) - px(x - 1, y, 0));
        }
    }
    check(corr > 0.0, "Bruit de detail : la derivee codee suit la pente de la valeur");
    bool inRange = true;
    for (size_t i = 0; i < t.mips[0].size; i += 4) {
        if (t.pixels[i] == 0 && t.pixels[i + 1] == 0) { inRange = false; }
    }
    check(inRange, "Bruit de detail : aucune derivee saturee a zero");
    const TextureData again = makeDetailNoiseTexture(256, 32);
    check(again.pixels == t.pixels, "Bruit de detail : deterministe");
}

void testWeatherVisuals() {
    std::printf("\n[Phase 27] Meteo visible\n");

    const WeatherSystem weather;
    const WeatherParams params;

    const auto sampleWith = [](float cloud, float precip, bool snowing, float wetness,
                               glm::vec3 wind) {
        WeatherSample s;
        s.cloudCover = cloud;
        s.precipitationMmPerHour = precip;
        s.snowing = snowing;
        s.wetness = wetness;
        s.windVector = wind;
        return s;
    };
    const auto saturation = [](const glm::vec3& c) {
        return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b});
    };

    const SynopticState noon = weather.synopticAt(12.0f);
    const SynopticState midnight = weather.synopticAt(24.0f);

    // --- Ciel clair et sec : RIEN ne change par rapport a la Phase 23 --------
    {
        const WeatherVisuals clear =
            weatherVisualsFor(noon, sampleWith(0.0f, 0.0f, false, 0.0f, glm::vec3(0.0f)), params);
        const SunLight reference = sunLightFor(noon, params);
        check(clear.light.intensity == reference.intensity &&
                  clear.light.ambient == reference.ambient &&
                  clear.light.skyColor == reference.skyColor &&
                  clear.light.color == reference.color &&
                  clear.light.direction == reference.direction,
              "Meteo visible : ciel clair et sec = lumiere de la Phase 23, au bit pres");
        check(clear.fogDensityPerMeter == 0.0f,
              "Meteo visible : aucun brouillard sans precipitation");
        check(clear.precipitation == 0.0f,
              "Meteo visible : aucune precipitation dessinee sans precipitation calculee");
        check(clear.groundDarkening == 0.0f,
              "Meteo visible : un sol sec n'est pas assombri");
    }

    // --- Nebulosite : monotone, et de jour seulement pour l'ambiante ---------
    {
        float lastDirect = 2.0f, lastSat = 2.0f, lastAmbient = -1.0f;
        bool monotone = true;
        for (int i = 0; i <= 10; ++i) {
            const float cloud = static_cast<float>(i) / 10.0f;
            const WeatherVisuals v = weatherVisualsFor(
                noon, sampleWith(cloud, 0.0f, false, 0.0f, glm::vec3(0.0f)), params);
            if (v.light.intensity > lastDirect || saturation(v.light.skyColor) > lastSat ||
                v.light.ambient < lastAmbient) {
                monotone = false;
            }
            lastDirect = v.light.intensity;
            lastSat = saturation(v.light.skyColor);
            lastAmbient = v.light.ambient;
        }
        check(monotone,
              "Meteo visible : plus de nuages = moins de soleil direct, ciel plus gris, "
              "ambiante plus diffuse - sans inversion sur toute la plage");

        const WeatherVisuals overcast = weatherVisualsFor(
            noon, sampleWith(1.0f, 0.0f, false, 0.0f, glm::vec3(0.0f)), params);
        checkNear(overcast.light.intensity,
                  sunLightFor(noon, params).intensity * (1.0f - params.cloudAspectDamping), 1e-5f,
                  "Meteo visible : le voile du soleil reprend cloudAspectDamping (un seul "
                  "chiffre pour 'les nuages cachent le soleil')");
        check(saturation(overcast.light.skyColor) < 1e-4f,
              "Meteo visible : un ciel entierement couvert est gris");
        // AJOUTE APRES LA PREMIERE CAPTURE : le gain d'ambiante etait ecrete a
        // 1, donc MORT en plein jour (ou l'ambiante vaut deja 1), et la boucle
        // monotone ci-dessus ne pouvait pas le voir - "ne decroit pas" est vrai
        // d'une constante. Il faut exiger que l'effet EXISTE.
        check(overcast.light.ambient > sunLightFor(noon, params).ambient * 1.5f,
              "Meteo visible : sous un ciel couvert de jour, l'ambiante diffuse "
              "augmente REELLEMENT (pas d'ecretage qui l'annule)");

        const WeatherVisuals nightClear = weatherVisualsFor(
            midnight, sampleWith(0.0f, 0.0f, false, 0.0f, glm::vec3(0.0f)), params);
        const WeatherVisuals nightOvercast = weatherVisualsFor(
            midnight, sampleWith(1.0f, 0.0f, false, 0.0f, glm::vec3(0.0f)), params);
        checkNear(nightOvercast.light.ambient, nightClear.light.ambient, 1e-6f,
                  "Meteo visible : une nuit couverte n'est pas plus claire qu'une nuit claire");
        check(glm::dot(nightOvercast.light.skyColor, glm::vec3(1.0f)) <=
                  glm::dot(nightClear.light.skyColor, glm::vec3(1.0f)) + 1e-5f,
              "Meteo visible : un ciel couvert de nuit reste un ciel de nuit");
    }

    // --- Precipitation, brouillard, neige, vent --------------------------------
    {
        const WeatherVisuals rain = weatherVisualsFor(
            noon, sampleWith(0.95f, 5.0f, false, 0.0f, glm::vec3(0.0f)), params);
        check(rain.precipitation > 0.5f && !rain.snow,
              "Meteo visible : 5 mm/h de pluie se dessinent, en pluie");
        const float visibilityKm = 3.912f / (rain.fogDensityPerMeter * 1000.0f);
        check(visibilityKm > 1.0f && visibilityKm < 20.0f,
              "Meteo visible : visibilite sous 5 mm/h entre 1 et 20 km (ordre de grandeur)");
        std::printf("    [brouillard] visibilite de Koschmieder sous 5 mm/h : %.1f km\n",
                    visibilityKm);

        const WeatherVisuals lightRain = weatherVisualsFor(
            noon, sampleWith(0.95f, 1.0f, false, 0.0f, glm::vec3(0.0f)), params);
        check(lightRain.fogDensityPerMeter < rain.fogDensityPerMeter &&
                  lightRain.precipitation < rain.precipitation,
              "Meteo visible : une pluie faible voile moins et se dessine moins qu'une forte");

        const WeatherVisuals snow = weatherVisualsFor(
            noon, sampleWith(0.95f, 5.0f, true, 0.0f, glm::vec3(0.0f)), params);
        check(snow.snow && snow.fogDensityPerMeter > rain.fogDensityPerMeter,
              "Meteo visible : a lame d'eau egale, la neige voile plus que la pluie");
        check(std::abs(snow.fallVelocity.y) < std::abs(rain.fallVelocity.y),
              "Meteo visible : la neige tombe plus lentement que la pluie");

        const glm::vec3 eastward(8.0f, 0.0f, 0.0f);
        const WeatherVisuals windy = weatherVisualsFor(
            noon, sampleWith(0.95f, 5.0f, false, 0.0f, eastward), params);
        check(windy.fallVelocity.x > 7.9f && std::abs(windy.fallVelocity.z) < 1e-5f,
              "Meteo visible : la pluie est poussee par le vent LOCAL, dans son sens");

        const WeatherVisuals wet = weatherVisualsFor(
            noon, sampleWith(0.0f, 0.0f, false, 1.0f, glm::vec3(0.0f)), params);
        checkNear(wet.groundDarkening, params.wetGroundDarkening, 1e-6f,
                  "Meteo visible : un sol ruisselant est assombri de wetGroundDarkening");
        check(wet.precipitation == 0.0f && wet.fogDensityPerMeter == 0.0f,
              "Meteo visible : un sol encore mouille apres l'averse ne fait pas pleuvoir");

        const WeatherVisuals garbage = weatherVisualsFor(
            noon, sampleWith(std::nanf(""), std::nanf(""), false, std::nanf(""),
                             glm::vec3(0.0f)), params);
        check(std::isfinite(garbage.fogDensityPerMeter) &&
                  std::isfinite(garbage.light.ambient) &&
                  std::isfinite(garbage.light.skyColor.r),
              "Meteo visible : une entree non finie ne produit pas de NaN a l'ecran");
    }

    // --- COHERENCE AVEC LE CALENDRIER REEL, et continuite ----------------------
    // Trente jours de jeu, une minute de jeu par pas : l'effet visuel doit
    // exister EXACTEMENT quand la meteo le dit, et ne jamais sauter.
    {
        int rainyMinutes = 0, mismatches = 0, phaseFlips = 0, unexplainedFogJumps = 0;
        float maxFogJump = 0.0f, maxSkyJump = 0.0f, maxPrecipJump = 0.0f;
        WeatherVisuals previous{};
        bool havePrevious = false;
        const glm::vec3 alpage(0.0f, 1500.0f, 0.0f);
        for (int minute = 0; minute < 30 * 24 * 60; ++minute) {
            const float hours = static_cast<float>(minute) / 60.0f;
            const SynopticState sky = weather.synopticAt(hours);
            const WeatherSample local = weather.sampleAt(alpage, sky, nullptr);
            const WeatherVisuals v = weatherVisualsFor(sky, local, params);

            const bool raining = local.precipitationMmPerHour > 0.0f;
            if (raining) { ++rainyMinutes; }
            if (raining != (v.precipitation > 0.0f) ||
                raining != (v.fogDensityPerMeter > 0.0f)) {
                ++mismatches;
            }
            if (havePrevious) {
                // La BASCULE PLUIE <-> NEIGE est une discontinuite du MODELE, pas
                // du rendu : WeatherSample::snowing est un booleen (seuil
                // snowThresholdC), et la neige voile x8 plus que la pluie. Le
                // premier jet de ce test l'a trouvee : j1 6 h 55 a 1500 m, la
                // visibilite passe de 1,6 a 13 km en une minute de jeu. La lisser
                // demanderait une phase MIXTE (pluie et neige melees) que Weather
                // ne modelise pas - prerequis manquant, documente, pas invente
                // ici. Le test l'ISOLE : ces minutes-la sont comptees a part, et
                // TOUT AUTRE saut reste un echec.
                const bool flip = v.snow != previous.snow;
                if (flip) { ++phaseFlips; }
                const float fogJump =
                    std::abs(v.fogDensityPerMeter - previous.fogDensityPerMeter);
                if (!flip && fogJump > 2e-4f) { ++unexplainedFogJumps; }
                if (!flip) { maxFogJump = std::max(maxFogJump, fogJump); }
                maxPrecipJump = std::max(maxPrecipJump,
                    std::abs(v.precipitation - previous.precipitation));
                maxSkyJump = std::max(maxSkyJump,
                    glm::length(v.light.skyColor - previous.light.skyColor));
            }
            previous = v;
            havePrevious = true;
        }
        check(rainyMinutes > 0,
              "Meteo visible : le calendrier de 30 jours contient de la pluie a montrer");
        check(mismatches == 0,
              "Meteo visible : pluie et brouillard a l'ecran EXACTEMENT quand la meteo "
              "calcule une precipitation, jamais sinon");
        // Bornes de continuite : une minute de jeu ne doit pas faire sauter
        // l'image. 0,05 de couleur de ciel ~ 13 niveaux sur 255 : c'est le
        // crepuscule, deja present depuis la Phase 23, qui fixe le plafond.
        //
        // Brouillard : 2e-4 /m par minute. La montee d'une averse de NEIGE
        // (x8) progresse d'environ 6e-5 /m par minute, lissement ; un saut
        // d'un ordre de grandeur au-dessus serait une vraie discontinuite.
        check(maxSkyJump < 0.05f && maxPrecipJump < 0.05f && unexplainedFogJumps == 0,
              "Meteo visible : continue dans le temps, hors bascule pluie/neige du modele");
        std::printf("    [calendrier] %d minutes de pluie sur 30 jours ; sauts max par "
                    "minute : ciel %.4f, pluie %.4f, brouillard %.2e /m ; %d bascule(s) "
                    "pluie/neige (discontinuite du modele, voir NOTES-PHASE27.md)\n",
                    rainyMinutes, maxSkyJump, maxPrecipJump, maxFogJump, phaseFlips);

        // Controle negatif sur les deux instants des captures de la phase :
        // meme heure, meme lieu, seul le jour change.
        const SynopticState rainyDay = weather.synopticAt(8 * 24.0f + 13.0f);
        const SynopticState clearDay = weather.synopticAt(12 * 24.0f + 13.0f);
        const WeatherVisuals rainy =
            weatherVisualsFor(rainyDay, weather.sampleAt(alpage, rainyDay, nullptr), params);
        const WeatherVisuals clear =
            weatherVisualsFor(clearDay, weather.sampleAt(alpage, clearDay, nullptr), params);
        check(rainy.precipitation > 0.0f && clear.precipitation == 0.0f,
              "Meteo visible : il pleut a l'ecran le jour 8 a 13 h, pas le jour 12");
        check(saturation(rainy.light.skyColor) < saturation(clear.light.skyColor),
              "Meteo visible : le ciel du jour 8 est plus gris que celui du jour 12");
    }
}

// ===========================================================================
// PHASE 28 : faconnage generique (world/Crafting), foyers, reposer au sol
// ===========================================================================
bool sameGround(const Ground& a, const Ground& b) {
    if (a.itemCount() != b.itemCount()) return false;
    for (std::size_t i = 0; i < a.itemCount(); ++i) {
        const GroundItem& x = a.items()[i];
        const GroundItem& y = b.items()[i];
        if (x.kind != y.kind || x.position != y.position || x.yaw != y.yaw || x.roll != y.roll) {
            return false;
        }
    }
    return true;
}

bool sameInventory(const Inventory& a, const Inventory& b) {
    for (uint32_t k = 0; k < kGroundItemKindCount; ++k) {
        if (a.count(static_cast<GroundItemKind>(k)) != b.count(static_cast<GroundItemKind>(k))) {
            return false;
        }
    }
    return true;
}

int countKind(const Ground& ground, GroundItemKind kind) {
    int n = 0;
    for (const GroundItem& item : ground.items()) {
        if (item.kind == kind) ++n;
    }
    return n;
}

const GroundItem* firstOfKind(const Ground& ground, GroundItemKind kind) {
    for (const GroundItem& item : ground.items()) {
        if (item.kind == kind) return &item;
    }
    return nullptr;
}

void testCraftingRecipeTable() {
    section("Phase 28 : table des recettes, nouvelles natures d'objets");

    // Unicite des id, sortie absente des ingredients, bornes.
    bool idsUnique = true;
    bool outNotInput = true;
    bool countsSane = true;
    for (std::size_t i = 0; i < kRecipes.size(); ++i) {
        for (std::size_t j = i + 1; j < kRecipes.size(); ++j) {
            if (std::strcmp(kRecipes[i].id, kRecipes[j].id) == 0) idsUnique = false;
        }
        const Recipe& r = kRecipes[i];
        if (r.inCount == 0 || r.inCount > r.in.size()) countsSane = false;
        for (uint8_t k = 0; k < r.inCount && k < r.in.size(); ++k) {
            if (r.in[k].count == 0) countsSane = false;
            if (r.hasOutput && r.in[k].kind == r.out) outNotInput = false;
        }
        check(findRecipe(r.id) == static_cast<int>(i),
              std::string("findRecipe retrouve ") + r.id);
    }
    check(idsUnique, "Recettes : id uniques");
    check(outNotInput, "Recettes : aucune sortie ne figure parmi ses ingredients");
    check(countsSane, "Recettes : nombres d'ingredients coherents");
    check(findRecipe("inexistante") == -1, "findRecipe : id inconnu -> -1");
    // PHASE 28 (Agent 3) : 8 -> 10 (abri, hache renforcee).
    check(kRecipes.size() == 10, "Dix recettes dans la table");
    check(findRecipe("shelter") == 0, "Priorite : l'abri en tete (la plus complexe)");
    check(findRecipe("shelter") < findRecipe("hearth"), "Priorite : abri avant foyer");
    check(findRecipe("pitched_axe") < findRecipe("axe"), "Priorite : hache renforcee avant hache");

    // Ordre de priorite : le foyer avant la hache, la hache avant la ligature,
    // la percussion en DERNIER (la plus simple).
    check(findRecipe("hearth") < findRecipe("axe"), "Priorite : foyer avant hache");
    check(findRecipe("axe") < findRecipe("cord"), "Priorite : hache avant ligature");
    check(findRecipe("pitch") < findRecipe("cord"), "Priorite : poix avant ligature");
    check(findRecipe("knap") == static_cast<int>(kRecipes.size()) - 1,
          "Priorite : la percussion est la derniere recette");

    // Recettes demandees par la conception, a l'unite pres.
    {
        const Recipe& h = kRecipes[static_cast<std::size_t>(findRecipe("hearth"))];
        check(h.out == GroundItemKind::Hearth && h.effect == CraftEffect::LightNewHearth,
              "Foyer : produit un foyer allume");
        check(h.in[0].kind == GroundItemKind::Stone && h.in[0].count == 3 &&
                  h.in[0].source == CraftSource::Placed && h.in[0].consumed,
              "Foyer : 3 pierres posees consommees");
        check(h.in[1].kind == GroundItemKind::Branch && h.in[1].count == 2 &&
                  h.in[1].source == CraftSource::Placed && h.in[1].consumed,
              "Foyer : 2 branches posees consommees");
        check(h.in[2].kind == GroundItemKind::Flake && h.in[2].source == CraftSource::Carried &&
                  !h.in[2].consumed,
              "Foyer : eclat porte, outil non consomme");
        const Recipe& p = kRecipes[static_cast<std::size_t>(findRecipe("pitch"))];
        check(p.station == CraftStation::LitHearth, "Poix : exige un foyer allume");
    }

    // Toutes les natures ont un rayon, des libelles et (pour les nouvelles) un mesh.
    check(kGroundItemKindCount == 15, "15 natures d'objets au sol (Phase 28, Agent 3)");
    bool radiiOk = true;
    bool labelsOk = true;
    for (uint32_t k = 0; k < kGroundItemKindCount; ++k) {
        const auto kind = static_cast<GroundItemKind>(k);
        if (!(groundItemRestRadius(kind) > 0.0f)) radiiOk = false;
        if (std::strcmp(groundItemLabel(kind), "objet") == 0 ||
            std::strcmp(groundItemLabelIndefinite(kind), "un objet") == 0 ||
            std::strcmp(groundItemLabelPlural(kind), "objets") == 0) {
            labelsOk = false;
        }
    }
    check(radiiOk, "Toutes les natures ont un rayon de repos > 0");
    check(labelsOk, "Toutes les natures ont leurs trois libelles");
    check(!groundItemIsPortable(GroundItemKind::Hearth), "Le foyer n'est pas portable");
    check(groundItemIsPortable(GroundItemKind::Axe) && groundItemIsPortable(GroundItemKind::Ash) &&
              groundItemIsPortable(GroundItemKind::Stone),
          "Hache, cendre, pierre : portables");
    check(!groundItemIsPortable(GroundItemKind::Shelter), "L'abri n'est pas portable");
    check(groundItemIsPortable(GroundItemKind::PitchedAxe), "La hache renforcee est portable");

    const std::array<std::pair<MeshData, float>, 6> meshes = { {
        { pitchedAxeMeshData(), kAxeRadius },
        { cordMeshData(), kCordRadius }, { axeMeshData(), kAxeRadius },
        { hearthMeshData(), kHearthRadius }, { ashMeshData(), kAshRadius },
        { pitchMeshData(), kPitchRadius },
    } };
    for (const auto& [mesh, radius] : meshes) {
        bool indicesValid = !mesh.empty() && mesh.indices.size() % 3 == 0;
        bool normalsUnit = true;
        bool restsOnGround = true;
        bool handSized = true;
        float minY = 1e9f;
        for (const uint32_t index : mesh.indices) {
            if (index >= mesh.vertices.size()) indicesValid = false;
        }
        for (const Vertex& v : mesh.vertices) {
            if (std::abs(glm::length(v.normal) - 1.0f) > 1e-3f) normalsUnit = false;
            if (v.pos.y > radius + 1e-4f || v.pos.y < -radius - 1e-4f) restsOnGround = false;
            if (std::abs(v.pos.x) > 0.5f || std::abs(v.pos.z) > 0.5f) handSized = false;
            minY = std::min(minY, v.pos.y);
        }
        check(indicesValid, "Mesh valide : " + mesh.name);
        check(normalsUnit, "Normales unitaires : " + mesh.name);
        check(restsOnGround, "Tient dans sa demi-hauteur : " + mesh.name);
        checkNear(minY, -radius, 0.01f, "Touche le sol (bas = -rayon) : " + mesh.name);
        check(handSized, "Dimensions plausibles : " + mesh.name);
    }

    // Resume d'inventaire : matieres toujours listees, objets faconnes seulement
    // s'ils sont portes.
    Inventory inventory;
    check(inventory.summary().find("buches 0") != std::string::npos &&
              inventory.summary().find("ligatures") == std::string::npos,
          "Resume : matieres a zero listees, objets faconnes a zero masques");
    inventory.add(GroundItemKind::Cord);
    check(inventory.summary().find("ligatures 1") != std::string::npos,
          "Resume : une ligature portee apparait");
}

void testCraftingKnapUnchanged() {
    section("Phase 28 : la percussion par la table = knapStones au bit pres");

    FlatTerrain flat;
    const glm::vec3 player{ 3.0f, 1.7f, -2.0f };
    const glm::vec3 facing{ 0.3f, -0.2f, -1.0f };

    Ground groundA(flat);
    Ground groundB(flat);
    Inventory invA;
    Inventory invB;
    invA.add(GroundItemKind::Stone, 2);
    invB.add(GroundItemKind::Stone, 2);
    HearthSet hearths;

    check(knapStones(invA, groundA, player, facing) == KnapResult::Success,
          "knapStones reussit (reference)");
    const CraftOutcome outcome = craftAt(invB, groundB, hearths, player, facing, 0.0f);
    check(outcome.result == CraftResult::Success && outcome.recipe == findRecipe("knap"),
          "craftAt choisit la percussion avec deux pierres portees");
    check(outcome.message == knapResultMessage(KnapResult::Success),
          "Message de succes identique a la Phase 6");
    check(sameInventory(invA, invB) && invB.count(GroundItemKind::Stone) == 1,
          "Une seule pierre consommee (le percuteur reste)");
    check(sameGround(groundA, groundB), "Eclat depose au meme endroit, meme yaw");

    // Refus : message historique, rien ne change.
    Inventory one;
    one.add(GroundItemKind::Stone);
    Ground empty(flat);
    const CraftOutcome refused = craftAt(one, empty, hearths, player, facing, 0.0f);
    check(refused.result == CraftResult::MissingIngredients, "Une pierre : refus");
    check(refused.message == knapResultMessage(KnapResult::NotEnoughStones),
          "Refus de percussion : message historique mot pour mot");
    check(one.count(GroundItemKind::Stone) == 1 && empty.itemCount() == 0,
          "Refus : rien consomme, rien depose");

    // Regard vertical : repli sans NaN, eclat a +X.
    Inventory two;
    two.add(GroundItemKind::Stone, 2);
    Ground g(flat);
    craftAt(two, g, hearths, player, { 0.0f, -1.0f, 0.0f }, 0.0f);
    check(g.itemCount() == 1 && !std::isnan(g.items()[0].position.x) &&
              !std::isnan(g.items()[0].position.z),
          "Regard vertical : aucun NaN");
    checkNear(g.items()[0].position.x, player.x + kCraftDropDistance, 1e-5f,
              "Regard vertical : repli sur +X a 1,1 m");
}

void testCraftingCord() {
    section("Phase 28 : ligature (3 fibres portees)");

    FlatTerrain flat;
    Ground ground(flat);
    HearthSet hearths;
    const glm::vec3 player{ 0.0f, 1.7f, 0.0f };
    const glm::vec3 facing{ 0.0f, 0.0f, -1.0f };

    Inventory inventory;
    inventory.add(GroundItemKind::Fiber, 2);
    // Une troisieme fibre AU SOL ne compte pas : la ligature se tord en main.
    ground.drop(GroundItemKind::Fiber, 0.5f, 0.0f, 0.0f, 0.0f);
    const Ground groundBefore = ground;
    const Inventory invBefore = inventory;
    const CraftOutcome refused = craftAt(inventory, ground, hearths, player, facing, 0.0f);
    check(refused.result == CraftResult::MissingIngredients &&
              refused.recipe == findRecipe("cord"),
          "Deux fibres portees (+1 posee) : refus, recette la plus proche = ligature");
    check(refused.message.find("tige a fibres") != std::string::npos &&
              refused.message.find("porter") != std::string::npos,
          "Le refus dit qu'il manque une tige a fibres a porter");
    check(sameGround(ground, groundBefore) && sameInventory(inventory, invBefore),
          "Refus : inventaire et sol identiques");

    inventory.add(GroundItemKind::Fiber, 2); // 4 portees
    const CraftOutcome ok = craftAt(inventory, ground, hearths, player, facing, 0.0f);
    check(ok.result == CraftResult::Success && ok.recipe == findRecipe("cord"),
          "Trois fibres portees : ligature");
    check(inventory.count(GroundItemKind::Fiber) == 1, "Exactement trois fibres consommees");
    check(inventory.count(GroundItemKind::Cord) == 0, "La ligature n'est pas en poche");
    check(countKind(ground, GroundItemKind::Fiber) == 1, "La fibre posee n'est pas touchee");
    const GroundItem* cord = firstOfKind(ground, GroundItemKind::Cord);
    check(cord != nullptr, "La ligature est au sol");
    if (cord != nullptr) {
        checkNear(cord->position.z, -kCraftDropDistance, 1e-5f, "Ligature a 1,1 m devant");
        checkNear(cord->position.y, kCordRadius, 1e-5f, "Ligature posee sur la surface");
    }
}

void testCraftingHearth() {
    section("Phase 28 : foyer (3 pierres + 2 branches posees, eclat porte)");

    FlatTerrain flat;
    Ground ground(flat);
    HearthSet hearths;
    const glm::vec3 player{ 0.0f, 1.7f, 0.0f };
    const glm::vec3 facing{ 1.0f, 0.0f, 0.0f };

    ground.drop(GroundItemKind::Stone, 1.0f, 0.0f, 0.0f, 0.0f);
    ground.drop(GroundItemKind::Stone, 1.0f, 1.0f, 0.0f, 0.0f);
    ground.drop(GroundItemKind::Stone, 2.0f, 0.0f, 0.0f, 0.0f);
    ground.drop(GroundItemKind::Branch, 2.0f, 1.0f, 0.0f, 0.0f);
    ground.drop(GroundItemKind::Branch, 1.5f, 0.5f, 0.0f, 0.0f);
    ground.drop(GroundItemKind::Log, 30.0f, 0.0f, 0.0f, 0.0f); // hors portee

    // Sans eclat : refus, rien ne bouge.
    Inventory inventory;
    const Ground before = ground;
    const CraftOutcome refused = craftAt(inventory, ground, hearths, player, facing, 10.0f);
    check(refused.result == CraftResult::MissingIngredients &&
              refused.recipe == findRecipe("hearth"),
          "Sans eclat : refus, recette la plus proche = foyer");
    check(refused.message.find("eclat") != std::string::npos, "Le refus nomme l'eclat");
    check(sameGround(ground, before) && inventory.total() == 0 && hearths.hearths().empty(),
          "Refus : sol, inventaire et foyers inchanges");

    inventory.add(GroundItemKind::Flake);
    const CraftOutcome ok = craftAt(inventory, ground, hearths, player, facing, 10.0f);
    check(ok.result == CraftResult::Success && ok.recipe == findRecipe("hearth"),
          "Avec l'eclat : foyer construit");
    check(inventory.count(GroundItemKind::Flake) == 1, "L'eclat (outil) n'est pas consomme");
    check(countKind(ground, GroundItemKind::Stone) == 0 &&
              countKind(ground, GroundItemKind::Branch) == 0,
          "Les 3 pierres et 2 branches sont consommees");
    check(countKind(ground, GroundItemKind::Log) == 1, "La buche hors portee est intacte");
    const GroundItem* hearthItem = firstOfKind(ground, GroundItemKind::Hearth);
    check(hearthItem != nullptr, "Le foyer existe au sol");
    if (hearthItem == nullptr) return;
    checkNear(hearthItem->position.x, 1.5f, 1e-4f, "Foyer au barycentre (x)");
    checkNear(hearthItem->position.z, 0.5f, 1e-4f, "Foyer au barycentre (z)");
    checkNear(hearthItem->position.y, kHearthRadius, 1e-5f, "Foyer pose sur la surface");
    check(hearths.hearths().size() == 1 && hearths.hearths()[0].lit, "Le foyer nait allume");
    checkNear(hearths.remainingHours(0, 10.0f), 1.0f, 1e-5f, "Combustion : 1 h de jeu");

    // Le foyer ne se ramasse pas.
    check(ground.findNearest(player, 5.0f) == kNoGroundItem,
          "findNearest ignore le foyer (objet fixe)");

    // Combustion : allume a mi-parcours, eteint a l'heure dite, UNE cendre.
    check(hearths.update(10.5f, ground) == 0 && hearths.hearths()[0].lit,
          "A +0,5 h le feu brule encore");
    check(countKind(ground, GroundItemKind::Ash) == 0, "Pas de cendre tant qu'il brule");
    check(hearths.update(11.0f, ground) == 1 && !hearths.hearths()[0].lit,
          "A +1 h le feu s'eteint");
    check(countKind(ground, GroundItemKind::Ash) == 1, "Une cendre est deposee");
    check(hearths.update(20.0f, ground) == 0 && countKind(ground, GroundItemKind::Ash) == 1,
          "Un foyer eteint ne produit pas d'autre cendre");
    check(countKind(ground, GroundItemKind::Hearth) == 1, "Le foyer eteint reste en place");
    const GroundItem* ash = firstOfKind(ground, GroundItemKind::Ash);
    if (ash != nullptr) {
        checkNear(ash->position.x, 1.5f, 1e-4f, "Cendre au centre du foyer");
        const int nearest = ground.findNearest(player, 5.0f);
        check(nearest >= 0 &&
                  ground.items()[static_cast<std::size_t>(nearest)].kind == GroundItemKind::Ash,
              "La cendre, elle, se ramasse");
    }

    // Rallumer : 2 branches posees + eclat porte, foyer froid a portee.
    ground.drop(GroundItemKind::Branch, 0.8f, -0.3f, 0.0f, 0.0f);
    const CraftOutcome oneBranch = craftAt(inventory, ground, hearths, player, facing, 20.0f);
    check(oneBranch.result == CraftResult::MissingIngredients &&
              !hearths.hearths()[0].lit,
          "Une seule branche : le foyer froid ne repart pas");
    ground.drop(GroundItemKind::Branch, 0.8f, 0.3f, 0.0f, 0.0f);
    const CraftOutcome relit = craftAt(inventory, ground, hearths, player, facing, 20.0f);
    check(relit.result == CraftResult::Success && relit.recipe == findRecipe("hearth_relight"),
          "Deux branches + eclat : le foyer est rallume");
    check(hearths.hearths()[0].lit && countKind(ground, GroundItemKind::Branch) == 0 &&
              inventory.count(GroundItemKind::Flake) == 1,
          "Rallumage : branches consommees, eclat garde");
    check(countKind(ground, GroundItemKind::Hearth) == 1, "Rallumage : aucun second foyer");

    // Alimenter : une buche posee pres du foyer allume prolonge d'1 h.
    ground.drop(GroundItemKind::Log, 0.5f, 0.0f, 0.0f, 0.0f);
    const CraftOutcome fed = craftAt(inventory, ground, hearths, player, facing, 20.5f);
    check(fed.result == CraftResult::Success && fed.recipe == findRecipe("hearth_feed_log"),
          "Buche posee : le foyer est alimente");
    checkNear(hearths.remainingHours(0, 20.5f), 1.5f, 1e-4f,
              "Alimenter prolonge d'1 h (0,5 h restante -> 1,5 h)");
    check(countKind(ground, GroundItemKind::Log) == 1, "Seule la buche proche est consommee");

    // POINT D'ACCROCHE (non branche) : chaleur au centre, nulle a 6 m, nulle eteint.
    const glm::vec3 center = hearths.hearths()[0].position;
    checkNear(hearths.warmthAt(center), 8.0f, 1e-4f, "Chaleur maximale au centre");
    checkNear(hearths.warmthAt(center + glm::vec3(3.0f, 0.0f, 0.0f)), 4.0f, 1e-4f,
              "Chaleur a mi-rayon : la moitie");
    checkNear(hearths.warmthAt(center + glm::vec3(6.0f, 0.0f, 0.0f)), 0.0f, 1e-6f,
              "Chaleur nulle a 6 m");
    hearths.update(100.0f, ground);
    checkNear(hearths.warmthAt(center), 0.0f, 1e-6f, "Foyer eteint : aucune chaleur");
    check(countKind(ground, GroundItemKind::Ash) == 2, "Seconde extinction : seconde cendre");
}

void testCraftingAxe() {
    section("Phase 28 : hache (eclat + branche + ligature poses)");

    FlatTerrain flat;
    Ground ground(flat);
    HearthSet hearths;
    Inventory inventory;
    const glm::vec3 player{ 0.0f, 1.7f, 0.0f };
    const glm::vec3 facing{ 0.0f, 0.0f, 1.0f };

    // Ingredients NON CONTIGUS, entrelaces avec des objets etrangers : la
    // consommation ne doit retirer que les bons (pickUp echange avec le dernier).
    ground.drop(GroundItemKind::Clay, 0.2f, 0.2f, 0.0f, 0.0f);
    ground.drop(GroundItemKind::Flake, 1.0f, 0.0f, 0.0f, 0.0f);
    ground.drop(GroundItemKind::Sand, -0.4f, 0.2f, 0.0f, 0.0f);
    ground.drop(GroundItemKind::Branch, 0.0f, 1.0f, 0.0f, 0.0f);
    ground.drop(GroundItemKind::Resin, 0.3f, -0.6f, 0.0f, 0.0f);
    ground.drop(GroundItemKind::Stone, 40.0f, 0.0f, 0.0f, 0.0f);

    const Ground before = ground;
    const CraftOutcome refused = craftAt(inventory, ground, hearths, player, facing, 0.0f);
    check(refused.result == CraftResult::MissingIngredients &&
              refused.recipe == findRecipe("axe"),
          "Sans ligature : refus, recette la plus proche = hache");
    check(refused.message.find("ligature") != std::string::npos, "Le refus nomme la ligature");
    check(sameGround(ground, before), "Refus : sol identique octet pour octet");

    // Une ligature PORTEE ne suffit pas : l'outil composite s'assemble au sol.
    inventory.add(GroundItemKind::Cord);
    check(craftAt(inventory, ground, hearths, player, facing, 0.0f).result ==
              CraftResult::MissingIngredients,
          "Ligature portee (non posee) : refus");
    check(inventory.count(GroundItemKind::Cord) == 1 && sameGround(ground, before),
          "Refus : la ligature portee n'est pas consommee");

    ground.drop(GroundItemKind::Cord, -1.0f, 0.0f, 0.0f, 0.0f);
    const CraftOutcome ok = craftAt(inventory, ground, hearths, player, facing, 0.0f);
    check(ok.result == CraftResult::Success && ok.recipe == findRecipe("axe"),
          "Eclat + branche + ligature poses : hache");
    check(countKind(ground, GroundItemKind::Flake) == 0 &&
              countKind(ground, GroundItemKind::Branch) == 0 &&
              countKind(ground, GroundItemKind::Cord) == 0,
          "Les trois ingredients sont consommes");
    check(countKind(ground, GroundItemKind::Clay) == 1 &&
              countKind(ground, GroundItemKind::Sand) == 1 &&
              countKind(ground, GroundItemKind::Resin) == 1 &&
              countKind(ground, GroundItemKind::Stone) == 1,
          "Les objets etrangers entrelaces sont tous intacts");
    check(ground.itemCount() == 5, "Sol : 4 etrangers + la hache");
    check(inventory.count(GroundItemKind::Cord) == 1, "La ligature portee est gardee");
    const GroundItem* axe = firstOfKind(ground, GroundItemKind::Axe);
    check(axe != nullptr, "La hache est au sol");
    if (axe != nullptr) {
        checkNear(axe->position.x, 0.0f, 1e-5f, "Hache au barycentre (x)");
        checkNear(axe->position.z, 1.0f / 3.0f, 1e-5f, "Hache au barycentre (z)");
        const int nearest = ground.findNearest({ 0.0f, 1.7f, 0.33f }, 0.1f);
        check(nearest >= 0, "La hache se ramasse");
        GroundItemKind picked = GroundItemKind::Log;
        ground.pickUp(nearest, &picked);
        inventory.add(picked);
        check(inventory.count(GroundItemKind::Axe) == 1,
              "Hache portee (point d'accroche du rendement, Agent 3)");
    }

    // Un ingredient pose juste hors de portee (2,6 m) ne compte pas.
    Ground far(flat);
    far.drop(GroundItemKind::Flake, 2.6f, 0.0f, 0.0f, 0.0f);
    far.drop(GroundItemKind::Branch, 0.0f, 1.0f, 0.0f, 0.0f);
    far.drop(GroundItemKind::Cord, -1.0f, 0.0f, 0.0f, 0.0f);
    Inventory none;
    check(craftAt(none, far, hearths, player, facing, 0.0f).result ==
              CraftResult::MissingIngredients,
          "Eclat pose a 2,6 m : hors de portee, refus");
    check(far.itemCount() == 3, "Refus : rien consomme");
}

void testPitchNeedsLitHearth() {
    section("Phase 28 : poix (2 resines + 1 cendre posees, foyer allume)");

    FlatTerrain flat;
    const glm::vec3 player{ 0.0f, 1.7f, 0.0f };
    const glm::vec3 facing{ 1.0f, 0.0f, 0.0f };

    const auto setup = [](Ground& g) {
        g.drop(GroundItemKind::Resin, 0.5f, 0.5f, 0.0f, 0.0f);
        g.drop(GroundItemKind::Resin, 0.5f, -0.5f, 0.0f, 0.0f);
        g.drop(GroundItemKind::Ash, -0.5f, 0.0f, 0.0f, 0.0f);
    };

    // 1. Aucun foyer.
    {
        Ground ground(flat);
        HearthSet hearths;
        Inventory inventory;
        setup(ground);
        const Ground before = ground;
        const CraftOutcome o = craftAt(inventory, ground, hearths, player, facing, 0.0f);
        check(o.result == CraftResult::MissingIngredients && o.recipe == findRecipe("pitch"),
              "Sans foyer : refus, recette la plus proche = poix");
        check(o.message.find("chauffer") != std::string::npos,
              "Le refus dit qu'il faut chauffer");
        check(sameGround(ground, before), "Sans foyer : rien consomme");
    }
    // 2. Foyer ETEINT a portee.
    {
        Ground ground(flat);
        HearthSet hearths;
        Inventory inventory;
        ground.drop(GroundItemKind::Hearth, 1.0f, 0.0f, 0.0f, 0.0f);
        hearths.addLit(ground.items().back().position, 0.0f);
        hearths.update(5.0f, ground); // eteint -> une cendre au centre
        check(!hearths.hearths()[0].lit, "Foyer eteint");
        setup(ground);
        const Ground before = ground;
        const CraftOutcome o = craftAt(inventory, ground, hearths, player, facing, 5.0f);
        check(o.result == CraftResult::MissingIngredients,
              "Foyer eteint : la poix est refusee");
        check(sameGround(ground, before), "Foyer eteint : rien consomme");
    }
    // 3. Foyer allume trop loin (2,5 m > kStationReach).
    {
        Ground ground(flat);
        HearthSet hearths;
        Inventory inventory;
        ground.drop(GroundItemKind::Hearth, 2.5f, 0.0f, 0.0f, 0.0f);
        hearths.addLit(ground.items().back().position, 0.0f);
        setup(ground);
        check(craftAt(inventory, ground, hearths, player, facing, 0.1f).result ==
                  CraftResult::MissingIngredients,
              "Foyer allume a 2,5 m : hors de portee du poste, refus");
    }
    // 4. Foyer allume a portee : succes.
    {
        Ground ground(flat);
        HearthSet hearths;
        Inventory inventory;
        ground.drop(GroundItemKind::Hearth, 1.5f, 0.0f, 0.0f, 0.0f);
        hearths.addLit(ground.items().back().position, 0.0f);
        setup(ground);
        ground.drop(GroundItemKind::Resin, -1.0f, -1.0f, 0.0f, 0.0f); // une de trop
        const CraftOutcome o = craftAt(inventory, ground, hearths, player, facing, 0.2f);
        check(o.result == CraftResult::Success && o.recipe == findRecipe("pitch"),
              "Foyer allume a 1,5 m : poix");
        check(countKind(ground, GroundItemKind::Resin) == 1 &&
                  countKind(ground, GroundItemKind::Ash) == 0,
              "Exactement 2 resines et 1 cendre consommees");
        check(countKind(ground, GroundItemKind::Hearth) == 1 && hearths.hearths()[0].lit,
              "Le foyer (poste) n'est pas consomme");
        check(countKind(ground, GroundItemKind::Pitch) == 1, "La poix est au sol");
        checkNear(hearths.remainingHours(0, 0.2f), 0.8f, 1e-4f,
                  "Faire de la poix ne consomme pas de temps de combustion");
    }
}

void testPutDown() {
    section("Phase 28 : reposer un objet porte (touche P)");

    FlatTerrain flat;
    Ground ground(flat);
    Inventory inventory;
    const glm::vec3 player{ 0.0f, 1.7f, 0.0f };
    const glm::vec3 facing{ 0.0f, -0.5f, -1.0f };

    GroundItemKind kind = GroundItemKind::Log;
    check(!chooseKindToPutDown(inventory, GroundItemKind::Stone, &kind),
          "Inventaire vide : rien a poser");
    check(!putDown(inventory, ground, GroundItemKind::Stone, player, facing) &&
              ground.itemCount() == 0,
          "putDown sans objet porte : refus, rien depose");

    inventory.add(GroundItemKind::Branch, 1);
    inventory.add(GroundItemKind::Stone, 3);
    check(chooseKindToPutDown(inventory, GroundItemKind::Stone, &kind) &&
              kind == GroundItemKind::Stone,
          "Le dernier ramasse est pose en priorite");
    check(chooseKindToPutDown(inventory, GroundItemKind::Resin, &kind) &&
              kind == GroundItemKind::Branch,
          "Sinon la premiere nature portee (ordre de l'enum)");

    for (int i = 0; i < 3; ++i) {
        check(putDown(inventory, ground, GroundItemKind::Stone, player, facing),
              "Pierre reposee " + std::to_string(i + 1));
    }
    check(inventory.count(GroundItemKind::Stone) == 0 && ground.itemCount() == 3,
          "Trois pierres passees de l'inventaire au sol");
    checkNear(ground.items()[0].position.z, -kPutDownDistance, 1e-5f,
              "Premier objet pose a 0,9 m devant");
    checkNear(ground.items()[0].position.y, kStoneRadius, 1e-5f, "Pose sur la surface");
    bool distinct = true;
    bool inReach = true;
    for (std::size_t i = 0; i < ground.itemCount(); ++i) {
        const glm::vec3 p = ground.items()[i].position;
        if (std::sqrt(p.x * p.x + p.z * p.z) > kCraftReach) inReach = false;
        for (std::size_t j = i + 1; j < ground.itemCount(); ++j) {
            if (glm::length(p - ground.items()[j].position) < 0.2f) distinct = false;
        }
    }
    check(distinct, "Objets reposes decales (pas de superposition exacte)");
    check(inReach, "Objets reposes a portee de faconnage");
    check(!putDown(inventory, ground, GroundItemKind::Hearth, player, facing),
          "Un foyer ne se 'repose' pas");

    // Le cycle complet du foyer au clavier : ramasser -> reposer -> C.
    inventory.add(GroundItemKind::Branch, 1); // 2 branches
    inventory.add(GroundItemKind::Flake, 1);
    putDown(inventory, ground, GroundItemKind::Branch, player, facing);
    putDown(inventory, ground, GroundItemKind::Branch, player, facing);
    HearthSet hearths;
    const CraftOutcome o = craftAt(inventory, ground, hearths, player, facing, 0.0f);
    check(o.result == CraftResult::Success && o.recipe == findRecipe("hearth"),
          "Reposer 3 pierres et 2 branches puis C : un foyer");
}

// ===========================================================================
// PHASE 28 (Agent 3) : les EFFETS branches sur les systemes existants
// ===========================================================================
void testHearthWarmthEffect() {
    section("Phase 28 : chaleur du foyer -> Needs (ambiante corrigee)");

    FlatTerrain flat;
    Ground ground(flat);
    HearthSet hearths;
    const glm::vec3 center{ 0.0f, 0.0f, 0.0f };
    checkNear(hearths.warmthAt(center), 0.0f, 1e-6f, "Aucun foyer : +0 degre");
    ground.drop(GroundItemKind::Hearth, 0.0f, 0.0f, 0.0f, 0.0f);
    hearths.addLit(ground.items().back().position, 0.0f);

    checkNear(hearths.warmthAt({ 0.0f, 1.7f, 0.0f }), 8.0f, 1e-4f, "Au centre : +8 degres");
    checkNear(hearths.warmthAt({ 1.5f, 1.7f, 0.0f }), 6.0f, 1e-4f, "A 1,5 m : +6 degres");
    checkNear(hearths.warmthAt({ 0.0f, 1.7f, 3.0f }), 4.0f, 1e-4f, "A 3 m : +4 degres");
    checkNear(hearths.warmthAt({ 6.0f, 1.7f, 0.0f }), 0.0f, 1e-4f, "A 6 m : +0 degre");
    checkNear(hearths.warmthAt({ 20.0f, 1.7f, 0.0f }), 0.0f, 1e-6f, "A 20 m : +0 degre (jamais negatif)");

    // Meme calcul que main.cpp : ambiante meteo + warmthAt, puis Needs.
    const auto feltAfter = [&](float warmth, bool sheltered) {
        Needs needs;
        NeedsContext ctx;
        ctx.ambientTemperatureC = 2.0f + warmth;
        ctx.windSpeedMps = 5.0f;
        ctx.sheltered = sheltered;
        for (int i = 0; i < 600; ++i) needs.update(0.1f, ctx);
        return std::make_pair(needs.ambientTemperatureC(), needs.temperature());
    };
    const auto cold = feltAfter(0.0f, false);
    const auto warm = feltAfter(hearths.warmthAt({ 1.5f, 1.7f, 0.0f }), false);
    checkNear(warm.first, 8.0f, 1e-4f, "Needs recoit l'ambiante corrigee (2 + 6 degres)");
    check(warm.second > cold.second,
          "Pres d'un feu allume, la jauge de chaleur baisse moins (60 s a 2 degres, vent 5 m/s)");

    // Extinction : le feu ne chauffe plus, la cendre tombe.
    hearths.update(1.01f, ground);
    check(!hearths.hearths()[0].lit, "Foyer eteint apres 1 h de jeu");
    checkNear(hearths.warmthAt({ 0.0f, 1.7f, 0.0f }), 0.0f, 1e-6f, "Foyer eteint : +0 degre");
    const auto afterOut = feltAfter(hearths.warmthAt({ 1.5f, 1.7f, 0.0f }), false);
    checkNear(afterOut.second, cold.second, 1e-6f, "Foyer eteint : jauge identique au froid sans feu");
    hearths.relight(0, 2.0f);
    checkNear(hearths.warmthAt({ 0.0f, 1.7f, 0.0f }), 8.0f, 1e-4f, "Rallume : +8 degres de nouveau");

    // Deux feux s'additionnent (somme dans warmthAt).
    ground.drop(GroundItemKind::Hearth, 2.0f, 0.0f, 0.0f, 0.0f);
    hearths.addLit(ground.items().back().position, 2.0f);
    checkNear(hearths.warmthAt({ 1.0f, 1.7f, 0.0f }), 2.0f * 8.0f * (1.0f - 1.0f / 6.0f), 1e-4f,
              "Entre deux feux a 1 m chacun : chaleurs additionnees");
}

void testAxeYieldEffect() {
    section("Phase 28 : rendement de la hache -> Forest::scheduleBucking");

    Inventory inventory;
    check(axeBonusLogs(inventory) == 0, "Sans hache : bonus 0");
    inventory.add(GroundItemKind::Axe);
    check(axeBonusLogs(inventory) == kAxeBonusLogs && kAxeBonusLogs == 1, "Hache portee : +1 buche");
    inventory.add(GroundItemKind::PitchedAxe);
    check(axeBonusLogs(inventory) == kPitchedAxeBonusLogs && kPitchedAxeBonusLogs == 2,
          "Hache renforcee portee : +2 (le meilleur outil, pas de cumul)");
    inventory.remove(GroundItemKind::Axe, 1);
    check(axeBonusLogs(inventory) == 2, "Hache renforcee seule : +2");

    check(Forest::buckingLogCount(4.0f, 0) == 2, "Arbre de 4 m : 2 buches (inchange)");
    check(Forest::buckingLogCount(7.0f, 0) == 2, "Arbre de 7 m : 2 buches (inchange)");
    check(Forest::buckingLogCount(12.0f, 0) == 4, "Arbre de 12 m : 4 buches (plafond inchange)");
    check(Forest::buckingLogCount(7.0f, 1) == 3 && Forest::buckingLogCount(12.0f, 1) == 5,
          "Hache : 3..5 au lieu de 2..4 (conception P3)");
    check(Forest::buckingLogCount(12.0f, 2) == 6, "Hache renforcee : +2");

    // Abattage reel, trois fois le meme arbre (meme graine) : 0, 1, 2 de bonus.
    const auto fellAndCount = [](int bonus, std::size_t* branchesOut, bool* felledOut) {
        FlatTerrain flat;
        Ground ground(flat);
        ForestParams params;
        params.treeCount = 0;
        Forest forest(flat, ground, 50.0f, params);
        forest.plantTree(0.0f, 0.0f, 12.0f);
        forest.setBuckingBonusLogs(bonus);
        *felledOut = forest.fell(0);
        for (int i = 0; i < 60 * 30 && (forest.trees()[0].state != TreeState::Fallen ||
                                         forest.pendingDropCount() > 0); ++i) {
            forest.update(1.0f / 60.0f);
        }
        *branchesOut = static_cast<std::size_t>(countKind(ground, GroundItemKind::Branch));
        return countKind(ground, GroundItemKind::Log);
    };
    std::size_t b0 = 0, b1 = 0, b2 = 0;
    bool f0 = false, f1 = false, f2 = false;
    const int l0 = fellAndCount(0, &b0, &f0);
    const int l1 = fellAndCount(1, &b1, &f1);
    const int l2 = fellAndCount(2, &b2, &f2);
    check(f0, "Abattage SANS hache toujours possible (scenario A intact)");
    check(l0 == 4 && b0 == 4, "Sans hache : 4 buches + 4 branches (Phase 5, inchange)");
    check(l1 == 5 && b1 == 4, "Avec hache : 5 buches, branches inchangees");
    check(l2 == 6 && b2 == 4, "Avec hache renforcee : 6 buches");
    check(f1 && f2, "Avec hache : l'abattage lui-meme est inchange");

    // Negatif borne a 0.
    FlatTerrain flat;
    Ground ground(flat);
    ForestParams params;
    params.treeCount = 0;
    Forest forest(flat, ground, 50.0f, params);
    forest.setBuckingBonusLogs(-3);
    check(forest.buckingBonusLogs() == 0, "Bonus negatif ramene a 0");
}

void testShelterEffect() {
    section("Phase 28 : abri (4 buches + 6 branches + 2 ligatures) -> NeedsContext::sheltered");

    FlatTerrain flat;
    Ground ground(flat);
    HearthSet hearths;
    Inventory inventory;
    const glm::vec3 player{ 0.0f, 1.7f, 0.0f };
    const glm::vec3 facing{ 1.0f, 0.0f, 0.0f };

    check(!shelterNear(ground, player), "Aucun abri : non abrite");

    // Ingredients poses en cercle (dans les 2,5 m), sauf UNE ligature.
    const auto ring = [&](GroundItemKind kind, int n, float radius, float phase) {
        for (int i = 0; i < n; ++i) {
            const float a = phase + 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
            ground.drop(kind, radius * std::cos(a), radius * std::sin(a), 0.0f, 0.0f);
        }
    };
    ring(GroundItemKind::Log, 4, 1.5f, 0.0f);
    ring(GroundItemKind::Branch, 6, 1.0f, 0.3f);
    ring(GroundItemKind::Cord, 1, 0.6f, 0.0f);
    {
        const Ground before = ground;
        const CraftOutcome o = craftAt(inventory, ground, hearths, player, facing, 0.0f);
        check(o.result == CraftResult::MissingIngredients && o.recipe == findRecipe("shelter"),
              "Une ligature manque : refus, recette la plus proche = abri");
        check(o.message.find("ligature") != std::string::npos, "Le refus nomme la ligature");
        check(sameGround(ground, before), "Refus : sol intact");
    }
    ring(GroundItemKind::Cord, 1, 0.6f, 3.14159265f); // diametralement opposee : barycentre au centre
    const CraftOutcome o = craftAt(inventory, ground, hearths, player, facing, 0.0f);
    check(o.result == CraftResult::Success && o.recipe == findRecipe("shelter"), "Abri construit");
    check(countKind(ground, GroundItemKind::Log) == 0 && countKind(ground, GroundItemKind::Branch) == 0 &&
              countKind(ground, GroundItemKind::Cord) == 0,
          "4 buches, 6 branches, 2 ligatures consommees (et pas le feu : aucun foyer)");
    check(countKind(ground, GroundItemKind::Shelter) == 1, "Un abri au sol");
    const GroundItem* shelter = firstOfKind(ground, GroundItemKind::Shelter);
    check(shelter != nullptr && std::abs(shelter->position.x) < 1e-3f &&
              std::abs(shelter->position.z) < 1e-3f,
          "L'abri nait au barycentre des ingredients (ici le centre)");
    check(shelter != nullptr && std::abs(shelter->position.y - kShelterRadius) < 1e-4f,
          "L'abri repose sur le sol");
    check(ground.findNearest(player, 2.5f) == kNoGroundItem, "L'abri ne se ramasse pas");

    // L'effet : a portee horizontale et verticale.
    check(shelterNear(ground, player), "Sous l'abri : abrite");
    check(shelterNear(ground, { 2.4f, 1.7f, 0.0f }), "A 2,4 m : abrite");
    check(!shelterNear(ground, { 2.6f, 1.7f, 0.0f }), "A 2,6 m : non abrite");
    check(!shelterNear(ground, { 0.0f, 40.0f, 0.0f }), "Survol a 40 m : non abrite");

    // Needs : +5 degres ressentis et vent annule (Needs.cpp), rien d'autre.
    Needs outside;
    Needs inside;
    NeedsContext ctx;
    ctx.ambientTemperatureC = 0.0f;
    ctx.windSpeedMps = 8.0f;
    outside.update(0.1f, ctx);
    ctx.sheltered = shelterNear(ground, player);
    inside.update(0.1f, ctx);
    check(inside.feltTemperatureC() > outside.feltTemperatureC() + 5.0f,
          "Abrite : ressenti > dehors + 5 degres (abri + vent annule)");
    checkNear(inside.ambientTemperatureC(), outside.ambientTemperatureC(), 1e-6f,
              "L'abri ne change pas l'ambiante, seulement le ressenti");

    // Mesh de l'abri : valide, pose au sol, dans ses demi-dimensions.
    const MeshData mesh = shelterMeshData();
    bool valid = !mesh.empty() && mesh.indices.size() % 3 == 0;
    float minY = 1e9f, maxY = -1e9f;
    bool inBox = true;
    for (const uint32_t i : mesh.indices) if (i >= mesh.vertices.size()) valid = false;
    for (const Vertex& v : mesh.vertices) {
        minY = std::min(minY, v.pos.y);
        maxY = std::max(maxY, v.pos.y);
        if (std::abs(v.pos.x) > kShelterHalfWidth + 1e-3f || std::abs(v.pos.z) > kShelterHalfDepth + 1e-3f)
            inBox = false;
        if (std::abs(glm::length(v.normal) - 1.0f) > 1e-3f) valid = false;
    }
    check(valid, "Mesh de l'abri valide (indices, normales)");
    checkNear(minY, -kShelterRadius, 0.01f, "Abri : bas du mesh = -rayon (pose au sol)");
    check(maxY <= kShelterRadius + 1e-3f, "Abri : tient dans sa demi-hauteur");
    check(inBox, "Abri : tient dans son emprise");

    // Hache renforcee : hache + poix posees.
    Ground g2(flat);
    g2.drop(GroundItemKind::Axe, 0.5f, 0.0f, 0.0f, 0.0f);
    g2.drop(GroundItemKind::Pitch, -0.5f, 0.0f, 0.0f, 0.0f);
    const CraftOutcome pa = craftAt(inventory, g2, hearths, player, facing, 0.0f);
    check(pa.result == CraftResult::Success && pa.recipe == findRecipe("pitched_axe"),
          "Hache + poix posees : hache renforcee");
    check(countKind(g2, GroundItemKind::PitchedAxe) == 1 && countKind(g2, GroundItemKind::Axe) == 0 &&
              countKind(g2, GroundItemKind::Pitch) == 0,
          "Hache et poix consommees, hache renforcee au sol");
}

// ===========================================================================
// PHASE 29 (Agent 2) : rochers et pierriers
// ===========================================================================

// Versant a pente constante `slope` le long de +X, rendu RUGUEUX par une grille
// d'aleas de +/- `bump` m au pas de 1 m (interpolee bilineairement, comme un DTM).
class RoughSlope : public HeightSampler {
public:
    RoughSlope(float slope, float bump) : slope_(slope), bump_(bump) {}
    float heightAt(float x, float z) const override {
        const float fx = std::floor(x), fz = std::floor(z);
        const float tx = x - fx, tz = z - fz;
        const auto n = [&](float a, float b) {
            uint32_t h = (static_cast<uint32_t>(static_cast<int32_t>(a)) * 0x9E3779B1u) ^
                         (static_cast<uint32_t>(static_cast<int32_t>(b)) * 0x85EBCA77u);
            h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
            return (static_cast<float>(h >> 8) / 16777216.0f * 2.0f - 1.0f) * bump_;
        };
        const float v = glm::mix(glm::mix(n(fx, fz), n(fx + 1.0f, fz), tx),
                                 glm::mix(n(fx, fz + 1.0f), n(fx + 1.0f, fz + 1.0f), tx), tz);
        return 1500.0f + slope_ * x + v;
    }
private:
    float slope_, bump_;
};

// Pente qui CROIT avec x (h = k u^2 / 2 -> pente = k u) : le masque 0,60-0,84 y
// est une bande, et tout ce qui en sort doit rester vide.
class RampingSlope : public HeightSampler {
public:
    float heightAt(float x, float) const override {
        const float u = x + 2000.0f; // pente 0,60 a x = -500, 0,84 a x = +100
        return 1200.0f + 0.5f * 0.0004f * u * u;
    }
};

void testRockMeshes() {
    section("Phase 29 : meshes de rochers (bloc, rocher, pierraille)");
    const MeshData meshes[3] = { boulderMeshData(), rockMeshData(), gravelMeshData() };
    const char* names[3] = { "gros bloc", "rocher moyen", "pierraille" };
    for (int m = 0; m < 3; ++m) {
        const MeshData& mesh = meshes[m];
        bool ok = !mesh.empty() && mesh.indices.size() % 3 == 0;
        glm::vec3 lo(1e9f), hi(-1e9f);
        for (uint32_t index : mesh.indices) { ok = ok && index < mesh.vertices.size(); }
        for (const Vertex& v : mesh.vertices) {
            ok = ok && std::isfinite(v.pos.x) && std::isfinite(v.pos.y) && std::isfinite(v.pos.z);
            ok = ok && std::abs(glm::length(v.normal) - 1.0f) < 1e-3f;
            lo = glm::min(lo, v.pos);
            hi = glm::max(hi, v.pos);
        }
        std::printf("  (%s : %u triangles, y %.2f..%.2f, emprise %.2f x %.2f)\n", names[m],
                    mesh.triangleCount(), lo.y, hi.y, hi.x - lo.x, hi.z - lo.z);
        check(ok, std::string("Mesh ") + names[m] + " : indices, normales et positions valides");
        check(mesh.triangleCount() >= 16 && mesh.triangleCount() <= 1200,
              std::string("Mesh ") + names[m] + " : bas-poly (16 a 1200 triangles)");
        check(lo.y < 0.0f && hi.y > 0.0f,
              std::string("Mesh ") + names[m] + " : en partie ENTERRE sous l'origine (pas de flottement en pente)");
        if (m < 2) {
            check(hi.y <= kRockVisibleHeight + 1e-3f && hi.y > 0.45f,
                  std::string("Mesh ") + names[m] + " : hauteur visible sous 0,70 (x echelle)");
            check(std::max(hi.x - lo.x, hi.z - lo.z) <= 2.0f * kRockFootprintRadius + 0.05f,
                  std::string("Mesh ") + names[m] + " : empreinte dans le rayon de collision");
        } else {
            // PHASE 30 (Agent B) : bornes elargies (etaient hi.y < 0,15 et
            // emprise <= 1,2). Les cailloux passent de 5-12 cm a 10-25 cm de
            // demi-largeur et sortent a ~55 % au lieu de 40 % : sommet <= 0,165,
            // emprise <= 2 x (0,44 + 0,25) = 1,38. La plaque reste BASSE (sous
            // 0,20 a l'echelle 1) - c'est ce que ce test protege.
            check(hi.y < 0.20f && std::max(hi.x - lo.x, hi.z - lo.z) <= 1.45f,
                  "Mesh pierraille : plaque basse d'un metre environ");
        }
    }
    check(meshes[0].triangleCount() > meshes[1].triangleCount(),
          "Variantes distinctes : le gros bloc est plus detaille que le rocher moyen");
}

void testRockPlacement() {
    section("Phase 29 : semis des pierriers (masque, determinisme, densite, LOD, collision)");
    const RockParams defaults;
    ConstantMap bare(0.0f);
    ConstantMap wooded(0.3f);

    // --- Pack / unpack de l'inclinaison ---------------------------------------
    {
        bool ok = true;
        for (float gx = -0.9f; gx <= 0.9f; gx += 0.13f) {
            for (float gz = -0.9f; gz <= 0.9f; gz += 0.17f) {
                const float packed = packRockVariation(0.37f, { gx, gz });
                const glm::vec2 g = unpackRockGradient(packed);
                ok = ok && std::abs(g.x - gx) <= 1.0f / 63.0f + 1e-4f &&
                     std::abs(g.y - gz) <= 1.0f / 63.0f + 1e-4f &&
                     std::abs(unpackRockTint(packed) - 0.37f) < 2e-3f;
            }
        }
        check(ok, "Inclinaison encodee dans variation : gradient a 1/63 pres, teinte conservee");
    }

    // --- Masque : chaque critere de la conception, isolement -----------------
    {
        FlatTerrain flat(1500.0f);
        SlopedTerrain gentle(1500.0f, 0.50f), scree(1500.0f, 0.72f), wall(1500.0f, 0.95f);
        Rocks onFlat(flat, &bare, 1000.0f), onGentle(gentle, &bare, 1000.0f),
              onScree(scree, &bare, 1000.0f), onWall(wall, &bare, 1000.0f),
              wooded72(scree, &wooded, 1000.0f), noMap(scree, nullptr, 1000.0f);
        check(onScree.maskAt(10.0f, 10.0f), "Masque : pente 0,72 nue -> pierrier");
        check(!onFlat.maskAt(10.0f, 10.0f), "Masque : terrain plat -> non");
        check(!onGentle.maskAt(10.0f, 10.0f), "Masque : pente 0,50 (< kWalkComfortSlope) -> non");
        check(!onWall.maskAt(10.0f, 10.0f), "Masque : pente 0,95 (>= kWalkMaxSlope, paroi) -> non");
        check(!wooded72.maskAt(10.0f, 10.0f), "Masque : couvert 0,30 (>= 0,10) -> non");
        check(!noMap.enabled() && !noMap.maskAt(10.0f, 10.0f) &&
                  noMap.blockCellRocks(0, 0).empty() && noMap.gravelCellRocks(0, 0).empty(),
              "Sans carte de couvert : AUCUN rocher (pas de semis sans donnee reelle)");
        SlopedTerrain edgeMin(1500.0f, kWalkComfortSlope + 0.002f);
        SlopedTerrain edgeMax(1500.0f, kWalkMaxSlope + 0.002f);
        check(Rocks(edgeMin, &bare, 1000.0f).maskAt(0.0f, 0.0f) &&
                  !Rocks(edgeMax, &bare, 1000.0f).maskAt(0.0f, 0.0f),
              "Masque : bornes [kWalkComfortSlope, kWalkMaxSlope[ respectees");
        checkNear(onScree.roughnessAt(3.0f, 7.0f), 0.0f, 1e-2f, "Rugosite d'un plan incline : nulle");
    }

    // --- Jamais hors masque, sur un relief ou le masque est une bande -------
    {
        RampingSlope ramp;
        HalfCoveredMap half; // boise a l'est de x = 0
        Rocks rocks(ramp, &half, 2400.0f);
        uint32_t total = 0, outside = 0, badY = 0;
        const auto inspect = [&](const std::vector<RockInstance>& list) {
            for (const RockInstance& r : list) {
                ++total;
                outside += rocks.maskAt(r.position.x, r.position.z) ? 0u : 1u;
                // Bande attendue : pente 0,6..0,84 -> x = -500..100, nue a l'ouest de 0.
                outside += (r.position.x < -505.0f || r.position.x > 0.0f) ? 1u : 0u;
                // PHASE 30 (Agent B) : la plaque de pierraille est ENFONCEE de
                // quelques cm la ou le sol est convexe (assise, voir
                // Rocks::makeCandidate) ; elle n'est donc plus a y = sol exact,
                // mais jamais AU-DESSUS du sol, et jamais de plus de 0,5 m dessous.
                // Blocs et rochers : y = sol, comme en Phase 29.
                const float ground = ramp.heightAt(r.position.x, r.position.z);
                if (r.kind == RockKind::Gravel) {
                    badY += (r.position.y > ground + 1e-2f || r.position.y < ground - 0.5f) ? 1u : 0u;
                } else {
                    badY += std::abs(r.position.y - ground) > 1e-2f ? 1u : 0u;
                }
            }
        };
        for (int32_t cz = -20; cz <= 20; ++cz) {
            for (int32_t cx = -150; cx <= 60; ++cx) { inspect(rocks.blockCellRocks(cx, cz)); }
        }
        for (int32_t cz = -40; cz <= 40; ++cz) {
            for (int32_t cx = -600; cx <= 240; cx += 3) { inspect(rocks.gravelCellRocks(cx, cz)); }
        }
        std::printf("  (%u rochers semes sur la bande, %u hors masque)\n", total, outside);
        check(total > 50, "Bande de pierrier : des rochers sont bien semes");
        check(outside == 0, "AUCUN rocher hors masque (pente, couvert) sur toute la bande");
        check(badY == 0, "Chaque rocher est pose sur le terrain (y = altitude du sol)");
    }

    // --- Rugosite -> choix du mesh -------------------------------------------
    RoughSlope rough(0.72f, 0.50f);
    SlopedTerrain smooth(1500.0f, 0.72f);
    {
        Rocks onRough(rough, &bare, 1000.0f), onSmooth(smooth, &bare, 1000.0f);
        std::printf("  (rugosite du versant bossele : %.3f m)\n", onRough.roughnessAt(5.5f, 5.5f));
        check(onRough.roughnessAt(5.5f, 5.5f) >= defaults.roughBlocks,
              "Rugosite d'un versant bossele (+/- 0,50 m) : >= 0,10 m");
        uint32_t roughBlocks = 0, roughGravel = 0, smoothBlocks = 0, smoothGravel = 0;
        for (int32_t cz = -8; cz <= 8; ++cz) {
            for (int32_t cx = -8; cx <= 8; ++cx) {
                roughBlocks += static_cast<uint32_t>(onRough.blockCellRocks(cx, cz).size());
                smoothBlocks += static_cast<uint32_t>(onSmooth.blockCellRocks(cx, cz).size());
                roughGravel += static_cast<uint32_t>(onRough.gravelCellRocks(cx, cz).size());
                smoothGravel += static_cast<uint32_t>(onSmooth.gravelCellRocks(cx, cz).size());
            }
        }
        std::printf("  (rugueux : %u blocs / %u plaques ; lisse : %u blocs / %u plaques)\n",
                    roughBlocks, roughGravel, smoothBlocks, smoothGravel);
        check(roughBlocks > 0 && smoothBlocks == 0, "Blocs seulement sur le rugueux (>= 0,10 m)");
        check(smoothGravel > 0 && roughGravel == 0, "Pierraille seulement sur le lisse (< 0,10 m)");
    }

    // --- Determinisme, bornes de densite, trainees ---------------------------
    {
        Rocks a(rough, &bare, 1000.0f), b(rough, &bare, 1000.0f);
        RockParams other;
        other.seed = 12345u;
        Rocks c(rough, &bare, 1000.0f, other);
        bool same = true, differs = false, bounded = true;
        uint32_t empty = 0, cells = 0;
        for (int32_t cz = -12; cz <= 12; ++cz) {
            for (int32_t cx = -12; cx <= 12; ++cx) {
                const auto ra = a.blockCellRocks(cx, cz);
                const auto rb = b.blockCellRocks(cx, cz);
                const auto rc = c.blockCellRocks(cx, cz);
                same = same && ra.size() == rb.size();
                for (std::size_t i = 0; same && i < ra.size(); ++i) {
                    same = ra[i].position == rb[i].position && ra[i].scale == rb[i].scale &&
                           ra[i].yaw == rb[i].yaw && ra[i].variation == rb[i].variation;
                }
                differs = differs || rc.size() != ra.size() ||
                          (!ra.empty() && ra[0].position != rc[0].position);
                // PHASE 31 (Agent 2) : + stoneNearSlots (emplacements de proximite,
                // rochers moyens seulement) ; les gros blocs restent bornes par
                // boulderSlots juste en dessous.
                bounded = bounded && ra.size() <= defaults.boulderSlots + defaults.stoneSlots + defaults.stoneNearSlots;
                uint32_t boulders = 0;
                for (const RockInstance& r : ra) {
                    boulders += (r.kind == RockKind::Boulder) ? 1u : 0u;
                    // PHASE 30 (Agent B) : borne basse = stoneSizeMin x roughScaleMin
                    // (etait stoneSizeMin) : la rugosite peut reduire un rocher
                    // moyen de 20 %. Borne haute inchangee (boulderSizeMax).
                    bounded = bounded &&
                              r.scale >= defaults.stoneSizeMin * defaults.roughScaleMin - 1e-4f &&
                              r.scale <= defaults.boulderSizeMax + 1e-4f;
                }
                bounded = bounded && boulders <= defaults.boulderSlots;
                ++cells;
                empty += ra.empty() ? 1u : 0u;
            }
        }
        check(same, "Determinisme : meme graine -> memes rochers, au bit pres");
        check(differs, "Graine differente -> semis different");
        check(bounded, "Densite bornee : <= boulderSlots + stoneSlots (+ stoneNearSlots, Phase 31) par cellule, tailles dans les bornes");
        std::printf("  (%u cellules sous masque : %u vides)\n", cells, empty);
        check(empty > 0 && empty < cells, "Trainees : le bruit module la densite (cellules vides ET peuplees sous masque)");
        const Rocks::Stats st = a.estimate(3);
        std::printf("  (estimation : masque %.2f, %.1f blocs/ha, %.1f rochers/ha)\n",
                    st.maskFraction, st.bouldersPerHa, st.stonesPerHa);
        check(st.maskFraction > 0.6, "Estimation : versant bossele majoritairement sous masque");
        check(st.bouldersPerHa > 0.0 &&
                  st.bouldersPerHa <= 1e4 * defaults.boulderSlots / (defaults.blockCell * defaults.blockCell),
              "Estimation : densite de gros blocs > 0 et sous le plafond des emplacements");
        // Anisotropie : la densite varie moins le long de la pente (x) qu'en travers (z).
        double alongVar = 0.0, acrossVar = 0.0;
        for (int i = 0; i < 400; ++i) {
            const float p = static_cast<float>(i) * 2.0f - 400.0f;
            alongVar += std::abs(a.streakDensityAt(p + 2.0f, 37.0f) - a.streakDensityAt(p, 37.0f));
            acrossVar += std::abs(a.streakDensityAt(37.0f, p + 2.0f) - a.streakDensityAt(37.0f, p));
        }
        std::printf("  (variation de densite : le long %.2f, en travers %.2f)\n", alongVar, acrossVar);
        check(acrossVar > 1.5 * alongVar, "Trainees allongees dans le sens de la pente");
    }

    // --- LOD de densite par distance, rendu, inclinaison ---------------------
    {
        Rocks rocks(rough, &bare, 1000.0f);
        std::vector<InstanceData> boulders, stones, gravel;
        const glm::vec3 eye(0.0f, rough.heightAt(0.0f, 0.0f) + 1.7f, 0.0f);
        for (int i = 0; i < 60; ++i) {
            rocks.gather(eye, permissiveFrustum(), boulders, stones, gravel);
        }
        float maxBoulder = 0.0f, maxStone = 0.0f;
        bool faded = true, tilt = true;
        for (const InstanceData& inst : boulders) {
            maxBoulder = std::max(maxBoulder, glm::length(inst.position - eye));
            faded = faded && inst.fade >= 0.0f && inst.fade <= 1.0f;
            const glm::vec2 g = unpackRockGradient(inst.variation);
            tilt = tilt && std::abs(g.x - 0.72f * defaults.tiltFollow) < 0.25f && std::abs(g.y) < 0.25f;
        }
        for (const InstanceData& inst : stones) {
            maxStone = std::max(maxStone, glm::length(inst.position - eye));
        }
        std::printf("  (%zu gros blocs jusqu'a %.0f m, %zu rochers jusqu'a %.0f m)\n",
                    boulders.size(), maxBoulder, stones.size(), maxStone);
        check(!boulders.empty() && !stones.empty(), "Rendu : des instances de blocs et de rochers");
        check(maxStone < defaults.stoneDistanceFar && maxBoulder < defaults.boulderDistanceFar,
              "LOD : aucune instance au-dela de sa portee");
        check(maxBoulder > defaults.stoneDistanceFar, "LOD : les gros blocs portent plus loin que les rochers");
        check(faded, "LOD : attenuation dans [0,1]");
        check(tilt, "Orientation : gradient encode = pente du sol x tiltFollow");
        check(gravel.empty(), "Versant rugueux : pas de pierraille rendue");
        check(rocks.cachedBlockCount() > 0, "Cache de cellules actif");

        Rocks smoothRocks(smooth, &bare, 1000.0f);
        const glm::vec3 eye2(0.0f, smooth.heightAt(0.0f, 0.0f) + 1.7f, 0.0f);
        for (int i = 0; i < 30; ++i) {
            smoothRocks.gather(eye2, permissiveFrustum(), boulders, stones, gravel);
        }
        float maxGravel = 0.0f;
        uint32_t nearCount = 0, farCount = 0;
        for (const InstanceData& inst : gravel) {
            const float d = glm::length(inst.position - eye2);
            maxGravel = std::max(maxGravel, d);
            if (d < 30.0f) { ++nearCount; } else { ++farCount; }
        }
        std::printf("  (pierraille : %zu plaques, %u a < 30 m, %u au-dela, max %.0f m)\n",
                    gravel.size(), nearCount, farCount, maxGravel);
        check(!gravel.empty() && maxGravel < defaults.gravelDistanceFar, "LOD pierraille : portee courte respectee");
        const double nearArea = 3.14159 * 30.0 * 30.0;
        const double farArea = 3.14159 * (70.0 * 70.0 - 30.0 * 30.0);
        check(static_cast<double>(nearCount) / nearArea > static_cast<double>(farCount) / farArea,
              "LOD de DENSITE : la pierraille s'eclaircit avec la distance");
    }

    // --- Collision des gros blocs ---------------------------------------------
    {
        Rocks rocks(rough, &bare, 1000.0f);
        RockParams noCol;
        noCol.collide = false;
        Rocks ghost(rough, &bare, 1000.0f, noCol);
        RockInstance big;
        bool haveBig = false;
        for (int32_t cz = -8; cz <= 8 && !haveBig; ++cz) {
            for (int32_t cx = -8; cx <= 8 && !haveBig; ++cx) {
                for (const RockInstance& r : rocks.blockCellRocks(cx, cz)) {
                    if (r.kind == RockKind::Boulder && r.scale >= defaults.collideMinScale) {
                        big = r;
                        haveBig = true;
                        break;
                    }
                }
            }
        }
        check(haveBig, "Collision : un gros bloc bloquant existe sur le versant rugueux");
        if (haveBig) {
            BodyCylinder body;
            body.position = { big.position.x + 0.1f, big.position.y + 1.7f, big.position.z };
            body.feetY = big.position.y;
            body.headY = big.position.y + 1.8f;
            check(rocks.overlaps(body), "Collision : le joueur dans un gros bloc est detecte");
            BodyCylinder pushed = body;
            pushed.position = rocks.resolve(body);
            check(!rocks.overlaps(pushed), "Collision : resolve() sort le joueur du bloc");
            check(!ghost.overlaps(body), "TEMOIN --no-rock-collision : bloc traversable");
            BodyCylinder flying = body;
            flying.feetY = big.position.y + big.scale + 5.0f;
            flying.headY = flying.feetY + 1.8f;
            check(!rocks.overlaps(flying), "Collision : un corps au-dessus du bloc (parapente) passe");
            CompositeCollisionSolver composite;
            composite.add(&rocks);
            check(composite.overlaps(body), "Collision : branchee par le CompositeCollisionSolver existant");
        }
        FlatTerrain flat(1500.0f);
        Rocks none(flat, &bare, 1000.0f);
        BodyCylinder body;
        body.position = { 3.0f, 1501.7f, 3.0f };
        body.feetY = 1500.0f;
        body.headY = 1501.8f;
        check(!none.overlaps(body) && none.resolve(body) == body.position,
              "Collision : terrain plat (hors masque) -> aucun obstacle");
    }
}

// ===========================================================================
// PHASE 30 (Agent B) : rochers LISIBLES - taille liee a la rugosite, pierraille
// en cailloux, ombre de contact. Le semis (presence, position, nombre) et la
// collision des gros blocs doivent rester ceux de la Phase 29 AU BIT PRES.
// ===========================================================================

// Empreinte du semis : FNV-1a sur (type, x, z au mm) de chaque instance, et
// separement sur (x, z, echelle au 1/10 000) des seuls GROS BLOCS - ceux qui
// portent la collision. Valeurs de reference relevees avec le code de la
// Phase 29 (build-phase30\agentB\sources_avant), AVANT toute modification.
struct RockFingerprint {
    uint64_t positions = 1469598103934665603ull;
    uint64_t boulders = 1469598103934665603ull;
    uint32_t counts[3] = { 0, 0, 0 };
};
void fnvMix(uint64_t& h, int64_t v) {
    for (int b = 0; b < 8; ++b) {
        h ^= static_cast<uint64_t>((v >> (8 * b)) & 0xFF);
        h *= 1099511628211ull;
    }
}
// PHASE 31 (Agent 2) : `includeNear` = false (defaut) ignore les emplacements de
// PROXIMITE ajoutes en Phase 31 : l'empreinte porte alors exactement sur le semis
// de la Phase 29/30, dont les valeurs de reference ci-dessous sont INCHANGEES.
RockFingerprint rockFingerprint(const Rocks& rocks, int32_t blockRange, int32_t gravelRange,
                                bool includeNear = false) {
    RockFingerprint fp;
    const auto add = [&](const RockInstance& r) {
        if (r.nearSlot && !includeNear) { return; }
        fp.counts[static_cast<int>(r.kind)] += 1;
        fnvMix(fp.positions, static_cast<int64_t>(r.kind));
        fnvMix(fp.positions, std::llround(r.position.x * 1000.0));
        fnvMix(fp.positions, std::llround(r.position.z * 1000.0));
        if (r.kind == RockKind::Boulder) {
            fnvMix(fp.boulders, std::llround(r.position.x * 1000.0));
            fnvMix(fp.boulders, std::llround(r.position.z * 1000.0));
            fnvMix(fp.boulders, std::llround(r.scale * 10000.0));
        }
    };
    for (int32_t cz = -blockRange; cz <= blockRange; ++cz) {
        for (int32_t cx = -blockRange; cx <= blockRange; ++cx) {
            for (const RockInstance& r : rocks.blockCellRocks(cx, cz)) { add(r); }
        }
    }
    for (int32_t cz = -gravelRange; cz <= gravelRange; ++cz) {
        for (int32_t cx = -gravelRange; cx <= gravelRange; ++cx) {
            for (const RockInstance& r : rocks.gravelCellRocks(cx, cz)) { add(r); }
        }
    }
    return fp;
}

void testRockReadability() {
    section("Phase 30 : rochers lisibles (taille ~ rugosite, semis et collision inchanges)");
    ConstantMap bare(0.0f);
    RoughSlope rough(0.72f, 0.50f);
    SlopedTerrain smooth(1500.0f, 0.72f);

    // --- Ancre de non-regression : semis et gros blocs de la Phase 29 --------
    {
        Rocks onRough(rough, &bare, 1000.0f), onSmooth(smooth, &bare, 1000.0f);
        const RockFingerprint a = rockFingerprint(onRough, 8, 24);
        const RockFingerprint b = rockFingerprint(onSmooth, 8, 24);
        std::printf("  (rugueux : %u blocs %u rochers %u plaques, positions %016llx, gros blocs %016llx)\n",
                    a.counts[0], a.counts[1], a.counts[2], static_cast<unsigned long long>(a.positions),
                    static_cast<unsigned long long>(a.boulders));
        std::printf("  (lisse   : %u blocs %u rochers %u plaques, positions %016llx)\n",
                    b.counts[0], b.counts[1], b.counts[2], static_cast<unsigned long long>(b.positions));
        check(a.counts[0] == 1204u && a.counts[1] == 2889u && a.counts[2] == 0u,
              "Phase 30 : comptes par type IDENTIQUES a la Phase 29 (versant rugueux)");
        check(a.positions == 0xffd1c148f9c8c6fbull, "Phase 30 : positions IDENTIQUES a la Phase 29 (versant rugueux)");
        check(a.boulders == 0xff89983fdbfdc457ull,
              "Phase 30 : gros blocs (position ET echelle, donc collision) IDENTIQUES a la Phase 29");
        check(b.counts[0] == 0u && b.counts[1] == 0u && b.counts[2] == 15168u &&
                  b.positions == 0xfc49aaeae3bcd6feull,
              "Phase 30 : pierraille du versant lisse : comptes et positions IDENTIQUES a la Phase 29");
        // PHASE 31 (Agent 2) : AVEC les emplacements de proximite, les gros blocs
        // (position ET echelle) restent ceux de la Phase 29 - aucun bloc ajoute.
        const RockFingerprint all = rockFingerprint(onRough, 8, 24, true);
        std::printf("  (avec proximite : %u blocs %u rochers %u plaques, gros blocs %016llx)\n", all.counts[0],
                    all.counts[1], all.counts[2], static_cast<unsigned long long>(all.boulders));
        check(all.counts[0] == 1204u && all.boulders == 0xff89983fdbfdc457ull,
              "Phase 31 : avec la proximite, gros blocs IDENTIQUES a la Phase 29 (empreinte)");
        check(all.counts[1] > a.counts[1], "Phase 31 : la proximite ajoute bien des rochers moyens (effet non nul)");
    }

    const RockParams defaults;

    // --- Loi rugosite -> facteur de taille -----------------------------------
    {
        checkNear(rockRoughnessScale(0.15f, defaults), 1.0f, 1e-6f, "Facteur de taille : 1 a la rugosite de reference 0,15 m");
        checkNear(rockRoughnessScale(0.0f, defaults), defaults.roughScaleMin, 1e-6f, "Facteur de taille : borne basse 0,8 (sol lisse)");
        checkNear(rockRoughnessScale(0.10f, defaults), defaults.roughScaleMin, 1e-6f, "Facteur de taille : 0,10 m (seuil des blocs) -> 0,8");
        checkNear(rockRoughnessScale(0.30f, defaults), 2.0f, 1e-6f, "Facteur de taille : 0,30 m -> 2");
        checkNear(rockRoughnessScale(5.0f, defaults), defaults.roughScaleMax, 1e-6f, "Facteur de taille : borne haute 2 (artefact de paroi)");
        bool monotone = true;
        for (float r = 0.0f; r < 1.0f; r += 0.01f) {
            monotone = monotone && rockRoughnessScale(r + 0.01f, defaults) >= rockRoughnessScale(r, defaults);
        }
        check(monotone, "Facteur de taille : monotone croissant en rugosite");
        check(defaults.stoneSizeCap < defaults.collideMinScale,
              "Plafond des rochers moyens SOUS le seuil de collision (pas de faux bloc traversable)");
    }

    // --- Rochers moyens : taille = base x facteur, bornee, deterministe -------
    {
        RoughSlope rougher(0.72f, 1.50f);
        Rocks onRough(rough, &bare, 1000.0f), onRougher(rougher, &bare, 1000.0f),
              again(rougher, &bare, 1000.0f);
        std::vector<float> s1, s2;
        bool law = true, capped = true, same = true;
        uint32_t boulderScaleOk = 0, boulders = 0;
        const auto inspect = [&](const Rocks& rocks, std::vector<float>& out) {
            for (int32_t cz = -6; cz <= 6; ++cz) {
                for (int32_t cx = -6; cx <= 6; ++cx) {
                    for (const RockInstance& r : rocks.blockCellRocks(cx, cz)) {
                        if (r.kind == RockKind::Boulder) {
                            ++boulders;
                            boulderScaleOk += (r.scale >= defaults.boulderSizeMin - 1e-4f &&
                                               r.scale <= defaults.boulderSizeMax + 1e-4f) ? 1u : 0u;
                            continue;
                        }
                        out.push_back(r.scale);
                        capped = capped && r.scale <= defaults.stoneSizeCap + 1e-6f &&
                                 r.scale >= defaults.stoneSizeMin * defaults.roughScaleMin - 1e-4f;
                        const float k = rockRoughnessScale(rocks.roughnessAt(r.position.x, r.position.z), defaults);
                        if (r.scale < defaults.stoneSizeCap - 1e-4f) {
                            // Hors plafond, l'echelle divisee par le facteur retombe dans la plage de base.
                            const float base = r.scale / k;
                            law = law && base >= defaults.stoneSizeMin - 1e-3f && base <= defaults.stoneSizeMax + 1e-3f;
                        }
                    }
                }
            }
        };
        inspect(onRough, s1);
        inspect(onRougher, s2);
        for (int32_t cz = -6; cz <= 6 && same; ++cz) {
            for (int32_t cx = -6; cx <= 6 && same; ++cx) {
                const auto a = onRougher.blockCellRocks(cx, cz), b = again.blockCellRocks(cx, cz);
                same = a.size() == b.size();
                for (std::size_t i = 0; same && i < a.size(); ++i) { same = a[i].scale == b[i].scale; }
            }
        }
        std::sort(s1.begin(), s1.end());
        std::sort(s2.begin(), s2.end());
        const float m1 = s1.empty() ? 0.0f : s1[s1.size() / 2];
        const float m2 = s2.empty() ? 0.0f : s2[s2.size() / 2];
        std::printf("  (rochers moyens : rugosite %.3f -> mediane %.2f (%zu) ; rugosite %.3f -> mediane %.2f (%zu))\n",
                    onRough.roughnessAt(5.5f, 5.5f), m1, s1.size(), onRougher.roughnessAt(5.5f, 5.5f), m2, s2.size());
        check(!s1.empty() && !s2.empty(), "Rochers moyens presents sur les deux versants");
        check(law, "Rochers moyens : echelle = base [0,6 ; 1,4] x facteur de rugosite");
        check(capped, "Rochers moyens : echelle dans [0,48 ; stoneSizeCap]");
        check(m2 > 1.2f * m1, "Rochers moyens : plus gros sur le versant plus rugueux");
        check(same, "Rochers moyens : taille deterministe (meme graine -> meme echelle au bit pres)");
        check(boulders > 0 && boulderScaleOk == boulders,
              "Gros blocs : loi de taille de la Phase 29 inchangee (1,1-2,6, independante de la rugosite)");
    }

    // Composantes d'un mesh de rochers : sommets relies par un triangle ou qui
    // partagent une position exacte (un caillou = une composante).
    const auto rockParts = [](const MeshData& mesh) {
        const std::size_t n = mesh.vertices.size();
        std::vector<std::size_t> parent(n);
        for (std::size_t i = 0; i < n; ++i) { parent[i] = i; }
        const auto root = [&](std::size_t i) {
            while (parent[i] != i) { i = parent[i] = parent[parent[i]]; }
            return i;
        };
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            parent[root(mesh.indices[t + 1])] = root(mesh.indices[t]);
            parent[root(mesh.indices[t + 2])] = root(mesh.indices[t]);
        }
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                if (mesh.vertices[i].pos == mesh.vertices[j].pos) { parent[root(j)] = root(i); }
            }
        }
        std::vector<std::size_t> part(n);
        for (std::size_t i = 0; i < n; ++i) { part[i] = root(i); }
        return part;
    };

    // --- Meshes : cailloux plus gros, ombre de contact, pas d'eclaircissement --
    {
        checkNear(rockContactShade(0.0f, 1.0f), kRockContactShade, 1e-6f, "Ombre de contact : x0,55 au sol");
        checkNear(rockContactShade(-0.2f, 1.0f), kRockContactShade, 1e-6f, "Ombre de contact : x0,55 sous le sol");
        checkNear(rockContactShade(kRockContactHeight, 1.0f), 1.0f, 1e-6f, "Ombre de contact : x1 a 0,4 du sommet");
        checkNear(rockContactShade(0.9f, 1.0f), 1.0f, 1e-6f, "Ombre de contact : x1 au-dessus");
        const MeshData boulder = boulderMeshData();
        float lowSum = 0.0f, highSum = 0.0f;
        uint32_t lowN = 0, highN = 0;
        for (const Vertex& v : boulder.vertices) {
            const float l = v.color.r + v.color.g + v.color.b;
            if (v.pos.y >= 0.0f && v.pos.y < 0.05f) { lowSum += l; ++lowN; }
            if (v.pos.y > 0.45f) { highSum += l; ++highN; }
        }
        check(lowN > 0 && highN > 0 && lowSum / lowN < 0.75f * (highSum / highN),
              "Gros bloc : le pied est nettement plus sombre que le haut (ombre de contact)");
        // Ne PAS eclaircir : aucun sommet plus clair que le plus clair de la Phase 29
        // (calcaire 0,63 x nuance 1,04 x teinte de caillou 1,05).
        float brightest = 0.0f;
        for (const MeshData& m : { boulder, rockMeshData(), gravelMeshData() }) {
            for (const Vertex& v : m.vertices) { brightest = std::max(brightest, v.color.r); }
        }
        check(brightest <= 0.63f * 1.04f * 1.05f + 1e-4f, "Rochers : albedo jamais plus clair qu'en Phase 29");
        float boulderTop = 0.0f;
        for (const Vertex& v : boulder.vertices) {
            if (v.pos.y > 0.45f) { boulderTop = std::max(boulderTop, v.color.r); }
        }
        check(boulderTop <= 0.63f * 0.85f * 1.04f + 1e-4f, "Gros bloc : patine grise (albedo <= 0,56)");

        const MeshData gravel = gravelMeshData();
        const std::vector<std::size_t> part = rockParts(gravel);
        std::vector<std::size_t> roots;
        std::vector<float> tops, widths;
        for (std::size_t i = 0; i < part.size(); ++i) {
            if (std::find(roots.begin(), roots.end(), part[i]) != roots.end()) { continue; }
            roots.push_back(part[i]);
            glm::vec3 lo(1e9f), hi(-1e9f);
            for (std::size_t j = 0; j < part.size(); ++j) {
                if (part[j] == part[i]) {
                    lo = glm::min(lo, gravel.vertices[j].pos);
                    hi = glm::max(hi, gravel.vertices[j].pos);
                }
            }
            tops.push_back(hi.y);
            widths.push_back(std::max(hi.x - lo.x, hi.z - lo.z));
        }
        std::sort(tops.begin(), tops.end());
        std::sort(widths.begin(), widths.end());
        std::printf("  (pierraille : %zu cailloux, sommet median %.3f, largeur mediane %.3f a l'echelle 1)\n",
                    roots.size(), tops[tops.size() / 2], widths[widths.size() / 2]);
        check(roots.size() == 11, "Pierraille : 11 cailloux par plaque (comme en Phase 29)");
        check(tops.front() > 0.0f && tops[tops.size() / 2] >= 0.06f,
              "Pierraille : chaque caillou depasse du sol, sommet median >= 6 cm a l'echelle 1");
        check(widths[widths.size() / 2] >= 0.18f, "Pierraille : largeur mediane d'un caillou >= 18 cm a l'echelle 1");
        // Budget de triangles : MESURE (entrelacee, 4 paires, vue pierrier a 361
        // plaques) : 32 faces par caillou = +0,50 ms de drawFrame, 20 faces =
        // +0,26 ms. Ce test empeche de revenir silencieusement a la sphere
        // subdivisee : au plus 20 faces (icosaedre) par caillou.
        check(gravel.indices.size() / 3 <= 11u * 20u,
              "Pierraille : au plus 20 triangles par caillou (cout GPU mesure)");
        // Faces sortantes (icosaedre oriente par le code, pas suppose) : la
        // normale de chaque facette s'eloigne du centre de son caillou.
        std::vector<glm::vec3> centre(part.size(), glm::vec3(0.0f));
        std::vector<float> weight(part.size(), 0.0f);
        for (std::size_t i = 0; i < part.size(); ++i) {
            centre[part[i]] += gravel.vertices[i].pos;
            weight[part[i]] += 1.0f;
        }
        bool outward = true;
        for (std::size_t i = 0; i < part.size(); ++i) {
            const glm::vec3 c = centre[part[i]] / weight[part[i]];
            outward = outward && glm::dot(gravel.vertices[i].normal, gravel.vertices[i].pos - c) > -1e-4f;
        }
        check(outward, "Pierraille : facettes orientees vers l'exterieur du caillou");
    }

    // --- Assise : aucun caillou ne flotte sur un sol qui se courbe -----------
    {
        // Versant 0,72 ondule (+/- 10 cm, periode ~3 m) : rugosite < 0,10, donc de
        // la pierraille, et un sol qui se courbe sous chaque plaque.
        class WavySlope : public HeightSampler {
        public:
            float heightAt(float x, float z) const override {
                return 1500.0f + 0.72f * x + 0.10f * std::sin(x * 1.9f) * std::cos(z * 2.3f + 0.4f);
            }
        };
        WavySlope wavy;
        Rocks rocks(wavy, &bare, 1000.0f);
        const MeshData gravel = gravelMeshData();
        const std::vector<std::size_t> part = rockParts(gravel);
        uint32_t plates = 0, pebbles = 0, floating = 0, floatingRaw = 0;
        for (int32_t cz = -10; cz <= 10 && plates < 80; ++cz) {
            for (int32_t cx = -10; cx <= 10 && plates < 80; ++cx) {
                for (const RockInstance& r : rocks.gravelCellRocks(cx, cz)) {
                    ++plates;
                    // Meme transformation que foliage.vert : echelle, lacet, inclinaison.
                    const glm::vec2 g = unpackRockGradient(r.variation);
                    const glm::vec3 nrm = glm::normalize(glm::vec3(-g.x, 1.0f, -g.y));
                    const glm::vec3 axis(nrm.z, 0.0f, -nrm.x);
                    const float sine = glm::length(axis);
                    glm::mat3 tilt(1.0f);
                    if (sine > 1e-4f) {
                        const glm::vec3 k = axis / sine;
                        const glm::mat3 K(0.0f, k.z, -k.y, -k.z, 0.0f, k.x, k.y, -k.x, 0.0f);
                        tilt = glm::mat3(nrm.y) + sine * K + (1.0f - nrm.y) * glm::outerProduct(k, k);
                    }
                    const float c = std::cos(r.yaw), sn = std::sin(r.yaw);
                    // Temoin negatif : la meme plaque posee a y = sol exact (sans assise).
                    const glm::vec3 unseated(r.position.x, wavy.heightAt(r.position.x, r.position.z), r.position.z);
                    std::vector<float> partMin(part.size(), 1e9f), rawMin(part.size(), 1e9f);
                    for (std::size_t i = 0; i < part.size(); ++i) {
                        const glm::vec3 l = gravel.vertices[i].pos * r.scale;
                        const glm::vec3 o = tilt * glm::vec3(c * l.x + sn * l.z, l.y, -sn * l.x + c * l.z);
                        const glm::vec3 w = r.position + o, w0 = unseated + o;
                        partMin[part[i]] = std::min(partMin[part[i]], w.y - wavy.heightAt(w.x, w.z));
                        rawMin[part[i]] = std::min(rawMin[part[i]], w0.y - wavy.heightAt(w0.x, w0.z));
                    }
                    for (std::size_t i = 0; i < part.size(); ++i) {
                        if (part[i] != i) { continue; }
                        ++pebbles;
                        floating += partMin[i] > 0.0f ? 1u : 0u;
                        floatingRaw += rawMin[i] > 0.0f ? 1u : 0u;
                    }
                }
            }
        }
        std::printf("  (assise : %u plaques, %u cailloux, %u flottants ; sans assise %u ; rugosite %.3f)\n",
                    plates, pebbles, floating, floatingRaw, rocks.roughnessAt(3.0f, 3.0f));
        check(floatingRaw > 0,
              "Assise : temoin negatif - sans enfoncement, des cailloux flotteraient (le test voit le defaut)");
        check(plates > 0 && pebbles == 11u * plates, "Assise : plaques de pierraille semees sur le versant ondule");
        check(floating == 0, "Assise : AUCUN caillou dont le point bas est au-dessus du sol");
    }
}

// ===========================================================================
// PHASE 31 (Agent 2) : DENSITE pres du joueur. Emplacements de PROXIMITE (portee
// courte, fondu large) sous le MEME masque ; plafond d'instances par bloc ; gros
// blocs et collision inchanges ; determinisme independant du chemin ; trainees
// toujours lisibles dans la densite reelle.
// ===========================================================================

// Parametres "Phase 30" : les memes, sans emplacement de proximite (temoin).
RockParams phase30RockParams() {
    RockParams p;
    p.stoneNearSlots = 0;
    p.gravelNearSlots = 0;
    return p;
}

void testRockNearDensity() {
    section("Phase 31 : densite des rochers pres du joueur (proximite, plafond, collision, chemin)");
    const RockParams defaults;
    ConstantMap bare(0.0f);
    RoughSlope rough(0.72f, 0.50f);
    SlopedTerrain smooth(1500.0f, 0.72f);

    // --- Les emplacements de proximite : rochers moyens et pierraille seulement --
    {
        Rocks onRough(rough, &bare, 1000.0f), onSmooth(smooth, &bare, 1000.0f);
        uint32_t nearStones = 0, nearGravel = 0, nearBoulders = 0;
        bool sizes = true, ranges = true;
        for (int32_t cz = -8; cz <= 8; ++cz) {
            for (int32_t cx = -8; cx <= 8; ++cx) {
                for (const RockInstance& r : onRough.blockCellRocks(cx, cz)) {
                    if (!r.nearSlot) { continue; }
                    nearBoulders += r.kind == RockKind::Boulder ? 1u : 0u;
                    nearStones += r.kind == RockKind::Stone ? 1u : 0u;
                    sizes = sizes && r.scale <= defaults.stoneSizeCap + 1e-6f;
                    ranges = ranges && r.maxDistance <= defaults.stoneNearDistanceFar + 1e-3f &&
                             r.maxDistance >= defaults.stoneNearDistanceNear - 1e-3f;
                }
            }
        }
        for (int32_t cz = -24; cz <= 24; ++cz) {
            for (int32_t cx = -24; cx <= 24; ++cx) {
                for (const RockInstance& r : onSmooth.gravelCellRocks(cx, cz)) {
                    if (!r.nearSlot) { continue; }
                    ++nearGravel;
                    ranges = ranges && r.kind == RockKind::Gravel &&
                             r.maxDistance <= defaults.gravelNearDistanceFar + 1e-3f &&
                             r.maxDistance >= defaults.gravelNearDistanceNear - 1e-3f;
                }
            }
        }
        std::printf("  (proximite : %u rochers moyens (versant rugueux), %u plaques (versant lisse), %u gros blocs)\n",
                    nearStones, nearGravel, nearBoulders);
        check(nearStones > 0 && nearGravel > 0, "Proximite : des rochers moyens ET des plaques sont ajoutes (effet non nul)");
        check(nearBoulders == 0, "Proximite : AUCUN gros bloc ajoute (les blocs solides ne changent pas)");
        check(sizes && defaults.stoneSizeCap < defaults.collideMinScale,
              "Proximite : rochers moyens plafonnes sous le seuil de collision (non solides)");
        check(ranges, "Proximite : portees dans [Near, Far] de proximite (portee COURTE)");
        check(defaults.stoneNearDistanceFar <= defaults.stoneDistanceNear &&
                  defaults.gravelNearDistanceFar <= defaults.gravelDistanceNear,
              "Proximite : portee max sous la plus courte portee d'origine");
        // Classe toujours decidee par la rugosite : pas de pierraille sur le rugueux,
        // pas de rocher sur le lisse, proximite comprise.
        uint32_t wrong = 0;
        for (int32_t cz = -8; cz <= 8; ++cz) {
            for (int32_t cx = -8; cx <= 8; ++cx) {
                wrong += static_cast<uint32_t>(onSmooth.blockCellRocks(cx, cz).size());
                wrong += static_cast<uint32_t>(onRough.gravelCellRocks(cx, cz).size());
            }
        }
        check(wrong == 0, "Proximite : regle rugosite -> classe respectee (rien de rugueux sur le lisse ni l'inverse)");
    }

    // --- Collision et blocs solides : IDENTIQUES au temoin sans proximite -------
    {
        Rocks rocks(rough, &bare, 1000.0f), witness(rough, &bare, 1000.0f, phase30RockParams());
        bool sameSolids = true;
        uint32_t solids = 0, bitten = 0;
        for (int32_t bz = -2; bz <= 1; ++bz) {
            for (int32_t bx = -2; bx <= 1; ++bx) {
                std::vector<RockInstance> a, b;
                for (const RockInstance& r : rocks.blockRocks(bx, bz, false)) {
                    if (r.kind == RockKind::Boulder) { a.push_back(r); }
                }
                for (const RockInstance& r : witness.blockRocks(bx, bz, false)) {
                    if (r.kind == RockKind::Boulder) { b.push_back(r); }
                }
                sameSolids = sameSolids && a.size() == b.size();
                for (std::size_t i = 0; sameSolids && i < a.size(); ++i) {
                    sameSolids = a[i].position == b[i].position && a[i].scale == b[i].scale;
                    solids += a[i].scale >= defaults.collideMinScale ? 1u : 0u;
                }
            }
        }
        // Corps sondes sur une grille dense autour des blocs : overlaps ET resolve.
        bool sameCollision = true;
        uint32_t hits = 0;
        for (float z = -60.0f; z <= 60.0f; z += 0.73f) {
            for (float x = -60.0f; x <= 60.0f; x += 0.79f) {
                BodyCylinder body;
                const float ground = rough.heightAt(x, z);
                body.position = { x, ground + 1.7f, z };
                body.feetY = ground;
                body.headY = ground + 1.8f;
                const bool o = rocks.overlaps(body);
                hits += o ? 1u : 0u;
                sameCollision = sameCollision && o == witness.overlaps(body) &&
                                rocks.resolve(body) == witness.resolve(body);
            }
        }
        (void)bitten;
        std::printf("  (blocs solides compares : %u ; corps en contact : %u)\n", solids, hits);
        check(solids > 0 && sameSolids, "Collision : ensemble des gros blocs (position, echelle) IDENTIQUE au temoin sans proximite");
        check(hits > 0 && sameCollision, "Collision : overlaps et resolve IDENTIQUES au temoin sur ~25 000 corps");
    }

    // --- Plafond par bloc : borne, origine intacte, eclaircissement uniforme ------
    {
        RockParams tight;
        tight.gravelInstanceCap = 64u * tight.gravelSlots; // le plus bas permis : 768
        Rocks capped(smooth, &bare, 1000.0f, tight);
        uint32_t blocksBitten = 0;
        bool bounded = true, originKept = true, subset = true, spread = true;
        for (int32_t bz = -2; bz <= 1; ++bz) {
            for (int32_t bx = -2; bx <= 1; ++bx) {
                std::vector<RockInstance> origin, nearAll;
                std::vector<int32_t> nearCellOf;
                for (int32_t j = 0; j < Rocks::kBlockCells; ++j) {
                    for (int32_t i = 0; i < Rocks::kBlockCells; ++i) {
                        for (const RockInstance& r : capped.gravelCellRocks(bx * 8 + i, bz * 8 + j)) {
                            (r.nearSlot ? nearAll : origin).push_back(r);
                            if (r.nearSlot) { nearCellOf.push_back(j * 8 + i); }
                        }
                    }
                }
                const std::vector<RockInstance> block = capped.blockRocks(bx, bz, true);
                bounded = bounded && block.size() <= tight.gravelInstanceCap;
                uint32_t keptOrigin = 0;
                std::vector<bool> cellKept(64, false), cellHad(64, false);
                for (const int32_t c : nearCellOf) { cellHad[static_cast<std::size_t>(c)] = true; }
                for (const RockInstance& r : block) {
                    if (!r.nearSlot) {
                        ++keptOrigin;
                        continue;
                    }
                    bool found = false;
                    for (std::size_t k = 0; k < nearAll.size() && !found; ++k) {
                        if (nearAll[k].position == r.position && nearAll[k].rank == r.rank) {
                            found = true;
                            cellKept[static_cast<std::size_t>(nearCellOf[k])] = true;
                        }
                    }
                    subset = subset && found;
                }
                originKept = originKept && keptOrigin == origin.size();
                if (origin.size() + nearAll.size() > tight.gravelInstanceCap) {
                    ++blocksBitten;
                    // Eclaircissement UNIFORME : les cellules qui avaient de la
                    // proximite en gardent presque toutes (pas de coupe par ordre).
                    uint32_t had = 0, kept = 0;
                    for (int c = 0; c < 64; ++c) {
                        had += cellHad[c] ? 1u : 0u;
                        kept += (cellHad[c] && cellKept[c]) ? 1u : 0u;
                    }
                    spread = spread && block.size() == tight.gravelInstanceCap && kept * 10u >= had * 8u;
                }
            }
        }
        std::printf("  (plafond %u par bloc : %u blocs eclaircis sur 16)\n", tight.gravelInstanceCap, blocksBitten);
        check(blocksBitten > 0, "Plafond : le cas teste le declenche bien (effet non nul)");
        check(bounded, "Plafond : jamais plus d'instances par bloc que le plafond");
        check(originKept, "Plafond : les instances d'origine ne sont JAMAIS retirees");
        check(subset, "Plafond : les instances gardees sont des candidats de proximite du bloc (au bit)");
        check(spread, "Plafond : plein exactement, eclaircissement uniforme (>= 80 % des cellules gardent de la proximite)");
        bool refused = false;
        try {
            RockParams bad;
            bad.blockInstanceCap = 64u * (bad.boulderSlots + bad.stoneSlots) - 1u;
            Rocks r(smooth, &bare, 1000.0f, bad);
        } catch (const std::exception&) {
            refused = true;
        }
        check(refused, "Plafond : un plafond sous le maximum d'origine est refuse (origine jamais tronquee)");
    }

    // --- Determinisme : meme resultat quel que soit le chemin parcouru -------------
    {
        Rocks pathA(smooth, &bare, 1000.0f), pathB(smooth, &bare, 1000.0f);
        Rocks roughA(rough, &bare, 1000.0f), roughB(rough, &bare, 1000.0f);
        std::vector<InstanceData> b1, s1, g1, b2, s2, g2;
        const auto eyeAt = [](const HeightSampler& t, float x, float z) { return glm::vec3(x, t.heightAt(x, z) + 1.7f, z); };
        // A : arrive par l'ouest ; B : par l'est puis le nord (ordres de semis differents, purges comprises).
        for (float x = -400.0f; x <= 0.0f; x += 7.0f) {
            pathA.gather(eyeAt(smooth, x, 0.0f), permissiveFrustum(), b1, s1, g1);
            roughA.gather(eyeAt(rough, x, 0.0f), permissiveFrustum(), b1, s1, g1);
        }
        for (float x = 400.0f; x >= 0.0f; x -= 11.0f) {
            pathB.gather(eyeAt(smooth, x, 300.0f), permissiveFrustum(), b2, s2, g2);
            roughB.gather(eyeAt(rough, x, 300.0f), permissiveFrustum(), b2, s2, g2);
        }
        for (float z = 300.0f; z >= 0.0f; z -= 13.0f) {
            pathB.gather(eyeAt(smooth, 0.0f, z), permissiveFrustum(), b2, s2, g2);
            roughB.gather(eyeAt(rough, 0.0f, z), permissiveFrustum(), b2, s2, g2);
        }
        const auto same = [](const std::vector<InstanceData>& a, const std::vector<InstanceData>& b) {
            if (a.size() != b.size()) { return false; }
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (a[i].position != b[i].position || a[i].scale != b[i].scale || a[i].yaw != b[i].yaw ||
                    a[i].variation != b[i].variation || a[i].fade != b[i].fade) {
                    return false;
                }
            }
            return true;
        };
        bool identical = true;
        std::size_t gravelSeen = 0, stonesSeen = 0;
        for (int i = 0; i < 80; ++i) { // budget de semis : on laisse les deux caches se remplir
            pathA.gather(eyeAt(smooth, 0.0f, 0.0f), permissiveFrustum(), b1, s1, g1);
            pathB.gather(eyeAt(smooth, 0.0f, 0.0f), permissiveFrustum(), b2, s2, g2);
        }
        identical = identical && same(b1, b2) && same(s1, s2) && same(g1, g2);
        gravelSeen = g1.size();
        for (int i = 0; i < 80; ++i) {
            roughA.gather(eyeAt(rough, 0.0f, 0.0f), permissiveFrustum(), b1, s1, g1);
            roughB.gather(eyeAt(rough, 0.0f, 0.0f), permissiveFrustum(), b2, s2, g2);
        }
        identical = identical && same(b1, b2) && same(s1, s2) && same(g1, g2);
        stonesSeen = s1.size();
        // Blocs construits dans deux ordres differents : contenu identique au bit.
        Rocks fwd(smooth, &bare, 1000.0f), bwd(smooth, &bare, 1000.0f);
        std::vector<std::vector<RockInstance>> f, b;
        for (int32_t k = -3; k <= 3; ++k) { f.push_back(fwd.blockRocks(k, -k, true)); }
        for (int32_t k = 3; k >= -3; --k) { b.insert(b.begin(), bwd.blockRocks(k, -k, true)); }
        bool sameBlocks = f.size() == b.size();
        for (std::size_t i = 0; sameBlocks && i < f.size(); ++i) {
            sameBlocks = f[i].size() == b[i].size();
            for (std::size_t j = 0; sameBlocks && j < f[i].size(); ++j) {
                sameBlocks = f[i][j].position == b[i][j].position && f[i][j].scale == b[i][j].scale &&
                             f[i][j].rank == b[i][j].rank && f[i][j].nearSlot == b[i][j].nearSlot;
            }
        }
        std::printf("  (chemins differents, meme oeil : %zu plaques, %zu rochers dessines)\n", gravelSeen, stonesSeen);
        check(gravelSeen > 0 && stonesSeen > 0 && identical,
              "Determinisme : deux chemins differents -> MEMES instances dessinees au meme point, au bit");
        check(sameBlocks, "Determinisme : un bloc ne depend pas de l'ordre dans lequel les blocs sont semes");
    }

    // --- LOD : la densite ajoutee s'eteint avec la distance, fondu sans saut -------
    {
        Rocks rocks(smooth, &bare, 1000.0f), witness(smooth, &bare, 1000.0f, phase30RockParams());
        std::vector<InstanceData> b, s, g, wb, ws, wg;
        const glm::vec3 eye(0.0f, smooth.heightAt(0.0f, 0.0f) + 1.7f, 0.0f);
        for (int i = 0; i < 80; ++i) {
            rocks.gather(eye, permissiveFrustum(), b, s, g);
            witness.gather(eye, permissiveFrustum(), wb, ws, wg);
        }
        const auto band = [&](const std::vector<InstanceData>& v, float lo, float hi) {
            uint32_t n = 0;
            for (const InstanceData& in : v) {
                const float d = glm::length(in.position - eye);
                n += (d >= lo && d < hi) ? 1u : 0u;
            }
            return n;
        };
        const uint32_t near0 = band(g, 0.0f, 12.0f), wnear0 = band(wg, 0.0f, 12.0f);
        const uint32_t far0 = band(g, defaults.gravelNearDistanceFar + 0.5f, 1e9f);
        const uint32_t wfar0 = band(wg, defaults.gravelNearDistanceFar + 0.5f, 1e9f);
        std::printf("  (plaques a < 12 m : %u contre %u sans proximite ; au-dela de %.0f m : %u contre %u)\n", near0, wnear0,
                    defaults.gravelNearDistanceFar, far0, wfar0);
        check(near0 >= 2u * wnear0 && wnear0 > 0, "LOD : densite de plaques au moins doublee a < 12 m");
        check(far0 == wfar0, "LOD : au-dela de la portee de proximite, EXACTEMENT les instances d'origine (aucun surcout lointain)");
        // Fondu : continu (pas de saut a la limite), et enfonce la plaque qui rapetisse.
        bool fadeOk = true;
        for (const InstanceData& in : g) {
            fadeOk = fadeOk && in.fade >= 0.0f && in.fade <= 1.0f;
        }
        check(fadeOk, "LOD : attenuation dans [0,1]");
        checkNear(rockFadeSink(RockKind::Gravel, 1.2f, 1.0f), 0.0f, 1e-9f, "Fondu : aucun enfoncement a pleine echelle (assise inchangee)");
        check(rockFadeSink(RockKind::Gravel, 1.0f, 0.3f) > rockFadeSink(RockKind::Gravel, 1.0f, 0.6f) &&
                  rockFadeSink(RockKind::Stone, 1.0f, 0.3f) == 0.0f && rockFadeSink(RockKind::Boulder, 1.0f, 0.3f) == 0.0f,
              "Fondu : la plaque s'enfonce en rapetissant ; blocs et rochers inchanges");
        // Pas de pop : une instance de proximite, vue par gather depuis un oeil place
        // juste EN DECA de sa portee, est presque nulle ; juste au-dela, absente ;
        // a mi-portee, entiere. Mesure sur le code, pas recalculee ici.
        RockInstance target;
        bool haveTarget = false;
        // Cherchee APRES le plafond (blockRocks) : c'est ce que gather lit.
        for (const RockInstance& r : rocks.blockRocks(0, 0, true)) {
            if (r.nearSlot && r.maxDistance > 20.0f) { target = r; haveTarget = true; break; }
        }
        const auto fadeSeenAt = [&](float fraction) {
            Rocks fresh(smooth, &bare, 1000.0f);
            const glm::vec3 eyeAt = target.position + glm::vec3(0.0f, 0.0f, fraction * target.maxDistance);
            std::vector<InstanceData> bb, ss, gg;
            for (int i = 0; i < 40; ++i) { fresh.gather(eyeAt, permissiveFrustum(), bb, ss, gg); }
            for (const InstanceData& in : gg) {
                if (in.position.x == target.position.x && in.position.z == target.position.z) { return in.fade; }
            }
            return -1.0f; // non dessinee
        };
        const float atHalf = fadeSeenAt(0.5f), atEdge = fadeSeenAt(0.995f), beyond = fadeSeenAt(1.005f);
        std::printf("  (fondu d'une plaque de proximite (portee %.1f m) : %.3f a mi-portee, %.3f a 99,5 %%, %s au-dela)\n",
                    target.maxDistance, atHalf, atEdge, beyond < 0.0f ? "absente" : "PRESENTE");
        check(haveTarget && atHalf == 1.0f && atEdge >= 0.0f && atEdge < 0.02f && beyond < 0.0f,
              "Fondu : entiere a mi-portee, presque nulle a la limite, absente au-dela (pas de pop)");
    }

    // --- Pose : aucun caillou ne flotte, a pleine echelle ET pendant le fondu --------
    {
        class WavySlope : public HeightSampler {
        public:
            float heightAt(float x, float z) const override {
                return 1500.0f + 0.72f * x + 0.10f * std::sin(x * 1.9f) * std::cos(z * 2.3f + 0.4f);
            }
        };
        WavySlope wavy;
        Rocks rocks(wavy, &bare, 1000.0f);
        const MeshData gravel = gravelMeshData();
        // Composantes (cailloux) : sommets relies par un triangle ou de meme position.
        const std::size_t nv = gravel.vertices.size();
        std::vector<std::size_t> part(nv);
        for (std::size_t i = 0; i < nv; ++i) { part[i] = i; }
        const auto root = [&](std::size_t i) {
            while (part[i] != i) { i = part[i] = part[part[i]]; }
            return i;
        };
        for (std::size_t t = 0; t + 2 < gravel.indices.size(); t += 3) {
            part[root(gravel.indices[t + 1])] = root(gravel.indices[t]);
            part[root(gravel.indices[t + 2])] = root(gravel.indices[t]);
        }
        for (std::size_t i = 0; i < nv; ++i) {
            for (std::size_t j = i + 1; j < nv; ++j) {
                if (gravel.vertices[i].pos == gravel.vertices[j].pos) { part[root(j)] = root(i); }
            }
        }
        for (std::size_t i = 0; i < nv; ++i) { part[i] = root(i); }
        uint32_t nearPlates = 0, floating = 0, checks = 0;
        for (int32_t cz = -6; cz <= 6; ++cz) {
            for (int32_t cx = -6; cx <= 6; ++cx) {
                for (const RockInstance& r : rocks.gravelCellRocks(cx, cz)) {
                    if (!r.nearSlot) { continue; }
                    ++nearPlates;
                    const glm::vec2 g = unpackRockGradient(r.variation);
                    const glm::vec3 nrm = glm::normalize(glm::vec3(-g.x, 1.0f, -g.y));
                    const glm::vec3 axis(nrm.z, 0.0f, -nrm.x);
                    const float sine = glm::length(axis);
                    glm::mat3 tilt(1.0f);
                    if (sine > 1e-4f) {
                        const glm::vec3 k = axis / sine;
                        const glm::mat3 K(0.0f, k.z, -k.y, -k.z, 0.0f, k.x, k.y, -k.x, 0.0f);
                        tilt = glm::mat3(nrm.y) + sine * K + (1.0f - nrm.y) * glm::outerProduct(k, k);
                    }
                    const float c = std::cos(r.yaw), sn = std::sin(r.yaw);
                    for (const float fade : { 1.0f, 0.6f, 0.3f }) {
                        const glm::vec3 base = r.position - glm::vec3(0.0f, rockFadeSink(r.kind, r.scale, fade), 0.0f);
                        // Chaque CAILLOU doit avoir son point bas sous le sol.
                        std::vector<float> lo(nv, 1e9f);
                        for (std::size_t i = 0; i < nv; ++i) {
                            const glm::vec3 l = gravel.vertices[i].pos * (r.scale * fade);
                            const glm::vec3 w = base + tilt * glm::vec3(c * l.x + sn * l.z, l.y, -sn * l.x + c * l.z);
                            lo[part[i]] = std::min(lo[part[i]], w.y - wavy.heightAt(w.x, w.z));
                        }
                        for (std::size_t i = 0; i < nv; ++i) {
                            if (part[i] != i) { continue; }
                            ++checks;
                            floating += lo[i] > 0.0f ? 1u : 0u;
                        }
                    }
                }
            }
        }
        std::printf("  (pose : %u plaques de proximite, %u controles, %u flottantes)\n", nearPlates, checks, floating);
        check(nearPlates > 0 && floating == 0, "Pose : aucune plaque de proximite au-dessus du sol (pleine echelle et fondu)");
    }

    // --- Trainees : la densite REELLE (proximite comprise) suit le bruit anisotrope --
    {
        Rocks rocks(smooth, &bare, 1000.0f);
        constexpr int N = 40; // 40 x 40 cellules de 8 m = 320 m
        std::vector<float> count(N * N, 0.0f), streak(N * N, 0.0f);
        for (int j = 0; j < N; ++j) {
            for (int i = 0; i < N; ++i) {
                const int32_t cx = i - N / 2, cz = j - N / 2;
                count[j * N + i] = static_cast<float>(rocks.gravelCellRocks(cx, cz).size());
                streak[j * N + i] = rocks.streakDensityAt((cx + 0.5f) * defaults.gravelCell, (cz + 0.5f) * defaults.gravelCell);
            }
        }
        double along = 0.0, across = 0.0, mc = 0.0, ms = 0.0;
        for (int j = 0; j < N; ++j) {
            for (int i = 0; i + 1 < N; ++i) {
                along += std::abs(count[j * N + i + 1] - count[j * N + i]);   // le long de la pente (x)
                across += std::abs(count[(i + 1) * N + j] - count[i * N + j]); // en travers (z)
            }
        }
        for (int k = 0; k < N * N; ++k) { mc += count[k]; ms += streak[k]; }
        mc /= N * N; ms /= N * N;
        double cov = 0.0, vc = 0.0, vs = 0.0;
        for (int k = 0; k < N * N; ++k) {
            cov += (count[k] - mc) * (streak[k] - ms);
            vc += (count[k] - mc) * (count[k] - mc);
            vs += (streak[k] - ms) * (streak[k] - ms);
        }
        const double corr = cov / std::sqrt(std::max(vc * vs, 1e-12));
        // CONTRASTE : la correlation et l'anisotropie ne voient pas une densite
        // ajoutee UNIFORMEMENT (une constante ne change ni l'une ni l'autre ; temoin
        // negatif fait, build-phase31\agent2\journaux). On exige donc que les zones
        // de trainee faible restent nettement plus clairsemees que les fortes, et
        // qu'AUCUNE instance ne soit la ou la trainee est nulle.
        double lowSum = 0.0, highSum = 0.0;
        uint32_t lowN = 0, highN = 0, onZero = 0, all = 0;
        for (int k = 0; k < N * N; ++k) {
            if (streak[k] <= 0.25f) { lowSum += count[k]; ++lowN; }
            if (streak[k] >= 0.75f) { highSum += count[k]; ++highN; }
        }
        for (int j = 0; j < N; ++j) {
            for (int i = 0; i < N; ++i) {
                for (const RockInstance& r : rocks.gravelCellRocks(i - N / 2, j - N / 2)) {
                    ++all;
                    onZero += rocks.streakDensityAt(r.position.x, r.position.z) <= 0.0f ? 1u : 0u;
                }
            }
        }
        const double contrast = (lowN && highN && lowSum > 0.0) ? (highSum / highN) / (lowSum / lowN) : 0.0;
        std::printf("  (densite reelle : variation le long %.0f, en travers %.0f ; correlation avec la trainee %.2f ; %.1f plaques/cellule ;"
                    " contraste fort/faible %.1f (%u/%u cellules) ; %u instances sur trainee nulle)\n",
                    along, across, corr, mc, contrast, highN, lowN, onZero);
        check(across > 1.5 * along, "Trainees : la densite REELLE (proximite comprise) reste allongee dans le sens de la pente");
        check(corr > 0.8, "Trainees : la densite reelle suit le bruit des trainees (pas un bruit blanc)");
        check(contrast >= 3.0, "Trainees : contraste de densite >= 3 entre trainee forte et faible (proximite comprise)");
        check(all > 0 && onZero == 0, "Trainees : aucune instance (proximite comprise) la ou la trainee est nulle");
    }
}

// ===========================================================================
// PHASE 29 (Agent 3) : buissons et petits arbres (classe 3 LiDAR)
// ===========================================================================

// F3 qui croit avec x : 0 a x = -400, 0,40 a x = +400 (le seuil 0,10 tombe a -200).
class LowVegRampX : public HeightSampler {
public:
    float heightAt(float x, float) const override {
        return glm::clamp((x + 400.0f) / 800.0f * 0.40f, 0.0f, 0.40f);
    }
};
// Couvert = F3 + F45, F45 croissant avec z : 0 a z = -400, 0,40 a z = +400
// (le seuil F45 0,20 tombe a z = 0).
class CoverRampXZ : public HeightSampler {
public:
    float heightAt(float x, float z) const override {
        return LowVegRampX().heightAt(x, z) + glm::clamp((z + 400.0f) / 800.0f * 0.40f, 0.0f, 0.40f);
    }
};

void testBushMeshes() {
    section("Phase 29 (Agent 3) : meshes de buisson et de petit arbre");
    const MeshData meshes[2] = { shrubMeshData(), smallTreeMeshData() };
    const char* names[2] = { "buisson", "petit arbre" };
    const float radii[2] = { kShrubFootprintRadius, kSmallTreeFootprintRadius };
    for (int m = 0; m < 2; ++m) {
        const MeshData& mesh = meshes[m];
        bool ok = !mesh.empty() && mesh.indices.size() % 3 == 0;
        glm::vec3 lo(1e9f), hi(-1e9f);
        float radial = 0.0f;
        for (uint32_t index : mesh.indices) { ok = ok && index < mesh.vertices.size(); }
        for (const Vertex& v : mesh.vertices) {
            ok = ok && std::isfinite(v.pos.x) && std::isfinite(v.pos.y) && std::isfinite(v.pos.z);
            ok = ok && std::abs(glm::length(v.normal) - 1.0f) < 1e-3f;
            lo = glm::min(lo, v.pos);
            hi = glm::max(hi, v.pos);
            radial = std::max(radial, std::sqrt(v.pos.x * v.pos.x + v.pos.z * v.pos.z));
        }
        std::printf("  (%s : %u triangles, y %.3f..%.3f, rayon %.2f)\n", names[m],
                    mesh.triangleCount(), lo.y, hi.y, radial);
        check(ok, std::string("Mesh ") + names[m] + " : indices, normales et positions valides");
        check(mesh.triangleCount() >= 16 && mesh.triangleCount() <= 400,
              std::string("Mesh ") + names[m] + " : bas-poly (16 a 400 triangles)");
        checkNear(hi.y, 1.0f, 1e-3f, std::string("Mesh ") + names[m] + " : hauteur unite (echelle = hauteur)");
        check(lo.y < 0.0f && lo.y >= -kBushBuriedDepth - 1e-4f,
              std::string("Mesh ") + names[m] + " : pied legerement enterre, pas davantage");
        check(radial <= radii[m] + 1e-3f,
              std::string("Mesh ") + names[m] + " : emprise dans le rayon annonce");
    }
    check(meshes[0].triangleCount() != meshes[1].triangleCount(), "Deux variantes distinctes");
}

void testBushPlacement() {
    section("Phase 29 (Agent 3) : semis des buissons (critere classe 3, determinisme, bornes)");
    const BushParams P;
    FlatTerrain flat(1500.0f);
    ConstantMap h25(2.5f), h0(0.0f), h10(10.0f), h12(1.2f);

    // --- Critere, isole -------------------------------------------------------
    {
        ConstantMap f20(0.20f), f05(0.05f), c30(0.30f), c45(0.45f), c08(0.08f);
        Bushes yes(flat, &f20, &h25, &c30, 1000.0f), lowF3(flat, &f05, &h25, &c30, 1000.0f),
               wooded(flat, &f20, &h25, &c45, 1000.0f), noF3(flat, nullptr, &h25, &c30, 1000.0f),
               noH(flat, &f20, nullptr, &c30, 1000.0f), noCover(flat, &f20, &h25, nullptr, 1000.0f),
               rockish(flat, &f20, &h25, &c08, 1000.0f);
        check(yes.maskAt(10.0f, 10.0f), "Critere : F3 0,20 et F45 0,10 -> buisson");
        check(!lowF3.maskAt(10.0f, 10.0f), "Critere : F3 0,05 (< 0,10) -> non");
        check(!wooded.maskAt(10.0f, 10.0f), "Critere : F45 0,25 (>= 0,20, foret) -> non");
        check(!rockish.maskAt(10.0f, 10.0f), "Critere : couvert 0,08 < 0,10 (domaine des rochers) -> non");
        check(!noF3.enabled() && !noH.enabled() && !noCover.enabled() &&
                  noF3.cellBushes(0, 0).empty() && noH.cellBushes(0, 0).empty() &&
                  noCover.cellBushes(0, 0).empty(),
              "Sans l'une des trois cartes : AUCUN buisson (pas de substitut)");
        check(!yes.maskAt(995.0f, 0.0f), "Critere : bord du terrain exclu");
        Bushes zeroH(flat, &f20, &h0, &c30, 1000.0f);
        bool none = true;
        for (int32_t c = -5; c <= 5; ++c) { none = none && zeroH.cellBushes(c, c).empty(); }
        check(none, "Hauteur classe 3 nulle -> aucun sujet (pas de hauteur inventee)");
    }

    // --- Aucun buisson hors critere, sur un champ qui traverse les seuils ----
    LowVegRampX f3;
    CoverRampXZ cover;
    Bushes field(flat, &f3, &h25, &cover, 1000.0f);
    std::vector<BushInstance> all;
    {
        bool inside = true;
        std::size_t perCellMax = 0;
        for (int32_t cz = -60; cz <= 60; ++cz) {
            for (int32_t cx = -60; cx <= 60; ++cx) {
                const auto cell = field.cellBushes(cx, cz);
                perCellMax = std::max(perCellMax, cell.size());
                for (const BushInstance& b : cell) {
                    const float vf3 = f3.heightAt(b.position.x, b.position.z);
                    const float vc = cover.heightAt(b.position.x, b.position.z);
                    inside = inside && vf3 >= P.lowVegMin && vc - vf3 < P.highVegMax && vc >= P.rockCoverMax;
                    all.push_back(b);
                }
            }
        }
        std::printf("  (%zu buissons semes sur 968 m x 968 m de champ en rampe)\n", all.size());
        check(all.size() > 500, "Le champ en rampe porte bien des buissons");
        check(inside, "AUCUN buisson hors critere (F3 >= 0,10, F45 < 0,20, couvert >= 0,10)");
        check(perCellMax <= P.slots, "Densite bornee : au plus `slots` sujets par cellule de 8 m");
        bool xOk = true, zOk = true;
        for (const BushInstance& b : all) {
            xOk = xOk && b.position.x >= -200.5f;
            zOk = zOk && b.position.z < 0.5f;
        }
        check(xOk && zOk, "Frontieres du champ reproduites (x >= -200, z < 0)");
    }

    // --- Densite lineaire en F3 ------------------------------------------------
    {
        ConstantMap c35(0.35f);
        ConstantMap f11(0.11f), f30(0.30f), cs11(0.21f);
        Bushes sparse(flat, &f11, &h25, &cs11, 1000.0f), dense(flat, &f30, &h25, &c35, 1000.0f);
        std::size_t ns = 0, nd = 0;
        for (int32_t cz = -20; cz < 20; ++cz) {
            for (int32_t cx = -20; cx < 20; ++cx) {
                ns += sparse.cellBushes(cx, cz).size();
                nd += dense.cellBushes(cx, cz).size();
            }
        }
        const double cells = 1600.0;
        std::printf("  (densite : %.2f / cellule a F3 0,11 ; %.2f a F3 0,30)\n", ns / cells, nd / cells);
        check(nd == static_cast<std::size_t>(cells) * P.slots, "F3 >= 0,30 : densite pleine, exactement `slots` par cellule");
        check(ns > 0 && ns * 2 < nd, "F3 0,11 : clairseme (moins de la moitie de la densite pleine)");
        const double perHa = nd / (cells * P.cell * P.cell / 10000.0);
        check(perHa <= 940.0, "Densite plafonnee a ~940 sujets / ha");
    }

    // --- Hauteur bornee et tiree sous la carte ---------------------------------
    {
        ConstantMap f30(0.30f), c35(0.35f);
        Bushes tall(flat, &f30, &h10, &c35, 1000.0f), low(flat, &f30, &h12, &c35, 1000.0f);
        bool tallOk = true, lowOk = true, kindOk = true;
        std::size_t smallTrees = 0, shrubs = 0;
        for (int32_t c = -30; c < 30; ++c) {
            for (const BushInstance& b : tall.cellBushes(c, -c)) {
                tallOk = tallOk && b.height >= P.heightMin && b.height <= P.heightMax;
                kindOk = kindOk && ((b.kind == BushKind::SmallTree) == (b.height >= P.smallTreeFrom));
                (b.kind == BushKind::SmallTree ? smallTrees : shrubs) += 1;
            }
            for (const BushInstance& b : low.cellBushes(c, c)) {
                lowOk = lowOk && b.height >= P.heightMin && b.height <= 1.2f + 1e-4f &&
                        b.kind == BushKind::Shrub;
            }
        }
        check(tallOk, "Hauteur bornee a [0,5 ; 3] m meme sous 10 m de classe 3");
        check(lowOk, "Hauteur <= hauteur max classe 3 de la cellule (1,2 m) -> buissons seulement");
        check(kindOk && smallTrees > 0, "Petit arbre si et seulement si hauteur >= 1,8 m");
        std::printf("  (sous 10 m : %zu petits arbres, %zu buissons)\n", smallTrees, shrubs);
        bool sitOk = true;
        for (const BushInstance& b : all) { sitOk = sitOk && b.position.y == 1500.0f; }
        check(sitOk, "Pied pose sur le terrain");
    }

    // --- Determinisme -----------------------------------------------------------
    {
        Bushes again(flat, &f3, &h25, &cover, 1000.0f);
        BushParams other;
        other.seed = 7u;
        Bushes reseeded(flat, &f3, &h25, &cover, 1000.0f, other);
        bool same = true, differs = false;
        for (int32_t cz = -30; cz <= 30; ++cz) {
            for (int32_t cx = -10; cx <= 40; ++cx) {
                const auto a = field.cellBushes(cx, cz);
                const auto b = again.cellBushes(cx, cz);
                const auto r = reseeded.cellBushes(cx, cz);
                same = same && a.size() == b.size();
                for (std::size_t i = 0; same && i < a.size(); ++i) {
                    same = std::memcmp(&a[i].position, &b[i].position, sizeof(glm::vec3)) == 0 &&
                           a[i].height == b[i].height && a[i].yaw == b[i].yaw && a[i].tint == b[i].tint &&
                           a[i].kind == b[i].kind && a[i].maxDistance == b[i].maxDistance;
                }
                if (!a.empty() && (r.empty() || r[0].position != a[0].position)) { differs = true; }
            }
        }
        check(same, "Semis deterministe au bit (deux instances, memes cellules)");
        check(differs, "La graine change le semis");
    }

    // --- Disjoint des rochers ---------------------------------------------------
    {
        // Pente de pierrier (0,72) et couvert en rampe qui traverse 0,10 :
        // aucun buisson la ou Rocks seme.
        SlopedTerrain scree(1500.0f, 0.72f);
        Bushes onScree(scree, &f3, &h25, &cover, 1000.0f);
        Rocks rocks(scree, &cover, 1000.0f);
        std::size_t n = 0;
        bool disjoint = true, rockZone = false;
        for (int32_t cz = -50; cz <= 50; cz += 2) {
            for (int32_t cx = -50; cx <= 50; cx += 2) {
                for (const BushInstance& b : onScree.cellBushes(cx, cz)) {
                    ++n;
                    disjoint = disjoint && !rocks.maskAt(b.position.x, b.position.z);
                }
                rockZone = rockZone || rocks.maskAt(cx * 8.0f + 4.0f, cz * 8.0f + 4.0f);
            }
        }
        check(n > 0 && rockZone, "Test des rochers significatif (buissons ET pierrier presents)");
        check(disjoint, "Aucun buisson sous le masque des rochers (disjoints par construction)");
    }

    // --- Emprise des arbres existants --------------------------------------------
    {
        ConstantMap f30(0.30f), c35(0.35f);
        Bushes open(flat, &f30, &h25, &c35, 1000.0f), guarded(flat, &f30, &h25, &c35, 1000.0f);
        guarded.setTreeFootprints({ glm::vec3(20.0f, 20.0f, 6.0f), glm::vec3(-100.0f, 40.0f, 4.0f) });
        std::size_t before = 0, inside = 0;
        for (int32_t cz = -3; cz <= 8; ++cz) {
            for (int32_t cx = -16; cx <= 4; ++cx) {
                for (const BushInstance& b : open.cellBushes(cx, cz)) {
                    before += glm::length(glm::vec2(b.position.x - 20.0f, b.position.z - 20.0f)) < 6.0f ? 1 : 0;
                }
                for (const BushInstance& b : guarded.cellBushes(cx, cz)) {
                    inside += glm::length(glm::vec2(b.position.x - 20.0f, b.position.z - 20.0f)) < 6.0f ? 1 : 0;
                    inside += glm::length(glm::vec2(b.position.x + 100.0f, b.position.z - 40.0f)) < 4.0f ? 1 : 0;
                }
            }
        }
        check(before > 0 && inside == 0, "Aucun buisson dans l'emprise d'un arbre abattable");
    }

    // --- gather : LOD, portee, fondu -----------------------------------------------
    {
        ConstantMap f30(0.30f), c35(0.35f);
        Bushes lod(flat, &f30, &h25, &c35, 2000.0f);
        std::vector<InstanceData> shrubs, trees;
        const glm::vec3 eye(0.0f, 1502.0f, 0.0f);
        for (int i = 0; i < 200; ++i) { lod.gather(eye, permissiveFrustum(), shrubs, trees); }
        bool inRange = true;
        for (const auto* list : { &shrubs, &trees }) {
            const float far = (list == &shrubs) ? P.shrubDistanceFar : P.treeDistanceFar;
            for (const InstanceData& d : *list) {
                const float dist = glm::length(d.position - eye);
                inRange = inRange && dist < far && d.fade >= 0.0f && d.fade <= 1.0f &&
                          d.scale >= P.heightMin && d.scale <= P.heightMax;
            }
        }
        std::size_t ringNear = 0, ringFar = 0;
        for (const InstanceData& d : shrubs) {
            const float dist = glm::length(glm::vec2(d.position.x, d.position.z));
            if (dist < 50.0f) { ++ringNear; }
            if (dist >= 100.0f && dist < 140.0f) { ++ringFar; }
        }
        const double densNear = ringNear / (3.14159 * 50.0 * 50.0);
        const double densFar = ringFar / (3.14159 * (140.0 * 140.0 - 100.0 * 100.0));
        std::printf("  (gather : %zu buissons, %zu petits arbres ; densite %.4f/m2 a < 50 m, %.4f a 100-140 m)\n",
                    shrubs.size(), trees.size(), densNear, densFar);
        check(!shrubs.empty() && !trees.empty(), "gather : deux listes instanciees remplies");
        check(inRange, "gather : portee, fondu et echelle bornes");
        check(densFar < 0.6 * densNear, "LOD de densite : eclaircissement avec la distance");
        check(lod.lastShrubCount() == shrubs.size() && lod.lastSmallTreeCount() == trees.size(),
              "Compteurs de diagnostic coherents");
    }
}

int main() {
    std::printf("=== Tests logiques Vercora (headless, sans Vulkan) ===\n");

    testExistingBehaviourStillWorks();
    testCollision();
    testPickup();
    testProgressiveFall();
    testStoneKnapping();
    testDrawListConstruction();
    testTerrainFile();
    testParaglider();
    testBike();
    testClimber();
    testWalkSlopeLimit();
    testTerrainQuadtree();
    testQuadtreeAncestry();
    testTextureFile();
    testGrassMesh();
    testGrassPhase18();
    testTileMeshingIsThreadSafe();
    testVegetationPlacement();
    testGrassPlacement();
    testInstanceLayout();
    testEditorAssets();
    testAssetCatalog();
    testTerrainRaycast();
    testEditorScene();
    testEditorSceneCollision();
    testEditorScenePick();
    testSceneFile();
    testEditorController();
    testNeeds();
    testWaterSources();
    testHabitatMemory();
    testAnimalMesh();
    testWildlife();
    testEnvironmentalFootprint();
    testWildlifeCollision();
    testMineralDeposits();
    testResin();
    testFiberPlants();
    testHarvestMeshes();
    testWeather();
    testWeatherGameplay();
    testWeatherVisuals();
    testDetailNoise();
    // PHASE 28
    testCraftingRecipeTable();
    testCraftingKnapUnchanged();
    testCraftingCord();
    testCraftingHearth();
    testCraftingAxe();
    testPitchNeedsLitHearth();
    testPutDown();
    // PHASE 28 (Agent 3) : effets branches
    testHearthWarmthEffect();
    testAxeYieldEffect();
    testShelterEffect();
    // PHASE 29 (Agent 2) : rochers et pierriers
    testRockMeshes();
    testRockPlacement();
    // PHASE 30 (Agent B) : rochers lisibles
    testRockReadability();
    // PHASE 31 (Agent 2) : densite pres du joueur
    testRockNearDensity();
    // PHASE 29 (Agent 3) : buissons et petits arbres
    testBushMeshes();
    testBushPlacement();

    std::printf("\n=====================================================\n");
    if (g_failures == 0) {
        std::printf("SUCCES : %d verifications passees.\n", g_checks);
        return 0;
    }
    std::printf("ECHEC : %d verifications sur %d ont echoue.\n", g_failures, g_checks);
    return 1;
}
