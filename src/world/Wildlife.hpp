#pragma once

#include "core/CollisionSolver.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace vercora {

class Habitat;
class HeightSampler;

// ===========================================================================
// PHASE 17 : la faune, et ce qu'elle rend visible.
// ===========================================================================
// Ce fichier n'a pas pour but de simuler des animaux. Il a pour but de rendre
// OBSERVABLE une consequence : la ou le joueur passe et repasse, il n'y a plus
// rien a voir. L'animal est le seul affichage de la memoire du milieu (voir
// world/Habitat.hpp) - il n'y a ni pop-up, ni compteur, ni message.
//
// D'ou un modele deliberement pauvre, et il faut le dire franchement :
//   - AUCUNE intelligence artificielle. Deux etats (paisible, en fuite), une
//     direction de fuite qui est la direction oppose au joueur, et c'est tout.
//     Pas de trajet, pas d'evitement d'obstacle, pas de troupeau.
//   - PHASE 20 : le joueur ne traverse PLUS un animal. Wildlife implemente
//     desormais CollisionSolver, exactement comme Forest et EditorScene, et
//     rejoint le CompositeCollisionSolver du monde. Ce qui reste faux et le
//     restera dans cette phase : un ANIMAL, lui, traverse toujours les troncs,
//     les rochers poses et ses congeneres. Ce n'est pas un oubli, c'est le
//     corollaire du "aucune IA" ci-dessus - une fuite en ligne droite qui
//     buterait sur un obstacle resterait COLLEE dessus, faute d'evitement, et
//     un animal encastre dans un tronc serait plus laid que le probleme
//     resolu. Le sens qui compte pour le joueur est celui qui est traite.
//   - AUCUNE persistance. Un animal qui s'eloigne assez disparait, et un autre
//     apparaitra ailleurs. Ce ne sont pas des individus qu'on suit, c'est une
//     POPULATION locale dont la densite depend de l'etat du milieu.
//
// C'est la meme economie que Vegetation : ce qui compte n'est pas l'objet, c'est
// la loi qui decide ou il y en a.
//
// LE SEMIS EST UN TIRAGE-REJET, comme celui de Forest : on tire un point au
// hasard autour du joueur et on le refuse s'il ne remplit pas TOUTES les
// conditions (dans l'emprise, pas trop pentu, assez boise selon la carte de
// couvert LiDAR, milieu pas trop derange, pas colle a un autre animal). Le
// nombre d'animaux reellement presents est donc une CONSEQUENCE du terrain et
// de l'etat de l'habitat, jamais une consigne.
//
// Ne connait RIEN de Vulkan : produit des positions et des matrices, c'est
// main.cpp qui en fait des DrawItem. Entierement testable sans GPU.
// ===========================================================================

struct WildlifeParams {
    // Population maximale simultanee AUTOUR DU JOUEUR. 24 : assez pour qu'une
    // zone calme paraisse habitee, assez peu pour que la difference avec une
    // zone derangee se compte a l'oeil.
    uint32_t maxAnimals = 24;

    // Anneau d'apparition autour du joueur, en m. Le minimum est le point qui
    // compte : un animal ne doit jamais apparaitre dans le champ PROCHE, sans
    // quoi on verrait la mecanique au lieu du monde.
    //
    // 55 m, CORRIGE en cours de session apres mesure sous Vulkan (scenario I).
    // Il valait 90 m, ce qui semblait prudent. Deux consequences, toutes deux
    // mauvaises, et aucune n'etait visible sans faire tourner le jeu :
    //   1. le joueur ne voyait jamais un animal a moins de 90 m, c'est-a-dire
    //      jamais un animal LISIBLE - un cervide de 1,3 m a cette distance est
    //      un point. La faune existait sans qu'on puisse la regarder.
    //   2. surtout, l'anneau d'apparition ne recouvrait presque pas le rayon
    //      derange (HabitatParams::noiseRadius, 120 m) : ce qui apparaissait
    //      apparaissait la ou le derangement est le plus faible, donc la zone
    //      autour du joueur se repeuplait indefiniment malgre sa presence. Le
    //      run le disait : un animal a portee de vue au debut, un a la fin,
    //      apres quatre minutes de presence continue.
    // 55 m reste au-dessus de fleeRadius (30 m) - on n'apparait pas sous le nez
    // du joueur - et bien en dessous du rayon derange, ce qui est la condition
    // pour que "la zone se vide" veuille dire quelque chose.
    float spawnRadiusMin = 55.0f;
    float spawnRadiusMax = 260.0f;

    // Au-dela, l'animal est oublie. Franchement plus grand que spawnRadiusMax :
    // un animal qui fuit doit pouvoir sortir de l'anneau sans disparaitre sous
    // les yeux du joueur.
    float despawnRadius = 420.0f;

    // Distance a laquelle l'approche du joueur declenche la fuite, en m. 30 m :
    // c'est la distance de fuite d'un chevreuil vraiment peu farouche, et c'est
    // surtout la distance a laquelle un animal de 1,3 m reste identifiable a
    // l'ecran. Plus court et on ne verrait jamais partir que des points.
    float fleeRadius = 30.0f;

    // Vitesse de fuite en m/s. 8 m/s = 29 km/h, soit environ trois fois la
    // vitesse de marche du joueur : la fuite se lit comme un depart net, et
    // poursuivre un animal ne mene nulle part - ce qui est le propos.
    float fleeSpeed = 8.0f;

    // Duree d'une pointe de fuite, en s. Au terme, l'animal se calme s'il est
    // assez loin, sinon il repart pour un tour.
    float fleeDuration = 5.0f;

    // Distance au joueur au-dela de laquelle un animal en fuite se calme.
    float fleeSafeDistance = 70.0f;

    // Deplacement paisible, en m/s. 0,5 : un animal qui broute avance
    // reellement, mais assez peu pour qu'on ne le prenne pas pour un patineur.
    float wanderSpeed = 0.5f;

    // Changement de cap paisible, en degres par seconde (amplitude du tirage).
    float wanderTurn = 25.0f;

    // --- Filtres de placement ----------------------------------------------
    // Couvert LiDAR minimal (meme grandeur que VegetationParams::coverMin) sous
    // lequel aucun animal n'est place. 0,25 : un peu plus permissif que le
    // semis d'arbres (0,20) parce qu'on cherche du BOIS, pas de la lande.
    float minCover = 0.25f;

    // Pente maximale (en tangente) acceptee. Un ongule de montagne tient sur du
    // raide, mais pas sur une paroi.
    //
    // PHASE 24 : 0,9 -> 0,80. La valeur n'a jamais eu de justification propre ;
    // ce qu'elle porte, c'est un RAPPORT, verifie par une assertion des tests
    // depuis la Phase 20 - aucun animal ne se tient sur une pente grimpable,
    // sans quoi on rencontrerait des cervides en pleine paroi. L'escalade
    // s'engage desormais a 0,84 au lieu de 1,00 (voir ClimberParams::wallSlope
    // et core/SlopeLimits.hpp), donc ce plafond devait descendre avec elle.
    //
    // L'effet reel sur le jeu est proche de nul, et c'est mesurable plutot que
    // suppose : les animaux sont semes sur les zones BOISEES, or ForestParams
    // ::maxSlope vaut 0,6 - aucun animal n'est jamais place au-dela. Le seuil
    // ne borne que l'errance a partir du point de depart.
    float maxSlope = 0.80f;

    // Distance minimale entre deux animaux, en m. Evite les tas.
    float minSpacing = 14.0f;

    // --- Collision (PHASE 20) ----------------------------------------------
    // Rayon du cylindre vertical qui represente l'animal face au joueur.
    //
    // 0,65 m = la DEMI-LONGUEUR du cervide (kAnimalLength / 2), c'est-a-dire le
    // cylindre CONTENANT sa boite englobante - meme convention que l'empreinte
    // des objets d'editeur (Phase 15), et pour la meme raison : un collider
    // trop petit se traverse par les coins, ce qui est exactement le defaut
    // qu'on vient corriger. Contrepartie assumee : de face, l'animal ne fait
    // que 0,42 m de large et son cylindre est donc trop genereux d'une
    // vingtaine de centimetres de chaque cote. Un cylindre ne peut pas etre
    // juste dans les deux sens, et l'erreur qui bloque un peu tot est moins
    // choquante que celle qui laisse passer au travers d'un flanc.
    //
    // Mis a l'echelle par Animal::scale, comme le mesh.
    float colliderRadius = 0.65f;

    // Un animal bloque-t-il le joueur ? Faux = comportement d'AVANT la Phase 20
    // (le joueur traverse la faune). C'est le TEMOIN de la mesure, au meme
    // titre que --no-scene pour l'editeur : sans lui, aucune mesure ne peut
    // montrer ce que la collision change, puisqu'on ne pourrait comparer qu'a
    // un autre binaire. Expose en ligne de commande par --no-animal-collision.
    bool solid = true;

    // Couvert suppose quand AUCUNE carte LiDAR n'est fournie. 0,5, donc
    // au-dessus de minCover : un terrain sans carte accepte la faune partout,
    // ce qui est le comportement voulu pour le terrain synthetique et pour les
    // tests. Ce n'est PAS un substitut par pente et altitude comme celui de
    // Vegetation - il n'y a rien a imiter ici, un animal n'a pas de sylviculture.
    float coverWithoutMap = 0.5f;

    // --- Cadence -----------------------------------------------------------
    // Intervalle entre deux vagues d'apparition, en s, et nombre de tirages par
    // vague. Une vague par seconde avec 6 tirages : le repeuplement d'une zone
    // calme prend une poignee de secondes, ce qui est assez lent pour se voir et
    // assez rapide pour tenir dans une session de test.
    float spawnInterval = 1.0f;
    uint32_t spawnAttempts = 6;

    // Deux Wildlife de meme graine sont identiques : les tests sont
    // reproductibles, comme pour Forest et Vegetation.
    uint32_t seed = 20260825u;
};

// Deux etats, pas trois : ce que le joueur doit pouvoir lire est "il est la" ou
// "il s'en va".
enum class AnimalState {
    Calm,    // broute, se deplace lentement
    Fleeing, // s'eloigne du joueur en ligne droite
};

struct Animal {
    // Sabots au sol : y = terrain.heightAt(x, z).
    glm::vec3 position{0.0f};

    // Cap en degres, convention du velo (0 = +X, croissant vers +Z).
    float headingDegrees = 0.0f;

    // Echelle UNIFORME du mesh (voir world/AnimalMesh.hpp). Autour de 1.
    float scale = 1.0f;

    AnimalState state = AnimalState::Calm;

    // Temps restant de la pointe de fuite en cours, en s.
    float fleeTimer = 0.0f;

    // Identite stable tant que l'animal vit. Sert aux tests et au journal : sans
    // elle, "il y a toujours douze animaux" ne dit pas si ce sont les memes.
    uint32_t id = 0;

    // translate(pieds) * rotateY(-cap) * scale(echelle). Voir la convention
    // detaillee dans world/AnimalMesh.hpp.
    glm::mat4 modelMatrix() const;
};

// Implemente CollisionSolver depuis la Phase 20 : la faune est la seule a
// savoir ou sont ses animaux, c'est donc a elle de dire au joueur qu'il ne peut
// pas passer au travers - meme partage des roles que Forest pour ses troncs et
// EditorScene pour ses objets poses. Ni Camera, ni Bike, ni Climber n'ont a
// changer : ils voient toujours l'unique CompositeCollisionSolver du monde.
class Wildlife : public CollisionSolver {
public:
    // `terrain` donne l'altitude et la pente, `cover` est la carte de couvert
    // LiDAR produite par tools/lidar_to_canopy.py - LA MEME que celle qui sert
    // au semis d'arbres de Vegetation, echantillonnee en coordonnees monde.
    // Elle peut etre nulle (voir WildlifeParams::coverWithoutMap). Aucun des
    // deux n'est possede : tous deux doivent vivre plus longtemps.
    // `terrainExtent` = demi-cote du terrain, pour ne rien placer hors donnees.
    Wildlife(const HeightSampler& terrain, const HeightSampler* cover,
             float terrainExtent, const WildlifeParams& params = {});

    // Fait vivre la population : disparitions, fuites, deplacements paisibles,
    // puis apparitions. L'`habitat` est consulte dans les deux sens - il decide
    // qui part (wildlifeFlees) et ou l'on peut reapparaitre (allowsWildlife) -
    // mais n'est jamais MODIFIE ici : la faune ne derange personne.
    void update(float deltaTime, const glm::vec3& playerPosition,
                const Habitat& habitat);

    const std::vector<Animal>& animals() const { return animals_; }
    std::size_t animalCount() const { return animals_.size(); }

    // Animaux presents a moins de `radius` du point donne. C'est LA grandeur
    // qui dit si une zone s'est videe, et le compte total ne la remplace pas :
    // la faune derangee s'ECARTE, elle ne s'evapore pas, donc la population
    // globale peut rester identique pendant qu'il n'y a plus rien autour du
    // joueur. Mesure sous Vulkan : 24 animaux au depart, 24 apres deux minutes
    // de presence - et zero a portee de vue.
    std::size_t countWithin(const glm::vec3& position, float radius) const;

    // Animaux actuellement en fuite. Expose pour le journal periodique et les
    // tests : c'est la grandeur qui dit que la reaction au joueur a eu lieu.
    std::size_t fleeingCount() const;

    // Nombre d'apparitions et de disparitions depuis le debut. Deux compteurs
    // qui montent en permanence, donc lisibles dans un journal une fois par
    // seconde : une population stable en nombre peut tres bien etre en train de
    // se renouveler entierement, et c'est ce qu'on veut pouvoir constater.
    uint32_t spawnCount() const { return spawnCount_; }
    uint32_t despawnCount() const { return despawnCount_; }

    // Force une tentative de peuplement immediate, sans attendre la cadence.
    // Utilisee au demarrage - sinon le joueur ouvre sur un monde vide qui se
    // remplit sous ses yeux - et par les tests, qui ne veulent pas simuler
    // vingt secondes pour observer un semis.
    void populate(const glm::vec3& playerPosition, const Habitat& habitat,
                  uint32_t waves = 8);

    const WildlifeParams& params() const { return params_; }

    // --- CollisionSolver (PHASE 20) ----------------------------------------
    glm::vec3 resolve(const BodyCylinder& body) const override;
    bool overlaps(const BodyCylinder& body) const override;

    // Rayon de collision effectif d'un animal donne (rayon nominal x echelle).
    // Expose parce que c'est la grandeur que les tests PREDISENT : un arret
    // annonce a `colliderRadius + bodyRadius` du centre n'est verifiable que si
    // l'appelant peut lire le meme nombre que le solveur.
    float colliderRadiusOf(const Animal& animal) const {
        return params_.colliderRadius * animal.scale;
    }

private:
    // Un point est-il un emplacement acceptable ? (emprise, pente, couvert,
    // etat du milieu, espacement)
    bool isPlaceable(float worldX, float worldZ, const Habitat& habitat) const;

    // Une vague de tirage-rejet.
    void spawnWave(const glm::vec3& playerPosition, const Habitat& habitat);

    // Pente du terrain, par differences finies. MEME definition que
    // Forest::slopeAt et Vegetation::slopeAt : les trois filtres doivent parler
    // de la meme grandeur.
    float slopeAt(float worldX, float worldZ) const;

    float coverAt(float worldX, float worldZ) const;

    const HeightSampler& terrain_;
    const HeightSampler* cover_ = nullptr;
    float terrainExtent_ = 0.0f;
    WildlifeParams params_;

    std::vector<Animal> animals_;
    float spawnAccumulator_ = 0.0f;
    uint32_t nextId_ = 1;
    uint32_t spawnCount_ = 0;
    uint32_t despawnCount_ = 0;

    std::mt19937 rng_;
};

} // namespace vercora
