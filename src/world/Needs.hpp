#pragma once

#include <string>

namespace vercora {

class HeightSampler;

// ===========================================================================
// PHASE 16 : les besoins primaires du joueur.
// ===========================================================================
// Quatre jauges - faim, soif, fatigue, temperature - dans [0, 1], ou 1 est
// l'etat satisfait et 0 l'etat critique. AUCUNE ne tue : conformement au
// cadrage ("bac a sable et survie semi-simulee", pas de punition brutale), une
// jauge a zero degrade les capacites physiques et rien d'autre. Il n'y a ni
// mort, ni ecran de fin, ni compteur de score.
//
// La classe est dans le meme moule que Paraglider / Bike / Climber : elle ne
// connait ni Vulkan, ni GLFW, ni la Camera. Elle recoit un pas de temps et un
// CONTEXTE (ce que le joueur est en train de faire, ou il est, quelle heure il
// est), et elle rend des jauges et des COEFFICIENTS. C'est l'appelant -
// main.cpp - qui applique ces coefficients aux entrees des activites. Rien
// dans Bike ni dans Climber n'a ete modifie pour cette phase, et c'est
// volontaire : une penalite qui multiplie une commande deja ecretee ne peut
// pas casser une physique dont les tests passent depuis deux phases.
//
// ---------------------------------------------------------------------------
// ECHELLE DE TEMPS : les taux sont en SECONDES REELLES
// ---------------------------------------------------------------------------
// Toutes les vitesses de variation ci-dessous sont exprimees par seconde REELLE
// de jeu, pas par heure simulee. La cible annoncee dans le cahier des charges
// de la phase est : rien de perceptible en quelques minutes, un vrai enjeu sur
// une session de 30 a 60 minutes. Les valeurs par defaut donnent, depuis des
// jauges pleines et en marchant sans arret :
//
//   fatigue     vide en  90 min   (penalite des 45 min, moitie de jauge)
//   soif        vide en  65 min a la marche  (2 h 30 a l'arret)
//   faim        vide en 126 min a la marche  (4 h    a l'arret)
//   temperature depend du relief et de l'heure - voir plus bas
//
// Autrement dit : sur une session d'une heure de marche, la fatigue est le
// premier sujet, la soif devient sensible vers la fin, la faim ne fait que
// commencer a se voir. C'est l'ordre voulu - il pousse a se reposer et a
// chercher un point d'eau, sans transformer une exploration en gestion de
// menus. Les activites intenses accelerent tout : l'escalade fatigue quatre
// fois plus vite que la marche, le velo pres de deux fois.
//
// Ces chiffres sont VERIFIES par les tests headless (tests/logic_tests.cpp,
// section Phase 16) : c'est la que se lit le comportement reellement obtenu.
// ===========================================================================

// Ce que le joueur est en train de faire. Une par mode de camera qui coute de
// l'energie, plus Rest pour l'arret complet.
enum class Activity {
    Rest,       // debout ou assis, immobile : la seule ou la fatigue remonte
    Walk,       // marche
    Bike,       // velo (Phase 13)
    Climb,      // escalade (Phase 14)
    Paraglide,  // parapente (Phase 8)
};

const char* activityLabel(Activity activity);

// Effort physique relatif de chaque activite, la marche valant 1. C'est le
// SEUL endroit ou l'intensite des activites est comparee ; faim, soif, fatigue
// et chaleur produite en derivent toutes.
//
//   Rest      0,0  aucun effort - et c'est ce zero qui fait du repos un choix
//   Walk      1,0  reference
//   Bike      1,8  pedaler coute plus que marcher a duree egale, mais on
//                  descend aussi en roue libre : d'ou 1,8 et non 3
//   Climb     4,0  l'escalade est l'effort maximal du jeu, et de loin. Elle a
//                  deja sa propre jauge courte (Climber::stamina, qui se vide
//                  en quelques minutes) ; la fatigue, elle, est l'enveloppe
//                  longue qui interdit d'enchainer dix parois dans l'apres-midi
//   Paraglide 0,3  le vol n'est pas un repos (on pilote, on a froid) mais ce
//                  n'est pas un effort musculaire
float activityEffort(Activity activity);

// ---------------------------------------------------------------------------
// Contexte d'une image : tout ce que les jauges doivent savoir du monde.
// ---------------------------------------------------------------------------
struct NeedsContext {
    Activity activity = Activity::Rest;

    // ----------------------------------------------------------------------
    // PHASE 23 - LA TEMPERATURE AMBIANTE ARRIVE DE L'EXTERIEUR
    // ----------------------------------------------------------------------
    // Jusqu'a la Phase 22, ce contexte portait l'altitude, l'heure et
    // l'exposition, et Needs en DEDUISAIT lui-meme l'ambiante par une formule
    // qui lui appartenait (ambientTemperature(), supprimee).
    //
    // Cette formule a ete REMPLACEE, pas doublee : le calcul vit desormais dans
    // world/Weather.hpp, qui sait des choses que Needs ne pouvait pas savoir
    // (saison, masse d'air, nebulosite, inversion de fond de cuvette), et Needs
    // en CONSOMME le resultat. Il n'existe donc qu'UN SEUL calcul de temperature
    // dans tout le jeu, et il est impossible que deux versions divergent - ce
    // qui etait le risque explicite du cahier des charges.
    //
    // Le remplissage habituel est :
    //     const WeatherSample w = weather.sampleAt(position, gameHours, terrain);
    //     ctx.ambientTemperatureC = w.temperatureC;
    //     ctx.windSpeedMps        = w.windMps;
    //
    // La valeur par defaut (12 degres = le confort par defaut) existe pour qu'un
    // contexte construit sans meteo - un test de faim, par exemple - ne fasse ni
    // chaud ni froid, au lieu de geler le joueur a 0 degre.
    float ambientTemperatureC = 12.0f;

    // Vent au point du joueur, en m/s. Alimente le refroidissement eolien (voir
    // windChillC dans Weather.hpp), qui a remplace la constante forfaitaire
    // NeedsParams::paraglideWindChillC de la Phase 16.
    //
    // EN VOL, C'EST A L'APPELANT D'Y AJOUTER LA VITESSE PROPRE DE L'AILE : un
    // parapente qui vole a 10,5 m/s dans un air calme subit bien 10,5 m/s de
    // vent relatif. C'est ce qui reproduit - en mieux, parce que ca depend
    // desormais du vent reel et de la temperature - le refroidissement du vol
    // libre que la Phase 16 codait en dur.
    float windSpeedMps = 0.0f;

    // Le joueur est-il abrite du vent et du froid ? Aucun abri n'existe encore
    // dans le jeu ; le champ est le point d'ancrage prevu pour le jour ou
    // l'artisanat en produira un, et les tests l'exercent deja. Depuis la
    // Phase 23 il annule AUSSI le refroidissement eolien, ce qui est la
    // definition meme d'un abri.
    bool sheltered = false;
};

// ---------------------------------------------------------------------------
// Parametres. Tous documentes, tous modifiables sans toucher au code.
// ---------------------------------------------------------------------------
struct NeedsParams {
    // --- Faim ---------------------------------------------------------------

    // Fraction de jauge perdue par seconde A L'ARRET. 1/(4 h) : un joueur
    // parfaitement immobile met quatre heures a avoir faim pour de bon. La
    // faim est volontairement la plus lente des quatre - c'est le besoin qu'on
    // ne peut pas encore satisfaire (voir eat(), sans appelant a ce stade).
    float hungerPerSecondAtRest = 1.0f / (4.0f * 3600.0f);

    // Part de l'effort dans la depense. Consommation = repos * (1 + gain*effort).
    // 0,9 : marcher (effort 1) fait consommer 1,9 fois plus qu'a l'arret.
    float hungerEffortGain = 0.9f;

    // --- Soif ---------------------------------------------------------------

    // 1/(2 h 30) a l'arret : on a soif plus vite qu'on a faim, ce qui est vrai
    // et ce qui est aussi le bon choix de jeu - la soif est le besoin qu'on
    // PEUT satisfaire (un point d'eau, voir world/WaterSource.hpp), elle doit
    // donc etre celle qui se manifeste.
    float thirstPerSecondAtRest = 1.0f / (2.5f * 3600.0f);

    // 1,3 : l'effort compte plus pour la soif que pour la faim. Marcher fait
    // boire 2,3 fois plus qu'a l'arret, grimper 6,2 fois plus.
    float thirstEffortGain = 1.3f;

    // Chaleur : au-dela de ce ressenti, on transpire et on boit davantage. 22
    // degres, et NON la temperature de confort. Les deux seuils ne sont pas le
    // meme : le confort (12 degres) est la limite en dessous de laquelle on se
    // refroidit, il reste tres frais. Les avoir confondus dans une premiere
    // version rendait le joueur assoiffe par une journee de printemps a 17
    // degres - la soif doublait, et cinq minutes de marche la faisaient deja
    // descendre de 15 %.
    float heatThirstThresholdC = 22.0f;

    // Soif supplementaire par degre RESSENTI au-dessus de ce seuil, en fraction
    // par seconde et par degre. Le climat de montagne est froid : ce terme est
    // presque toujours nul, il existe pour ne pas mentir sur le modele.
    float thirstPerSecondPerDegreeHeat = 1.0f / (6.0f * 3600.0f);

    // --- Fatigue ------------------------------------------------------------

    // Fatigue perdue par seconde A LA MARCHE (effort 1). 1/(90 min) : une
    // heure et demie de marche continue vide la jauge. Les autres activites
    // sont ce taux multiplie par leur effort.
    float fatiguePerSecondWalking = 1.0f / (90.0f * 60.0f);

    // Recuperation a l'arret complet, par seconde. 1/(20 min) : vingt minutes
    // d'immobilite REELLE refont une jauge pleine. C'est long a l'echelle d'une
    // session, et c'est le but - se reposer doit etre une decision, pas un
    // reflexe qu'on prend toutes les deux minutes.
    float fatigueRecoveryPerSecond = 1.0f / (20.0f * 60.0f);

    // --- Temperature --------------------------------------------------------

    // Temperature de confort du joueur, en degres Celsius : au-dessus la jauge
    // remonte, en dessous elle descend. 12 degres et non 20 : le joueur est
    // habille pour la montagne et en mouvement.
    float comfortTemperatureC = 12.0f;

    // PHASE 23 : cinq parametres ont QUITTE cette struct - la moyenne au niveau
    // de la mer, le gradient adiabatique, l'amplitude et l'heure du cycle
    // diurne, et le bonus d'adret. Ils ne sont pas supprimes : ils vivent
    // desormais dans WeatherParams (world/Weather.hpp), avec le calcul qui les
    // employait. Ce qui reste ici ne decrit plus le CLIMAT mais le JOUEUR - sa
    // temperature de confort, ce que son effort lui apporte, ce que la fatigue
    // lui coute. C'est la bonne frontiere, et c'est celle qui garantit qu'il n'y
    // a qu'une seule source de verite sur l'ambiante.

    // Chaleur produite par l'effort, en degres d'ambiante equivalente et par
    // unite d'effort. 1,5 : marcher rechauffe de 1,5 degre, grimper de 6. C'est
    // ce qui rend un bivouac immobile plus froid qu'une montee.
    float exertionWarmthCPerEffort = 1.5f;

    // PHASE 23 : paraglideWindChillC (-6 degres forfaitaires en vol) a ete
    // SUPPRIME. Le refroidissement eolien est desormais calcule par la formule
    // JAG/TI (voir windChillC dans Weather.hpp) a partir du vent REEL au point
    // et de la temperature REELLE - vitesse propre de l'aile comprise, que
    // l'appelant ajoute a NeedsContext::windSpeedMps. Le vol reste donc
    // refroidissant, mais il l'est desormais pour la bonne raison, et la marche
    // en crete par vent fort l'est aussi, ce qu'un forfait reserve au vol ne
    // pouvait pas exprimer.

    // Penalite thermique d'un joueur epuise, en degres a fatigue nulle. -3 :
    // la fatigue empeche de se rechauffer. C'est le seul couplage entre jauges
    // qui passe par la temperature ressentie plutot que par un coefficient.
    float exhaustionChillC = -3.0f;

    // Abri : degres regagnes quand NeedsContext::sheltered est vrai.
    float shelterWarmthC = 5.0f;

    // Vitesse de refroidissement, en fraction de jauge par seconde et par degre
    // manquant. 1/(1800 * 10) : dix degres sous le confort (donc 2 degres
    // ressentis) vident la jauge en 30 minutes ; cinq degres en dessous, il
    // faut une heure. Une nuit en crete devient un vrai probleme, une soiree
    // fraiche en vallee non.
    float coolingPerSecondPerDegree = 1.0f / (1800.0f * 10.0f);

    // Vitesse maximale de rechauffement, par seconde. 1/(10 min) : on se
    // rechauffe trois fois plus vite qu'on ne se refroidit, ce qui est le bon
    // sens du desequilibre - se remettre d'un coup de froid ne doit pas couter
    // une demi-session.
    float warmingPerSecond = 1.0f / (10.0f * 60.0f);

    // Ecart au-dessus du confort qui donne le rechauffement maximal, en degres.
    float fullWarmingMarginC = 5.0f;

    // --- Consequences -------------------------------------------------------

    // Seuil sous lequel faim, soif et froid commencent a penaliser. 0,25 : les
    // trois premiers quarts de chaque jauge sont sans effet, ce qui evite que
    // le joueur soit diminue en permanence.
    float criticalThreshold = 0.25f;

    // Coefficient de deplacement atteint a jauge NULLE, par besoin. Aucun n'est
    // proche de zero : le joueur ralentit, il ne se fige jamais.
    float hungerFloorFactor = 0.70f;
    float thirstFloorFactor = 0.65f;
    float coldFloorFactor = 0.70f;

    // Fatigue : la penalite commence plus tot (a la moitie de la jauge) parce
    // que la fatigue EST la jauge d'effort - c'est elle qui doit se sentir en
    // premier dans les activites physiques.
    float fatiguePenaltyThreshold = 0.50f;
    float fatigueFloorFactor = 0.55f;

    // Plancher absolu du coefficient de deplacement, une fois les quatre
    // penalites multipliees. 0,35 : un joueur au plus mal avance encore a plus
    // du tiers de sa vitesse. C'est la traduction chiffree de "pas de punition
    // brutale" - sans ce plancher, le produit des quatre facteurs tomberait a
    // 0,17 et le joueur serait pratiquement immobilise loin de tout.
    float minimumMovementFactor = 0.35f;

    // Facteur d'aggravation de la fatigue par les autres manques. A 1,0, un
    // joueur affame, assoiffe ou glace se fatigue deux fois plus vite et
    // recupere deux fois moins vite. C'est ce qui empeche de compenser
    // indefiniment la soif par des pauses.
    float deficitFatigueGain = 1.0f;
};

// ---------------------------------------------------------------------------
// Horloge de jeu
// ---------------------------------------------------------------------------
// PHASE 23 - L'INCOHERENCE DE LA PHASE 16 EST CORRIGEE. Le commentaire qui
// occupait cette place disait : "cette horloge n'existe QUE pour la temperature,
// et rien dans le rendu ne change avec elle". Ce n'est plus vrai. La direction
// et l'intensite de la lumiere directionnelle suivent desormais cette horloge
// (voir SynopticState::sunDirection et Renderer::setSun), donc une heure de jeu
// se VOIT autant qu'elle se sent.
//
// Deuxieme changement de la Phase 23 : l'horloge compte desormais le temps
// TOTAL, et pas seulement l'heure du jour. Le systeme meteo en a besoin - une
// perturbation dure plusieurs jours, elle ne peut pas etre indexee par une heure
// qui reboucle toutes les 24 h. hours() n'a pas change (c'est toujours l'heure
// locale dans [0, 24)), totalHours() est le nouveau compteur monotone.
//
// Facteur 12 par defaut : une minute reelle vaut douze minutes de jeu, donc une
// session d'une heure couvre douze heures simulees et traverse reellement une
// tombee de nuit. Attention, ce facteur n'accelere PAS les jauges - elles
// restent reglees en secondes reelles (voir l'en-tete). Il ne pilote que
// l'heure, donc l'ambiante.
class GameClock {
public:
    explicit GameClock(float startHour = 9.0f, float scale = 12.0f);

    void advance(float realSeconds);

    // Heure LOCALE dans [0, 24).
    float hours() const { return hours_; }

    // Heures ECOULEES depuis le debut de partie, sans rebouclage. C'est cette
    // valeur - et non hours() - qu'attend WeatherSystem::synopticAt : le
    // calendrier des perturbations se compte en jours.
    //
    // Le compteur demarre a l'heure de depart (9 h par defaut) et non a zero,
    // pour que totalHours() et hours() restent d'accord modulo 24 : un test qui
    // verifie que la meteo de 14 h est la meme au jour 0 et au jour 3 n'a pas a
    // se souvenir d'un decalage.
    float totalHours() const { return totalHours_; }

    // Jour de partie ecoule, 0 pour le premier.
    int day() const { return static_cast<int>(totalHours_ / 24.0f); }

    float scale() const { return scale_; }

    // Regle l'heure LOCALE. Le compteur total est recale sur le meme jour, donc
    // avancer l'horloge a la main ne fait pas voyager dans le temps meteo de
    // plusieurs jours.
    void setHours(float hours);

    // "09:35", pour le titre de la fenetre et le journal.
    std::string clockText() const;

private:
    float hours_;
    float totalHours_ = 0.0f;
    float scale_;
};

// ---------------------------------------------------------------------------
// PHASE 23 : terrainExposure() et kFullExposureSlope ont DEMENAGE dans
// world/Weather.hpp. Ils n'ont jamais decrit un besoin du joueur - ils
// decrivent le terrain - et c'est le systeme meteo qui en a desormais l'usage
// principal. Le corps de la fonction est inchange, seul son domicile a change.
// Les appelants (main.cpp, les tests) incluent Weather.hpp.

// ---------------------------------------------------------------------------
// Les quatre jauges
// ---------------------------------------------------------------------------
class Needs {
public:
    explicit Needs(const NeedsParams& params = {});

    const NeedsParams& params() const { return params_; }

    // Fait avancer les quatre jauges d'un pas de temps. C'est le seul point
    // d'entree du modele ; deltaTime negatif ou nul ne fait rien.
    void update(float deltaTime, const NeedsContext& context);

    // --- Etat ---------------------------------------------------------------

    float hunger() const { return hunger_; }
    float thirst() const { return thirst_; }
    float fatigue() const { return fatigue_; }
    float temperature() const { return temperature_; }

    // Temperature AMBIANTE du dernier update(), en degres : ce qu'il fait
    // dehors, avant la chaleur de l'effort. Exposee pour le journal et les
    // tests - c'est elle qui relie la jauge au relief et a l'heure.
    float ambientTemperatureC() const { return lastAmbientC_; }

    // Temperature RESSENTIE du dernier update() : ambiante + effort + abri
    // - vent - epuisement. C'est celle que la jauge compare au confort.
    float feltTemperatureC() const { return lastFeltC_; }

    // --- Consequences -------------------------------------------------------

    // Coefficient a appliquer a une commande de deplacement, dans
    // [minimumMovementFactor, 1]. Produit des quatre penalites.
    //
    // C'est la valeur que main.cpp multiplie a la vitesse de marche
    // (Camera::setMoveSpeed), au pedalage (Bike::update) et a la commande de
    // montee (Climber::update). Une seule grandeur pour les quatre activites :
    // il ne peut pas y en avoir une qui oublie une jauge.
    float movementFactor() const;

    // Facteurs individuels, exposes parce qu'un test qui ne verifie que le
    // produit ne dit pas laquelle des quatre jauges a mordu.
    float fatigueFactor() const;
    float hungerFactor() const;
    float thirstFactor() const;
    float coldFactor() const;

    // Le manque le plus grave parmi faim, soif et froid, dans [0, 1] (0 = rien
    // a signaler). Sert a aggraver la fatigue et a nommer le probleme dans le
    // journal.
    float worstDeficit() const;

    // Y a-t-il au moins un besoin sous le seuil critique ?
    bool inTrouble() const { return worstDeficit() > 0.0f; }

    // --- Regeneration -------------------------------------------------------

    // Boire un coup. Remplit la soif et rend true, sauf si la jauge etait deja
    // pleine (rien a boire, rien a signaler) - l'appelant peut donc utiliser le
    // retour pour decider s'il journalise.
    //
    // C'est le point d'ancrage demande par le cahier des charges. main.cpp ne
    // l'appelle que si le joueur est effectivement AU BORD d'un point d'eau
    // detecte sur le relief (voir world/WaterSource.hpp) : la jauge n'est pas
    // remplissable a volonte depuis n'importe ou.
    bool drink();

    // Manger. AUCUN APPELANT A CE STADE, et c'est assume : le jeu ne produit
    // rien de comestible (l'inventaire ne connait que buches, branches, pierres
    // et eclats). La fonction existe pour que le jour ou la cueillette ou la
    // chasse arrivera, il n'y ait rien a inventer ici - et parce que les tests
    // headless peuvent ainsi verifier le remplissage sans attendre ce jour.
    // nutrition est la fraction de jauge rendue, ecretee dans [0, 1].
    bool eat(float nutrition);

    // Remise a plat, pour les tests et pour une nouvelle partie.
    void reset();

    // Reglage direct d'une jauge, pour les tests et le demarrage d'une partie
    // qui ne commencerait pas frais. Ecrete dans [0, 1].
    void setHunger(float value);
    void setThirst(float value);
    void setFatigue(float value);
    void setTemperature(float value);

    // Ligne compacte "faim 87% soif 62% forme 44% chaud 91%", pour le titre de
    // la fenetre. Volontairement des ENTIERS : le titre est relu a l'oeil
    // soixante fois par seconde, pas depouille par un script.
    std::string summary() const;

    // Ligne detaillee pour le journal : les jauges, les temperatures, le
    // coefficient de deplacement. C'est cette ligne que depouille le harnais
    // Vulkan.
    std::string statusLine() const;

private:
    NeedsParams params_;

    float hunger_ = 1.0f;
    float thirst_ = 1.0f;
    float fatigue_ = 1.0f;
    float temperature_ = 1.0f;

    float lastAmbientC_ = 0.0f;
    float lastFeltC_ = 0.0f;
};

// PHASE 23 : ambientTemperature() et daylightFactor() ont ete SUPPRIMES d'ici.
// Leurs remplacants vivent dans world/Weather.hpp :
//   ambientTemperature(...)  ->  WeatherSystem::sampleAt(...).temperatureC
//   daylightFactor(hour)     ->  WeatherSystem::synopticAt(...).daylight
//
// Ce ne sont pas des renommages. La temperature tient desormais compte de la
// saison, de la masse d'air, de la nebulosite et de l'inversion de fond de
// cuvette ; l'ensoleillement est calcule a partir d'une vraie position solaire
// (latitude et declinaison) au lieu d'une demi-sinusoide fixe de 6 h a 18 h.

} // namespace vercora
