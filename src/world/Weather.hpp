#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace vercora {

class HeightSampler;

// ===========================================================================
// PHASE 23 - Systeme meteo deterministe
// ===========================================================================
//
// CE QUE CE MODULE EST, en une phrase : une FONCTION D'ETAT PURE. Pour un point
// (x, y, z) et un instant t, il rend l'etat meteo exact, sans aucun etat cache
// et sans aucune integration dependante du pas de temps.
//
// La consequence de jeu est celle demandee par le cahier des charges : deux
// joueurs qui arrivent au meme endroit au meme instant de jeu vivent EXACTEMENT
// les memes conditions, quel que soit le chemin parcouru, la duree du trajet, le
// nombre d'images par seconde ou le nombre de fois ou la partie a ete mise en
// pause. C'est aussi ce qui rend le systeme testable : un test peut sauter
// directement a t = 137 h sans simuler les 137 heures.
//
// Contraste explicite avec Needs (Phase 16) : les quatre jauges, elles,
// INTEGRENT dans le temps - elles ont une memoire, c'est leur role. Ce module
// n'en a aucune. La seule grandeur qui pourrait sembler avoir de la memoire (la
// mouillure du sol, qui depend de la pluie recente) est obtenue en RELISANT le
// passe de la fonction synoptique, pas en accumulant : voir groundWetness().
//
// ---------------------------------------------------------------------------
// DEUX COUCHES
// ---------------------------------------------------------------------------
//   Couche 1, synoptique (0D) : l'etat macro du massif a l'instant t. Saison,
//     cycle diurne, passage de perturbations. Ne connait AUCUNE position.
//     -> SynopticState / WeatherSystem::synopticAt()
//
//   Couche 2, spatialisation (3D) : la reponse du terrain a cet etat macro en un
//     point donne. Gradient altitudinal, adret/ubac, inversion de fond de
//     cuvette, vent local par sondage amont, renforcement orographique.
//     -> WeatherSample / WeatherSystem::sampleAt()
//
// ---------------------------------------------------------------------------
// AVERTISSEMENT SUR LES VALEURS NUMERIQUES - A LIRE AVANT DE S'Y FIER
// ---------------------------------------------------------------------------
// AUCUNE des valeurs de WeatherParams n'a ete validee par un climatologue. Ce
// sont des ORDRES DE GRANDEUR plausibles, choisis pour que le systeme produise
// un comportement lisible en jeu, et regroupes ICI - dans une seule struct -
// precisement pour pouvoir etre remplaces en bloc apres une vraie relecture
// scientifique.
//
// Ce qui est en revanche implemente serieusement et ne depend pas de ces
// chiffres : l'ARCHITECTURE (purete, determinisme, separation des deux couches)
// et les MECANISMES (comment un gradient, un sondage amont ou une inversion se
// calculent). Changer les chiffres change l'intensite des phenomenes, pas leur
// existence ni leur signe. Les tests headless verifient donc des PROPRIETES
// ("un adret est plus chaud qu'un ubac", "un col vente plus qu'un plateau") et
// jamais des valeurs absolues.
//
// Chaque champ ci-dessous porte, en commentaire : ce qu'il fait, sa valeur
// provisoire, et d'ou elle sort.
// ===========================================================================

// ---------------------------------------------------------------------------
// Constantes provisoires - LE SEUL ENDROIT A MODIFIER APRES RECALIBRAGE
// ---------------------------------------------------------------------------
struct WeatherParams {
    // =======================================================================
    // 1. TEMPS ET SAISON
    // =======================================================================

    // Jour de l'annee du debut de partie, dans [0, 365). 135 = mi-mai : une fin
    // de printemps en moyenne montagne, qui est le climat que decrivaient deja
    // les parametres de la Phase 16 (7 degres de moyenne a 1200 m).
    // PROVISOIRE : c'est un choix de decor, pas une mesure.
    float startDayOfYear = 135.0f;

    // Latitude du massif, en degres. 45,0 : la dalle LiDAR chargee est en Isere
    // (Le Gua), donc tout pres du 45e parallele. C'est la seule valeur de ce
    // fichier qui soit un FAIT plutot qu'un reglage.
    float latitudeDegrees = 45.0f;

    // =======================================================================
    // 2. TEMPERATURE DE REFERENCE (couche 1)
    // =======================================================================

    // Moyenne annuelle au NIVEAU DE LA MER, en degres. 11,0 - un peu plus bas
    // que les 15,0 de NeedsParams (Phase 16), parce que ce 15,0 englobait
    // implicitement la saison : ici la saison est explicite et s'y ajoute.
    // PROVISOIRE.
    float seaLevelAnnualMeanC = 11.0f;

    // Demi-amplitude du cycle SAISONNIER au niveau de la mer, en degres. 9,0
    // donne 18 degres d'ecart entre le coeur de l'hiver et celui de l'ete.
    // PROVISOIRE.
    float seasonalAmplitudeC = 9.0f;

    // Jour du maximum thermique annuel. 205 (fin juillet) et non le solstice
    // (172) : le sol et l'air restituent avec un mois de retard. Le principe est
    // reel, la valeur exacte est PROVISOIRE.
    float seasonalPeakDay = 205.0f;

    // Deuxieme harmonique du cycle saisonnier, en degres. 1,2 : elle rend l'ete
    // legerement plus large et l'hiver legerement plus creux qu'une sinusoide
    // pure. Purement cosmetique a ce stade. PROVISOIRE.
    float seasonalHarmonic2C = 1.2f;

    // Demi-amplitude du cycle DIURNE, en degres. 8,0 : valeur reprise telle
    // quelle de NeedsParams::diurnalAmplitudeC (Phase 16), pour que le
    // remplacement du calcul de temperature ne change pas silencieusement
    // l'equilibre des besoins. PROVISOIRE, comme elle l'etait deja.
    float diurnalAmplitudeC = 8.0f;

    // Heure du maximum thermique quotidien. 15,0, reprise de la Phase 16 : le
    // sol restitue avec retard, le maximum n'est pas a midi. PROVISOIRE.
    float diurnalPeakHour = 15.0f;

    // Deuxieme harmonique du cycle diurne, en degres. 1,0 : elle asymetrise la
    // journee - le rechauffement du matin est plus rapide que le refroidissement
    // du soir, ce qui est le comportement reel. PROVISOIRE.
    float diurnalHarmonic2C = 1.0f;

    // Attenuation de l'amplitude diurne par la couverture nuageuse, sans unite.
    // 0,60 : sous un ciel entierement couvert, l'ecart jour/nuit tombe a 40 % de
    // sa valeur par ciel clair. Le MECANISME est reel et important (c'est lui
    // qui fait qu'une nuit couverte est douce et une nuit claire glaciale) ; la
    // valeur est PROVISOIRE.
    float cloudDiurnalDamping = 0.60f;

    // Gradient thermique vertical, en degres par km. 6,5 = gradient adiabatique
    // standard de l'atmosphere. Valeur reprise de NeedsParams::lapseRateCPerKm.
    // C'est la moins provisoire des valeurs de ce fichier, mais un gradient reel
    // varie de 4 (air humide) a 9,8 (air sec) : a recalibrer avec le
    // climatologue si le realisme prime.
    float lapseRateCPerKm = 6.5f;

    // =======================================================================
    // 3. PERTURBATIONS (couche 1) - bruit 1D en fonction du temps
    // =======================================================================

    // Duree caracteristique d'un systeme meteo, en JOURS de jeu. 3,5 : c'est la
    // periode du bruit, donc la duree typique d'un cycle "beau temps ->
    // degradation -> retour au beau". Ordre de grandeur reel pour un regime
    // perturbe d'ouest en moyenne montagne. PROVISOIRE.
    float frontPeriodDays = 3.5f;

    // Nombre d'octaves du bruit synoptique. 3 : assez pour qu'une perturbation
    // ait de la structure (des accalmies dans un mauvais temps) sans devenir du
    // bruit blanc. Ce n'est pas une valeur climatologique, c'est un reglage de
    // texture.
    int frontOctaves = 3;

    // Gain applique au bruit de perturbation avant ecretage dans [-1, 1]. 2,2.
    //
    // POURQUOI CE PARAMETRE EXISTE, et ce n'est pas un fudge : un fBm de bruit
    // de gradient n'atteint PAS ses bornes theoriques. Sans gain, la nebulosite
    // mesuree sur trente jours de jeu simules restait entre 0,20 et 0,70 - donc
    // il n'y avait JAMAIS de grand beau (jamais de nuit claire, donc jamais
    // d'inversion) et presque jamais de vraie pluie. Ce n'est pas un reglage de
    // gout, c'est ce qui rend le modele capable de produire ses propres cas
    // extremes ; les tests headless verifient qu'un mois de jeu contient bien
    // des journees claires ET des journees de pluie. PROVISOIRE.
    float frontNoiseGain = 2.2f;

    // Amplitude thermique d'une perturbation, en degres. 4,0 : une masse d'air
    // froide fait 4 degres de moins que la moyenne de saison, une masse chaude 4
    // de plus. PROVISOIRE.
    float frontTemperatureAmplitudeC = 4.0f;

    // Nebulosite moyenne, dans [0, 1]. 0,45 : un ciel a moitie couvert en
    // moyenne, ce qui laisse de vraies journees claires et de vraies journees
    // bouchees. PROVISOIRE.
    float meanCloudCover = 0.45f;

    // Demi-amplitude de la nebulosite. 0,50 : combinee a la moyenne ci-dessus,
    // elle permet d'atteindre 0 (grand beau) comme 0,95 (couvert). PROVISOIRE.
    float cloudAmplitude = 0.50f;

    // Nebulosite au-dela de laquelle il PLEUT. 0,72 : un ciel couvert ne donne
    // pas systematiquement de la pluie, il faut le haut de la fourchette. C'est
    // ce seuil qui fixe la frequence des precipitations, et il est PROVISOIRE -
    // c'est probablement la valeur la plus arbitraire du fichier.
    float precipitationCloudThreshold = 0.72f;

    // Intensite de precipitation, en mm/h equivalents, atteinte a nebulosite 1.
    // 6,0 : une pluie franche sans etre un orage. PROVISOIRE.
    float precipitationMaxMmPerHour = 6.0f;

    // Temperature en dessous de laquelle la precipitation tombe en NEIGE.
    // 1,0 degre et non 0 : la neige tient jusqu'a environ +1 degre dans l'air
    // reel. Sert a NOMMER le phenomene ; aucun effet gameplay a ce stade.
    float snowThresholdC = 1.0f;

    // =======================================================================
    // 4. VENT SYNOPTIQUE (couche 1)
    // =======================================================================

    // Direction moyenne d'ou VIENT le vent, en degres de cap compas
    // (0 = nord, 90 = est). 270 = ouest : le regime dominant des Alpes du Nord.
    // Le principe (un regime dominant existe) est solide, la valeur exacte est
    // PROVISOIRE.
    float meanWindFromDegrees = 270.0f;

    // Debattement de la direction autour de la moyenne, en degres. 75 : le vent
    // balaie du sud-ouest au nord-ouest selon la perturbation, sans jamais
    // devenir un vent d'est. PROVISOIRE.
    float windDirectionSwingDegrees = 75.0f;

    // Vent synoptique de base au niveau de la mer, en m/s. 3,0 : une brise
    // permanente, jamais un calme parfait. PROVISOIRE.
    float baseWindMps = 3.0f;

    // Renfort de vent apporte par une perturbation, en m/s. 9,0 : le passage
    // d'un front porte le vent de reference a 12 m/s (43 km/h) au niveau de la
    // mer. PROVISOIRE.
    float frontWindAmplitudeMps = 9.0f;

    // Augmentation du vent avec l'ALTITUDE, en m/s par km. 4,0 : le frottement
    // du sol diminue avec la hauteur, une crete vente plus qu'une vallee meme
    // sans effet de relief. C'est un mecanisme reel et de premier ordre ; la
    // valeur est PROVISOIRE.
    float windAltitudeGainMpsPerKm = 4.0f;

    // =======================================================================
    // 5. SPATIALISATION - TEMPERATURE (couche 2)
    // =======================================================================

    // Bonus thermique d'un adret en plein soleil, en degres. 4,0, repris de
    // NeedsParams::exposureBonusC (Phase 16). PROVISOIRE.
    float aspectBonusC = 4.0f;

    // Attenuation du bonus d'adret par les nuages. 0,85 : sous un ciel couvert,
    // il ne reste que 15 % de l'ecart adret/ubac. Le mecanisme est reel (pas de
    // rayonnement direct, pas d'adret) ; la valeur est PROVISOIRE.
    float cloudAspectDamping = 0.85f;

    // Sinus d'elevation solaire au-dela duquel l'effet d'adret joue a PLEIN.
    // 0,25, soit un soleil a 14,5 degres au-dessus de l'horizon.
    //
    // Ce parametre a ete AJOUTE APRES UN ECHEC DE TEST, et il vaut la peine de
    // dire lequel : sans lui, un versant sud etait plus chaud qu'un ubac de
    // 2,9 degres A DEUX HEURES DU MATIN. La cause etait que l'ecart d'incidence
    // se calcule encore quand le soleil est SOUS l'horizon, ou il n'a plus
    // aucun sens. Le facteur eteint l'effet quand le soleil est bas, ce qui est
    // aussi le comportement reel - un rayonnement rasant chauffe peu, quelle
    // que soit l'orientation. PROVISOIRE.
    float aspectFullDaylightSin = 0.25f;

    // Ecart de cosinus d'incidence qui vaut un effet d'adret COMPLET, sans
    // unite. 0,35.
    //
    // A QUOI CA SERT, et pourquoi ce n'est pas un detail : l'adret/ubac est
    // calcule par PRODUIT SCALAIRE entre la normale du terrain et la direction
    // du soleil, compare a ce que recevrait du terrain PLAT au meme instant. La
    // difference des deux est un ecart de cosinus, sans unite, qu'il faut bien
    // convertir en degres - c'est ce que fait ce nombre.
    //
    // Le comportement qui en decoule est le bon, et il est meilleur que celui de
    // la Phase 16 : a midi, soleil au zenith, un versant sud ne gagne presque
    // rien (il est deja perpendiculaire au rayonnement) alors qu'un versant nord
    // perd beaucoup ; au petit matin, un versant EST recoit le maximum, ce qu'un
    // modele purement nord/sud ne pouvait pas produire. PROVISOIRE.
    float aspectIncidenceReference = 0.35f;

    // Refroidissement MAXIMAL d'un fond de cuvette parfaitement piege par une
    // nuit claire et calme, en degres. -6,0 : une inversion nocturne reelle en
    // moyenne montagne atteint couramment 5 a 10 degres. PROVISOIRE, mais le
    // signe et l'ordre de grandeur sont surs.
    float inversionMaxC = -6.0f;

    // Vent au-dela duquel l'inversion est entierement BRASSEE, en m/s. 4,0 : de
    // l'air froid ne stagne pas dans un fond de vallee vente. Mecanisme reel,
    // valeur PROVISOIRE.
    float inversionMixingWindMps = 4.0f;

    // =======================================================================
    // 6. SPATIALISATION - VENT LOCAL, SONDAGE AMONT (couche 2)
    // =======================================================================
    //
    // Le sondage amont est la technique centrale de cette couche : au lieu de
    // simuler l'ecoulement, on DEMANDE AU RELIEF ce qu'il fait au vent, en
    // lisant la heightmap a quelques distances autour du point. Cout total :
    // 7 appels a heightAt() par evaluation (le point, trois amont, un aval, deux
    // lateraux), soit le budget d'une seule lecture de pente du velo.

    // Distances de sondage EN AMONT, en metres. 250 / 750 / 2000 : trois
    // echelles, parce qu'un seul sondage ne peut pas distinguer un ressaut de
    // dix metres d'un versant de deux kilometres. Ordres de grandeur suggeres
    // par la recherche preparatoire, PROVISOIRES.
    float upwindNearMeters = 250.0f;
    float upwindMidMeters  = 750.0f;
    float upwindFarMeters  = 2000.0f;

    // Distance de sondage EN AVAL, en metres. 100 : assez proche pour dire si le
    // point est sur une rupture de pente, donc en bord de crete. PROVISOIRE.
    float downwindMeters = 100.0f;

    // Distance de sondage LATERAL, perpendiculairement au vent, en metres. 400 :
    // l'echelle d'un col. C'est ce sondage, et lui seul, qui detecte un
    // retrecissement. PROVISOIRE.
    float lateralMeters = 400.0f;

    // Denivele, en metres, qui vaut une exposition COMPLETE. 150 : un point qui
    // domine son amont de 150 m est pleinement expose ; un point qui est 150 m
    // en dessous de son amont est pleinement abrite. PROVISOIRE.
    float exposureReliefMeters = 150.0f;

    // Gain de vent d'un point pleinement expose, sans unite. 0,80 : une crete
    // vente 1,8 fois plus que le vent de reference. Ordre de grandeur reel
    // (l'acceleration au-dessus d'une crete est de 1,5 a 2). PROVISOIRE.
    float exposureWindGain = 0.80f;

    // Reduction de vent d'un point pleinement abrite, sans unite. 0,75 : un fond
    // de vallee abrite ne garde que 25 % du vent de reference. PROVISOIRE.
    float shelterWindReduction = 0.75f;

    // Denivele lateral, en metres, qui vaut un retrecissement COMPLET. 200 : un
    // col dont les deux epaules dominent de 200 m est un entonnoir parfait.
    // PROVISOIRE.
    float venturiReliefMeters = 200.0f;

    // Gain de vent d'un col parfait, sans unite. 0,70 : le vent y est 1,7 fois
    // celui du terrain libre. Effet Venturi reel et bien connu des vols en
    // montagne ; la valeur est PROVISOIRE.
    float venturiWindGain = 0.70f;

    // Vent local maximal, en m/s, tous facteurs cumules. 35 : garde-fou, pas un
    // phenomene. Sans lui, un col en crete un jour de tempete cumulerait les
    // gains et sortirait une valeur absurde.
    float maxLocalWindMps = 35.0f;

    // --- Composante VERTICALE ---------------------------------------------

    // Efficacite de la pente ascendante a soulever l'air (ridge lift), sans
    // unite. 0,55 : une pente de 50 % face a un vent de 10 m/s produit
    // 0,55 * 0,5 * 10 = 2,75 m/s d'ascendance. C'est l'ordre de grandeur reel
    // d'un vol de pente exploitable. PROVISOIRE.
    float ridgeLiftEfficiency = 0.55f;

    // Vitesse verticale maximale d'origine dynamique, en m/s. 6,0 : garde-fou.
    float maxRidgeLiftMps = 6.0f;

    // Subsidence maximale sous le vent, en m/s. -3,0 : le rabattant derriere une
    // crete, qui est ce qui pose les vrais problemes en parapente. Le signe est
    // sur, la valeur est PROVISOIRE.
    float leeSubsidenceMps = -3.0f;

    // Intensite de turbulence maximale sous le vent, dans [0, 1]. 1,0 par
    // construction ; le champ existe pour pouvoir l'adoucir sans toucher au
    // code.
    float leeTurbulenceMax = 1.0f;

    // Vent de reference, en m/s, au-dela duquel la turbulence de sillage est a
    // son maximum. 12,0 : sans vent, pas de turbulence de relief. PROVISOIRE.
    float turbulenceFullWindMps = 12.0f;

    // --- THERMIQUES --------------------------------------------------------

    // Ascendance thermique maximale, en m/s, sur un adret parfaitement expose,
    // par ciel clair, a l'heure la plus chaude. 3,5 : une bonne thermique de
    // moyenne montagne au printemps. PROVISOIRE.
    float thermalMaxMps = 3.5f;

    // Heure du maximum de production thermique. 14,0, une heure AVANT le maximum
    // de temperature : les thermiques sont pilotees par le flux de chaleur du
    // sol, pas par la temperature de l'air, et le flux culmine plus tot.
    // PROVISOIRE, mais l'antecedence est reelle.
    float thermalPeakHour = 14.0f;

    // Demi-largeur de la plage horaire thermique, en heures. 4,5 : les
    // thermiques vivent de ~9 h 30 a ~18 h 30. PROVISOIRE.
    float thermalHalfWidthHours = 4.5f;

    // Attenuation des thermiques par les nuages. 0,80 : sous un ciel couvert il
    // ne reste que 20 % du declenchement. Mecanisme reel, valeur PROVISOIRE.
    float cloudThermalDamping = 0.80f;

    // =======================================================================
    // 6 bis. LUMIERE DU JOUR (rendu)
    // =======================================================================
    //
    // POURQUOI CES QUATRE VALEURS SONT ICI ET NON DANS LE RENDERER : parce que
    // la Phase 16 avait laisse une incoherence explicite - une horloge de jeu
    // acceleree douze fois, dont RIEN dans l'image ne tenait compte, le soleil
    // des shaders etant une direction constante depuis la Phase 2. Corriger ca
    // demande que la lumiere vienne de la MEME position solaire que l'adret et
    // les thermiques, sinon on recree exactement le probleme qu'on repare : deux
    // soleils qui peuvent diverger. Voir sunLightFor().

    // Sinus d'elevation solaire au-dela duquel la lumiere directe est a pleine
    // intensite. 0,15 (8,6 degres) : le soleil met un moment a "s'allumer" apres
    // le lever, ce qui donne une aube et un crepuscule au lieu d'un interrupteur.
    // PROVISOIRE.
    float lightFullDaylightSin = 0.15f;

    // Part de l'eclairage ambiant conservee en pleine nuit, sans unite. 0,30 :
    // la nuit est sombre mais le relief reste lisible. C'est un choix de
    // JOUABILITE assume, pas une mesure - une vraie nuit sans lune est noire, et
    // un jeu d'exploration ou l'on ne voit rien pendant la moitie du temps de jeu
    // n'est pas jouable en l'etat (aucune source de lumiere portable n'existe
    // encore). PROVISOIRE, et a revoir le jour ou le feu de camp arrivera.
    float nightAmbientFactor = 0.30f;

    // Couleur de la lumiere en plein jour, puis a l'horizon. La seconde est la
    // teinte rouge-orangee d'un soleil rasant, dont la lumiere traverse beaucoup
    // plus d'atmosphere. PROVISOIRES toutes les deux, mais le SENS du glissement
    // est reel.
    glm::vec3 daylightColor{1.00f, 0.97f, 0.90f};
    glm::vec3 horizonColor{1.00f, 0.62f, 0.36f};

    // Couleur de l'ambiante nocturne. Bleutee : c'est la convention de tout
    // rendu de nuit, et elle vient de la diffusion de Rayleigh du peu de lumiere
    // residuelle. PROVISOIRE.
    glm::vec3 nightColor{0.42f, 0.50f, 0.72f};

    // Couleur du CIEL en plein jour, au crepuscule et en pleine nuit. Le ciel
    // n'est pas eclaire par le modele de lumiere - c'est une couleur
    // d'effacement - donc il lui faut sa propre palette.
    //
    // Ces trois valeurs ont ete ajoutees APRES la premiere capture du cycle :
    // le relief s'assombrissait correctement mais le ciel restait le bleu vif de
    // midi, ce qui donnait une nuit ou l'on distinguait la silhouette NOIRE des
    // sapins sur un ciel de plein jour. La lumiere etait juste, l'image ne
    // l'etait pas. PROVISOIRES.
    glm::vec3 daySkyColor{0.52f, 0.66f, 0.82f};
    glm::vec3 duskSkyColor{0.42f, 0.33f, 0.36f};
    glm::vec3 nightSkyColor{0.03f, 0.04f, 0.09f};

    // =======================================================================
    // 6 ter. METEO VISIBLE (rendu) - PHASE 27
    // =======================================================================
    //
    // CE BLOC N'AJOUTE AUCUNE MECANIQUE METEO. Chaque valeur ci-dessous est un
    // coefficient d'AFFICHAGE qui traduit en image une grandeur que ce module
    // calculait DEJA - nebulosite, precipitation, neige (donc temperature),
    // vent, mouillure. Aucun de ces champs ne cree de phenomene : mis a zero, ils
    // eteignent l'effet visuel, et le temps qu'il fait reste exactement le meme.
    // La tracabilite complete, grandeur par grandeur, est dans
    // NOTES-PHASE27.md. Voir weatherVisualsFor().
    //
    // Ce que ces coefficients ne pretendent PAS etre : des mesures. Ils sont
    // PROVISOIRES au meme titre que tout ce fichier, et regroupes ici pour la
    // meme raison - pouvoir etre revus en bloc.

    // Assombrissement du ciel COUVERT par rapport a un ciel clair de meme
    // luminosite, sans unite. 0,80 : un ciel bouche est gris et un peu plus
    // sombre qu'un ciel bleu. Pilote par : cloudCover. Le ciel est d'abord
    // DESATURE vers sa propre luminance, puis assombri : c'est ce qui laisse le
    // cycle jour/nuit intact (un ciel couvert de nuit reste un ciel de nuit).
    float overcastSkyDarkening = 0.80f;

    // Gain de l'eclairage AMBIANT sous un ciel couvert, sans unite. 1,0 : a
    // couverture totale et en plein jour, l'ambiante double. Pilote par :
    // cloudCover. Le mecanisme est reel - un ciel couvert diffuse vers le sol une
    // bonne partie de la lumiere qu'il retire au rayonnement direct, ce qui
    // aplatit l'image au lieu de simplement la noircir. N'agit que de JOUR : une
    // nuit couverte n'est pas plus claire qu'une nuit claire. PROVISOIRE.
    //
    // L'extinction du rayonnement DIRECT, elle, n'a pas de parametre ici : elle
    // reprend cloudAspectDamping (section 5), qui modelisait deja exactement ce
    // phenomene pour l'adret. Un seul chiffre pour "les nuages cachent le
    // soleil", donc un seul soleil voile dans tout le jeu.
    float overcastAmbientGain = 1.0f;

    // Coefficient d'extinction de la PLUIE, en 1/km, pour 1 mm/h, et son
    // exposant. Pilote par : precipitationMmPerHour locale. Forme en loi de
    // puissance sigma = a * R^b, qui est la forme des relations empiriques
    // publiees pluie/visibilite ; les deux valeurs sont des ORDRES DE GRANDEUR
    // PROVISOIRES, non verifies contre une source precise. Avec 0,25 et 0,63,
    // une pluie de 5 mm/h donne sigma = 0,69 /km, soit une visibilite de
    // Koschmieder (3,912 / sigma) de 5,7 km : la pluie voile le fond de la zone
    // de 5 km, pas le premier plan. PROVISOIRE.
    float rainExtinctionPerKm = 0.25f;
    float rainExtinctionExponent = 0.63f;

    // Multiplicateur d'extinction quand la precipitation tombe en NEIGE, sans
    // unite. 8,0 : a lame d'eau egale, la neige reduit beaucoup plus la
    // visibilite que la pluie (flocons larges et lents). Le sens est sur,
    // l'ordre de grandeur est PROVISOIRE. Pilote par : snowing, donc par la
    // temperature locale et snowThresholdC.
    float snowExtinctionMultiplier = 8.0f;

    // Vitesse de chute apparente, en m/s, de la pluie et de la neige, pour
    // l'inclinaison des traits a l'ecran (combinee au VENT LOCAL). 6,5 et 1,2 :
    // vitesses terminales usuelles d'une goutte millimetrique et d'un flocon.
    // Ce sont des faits physiques plutot que des reglages, mais retenus a la
    // louche.
    float rainFallSpeedMps = 6.5f;
    float snowFallSpeedMps = 1.2f;

    // Assombrissement d'un sol MOUILLE, sans unite. 0,35 : un sol ruisselant
    // renvoie 35 % de lumiere en moins qu'un sol sec. Pilote par : wetness. Le
    // phenomene est reel et bien connu (un sol humide fonce nettement) ;
    // l'amplitude est PROVISOIRE.
    float wetGroundDarkening = 0.35f;

    // =======================================================================
    // 7. PRECIPITATIONS LOCALES ET ETAT DU SOL (couche 2)
    // =======================================================================

    // Renforcement orographique maximal, sans unite. 0,90 : un versant au vent
    // recoit presque le double de precipitation d'un terrain plat. Le mecanisme
    // (barriere orographique) est reel et de premier ordre en montagne ; le
    // modele employe ici est volontairement SIMPLE - pas d'ombre pluviometrique
    // sous le vent au-dela de la simple absence de renforcement. PROVISOIRE.
    float orographicGain = 0.90f;

    // Pente ASCENDANTE DANS LE SENS DU VENT, en rise par metre, qui vaut un
    // renforcement orographique complet. 0,35 : un versant a 35 % face au flux
    // souleve deja franchement la masse d'air. PROVISOIRE.
    float orographicFullSlope = 0.35f;

    // Nebulosite ajoutee par un renforcement orographique complet, dans [0, 1].
    // 0,15 : le versant au vent s'accroche des nuages que le fond de vallee n'a
    // pas. PROVISOIRE.
    float orographicCloudGain = 0.15f;

    // Renforcement des precipitations avec l'altitude, par km. 0,35 : il pleut
    // et il neige plus haut que plus bas. PROVISOIRE.
    float precipitationAltitudeGainPerKm = 0.35f;

    // --- Mouillure du sol ---------------------------------------------------
    //
    // NON INTEGREE : la mouillure est obtenue en RELISANT la pluie des dernieres
    // heures dans la fonction synoptique (qui est pure), pas en accumulant image
    // par image. C'est ce qui la garde deterministe et rejouable.

    // Fenetre de memoire de la pluie, en HEURES de jeu. 6,0 : de la pluie tombee
    // il y a plus de six heures ne compte plus. PROVISOIRE.
    float wetnessMemoryHours = 6.0f;

    // Nombre d'echantillons de cette fenetre. 8 : un echantillon toutes les
    // 45 minutes de jeu. Reglage de COUT, pas de climat - c'est le seul endroit
    // ou l'evaluation meteo depasse la dizaine d'operations.
    int wetnessSamples = 8;

    // Precipitation, en mm/h, qui suffit a saturer le sol. 3,0. PROVISOIRE.
    float wetnessSaturationMmPerHour = 3.0f;

    // Temps de sechage, en heures de jeu. 4,0 : c'est la constante de la
    // decroissance exponentielle appliquee a chaque echantillon passe.
    // PROVISOIRE.
    float dryingTimeHours = 4.0f;
};

// ---------------------------------------------------------------------------
// COUCHE 1 - Etat synoptique du massif (0D, aucune position)
// ---------------------------------------------------------------------------
struct SynopticState {
    // Temps de jeu correspondant, en heures depuis le debut de partie. Recopie
    // ici pour qu'un etat soit auto-descriptif dans un journal ou un test.
    float gameHours = 0.0f;

    // Heure locale dans [0, 24) et jour de l'annee dans [0, 365).
    float timeOfDayHours = 12.0f;
    float dayOfYear = 0.0f;

    // Temperature de reference AU NIVEAU DE LA MER, en degres. C'est la sortie
    // principale de la couche 1 : la couche 2 en tire toutes les temperatures
    // locales par le gradient altitudinal.
    float seaLevelTemperatureC = 0.0f;

    // Direction d'ou VIENT le vent, en degres compas (0 = nord, 90 = est).
    float windFromDegrees = 270.0f;

    // Vitesse du vent synoptique AU NIVEAU DE LA MER, en m/s. La couche 2 y
    // ajoute l'altitude et le relief.
    float seaLevelWindMps = 0.0f;

    // Nebulosite dans [0, 1] : 0 = grand beau, 1 = entierement couvert.
    float cloudCover = 0.0f;

    // Precipitation de reference (terrain plat, niveau de la mer), en mm/h.
    float precipitationMmPerHour = 0.0f;

    // Elevation du soleil au-dessus de l'horizon, en degres (negative la nuit),
    // et azimut d'ou il VIENT, en degres compas.
    float sunElevationDegrees = 0.0f;
    float sunAzimuthDegrees = 180.0f;

    // Part de soleil dans [0, 1] : sinus de l'elevation, ecrete a 0 la nuit.
    // Remplace daylightFactor() de la Phase 16, qui etait une demi-sinusoide
    // fixe de 6 h a 18 h sans aucune notion de saison ni de latitude.
    float daylight = 0.0f;

    // Direction unitaire DU POINT VERS LE SOLEIL, en repere monde
    // (+X = est, +Z = sud, +Y = haut). C'est ce vecteur qui sert a l'adret/ubac
    // ET a la lumiere directionnelle du rendu : une seule definition du soleil
    // dans tout le jeu.
    glm::vec3 sunDirection{0.0f, 1.0f, 0.0f};

    // Ligne compacte pour le journal et les tests.
    std::string describe() const;
};

// ---------------------------------------------------------------------------
// COUCHE 2 - Etat meteo en un point du monde
// ---------------------------------------------------------------------------
struct WeatherSample {
    // --- Temperature -------------------------------------------------------

    // Temperature AMBIANTE en degres au point demande. C'est desormais la SEULE
    // temperature ambiante du jeu : Needs la consomme au lieu de la recalculer
    // (voir NeedsContext::ambientTemperatureC).
    float temperatureC = 0.0f;

    // Decomposition, exposee pour les tests et le journal - un test qui ne voit
    // que la somme ne dit pas lequel des trois termes a mordu.
    float lapseC = 0.0f;      // part du gradient altitudinal (negative en altitude)
    float aspectC = 0.0f;     // part de l'adret/ubac
    float inversionC = 0.0f;  // part de l'inversion de fond de cuvette (<= 0)

    // --- Vent --------------------------------------------------------------

    // Vitesse du vent HORIZONTAL au point, en m/s.
    float windMps = 0.0f;

    // Vecteur vent horizontal complet, en repere monde, en m/s : il POINTE dans
    // le sens ou l'air se deplace (et non d'ou il vient).
    glm::vec3 windVector{0.0f};

    // Vitesse VERTICALE de l'air, en m/s. Positive = ascendance. Somme des trois
    // contributions exposees juste en dessous.
    float verticalMps = 0.0f;
    float ridgeLiftMps = 0.0f;  // dynamique : le relief force l'air a monter
    float thermalMps = 0.0f;    // convectif : le sol chauffe au soleil
    float subsidenceMps = 0.0f; // sous le vent : l'air redescend (<= 0)

    // Intensite de turbulence dans [0, 1]. 0 = air lisse.
    float turbulence = 0.0f;

    // --- Humidite ----------------------------------------------------------

    // Nebulosite locale dans [0, 1] (celle de la couche 1, modulee).
    float cloudCover = 0.0f;

    // Precipitation locale, en mm/h.
    float precipitationMmPerHour = 0.0f;

    // La precipitation tombe-t-elle en neige au point ?
    bool snowing = false;

    // Mouillure du sol et du rocher dans [0, 1]. 0 = sec, 1 = ruisselant.
    float wetness = 0.0f;

    std::string describe() const;
};

// ---------------------------------------------------------------------------
// Carte de piegeage d'air froid
// ---------------------------------------------------------------------------
// Precalculee UNE FOIS au chargement du terrain, jamais par image - c'est
// l'exigence de cout du cahier des charges.
//
// CE QU'ELLE MESURE : a quel point un point du terrain est un CREUX ENFERME. On
// sonde, autour de chaque cellule, huit directions a trois rayons, et on regarde
// combien de ces 24 sondes sont PLUS HAUTES que la cellule, et de combien. Un
// fond de cuvette est entoure de terrain plus haut dans toutes les directions ;
// une crete ne l'est dans aucune ; un versant l'est dans la moitie.
//
// POURQUOI PAS SIMPLEMENT "L'ALTITUDE EST BASSE" : parce qu'un versant a
// mi-hauteur et un fond de vallee peuvent etre a la meme altitude, et seul le
// second piege l'air froid. C'est l'ENFERMEMENT qui compte, pas l'altitude - et
// c'est exactement la lecon deja apprise en Phase 22 sur le relief (savoir ou il
// y a de la pente ne dit pas par ou l'aborder).
class ColdAirTrapMap {
public:
    // Rayons de sondage, en metres. 120 / 350 / 900 : une cuvette de fond de
    // vallee, un cirque, un bassin. PROVISOIRES comme le reste.
    static constexpr float kProbeRadii[3] = {120.0f, 350.0f, 900.0f};

    // Denivele, en metres, au-dela duquel une sonde compte pour un enfermement
    // COMPLET dans sa direction. 80 : un relief de 80 m au-dessus de soi suffit
    // a retenir une nappe d'air froid. PROVISOIRE.
    static constexpr float kFullEnclosureRise = 80.0f;

    // Carte vide : index 0 partout. C'est le repli legitime quand aucun terrain
    // n'est charge - le systeme meteo reste utilisable, sans inversion.
    ColdAirTrapMap() = default;

    // Construit la carte sur l'emprise [-halfSize, +halfSize] en X et Z.
    //
    //   resolution : cote de la grille. 128 sur les 5 km de la zone de jeu donne
    //                des cellules de 39 m, ce qui est la bonne echelle pour une
    //                cuvette (une cuvette de 39 m n'en est pas une). Cout :
    //                128 * 128 * 24 = 393 216 lectures de heightmap, une seule
    //                fois au chargement.
    ColdAirTrapMap(const HeightSampler& terrain, float halfSize,
                   uint32_t resolution);

    bool empty() const { return values_.empty(); }
    uint32_t resolution() const { return resolution_; }
    float halfSize() const { return halfSize_; }

    // Index de piegeage dans [0, 1] au point monde donne, par interpolation
    // bilineaire. Hors emprise : la valeur du bord le plus proche (meme contrat
    // de TOTALITE que HeightSampler::heightAt).
    float at(float worldX, float worldZ) const;

    // Statistiques, pour le journal de chargement et les tests.
    float maxValue() const { return maxValue_; }
    float meanValue() const { return meanValue_; }

private:
    std::vector<float> values_;
    uint32_t resolution_ = 0;
    float halfSize_ = 0.0f;
    float maxValue_ = 0.0f;
    float meanValue_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Exposition du versant (adret / ubac)
// ---------------------------------------------------------------------------
// DEPLACEE DEPUIS Needs.hpp EN PHASE 23. Elle n'a jamais decrit un besoin du
// joueur : elle decrit le TERRAIN, donc sa place est ici, a cote du reste de la
// spatialisation. Le corps est inchange.
//
// ELLE N'EST PLUS SUR LE CHEMIN DU GAMEPLAY, et il faut le dire clairement :
// depuis la Phase 23, l'effet thermique d'un versant passe par le PRODUIT
// SCALAIRE entre la normale du terrain et la direction du soleil (voir
// WeatherSystem::sampleAt), qui sait faire ce que cette fonction ne sait pas -
// distinguer un versant est d'un versant ouest, et savoir a quelle heure ca
// compte. Ce qui reste ici est une requete de terrain simple, honnete et bon
// marche : "ce point regarde-t-il le sud ?". Elle est toujours exercee par les
// tests, et c'est aujourd'hui son seul appelant.
//
// Renvoie 0 pour un ubac franc (versant nord), 1 pour un adret franc (versant
// sud), 0,5 sur du plat ou sur un versant est/ouest.
//
// La convention d'axes est celle du projet depuis la Phase 7, verifiee dans
// Heightmap::toProjected : **+X va vers l'EST, +Z va vers le SUD**. Un versant
// expose au sud descend donc vers +Z, c'est-a-dire dh/dz < 0.
//
// sampleStep est la DEMI-base de la difference finie, en metres. 2,0 par defaut,
// donc une base de 4 m : exactement la valeur et la raison de
// BikeParams::slopeSampleStep - au pas de 1 m, le bruit decimetrique du LiDAR
// fait lire un adret sur un ubac.
//
// L'exposition est ponderee par la PENTE : sur du terrain quasi plat il n'y a ni
// adret ni ubac, la fonction rend 0,5 quelle que soit la direction du gradient
// residuel.
float terrainExposure(const HeightSampler& terrain, float worldX, float worldZ,
                      float sampleStep = 2.0f);

// Pente (magnitude du gradient) au-dela de laquelle l'exposition est prise en
// compte a 100 %. 30 % : un versant a 30 % est franchement oriente.
inline constexpr float kFullExposureSlope = 0.30f;

// Normale unitaire de la surface en un point, en repere monde. Sert a
// l'adret/ubac "vrai" (produit scalaire avec la direction du soleil) et au
// ridge lift. Meme base de difference finie que terrainExposure.
glm::vec3 terrainNormal(const HeightSampler& terrain, float worldX, float worldZ,
                        float sampleStep = 2.0f);

// Direction monde unitaire correspondant a un cap compas (0 = nord, 90 = est),
// dans la convention d'axes du projet (+X est, +Z sud). Exposee parce que la
// direction du vent est manipulee en degres compas partout dans ce fichier et
// qu'une seule conversion doit exister.
glm::vec3 compassDirection(float degrees);

// ---------------------------------------------------------------------------
// Le systeme
// ---------------------------------------------------------------------------
// SEED : fixe par defaut (kDefaultSeed), et DERIVABLE d'une graine de partie par
// le constructeur. Le choix par defaut est le seed FIXE, et il est delibere :
//
//   - la zone de jeu est unique et fabriquee a la main (une dalle LiDAR reelle),
//     pas generee par partie : rien d'autre dans Vercora n'est reseede par
//     partie, et le semis de vegetation comme celui de la faune sont eux aussi
//     deterministes a graine fixe depuis la Phase 11 ;
//   - un seed fixe rend une capture d'ecran comparable a la precedente, ce qui
//     est la condition meme de la verification visuelle du projet (voir la lecon
//     de la Phase 12 sur --look) ;
//   - le jour ou les parties multiples existeront, passer la graine de partie au
//     constructeur suffit, sans rien changer d'autre.
class WeatherSystem {
public:
    // Graine par defaut. Valeur arbitraire, choisie une fois et figee : la
    // changer change tout le calendrier meteo de toutes les parties.
    static constexpr uint32_t kDefaultSeed = 0x5EA50Eu;

    explicit WeatherSystem(const WeatherParams& params = {},
                           uint32_t seed = kDefaultSeed);

    const WeatherParams& params() const { return params_; }
    uint32_t seed() const { return seed_; }

    // Relie une carte de piegeage d'air froid. Sans elle, le systeme fonctionne
    // et l'inversion vaut zero partout - c'est un repli explicite, pas une
    // panne. Le pointeur n'est PAS possede : la carte vit aussi longtemps que le
    // terrain, donc plus longtemps que n'importe quelle evaluation meteo.
    void setTrapMap(const ColdAirTrapMap* map) { trapMap_ = map; }
    const ColdAirTrapMap* trapMap() const { return trapMap_; }

    // --- COUCHE 1 ----------------------------------------------------------

    // Etat synoptique a gameHours heures depuis le debut de partie. FONCTION
    // PURE : aucun etat de l'objet n'est modifie, et deux appels avec la meme
    // valeur rendent bit a bit le meme resultat.
    SynopticState synopticAt(float gameHours) const;

    // --- COUCHE 2 ----------------------------------------------------------

    // Etat meteo complet au point (x, y, z) a l'instant gameHours.
    //
    // terrain peut etre nullptr : on obtient alors la meteo "en air libre"
    // (gradient altitudinal, vent synoptique corrige de l'altitude, nebulosite),
    // sans aucun effet de relief. C'est le repli utilise avant qu'un terrain soit
    // charge, et il est explicitement teste.
    WeatherSample sampleAt(const glm::vec3& position, float gameHours,
                           const HeightSampler* terrain) const;

    // Meme chose a partir d'un etat synoptique DEJA calcule. C'est cette
    // surcharge qu'il faut appeler quand plusieurs points sont echantillonnes au
    // meme instant (le joueur et des zones de vol, par exemple) : la couche 1 ne
    // depend pas de la position, la recalculer par point serait du gaspillage.
    WeatherSample sampleAt(const glm::vec3& position, const SynopticState& sky,
                           const HeightSampler* terrain) const;

    // Mouillure du sol dans [0, 1] a l'instant gameHours. Exposee separement
    // parce qu'elle coute plus cher que le reste (elle relit wetnessSamples
    // instants passes) et que tous les appelants n'en ont pas besoin.
    // sampleAt() l'inclut deja dans WeatherSample::wetness.
    float groundWetness(float gameHours, float altitudeMeters,
                        float orographicFactor) const;

    // -----------------------------------------------------------------------
    // PHASE 26 : l'exposition adret / ubac CLIMATIQUE, dans [-1, +1].
    // -----------------------------------------------------------------------
    // +1 = adret franc, 0 = plat ou indifferent, -1 = ubac franc.
    //
    // POURQUOI CETTE FONCTION EXISTE A COTE DE WeatherSample::aspectC. Les deux
    // reposent sur le MEME signal - aspectSignal(), une seule implementation
    // dans le .cpp - mais pas au meme instant, et c'est toute la difference :
    //
    //   * aspectC repond a « ce versant recoit-il le soleil MAINTENANT » : il
    //     tourne avec l'heure, s'attenue sous les nuages, et s'eteint la nuit.
    //     C'est ce qu'il faut pour une temperature, et c'est ce qui a corrige en
    //     Phase 23 le versant sud plus chaud a deux heures du matin ;
    //   * celle-ci repond a « ce versant est-il un adret », question a laquelle
    //     l'heure qu'il est n'a rien a voir. Elle evalue donc le signal AU
    //     SOLEIL DE MIDI, et ignore la nebulosite du jour.
    //
    // Une essence d'arbre, une repartition de vegetation, tout ce qui releve du
    // CLIMAT d'un versant et non de sa meteo doit passer par ici : la valeur y
    // est stable dans le temps. Un arbre qui changerait d'essence au coucher du
    // soleil serait le symptome exact d'un appel a la mauvaise des deux.
    //
    // terrain nul => 0 (aucun relief, donc aucun versant).
    float aspectExposureAt(const glm::vec3& position,
                           const HeightSampler* terrain) const;

private:
    // Bruit de perturbation dans [-1, 1] a l'instant donne, sur un canal donne
    // (temperature, nuage, vent, direction). Des canaux SEPARES et non un seul
    // bruit deforme : sans ca, le vent forcirait toujours exactement en meme
    // temps que le ciel se couvre, ce qui se remarque en une journee de jeu.
    float frontNoise(float gameHours, uint32_t channel) const;

    // PHASE 26. Signal d'adret/ubac brut dans [-1, +1] pour une normale de
    // terrain et une direction solaire donnees : l'ecart entre le cosinus
    // d'incidence du point et celui qu'aurait du terrain PLAT, rapporte a
    // params_.aspectIncidenceReference.
    //
    // EXTRAITE de sampleAt SANS EN CHANGER UNE LIGNE DE CALCUL, precisement
    // pour qu'il n'y ait pas deux formules d'adret dans le jeu. C'est la meme
    // exigence que la Phase 23 (un seul soleil) et que la Phase 25 (un seul
    // ecart de reference), appliquee ici a la loi elle-meme : sampleAt l'evalue
    // au soleil courant, aspectExposureAt au soleil de midi, et personne
    // d'autre ne l'ecrit.
    float aspectSignal(const glm::vec3& normal, const glm::vec3& sunDirection,
                       bool sunUp) const;

    WeatherParams params_;
    uint32_t seed_ = kDefaultSeed;
    const ColdAirTrapMap* trapMap_ = nullptr;
};

// ---------------------------------------------------------------------------
// Lumiere du jour, pour le rendu
// ---------------------------------------------------------------------------
// PHASE 23 : CORRECTION DE L'INCOHERENCE DE LA PHASE 16.
//
// Le jeu simulait une horloge acceleree douze fois depuis la Phase 16, et rien
// dans l'image ne bougeait avec elle : la lumiere directionnelle des shaders
// etait la constante vec3(-0.4, -1.0, -0.6) depuis la Phase 2. Une session
// d'une heure traversait une tombee de nuit qui ne se voyait nulle part.
//
// Ce qui suit alimente la lumiere directionnelle EXISTANTE (Blinn-Phong,
// Phase 2) a partir de la position solaire deja calculee pour l'adret et les
// thermiques. Il n'y a donc toujours qu'UN SEUL soleil dans le jeu - c'est la
// meme exigence que pour la temperature, appliquee a la lumiere.
//
// Ce que ce n'est PAS : un ciel physique. Pas de diffusion atmospherique
// calculee, pas d'ombres portees, pas de lune. La direction et l'intensite
// suivent le cycle, c'est tout ce que le cahier des charges demandait.
struct SunLight {
    // Direction unitaire DU FRAGMENT VERS LA LUMIERE, en repere monde. Quand le
    // soleil est sous l'horizon elle bascule a la verticale : une lumiere
    // directionnelle venue d'en dessous eclairerait le relief par le bas, ce qui
    // est le defaut visuel classique d'un cycle jour/nuit bacle.
    glm::vec3 direction{0.0f, 1.0f, 0.0f};

    // Intensite de la lumiere DIRECTE, dans [0, 1]. Nulle la nuit.
    float intensity = 1.0f;

    // Couleur de la lumiere. Chaude au zenith, orangee a l'horizon, bleutee la
    // nuit.
    glm::vec3 color{1.0f, 0.97f, 0.90f};

    // MULTIPLICATEUR de l'eclairage ambiant propre a chaque shader, dans
    // [nightAmbientFactor, 1]. Un multiplicateur et non une valeur absolue,
    // parce que les trois shaders ont des ambiantes differentes et justifiees
    // (0,15 pour le cube, 0,18 pour le terrain, 0,24 pour le feuillage, qui
    // laisse passer la lumiere) : les remplacer par une valeur unique aurait
    // ete une regression de rendu deguisee en correction de cycle.
    float ambient = 1.0f;

    // Couleur du CIEL, a poser dans la couleur d'effacement du renderer. Elle
    // suit sa propre palette et non celle de la lumiere : le ciel n'est pas
    // eclaire par le modele d'eclairage, c'est un fond.
    glm::vec3 skyColor{0.52f, 0.66f, 0.82f};
};

// Lumiere correspondant a un etat synoptique. Fonction PURE, comme tout le
// reste du module : la meme heure rend la meme lumiere, donc deux captures
// d'ecran au meme instant de jeu sont comparables - ce qui est la condition de
// la verification visuelle du projet depuis la Phase 12.
SunLight sunLightFor(const SynopticState& sky, const WeatherParams& params = {});

// ---------------------------------------------------------------------------
// PHASE 27 : la meteo VISIBLE
// ---------------------------------------------------------------------------
// Jusqu'ici, un jour de pluie a 95 % de nebulosite se rendait EXACTEMENT comme
// un jour de grand beau : ciel bleu vif, soleil franc. La meteo etait calculee,
// journalisee, consommee par les besoins et les activites, et invisible.
//
// Cette fonction est la SEULE traduction meteo -> image du jeu. Elle ne calcule
// aucune grandeur meteo : elle LIT l'etat synoptique et l'echantillon local
// deja produits par WeatherSystem, et les convertit en parametres de rendu.
// Fonction PURE, comme sunLightFor : la meme meteo rend la meme image, et elle
// se teste sans GPU.
struct WeatherVisuals {
    // Lumiere du jour, CORRIGEE de la nebulosite : rayonnement direct voile par
    // les nuages, ambiante renforcee par la diffusion, ciel grise. Par ciel
    // clair, identique bit a bit a sunLightFor().
    SunLight light;

    // Brouillard de distance : couleur (celle du ciel, pour que le relief se
    // fonde dans le fond) et coefficient d'extinction, en 1/m. Nul sans
    // precipitation.
    glm::vec3 fogColor{0.52f, 0.66f, 0.82f};
    float fogDensityPerMeter = 0.0f;

    // Precipitation a l'ecran : intensite dans [0, 1] (0 = rien a dessiner),
    // nature (neige ou pluie) et vitesse apparente de chute en repere monde,
    // en m/s (vent local + chute verticale).
    float precipitation = 0.0f;
    bool snow = false;
    glm::vec3 fallVelocity{0.0f, -6.5f, 0.0f};

    // Assombrissement de l'albedo du sol par la mouillure, dans [0, 1].
    float groundDarkening = 0.0f;
};

WeatherVisuals weatherVisualsFor(const SynopticState& sky, const WeatherSample& local,
                                 const WeatherParams& params = {});

// ---------------------------------------------------------------------------
// Refroidissement eolien
// ---------------------------------------------------------------------------
// Formule JAG/TI (Joint Action Group for Temperature Indices), celle qu'emploient
// Environnement Canada et le NWS depuis 2001 :
//
//   Tressenti = 13,12 + 0,6215 T - 11,37 V^0,16 + 0,3965 T V^0,16
//
// avec T en degres Celsius et V la vitesse du vent en km/h a 10 m.
//
// Son domaine de validite est T <= 10 degres et V >= 4,8 km/h. HORS de ce
// domaine, la fonction rend la temperature INCHANGEE plutot qu'une
// extrapolation : au-dessus de 10 degres la formule diverge (elle rendrait un
// vent RECHAUFFANT a 25 degres, ce qui est faux), et sous 4,8 km/h elle rend un
// ressenti superieur a l'ambiante. C'est la seule formule de tout ce fichier qui
// ne soit PAS provisoire - elle est publiee et sourcable.
float windChillC(float temperatureC, float windMps);

} // namespace vercora
