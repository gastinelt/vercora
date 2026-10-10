#pragma once

#include <glm/glm.hpp>

namespace vercora {

class HeightSampler;

// Modele de vol simplifie (Phase 8) : pas une simulation aerodynamique - pas de
// decrochage, pas de fermeture, pas de pilotage a la sellette - juste de quoi
// decoller, tourner, freiner et se poser de facon credible.
//
// PHASE 23 : "pas de vent, pas de thermiques" n'est plus vrai. L'air dans lequel
// l'aile vole est desormais decrit par AirState, que l'appelant remplit depuis
// world/Weather.hpp. Le reste du modele est INCHANGE - c'est un ajout au
// deplacement, pas une reecriture de la physique de vol.

// ---------------------------------------------------------------------------
// L'air dans lequel l'aile vole
// ---------------------------------------------------------------------------
// Volontairement une struct NUE et non un WeatherSample : Paraglider ne doit
// pas plus dependre du systeme meteo qu'il ne depend de Vulkan. Il decrit ce
// dont il a besoin, l'appelant le remplit. C'est la meme separation que
// HeightSampler, qui dit "une hauteur en fonction d'une position" sans savoir
// d'ou elle vient.
//
// Les valeurs par defaut sont l'AIR CALME, donc exactement le vol de la
// Phase 8 : un appelant qui ignore la meteo obtient le comportement d'avant.
struct AirState {
    // Vent horizontal, en repere monde, en m/s. Il POINTE dans le sens ou l'air
    // se deplace. Une aile derive avec la masse d'air : c'est l'effet le plus
    // important de tout ce bloc, et celui qui change le plus la facon de voler.
    glm::vec3 wind{0.0f};

    // Vitesse VERTICALE de la masse d'air, en m/s. Positive = ascendance
    // (thermique, ou vent qui remonte une pente). Negative = subsidence.
    float vertical = 0.0f;

    // Turbulence dans [0, 1]. 0 = air lisse.
    float turbulence = 0.0f;
};

struct ParagliderParams {
    // Vitesse de croisière en air calme, en unites monde/seconde (= m/s, meme
    // convention que le reste du jeu). ~38 km/h, au milieu de la fourchette
    // 35-40 km/h d'une aile-ecole typique.
    float cruiseSpeed = 10.5f;

    // Vitesse une fois les freins tires a fond. Un vrai parapente perd environ
    // un tiers de sa vitesse de croisiere aux "grandes oreilles"/plein frein.
    float brakeSpeed = 7.0f;

    // Taux de chute en air calme, vol stabilise : 1 a 1,5 m/s pour une aile
    // grand public. On prend le milieu de la fourchette.
    float cruiseSinkRate = 1.25f;

    // Le frein degrade legerement la finesse (on troque de la vitesse contre de
    // la trainee), donc un taux de chute un peu plus fort qu'en croisiere.
    float brakeSinkRate = 1.6f;

    // Vitesse angulaire maximale du cap, en degres/seconde. Avec la vitesse de
    // croisiere ci-dessus, ca donne un rayon de virage stabilise d'environ
    // vitesse / vitesseAngulaire(rad/s) ~ 10.5 / 0.44 ~ 24 m - un virage large et
    // progressif, pas une rotation sur place.
    float turnRateDegPerSec = 25.0f;

    // Constante de temps de la reponse vitesse/taux de chute au frein. Tirer un
    // frein ne change pas le vol instantanement : l'aile met une fraction de
    // seconde a se stabiliser sur son nouveau regime. Voir update().
    float responseTime = 0.5f;

    // --- PHASE 23 : reponse a l'air --------------------------------------
    //
    // VALEURS PROVISOIRES, au meme titre que celles de WeatherParams : elles
    // decrivent la reponse de l'aile a un air dont l'intensite n'est elle-meme
    // pas encore calibree. Elles vivent ici et non dans WeatherParams parce
    // qu'elles decrivent l'AILE, pas l'atmosphere.

    // Ecart de cap maximal, en degres par seconde, impose par une turbulence
    // maximale. 12 : l'aile est chahutee, elle ne part pas en vrille. Le modele
    // ne represente ni fermeture ni decrochage - c'est la limite assumee de
    // cette session, et c'est pour ca que le chiffre reste modeste.
    float turbulenceYawDegPerSec = 12.0f;

    // Degradation du taux de chute par une turbulence maximale, en m/s. 0,8 :
    // un air brasse coute de la finesse, ce qui est reel et se sent - un vol
    // sous le vent d'une crete ne va pas aussi loin qu'un vol en air lisse.
    float turbulenceSinkPenalty = 0.8f;

    // Periode du chahut turbulent, en secondes. 1,7 : l'echelle de temps d'une
    // rafale ressentie sous une aile. Une valeur NON ronde et sans rapport
    // simple avec les autres, pour que les deux perturbations (cap et chute) ne
    // se synchronisent pas en un balancement periodique visible.
    float turbulencePeriod = 1.7f;
};

// Position, cap et vitesse d'un vol en parapente. Ne connait rien de Vulkan ni
// de la Camera : elle produit une position et un cap, c'est l'appelant (voir
// main.cpp) qui en tire une pose de camera en chase-cam via
// Camera::setChasePose(). Meme separation que Forest vis-a-vis du rendu.
class Paraglider {
public:
    explicit Paraglider(const ParagliderParams& params = {});

    // Debute un vol a la position et au cap (degres, meme convention que
    // Camera : 0 = +X, 90 = +Z) donnes. La position DOIT deja etre au-dessus du
    // terrain - c'est a l'appelant de le verifier avant de decoller (main.cpp
    // exige une hauteur minimale au-dessus du sol).
    void launch(const glm::vec3& position, float headingDegrees);

    // Fait avancer la simulation d'un pas de temps. turnInput dans [-1, 1]
    // (virage gauche/droite), brakeInput dans [0, 1] (frein progressif). Les
    // deux sont ecretes en interne : l'appelant peut passer des valeurs brutes
    // de touches (0/1) sans y penser.
    // air decrit la masse d'air au point de l'aile (vent, ascendance,
    // turbulence). Sa valeur par defaut est l'air CALME : tous les appelants
    // ecrits avant la Phase 23 - dont les tests de la Phase 8 - continuent de
    // decrire exactement le meme vol.
    void update(float deltaTime, float turnInput, float brakeInput,
                const AirState& air = {});

    // Vitesse verticale REELLE du dernier pas, en m/s (positive = ca monte).
    // C'est le taux de chute de l'aile corrige de l'air : dans une bonne
    // thermique elle est positive, et c'est tout l'interet.
    //
    // Exposee parce qu'un vol qui gagne de l'altitude est la propriete que les
    // tests doivent verifier, et parce que c'est la seule grandeur qu'un vario
    // afficherait si le jeu en avait un.
    float climbRateMps() const { return climbRate_; }

    // Temps de vol ecoule depuis launch(), en secondes. Pilote le chahut
    // turbulent, et sert de repere au journal.
    float flightTime() const { return flightTime_; }

    // Termine le vol des que l'altitude rejoint le terrain sous le pilote (plus
    // eyeHeight, meme convention que Camera::eyeHeight_). Renvoie true la frame
    // ou l'atterrissage vient d'avoir lieu ; ajuste position().y pour qu'elle
    // colle exactement au sol, comme Camera::snapToTerrain.
    bool checkLanding(const HeightSampler& terrain, float eyeHeight);

    // Fin de vol immediate, quelle que soit l'altitude - filet de securite pour
    // interrompre un vol qui ne rencontrerait pas le relief (ou pour un abandon
    // volontaire). Ne touche pas a la position : c'est a l'appelant de la reposer
    // sur le terrain s'il le souhaite.
    void abort() { flying_ = false; }

    bool flying() const { return flying_; }
    const glm::vec3& position() const { return position_; }
    float headingDegrees() const { return heading_; }

    // Direction horizontale unitaire du cap courant.
    glm::vec3 forward() const;

private:
    ParagliderParams params_;

    glm::vec3 position_{0.0f};
    float heading_ = 0.0f; // degres, convention Camera : 0 = +X, 90 = +Z

    float speed_ = 0.0f;    // vitesse horizontale courante, m/s (AIR, pas sol)
    float sinkRate_ = 0.0f; // taux de chute courant, m/s

    float climbRate_ = 0.0f;  // vitesse verticale reelle du dernier pas
    float flightTime_ = 0.0f; // secondes depuis launch()

    bool flying_ = false;
};

} // namespace vercora
