#pragma once

#include <glm/glm.hpp>

namespace vercora {

class HeightSampler;
class CollisionSolver;

// Modele de VTT simplifie (Phase 13), dans le meme esprit que world/Paraglider :
// pas une simulation de dynamique du vehicule - pas de suspension, pas de
// transfert de charge, pas de rapport de vitesses, pas d'adherence variable -
// juste de quoi rouler, monter, descendre, tourner et freiner de facon credible.
//
// Le modele longitudinal tient en une somme d'accelerations, integree a chaque
// pas de temps (donc AUCUN changement instantane de vitesse) :
//
//   a = a_gravite(pente) + a_pedalage(vitesse) - a_frein - a_resistances(vitesse)
//
// Chaque terme est justifie et chiffre ci-dessous. Les vitesses d'equilibre qui
// en decoulent sont dans NOTES-PHASE-VELO.md et VERIFIEES par les tests
// headless (tests/logic_tests.cpp, section Phase 13) : c'est la que se lit le
// comportement reellement obtenu, pas seulement celui vise.
struct BikeParams {
    // --- Motricite -----------------------------------------------------------

    // Acceleration disponible au pedalage a l'arret, en m/s^2. 2,6 m/s^2 sur un
    // VTT charge (~95 kg avec le cycliste) correspond a ~250 W au demarrage,
    // c'est-a-dire un cycliste ordinaire en danseuse, pas un sprinter.
    //
    // C'est ce terme, confronte a la pesanteur, qui fixe la PENTE MAXIMALE
    // FRANCHISSABLE : le velo cale des que g*sin(theta) depasse
    // pedalAcceleration - rollingResistance, soit **23,6 % de pente**. Au-dela
    // il faut zigzaguer (le modele lit la pente LE LONG DU CAP, pas la pente
    // maximale : traverser en biais reduit reellement l'effort) ou mettre pied a
    // terre. C'est voulu - un sentier a 25 % se monte a pied - mais c'est le
    // parametre a revoir en premier si la zone de jeu se revele trop verrouillee.
    float pedalAcceleration = 2.6f;

    // Vitesse a laquelle le pedalage ne pousse plus du tout, en m/s. Le terme de
    // poussee est pedalAcceleration * (1 - v / pedalSpeedLimit) : c'est la facon
    // la plus simple de representer qu'a haute vitesse la cadence s'emballe et
    // que le cycliste ne developpe plus d'effort utile. 6,5 m/s = 23,4 km/h.
    //
    // Ce n'est PAS la vitesse de pointe sur le plat : les resistances ci-dessous
    // en retirent encore, et l'equilibre reel tombe vers 4,6 m/s (16,6 km/h).
    float pedalSpeedLimit = 6.5f;

    // --- Resistances ---------------------------------------------------------

    // Resistance au roulement, en m/s^2 (constante, independante de la vitesse).
    // 0,35 m/s^2 correspond a un coefficient de ~0,036 : un pneu large a basse
    // pression sur un chemin de terre, soit cinq a dix fois une roue de route
    // sur bitume. C'est ce terme qui empeche le velo de descendre tout seul une
    // pente tres faible.
    float rollingResistance = 0.35f;

    // Terme quadratique, en 1/m : la deceleration vaut dragCoefficient * v^2.
    //
    // ATTENTION, c'est un terme GROUPE et non la seule trainee aerodynamique.
    // L'aero pure d'un cycliste redresse vaut ~0,004 /m ; on prend 0,014, soit
    // trois fois plus, pour representer d'un seul coup ce que ce modele ne
    // simule pas : sol irregulier, pompage de la suspension, et surtout le fait
    // qu'un vrai vetetiste ne laisse pas filer sa machine sur un sentier.
    //
    // C'est ce coefficient qui FIXE les vitesses de pointe en descente, et c'est
    // pour ca qu'il est le parametre le plus arbitraire du modele. A 20 % de
    // pente il donne 10,6 m/s (38 km/h), a 30 % 13,3 m/s (48 km/h) : la
    // fourchette VTT visee.
    float dragCoefficient = 0.014f;

    // --- Freinage ------------------------------------------------------------

    // Deceleration au frein a fond, en m/s^2. 6,0 m/s^2 ~ 0,61 g : c'est a peu
    // pres la limite avant le passage par-dessus le guidon sur un VTT, et bien
    // au-dela de ce que le terme de resistance seul peut faire. Le freinage
    // demande est PROGRESSIF (brakeInput dans [0, 1]).
    float brakeDeceleration = 6.0f;

    // --- PHASE 23 : etat du sol ---------------------------------------------
    //
    // Un sol detrempe roule plus mal et freine moins bien. C'est un modele
    // DELIBEREMENT MINIMAL - une seule grandeur, l'humidite dans [0, 1], et deux
    // coefficients - et le cahier des charges laissait le choix entre ca et le
    // reporter a plus tard. Il est fait parce qu'il ne demande aucune donnee
    // nouvelle (WeatherSample::wetness existe deja) et parce qu'il se branche sur
    // des termes qui existent deja dans l'equation du mouvement, sans y ajouter
    // le moindre etat.
    //
    // CE QU'IL NE FAIT PAS, et c'est note comme amelioration future dans
    // NOTES-PHASE23.md : aucune nature de sol (terre, roche, herbe - le LiDAR
    // n'en porte aucune, voir la limite deja etablie en Phase 22 pour les
    // gisements), aucune boue qui se formerait et secherait par endroits, aucune
    // perte d'adherence EN VIRAGE (le terme lateral n'est pas touche, parce que
    // le rayon de braquage est un equilibre deja verifie par les tests de la
    // Phase 13 et qu'y toucher sans mesure serait un pari).
    //
    // VALEURS PROVISOIRES.

    // Surcout de resistance au roulement sur sol detrempe, en fraction. 0,45 :
    // une piste boueuse coute la moitie de resistance en plus qu'une piste
    // seche.
    float wetRollingGain = 0.45f;

    // Perte de freinage sur sol detrempe, en fraction. 0,30 : on freine 30 % moins
    // fort, ce qui est la partie qui SE SENT en descente.
    float wetBrakingLoss = 0.30f;

    // --- Plafond -------------------------------------------------------------

    // Vitesse maximale absolue, en m/s. 13,9 m/s = 50,0 km/h, le haut de la
    // fourchette demandee. Ce n'est PAS le regulateur habituel du modele : les
    // resistances y parviennent seules jusqu'a ~32 % de pente. Au-dela, ce
    // plafond prend le relais plutot que de laisser la vitesse diverger sur une
    // paroi LiDAR (la zone de jeu a des pentes locales bien pires que 32 %).
    float maxSpeed = 13.9f;

    // --- Direction -----------------------------------------------------------

    // Acceleration laterale maximale en virage, en m/s^2. 4,0 m/s^2 ~ 0,41 g :
    // l'adherence d'un pneu large sur terre seche, franchement en dessous de ce
    // que tient un pneu de route sur bitume.
    //
    // C'est ce qui donne un RAYON DE BRAQUAGE COHERENT : le rayon stabilise vaut
    // R = v^2 / maxLateralAccel, donc 6,3 m a 5 m/s et 25 m a 10 m/s. La vitesse
    // angulaire correspondante est maxLateralAccel / v, donc elle DIMINUE quand
    // la vitesse monte - impossible de pivoter sur place a pleine vitesse.
    float maxLateralAccel = 4.0f;

    // Plafond de vitesse angulaire, en degres/seconde. Necessaire parce que la
    // formule ci-dessus diverge a l'arret (R = 0). 140 deg/s est la vitesse a
    // laquelle un cycliste presque immobile peut replacer sa machine avec les
    // pieds - c'est ce qui evite de rester coince nez dans une pente trop raide.
    float maxTurnRateDegPerSec = 140.0f;

    // --- Geometrie -----------------------------------------------------------

    // Demi-largeur du velo pour la COLLISION, en unites monde. 0,45 m contre
    // 0,35 m pour le joueur a pied (Camera::bodyRadius_) : un guidon est plus
    // large que des epaules. Volontairement en dessous de la demi-largeur reelle
    // d'un guidon (~0,37 m) plus la roue : on ne cherche pas a accrocher un
    // tronc du bout du cintre, on cherche a ne pas le traverser.
    float bodyRadius = 0.45f;

    // Hauteur de l'oeil au-dessus du sol une fois en selle, en unites monde.
    // Meme valeur que la marche (kDefaultEyeHeight) : assis sur une selle de
    // VTT, les yeux sont a peu pres a la meme hauteur que debout. La valeur
    // vient de l'appelant pour que world/ ne dependent pas de core/Camera.hpp.
    float eyeHeight = 1.7f;

    // Demi-base des differences finies centrees utilisees pour mesurer la pente
    // locale, en metres. La pente est donc lue sur 2 x slopeSampleStep.
    //
    // 1,0 m - le pas de la heightmap - PARAIT le choix evident et ne marche pas.
    // Mesure faite sur un versant reel de la dalle LHD_0907_6440 (200 m plein
    // nord depuis E 907172 / N 6439790, qui descend de 26,7 % en moyenne) :
    //
    //   demi-base | ecart-type de la pente | sections en CONTRE-pente | pic
    //       1 m   |         16,4 %         |          8,0 %           | +22,0 %
    //       2 m   |         15,1 %         |          6,1 %           | +10,5 %
    //   >>  2 m sur 4 m de base (le reglage retenu, differences a +/-2 m)
    //       4 m   |         14,0 %         |          1,6 %           |  +1,1 %
    //
    // A 1 m, une descente franche presente donc 8 % de micro-sections montantes
    // a plus de 20 % : de quoi CALER le velo (la pente maximale franchissable
    // est de 23,6 %) au beau milieu d'un devers qui descend. Constate en jeu :
    // le velo s'immobilisait definitivement sur un versant a -27 %.
    //
    // 2,0 m (base de 4 m) ramene ces sections a 1,6 % avec un pic a +1,1 %,
    // SANS toucher a la pente moyenne (-26,9 % contre -26,8 %). Ce n'est pas un
    // artifice : un DTM LiDAR a 1 m porte du bruit decimetrique, et une machine
    // de 1,1 m d'empattement lancee a 20 km/h ne "sent" pas un creux d'un metre.
    // Au-dela de 4 m de base on commencerait a effacer de vraies ruptures de
    // pente, d'ou l'arret ici.
    float slopeSampleStep = 2.0f;

    // Inclinaison visuelle maximale, en degres, dans les deux axes. Sert
    // uniquement d'ecretage d'affichage : une paroi LiDAR verticale donnerait
    // sinon un marqueur couche a plat.
    float maxVisualTiltDegrees = 45.0f;
};

// Position, cap, vitesse et assiette d'une sortie a velo.
//
// Comme Paraglider, cette classe ne connait NI Vulkan NI la Camera : elle
// produit une position, un cap et deux angles d'assiette, et c'est l'appelant
// (main.cpp) qui en tire une pose de camera a la troisieme personne via
// Camera::setChasePose().
//
// Deux differences de fond avec le parapente, toutes deux voulues :
//  - le velo est TOUJOURS en contact avec le sol : sa position verticale n'est
//    pas integree, elle est IMPOSEE par heightAt() a chaque pas. Il n'y a donc
//    ni saut, ni chute, ni decollage sur une bosse ;
//  - le velo respecte les OBSTACLES, via le meme core/CollisionSolver et la meme
//    fonction moveBody() que le joueur a pied. Aucune logique de collision n'est
//    reecrite ici.
class Bike {
public:
    explicit Bike(const BikeParams& params = {});

    const BikeParams& params() const { return params_; }

    // Enfourche le velo a la position et au cap donnes (degres, convention
    // Camera : 0 = +X, 90 = +Z). La position est immediatement recollee au
    // terrain, donc l'appelant n'a pas a fournir un Y correct : seuls X et Z
    // comptent. La vitesse part de zero - pas de demarrage lance.
    void mount(const glm::vec3& position, float headingDegrees,
               const HeightSampler& terrain);

    // Descend de velo. Ne deplace pas la position : c'est a l'appelant d'y
    // reposer le joueur (voir main.cpp), exactement comme pour Paraglider.
    void dismount() { riding_ = false; }

    // Fait avancer la simulation d'un pas de temps.
    //
    //   steerInput  dans [-1, 1] : -1 = a gauche, +1 = a droite
    //   pedalInput  dans [0, 1]  : effort de pedalage
    //   brakeInput  dans [0, 1]  : freinage manuel. PRIORITAIRE sur le pedalage
    //                              (on ne pedale pas en freinant).
    //   terrain     : impose l'altitude ET fournit la pente. Obligatoire.
    //   obstacles   : arbres et souches. nullptr = on traverse tout, comme la
    //                 camera sans solveur.
    //
    // Les entrees sont ecretees en interne : l'appelant peut passer des valeurs
    // brutes de touches (0/1) sans y penser, comme Paraglider::update.
    //
    // PHASE 23 - wetness : humidite du sol dans [0, 1] (0 = sec), tiree de
    // WeatherSample::wetness. Valeur par defaut 0 : tous les appelants ecrits
    // avant la Phase 23 - dont les tests de la Phase 13, qui verifient des
    // vitesses d'equilibre au centieme - decrivent exactement la meme physique.
    void update(float deltaTime, float steerInput, float pedalInput, float brakeInput,
                const HeightSampler& terrain, const CollisionSolver* obstacles,
                float wetness = 0.0f);

    bool riding() const { return riding_; }
    const glm::vec3& position() const { return position_; }
    float headingDegrees() const { return heading_; }

    // Vitesse le long du sol, en m/s (toujours >= 0 : ce modele ne recule pas).
    float speed() const { return speed_; }

    // Vitesse en km/h - la seule unite dans laquelle une vitesse de velo se lit,
    // et donc celle des journaux et des tests.
    float speedKmh() const { return speed_ * 3.6f; }

    // Assiette VISUELLE, en degres. Ne retroagit sur rien : c'est l'inclinaison
    // du marqueur, pas un etat physique.
    //   pitch > 0 : nez leve, le velo monte ;
    //   roll  > 0 : le velo penche sur la DROITE du cycliste.
    // Le roulis somme deux causes : la pente en travers (le velo epouse le
    // devers) et l'inclinaison en virage (angle d'equilibre atan(a_lat / g)).
    float pitchDegrees() const { return pitch_; }
    float rollDegrees() const { return roll_; }

    // Direction horizontale unitaire du cap courant. Meme convention que
    // Paraglider::forward().
    glm::vec3 forward() const;

    // Direction horizontale unitaire a la droite du cycliste. Meme construction
    // que Camera::right_ (cross(front, worldUp)).
    glm::vec3 right() const;

    // Pente le long du cap, en RISE PAR METRE HORIZONTAL (donc une tangente :
    // 0,20 = 20 % = 11,3 degres). Positive en montee. Exposee parce que c'est
    // l'entree qui explique toute la dynamique longitudinale, et qu'un test qui
    // ne pourrait pas la lire devrait la redemontrer a l'envers.
    float slopeAlongHeading(const HeightSampler& terrain) const;

private:
    // Gradient de la surface au point courant, par differences finies centrees.
    // .x = dh/dx, .y = dh/dz (rise par metre horizontal).
    glm::vec2 terrainGradient(const HeightSampler& terrain) const;

    BikeParams params_;

    glm::vec3 position_{0.0f}; // hauteur d'OEIL, comme Camera::position_
    float heading_ = 0.0f;     // degres, convention Camera : 0 = +X, 90 = +Z

    float speed_ = 0.0f; // m/s, toujours >= 0

    float pitch_ = 0.0f; // degres, visuel seulement
    float roll_ = 0.0f;  // degres, visuel seulement

    bool riding_ = false;
};

} // namespace vercora
