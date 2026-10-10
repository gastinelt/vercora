#pragma once

#include "world/Clearing.hpp"
#include "world/GroundCover.hpp"
#include "world/Needs.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace vercora {

// ===========================================================================
// PHASE 17 : la MEMOIRE LOCALE du milieu.
// ===========================================================================
// Le jeu ne dit jamais au joueur qu'il derange. Il s'en SOUVIENT, par endroit,
// et ce souvenir change ce qui se passe la : la faune n'y revient pas tant que
// la zone reste frequentee, et un versant vide de ses arbres perd son couvert
// au sol. C'est tout le contenu de ce fichier - aucun message, aucun compteur
// affiche, aucune notification.
//
// DEUX MEMOIRES, PAS UNE. Elles ne se comportent pas pareil et les melanger
// aurait ete une erreur de modele :
//
//   frequentation (disturbance) - monte avec la PRESENCE du joueur, ponderee
//     par le bruit de son activite, et redescend en quelques minutes. C'est un
//     derangement : il cesse quand on s'en va.
//
//   deforestation (logging)     - monte par MARCHE, un cran par arbre abattu,
//     et redescend dix fois plus lentement. Ce n'est pas un derangement, c'est
//     un prelevement : la zone met du temps a s'en remettre, et pendant ce
//     temps elle est visiblement plus nue.
//
// UNE GRILLE CREUSE, PAS UN TABLEAU. La zone fait 25 km2, soit 24 000 cellules
// de 32 m. Les allouer toutes serait possible mais faux d'esprit : le joueur en
// touche quelques dizaines par partie, et une cellule revenue a zero est
// oubliee (voir update). C'est le meme raisonnement que le semis a la demande
// de Vegetation - ce qui n'a pas ete visite n'existe pas.
//
// Ne connait ni Vulkan, ni le terrain, ni la faune : une position, un temps,
// des compteurs. Entierement testable sans GPU.
// ===========================================================================

struct HabitatParams {
    // Cote d'une cellule de memoire, en m. 32 m : de l'ordre de la cellule de
    // semis d'arbres (24 m), donc une zone degradee contient assez d'arbres
    // pour que leur disparition se voie, et assez petite pour qu'on puisse
    // "sortir" du derangement en marchant une trentaine de secondes.
    float cellSize = 32.0f;

    // Rayon, en m, sur lequel la presence du joueur derange. Le derangement
    // deborde donc toujours sur les cellules voisines, avec une ponderation qui
    // decroit (voir notePresence). Sans ce debordement, la memoire aurait des
    // bords carres de 32 m parfaitement lisibles a l'usage - on apprendrait la
    // grille, pas le milieu.
    //
    // 120 m, et le chiffre a ete CORRIGE en cours de session apres une mesure
    // sous Vulkan. Il valait 45 m, ce qui paraissait raisonnable - un peu plus
    // d'une cellule. Le scenario I a montre que ca ne pouvait pas marcher : la
    // faune n'apparait jamais a moins de WildlifeParams::spawnRadiusMin, soit
    // 90 m, donc AUCUN animal ne se trouvait jamais dans le rayon derange. Un
    // joueur pouvait rester deux minutes au meme endroit, sa cellule saturait a
    // 100 % - et il ne se passait rien. Releve du run : 24 animaux au depart,
    // 24 a la fin, zero fuite.
    //
    // La lecon est generale et vaut d'etre notee : deux rayons regles chacun
    // pour de bonnes raisons, dans deux fichiers differents, peuvent se rater
    // completement. Il faut que noiseRadius depasse spawnRadiusMin, sans quoi la
    // deuxieme mecanique de la phase n'existe pas.
    float noiseRadius = 120.0f;

    // Montee de la frequentation, par seconde, dans la cellule ou se tient le
    // joueur et pour un bruit de 1 (la marche). 1/60 : une minute de marche sur
    // place sature la cellule, quatre a cinq minutes suffisent depuis un camp
    // installe a quarante metres. C'est court a l'echelle d'une partie et c'est
    // voulu - le systeme doit etre OBSERVABLE en jouant, pas au bout d'une
    // heure.
    float disturbanceRise = 1.0f / 60.0f;

    // Descente de la frequentation, par seconde, quand le joueur est parti.
    // 1/240 : quatre minutes pour tout oublier depuis la saturation, donc
    // quatre fois plus lent que la montee. L'asymetrie EST le propos : on
    // derange plus vite qu'on ne se fait oublier.
    float disturbanceDecay = 1.0f / 240.0f;

    // Cran de deforestation par arbre abattu, au pied de l'arbre. 0,34 : trois
    // arbres dans la meme cellule saturent. Le seuil n'est donc pas "couper du
    // bois", c'est "vider un bosquet" - couper un arbre isole pour son bois ne
    // declenche rien, et c'est exactement la nuance recherchee.
    float loggingPerTree = 0.34f;

    // Un abattage FAIT DU BRUIT : il ajoute aussi ce cran de frequentation.
    float loggingNoise = 0.25f;

    // Descente de la deforestation, par seconde. 1/900 : un quart d'heure de
    // jeu reel pour qu'une coupe saturee redevienne un sous-bois. Quinze fois
    // plus lent que la frequentation.
    float loggingDecay = 1.0f / 900.0f;

    // --- Seuils de faune ----------------------------------------------------
    // Deux seuils DISTINCTS, et c'est une hysteresis deliberee : les animaux
    // presents partent au-dessus de fleeThreshold, et ne reviennent que sous
    // respawnThreshold. Avec un seuil unique, une zone posee juste dessus
    // ferait clignoter la faune - apparition, fuite, apparition - a chaque
    // image.
    float fleeThreshold = 0.55f;
    float respawnThreshold = 0.35f;

    // Deforestation au-dela de laquelle plus aucune faune ne reapparait, quelle
    // que soit la frequentation. 0,40, soit un peu plus d'un arbre : une coupe
    // vide la zone plus surement qu'un passage bruyant, et pour plus longtemps
    // (voir loggingDecay).
    float loggingBlocksWildlife = 0.40f;

    // --- Couvert au sol -----------------------------------------------------
    // Bornes de la rampe de mise a nu : en dessous de la premiere, le sol est
    // intact ; au-dessus de la seconde, il est aussi nu qu'il peut l'etre.
    float barenessBegin = 0.25f;
    float barenessFull = 1.00f;

    // Ce qui SUBSISTE de couvert au sol au pire de la degradation. 0,15 et non
    // zero : une coupe rase n'est pas de la roche nue, il y reste des touffes.
    // Zero se lirait comme un bug d'affichage, pas comme un milieu abime.
    float minGroundCover = 0.15f;
};

// L'etat d'une cellule. Les deux compteurs vivent dans [0, 1] ; treesFelled est
// un compte brut, pour le journal et les tests (un compteur ecrete ne dit plus
// combien d'arbres sont tombes).
struct HabitatCell {
    float disturbance = 0.0f;
    float logging = 0.0f;
    uint32_t treesFelled = 0;

    // Le joueur derange cette cellule EN CE MOMENT. Pose par notePresence,
    // consomme et remis a zero par update, qui ne fait decroitre que les
    // cellules libres. C'est un detail d'implementation - le contrat reste
    // "notePresence puis update, une fois par image, dans cet ordre" - mais un
    // detail qui decide de tout : sans lui, montee et decroissance se
    // retranchent, et une presence a quarante metres ne s'accumule jamais.
    bool occupied = false;
};

// PHASE 22 : l'Habitat repond desormais a DEUX questions du semis de decor, par
// deux interfaces distinctes qu'il implemente toutes les deux. `groundCover`
// (Phase 17) rarefie l'herbe la ou le sol est mis a nu ; `clearing` (Phase 22)
// fait pousser les plantes a fibres la ou la foret a ete ouverte. Les deux se
// lisent aujourd'hui dans le meme compteur de deforestation, et le pourquoi de
// leur separation est dans world/Clearing.hpp.
class Habitat : public GroundCoverSource, public ClearingSource {
public:
    explicit Habitat(const HabitatParams& params = {});

    // --- Ecriture -----------------------------------------------------------
    // Presence du joueur pendant deltaTime, avec le bruit de son activite
    // (voir activityNoise). Touche la cellule du joueur ET ses voisines, avec
    // une ponderation qui decroit jusqu'a zero a noiseRadius.
    void notePresence(const glm::vec3& position, float deltaTime, float noise);

    // Un arbre vient d'etre abattu ici. Appele par le gameplay APRES un
    // Forest::fell reussi - Forest lui-meme ne connait pas l'Habitat, et c'est
    // volontaire : l'abattage est une mecanique de la Phase 5, l'empreinte
    // qu'il laisse est une lecture de la Phase 17.
    void recordFelling(const glm::vec3& position);

    // Decroissance des deux memoires et OUBLI des cellules revenues a zero.
    void update(float deltaTime);

    // --- Lecture ------------------------------------------------------------
    // Valeurs de la cellule contenant le point. Pas d'interpolation : ce sont
    // des grandeurs de DECISION (seuils de faune), et une decision doit etre
    // stable sur toute une cellule plutot que de dependre du demi-metre pres.
    float disturbanceAt(float worldX, float worldZ) const;
    float loggingAt(float worldX, float worldZ) const;
    uint32_t treesFelledAt(float worldX, float worldZ) const;

    // Couvert au sol, INTERPOLE bilineairement entre les centres de cellule.
    // Ici l'interpolation est indispensable : c'est une grandeur d'AFFICHAGE, et
    // une valeur constante par cellule dessinerait au sol un damier de 32 m de
    // cote, qui se lirait comme un defaut de rendu et non comme une clairiere.
    float groundCover(float worldX, float worldZ) const override;

    // Part de sol OUVERT, dans [0, 1]. C'est le compteur de deforestation de la
    // cellule, ecrete - donc sans interpolation, contrairement a groundCover.
    // Ici la constance par cellule est le bon choix : une recolonisation par les
    // ronces se fait par TACHES, et une tache de 32 m de cote est exactement ce
    // qu'on veut voir apres une coupe. C'est la meme distinction qu'entre
    // disturbanceAt (decision, constante) et groundCover (affichage, interpole).
    float clearing(float worldX, float worldZ) const override;

    // La faune peut-elle REAPPARAITRE ici ? (frequentation sous le seuil bas ET
    // deforestation sous le sien)
    bool allowsWildlife(float worldX, float worldZ) const;

    // La faune DEJA presente ici doit-elle partir ? (frequentation au-dessus du
    // seuil haut)
    bool wildlifeFlees(float worldX, float worldZ) const;

    // --- Diagnostic ---------------------------------------------------------
    std::size_t trackedCellCount() const { return cells_.size(); }
    const HabitatParams& params() const { return params_; }

    // Cellule la plus degradee des deux natures, pour le journal periodique.
    float peakDisturbance() const;
    float peakLogging() const;

private:
    struct CellKey {
        int32_t x = 0;
        int32_t z = 0;
        bool operator==(const CellKey& other) const { return x == other.x && z == other.z; }
    };
    struct CellKeyHash {
        std::size_t operator()(const CellKey& key) const {
            // Meme melange que Vegetation::CellKeyHash : les deux moities
            // occupent des bits distincts.
            return (static_cast<std::size_t>(static_cast<uint32_t>(key.x)) * 0x9E3779B1ull)
                 ^ (static_cast<std::size_t>(static_cast<uint32_t>(key.z)) * 0x85EBCA77ull);
        }
    };

    CellKey keyAt(float worldX, float worldZ) const;
    const HabitatCell* find(const CellKey& key) const;

    // Couvert au sol deduit du seul compteur de deforestation d'une cellule.
    float coverFromLogging(float logging) const;

    HabitatParams params_;
    std::unordered_map<CellKey, HabitatCell, CellKeyHash> cells_;
};

// Bruit relatif de chaque activite, la marche valant 1. C'est le pendant exact
// de activityEffort (world/Needs.hpp) : la meme liste d'activites, mais classee
// sur ce qu'elle DERANGE et non sur ce qu'elle coute au joueur. Les deux
// echelles different vraiment, et c'est le coeur du sujet :
//
//   repos      0,25  un homme immobile derange encore - il est la, il sent
//   marche     1,00  reference
//   velo       2,00  vite, mecanique, sur des lignes : ce qui derange le plus
//   escalade   0,60  lent et silencieux, mais sur une paroi, donc en evidence
//   parapente  0,15  silencieux et passager, il ne fait que traverser
//
// L'escalade est l'exemple qui montre que les deux echelles ne sont pas la
// meme : c'est l'effort MAXIMAL du joueur (4,0 chez Needs) et l'une des
// activites qui derange le MOINS.
//
// Prend l'Activity de world/Needs.hpp - la meme enumeration, pas une copie :
// deux listes d'activites finiraient par diverger le jour ou une cinquieme
// activite arrivera, et le compilateur ne dirait rien.
float activityNoise(Activity activity);

} // namespace vercora
