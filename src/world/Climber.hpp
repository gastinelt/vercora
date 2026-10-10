#pragma once

#include "core/SlopeLimits.hpp"

#include <glm/glm.hpp>

namespace vercora {

class HeightSampler;
class CollisionSolver;

// Modele d'ESCALADE simplifie (Phase 14), dans le meme esprit que
// world/Paraglider et world/Bike : pas une simulation de grimpe - pas de prises
// individuelles, pas de materiel, pas d'adherence variable, pas de meteo - juste
// de quoi s'engager sur une vraie paroi du terrain LiDAR, y monter, y traverser,
// en redescendre ou en tomber quand l'energie manque.
//
// LA DIFFERENCE DE FOND AVEC LES TROIS AUTRES ACTIVITES tient en une phrase :
// marcher, rouler et voler se font AU-DESSUS de la surface, grimper se fait
// CONTRE elle. Ce que ca change concretement est detaille sur position() plus
// bas - le grimpeur n'a pas de "hauteur d'oeil au-dessus du sol".
//
// Ce que ce modele NE fait PAS, et qui est assume pour cette session :
//   - aucune prise individuelle : une paroi assez raide est grimpable
//     ENTIEREMENT, il n'y a pas de "bon" ou de "mauvais" endroit sur la face ;
//   - aucun SURPLOMB : une heightmap 2,5D ne peut pas en representer (limite
//     deja documentee en Phase 9 sur les noeuds partages des falaises) ;
//   - aucun degat, aucune corde, aucun point d'assurage.
struct ClimberParams {
    // --- Detection de paroi --------------------------------------------------

    // Pente a partir de laquelle une zone est une PAROI et non un versant raide,
    // en RISE PAR METRE HORIZONTAL (1,00 = 45 degres = 100 %).
    //
    // CE SEUIL EST UN CHOIX DE CONCEPTION, PAS UNE DECOUVERTE. La distribution
    // des pentes de la zone de jeu, mesuree par tools/find_climb_walls.py sur
    // les 25 millions d'echantillons du terrain assemble, est un CONTINUUM sans
    // aucune bimodalite - il n'existe pas de "creux" ou poser le seuil :
    //
    //     p50   47,7 %   p75   65,7 %   p90   92,4 %
    //     p95  129,4 %   p99  342,5 %   max 5018,2 %
    //
    // Trois arguments ont fixe la valeur a 1,00 :
    //
    //  1. 45 degres est la borne basse conventionnelle du terrain d'escalade :
    //     en dessous c'est une desescalade ou un pas d'alpinisme facile, au
    //     dessus il faut les mains.
    //  2. C'est NETTEMENT au-dessus de l'angle de repos des materiaux meubles
    //     (35 a 40 degres, soit 70 a 84 %). Une face a 45 degres dans ce massif
    //     ne retient ni eboulis ni terre : c'est du rocher nu, donc precisement
    //     ce qui se grimpe. Le seuil separe donc une realite physique, meme si
    //     la distribution des pentes, elle, ne la montre pas.
    //  3. Il retient 8,4 % de la zone de jeu. Assez pour que l'escalade soit une
    //     vraie facon de franchir le relief, assez rare pour que la marche reste
    //     le mode par defaut. A 70 % on retiendrait 20,7 % du massif, et tout
    //     versant deviendrait une paroi ; a 150 %, 3,8 %, et l'activite ne
    //     servirait que sur les grandes falaises inabordables.
    //
    // PHASE 24 - CE SEUIL A ETE ABAISSE A 0,84 (kWalkMaxSlope), ET LA RAISON
    // N'EST PAS UN REGLAGE MAIS UNE COHERENCE.
    //
    // La Phase 24 a donne a la marche une limite de pente (0,84, l'angle de
    // repos des materiaux meubles - voir core/SlopeLimits.hpp). Tant que
    // l'escalade ne s'engageait qu'a partir de 1,00, il existait une bande
    // [0,84 ; 1,00[ - environ 5 % du massif, soit un versant sur vingt - ou le
    // joueur ne pouvait NI marcher NI grimper. C'etait le vrai defaut de
    // coherence entre les deux activites, et un seuil de plus ne l'aurait pas
    // corrige : deux nombres regles separement finissent toujours par diverger.
    //
    // La correction est donc structurelle : ce seuil EST la limite de marche,
    // la meme constante, et il ne peut plus exister de trou entre les deux.
    //
    // Ce que ca change pour les trois arguments ci-dessus : le (1) devient 40
    // degres au lieu de 45 - toujours au-dessus de l'angle de repos, donc
    // toujours du rocher nu, l'argument (2) tient inchange ; le (3) passe de
    // 8,4 % a environ 13 % de la zone de jeu, ce qui reste tres loin des 20,7 %
    // que la Phase 14 jugeait excessifs.
    //
    // tools/find_climb_walls.py, lui, continue de chercher a 1,00 : c'est un
    // outil de REPERAGE hors ligne, et une paroi a 45 degres reste une
    // meilleure voie de reference qu'une a 40. Il rend desormais un
    // sous-ensemble de ce qui est grimpable en jeu, ce qui est le bon sens de
    // l'ecart - jamais l'inverse.
    float wallSlope = kWalkMaxSlope;

    // Pente sous laquelle on remarche, meme rise par metre. STRICTEMENT plus
    // basse que wallSlope, et c'est tout l'interet : les deux seuils forment une
    // HYSTERESIS.
    //
    // Sans elle, une paroi reelle - qui oscille autour de son seuil d'un metre a
    // l'autre - ferait entrer et sortir du mode escalade a chaque frame. Avec
    // elle, il faut vraiment atteindre du terrain marchable pour sortir.
    //
    // 0,60 (31 degres) est la pente au-dela de laquelle un sentier de montagne
    // ne se fait plus debout de facon soutenue ; c'est aussi la valeur que
    // tools/find_climb_walls.py emploie pour juger si une paroi a un pied
    // abordable, et les deux DOIVENT rester d'accord.
    // PHASE 24 : la valeur est passee dans core/SlopeLimits.hpp
    // (kWalkComfortSlope) pour que la marche et l'escalade lisent le MEME
    // nombre. Elle n'a pas change : 0,60.
    float walkableSlope = kWalkComfortSlope;

    // Demi-base des differences finies centrees qui mesurent la pente, en m. La
    // pente est donc lue sur 2 x slopeSampleStep.
    //
    // Meme valeur et MEME RAISON qu'en Phase 13 (BikeParams::slopeSampleStep) :
    // a 1 m - le pas de la heightmap - le bruit decimetrique d'un DTM LiDAR
    // fabrique des pentes fantomes. Pour le velo ca produisait 8 % de
    // micro-sections en contre-pente ; ici ca produirait des "parois" au milieu
    // d'un pierrier plat, donc un joueur qui entre en escalade sur un caillou.
    // Une base de 4 m est aussi l'echelle physique du probleme : un grimpeur
    // fait 2 m, il ne s'accroche pas a une bosse d'un metre.
    float slopeSampleStep = 2.0f;

    // Portee de la prise de contact, en m, mesuree a l'horizontale devant le
    // joueur. Un peu moins que la portee de ramassage (kPickupReach = 2,5 m) :
    // on s'engage sur une paroi qu'on touche, pas sur une qu'on voit.
    float reach = 2.0f;

    // Longueur de surface sondee vers le haut pour confirmer qu'un point est une
    // vraie paroi, en m, et gain d'altitude minimal exige sur cette longueur.
    //
    // POURQUOI CES DEUX PARAMETRES EXISTENT : parce que le seuil de pente seul ne
    // suffit pas sur du relief mesure, et ce n'est pas une supposition - c'est un
    // echec observe. Le premier essai n'exigeait qu'un point au-dessus de
    // wallSlope ; sur la paroi de reference LiDAR (E 899850 / N 6435230), le
    // joueur qui s'approchait s'accrochait au PREMIER point raide rencontre, qui
    // etait un ressaut isole de quelques decimetres devant la paroi, et
    // l'escalade se terminait par un "sommet" apres 0,3 m gagnes en 1,2 s.
    //
    // La base de 4 m (slopeSampleStep) elimine le bruit decimetrique, mais elle
    // ne peut rien contre un accident de terrain REEL de deux metres : celui-la
    // est une vraie pente forte, simplement il ne mene nulle part. La seule
    // question qui distingue une paroi d'un ressaut est donc "est-ce qu'y grimper
    // fait reellement gagner de la hauteur ?", et c'est exactement ce que ces
    // deux valeurs demandent.
    //
    // Les valeurs sont accordees sur wallSlope et non choisies au hasard : a 45
    // degres pile, 6 m parcourus sur la surface font gagner 4,24 m. Un point tout
    // juste au seuil passe donc de justesse, et tout ce qui s'aplatit avant est
    // rejete.
    float minWallRun = 6.0f;
    float minWallRise = 4.0f;

    // --- Deplacement ---------------------------------------------------------

    // Vitesse d'ascension/descente LE LONG DE LA SURFACE, en m/s.
    //
    // Un grimpeur reel progresse a 0,1 - 0,3 m/s en rocher soutenu. 1,0 m/s est
    // donc environ 5 fois trop rapide, et c'est assume : c'est exactement la
    // meme concession que la vitesse de marche du moteur (14 m/s, soit ~10 fois
    // une marche reelle), choisie pour que traverser 25 km2 ne prenne pas une
    // soiree. Ce qui compte pour la consigne "plus lent que la marche" est le
    // RAPPORT, et il est de 14 : on grimpe quatorze fois moins vite qu'on ne
    // marche. Sur la paroi de reference (61 degres) ca donne 0,87 m/s de gain
    // d'altitude, soit 44 m en 50 s.
    float climbSpeed = 1.0f;

    // Vitesse de traversee laterale, en m/s. Plus lente que l'ascension parce
    // qu'une traversee l'est reellement : on deplace un appui a la fois sans
    // jamais pouvoir se reposer sur les jambes comme en montee.
    float traverseSpeed = 0.6f;

    // Distance a laquelle l'oeil se tient de la surface, en m. Purement une
    // question de cadrage : colle a la paroi, la camera aurait le rocher dans
    // son plan proche. 0,6 m ~ une longueur d'avant-bras, ce qui donne la vue
    // qu'a reellement un grimpeur, nez a une prise.
    float wallOffset = 0.6f;

    // Elevation de l'oeil au-dessus du point d'accroche, en m. PETITE, et c'est
    // le point : sur une paroi, le grimpeur est ETENDU LE LONG de la surface, il
    // n'est pas debout dessus. Les 1,7 m de la marche n'ont aucun sens ici - ils
    // placeraient l'oeil bien au-dessus des mains.
    float eyeRise = 0.3f;

    // --- Energie -------------------------------------------------------------
    //
    // Une seule jauge dans [0, 1], volontairement isolee : la consigne de
    // session demande ce parametre seul, pas un systeme de besoins primaires.
    // Les couts sont exprimes PAR METRE et non par seconde partout ou un travail
    // physique est en jeu - monter 40 m coute la meme chose qu'on le fasse vite
    // ou lentement, ce qui est le comportement correct et evite en prime de
    // rendre la jauge dependante du nombre d'images par seconde.

    // Cout par metre d'altitude GAGNEE. 0,016 /m => une jauge pleine vaut
    // 62,5 m de paroi strictement verticale.
    //
    // C'est ce chiffre qui fixe ce que l'escalade permet dans ce massif, et le
    // resultat est interessant : les parois de reference abordables font 30 a
    // 44 m, donc elles passent ; les grandes falaises du massif font 250 a 276 m
    // (mesurees par find_climb_walls.py), donc elles sont HORS DE PORTEE d'une
    // seule jauge. Ce n'est pas un reglage rate, c'est la limite qui donne son
    // sens a l'activite - et elle correspond a la realite, ou une paroi de
    // 270 m se fait en plusieurs longueurs avec des relais.
    float staminaPerMeterUp = 0.016f;

    // Cout par metre d'altitude PERDUE en desescalade. Trois fois moins cher que
    // la montee. Physiquement une desescalade fatigue presque autant ; le choix
    // est un choix de JEU, et il est delibere : redescendre doit toujours rester
    // une porte de sortie, sinon un joueur qui s'engage trop haut n'a plus que la
    // chute comme issue, ce qui rend l'activite punitive au lieu d'etre tendue.
    float staminaPerMeterDown = 0.005f;

    // Cout par metre parcouru a l'HORIZONTALE (traversee, et la composante
    // horizontale d'une montee en paroi non verticale). Meme ordre que la
    // desescalade : on ne lutte pas contre la pesanteur.
    float staminaPerMeterLateral = 0.005f;

    // Cout par SECONDE quand on est accroche sans bouger. Le seul cout au temps
    // du modele, et il en faut un : sans lui, rester suspendu serait un repos
    // gratuit, et la jauge cesserait de limiter quoi que ce soit. 0,004 /s =>
    // 250 s (4 min) suspendu immobile, ce qui est genereux mais fini.
    float staminaHangPerSecond = 0.004f;

    // Recuperation par seconde, une fois REVENU AU SOL. 0,05 /s => 20 s de repos
    // pour une jauge vide. La recuperation ne se fait jamais en paroi : c'est ce
    // qui force a topper ou a redescendre, donc ce qui fait de la jauge une
    // contrainte de trajet et pas seulement un compteur.
    float staminaRecoveryPerSecond = 0.05f;

    // Demi-largeur du grimpeur pour la COLLISION, en m. Meme valeur que le
    // joueur a pied (Camera::bodyRadius_) : c'est le meme corps.
    float bodyRadius = 0.35f;

    // --- PHASE 23 : adherence de la paroi -------------------------------
    //
    // L'humidite locale (WeatherSample::wetness) change la facon dont le rocher
    // tient sous les mains et les pieds. L'effet est MESURE et porte sur la
    // physique existante - la vitesse de progression et le cout energetique -
    // et non sur un booleen "ca glisse" qui n'aurait aucune traduction dans le
    // modele. C'est la demande explicite du cahier des charges de la phase.
    //
    // VALEURS PROVISOIRES, comme celles de WeatherParams. Elles vivent ici parce
    // qu'elles decrivent la REPONSE DU GRIMPEUR a une paroi mouillee, pas
    // l'atmosphere.

    // Adherence d'une paroi RUISSELANTE, dans ]0, 1]. 0,45 : sur du rocher
    // trempe on grimpe a peu pres deux fois moins vite, ce qui est l'ordre de
    // grandeur reel - un calcaire mouille est franchement savonneux, un granite
    // beaucoup moins, et le modele ne connait pas la nature de la roche (voir la
    // limite deja notee en Phase 22 sur les gisements : le LiDAR ne porte aucune
    // information de materiau).
    float wetGrip = 0.45f;

    // Cout energetique d'une paroi de grip minimal, en multiple du cout sec.
    // 1,8 : lutter contre le glissement fatigue, on serre les prises. Le cout
    // varie donc DANS LE MEME SENS que le temps passe, ce qui est ce qui rend
    // une paroi mouillee reellement plus dure a monter et pas seulement plus
    // lente.
    float wetStaminaGain = 1.8f;

    // Demi-hauteur du corps le long de la paroi, en m. Le grimpeur est etendu
    // sur la surface, son corps occupe donc une bande CENTREE sur le point
    // d'accroche, et non une bande qui part des pieds comme a la marche.
    float bodyHalfHeight = 0.9f;
};

// Comment se termine une session d'escalade. Une valeur par sortie DISTINCTE,
// et non un simple booleen, parce que les trois n'ont pas les memes suites :
// seul le sommet laisse le joueur la ou il est arrive.
enum class ClimbExit {
    None,       // toujours en paroi
    ToppedOut,  // sommet atteint : on reste sur place, en marche
    Released,   // lachage volontaire : retour au pied
    Exhausted,  // jauge vide : chute, retour au pied
};

// Position, energie et etat d'une escalade en cours.
//
// Comme Paraglider et Bike, cette classe ne connait NI Vulkan NI la Camera. Elle
// consomme core/HeightSampler (la surface et sa pente) et core/CollisionSolver
// (les obstacles), donc elle est EXECUTEE dans les tests headless.
//
// LE MODELE DE DEPLACEMENT, en une phrase : le grimpeur reste EXACTEMENT sur la
// surface, et il s'y deplace le long de deux directions tangentes - la ligne de
// plus grande pente (monter/descendre) et la ligne de niveau (traverser).
//
// Ce choix est le meme invariant que celui du velo ("la verticale n'est jamais
// une variable d'etat, position.y est relue de heightAt() a chaque pas"), et il
// est ce qui rend l'escalade compatible avec une heightmap sans rien ajouter au
// moteur : il n'y a pas de "volume" de falaise a representer, seulement une
// surface a suivre. Sur une paroi a 87 degres - il y en a de vraies dans la zone
// de jeu, voir NOTES-PHASE-ESCALADE.md - avancer d'un metre le long de la
// surface fait gagner 0,999 m d'altitude et 0,05 m a l'horizontale : la montee
// est donc quasi verticale, sans qu'aucun cas particulier n'ait ete ecrit.
//
// La limite honnete de ce modele est celle de la 2,5D : un SURPLOMB n'existe
// pas. Une paroi qui deverse est representee par la heightmap comme une paroi
// tres raide mais positive, et se grimpe comme telle.
class Climber {
public:
    explicit Climber(const ClimberParams& params = {});

    const ClimberParams& params() const { return params_; }

    // --- Detection de paroi (utilisable sans etre en escalade) ---------------

    // Pente locale de la surface (magnitude du gradient, rise par metre) en
    // (worldX, worldZ). Lue sur une base de 2 x slopeSampleStep.
    float slopeAt(const HeightSampler& terrain, float worldX, float worldZ) const;

    // Cette position est-elle sur une paroi grimpable ?
    bool isWall(const HeightSampler& terrain, float worldX, float worldZ) const {
        return slopeAt(terrain, worldX, worldZ) >= params_.wallSlope;
    }

    // Cherche une paroi devant le joueur et s'y engage.
    //
    //   eyePosition   : position de l'oeil du joueur a pied (Camera::position()).
    //   lookDirection : direction du regard ; seule sa projection horizontale
    //                   compte, regarder le ciel ne doit pas changer la cible.
    //
    // Sonde la surface a intervalles reguliers de 0 a params.reach devant le
    // joueur et s'accroche au PREMIER point dont la pente atteint wallSlope.
    // Sonder plusieurs distances et non la seule portee maximale est ce qui
    // permet de s'engager qu'on soit colle a la paroi ou a deux metres.
    //
    // Un point n'est retenu que s'il est une paroi ET si grimper depuis lui fait
    // reellement gagner de la hauteur (voir minWallRun / minWallRise) : sans
    // cette seconde condition, on s'accroche au premier ressaut venu.
    //
    // Renvoie false et ne change RIEN si aucune paroi n'est a portee, ou si la
    // jauge est deja trop basse pour s'engager (voir minStaminaToStart()).
    // L'appelant peut donc appeler sans verifier quoi que ce soit d'abord.
    bool tryStart(const HeightSampler& terrain, const glm::vec3& eyePosition,
                  const glm::vec3& lookDirection);

    // --- Escalade en cours ---------------------------------------------------

    // Fait avancer l'escalade d'un pas de temps.
    //
    //   climbInput    dans [-1, 1] : +1 monte, -1 desescalade
    //   traverseInput dans [-1, 1] : +1 vers la DROITE du grimpeur face a la
    //                                paroi, -1 vers la gauche
    //   terrain       : impose la surface suivie ET fournit la pente. Obligatoire.
    //   obstacles     : troncs et souches. nullptr = on traverse tout.
    //
    // Les entrees sont ecretees en interne, comme Bike::update.
    //
    // Renvoie la facon dont l'escalade s'est terminee PENDANT ce pas, ou
    // ClimbExit::None si elle continue. Quand la valeur n'est pas None, climbing()
    // est repasse a false et position() est deja la position finale : sommet pour
    // ToppedOut, pied de paroi pour Released et Exhausted.
    //
    // PHASE 23 - grip : adherence de la paroi dans [0, 1], 1 = rocher sec.
    // L'appelant la tire de l'humidite locale (voir gripForWetness). Elle
    // RALENTIT la progression et RENCHERIT chaque metre gagne. Sa valeur par
    // defaut est 1 : tous les appelants ecrits avant la Phase 23 - dont les
    // tests de la Phase 14 - decrivent exactement la meme escalade.
    ClimbExit update(float deltaTime, float climbInput, float traverseInput,
                     const HeightSampler& terrain, const CollisionSolver* obstacles,
                     float grip = 1.0f);

    // Adherence correspondant a une humidite dans [0, 1]. Interpolation lineaire
    // entre 1 (sec) et params.wetGrip (ruisselant).
    //
    // Exposee comme une fonction de la CLASSE et non laissee a l'appelant pour
    // qu'il n'y ait qu'une seule traduction humidite -> adherence dans le jeu :
    // main.cpp et les tests doivent employer la meme, sinon le comportement
    // teste n'est pas celui qui est joue.
    float gripForWetness(float wetness) const;

    // Lachage volontaire. Meme issue qu'une chute d'epuisement - retour au pied
    // de la paroi - et c'est voulu : lacher une paroi, c'est descendre au pied,
    // qu'on l'ait decide ou non. Une seule reinitialisation de position a ecrire,
    // donc une seule a verifier.
    //
    // Sans effet (et renvoie None) si on n'est pas en escalade.
    ClimbExit release();

    // Recuperation au sol. A appeler a chaque frame QUAND ON NE GRIMPE PAS ;
    // sans effet pendant une escalade, l'appelant n'a donc pas a s'en soucier.
    void restOnGround(float deltaTime);

    bool climbing() const { return climbing_; }

    // Position de l'OEIL, en monde.
    //
    // ATTENTION, la convention n'est PAS celle des trois autres activites. A
    // pied, a velo et en parapente, position() est l'oeil place a eyeHeight
    // AU-DESSUS de la surface. En escalade, le grimpeur est CONTRE la surface :
    // l'oeil est le point d'accroche decale de wallOffset vers l'exterieur de la
    // paroi (le long de la descente) et de eyeRise vers le haut. C'est la seule
    // facon d'obtenir la vue reelle d'un grimpeur, et c'est aussi ce qui
    // garantit que l'oeil est HORS du relief : le decalage se fait vers l'aval,
    // donc vers une altitude de terrain plus basse que celle de l'accroche.
    glm::vec3 position() const;

    // Point d'accroche : le point exactement SUR la surface. C'est l'etat reel
    // du grimpeur ; position() n'en est qu'une vue. Expose parce que c'est lui
    // qu'un test doit confronter a heightAt(), et lui qui devient la position du
    // joueur au sommet.
    const glm::vec3& anchor() const { return anchor_; }

    // Point ou le joueur s'est engage sur la paroi - du terrain marchable, par
    // construction. C'est la position de repli d'une chute ou d'un lachage.
    const glm::vec3& groundAnchor() const { return groundAnchor_; }

    // Jauge d'energie dans [0, 1]. 1 = frais, 0 = chute.
    float stamina() const { return stamina_; }

    // Energie minimale pour S'ENGAGER sur une paroi. Un seuil d'entree distinct
    // de zero evite le cas absurde ou le joueur s'accroche avec 1 % de jauge et
    // retombe dans la seconde ; il laisse de quoi monter puis redescendre.
    static constexpr float minStaminaToStart() { return 0.15f; }

    // Altitude gagnee depuis l'engagement, en m. Negative si on est descendu
    // sous son point de depart. Sert au journal et aux tests : c'est la mesure
    // qui dit si une escalade a reellement servi a quelque chose.
    float heightGained() const { return anchor_.y - groundAnchor_.y; }

    // Direction horizontale unitaire vers l'INTERIEUR de la paroi (la ligne de
    // plus grande pente, vers le haut). C'est la direction dans laquelle le
    // grimpeur fait face. Renvoie (0,0,0) sur une surface strictement plate, ou
    // elle n'est pas definie - cas qui ne peut pas se produire en paroi.
    glm::vec3 wallFacing(const HeightSampler& terrain) const;

    // Normale horizontale sortante de la paroi : -wallFacing(). C'est vers la
    // que l'oeil est decale, et c'est la direction dans laquelle on tombe.
    glm::vec3 wallOutward(const HeightSampler& terrain) const {
        return -wallFacing(terrain);
    }

private:
    // Gradient de la surface, differences finies centrees. .x = dh/dx,
    // .y = dh/dz. Meme construction que Bike::terrainGradient.
    glm::vec2 gradientAt(const HeightSampler& terrain, float worldX, float worldZ) const;

    // Le grimpeur a-t-il atteint du terrain ou l'on tient debout ?
    //
    // DEUX points sont testes, pas un : le point d'accroche ET un point 3 m plus
    // loin dans la pente. Un seul point sortirait du mode escalade sur la
    // moindre dalle horizontale au milieu d'une face, donc EN PLEIN VIDE. Exiger
    // que ca tienne sur 3 m demande une vraie vire - et une vire de 3 m de
    // profondeur EST un endroit ou l'on se tient debout, donc y terminer
    // l'escalade est le comportement correct et non un pis-aller.
    bool hasToppedOut(const HeightSampler& terrain) const;

    // Gain d'altitude qu'on obtiendrait en grimpant minWallRun metres de surface
    // depuis (worldX, worldZ), en suivant la ligne de plus grande pente comme le
    // fait update(). C'est la mesure qui distingue une paroi d'un ressaut isole :
    // elle repond a "grimper ici mene-t-il quelque part ?" plutot qu'a "est-ce
    // raide ici ?", qui est la question a laquelle un seuil de pente repond deja
    // et qui ne suffit pas.
    float wallRise(const HeightSampler& terrain, float worldX, float worldZ) const;

    // Recolle le point d'accroche sur la surface. Appele apres tout deplacement
    // horizontal, comme Bike le fait de position_.y.
    void snapAnchor(const HeightSampler& terrain);

    ClimberParams params_;

    glm::vec3 anchor_{0.0f};       // point SUR la surface
    glm::vec3 groundAnchor_{0.0f}; // pied de paroi, position de repli

    float stamina_ = 1.0f;
    bool climbing_ = false;

    // Cache de la normale sortante du dernier pas. position() est const et ne
    // recoit pas de terrain : elle ne peut donc pas recalculer le gradient. Le
    // cacher ici est ce qui permet a l'appelant de lire la pose de la camera
    // sans avoir a repasser le terrain.
    glm::vec3 outward_{1.0f, 0.0f, 0.0f};
};

} // namespace vercora
