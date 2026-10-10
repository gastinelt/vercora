#include "assets/Heightmap.hpp"
#include "assets/ObjLoader.hpp"
#include "assets/TerrainFile.hpp"
#include "assets/TextureFile.hpp"
#include "core/Camera.hpp"
#include "core/Frustum.hpp"
#include "core/FpsCounter.hpp"
#include "core/Logger.hpp"
#include "core/CompositeCollisionSolver.hpp"
#include "core/Window.hpp"
#include "editor/AssetCatalog.hpp"
#include "editor/EditorController.hpp"
#include "core/Paths.hpp"
#include "editor/EditorScene.hpp"
#include "editor/PlacedObject.hpp"
#include "editor/SceneFile.hpp"
#include "renderer/CubeMesh.hpp"
#include "renderer/DrawItem.hpp"
#include "renderer/MeshData.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/TerrainMesh.hpp"
#include "renderer/TerrainStreamer.hpp"
#include "renderer/VulkanContext.hpp"
#include "world/AnimalMesh.hpp"
#include "world/Forest.hpp"
#include "world/Habitat.hpp"
#include "world/GrassMesh.hpp"
#include "world/Ground.hpp"
#include "world/GroundItem.hpp"
#include "world/Inventory.hpp"
#include "world/Knapping.hpp"
#include "world/Crafting.hpp"
#include "world/CraftMesh.hpp"
#include "world/Bike.hpp"
#include "world/Climber.hpp"
#include "world/Needs.hpp"
#include "world/Weather.hpp"
#include "world/WaterSource.hpp"
#include "world/Deposits.hpp"
#include "world/FiberPlants.hpp"
#include "world/RockMesh.hpp"
#include "world/Rocks.hpp"
#include "world/BushMesh.hpp"
#include "world/Bushes.hpp"
#include "world/HarvestMesh.hpp"
#include "world/Resin.hpp"
#include "world/Paraglider.hpp"
#include "world/StoneMesh.hpp"
#include "world/Tree.hpp"
#include "world/TreeMesh.hpp"
#include "world/Vegetation.hpp"
#include "world/Wildlife.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace vercora;

namespace {

// ---------------------------------------------------------------------------
// Scene affichee : le seul endroit a changer pour tester autre chose.
// ---------------------------------------------------------------------------
// Sans argument, on affiche la dalle LiDAR HD REELLE (Phase 7), peuplee des
// arbres de la Phase 5 et des pierres de la Phase 6. Les scenes des phases
// precedentes restent accessibles sans recompiler :
//   vercora.exe                          -> dalle LiDAR reelle (defaut)
//   vercora.exe terrain                  -> terrain synthetique (Phase 4)
//   vercora.exe chemin\vers\dalle.vchm   -> une autre heightmap reelle
//   vercora.exe cube                     -> cube code en dur (Phase 1)
//   vercora.exe chemin\vers\modele.obj   -> mesh charge depuis un fichier
//
// Plus un mode outil, qui n'ouvre aucune fenetre (voir dumpWorld plus bas) :
//   vercora.exe --dump-world [scene]     -> imprime le monde seme, puis sort
//
// Plus une option de comparaison, ajoutee en Phase 10 :
//   vercora.exe [scene] --no-lod         -> terrain d'un SEUL bloc, sans
//                                           quadtree, LOD ni culling : le
//                                           comportement exact de la Phase 9.
// Elle n'existe que pour mesurer l'avant/apres sur la meme scene et la meme
// position de camera. Sans elle, toute scene de terrain passe par le quadtree.
//
// Plus trois options de la Phase 12, autour du streaming des tuiles :
//   vercora.exe [scene] --no-tile-fallback   -> pas de repli sur l'ancetre : une
//                                               tuile pas encore construite
//                                               laisse un TROU (comportement de
//                                               la Phase 10).
//   vercora.exe [scene] --tile-budget=N      -> N tuiles construites au plus par
//                                               frame (defaut 64 ; 4 reproduit
//                                               le plafond de la Phase 10).
//   vercora.exe [scene] --tile-threads=N     -> degre de parallelisme du MAILLAGE
//                                               CPU des tuiles, thread de rendu
//                                               compris (defaut 4). 1 remet ce
//                                               maillage entierement sequentiel,
//                                               c'est-a-dire dans l'etat d'avant
//                                               la Phase 19 : c'est le TEMOIN de
//                                               la parallelisation. Aucun appel
//                                               Vulkan ne change de thread quelle
//                                               que soit la valeur.
//   vercora.exe [scene] --flythrough=V[,H]   -> survol SCRIPTE : la camera avance
//                                               toute seule a V m/s, H metres
//                                               au-dessus du relief (defaut 120),
//                                               et tourne de 90 deg en arrivant
//                                               au bord de l'emprise.
// Les trois servent au meme controle : reproduire, mesurer et comparer le cas
// qui produisait des trous a l'horizon en deplacement rapide, sans recompiler
// entre les deux moities de la comparaison.
//
// Plus quatre options de la Phase 15, autour de l'EDITEUR DE NIVEAU :
//   vercora.exe [scene] --editor              -> demarre directement en mode
//                                                edition (sinon la touche F2)
//   vercora.exe [scene] --scene=chemin.vcsc   -> fichier de scene explicite, au
//                                                lieu de la convention
//                                                assets/scenes/<terrain>.vcsc
//   vercora.exe [scene] --no-scene            -> ni catalogue, ni objets poses,
//                                                ni mode editeur : le
//                                                comportement exact d'avant la
//                                                Phase 15, garde pour la
//                                                comparaison et la mesure
//   vercora.exe [scene] --editor-assets=rep   -> dossier de modeles a scanner,
//                                                au lieu de assets/editor
// Le mode editeur est un outil de DEVELOPPEMENT : il n'a pas d'interface
// graphique, sa liste d'assets et son etat s'affichent dans le journal. Les
// objets poses, eux, font partie du monde et sont affiches et solides dans le
// jeu normal - c'est justement le point.
//
// Le terrain synthetique n'est PAS supprime, et pour deux raisons distinctes :
// il reste la scene de reference des tests (monde deterministe, arbres a
// positions connues, cf. dumpWorld plus bas), et il sert de repli si le
// fichier de dalle est absent - le depot ne versionne pas les 4 Mo de la
// heightmap, encore moins les 167 Mo du .laz d'origine.
//
// ASSETS_DIR est defini par CMake et pointe sur le dossier assets/ a la racine
// du projet : l'executable trouve donc ses assets quel que soit le repertoire
// courant depuis lequel on le lance.
constexpr const char* kProceduralKeyword = "terrain";
constexpr const char* kBuiltinCubeKeyword = "cube";

// Dalle chargee par defaut. Produite par :
//   python tools/lidar_to_heightmap.py \
//       data/lidar/LHD_FXX_0907_6440_PTS_LAMB93_IGN69.copc.laz \
//       -o assets/terrain/LHD_0907_6440_1m.vchm --cell 1.0
// Voir NOTES-PHASE7.md pour la provenance de la dalle et le detail du choix de
// resolution.
// PHASE 21 : le prefixe n'est plus la macro ASSETS_DIR mais assetsDir(), qui
// prefere le dossier assets\ pose a cote de l'executable quand il existe. Sur
// cette machine, rien ne change ; dans le package distribue, tout en depend.
// Voir core/Paths.hpp.
const std::string kDefaultTerrainFile = assetsDir() + "/terrain/LHD_0907_6440_1m.vchm";

// PHASE 21 : la dalle chargee par defaut EN MODE DEMO (--demo). La demo montre
// la zone de 25 km2 - c'est tout son propos - alors que le defaut de
// developpement reste la dalle unique de la Phase 7, qui charge en une seconde.
// Produite par tools/assemble_terrain.py, voir NOTES-PHASE9.md.
const std::string kDemoTerrainFile = assetsDir() + "/terrain/vercora_zone_1m.vchm";

// Repli du mode demo si la dalle 1 m est absente du package : meme zone, pas de
// 2 m, quatre fois plus legere (25 Mo contre 100). Voir NOTES-PHASE21.md.
const std::string kDemoTerrainFallback = assetsDir() + "/terrain/vercora_zone_2m.vchm";

// Portee de la visee, en unites monde (= metres). Au-dela, on considere que le
// joueur ne peut pas atteindre l'arbre avec un outil a main.
constexpr float kFellingReach = 40.0f;

// Portee du ramassage, mesuree a l'horizontale. Un peu plus qu'un bras tendu :
// il faut pouvoir attraper une buche sans se placer au centimetre pres.
constexpr float kPickupReach = 2.5f;

// Nombre de pierres semees. Il en faut DEUX pour percuter, mais n'en semer que
// deux obligerait a quadriller la carte pour trouver la seconde. Six laisse une
// chance raisonnable d'en croiser deux en explorant, sans transformer la
// montagne en carriere.
constexpr uint32_t kStoneCount = 6;

// Touches d'action. GLFW raisonne en POSITIONS PHYSIQUES facon QWERTY US : sur
// un clavier AZERTY francais, ces trois tokens tombent bien sur les touches
// gravees E, R et C (contrairement a W/A qui, eux, donnent Z/Q - voir Camera).
constexpr int kFellKey = GLFW_KEY_E;    // abattre l'arbre vise
constexpr int kPickupKey = GLFW_KEY_R;  // ramasser l'objet au sol le plus proche
constexpr int kKnapKey = GLFW_KEY_C;    // percuter deux pierres ; PHASE 28 : faconner (toutes recettes)
// PHASE 28. Reposer au sol un objet porte (le dernier ramasse s'il en reste).
// P comme "poser" ; P occupe la MEME position physique en AZERTY et en QWERTY
// (rangee du haut ...-O-P), comme E/R/T/Y/U/I. "R maintenu", propose par le
// document de conception, aurait heurte le ramassage, declenche a l'APPUI.
constexpr int kPutDownKey = GLFW_KEY_P;
constexpr int kParaglideKey = GLFW_KEY_V; // decoller/atterrir en parapente

// PHASE 13. Monter sur le velo / en descendre.
//
// Choix de la touche : B comme "bike"/"velo", et surtout B est l'une des rares
// lettres qui occupe la MEME position physique en AZERTY et en QWERTY (rangee du
// bas : W-X-C-V-B en AZERTY, Z-X-C-V-B en QWERTY). Le token GLFW_KEY_B tombe
// donc sur la touche gravee B des deux cotes, sans le decalage qui touche W/A -
// voir la note clavier de core/Camera.hpp. Elle est aussi juste a cote de V, la
// touche du parapente : les deux activites montees se declenchent du meme doigt.
constexpr int kBikeKey = GLFW_KEY_B;      // enfourcher le velo / en descendre

// PHASE 14. S'engager sur une paroi / lacher.
//
// Meme raisonnement que pour B : X occupe la MEME position physique en AZERTY et
// en QWERTY (rangee du bas W-X-C-V-B / Z-X-C-V-B), donc le token GLFW_KEY_X
// tombe sur la touche gravee X des deux cotes. Les quatre activites et les deux
// actions d'outillage occupent ainsi une seule rangee contigue sous la main
// gauche : X escalade, C percussion, V parapente, B velo.
// PHASE 25. Intensite MAITRESSE de l'indice visuel adret/ubac, dans [0, 1].
//
// 1,0 = les teintes de shaders/terrain.frag sont appliquees en plein, ce qui
// represente au plus 7 % par canal et environ 13 % d'ecart total entre un adret
// et un ubac vus cote a cote. C'est le reglage retenu : en dessous, l'indice
// cesse d'etre perceptible sur une orthophoto, ce qui lui ferait manquer sa
// seule raison d'etre.
//
// Nommee ici plutot qu'ecrite en dur dans l'appel : c'est le seul bouton a
// tourner si l'effet devait etre juge trop marque, et il doit se trouver sans
// lire le shader.
constexpr float kAspectTintIntensity = 1.0f;

// PHASE 27, chantier 3 : micro-relief du sol vu de pres. Voir shaders/
// terrain.frag et NOTES-PHASE27.md. Force a 1 = pentes de grain de 0,6 et
// couleur +/- 18 % sur le sol le plus rugueux (plancher de 35 % sur le sol
// lisse). Le motif de base est le texel de l'orthophoto quand il est connu,
// 0,61 m sinon.
constexpr float kGroundDetailStrength = 1.0f;
constexpr float kGroundDetailFadeStart = 5.0f;
constexpr float kGroundDetailFadeEnd = 20.0f;
constexpr float kGroundDetailDefaultPattern = 0.61f;

// Valeur de WeatherParams::aspectIncidenceReference que shaders/terrain.frag
// code en dur (kAspectIncidenceReference). Un shader ne peut pas lire un
// parametre C++ : la seule protection contre une divergence est de la rendre
// BRUYANTE. Verifiee une fois au demarrage.
constexpr float kShaderAspectIncidenceReference = 0.35f;

constexpr int kClimbKey = GLFW_KEY_X;     // s'engager sur une paroi / lacher

// PHASE 16. Boire au point d'eau le plus proche.
//
// Meme raisonnement de clavier que pour B et X : la rangee du haut est
// A-Z-E-R-T-Y en AZERTY et Q-W-E-R-T-Y en QWERTY, donc E, R, T et Y y occupent
// la MEME position physique des deux cotes. Le token GLFW_KEY_T tombe sur la
// touche gravee T partout, et elle est voisine de R (ramasser) : les deux
// gestes qui consomment le monde sont sous le meme doigt.
constexpr int kDrinkKey = GLFW_KEY_T;     // boire au point d'eau

// PHASE 22. Les trois gestes de recolte ajoutes par cette phase.
//
// MEME raisonnement de clavier que pour B, X et T, et il finit de se payer ici :
// la rangee du haut est A-Z-E-R-T-Y-U-I-O-P en AZERTY et Q-W-E-R-T-Y-U-I-O-P en
// QWERTY, donc TOUT ce qui suit le Z/W y occupe la meme position physique. Les
// six gestes du joueur SUR LE MONDE tiennent donc desormais dans une seule
// rangee contigue, dans l'ordre ou on les apprend :
//
//   E abattre  R ramasser  T boire  Y creuser  U saigner  I arracher
//
// Ce n'est pas une coquetterie : c'est ce qui evite d'aller chercher des lettres
// decalees entre les deux dispositions (le piege qui avait fait croire pendant
// tout un run que le jeu ignorait W - voir la note clavier de core/Camera.hpp).
constexpr int kDigKey   = GLFW_KEY_Y; // creuser un gisement d'argile ou de sable
constexpr int kResinKey = GLFW_KEY_U; // saigner le resineux vise, SANS l'abattre
constexpr int kFiberKey = GLFW_KEY_I; // arracher la plante a fibres a ses pieds

// Vitesse de marche de reference, en m/s. Nommee depuis la Phase 16 parce que
// la vitesse effective en est desormais une FRACTION (voir Needs::movementFactor)
// et qu'elle est reappliquee a chaque image : la valeur de reference ne peut
// donc plus vivre en dur dans l'appel a setMoveSpeed.
constexpr float kWalkSpeed = 14.0f;

// En dessous de cette vitesse horizontale (m/s), un joueur A PIED est considere
// immobile, donc AU REPOS - le seul etat qui refait la forme. Elle est lue sur
// le deplacement reel de la camera entre deux images et non sur les touches :
// un joueur qui pousse contre un rocher tient sa touche mais n'avance pas, et
// il ne doit pas recuperer pour autant. 0,20 m/s, soit 1,4 % de kWalkSpeed :
// assez bas pour qu'un arret soit franc, assez haut pour absorber le residu
// numerique d'une camera collee a un obstacle.
constexpr float kRestSpeedThreshold = 0.20f;
constexpr int kOrthoKey = GLFW_KEY_O;     // orthophoto <-> coloration procedurale
constexpr int kVegetationKey = GLFW_KEY_G; // vegetation de decor : tout / arbres / rien

// ---------------------------------------------------------------------------
// PHASE 15 : les touches du MODE EDITEUR
// ---------------------------------------------------------------------------
// Elles ne sont lues QUE dans ce mode, et le mode desactive en retour toutes les
// touches de jeu (abattage, ramassage, percussion, les quatre activites, la
// bascule de vegetation). Les deux jeux de touches ne se marchent donc jamais
// dessus, ce qui laisse le champ libre pour reprendre ici des lettres deja
// prises ailleurs - E designe un arbre a abattre en jeu et selectionne un objet
// en edition, sans ambiguite possible.
//
// CHOIX DES TOUCHES. Le jeu doit rester utilisable en AZERTY comme en QWERTY, et
// GLFW raisonne en positions physiques facon QWERTY US (voir la note clavier de
// core/Camera.hpp). Les touches de reglage sont donc prises parmi celles qui
// occupent la MEME position physique sur les deux dispositions : les fleches,
// le pave de navigation (Inser/Suppr/Debut/Fin/Page), la rangee de fonctions,
// Tab, Retour arriere, Echap et la rangee des chiffres. Les seules lettres
// utilisees - E, G, K - sont a la meme place sur les deux.
constexpr int kEditorToggleKey = GLFW_KEY_F2;   // entrer / sortir du mode edition

// Equivalent CLAVIER du clic gauche : poser l'objet arme / reposer l'objet en
// main. Deux raisons, la seconde etant decisive :
//   - a l'usage, on regle le cap et l'echelle aux fleches puis on pose, sans
//     avoir a ramener la main sur la souris ;
//   - le harnais automatise ne peut piloter que le CLAVIER. Il pilote la fenetre
//     par PostMessage, or GLFW lit la position du curseur avec GetCursorPos(),
//     c'est-a-dire la souris SYSTEME - un WM_MOUSEMOVE poste n'a aucun effet
//     (voir l'en-tete de tools/run_vulkan_check.ps1). Sans cette touche, la pose
//     d'objet serait la seule fonction de l'editeur qu'aucun test ne pourrait
//     exercer sous Vulkan.
constexpr int kEditorPlaceKey   = GLFW_KEY_INSERT;     // poser (equivalent du clic)
constexpr int kEditorPickKey    = GLFW_KEY_E;          // selectionner l'objet vise
constexpr int kEditorGrabKey    = GLFW_KEY_G;          // saisir / reposer
constexpr int kEditorSolidKey   = GLFW_KEY_K;          // solide <-> traversable
constexpr int kEditorDeleteKey  = GLFW_KEY_DELETE;     // supprimer la selection
constexpr int kEditorCancelKey  = GLFW_KEY_ESCAPE;     // abandonner saisie/selection
constexpr int kEditorNextKey    = GLFW_KEY_TAB;        // asset suivant
constexpr int kEditorNextKey2   = GLFW_KEY_PAGE_DOWN;  // idem, sous la main droite
constexpr int kEditorPrevKey    = GLFW_KEY_PAGE_UP;    // asset precedent
constexpr int kEditorUndoKey    = GLFW_KEY_BACKSPACE;  // annuler la derniere action
constexpr int kEditorSaveKey    = GLFW_KEY_F5;         // ecrire le fichier de scene
constexpr int kEditorReloadKey  = GLFW_KEY_F9;         // relire le fichier de scene
constexpr int kEditorListKey    = GLFW_KEY_L;          // reafficher catalogue + etat
constexpr int kEditorFineKey    = GLFW_KEY_LEFT_CONTROL; // pas fin sur les reglages

// Selectionner l'objet POSE suivant/precedent sans viser (ergonomie ajoutee
// apres la session initiale de la Phase 15, voir NOTES-PHASE15.md ss13).
// F3/F4 : libres, memes touches physiques en AZERTY et QWERTY (rangee de
// fonctions), et adjacentes a F2/F5 deja pris par l'editeur.
constexpr int kEditorCycleNextKey = GLFW_KEY_F3;
constexpr int kEditorCyclePrevKey = GLFW_KEY_F4;

// Vitesse de vol du mode editeur, en m/s, et ses bornes.
//
// 60 m/s par defaut : la zone fait 5 km de cote, donc une traversee complete
// prend ~83 s - assez rapide pour aller voir l'autre versant, assez lent pour
// poser quelque chose sans depasser. La molette la multiplie ou la divise, ce
// qui est le seul reglage vraiment necessaire : on alterne sans arret entre
// survol de reconnaissance et placement au metre pres.
constexpr float kEditorFlySpeed = 60.0f;
constexpr float kEditorFlySpeedMin = 2.0f;
constexpr float kEditorFlySpeedMax = 600.0f;
constexpr float kEditorFlySpeedStep = 1.25f; // par cran de molette

// Teintes de temoin, au format attendu par DrawItem::params pour le materiau
// Object : (force, rouge, vert, bleu). Voir shaders/cube.frag.
//
// Cyan pour le fantome, ambre pour la selection : deux couleurs qu'aucun asset
// de cette session ne porte (ils sont tous gris, bruns ou ocres) et que le
// terrain ne porte pas non plus. Une teinte verte se serait confondue avec la
// vegetation, une teinte grise avec le rocher.
const glm::vec4 kGhostTint{0.55f, 0.25f, 0.85f, 0.95f};
const glm::vec4 kSelectionTint{0.55f, 1.00f, 0.62f, 0.15f};

// Dossier scanne au demarrage de l'editeur, et dossier ou sont rangees les
// scenes. Tous deux sous assets/, donc VERSIONNES (contrairement a data/, que le
// .gitignore exclut) : une scene composee a la main est du contenu, au meme
// titre qu'un modele - la perdre couterait le temps de la refaire, alors qu'une
// heightmap se regenere en quatre secondes depuis le .laz.
const std::string kEditorAssetsDir = assetsDir() + "/editor";
const std::string kScenesDir = assetsDir() + "/scenes";

// Hauteur minimale au-dessus du terrain pour pouvoir decoller (unites monde =
// metres), verifiee EN MODE LIBRE. Sans ce seuil, "decoller" depuis un sol plat
// n'aurait aucun sens ; l'exiger evite aussi un vol qui commence deja pose (la
// toute premiere frame de checkLanding le terminerait aussitot).
constexpr float kMinLaunchHeightAboveTerrain = 5.0f;

// Distance et hauteur de la camera derriere le pilote en vol (chase-cam), en
// unites monde. Assez loin pour voir le pilote ET le relief qui vient, assez
// pres pour rester une vue a la troisieme personne lisible et non un plan large.
constexpr float kGlideChaseDistance = 6.0f;
constexpr float kGlideChaseHeight = 2.2f;

// PHASE 13. Meme chose pour le velo, en plus rapproche : le parapente se lit a
// l'echelle du relief traverse, le velo se lit a l'echelle du sentier - il faut
// voir ou passent les roues et comment la machine s'incline, pas un panorama.
constexpr float kRideChaseDistance = 5.0f;
constexpr float kRideChaseHeight = 2.0f;

// Garde au sol MINIMALE de la camera de poursuite du velo, en unites monde.
//
// Necessaire et pas seulement cosmetique : en descente, l'oeil recule DANS la
// pente qu'on vient de devaler, et sans ce plancher il se retrouve sous la
// surface - on voit alors l'interieur du terrain. Le parapente n'a pas ce
// probleme (il est en l'air), c'est pour ca que la contrainte n'existe qu'ici.
constexpr float kRideChaseMinClearance = 1.2f;

// Front descendant d'une touche.
//
// Toutes les actions du jeu en ont besoin, et pour la meme raison : sans lui,
// maintenir la touche une demi-seconde declencherait l'action a chaque frame -
// soit une soixantaine d'abattages, ou l'inventaire vide d'un coup. La Phase 5
// gerait ca avec un booleen nomme a la main par action ; les regrouper ici evite
// d'en oublier un au prochain ajout.
class KeyEdge {
public:
    bool justPressed(const Window& window, int key) {
        return justPressed(window.isKeyPressed(key));
    }

    // PHASE 15 : meme mecanique sur un etat DEJA LU. Le clic gauche de l'editeur
    // en a besoin (il vient de isMouseButtonPressed, pas de isKeyPressed), et il
    // en a besoin pour exactement la meme raison que les touches : sans front
    // descendant, maintenir le bouton une demi-seconde poserait une soixantaine
    // de rochers empiles au meme endroit - et soixante annulations a defaire.
    bool justPressed(bool down) {
        const bool edge = down && !wasDown_;
        wasDown_ = down;
        return edge;
    }

private:
    bool wasDown_ = false;
};

// ---------------------------------------------------------------------------
// PHASE 14 : fin d'une escalade
// ---------------------------------------------------------------------------
// Les trois sorties d'escalade (sommet, lachage, epuisement) se produisent a
// DEUX endroits du code - la touche X pour le lachage volontaire, Climber::update
// pour les deux autres - mais elles ont exactement les memes suites : reposer le
// joueur et revenir en marche. Les regrouper ici est ce qui garantit qu'on ne
// puisse pas reparer l'une en oubliant l'autre.
//
// Climber a deja place son point d'accroche a la bonne position avant de rendre
// la main : au sommet pour ToppedOut, au pied de la paroi pour Released et
// Exhausted. Il n'y a donc PAS de choix de position a refaire ici - une seule
// reinitialisation existe, dans world/Climber, et c'est elle que les tests
// headless verifient.
//
// On passe par anchor() et non par position() : anchor() est le point SUR la
// surface, alors que position() est l'oeil du grimpeur, decale de la paroi.
// Camera::setMode(Walk) reposera la camera a la bonne hauteur d'oeil tout seul
// (snapToTerrain), donc seuls X et Z comptent vraiment.
void handleClimbExit(ClimbExit exit, const Climber& climber, Camera& camera) {
    if (exit == ClimbExit::None) {
        return;
    }

    camera.setPosition(climber.anchor());
    camera.setMode(CameraMode::Walk);

    switch (exit) {
        case ClimbExit::ToppedOut:
            logInfo("Escalade : SOMMET atteint, " +
                    std::to_string(climber.heightGained()) +
                    " m gagnes. Energie restante " +
                    std::to_string(100.0f * climber.stamina()) + " %.");
            break;
        case ClimbExit::Released:
            logInfo("Escalade : lachee volontairement, retour au pied de la paroi. "
                    "Energie restante " +
                    std::to_string(100.0f * climber.stamina()) + " %.");
            break;
        case ClimbExit::Exhausted:
            logInfo("Escalade : CHUTE, energie epuisee. Retour au pied de la "
                    "paroi (aucun degat : hors scope de cette session).");
            break;
        case ClimbExit::None:
            break;
    }
}

// ---------------------------------------------------------------------------
// PHASE 11 : orthophoto
// ---------------------------------------------------------------------------
// Chemin .vcto associe par CONVENTION a un terrain .vchm :
//   assets/terrain/vercora_zone_1m.vchm -> assets/terrain/vercora_zone_ortho.vcto
// Le suffixe de resolution est retire, parce qu'une meme zone photographiee une
// fois sert au terrain a 1 m comme a celui a 2 m : ce sont deux echantillonnages
// du meme relief, pas deux endroits.
std::string orthoPathFor(const std::string& terrainPath) {
    std::string stem = terrainPath;
    const std::size_t dot = stem.rfind('.');
    if (dot != std::string::npos) {
        stem.resize(dot);
    }
    for (const char* suffix : { "_1m", "_2m", "_5m" }) {
        const std::size_t length = std::strlen(suffix);
        if (stem.size() > length && stem.compare(stem.size() - length, length, suffix) == 0) {
            stem.resize(stem.size() - length);
            break;
        }
    }
    return stem + "_ortho.vcto";
}

// Meme convention pour les cartes de vegetation produites par
// tools/lidar_to_canopy.py :
//   assets/terrain/vercora_zone_1m.vchm -> ..._canopy.vchm et ..._canopy_height.vchm
std::string canopyPathFor(const std::string& terrainPath, const std::string& suffix) {
    std::string path = orthoPathFor(terrainPath);
    // orthoPathFor a deja retire l'extension et le suffixe de resolution ; il ne
    // reste qu'a remplacer sa terminaison.
    const std::string tail = "_ortho.vcto";
    path.resize(path.size() - tail.size());
    return path + "_canopy" + suffix + ".vchm";
}

// Refuse une texture qui ne recouvre pas EXACTEMENT le terrain charge.
//
// C'est le controle qui manquait a la Phase 7 : sans lui, associer par megarde
// la photo d'une zone a le relief d'une autre produit un rendu parfaitement
// plausible - des couleurs de montagne sur de la montagne - que rien ne
// signale. La tolerance est d'un demi-metre, soit moins d'un texel : les deux
// emprises viennent du meme en-tete .vchm, elles doivent coincider au bit pres
// ou presque.
bool orthoMatchesTerrain(const TextureData& texture, const Heightmap& heightmap) {
    if (texture.epsg != heightmap.geo().epsg) {
        logWarn("Orthophoto : projection " + std::to_string(texture.epsg) +
                " contre " + std::to_string(heightmap.geo().epsg) +
                " pour le terrain. Texture ignoree.");
        return false;
    }

    const double spacing = static_cast<double>(heightmap.spacing());
    const double span = (heightmap.resolution() - 1) * spacing;
    const double eastMin = heightmap.geo().originEast;
    const double northMax = heightmap.geo().originNorth;

    const double tolerance = 0.5;
    const bool ok =
        std::abs(texture.eastMin - eastMin) < tolerance &&
        std::abs(texture.eastMax - (eastMin + span)) < tolerance &&
        std::abs(texture.northMax - northMax) < tolerance &&
        std::abs(texture.northMin - (northMax - span)) < tolerance;

    if (!ok) {
        logWarn("Orthophoto : emprise E " + std::to_string(texture.eastMin) + ".." +
                std::to_string(texture.eastMax) + " N " + std::to_string(texture.northMin) +
                ".." + std::to_string(texture.northMax) +
                ", alors que le terrain couvre E " + std::to_string(eastMin) + ".." +
                std::to_string(eastMin + span) + " N " + std::to_string(northMax - span) +
                ".." + std::to_string(northMax) + ". Texture ignoree.");
    }
    return ok;
}

// Choisit et construit le terrain a partir de l'argument de ligne de commande.
// Renvoie nullptr si l'argument ne designe pas une scene de terrain (cube, .obj).
//
// Le REPLI sur le terrain synthetique n'est fait que pour la dalle par DEFAUT.
// Un chemin donne explicitement qui n'existe pas est une erreur, pas une
// occasion de silencieusement afficher autre chose : demander une dalle precise
// et obtenir du bruit fractal sans s'en apercevoir serait le pire des deux
// mondes.
std::unique_ptr<Heightmap> makeTerrain(const std::string& sceneArgument,
                                       bool useDefaultTerrain, bool demoMode) {
    if (sceneArgument == kProceduralKeyword) {
        logInfo("Terrain : generateur procedural (Phase 4), demande explicitement.");
        return std::make_unique<Heightmap>();
    }

    if (useDefaultTerrain) {
        // PHASE 21 : la demo montre la zone de 25 km2 (c'est tout son propos),
        // pas la dalle unique de developpement. Repli en cascade sur la version
        // 2 m si la 1 m est absente du package (voir NOTES-PHASE21.md) - les
        // deux couvrent la meme emprise, seule la resolution differe.
        if (demoMode) {
            if (std::filesystem::exists(kDemoTerrainFile)) {
                return std::make_unique<Heightmap>(loadTerrainFile(kDemoTerrainFile));
            }
            if (std::filesystem::exists(kDemoTerrainFallback)) {
                logWarn("Terrain : dalle demo 1 m absente, repli sur la 2 m (" +
                        kDemoTerrainFallback + ").");
                return std::make_unique<Heightmap>(loadTerrainFile(kDemoTerrainFallback));
            }
            logWarn("Terrain : ni " + kDemoTerrainFile + " ni " + kDemoTerrainFallback +
                    " ne sont presents. Repli sur le terrain synthetique.");
            return std::make_unique<Heightmap>();
        }
        if (!std::filesystem::exists(kDefaultTerrainFile)) {
            logWarn(std::string("Terrain : dalle LiDAR absente (") + kDefaultTerrainFile +
                    "). Repli sur le terrain synthetique. Pour la produire :");
            logWarn("  python tools/lidar_to_heightmap.py "
                    "data/lidar/<dalle>.copc.laz -o " + std::string(kDefaultTerrainFile) +
                    " --cell 1.0");
            return std::make_unique<Heightmap>();
        }
        return std::make_unique<Heightmap>(loadTerrainFile(kDefaultTerrainFile));
    }

    if (sceneArgument.size() > 5 &&
        sceneArgument.compare(sceneArgument.size() - 5, 5, ".vchm") == 0) {
        return std::make_unique<Heightmap>(loadTerrainFile(sceneArgument));
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Mode --dump-world : imprime le monde seme, puis sort. Aucune fenetre, aucun
// Vulkan.
// ---------------------------------------------------------------------------
// Sert au pilotage de tools\run_vulkan_check.ps1 : piloter le jeu a l'aveugle ne
// prouve rien - on appuie sur E sans jamais viser un arbre, sur R sans rien
// avoir sous la main. En connaissant le monde, le script vise de vraies cibles.
//
// C'ETAIT un programme separe (tools\dump_world.cpp) jusqu'a la Phase 7, qui
// RECOPIAIT la construction du monde de main.cpp en comptant sur le fait que les
// deux resteraient dans le meme ordre - le semis etant deterministe, un ordre
// different aurait donne un monde different, et le script aurait vise a cote
// sans que rien ne le signale. Le replier ici supprime cette duplication : le
// dump traverse EXACTEMENT le code du jeu, par construction.
//
// (Le declencheur immediat etait plus terre a terre : Smart App Control, actif
// sur cette machine, bloque a l'execution les binaires non signes fraichement
// compiles, verdict par empreinte et donc imprevisible. vercora.exe, lui, passe.
// Une raison de moins d'avoir un second executable.)
//
// Format, une ligne par objet :
//     KIND x z distance capHorizontaleDegres
// puis, pour chaque arbre reellement abattable au cap initial :
//     AIM indice camX camZ recul
void dumpWorld(const Heightmap& heightmap, const Forest& forest, const Ground& ground) {
    struct Entry {
        std::string kind;
        float x = 0.0f;
        float z = 0.0f;
        float distance = 0.0f;
        float bearing = 0.0f;
    };
    std::vector<Entry> entries;

    // L'INDICE est celui que le jeu journalise ("Abattage de l'arbre 8") : c'est
    // ce qui permet de verifier apres coup qu'on a bien abattu l'arbre VISE, et
    // donc que le pilotage de la camera est precis.
    for (std::size_t i = 0; i < forest.trees().size(); ++i) {
        entries.push_back({ "TREE" + std::to_string(i),
                            forest.trees()[i].base.x, forest.trees()[i].base.z });
    }
    for (const GroundItem& item : ground.items()) {
        entries.push_back({ groundItemLabel(item.kind), item.position.x, item.position.z });
    }

    for (Entry& entry : entries) {
        entry.distance = std::sqrt(entry.x * entry.x + entry.z * entry.z);
        // Convention de Camera::updateVectors : front = (cos(yaw), *, sin(yaw)),
        // yaw en degres. Pour regarder un point depuis l'origine il faut donc
        // yaw = atan2(z, x).
        entry.bearing = glm::degrees(std::atan2(entry.z, entry.x));
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.distance < b.distance; });

    for (const Entry& entry : entries) {
        std::printf("%s %.3f %.3f %.3f %.3f\n", entry.kind.c_str(), entry.x, entry.z,
                    entry.distance, entry.bearing);
    }

    // --- Reculs d'ou un arbre est REELLEMENT visible ------------------------
    // Corrige en Phase 8. Le dump visait auparavant avec un rayon HORIZONTAL au
    // cap initial (front = (0, 0, -1), pitch nul) : sur le terrain synthetique,
    // assez lisse pour qu'un arbre a 9 m au sud reste toujours dans l'axe, ca
    // donnait 16 arbres abattables sur 16. Sur le relief mesure, un arbre en
    // contrebas passait entierement sous ce rayon et un arbre en amont
    // entierement au-dessus : 6 sur 16 seulement, alors que le jeu, lui, vise
    // avec camera.front() qui inclut le pitch (Camera::updateVectors, front.y =
    // sin(pitch)) - voir main.cpp, la boucle de gameplay qui appelle pickTree.
    // Ce n'etait donc pas un bug de visee, seulement un rayon de TEST trop
    // simple pour representer ce qu'un joueur fait reellement en inclinant la
    // camera vers le haut ou le bas.
    //
    // Le dump vise maintenant le TRONC de l'arbre (mi-hauteur) depuis l'oeil,
    // direction = normalize(cible - oeil) : c'est exactement le rayon qu'un
    // joueur obtient en pointant la souris sur l'arbre, pitch inclus. Le pitch
    // requis est journalise pour distinguer les trois cas demandes (arbre
    // au-dessus, en dessous, au meme niveau que l'oeil).
    for (std::size_t i = 0; i < forest.trees().size(); ++i) {
        const Tree& tree = forest.trees()[i];
        const glm::vec3 aimPoint = tree.base + glm::vec3(0.0f, tree.height * 0.5f, 0.0f);

        // Le recul doit rester assez grand pour ne pas etre DANS le houppier, et
        // assez petit pour que la derive du pilotage a l'estime reste sous la
        // tolerance de visee.
        for (float standoff = 6.0f; standoff <= 16.0f; standoff += 0.5f) {
            const float camX = tree.base.x;
            const float camZ = tree.base.z + standoff; // plein sud de l'arbre
            const glm::vec3 eye(camX,
                                heightmap.heightAt(camX, camZ) + kDefaultEyeHeight,
                                camZ);
            const glm::vec3 toAim = aimPoint - eye;
            const float horizontal = std::sqrt(toAim.x * toAim.x + toAim.z * toAim.z);
            const float pitchDeg = glm::degrees(std::atan2(toAim.y, horizontal));

            // Meme limite que Camera::update (std::clamp(pitch_, -89, 89)) : un
            // pitch que le joueur ne peut pas physiquement atteindre ne compte
            // pas comme "visable".
            if (std::abs(pitchDeg) > 89.0f) {
                continue;
            }

            const glm::vec3 direction = glm::normalize(toAim);
            if (forest.pickTree(eye, direction, kFellingReach) == static_cast<int>(i)) {
                const char* elevation = pitchDeg > 2.0f    ? "au-dessus"
                                        : pitchDeg < -2.0f ? "en-dessous"
                                                           : "au-niveau";
                std::printf("AIM %zu %.3f %.3f %.3f %.2f %s\n", i, camX, camZ, standoff,
                            pitchDeg, elevation);
                break;
            }
        }
    }

    // --- Reculs abattables au pitch NUL, pour le tir reellement joue -----------
    // run_vulkan_check.ps1 ne peut pas incliner la camera pendant qu'il pilote le
    // vrai jeu : GLFW lit le curseur SYSTEME (glfwGetCursorPos -> GetCursorPos)
    // et non les messages postes, donc faire tourner la camera exigerait de
    // deplacer reellement le pointeur - voler la souris a l'utilisateur pendant
    // le test. Le pitch en jeu reste donc a 0 tout du long du scenario C. Le
    // script a besoin d'au moins une cible que ce rayon horizontal touche
    // reellement, faute de quoi il ne peut prouver l'abattage lui-meme (meme si
    // pickTree, verifie ci-dessus, sait viser en hauteur).
    for (std::size_t i = 0; i < forest.trees().size(); ++i) {
        const Tree& tree = forest.trees()[i];
        for (float standoff = 6.0f; standoff <= 16.0f; standoff += 0.5f) {
            const float camX = tree.base.x;
            const float camZ = tree.base.z + standoff;
            const glm::vec3 eye(camX,
                                heightmap.heightAt(camX, camZ) + kDefaultEyeHeight,
                                camZ);
            if (forest.pickTree(eye, glm::vec3(0.0f, 0.0f, -1.0f), kFellingReach) ==
                static_cast<int>(i)) {
                std::printf("AIMLEVEL %zu %.3f %.3f %.3f\n", i, camX, camZ, standoff);
                break;
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        // --dump-world doit venir en PREMIER, la scene eventuelle juste apres :
        //   vercora.exe --dump-world                  -> dalle par defaut
        //   vercora.exe --dump-world terrain          -> terrain synthetique
        //   vercora.exe --dump-world chemin\x.vchm    -> une autre dalle
        // Les options commencent par "--" et peuvent etre placees n'importe ou ;
        // le premier argument restant designe la scene. Un simple balayage
        // suffit : il y a deux options en tout, et une vraie bibliotheque
        // d'analyse d'arguments serait plus de code que le reste du fichier.
        bool dumpWorldOnly = false;
        // PHASE 21 : mode DEMO, pense pour un .exe envoye a quelqu'un qui n'a ni
        // le depot ni l'environnement de developpement (voir NOTES-PHASE21.md).
        // C'est un DRAPEAU DE LANCEMENT et non une configuration de compilation :
        // le binaire distribue est EXACTEMENT celui qu'on utilise pour
        // developper, en Release plutot qu'en Debug. Un flag runtime plutot
        // qu'une macro parce que la demo ne retranche aucun code, seulement de
        // l'INITIALISATION - la meme raison qui fait deja exister --no-wildlife
        // et --no-scene : une deuxieme configuration de build a maintenir en
        // parallele aurait un cout permanent pour un besoin ponctuel.
        //
        // Ce que --demo coupe, et pourquoi :
        //   - la faune (Wildlife/Habitat) et les besoins (Needs) : deux systemes
        //     de simulation qui tournent en continu sans rien ajouter a
        //     "se deplacer et regarder le terrain" ;
        //   - velo, parapente, escalade : trois activites montees, hors sujet ;
        //   - abattage, ramassage, percussion : l'interaction avec les arbres et
        //     les pierres reste du RENDU et de la COLLISION (la foret n'est pas
        //     retiree), seule la boucle d'artisanat est coupee ;
        //   - le mode editeur : c'est un outil de developpement, pas du contenu
        //     a montrer.
        // Restent : marche, vol libre (F), collision terrain/arbres, FPS.
        bool demoMode = false;
        bool useLod = true;
        bool useOrtho = true;
        bool useVegetation = true;
        bool useGrass = true;
        // PHASE 18 : temoin de la DECOUPE ALPHA des brins d'herbe. Rend les
        // quads opaques de la Phase 11 sans rien changer d'autre - ni la
        // densite, ni le maillage, ni le chemin de rendu - ce qui est la seule
        // facon de mesurer ce que la decoupe coute a elle seule.
        bool useGrassCutout = true;
        // PHASE 18 : temoin de l'accelaration de la construction de tuile.
        // Meme role que --no-grass-cutout : mesurer un changement sur le MEME
        // binaire et la meme trajectoire, la seule comparaison qui ait du sens.
        bool cacheTileWork = true;
        // PHASE 19 : 0 = laisser le defaut du TerrainStreamer (4). 1 remet le
        // maillage des tuiles entierement sequentiel, c'est-a-dire dans l'etat
        // d'avant la phase - TEMOIN de mesure, au meme titre que
        // --no-tile-cache.
        uint32_t tileThreads = 0;
        // PHASE 17 : --no-wildlife coupe la faune ET la memoire du milieu. Meme
        // role que --no-vegetation : c'est un TEMOIN. Sans lui, on ne peut pas
        // attribuer un ecart de FPS - ou un ecart de comportement dans un
        // scenario automatise - a ce qui a ete ajoute cette phase plutot qu'a
        // autre chose. La lecon du scenario G de la Phase 15 vaut ici aussi :
        // une mesure sans temoin ne prouve rien.
        bool speciesFollowAspect = true; // PHASE 26, voir --no-species-aspect
        bool singleTreeSpecies = false;  // PHASE 26, voir --single-tree-species
        bool useWildlife = true;
        // PHASE 20 : --no-animal-collision garde la faune VISIBLE mais la rend
        // traversable, c'est-a-dire le comportement exact d'avant cette phase.
        // C'est le temoin qui compte pour la mesure de collision : --no-wildlife
        // supprime les animaux, donc il ne peut pas montrer qu'on traverse celui
        // qui est la. Meme role que --no-scene pour l'editeur en Phase 15.
        bool animalCollision = true;
        // PHASE 29 : rochers et pierriers. --no-rocks = TEMOIN de cout (le monde
        // d'avant la Phase 29) ; --no-rock-collision garde les rochers visibles
        // mais traversables, comme --no-animal-collision pour la faune.
        bool useRocks = true;
        bool rockCollision = true;
        // PHASE 29 (Agent 3) : buissons (classe 3 LiDAR). --no-bushes = temoin.
        bool useBushes = true;
        // --start=E,N : point de depart en Lambert 93. PHASE 11. La camera
        // demarrait toujours au CENTRE du terrain, ce qui tombe sur du lapiaz nu
        // au milieu de cette zone : impossible d'aller regarder une foret ou une
        // prairie sans piloter la souris a la main, donc impossible d'en faire
        // une capture automatisee reproductible.
        std::string startArgument;

        // PHASE 23 : heure de jeu de depart et vitesse de l'horloge.
        //
        // POURQUOI CES DEUX OPTIONS EXISTENT : parce que le cycle jour/nuit rend
        // toute capture d'ecran dependante de l'instant, et que le projet a
        // deja appris deux fois ce que coute une capture non reproductible
        // (Phase 12 : trois captures de la meme scene differaient sur 5 % de
        // ciel faute de --look epingle). --hour fixe le moment, --time-scale=0
        // GELE l'horloge pour que la duree de mise en place de la capture ne
        // deplace pas le soleil. Les deux ensemble sont ce qui rend deux
        // captures a la meme heure comparables au pixel pres.
        //
        // -1 = non demande, on garde les valeurs par defaut de GameClock.
        float startHourArgument = -1.0f;
        // PHASE 27 : jour de partie de depart. --hour seul ne choisissait que
        // l'heure du JOUR 0, donc jamais une perturbation : impossible de
        // capturer la pluie. --day epingle le jour du calendrier meteo, qui est
        // deterministe (seed fixe), exactement comme --hour epingle l'heure.
        int startDayArgument = 0;
        float timeScaleArgument = -1.0f;
        // --look=yaw,pitch en degres, et --lock-input pour geler la camera.
        // Les deux vont ensemble : sans le verrou, la souris systeme peut
        // reorienter la camera pendant une capture (voir Camera::setInputEnabled).
        std::string lookArgument;
        bool lockInput = false;
        // PHASE 27 : TEMOIN ETROIT de la meteo visible. Il ne coupe QUE la
        // traduction meteo -> image (ciel grise, soleil voile, brouillard,
        // pluie/neige, sol mouille) : la meteo continue d'etre calculee et de
        // peser sur les besoins et les activites exactement comme avant. Deux
        // captures ou deux mesures avec et sans ce drapeau ne different donc que
        // par ce que la Phase 27 a ajoute a l'ecran.
        bool weatherVisuals = true;
        // PHASE 27, chantier 3 : TEMOIN ETROIT du micro-relief. Coupe le grain
        // de sol et lui seul.
        bool groundDetail = true;
        // Taille du motif de base : le texel de l'orthophoto reellement chargee.
        float groundDetailPattern = kGroundDetailDefaultPattern;
        std::string orthoArgument;   // --ortho=chemin.vcto, sinon la convention
        // PHASE 12 : streaming des tuiles. Voir l'en-tete de ce fichier.
        bool tileFallback = true;
        uint32_t tileBudget = 0;          // 0 = laisser le defaut du TerrainStreamer
        float flythroughSpeed = 0.0f;     // 0 = pas de survol scripte
        float flythroughAltitude = 120.0f;
        // PHASE 15 : editeur de niveau.
        //   --editor            demarre directement en mode edition (sinon F2)
        //   --scene=chemin      fichier de scene explicite (sinon la convention)
        //   --no-scene          ne charge ni n'ecrit aucun objet pose
        //   --editor-assets=rep dossier de modeles a scanner (sinon assets/editor)
        bool startInEditor = false;
        bool useScene = true;
        std::string sceneFileArgument;
        std::string editorAssetsArgument;
        std::string sceneArgument;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--dump-world") {
                dumpWorldOnly = true;
            } else if (argument == "--demo") {
                demoMode = true;
            } else if (argument == "--no-lod") {
                useLod = false;
            } else if (argument == "--no-ortho") {
                useOrtho = false;
            } else if (argument == "--no-vegetation") {
                useVegetation = false;
            } else if (argument == "--no-grass") {
                useGrass = false;
            } else if (argument == "--no-grass-cutout") {
                useGrassCutout = false;
            } else if (argument == "--single-tree-species") {
                // PHASE 26 : TEMOIN DE COUT, et lui seul.
                //
                // Force 100 % de resineux, donc un vecteur de feuillus VIDE, donc
                // UN SEUL appel de dessin instancie pour les arbres - exactement
                // la situation d'avant cette phase. C'est le seul moyen de
                // mesurer ce que coute le second appel sans changer de binaire,
                // de vue, ni de nombre d'instances : le total d'arbres soumis est
                // rigoureusement le meme, seule leur repartition entre un et deux
                // appels change.
                //
                // A ne pas confondre avec --no-species-aspect, qui garde les deux
                // appels et ne debranche que le couplage au versant.
                singleTreeSpecies = true;
            } else if (argument == "--no-species-aspect") {
                // PHASE 26 : LE TEMOIN du chantier 4.
                //
                // Il n'eteint PAS les essences - ce serait un temoin trop large,
                // qui melangerait deux effets : « il y a deux formes d'arbres
                // maintenant » et « leur repartition suit le versant ». Il annule
                // seulement le COUPLAGE A L'EXPOSITION (aspectSwing = 0).
                //
                // Consequence : meme nombre d'arbres, meme proportion globale de
                // resineux, memes deux silhouettes - mais reparties au hasard au
                // lieu de suivre l'adret et l'ubac. Toute difference entre les
                // deux captures ne peut donc venir que de ce couplage, qui est
                // exactement ce que cette phase pretend avoir ajoute.
                speciesFollowAspect = false;
            } else if (argument == "--no-wildlife") {
                useWildlife = false;
            } else if (argument == "--no-animal-collision") {
                animalCollision = false;
            } else if (argument == "--no-rocks") {
                useRocks = false;
            } else if (argument == "--no-rock-collision") {
                rockCollision = false;
            } else if (argument == "--no-bushes") {
                useBushes = false;
            } else if (argument == "--lock-input") {
                lockInput = true;
            } else if (argument == "--no-weather-visuals") {
                weatherVisuals = false;
            } else if (argument == "--no-ground-detail") {
                groundDetail = false;
            } else if (argument == "--editor") {
                startInEditor = true;
            } else if (argument == "--no-scene") {
                useScene = false;
            } else if (argument.rfind("--scene=", 0) == 0) {
                sceneFileArgument = argument.substr(std::strlen("--scene="));
            } else if (argument.rfind("--editor-assets=", 0) == 0) {
                editorAssetsArgument = argument.substr(std::strlen("--editor-assets="));
            } else if (argument == "--no-tile-cache") {
                // PHASE 18 : temoin de la mise en cache du travail de tuile.
                cacheTileWork = false;
            } else if (argument.rfind("--tile-threads=", 0) == 0) {
                try {
                    const long value =
                        std::stol(argument.substr(std::strlen("--tile-threads=")));
                    if (value < 1 || value > 64) {
                        logWarn("--tile-threads doit valoir entre 1 et 64. Ignore.");
                    } else {
                        tileThreads = static_cast<uint32_t>(value);
                    }
                } catch (const std::exception&) {
                    logWarn("--tile-threads : nombre illisible. Ignore : " + argument);
                }
            } else if (argument == "--no-tile-fallback") {
                tileFallback = false;
            } else if (argument.rfind("--tile-budget=", 0) == 0) {
                try {
                    const long value =
                        std::stol(argument.substr(std::strlen("--tile-budget=")));
                    if (value < 1) {
                        logWarn("--tile-budget doit valoir au moins 1. Ignore.");
                    } else {
                        tileBudget = static_cast<uint32_t>(value);
                    }
                } catch (const std::exception&) {
                    logWarn("--tile-budget : nombre illisible. Ignore : " + argument);
                }
            } else if (argument.rfind("--flythrough=", 0) == 0) {
                const std::string value = argument.substr(std::strlen("--flythrough="));
                const std::size_t comma = value.find(',');
                try {
                    flythroughSpeed = std::stof(value.substr(0, comma));
                    if (comma != std::string::npos) {
                        flythroughAltitude = std::stof(value.substr(comma + 1));
                    }
                    if (!(flythroughSpeed > 0.0f)) {
                        logWarn("--flythrough : vitesse nulle ou negative. Ignore.");
                        flythroughSpeed = 0.0f;
                    }
                } catch (const std::exception&) {
                    logWarn("--flythrough attend \"vitesse[,altitude]\" en m/s et m. "
                            "Ignore : " + argument);
                    flythroughSpeed = 0.0f;
                }
            } else if (argument.rfind("--look=", 0) == 0) {
                lookArgument = argument.substr(std::strlen("--look="));
            } else if (argument.rfind("--start=", 0) == 0) {
                startArgument = argument.substr(std::strlen("--start="));
            } else if (argument.rfind("--hour=", 0) == 0) {
                try {
                    const float value = std::stof(argument.substr(std::strlen("--hour=")));
                    if (value < 0.0f || value >= 24.0f) {
                        logWarn("--hour attend une heure dans [0, 24). Ignore : " + argument);
                    } else {
                        startHourArgument = value;
                    }
                } catch (const std::exception&) {
                    logWarn("--hour : heure illisible. Ignore : " + argument);
                }
            } else if (argument.rfind("--day=", 0) == 0) {
                try {
                    const int value = std::stoi(argument.substr(std::strlen("--day=")));
                    if (value < 0 || value > 3650) {
                        logWarn("--day attend un jour de partie dans [0, 3650]. Ignore : " +
                                argument);
                    } else {
                        startDayArgument = value;
                    }
                } catch (const std::exception&) {
                    logWarn("--day : jour illisible. Ignore : " + argument);
                }
            } else if (argument.rfind("--time-scale=", 0) == 0) {
                try {
                    const float value =
                        std::stof(argument.substr(std::strlen("--time-scale=")));
                    if (value < 0.0f) {
                        logWarn("--time-scale doit etre positif ou nul (0 = horloge "
                                "gelee). Ignore : " + argument);
                    } else {
                        timeScaleArgument = value;
                    }
                } catch (const std::exception&) {
                    logWarn("--time-scale : nombre illisible. Ignore : " + argument);
                }
            } else if (argument.rfind("--ortho=", 0) == 0) {
                orthoArgument = argument.substr(std::strlen("--ortho="));
            } else if (argument.rfind("--", 0) == 0) {
                logWarn("Option inconnue, ignoree : " + argument);
            } else if (sceneArgument.empty()) {
                sceneArgument = argument;
            }
        }
        const bool useDefaultTerrain = sceneArgument.empty();

        // PHASE 21 : demo AUTO-DETECTEE quand la dalle de DEVELOPPEMENT est
        // absente mais qu'une dalle de zone existe. Sans ca, double-cliquer sur
        // le vercora.exe d'un package distribue (voir NOTES-PHASE21.md) sans
        // argument retombait sur kDefaultTerrainFile, absent du package -> repli
        // sur le terrain SYNTHETIQUE 128x128, silencieusement : la personne qui
        // recoit la demo n'a aucune ligne de commande a taper, elle double-clique
        // et doit voir la vraie zone. Un chemin de scene EXPLICITE (sceneArgument
        // non vide) ou --demo deja tape ne sont pas affectes par cette detection.
        if (!demoMode && useDefaultTerrain && !std::filesystem::exists(kDefaultTerrainFile) &&
            (std::filesystem::exists(kDemoTerrainFile) ||
             std::filesystem::exists(kDemoTerrainFallback))) {
            demoMode = true;
        }

        // PHASE 21 : --demo coupe la faune, les besoins, les trois activites
        // montees, l'artisanat interactif et l'editeur - voir le commentaire de
        // la declaration de demoMode plus haut pour le detail et la raison de
        // chaque coupure. Applique ICI, juste apres le parsing et avant toute
        // construction du monde, pour que le reste du fichier n'ait plus a
        // savoir QUE ces flags ont ete forces - seulement que useWildlife vaut
        // false, exactement comme si --no-wildlife --no-scene avaient ete tapes.
        if (demoMode) {
            useWildlife = false;
            useScene = false;
            startInEditor = false;
            logInfo("Mode DEMO : faune, besoins, velo/parapente/escalade, "
                    "artisanat et editeur desactives. Marche, vol libre (F) et "
                    "compteur FPS seulement.");
        }

        // ------------------------------------------------------------------
        // 1. Monde cote CPU d'abord : on echoue vite et clairement, sans avoir
        //    ouvert de fenetre ni initialise Vulkan.
        // ------------------------------------------------------------------
        // Depuis la Phase 7 le relief peut venir d'une vraie dalle LiDAR HD ; les
        // arbres et les pierres, eux, restent semes de facon procedurale (la
        // classification LiDAR contient bien la vegetation, mais en tirer des
        // arbres individuels est un autre sujet).
        std::unique_ptr<Heightmap> heightmap = makeTerrain(sceneArgument, useDefaultTerrain, demoMode);

        // PHASE 21 : chemin de la dalle REELLEMENT chargee par makeTerrain (et
        // non plus systematiquement kDefaultTerrainFile) : le mode demo peut y
        // avoir substitue vercora_zone_1m/2m.vchm. Sert de base a l'orthophoto
        // (convention de nom, voir orthoPathFor) et au nom de fichier de scene -
        // les deux doivent designer la MEME dalle que celle affichee, ou
        // l'orthophoto chargee ne recouvrirait pas le terrain (orthoMatchesTerrain
        // la rejetterait) et le fichier de scene se romprait au premier F9.
        const std::string resolvedTerrainFile = !useDefaultTerrain ? sceneArgument
            : demoMode
                ? (std::filesystem::exists(kDemoTerrainFile) ? kDemoTerrainFile
                                                              : kDemoTerrainFallback)
                : kDefaultTerrainFile;
        const bool terrainScene = (heightmap != nullptr);

        // PHASE 26 : LES reglages d'essence, en UN exemplaire. Pousses a la fois
        // dans la Forest (arbres abattables) et dans la Vegetation (decor) pour
        // qu'il soit impossible que les deux systemes appliquent des lois
        // differentes au meme versant.
        TreeSpeciesParams kTreeSpecies{};
        if (singleTreeSpecies) {
            kTreeSpecies.coniferShareNeutral = 1.0f;
            kTreeSpecies.coniferShareMin = 1.0f;
            kTreeSpecies.coniferShareMax = 1.0f;
            kTreeSpecies.aspectSwing = 0.0f;
            logInfo("TEMOIN --single-tree-species : une seule essence, donc UN SEUL "
                    "appel de dessin instancie pour les arbres (etat d'avant la "
                    "Phase 26). Sert a mesurer le cout du second appel.");
        }
        if (!speciesFollowAspect) {
            kTreeSpecies.aspectSwing = 0.0f;
            logInfo("TEMOIN --no-species-aspect : les essences ne suivent plus le "
                    "versant (aspectSwing = 0). Meme nombre d'arbres et meme part "
                    "de resineux, mais reparties au hasard.");
        }

        // --- METEO, declaree AVANT le monde (PHASE 26) ----------------------
        // Deterministe et SANS ETAT (voir world/Weather.hpp) : il n'y a rien a
        // faire avancer d'une image a l'autre, on l'INTERROGE. Le seed est celui
        // par defaut, donc FIXE.
        //
        // POURQUOI ICI ET PLUS BAS COMME AVANT. La Phase 26 fait dependre
        // l'ESSENCE de chaque arbre de l'exposition adret/ubac, laquelle vient de
        // ce systeme ; or la Forest seme ses arbres dans son constructeur. Le
        // soleil doit donc exister avant le monde. C'est un simple deplacement de
        // declaration : l'objet est sans etat, rien ne depend de l'instant ou il
        // est construit.
        WeatherSystem weather;

        // Adaptateur : donne a la Forest et a la Vegetation l'exposition
        // adret/ubac SANS leur faire connaitre WeatherSystem. Meme patron que
        // GroundCoverSource (Phase 17) - une interface minuscule, une
        // implementation qui delegue, et deux systemes qui restent testables
        // sans monter toute la meteo.
        //
        // Il ne CALCULE rien : il transmet. Le calcul est celui de la Phase 23,
        // evalue au soleil de midi (voir WeatherSystem::aspectExposureAt).
        class WeatherAspectExposure final : public AspectExposureSource {
        public:
            WeatherAspectExposure(const WeatherSystem& weather, const HeightSampler* terrain)
                : weather_(weather), terrain_(terrain) {}
            float aspectExposureAt(float worldX, float worldZ) const override {
                // Y est indifferent : aspectExposureAt ne lit que la NORMALE du
                // relief en (x, z). On passe 0 plutot que de sonder l'altitude
                // pour rien.
                return weather_.aspectExposureAt(glm::vec3(worldX, 0.0f, worldZ), terrain_);
            }
        private:
            const WeatherSystem& weather_;
            const HeightSampler* terrain_ = nullptr;
        };
        const WeatherAspectExposure aspectExposure(weather, heightmap.get());

        std::unique_ptr<Ground> ground;
        std::unique_ptr<Forest> forest;

        // PHASE 15 : le monde a desormais DEUX sources d'obstacles - les troncs
        // et souches de la Forest (Phase 6) et les objets solides poses par
        // l'editeur. La camera, le velo et le grimpeur continuent de ne voir
        // qu'un seul CollisionSolver : celui-ci les additionne.
        //
        // Aucune des trois classes n'a ete modifiee pour ca. C'est exactement ce
        // que l'interface de la Phase 6 permettait, et la premiere fois qu'on
        // s'en sert.
        CompositeCollisionSolver worldSolver;

        MeshData sceneMesh;
        RenderOptions renderOptions;

        Inventory inventory;

        // PHASE 28 : etat de combustion des foyers (world/Crafting), tenu hors du
        // Ground, et nature du dernier objet ramasse (ce que P repose d'abord).
        HearthSet hearths;
        GroundItemKind lastPickedKind = GroundItemKind::Stone;

        if (terrainScene) {
            // Le maillage d'un seul bloc n'est construit que si on en a
            // reellement besoin :
            //   - jamais en --dump-world (des millions de triangles batis pour
            //     etre jetes aussitot) ;
            //   - jamais avec le quadtree, qui maille tuile par tuile a la
            //     demande. C'est le premier poste d'economie de RAM de la
            //     Phase 10 : sur la zone de 25 km2 au pas de 1 m, ce seul appel
            //     produisait 25 M de sommets et 50 M de triangles cote CPU,
            //     recopies ensuite sur le GPU.
            if (!dumpWorldOnly && !useLod) {
                sceneMesh = terrainMeshData(*heightmap);
            }

            renderOptions.heightRange = { heightmap->minHeight(), heightmap->maxHeight() };
            // Voir loin : le terrain fait worldSize d'un bord a l'autre, et on le
            // regarde en diagonale depuis le sol.
            renderOptions.farPlane = 3.0f * heightmap->worldSize();
            // Ciel diurne : sans lui, l'horizon se confond avec le fond noir et
            // on ne voit plus ou s'arrete le relief.
            renderOptions.clearColor = { 0.52f, 0.66f, 0.82f };

            const float extent = 0.5f * heightmap->worldSize();

            // Le sol est cree AVANT la foret : c'est lui qui recevra les buches
            // et les branches produites par les abattages.
            ground = std::make_unique<Ground>(*heightmap);
            // PHASE 26 : l'exposition adret/ubac est passee au CONSTRUCTEUR, seul
            // moment ou elle sert - c'est lui qui seme, donc lui qui decide des
            // essences. Sur le terrain synthetique elle est plate, donc neutre.
            forest = std::make_unique<Forest>(*heightmap, *ground, extent, ForestParams{},
                                              &aspectExposure, kTreeSpecies);
            worldSolver.add(forest.get());
            // PHASE 26 : la composition des arbres ABATTABLES. C'est aussi le
            // nombre d'arbres qui donneront de la resine - les deux ne peuvent
            // plus differer, ils lisent le meme champ.
            logInfo("Essences : " + std::to_string(forest->coniferCount()) + " resineux sur " +
                    std::to_string(forest->trees().size()) +
                    " arbres abattables (les resineux seuls donnent de la resine, "
                    "et se reconnaissent a leur silhouette conique).");

            // Le nombre d'arbres SEMES est journalise depuis la Phase 7 : sur du
            // relief reel, le filtre de pente (ForestParams::maxSlope) en rejette
            // une partie, ce qui est normal mais doit rester visible. Un semis a
            // zero arbre serait le premier symptome d'un terrain trop raide pour
            // le gameplay - une information qu'un terrain synthetique bien lisse
            // ne pouvait pas donner.
            logInfo("Arbres semes : " + std::to_string(forest->treeCount()) +
                    " (le relief refuse les pentes fortes).");

            const uint32_t stones = ground->scatterStones(kStoneCount, 26.0f, 5.0f, extent);
            logInfo("Pierres semees : " + std::to_string(stones) + " sur " +
                    std::to_string(kStoneCount) + " demandees.");
        } else if (sceneArgument == kBuiltinCubeKeyword) {
            logInfo("Geometrie : cube code en dur (Phase 1).");
            sceneMesh = cubeMeshData();
        } else {
            sceneMesh = loadObjMesh(sceneArgument);
        }

        if (dumpWorldOnly) {
            if (forest && ground) {
                dumpWorld(*heightmap, *forest, *ground);
            } else {
                logWarn("--dump-world : cette scene n'a ni foret ni objets au sol.");
            }
            return EXIT_SUCCESS;
        }

        // Le maillage n'a pas toujours de nom a ce stade (avec le quadtree il
        // n'existe pas encore), d'ou le repli sur le libelle de la heightmap.
        const std::string sceneName = !sceneMesh.name.empty()
            ? sceneMesh.name
            : (heightmap ? heightmap->describe() : std::string("scene"));
        // PHASE 21 : en mode demo, le titre se limite a "Vercora - DEMO" - pas
        // de nom de scene technique (illisible pour quelqu'un hors du projet),
        // pas de suffixe LOD. Le detail complet reste le titre de developpement.
        const std::string kBaseTitle = demoMode
            ? std::string("Vercora - DEMO")
            : "Vercora - Phase 10 - " + sceneName +
              (terrainScene && useLod ? " - LOD" : "");

        // ------------------------------------------------------------------
        // 2. Ordre d'initialisation : fenetre -> contexte Vulkan -> renderer.
        // ------------------------------------------------------------------
        Window window(1280, 720, kBaseTitle);
        VulkanContext context(window);
        Renderer renderer(context, window, renderOptions);

        // Enregistrement des geometries : une seule fois, quel que soit le
        // nombre d'instances qui les utiliseront ensuite.
        // Avec le quadtree, il n'y a pas de geometrie de scene unique : chaque
        // tuile est enregistree et liberee par le TerrainStreamer.
        const MeshHandle sceneMeshHandle =
            sceneMesh.empty() ? kInvalidMesh : renderer.addMesh(sceneMesh);

        // ------------------------------------------------------------------
        // 2 bis. Orthophoto (Phase 11). Chargee APRES le Renderer, qui possede
        //        le command pool servant au transfert. Toute defaillance est
        //        NON FATALE : sans photo le terrain reprend sa coloration
        //        procedurale par altitude et par pente, ce qui reste un monde
        //        jouable. La seule chose interdite est de draper en silence une
        //        photo qui ne correspond pas (voir orthoMatchesTerrain).
        // ------------------------------------------------------------------
        if (terrainScene && useOrtho && heightmap->geo().isGeoreferenced()) {
            const std::string terrainPath =
                resolvedTerrainFile;
            const std::string orthoPath =
                orthoArgument.empty() ? orthoPathFor(terrainPath) : orthoArgument;

            if (!std::filesystem::exists(orthoPath)) {
                logInfo("Orthophoto : aucune texture pour ce terrain (" + orthoPath +
                        " absent). Coloration procedurale. Pour la produire :");
                logInfo("  python tools/fetch_ortho.py " + terrainPath + " -o data/ortho");
                logInfo("  python tools/make_ortho_texture.py " + terrainPath +
                        " -i data/ortho -o " + orthoPath);
            } else {
                try {
                    const TextureData ortho = loadTextureFile(orthoPath);
                    if (orthoMatchesTerrain(ortho, *heightmap)) {
                        renderer.setTerrainTexture(ortho);
                        renderer.setOrthoMix(1.0f);
                        groundDetailPattern = ortho.metresPerTexel();
                        logInfo("Orthophoto : " +
                                std::to_string(ortho.metresPerTexel()) +
                                " m/texel pour un terrain au pas de " +
                                std::to_string(heightmap->spacing()) +
                                " m. Touche O pour basculer vers la coloration "
                                "procedurale.");
                    }
                } catch (const std::exception& error) {
                    logWarn(std::string("Orthophoto : ") + error.what() +
                            " Coloration procedurale conservee.");
                }
            }
        } else if (terrainScene && !useOrtho) {
            logInfo("Orthophoto desactivee (--no-ortho) : coloration procedurale.");
        }

        std::unique_ptr<TerrainStreamer> terrainStreamer;
        if (terrainScene && useLod) {
            TerrainStreamer::Params streamerParams;
            streamerParams.ancestorFallback = tileFallback;
            streamerParams.cacheTileWork = cacheTileWork;
            if (tileThreads > 0) {
                streamerParams.meshThreads = tileThreads;
            }
            if (tileBudget > 0) {
                streamerParams.maxUploadsPerFrame = tileBudget;
            }
            if (!tileFallback) {
                logInfo("Repli sur l'ancetre DESACTIVE (--no-tile-fallback) : une "
                        "tuile pas encore construite laissera un trou, comme en "
                        "Phase 10. Garde pour la comparaison.");
            }
            terrainStreamer =
                std::make_unique<TerrainStreamer>(*heightmap, renderer, streamerParams);
        } else if (terrainScene) {
            logInfo("Terrain dessine d'un seul bloc (--no-lod) : ni quadtree, ni "
                    "niveau de detail, ni frustum culling. C'est le comportement "
                    "de la Phase 9, garde pour la comparaison.");
        }

        // ------------------------------------------------------------------
        // 2 ter. Vegetation de decor (Phase 11). Les deux cartes LiDAR sont
        //        optionnelles : sans elles, Vegetation retombe sur un substitut
        //        par pente et altitude (voir world/Vegetation.cpp). Leur absence
        //        n'est donc PAS une erreur - c'est le cas normal du terrain
        //        synthetique et des dalles sans carte produite.
        // ------------------------------------------------------------------
        std::unique_ptr<Heightmap> coverMap;
        std::unique_ptr<Heightmap> canopyHeightMap;
        std::unique_ptr<Vegetation> vegetation;

        // --- PHASE 17 : empreinte environnementale ------------------------
        // L'Habitat existe TOUJOURS, meme sans terrain : ce n'est qu'une grille
        // de compteurs, il n'a besoin de rien. La faune, elle, a besoin d'un
        // relief sous les sabots, comme les quatre activites du joueur.
        Habitat habitat;
        std::unique_ptr<Wildlife> wildlife;

        if (terrainScene && useVegetation) {
            const std::string terrainPath =
                resolvedTerrainFile;
            const std::string coverPath = canopyPathFor(terrainPath, "");
            const std::string heightPath = canopyPathFor(terrainPath, "_height");

            for (auto* slot : { &coverMap, &canopyHeightMap }) {
                const std::string& path = (slot == &coverMap) ? coverPath : heightPath;
                if (!std::filesystem::exists(path)) {
                    continue;
                }
                try {
                    *slot = std::make_unique<Heightmap>(loadTerrainFile(path));
                } catch (const std::exception& error) {
                    logWarn(std::string("Carte de vegetation : ") + error.what());
                }
            }

            if (coverMap) {
                logInfo("Vegetation : carte de couvert LiDAR chargee (" + coverPath + ").");
            } else {
                logInfo("Vegetation : pas de carte de couvert (" + coverPath +
                        " absent). Semis de substitut par pente et altitude. "
                        "Pour la produire : python tools/lidar_to_canopy.py data/lidar "
                        "-o " + coverPath + " --extent " + terrainPath);
            }

            VegetationParams vegetationParams;
            if (!useGrassCutout) {
                vegetationParams.grassCutout = 0.0f;
            }
            vegetationParams.species = kTreeSpecies; // PHASE 26 : la meme loi que la Forest
            vegetation = std::make_unique<Vegetation>(
                *heightmap, coverMap.get(), canopyHeightMap.get(),
                0.5f * heightmap->worldSize(), vegetationParams);

            // PHASE 17 : l'herbe consulte desormais l'etat du sol. Branche ICI
            // et nulle part ailleurs - c'est le seul point de contact entre le
            // decor et la memoire du milieu. Non branche sous --no-wildlife :
            // le temoin doit etre le comportement d'AVANT la phase, pas
            // "presque".
            if (useWildlife) {
                vegetation->setGroundCover(&habitat);
            }

            // PHASE 26 : l'exposition adret/ubac, qui decide de l'ESSENCE des
            // arbres de decor - et d'elle seule. La PRESENCE d'un arbre reste
            // pilotee par la carte de couvert LiDAR de la Phase 11, exactement
            // comme avant : voir le commentaire de buildTreeCell, ou l'essence
            // est choisie APRES tous les filtres de presence.
            vegetation->setAspectExposure(&aspectExposure);

            logInfo("Vegetation : densite estimee " +
                    std::to_string(vegetation->estimateTreeDensityPerHectare()) +
                    " arbres/ha sur toute la zone. Touche G pour basculer "
                    "tout / arbres seuls / rien.");
        }

        // --- PHASE 17 : la faune -------------------------------------------
        // Cree APRES la vegetation parce qu'elle lit la MEME carte de couvert
        // LiDAR pour savoir ou est le bois. Elle peut etre nulle (terrain sans
        // carte) : la faune se place alors partout ou la pente le permet, voir
        // WildlifeParams::coverWithoutMap.
        //
        // Independante de --no-vegetation : couper le decor est un reglage
        // d'affichage pour mesurer des FPS, ce n'est pas une raison de vider le
        // monde de ses animaux.
        if (terrainScene && useWildlife) {
            WildlifeParams wildlifeParams;
            wildlifeParams.solid = animalCollision;
            wildlife = std::make_unique<Wildlife>(*heightmap, coverMap.get(),
                                                  0.5f * heightmap->worldSize(),
                                                  wildlifeParams);

            // PHASE 20 : troisieme source d'obstacles du monde, apres les
            // troncs de la Forest (Phase 6) et les objets poses par l'editeur
            // (Phase 15). Une seule ligne, et elle suffit pour LES TROIS
            // activites qui se deplacent au sol : la marche, le velo et
            // l'escalade recoivent tous le meme `worldSolver`, aucune des trois
            // classes n'a ete touchee. C'est le meme argument qu'en Phase 15,
            // et c'est la deuxieme fois qu'il tient.
            worldSolver.add(wildlife.get());
            logInfo(std::string("Faune : collision ") +
                    (animalCollision ? "ACTIVE (Phase 20), rayon " +
                                           std::to_string(wildlifeParams.colliderRadius) +
                                           " m par animal."
                                     : "DESACTIVEE (--no-animal-collision) : "
                                       "c'est le comportement d'avant la Phase 20, "
                                       "garde comme temoin."));
        } else if (terrainScene) {
            logInfo("Faune et memoire du milieu DESACTIVEES (--no-wildlife) : "
                    "c'est le comportement d'avant la Phase 17, garde comme temoin.");
        }

        MeshHandle treeMeshHandle = kInvalidMesh;
        MeshHandle stumpMeshHandle = kInvalidMesh;
        MeshHandle avatarMeshHandle = kInvalidMesh; // marqueur visuel du joueur en activite
        MeshHandle broadleafMeshHandle = kInvalidMesh;        // PHASE 26
        MeshHandle sceneryTreeMeshHandle = kInvalidMesh;
        MeshHandle sceneryBroadleafMeshHandle = kInvalidMesh;  // PHASE 26
        MeshHandle grassMeshHandle = kInvalidMesh;
        MeshHandle fiberMeshHandle = kInvalidMesh; // PHASE 22
        MeshHandle animalMeshHandle = kInvalidMesh; // PHASE 17

        // Un mesh par nature d'objet au sol, indexe par GroundItemKind : la
        // boucle de rendu se reduit alors a une indexation, sans cascade de if.
        std::array<MeshHandle, kGroundItemKindCount> groundMeshes{};
        groundMeshes.fill(kInvalidMesh);

        if (terrainScene) {
            treeMeshHandle = renderer.addMesh(treeMeshData(kTreeProportions));
            // PHASE 26 : la seconde silhouette. MEMES proportions, donc meme
            // tronc, donc LA MEME SOUCHE convient aux deux essences - c'est ce
            // qui evite un second mesh de souche et garantit que le trait de
            // coupe coincide quelle que soit l'essence abattue.
            broadleafMeshHandle = renderer.addMesh(broadleafTreeMeshData(kTreeProportions));
            stumpMeshHandle = renderer.addMesh(stumpMeshData(kTreeProportions));

            groundMeshes[static_cast<std::size_t>(GroundItemKind::Log)] =
                renderer.addMesh(logMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Branch)] =
                renderer.addMesh(branchMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Stone)] =
                renderer.addMesh(stoneMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Flake)] =
                renderer.addMesh(flakeMeshData());
            // PHASE 22 : les quatre matieres du document de cadrage qui
            // manquaient. Elles passent par la MEME table indexee par
            // GroundItemKind, donc par le meme unique point de rendu plus bas -
            // aucune cascade de if n'a ete ajoutee pour elles.
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Clay)] =
                renderer.addMesh(clayLumpMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Sand)] =
                renderer.addMesh(sandPileMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Resin)] =
                renderer.addMesh(resinBlobMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Fiber)] =
                renderer.addMesh(fiberBundleMeshData());
            // PHASE 28 : objets faconnes, meme table, meme point de rendu.
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Cord)] =
                renderer.addMesh(cordMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Axe)] =
                renderer.addMesh(axeMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Hearth)] =
                renderer.addMesh(hearthMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Ash)] =
                renderer.addMesh(ashMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Pitch)] =
                renderer.addMesh(pitchMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::PitchedAxe)] =
                renderer.addMesh(pitchedAxeMeshData());
            groundMeshes[static_cast<std::size_t>(GroundItemKind::Shelter)] =
                renderer.addMesh(shelterMeshData());

            // Marqueur du joueur pendant une activite a la troisieme personne
            // (parapente depuis la Phase 8, velo depuis la Phase 13) : sans lui,
            // la vue chase-cam montrerait le relief mais rien qui indique ou est
            // le joueur - le point meme d'une camera a la troisieme personne. Un
            // cube fait l'affaire ; les vrais meshs d'aile et de velo viendront
            // avec l'habillage visuel, hors scope des deux sessions de physique.
            avatarMeshHandle = renderer.addMesh(cubeMeshData());

            // PHASE 17 : une seule geometrie de cervide pour toute la faune,
            // instanciee par un DrawItem et une matrice comme les arbres
            // abattables. Voir world/AnimalMesh.hpp pour la convention.
            animalMeshHandle = renderer.addMesh(animalMeshData());

            if (vegetation) {
                // L'arbre de DECOR reutilise exactement le mesh des arbres
                // abattables : meme silhouette, donc aucune couture visible
                // entre les deux systemes. Seule la voie de rendu differe
                // (instanciee au lieu d'un DrawItem par arbre).
                sceneryTreeMeshHandle = treeMeshHandle;
                sceneryBroadleafMeshHandle = broadleafMeshHandle;
                grassMeshHandle = renderer.addMesh(grassTuftMeshData());
                // PHASE 22 : la plante a fibres SUR PIED. Meme voie de rendu
                // instanciee que l'herbe, meme materiau, meme mesh unite.
                fiberMeshHandle = renderer.addMesh(fiberPlantMeshData());
            }
        }

        // ------------------------------------------------------------------
        // 2 quater. EDITEUR DE NIVEAU (PHASE 15).
        // ------------------------------------------------------------------
        // Le catalogue et la scene sont montes MEME HORS MODE EDITEUR, et c'est
        // le point important : le jeu normal doit afficher les objets poses. Le
        // mode edition n'ajoute que les commandes ; ce qui a ete pose est du
        // contenu du monde, pas un artefact d'outil.
        //
        // Ordre : APRES le Renderer, parce que chaque geometrie du catalogue est
        // televersee des maintenant (une seule fois, quel que soit le nombre
        // d'objets qui l'utiliseront - c'est le decouplage geometrie/instances de
        // la Phase 5). Et AVANT la camera, qui doit recevoir un solveur de
        // collision deja complet.
        AssetCatalog editorCatalog;
        EditorScene editorScene;
        // Resolution asset -> geometrie, par HACHAGE et non par balayage du
        // catalogue : la liste de dessin est reconstruite a chaque image, et un
        // AssetCatalog::indexOf() par objet et par image serait une comparaison
        // de chaines par asset et par objet - negligeable a dix objets, plus du
        // tout a plusieurs milliers.
        std::unordered_map<std::string, const AssetEntry*> assetByName;
        std::string scenePath;
        std::string sceneTerrainLabel;

        if (terrainScene && useScene) {
            editorCatalog.addBuiltins();
            editorCatalog.scanDirectory(editorAssetsArgument.empty()
                                            ? std::string(kEditorAssetsDir)
                                            : editorAssetsArgument);

            for (AssetEntry& entry : editorCatalog.entries()) {
                // log a false : une ligne par asset noierait le reste, on
                // journalise un resume juste apres.
                entry.meshHandle = renderer.addMesh(entry.mesh, false);
            }
            for (const AssetEntry& entry : editorCatalog.entries()) {
                assetByName.emplace(entry.id, &entry);
            }
            logInfo("Editeur : " + std::to_string(editorCatalog.size()) +
                    " asset(s) au catalogue, geometries televersees.");

            editorScene.setCatalog(&editorCatalog);
            worldSolver.add(&editorScene);

            const std::string terrainPath =
                resolvedTerrainFile;
            sceneTerrainLabel = std::filesystem::path(terrainPath).filename().string();
            scenePath = sceneFileArgument.empty()
                            ? scenePathFor(terrainPath, kScenesDir)
                            : sceneFileArgument;
        } else if (startInEditor) {
            // Demander l'editeur et ne pas l'obtenir doit se DIRE. Les deux cas
            // ou il n'existe pas sont legitimes - on peut vouloir mesurer une
            // scene sans lui, ou regarder un .obj isole - mais rester muet
            // laisserait croire a une touche F2 qui ne repond pas.
            logWarn(terrainScene
                        ? "--editor ignore : --no-scene coupe l'editeur et les objets poses."
                        : "--editor ignore : l'editeur a besoin d'une scene de terrain "
                          "(il pose des objets SUR le relief).");
        }

        // Chargement (ou rechargement a la touche F9) de la scene depuis le
        // disque. Ecrit comme une lambda parce que les deux appelants - le
        // demarrage et la touche - doivent faire EXACTEMENT la meme chose, y
        // compris le controle des assets manquants ; les separer serait le moyen
        // sur d'en corriger un seul le jour ou ca changera.
        const auto loadScene = [&]() {
            if (scenePath.empty() || heightmap == nullptr) {
                return;
            }
            const SceneLoadResult result = loadSceneFile(scenePath, *heightmap);
            if (!result.fileExisted) {
                logInfo("Scene : aucun fichier a " + scenePath +
                        " (monde sans objets poses). Il sera cree par la touche F5 "
                        "depuis le mode editeur (F2).");
                editorScene.clear();
                return;
            }
            if (!result.ok) {
                logWarn("Scene : " + scenePath + " illisible (" + result.error +
                        "). Aucun objet charge, le fichier n'est PAS ecrase tant "
                        "qu'on ne sauvegarde pas.");
                return;
            }

            editorScene.replaceAll(result.objects);

            // Assets manquants : signales UNE fois, par nom, avec le compte.
            // C'est le seul defaut de scene qu'on ne peut pas voir a l'ecran -
            // un objet dont le modele n'existe plus n'est simplement pas
            // dessine, ce qui ressemble a "je n'avais rien pose la".
            std::unordered_map<std::string, uint32_t> missing;
            for (const PlacedObject& object : editorScene.objects()) {
                if (assetByName.find(object.asset) == assetByName.end()) {
                    ++missing[object.asset];
                }
            }
            for (const auto& [name, count] : missing) {
                logWarn("Scene : " + std::to_string(count) + " objet(s) referencent "
                        "l'asset \"" + name + "\", absent du catalogue. Ils sont "
                        "conserves dans le fichier mais ni dessines ni solides.");
            }

            logInfo("Scene : " + std::to_string(editorScene.size()) + " objet(s) charges "
                    "depuis " + scenePath + " (" + std::to_string(editorScene.solidCount()) +
                    " solide(s), terrain d'origine \"" + result.terrainLabel + "\", EPSG:" +
                    std::to_string(result.epsg) + ").");
            if (result.skippedObjects > 0) {
                logWarn("Scene : " + std::to_string(result.skippedObjects) +
                        " ligne(s) d'objet mal formees, sautees.");
            }
            if (result.outsideExtent > 0) {
                logWarn("Scene : " + std::to_string(result.outsideExtent) +
                        " objet(s) hors de l'emprise du terrain charge. Scene "
                        "composee sur une autre emprise ?");
            }
        };
        loadScene();

        // ------------------------------------------------------------------
        // 3. Camera. Sur le terrain : au centre, en mode marche (le Y exact est
        //    impose par setMode). Sur un objet : en recul, comme en Phase 1.
        // ------------------------------------------------------------------
        Camera camera(terrainScene ? glm::vec3(0.0f, 0.0f, 0.0f)
                                   : glm::vec3(0.0f, 0.0f, 3.0f));

        if (terrainScene) {
            camera.setHeightSampler(heightmap.get());
            // Le solveur composite plutot que la Forest directement (PHASE 15) :
            // il contient la Forest - qui sait ou sont les troncs et les souches -
            // ET la scene d'editeur, qui sait ou sont les rochers poses.
            camera.setCollisionSolver(&worldSolver);
            camera.setMoveSpeed(kWalkSpeed);
            camera.setMode(CameraMode::Walk);

            if (!startArgument.empty()) {
                if (!heightmap->geo().isGeoreferenced()) {
                    logWarn("--start ignore : ce terrain n'est pas georeference.");
                } else {
                    const std::size_t comma = startArgument.find(',');
                    if (comma == std::string::npos) {
                        logWarn("--start attend \"est,nord\" en Lambert 93, par exemple "
                                "--start=901315,6435855. Ignore : " + startArgument);
                    } else {
                        try {
                            const double east = std::stod(startArgument.substr(0, comma));
                            const double north = std::stod(startArgument.substr(comma + 1));
                            // Inverse de Heightmap::toProjected : le terrain est
                            // centre sur l'origine, l'est croit avec X et le nord
                            // DECROIT avec Z (voir Heightmap.hpp).
                            const float worldX =
                                static_cast<float>(east - heightmap->geo().originEast) -
                                0.5f * heightmap->worldSize();
                            const float worldZ =
                                static_cast<float>(heightmap->geo().originNorth - north) -
                                0.5f * heightmap->worldSize();
                            const float half = 0.5f * heightmap->worldSize();
                            if (std::abs(worldX) > half || std::abs(worldZ) > half) {
                                logWarn("--start est hors de l'emprise du terrain. Ignore.");
                            } else {
                                camera.setPosition({ worldX, 0.0f, worldZ });
                                camera.setMode(CameraMode::Walk); // recolle au sol
                            }
                        } catch (const std::exception&) {
                            logWarn("--start : coordonnees illisibles. Ignore : " +
                                    startArgument);
                        }
                    }
                }
            }

            if (!lookArgument.empty()) {
                const std::size_t comma = lookArgument.find(',');
                if (comma == std::string::npos) {
                    logWarn("--look attend \"yaw,pitch\" en degres. Ignore : " + lookArgument);
                } else {
                    try {
                        camera.setOrientation(std::stof(lookArgument.substr(0, comma)),
                                              std::stof(lookArgument.substr(comma + 1)));
                    } catch (const std::exception&) {
                        logWarn("--look : angles illisibles. Ignore : " + lookArgument);
                    }
                }
            }

            if (flythroughSpeed > 0.0f) {
                // Le survol pilote la position lui-meme : la camera doit etre en
                // mode libre (sinon elle se recolle au sol a chaque frame) et ne
                // doit pas recevoir le clavier ni la souris, sans quoi la
                // trajectoire ne serait plus reproductible d'un run a l'autre.
                camera.setMode(CameraMode::Free);
                camera.setInputEnabled(false);
                logInfo("Survol scripte (--flythrough) : " +
                        std::to_string(flythroughSpeed) + " m/s a " +
                        std::to_string(flythroughAltitude) +
                        " m au-dessus du relief, virage de 90 deg au bord de "
                        "l'emprise. Clavier et souris ignores.");
            }

            if (lockInput) {
                camera.setInputEnabled(false);
                logInfo("Camera VERROUILLEE (--lock-input) : ni clavier ni souris. "
                        "La vue est entierement determinee par --start et --look. "
                        "La pose au CLIC de l'editeur est coupee elle aussi - un "
                        "clic reel de l'utilisateur poserait sinon un objet "
                        "pendant une mesure automatisee (utiliser INSER).");
            }

            if (heightmap->geo().isGeoreferenced()) {
                // Le point de depart, exprime dans la projection d'origine : de
                // quoi retrouver l'endroit sur une carte et confronter le relief
                // affiche a la realite. C'est le seul controle possible du bon
                // sens des axes sans orthophoto - un terrain en miroir donnerait
                // ici des coordonnees parfaitement plausibles mais un versant du
                // mauvais cote.
                double east = 0.0, north = 0.0;
                heightmap->toProjected(camera.position().x, camera.position().z,
                                       &east, &north);
                logInfo("Position de depart : E " + std::to_string(east) + " N " +
                        std::to_string(north) + " (EPSG:" +
                        std::to_string(heightmap->geo().epsg) + "), altitude " +
                        std::to_string(camera.position().y) + " m.");
            }
        }

        FpsCounter fpsCounter;

        // Le vol n'existe que sur une scene de terrain : decoller suppose un
        // relief sous les pieds pour mesurer la hauteur de depart et detecter
        // l'atterrissage.
        Paraglider paraglider;
        KeyEdge paraglideKey;

        // PHASE 13 : le velo. Meme conditions d'existence que le parapente - il
        // lui faut un relief sous les roues pour avoir une pente et une
        // altitude. Les parametres par defaut sont ceux documentes dans
        // world/Bike.hpp ; seule la hauteur d'oeil est reprise de la camera,
        // pour que descendre de velo ne fasse pas sauter le point de vue.
        BikeParams bikeParams;
        bikeParams.eyeHeight = kDefaultEyeHeight;
        Bike bike(bikeParams);
        KeyEdge bikeKey;

        // PHASE 14 : l'escalade. Comme les deux precedentes, elle n'existe que
        // sur une scene de terrain - il lui faut une surface dont lire la pente.
        // Tous les parametres sont ceux documentes dans world/Climber.hpp ;
        // aucun n'est repris de la camera, parce que l'escalade n'a justement
        // PAS de hauteur d'oeil au-dessus du sol (voir Climber::position()).
        Climber climber;
        KeyEdge climbKey;

        // --- PHASE 16 : les besoins primaires -----------------------------
        //
        // Contrairement aux quatre activites, les besoins existent AUSSI sur une
        // scene d'objet isole : ils ne demandent au monde qu'une altitude et une
        // heure. Ce qui exige un relief, c'est la temperature liee au versant et
        // la recherche des points d'eau - les deux sont gardees par `heightmap`
        // plus bas, pas par l'existence des jauges.
        Needs needs;

        // PHASE 23 : l'horloge prend l'heure et l'echelle demandees en ligne de
        // commande. GameClock refuse une echelle nulle (elle la ramene a 1), ce
        // qui est le bon comportement pour un jeu ; pour GELER l'horloge d'une
        // capture, on ne touche donc pas a l'echelle, on cesse simplement de
        // l'avancer - voir clockFrozen plus bas.
        const bool clockFrozen = (timeScaleArgument == 0.0f);
        GameClock clock(startHourArgument >= 0.0f ? startHourArgument : 9.0f,
                        timeScaleArgument > 0.0f ? timeScaleArgument : 12.0f);
        if (startDayArgument > 0) {
            // L'horloge n'expose que advance() pour avancer le compteur TOTAL :
            // on s'en sert plutot que d'ajouter une porte derobee. Le calcul est
            // exact a la precision du float (au pire quelques secondes de jeu
            // sur dix ans).
            clock.advance(static_cast<float>(startDayArgument) * 24.0f * 3600.0f /
                          clock.scale());
        }
        if (startHourArgument >= 0.0f || timeScaleArgument >= 0.0f || startDayArgument > 0) {
            logInfo(std::string("Horloge : depart au jour ") +
                    std::to_string(clock.day()) + " a " + clock.clockText() +
                    (clockFrozen ? ", GELEE (--time-scale=0)."
                                 : ", facteur " + std::to_string(clock.scale()) + "."));
        }

        // --- PHASE 23 : le systeme meteo --------------------------------
        //
        // Deterministe et SANS ETAT (voir world/Weather.hpp) : il n'y a rien a
        // faire avancer d'une image a l'autre, on l'INTERROGE. Le seul etat du
        // monde meteo est l'heure de jeu, et elle vit deja dans `clock`.
        //
        // Le seed est celui par defaut, donc FIXE : la zone de jeu est unique et
        // fabriquee a la main, rien d'autre dans Vercora n'est reseede par
        // partie, et un calendrier meteo fixe est ce qui rend deux captures du
        // meme instant comparables (voir la lecon de la Phase 12 sur --look).
        // PHASE 26 : la declaration a REMONTE avant la construction du monde
        // (voir plus haut). L'ordre importe desormais : la Forest decide de
        // l'essence de chaque arbre au moment ou elle le plante, donc elle a
        // besoin que le soleil existe avant elle. Rien d'autre n'a change - le
        // systeme est sans etat, le construire plus tot ne coute rien.

        // PHASE 25 : le shader terrain code en dur l'ecart de reference de
        // l'adret/ubac, faute de pouvoir lire un parametre C++. Si quelqu'un
        // regle la meteo sans toucher au shader, l'indice VISUEL cesse d'etre
        // d'accord avec la TEMPERATURE - exactement le genre de double source de
        // verite que la Phase 23 a pris soin d'eviter. On ne peut pas empecher
        // la divergence ; on peut refuser qu'elle soit silencieuse.
        if (std::abs(weather.params().aspectIncidenceReference -
                     kShaderAspectIncidenceReference) > 1e-4f) {
            logWarn("Adret/ubac : WeatherParams::aspectIncidenceReference vaut " +
                    std::to_string(weather.params().aspectIncidenceReference) +
                    " alors que shaders/terrain.frag code " +
                    std::to_string(kShaderAspectIncidenceReference) +
                    ". L'indice visuel et la temperature ne diront plus la meme "
                    "chose : mettre les deux d'accord.");
        }
        ColdAirTrapMap coldAirTraps;

        // Etat meteo de l'image courante. Calcule UNE FOIS par image, au point du
        // joueur, et relu par tout ce qui en a besoin - les besoins, les quatre
        // activites, la lumiere. Une seule evaluation par image : recalculer
        // dans chaque activite serait a la fois du gaspillage et le retour du
        // probleme que cette phase corrige (plusieurs sources de verite).
        SynopticState skyState = weather.synopticAt(clock.totalHours());
        WeatherSample localWeather;
        float lastWeatherVisualsLog = -1.0f; // PHASE 27, journal borne par le temps
        KeyEdge drinkKey;
        KeyEdge digKey;   // PHASE 22
        KeyEdge resinKey; // PHASE 22
        KeyEdge fiberKey; // PHASE 22

        // Position de la camera a l'image precedente : sert a distinguer un
        // joueur qui MARCHE d'un joueur qui s'est arrete (voir
        // kRestSpeedThreshold). Initialisee a la position de depart pour que la
        // toute premiere image ne compte pas comme un deplacement de 500 m.
        glm::vec3 previousPlayerPosition = camera.position();

        // Points d'eau : detectes UNE FOIS au chargement, jamais recalcules -
        // le relief ne bouge pas. Le balayage coute quelques milliers d'appels a
        // heightAt (voir world/WaterSource.hpp pour la methode et ses limites).
        std::vector<WaterSource> waterSources;
        if (heightmap) {
            const float half = 0.5f * heightmap->worldSize();
            waterSources = findWaterSources(*heightmap, -half, half, -half, half);
            logInfo("Points d'eau : " + std::to_string(waterSources.size()) +
                    " creux retenus sur " + std::to_string(heightmap->worldSize()) +
                    " m d'emprise. Touche T pour boire quand on est a moins de " +
                    std::to_string(kDrinkReach) + " m de l'un d'eux.");
            for (std::size_t i = 0; i < waterSources.size() && i < 5; ++i) {
                logInfo("  point d'eau " + std::to_string(i + 1) + " : " +
                        std::to_string(waterSources[i].position.x) + " " +
                        std::to_string(waterSources[i].position.y) + " " +
                        std::to_string(waterSources[i].position.z) +
                        " (creux de " + std::to_string(waterSources[i].depth) + " m)");
            }
            if (waterSources.empty()) {
                logWarn("Points d'eau : aucun creux ne passe les criteres sur ce "
                        "relief. La soif ne sera pas etanchable ici - voir "
                        "world/WaterSource.hpp (WaterSurveyParams).");
            }
        }

        // --- PHASE 23 : carte de piegeage d'air froid -----------------------
        // Precalculee UNE FOIS au chargement, exactement comme les points d'eau
        // ci-dessus et pour la meme raison - le relief ne bouge pas. C'est
        // l'exigence de cout du cahier des charges : l'inversion de fond de
        // cuvette demande de savoir quels points sont ENFERMES par du relief plus
        // haut, ce qui est une question sur la topographie et pas sur l'instant.
        //
        // 128 x 128 sur les 25 km2 de la zone de jeu = des cellules de 39 m, la
        // bonne echelle pour une cuvette (une cuvette de 39 m n'en est pas une),
        // pour 393 216 lectures de heightmap payees une seule fois.
        if (heightmap) {
            const float half = 0.5f * heightmap->worldSize();
            coldAirTraps = ColdAirTrapMap(*heightmap, half, 128);
            weather.setTrapMap(&coldAirTraps);
            logInfo("Meteo : carte de piegeage d'air froid 128x128 sur " +
                    std::to_string(heightmap->worldSize()) +
                    " m (index moyen " + std::to_string(coldAirTraps.meanValue()) +
                    ", maximum " + std::to_string(coldAirTraps.maxValue()) + ").");
            logInfo("Meteo : " + weather.synopticAt(clock.totalHours()).describe());
        } else {
            logInfo("Meteo : aucun relief charge - pas de carte de piegeage, donc "
                    "aucune inversion de fond de cuvette. Le reste du systeme "
                    "meteo fonctionne normalement.");
        }

        // --- PHASE 22 : les gisements d'argile et de sable ------------------
        // Detectes UNE FOIS au chargement, comme les points d'eau et pour la
        // meme raison : le relief ne bouge pas. Ils sont cherches APRES l'eau,
        // et ce n'est pas un detail d'ordonnancement - c'est la liste des points
        // d'eau qui decide lesquels sont de l'argile et lesquels sont du sable
        // (voir world/Deposits.hpp).
        //
        // A LIRE AVANT DE S'Y FIER : ce placement est un PROXY MORPHOLOGIQUE et
        // non une carte geologique. Le LiDAR ne porte aucune information de
        // nature de terrain ; ces gisements sont places la ou ils seraient
        // PLAUSIBLES, pas la ou ils sont.
        std::vector<MineralDeposit> deposits;
        if (heightmap) {
            const float half = 0.5f * heightmap->worldSize();
            deposits = findMineralDeposits(*heightmap, -half, half, -half, half,
                                           waterSources);
            std::size_t clayCount = 0;
            for (const MineralDeposit& deposit : deposits) {
                if (deposit.kind == DepositKind::Clay) {
                    ++clayCount;
                }
            }
            logInfo("Gisements : " + std::to_string(deposits.size()) +
                    " retenus (" + std::to_string(clayCount) + " d'argile, " +
                    std::to_string(deposits.size() - clayCount) + " de sable). "
                    "Touche Y pour creuser a moins de " +
                    std::to_string(kDigReach) + " m. PLACEMENT PAR PROXY "
                    "MORPHOLOGIQUE (creux + versant amont, argile pres de l'eau) "
                    "- ce n'est PAS une carte geologique, voir "
                    "world/Deposits.hpp.");
            for (std::size_t i = 0; i < deposits.size() && i < 5; ++i) {
                logInfo(std::string("  gisement ") + std::to_string(i + 1) + " : " +
                        depositLabel(deposits[i].kind) + " en " +
                        std::to_string(deposits[i].position.x) + " " +
                        std::to_string(deposits[i].position.y) + " " +
                        std::to_string(deposits[i].position.z) +
                        " (creux " + std::to_string(deposits[i].depth) +
                        " m, amont " + std::to_string(deposits[i].upslope) + " m)");
            }
        }

        // --- PHASE 22 : le marquage des resineux ---------------------------
        // Aucune donnee ne dit qu'un arbre est un resineux (ni le mesh, ni la
        // carte de canopee) : c'est une convention DETERMINISTE, recalculee et
        // jamais stockee. Voir world/Resin.hpp pour la limite assumee.
        ResinStand resinStand;

        // --- PHASE 22 : les plantes a fibres -------------------------------
        // Branchees sur l'Habitat par ClearingSource : une zone ou le joueur a
        // abattu des arbres se met a porter des ronces. C'est la premiere fois
        // dans ce projet qu'une action du joueur FAIT APPARAITRE quelque chose
        // au lieu de le retirer.
        std::unique_ptr<FiberPlants> fiberPlants;
        if (terrainScene && heightmap) {
            fiberPlants = std::make_unique<FiberPlants>(
                *heightmap, coverMap.get(), 0.5f * heightmap->worldSize());
            fiberPlants->setClearingSource(&habitat);
            logInfo(std::string("Plantes a fibres : semis actif") +
                    (coverMap ? " (lisiere LiDAR + sol ouvert par le joueur)"
                              : " (sol ouvert par le joueur SEULEMENT : aucune "
                                "carte de couvert, donc aucune lisiere connue)") +
                    ". Touche I pour en arracher une a moins de " +
                    std::to_string(kPullReach) + " m.");
        }

        // --- PHASE 29 : rochers et pierriers --------------------------------
        // Semes UNIQUEMENT sous le masque de NOTES-PHASE29-DESIGN.md (pente
        // Climber 0,60-0,84, couvert LiDAR < 0,10). Sans carte de couvert,
        // Rocks est vide : aucun rocher sans donnee reelle.
        std::unique_ptr<Rocks> rocks;
        MeshHandle boulderMeshHandle = kInvalidMesh;
        MeshHandle rockMeshHandle = kInvalidMesh;
        MeshHandle gravelMeshHandle = kInvalidMesh;
        if (terrainScene && heightmap && useRocks) {
            RockParams rockParams;
            rockParams.collide = rockCollision;
            rocks = std::make_unique<Rocks>(*heightmap, coverMap.get(),
                                            0.5f * heightmap->worldSize(), rockParams);
            if (rocks->enabled()) {
                boulderMeshHandle = renderer.addMesh(boulderMeshData());
                rockMeshHandle = renderer.addMesh(rockMeshData());
                gravelMeshHandle = renderer.addMesh(gravelMeshData());
                worldSolver.add(rocks.get());
                const Rocks::Stats rs = rocks->estimate();
                char line[320];
                std::snprintf(line, sizeof(line),
                              "Rochers (PHASE 29) : masque pierrier %.1f %% de la zone ; "
                              "%.2f gros blocs, %.2f rochers, %.1f plaques de pierraille "
                              "par ha de zone ; collision des gros blocs %s.",
                              100.0 * rs.maskFraction, rs.bouldersPerHa, rs.stonesPerHa,
                              rs.gravelPerHa,
                              rockCollision ? "ACTIVE" : "DESACTIVEE (--no-rock-collision)");
                logInfo(line);
            } else {
                logInfo("Rochers (PHASE 29) : aucune carte de couvert LiDAR, donc AUCUN "
                        "rocher (pas de semis sans donnee reelle).");
            }
        } else if (terrainScene && !useRocks) {
            logInfo("Rochers DESACTIVES (--no-rocks) : monde d'avant la Phase 29, temoin.");
        }

        // --- PHASE 29 (Agent 3) : buissons et petits arbres -----------------
        // Pilotes par la classe 3 LiDAR seule (NOTES-PHASE29-DESIGN.md s.2) :
        // assets/terrain/<zone>_canopy_lowveg.vchm (F3) et
        // <zone>_canopy_lowveg_height.vchm (h max classe 3), meme convention de
        // nommage que les cartes de couvert (canopyPathFor). Le couvert (deja
        // charge) ne sert qu'a EXCLURE. Une carte absente -> aucun buisson.
        std::unique_ptr<Heightmap> lowVegMap;
        std::unique_ptr<Heightmap> lowVegHeightMap;
        std::unique_ptr<Bushes> bushes;
        MeshHandle shrubMeshHandle = kInvalidMesh;
        MeshHandle smallTreeMeshHandle = kInvalidMesh;
        if (terrainScene && heightmap && useBushes) {
            const std::string lowVegPath = canopyPathFor(resolvedTerrainFile, "_lowveg");
            const std::string lowVegHeightPath = canopyPathFor(resolvedTerrainFile, "_lowveg_height");
            for (auto* slot : { &lowVegMap, &lowVegHeightMap }) {
                const std::string& path = (slot == &lowVegMap) ? lowVegPath : lowVegHeightPath;
                if (!std::filesystem::exists(path)) { continue; }
                try {
                    *slot = std::make_unique<Heightmap>(loadTerrainFile(path));
                } catch (const std::exception& error) {
                    logWarn(std::string("Carte de vegetation basse : ") + error.what());
                }
            }
            bushes = std::make_unique<Bushes>(*heightmap, lowVegMap.get(), lowVegHeightMap.get(),
                                              coverMap.get(), 0.5f * heightmap->worldSize());
            if (bushes->enabled()) {
                if (forest) {
                    std::vector<glm::vec3> footprints;
                    footprints.reserve(forest->trees().size());
                    for (const Tree& tree : forest->trees()) {
                        footprints.emplace_back(tree.base.x, tree.base.z, tree.crownRadius());
                    }
                    bushes->setTreeFootprints(std::move(footprints));
                }
                shrubMeshHandle = renderer.addMesh(shrubMeshData());
                smallTreeMeshHandle = renderer.addMesh(smallTreeMeshData());
                const Bushes::Stats bs = bushes->estimate();
                char line[320];
                std::snprintf(line, sizeof(line),
                              "Buissons (PHASE 29) : masque classe 3 %.1f %% de la zone ; "
                              "%.1f buissons et %.1f petits arbres par ha de zone ; "
                              "traversables ; %zu emprises d'arbres exclues.",
                              100.0 * bs.maskFraction, bs.shrubsPerHa, bs.smallTreesPerHa,
                              forest ? forest->trees().size() : std::size_t{0});
                logInfo(line);
            } else {
                logInfo("Buissons (PHASE 29) : carte classe 3 ou couvert absent (" + lowVegPath +
                        "), donc AUCUN buisson (pas de semis sans donnee reelle).");
            }
        } else if (terrainScene && !useBushes) {
            logInfo("Buissons DESACTIVES (--no-bushes) : temoin.");
        }

        // --- PHASE 17 : premier peuplement ----------------------------------
        // Peupler AVANT la premiere image, sinon le joueur voit le monde se
        // remplir sous ses yeux pendant les premieres secondes - ce qui montre
        // la mecanique au lieu du monde. C'est la meme raison qui fait que le
        // semis d'arbres est fait au constructeur de Forest.
        if (wildlife) {
            wildlife->populate(camera.position(), habitat);
            logInfo("Faune : " + std::to_string(wildlife->animalCount()) +
                    " animaux au depart dans un rayon de " +
                    std::to_string(wildlife->params().spawnRadiusMax) +
                    " m (le couvert et la pente refusent le reste).");
            if (wildlife->animalCount() == 0) {
                logWarn("Faune : aucun animal place. Terrain trop raide ou couvert "
                        "LiDAR trop faible autour du depart - voir "
                        "WildlifeParams::minCover et maxSlope.");
            }
        }

        logInfo("Initialisation terminee. Clic droit maintenu + ZQSD pour bouger"
                + std::string(terrainScene
                    ? ", F pour basculer marche/vol libre, E pour abattre l'arbre"
                      " vise, R pour ramasser, P pour reposer au sol, C pour faconner"
                      " (percuter deux pierres, ou assembler ce qui est pose), V pour"
                      " decoller en parapente depuis le mode vol libre (en hauteur),"
                      " B pour enfourcher le velo depuis la marche (puis Z pedaler,"
                      " S freiner, Q/D tourner), X pour s'engager sur une paroi"
                      " grimpable depuis la marche (puis Z/S monter et descendre,"
                      " Q/D traverser), T pour boire a un point d'eau,"
                      " F2 pour le MODE EDITEUR."
                    : ", espace/shift pour monter/descendre."));

        // Reutilisee a chaque frame : la liste des objets varie en contenu mais
        // pas en ordre de grandeur, donc on garde sa capacite d'une frame a
        // l'autre au lieu de reallouer 60 fois par seconde.
        std::vector<DrawItem> drawItems;

        // Reutilises d'une frame a l'autre : leur capacite se stabilise apres
        // quelques images, donc plus aucune allocation en regime etabli.
        std::vector<InstancedDrawItem> instancedItems;
        std::vector<InstanceData> treeInstances;         // resineux (PHASE 26)
        std::vector<InstanceData> broadleafInstances;    // feuillus (PHASE 26)
        std::vector<InstanceData> grassInstances;
        std::vector<InstanceData> fiberInstances; // PHASE 22
        std::vector<InstanceData> boulderInstances; // PHASE 29
        std::vector<InstanceData> stoneInstances;   // PHASE 29
        std::vector<InstanceData> gravelInstances;  // PHASE 29
        std::vector<InstanceData> shrubInstances;     // PHASE 29 (Agent 3)
        std::vector<InstanceData> smallTreeInstances; // PHASE 29 (Agent 3)

        // Trois etats plutot qu'un booleen : pouvoir isoler les arbres de
        // l'herbe est ce qui permet d'attribuer un cout de FPS a l'un ou a
        // l'autre pendant une mesure, et de faire des captures comparables.
        enum class VegetationMode { All, TreesOnly, None };
        VegetationMode vegetationMode = VegetationMode::All;

        const auto startTime = std::chrono::steady_clock::now();
        double lastTime = glfwGetTime();

        KeyEdge fellKey;
        KeyEdge orthoKey;
        KeyEdge vegetationKey;
        KeyEdge pickupKey;
        KeyEdge knapKey;
        KeyEdge putDownKey; // PHASE 28

        // --- PHASE 15 : etat du mode editeur ----------------------------
        EditorController editorController(editorCatalog, editorScene);
        bool editorMode = false;
        // --editor : la bascule est demandee comme si F2 avait ete presse a la
        // premiere image, plutot que dupliquee ici. C'est ce qui garantit que
        // demarrer en edition passe par EXACTEMENT le meme chemin que d'y entrer
        // en cours de partie - donc que les deux ne peuvent pas diverger.
        bool editorToggleRequested = startInEditor;
        float editorFlySpeed = kEditorFlySpeed;
        // Ce qu'on rend a la sortie du mode edition. La camera d'edition est un
        // vol libre rapide ; retrouver la marche - et la vitesse de marche -
        // exactement comme on les avait laissees est ce qui permet d'aller
        // retoucher un rocher en pleine partie sans perdre sa position.
        CameraMode editorPreviousMode = CameraMode::Walk;
        float editorPreviousSpeed = 14.0f;

        // PHASE 24 : dernier etat JOURNALISE du filtre de pente de la marche.
        // Sert uniquement a n'ecrire une ligne que sur transition - voir le bloc
        // qui suit camera.update().
        bool walkBlockedWasLogged = false;
        bool walkSlidingWasLogged = false;
        // Instant de la derniere ligne emise par ce bloc. Voir le limiteur de
        // cadence qui suit camera.update() : un etat qui BAT n'est pas un
        // evenement, et ne doit pas se payer en lignes de journal.
        double walkLogLastTime = -1000.0;

        KeyEdge editorToggleKey;
        KeyEdge editorPlaceButton;
        KeyEdge editorPlaceKey;
        KeyEdge editorPickKey;
        KeyEdge editorGrabKey;
        KeyEdge editorDeleteKey;
        KeyEdge editorCancelKey;
        KeyEdge editorNextKey;
        KeyEdge editorNextKey2;
        KeyEdge editorPrevKey;
        KeyEdge editorSolidKey;
        KeyEdge editorUndoKey;
        KeyEdge editorSaveKey;
        KeyEdge editorReloadKey;
        KeyEdge editorListKey;
        KeyEdge editorCycleNextKey;
        KeyEdge editorCyclePrevKey;
        std::array<KeyEdge, 9> editorQuickKeys{};

        const std::string kEditorHelp =
            "Editeur (F2 pour sortir) : ZQSD + espace/maj-gauche pour voler, "
            "molette = vitesse de vol, clic droit = regarder. "
            "Clic gauche ou INSER = poser au point vise, E = selectionner "
            "l'objet vise, "
            "G = saisir/reposer, SUPPR = supprimer, ECHAP = annuler la saisie. "
            "TAB ou PAGE_SUIV / PAGE_PREC = asset suivant/precedent, 1..9 = rang "
            "direct. Fleches gauche/droite = tourner, haut/bas = echelle, "
            "DEBUT/FIN = monter/descendre par rapport au sol, K = solide <-> "
            "traversable, CTRL gauche = pas fin. F3/F4 = objet pose suivant/"
            "precedent (sans viser). RETOUR ARRIERE = annuler, "
            "F5 = sauvegarder, F9 = recharger, L = reafficher le catalogue.";

        // Resynchronise TOUS les fronts descendants, jeu et editeur confondus.
        //
        // Appelee a chaque bascule de mode, et pour un defaut precis : chaque
        // KeyEdge ne se met a jour que lorsqu'on l'interroge, or le jeu
        // n'interroge pas les touches d'editeur et reciproquement. Une touche
        // maintenue pendant la bascule apparaitrait donc comme un front
        // descendant a la premiere image du nouveau mode - entrer en edition
        // avec E enfonce selectionnerait un objet, en sortir avec E enfonce
        // abattrait un arbre. Les interroger tous une fois au moment de la
        // bascule fixe leur etat sur la realite du clavier.
        const auto syncAllKeyEdges = [&]() {
            fellKey.justPressed(window, kFellKey);
            pickupKey.justPressed(window, kPickupKey);
            knapKey.justPressed(window, kKnapKey);
            putDownKey.justPressed(window, kPutDownKey);
            paraglideKey.justPressed(window, kParaglideKey);
            bikeKey.justPressed(window, kBikeKey);
            climbKey.justPressed(window, kClimbKey);
            drinkKey.justPressed(window, kDrinkKey);
            digKey.justPressed(window, kDigKey);
            resinKey.justPressed(window, kResinKey);
            fiberKey.justPressed(window, kFiberKey);
            vegetationKey.justPressed(window, kVegetationKey);
            orthoKey.justPressed(window, kOrthoKey);

            editorPlaceButton.justPressed(
                window.isMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT));
            editorPlaceKey.justPressed(window, kEditorPlaceKey);
            editorPickKey.justPressed(window, kEditorPickKey);
            editorGrabKey.justPressed(window, kEditorGrabKey);
            editorDeleteKey.justPressed(window, kEditorDeleteKey);
            editorCancelKey.justPressed(window, kEditorCancelKey);
            editorNextKey.justPressed(window, kEditorNextKey);
            editorNextKey2.justPressed(window, kEditorNextKey2);
            editorPrevKey.justPressed(window, kEditorPrevKey);
            editorSolidKey.justPressed(window, kEditorSolidKey);
            editorUndoKey.justPressed(window, kEditorUndoKey);
            editorSaveKey.justPressed(window, kEditorSaveKey);
            editorReloadKey.justPressed(window, kEditorReloadKey);
            editorListKey.justPressed(window, kEditorListKey);
            editorCycleNextKey.justPressed(window, kEditorCycleNextKey);
            editorCyclePrevKey.justPressed(window, kEditorCyclePrevKey);
            for (int i = 0; i < static_cast<int>(editorQuickKeys.size()); ++i) {
                editorQuickKeys[static_cast<std::size_t>(i)].justPressed(
                    window, GLFW_KEY_1 + i);
            }
        };

        // Le titre porte le compteur d'inventaire : c'est le seul retour visuel
        // du ramassage a ce stade, conformement a la philosophie "zero menu".
        // On ne le reecrit que quand quelque chose change, un appel systeme par
        // frame pour rien etant inutile.
        std::string inventoryText = inventory.summary();
        bool titleDirty = true;
        long displayedFps = 0;

        // --- PHASE 19 : ou passe le temps CPU d'une image ? -----------------
        // Meme demarche que TerrainStreamer::buildProfile en Phase 18, appliquee
        // cette fois a l'IMAGE entiere : la Phase 18 avait montre que le poste
        // qu'on croit couteux ne l'est pas, et que le poste couteux n'est pas
        // celui qu'on regarde. Rien ici n'est optimise avant d'avoir ete mesure.
        //
        // Le chronometre est pris SANS condition : un profilage qu'on active a
        // la demande finit par mesurer un binaire different de celui qui tourne.
        // Le cout est de quelques dizaines de nanosecondes par borne, contre des
        // centaines de microsecondes par poste.
        struct FrameProfile {
            uint64_t frames = 0;
            uint64_t streamingMicros = 0;   // terrainStreamer->appendDrawItems
            uint64_t treesMicros = 0;       // Vegetation::gatherTrees
            uint64_t grassMicros = 0;       // Vegetation::gatherGrass
            uint64_t fiberMicros = 0;       // FiberPlants::gather (PHASE 22)
            uint64_t rocksMicros = 0;       // Rocks::gather (PHASE 29)
            uint64_t bushesMicros = 0;      // Bushes::gather (PHASE 29, Agent 3)
            uint64_t drawMicros = 0;        // Renderer::drawFrame
            uint64_t frustumMicros = 0;     // projection * vue + extraction des plans
            // PHASE 27 : la traduction meteo -> image (weatherVisualsFor et
            // les appels au renderer). Mesuree pour pouvoir AFFIRMER son cout
            // CPU au lieu de le supposer negligeable.
            uint64_t weatherVisualsMicros = 0;
        } frameProfile;
        using ProfileClock = std::chrono::steady_clock;
        const auto profileMicros = [](ProfileClock::time_point a,
                                      ProfileClock::time_point b) {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
        };

        // PHASE 27 : duree reelle de la boucle, pour que le bilan donne une
        // cadence (images / secondes de boucle) sans dependre du titre de la
        // fenetre, dont la Phase 18 a montre qu'il varie de ~15 % entre deux
        // runs identiques. Chargement exclu : il precede la boucle.
        const auto loopStart = ProfileClock::now();
        while (!window.shouldClose()) {
            window.pollEvents();

            const double now = glfwGetTime();
            const float deltaTime = static_cast<float>(now - lastTime);
            lastTime = now;

            // Temps ecoule depuis le demarrage, pousse aux shaders de vegetation
            // pour l'ondulation de l'herbe. glfwGetTime part de zero au
            // demarrage, donc la valeur reste petite et n'use pas la precision
            // d'un float, contrairement a une horloge absolue.
            const float elapsedSeconds = static_cast<float>(now);

            // --- Bascule du MODE EDITEUR (PHASE 15) ------------------------
            // Traitee AVANT camera.update : entrer en edition change le mode et
            // la vitesse de la camera, et on veut que ce soit effectif des cette
            // image plutot qu'a la suivante.
            //
            // Le front descendant est lu dans une variable AVANT le test, et non
            // dans la condition : place a droite d'un `||`, il serait
            // court-circuite l'image ou --editor declenche la bascule, et l'etat
            // de la touche F2 ne serait plus tenu a jour.
            const bool editorToggleEdge =
                editorToggleKey.justPressed(window, kEditorToggleKey);
            if (terrainScene && useScene && (editorToggleEdge || editorToggleRequested)) {
                editorToggleRequested = false;
                if (!editorMode &&
                    (paraglider.flying() || bike.riding() || climber.climbing())) {
                    // Refus plutot que sortie d'office de l'activite : l'editeur
                    // reprend la camera, et une activite en cours continuerait de
                    // lui imposer sa pose a chaque image. Les deux se battraient.
                    logInfo("Editeur : pas pendant une activite. Terminer le vol, "
                            "la descente ou l'escalade d'abord.");
                } else if (!editorMode) {
                    editorMode = true;
                    editorPreviousMode = camera.mode();
                    editorPreviousSpeed = camera.moveSpeed();
                    camera.setMode(CameraMode::Free);
                    camera.setMoveSpeed(editorFlySpeed);
                    // Sans ce verrou, la touche F - encore lue par la camera -
                    // reposerait l'editeur au sol en mode marche.
                    camera.setModeLocked(true);
                    editorController.markStatusChanged();
                    logInfo("=== MODE EDITEUR ===");
                    logInfo(kEditorHelp);
                    logInfo(editorCatalog.listing(editorController.brushIndex()));
                    titleDirty = true;
                    syncAllKeyEdges();
                } else {
                    editorMode = false;
                    editorController.resetInteraction();
                    camera.setModeLocked(false);
                    camera.setMoveSpeed(editorPreviousSpeed);
                    camera.setMode(editorPreviousMode);
                    logInfo("=== retour au JEU. Les objets poses restent affiches "
                            "et solides ; ils ne sont sur le DISQUE que si F5 a ete "
                            "presse en mode editeur. ===");
                    titleDirty = true;
                    syncAllKeyEdges();
                }
            }

            // ===============================================================
            // PHASE 16 : besoins primaires
            // ===============================================================
            // Place AVANT camera.update, et c'est le point qui compte : la
            // penalite de fatigue agit en reglant la vitesse de la camera, or
            // camera.update deplace le joueur avec la vitesse courante. Calculee
            // apres, la penalite ne mordrait qu'a l'image suivante - un decalage
            // invisible a l'oeil, mais qui rendrait toute mesure fausse d'une
            // image.
            //
            // Le MODE EDITEUR gele tout : c'est un outil de developpement, on y
            // survole 25 km2 en vol libre, et voir la fatigue tomber pendant
            // qu'on pose des rochers n'aurait aucun sens. Les jauges reprennent
            // exactement ou elles en etaient en sortant (F2).
            if (!editorMode) {
                if (!clockFrozen) {
                    clock.advance(deltaTime);
                }

                // Ou est REELLEMENT le joueur : chaque activite a sa propre
                // position, la camera ne suit qu'en chase-cam pour deux d'entre
                // elles. Prendre camera.position() sans distinguer donnerait
                // l'altitude de l'oeil qui suit le parapente, pas celle du
                // pilote.
                glm::vec3 playerPosition = camera.position();
                Activity activity = Activity::Walk;
                if (paraglider.flying()) {
                    playerPosition = paraglider.position();
                    activity = Activity::Paraglide;
                } else if (bike.riding()) {
                    playerPosition = bike.position();
                    activity = Activity::Bike;
                } else if (climber.climbing()) {
                    playerPosition = climber.anchor();
                    activity = Activity::Climb;
                } else {
                    // A pied : marche ou repos, selon qu'on a bouge ou non
                    // depuis l'image precedente. Le mode vol libre (F) compte
                    // comme du repos - ce n'est pas une facon de se deplacer
                    // dans le monde, c'est un mode d'inspection.
                    const glm::vec3 step = playerPosition - previousPlayerPosition;
                    const float horizontal =
                        std::sqrt(step.x * step.x + step.z * step.z);
                    const bool moving =
                        camera.mode() == CameraMode::Walk && deltaTime > 0.0f &&
                        horizontal / deltaTime > kRestSpeedThreshold;
                    activity = moving ? Activity::Walk : Activity::Rest;
                }
                previousPlayerPosition = camera.position();

                // --- PHASE 23 : la meteo de cette image ------------------
                // UNE seule evaluation par image, au point ou est reellement le
                // joueur. Tout ce qui suit - les jauges, les quatre activites,
                // la lumiere - relit CE resultat. C'est la traduction directe de
                // "une seule source de verite" : deux appels a sampleAt() dans
                // la meme image seraient deja deux occasions de diverger, en
                // plus d'un cout paye deux fois.
                skyState = weather.synopticAt(clock.totalHours());
                localWeather = weather.sampleAt(playerPosition, skyState, heightmap.get());

                // PHASE 21 : en mode DEMO, les besoins n'existent pas - ni la
                // jauge, ni son effet sur la vitesse de marche, ni l'action de
                // boire. La vitesse reste donc la reference constante kWalkSpeed
                // au lieu d'etre modulee par needs.movementFactor().
                if (!demoMode) {
                    NeedsContext needsContext;
                    needsContext.activity = activity;

                    // PHASE 23 : l'ambiante n'est plus calculee par Needs. Elle
                    // vient du systeme meteo, qui tient compte de l'altitude, de
                    // l'heure, de la saison, de la masse d'air, du versant et de
                    // l'inversion de fond de cuvette. Un seul calcul de
                    // temperature dans tout le jeu.
                    needsContext.ambientTemperatureC = localWeather.temperatureC;

                    // PHASE 28 (Agent 3) : chaleur des foyers ALLUMES. On corrige
                    // l'ambiante LOCALE au lieu d'ecrire une seconde formule :
                    // Needs la traite ensuite comme n'importe quelle ambiante
                    // (vent, effort, abri, marge au confort). 8 degres au centre,
                    // lineaire jusqu'a 0 a 6 m (HearthParams). Sans foyer : + 0,
                    // exactement l'ambiante meteo d'avant (scenario H intact).
                    needsContext.ambientTemperatureC += hearths.warmthAt(playerPosition);

                    // PHASE 28 (Agent 3) : premier appelant de sheltered. Un abri
                    // pose a moins de 2,5 m : +5 degres et vent annule (Needs.cpp).
                    needsContext.sheltered = ground && shelterNear(*ground, playerPosition);

                    // Vent RELATIF : au vent du lieu s'ajoute la vitesse propre
                    // du joueur quand elle compte. En parapente et a velo on
                    // fabrique son propre vent, et c'est ce qui refroidit - c'est
                    // le mecanisme reel derriere le forfait de -6 degres que la
                    // Phase 16 reservait au vol.
                    float relativeWind = localWeather.windMps;
                    if (paraglider.flying()) {
                        relativeWind += ParagliderParams{}.cruiseSpeed;
                    } else if (bike.riding()) {
                        relativeWind += bike.speed();
                    }
                    needsContext.windSpeedMps = relativeWind;

                    needs.update(deltaTime, needsContext);

                    // LA consequence, appliquee en un seul endroit pour les
                    // quatre activites : la marche ici, le pedalage et la montee
                    // en paroi plus bas (les deux multiplient leur commande par
                    // ce meme coefficient). Reappliquee a chaque image parce que
                    // le coefficient bouge en continu.
                    camera.setMoveSpeed(kWalkSpeed * needs.movementFactor());

                    // --- Boire -------------------------------------------
                    // Uniquement a pied et a portee d'un point d'eau detecte sur
                    // le relief. Le refus est journalise avec la distance : sans
                    // elle, "trop loin" ne dit pas s'il faut faire deux pas ou
                    // deux kilometres.
                    if (drinkKey.justPressed(window, kDrinkKey)) {
                        float distance = 0.0f;
                        const WaterSource* source =
                            nearestWaterSource(waterSources, playerPosition, &distance);
                        if (source == nullptr) {
                            logInfo("Boire : aucun point d'eau connu sur ce terrain.");
                        } else if (distance > kDrinkReach) {
                            logInfo("Boire : le point d'eau le plus proche est a " +
                                    std::to_string(distance) + " m (il faut etre a " +
                                    std::to_string(kDrinkReach) + " m).");
                        } else if (needs.drink()) {
                            logInfo("Boire : soif etanchee au point d'eau (" +
                                    std::to_string(distance) + " m). " +
                                    needs.statusLine());
                        } else {
                            logInfo("Boire : pas soif.");
                        }
                    }

                    // --- PHASE 22 : creuser un gisement ------------------
                    // Meme forme exactement que "boire" juste au-dessus : une
                    // portee horizontale, un refus JOURNALISE AVEC LA DISTANCE
                    // (sans elle, "trop loin" ne dit pas s'il faut faire deux
                    // pas ou deux kilometres), et rien qui ressemble a un menu.
                    //
                    // La matiere tombe AU SOL devant le joueur et non dans son
                    // inventaire : c'est la regle posee en Phase 6 par l'eclat
                    // de percussion, et elle vaut pour les trois matieres de
                    // cette phase. Ce qui est produit doit d'abord EXISTER
                    // quelque part.
                    if (digKey.justPressed(window, kDigKey)) {
                        float distance = 0.0f;
                        const MineralDeposit* deposit =
                            nearestDeposit(deposits, playerPosition, &distance);
                        if (deposit == nullptr) {
                            logInfo("Creuser : aucun gisement connu sur ce terrain.");
                        } else if (distance > kDigReach) {
                            logInfo(std::string("Creuser : le gisement de ") +
                                    depositLabel(deposit->kind) + " le plus proche "
                                    "est a " + std::to_string(distance) +
                                    " m (il faut etre a " + std::to_string(kDigReach) +
                                    " m).");
                        } else if (ground) {
                            const GroundItemKind kind =
                                (deposit->kind == DepositKind::Clay)
                                    ? GroundItemKind::Clay
                                    : GroundItemKind::Sand;
                            const glm::vec3 facing = camera.front();
                            glm::vec3 flat(facing.x, 0.0f, facing.z);
                            const float flatLength =
                                std::sqrt(flat.x * flat.x + flat.z * flat.z);
                            // Meme precaution qu'au taillage de pierre : on teste
                            // AVANT de normaliser. Normaliser un vecteur nul
                            // propagerait des NaN jusque dans la position de
                            // l'objet, qui disparaitrait du monde sans un mot.
                            flat = (flatLength > 1e-4f) ? flat / flatLength
                                                        : glm::vec3(1.0f, 0.0f, 0.0f);
                            ground->drop(kind, playerPosition.x + flat.x * 1.1f,
                                         playerPosition.z + flat.z * 1.1f,
                                         std::atan2(-flat.z, flat.x), 0.0f);
                            logInfo(std::string("Creuser : ") +
                                    groundItemLabelIndefinite(kind) +
                                    " se detache du gisement et tombe a vos pieds.");
                        }
                    }

                    // --- PHASE 22 : saigner un resineux ------------------
                    // La difference avec l'abattage (touche E) est le point de
                    // toute la mecanique, et elle est visible ici : on VISE le
                    // meme arbre, par le MEME Forest::pickTree et avec la meme
                    // portee - mais rien n'est abattu, rien n'est debite, et
                    // l'Habitat n'enregistre aucune coupe. L'arbre reste debout.
                    if (resinKey.justPressed(window, kResinKey)) {
                        if (!forest) {
                            logInfo("Resine : aucun arbre dans ce monde.");
                        } else {
                            const int target = forest->pickTree(
                                camera.position(), camera.front(), kFellingReach);
                            // PHASE 26 : l'essence vient de L'ARBRE VISE, celle-la
                            // meme dont la silhouette est dessinee a l'ecran. Un
                            // arbre conique donne de la resine, un arbre a
                            // houppier rond n'en donne pas, et le joueur le voit
                            // AVANT d'appuyer - c'est la dette de la Phase 22 qui
                            // se referme ici, au point d'usage.
                            const TreeSpecies aimedSpecies =
                                (target >= 0)
                                    ? forest->trees()[static_cast<std::size_t>(target)].species
                                    : TreeSpecies::Broadleaf;
                            const ResinResult result =
                                resinStand.tap(target, aimedSpecies, elapsedSeconds);
                            logInfo(resinResultMessage(result));
                            if (result == ResinResult::StillDry && target >= 0) {
                                const float wait =
                                    resinStand.secondsUntilFlow(target, elapsedSeconds);
                                logInfo("  entailles deja faites : " +
                                        std::to_string(resinStand.tapCount(target)) +
                                        (wait > 0.0f
                                             ? ", il faut encore " +
                                                   std::to_string(wait) + " s"
                                             : ", arbre epuise"));
                            }
                            if (result == ResinResult::Success && ground) {
                                const glm::vec3 base = forest->trees()[
                                    static_cast<std::size_t>(target)].base;
                                ground->drop(GroundItemKind::Resin, base.x, base.z,
                                             0.0f, 0.0f);
                            }
                        }
                    }

                    // --- PHASE 22 : arracher une plante a fibres ---------
                    // La seule des trois qui RETIRE quelque chose du monde : la
                    // plante disparait du semis et met sept minutes a repousser,
                    // a la meme place et a la meme taille (le semis est une
                    // fonction deterministe de la cellule, donc rien ne se
                    // perd). Les gisements et les arbres, eux, restent.
                    if (fiberKey.justPressed(window, kFiberKey)) {
                        if (!fiberPlants) {
                            logInfo("Fibres : aucun semis de plantes sur ce terrain.");
                        } else {
                            FiberPlant pulled;
                            if (fiberPlants->pullNearest(playerPosition, kPullReach,
                                                         &pulled)) {
                                if (ground) {
                                    ground->drop(GroundItemKind::Fiber,
                                                 pulled.position.x, pulled.position.z,
                                                 pulled.yaw, 0.0f);
                                }
                                logInfo("Fibres : la tige cede, un fagot tombe a vos "
                                        "pieds. La plante repoussera ici meme.");
                            } else {
                                logInfo("Fibres : aucune plante a fibres a portee "
                                        "(il faut etre a " + std::to_string(kPullReach) +
                                        " m). Elles poussent en lisiere de foret et sur "
                                        "les coupes.");
                            }
                        }
                    }
                } else {
                    camera.setMoveSpeed(kWalkSpeed);
                    // Fronts tenus a jour a vide : sans ca, revenir a la marche en
                    // tenant l'une de ces touches declencherait le geste au
                    // premier relachement.
                    drinkKey.justPressed(window, kDrinkKey);
                    digKey.justPressed(window, kDigKey);
                    resinKey.justPressed(window, kResinKey);
                    fiberKey.justPressed(window, kFiberKey);
                }

                // ===========================================================
                // PHASE 17 : empreinte environnementale
                // ===========================================================
                // Trois lignes, et tout le systeme est la. Elles sont placees
                // DANS le bloc des besoins parce qu'elles ont besoin d'exactement
                // les memes deux grandeurs, deja calculees juste au-dessus : ou
                // est REELLEMENT le joueur (et non ou est la camera, qui suit en
                // chase-cam pendant deux activites) et CE QU'IL FAIT.
                //
                // Les recalculer ici les ferait diverger le jour ou une
                // cinquieme activite arrivera. Consequence assumee : comme les
                // besoins, l'empreinte est GELEE en mode editeur - survoler
                // 25 km2 en vol libre pour poser des rochers ne doit pas
                // effrayer la faune de toute la vallee.
                if (useWildlife) {
                    habitat.notePresence(playerPosition, deltaTime,
                                         activityNoise(activity));
                    habitat.update(deltaTime);
                    if (wildlife) {
                        wildlife->update(deltaTime, playerPosition, habitat);
                    }
                }

                // PHASE 22 : la repousse des plantes arrachees. Volontairement
                // HORS du bloc `useWildlife` ci-dessus : --no-wildlife est le
                // temoin de la faune, pas une raison pour qu'une ronce cesse de
                // repousser. Elle est en revanche dans le bloc des besoins,
                // donc GELEE en mode editeur, pour la meme raison qu'eux.
                if (fiberPlants) {
                    fiberPlants->update(deltaTime);
                }

                // Le titre n'est PAS marque sale ici. Il l'est deja une fois
                // par seconde par le compteur de FPS, et les jauges y sont
                // affichees en pourcentages ENTIERS : les rafraichir soixante
                // fois par seconde ferait soixante appels systeme pour montrer
                // le meme texte.
            } else {
                // Le front descendant de la touche est tenu a jour a vide, comme
                // toutes les touches de jeu pendant l'edition (voir
                // syncAllKeyEdges) : sans ca, sortir de l'editeur en tenant T
                // ferait boire au premier relachement.
                drinkKey.justPressed(window, kDrinkKey);
                digKey.justPressed(window, kDigKey);
                resinKey.justPressed(window, kResinKey);
                fiberKey.justPressed(window, kFiberKey);
            }

            camera.update(window, deltaTime);

            // --- PHASE 24 : la pente a-t-elle arrete le marcheur ? -----------
            // Journalise UNIQUEMENT les CHANGEMENTS d'etat (marchable <-> trop
            // raide, glisse <-> tient debout). Ce n'est pas de l'economie de
            // place : le harnais de verification paie un cout par ligne de
            // journal et son retard croit avec le volume - une ligne par frame
            // ferait echouer des scenarios entiers pour un jeu correct, ce qui
            // s'est deja produit en Phase 23. Deux ou trois lignes par approche
            // de paroi suffisent a dire ce qui s'est passe.
            if (camera.mode() == CameraMode::Walk) {
                const WalkStepResult& walkStep = camera.lastWalkStep();
                const bool blocked = walkStep.speedFactor <= 0.0f;

                // LIMITEUR DE CADENCE, et ce n'est pas du confort.
                //
                // "Journaliser sur changement d'etat" ne suffit PAS : un joueur
                // qui longe du terrain juste au seuil bascule d'un cote a
                // l'autre a chaque frame, et chaque bascule est un changement
                // d'etat legitime. Premiere mesure sur la suite complete : 494
                // lignes, et le scenario I a perdu 40 % de sa telemetrie (150
                // releves au lieu de 254).
                //
                // C'est le meme defaut qu'en Phase 23, une cadence de journal
                // qui fait echouer un scenario pour un jeu parfaitement
                // correct - et il s'est reproduit ici parce que "sur
                // changement" SEMBLE deja econome. Un etat qui BAT n'est pas un
                // evenement.
                //
                // Une seconde entre deux lignes : assez pour qu'une vraie
                // approche de paroi laisse une trace lisible, assez pour qu'un
                // battement a 60 Hz coute au plus une ligne par seconde.
                constexpr double kWalkLogMinInterval = 1.0;
                const bool walkLogAllowed =
                    (now - walkLogLastTime) >= kWalkLogMinInterval;

                if (blocked != walkBlockedWasLogged && walkLogAllowed) {
                    walkLogLastTime = now;
                    walkBlockedWasLogged = blocked;
                    logInfo(std::string("Marche : ") +
                            (blocked ? "pente trop raide, progression arretee ("
                                     : "pente de nouveau franchissable (") +
                            std::to_string(100.0f * walkStep.slopeAhead) +
                            " % devant, limite " +
                            std::to_string(100.0f * camera.walkSlopeParams().maxSlope) +
                            " %).");
                }
                if (walkStep.sliding != walkSlidingWasLogged && walkLogAllowed) {
                    walkLogLastTime = now;
                    walkSlidingWasLogged = walkStep.sliding;
                    logInfo(std::string("Marche : ") +
                            (walkStep.sliding ? "le joueur GLISSE vers l'aval ("
                                              : "le joueur retrouve son appui (") +
                            std::to_string(100.0f * walkStep.slopeUnderFoot) +
                            " % sous les pieds).");
                }
            }

            // --- Survol scripte (PHASE 12) ----------------------------------
            // Reproduit le scenario qui a motive le chantier : traverser la zone
            // de 25 km2 assez vite pour que le streaming n'ait pas le temps de
            // suivre. Scripte plutot que pilote au clavier parce que le harnais
            // de test ne peut pas commander la souris (voir run_vulkan_check.ps1)
            // et parce qu'une trajectoire reproductible est la condition pour
            // comparer deux runs.
            if (flythroughSpeed > 0.0f && heightmap) {
                glm::vec3 position = camera.position();
                const glm::vec3 front = camera.front();
                glm::vec3 heading(front.x, 0.0f, front.z);
                const float headingLength = glm::length(heading);
                // Camera exactement verticale : cap indefini, on garde l'axe X.
                heading = (headingLength > 1e-4f) ? heading / headingLength
                                                  : glm::vec3(1.0f, 0.0f, 0.0f);

                position += heading * flythroughSpeed * deltaTime;

                // Virage de 90 degres a l'approche du bord : la camera reste dans
                // l'emprise indefiniment, donc la duree du test ne depend plus de
                // la taille du terrain. La marge evite de tourner PILE sur le
                // bord, ou la moitie du champ de vision serait hors donnees.
                const float limit = 0.5f * heightmap->worldSize() - 250.0f;
                if (std::abs(position.x) > limit || std::abs(position.z) > limit) {
                    position.x = std::clamp(position.x, -limit, limit);
                    position.z = std::clamp(position.z, -limit, limit);
                    camera.setOrientation(camera.yawDegrees() + 90.0f,
                                          camera.pitchDegrees());
                }

                position.y = heightmap->heightAt(position.x, position.z) +
                             flythroughAltitude;
                camera.setPosition(position);
            }

            // --- MODE EDITEUR : entrees, placement, previsualisation --------
            // Tout le mode tient dans ce bloc, et il est EXCLUSIF : tant qu'il
            // est actif, aucune touche de jeu n'est lue (voir les gardes
            // !editorMode ci-dessous). C'est ce qui autorise le mode edition a
            // reprendre des lettres deja prises par le jeu.
            if (editorMode && heightmap) {
                // Molette : vitesse de vol. Multiplicative, comme le reglage
                // d'echelle de l'editeur et pour la meme raison - le rapport
                // entre 5 et 10 m/s est le meme geste que celui entre 200 et
                // 400, alors qu'un pas additif rendrait l'un des deux inutile.
                const double scroll = window.consumeScroll();
                if (scroll != 0.0) {
                    editorFlySpeed = std::clamp(
                        editorFlySpeed *
                            std::pow(kEditorFlySpeedStep, static_cast<float>(scroll)),
                        kEditorFlySpeedMin, kEditorFlySpeedMax);
                    camera.setMoveSpeed(editorFlySpeed);
                    logInfo("Editeur : vitesse de vol " +
                            std::to_string(editorFlySpeed) + " m/s.");
                }

                EditorInput editorInput;
                editorInput.deltaTime = deltaTime;
                editorInput.now = elapsedSeconds;
                // MEME rayon que la visee d'arbre du jeu (Forest::pickTree est
                // appele plus bas avec ces deux memes valeurs) : le reticule de
                // l'editeur est donc exactement celui du joueur.
                editorInput.eye = camera.position();
                editorInput.forward = camera.front();

                // --- LA SOURIS EST COUPEE PAR --lock-input -----------------
                //
                // Defaut REEL, constate pendant la mise au point du scenario G :
                // une execution complete du harnais a pose NEUF objets pour trois
                // touches envoyees, et le scenario joue seul en posait trois. La
                // difference n'etait ni le code, ni le timing - c'est que la
                // suite dure huit minutes, que chaque processus prend le focus en
                // demarrant, et que les clics REELS de l'utilisateur atterrissent
                // alors dans la fenetre du jeu.
                //
                // C'est exactement le probleme de la Phase 11, qui avait donne
                // --lock-input : "le jeu lit le clavier et la souris SYSTEME".
                // Sauf que ce verrou-la ne couvrait que la CAMERA, l'editeur
                // n'existant pas encore. Reproduit a la demande : trois
                // WM_LBUTTONDOWN postes dans une fenetre lancee avec
                // --lock-input posent trois objets.
                //
                // Nuance importante sur GLFW : la position du curseur vient de
                // GetCursorPos(), donc un WM_MOUSEMOVE poste n'a aucun effet -
                // c'est la limite deja documentee. Mais l'ETAT DES BOUTONS, lui,
                // est mis en cache depuis les messages de fenetre. Les boutons
                // sont donc atteignables la ou le curseur ne l'est pas, ce qui
                // est precisement ce qui rendait ce defaut invisible.
                //
                // On ne coupe QUE la souris, et pas tout : le clavier doit rester
                // vivant pour qu'un scenario puisse piloter l'editeur camera
                // figee. C'est la raison d'etre de la touche INSER.
                // Le front descendant est calcule DANS TOUS LES CAS et seul son
                // RESULTAT est filtre : un `&&` court-circuite, et laisser
                // l'etat du bouton non tenu a jour reintroduirait un front
                // parasite le jour ou le verrou deviendrait basculable.
                const bool clickEdge = editorPlaceButton.justPressed(
                    window.isMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT));
                const bool placeByClick = clickEdge && !lockInput;
                const bool placeByKey =
                    editorPlaceKey.justPressed(window, kEditorPlaceKey);
                editorInput.placePressed = placeByClick || placeByKey;
                editorInput.pickPressed =
                    editorPickKey.justPressed(window, kEditorPickKey);
                editorInput.grabPressed =
                    editorGrabKey.justPressed(window, kEditorGrabKey);
                editorInput.deletePressed =
                    editorDeleteKey.justPressed(window, kEditorDeleteKey);
                editorInput.cancelPressed =
                    editorCancelKey.justPressed(window, kEditorCancelKey);
                // Les deux touches "asset suivant" sont interrogees AVANT d'etre
                // combinees : un || court-circuiterait la seconde et son front
                // descendant ne serait plus tenu a jour.
                const bool nextA = editorNextKey.justPressed(window, kEditorNextKey);
                const bool nextB = editorNextKey2.justPressed(window, kEditorNextKey2);
                editorInput.nextAssetPressed = nextA || nextB;
                editorInput.prevAssetPressed =
                    editorPrevKey.justPressed(window, kEditorPrevKey);
                editorInput.toggleSolidPressed =
                    editorSolidKey.justPressed(window, kEditorSolidKey);
                editorInput.undoPressed =
                    editorUndoKey.justPressed(window, kEditorUndoKey);
                editorInput.savePressed =
                    editorSaveKey.justPressed(window, kEditorSaveKey);
                editorInput.reloadPressed =
                    editorReloadKey.justPressed(window, kEditorReloadKey);
                editorInput.listPressed =
                    editorListKey.justPressed(window, kEditorListKey);
                editorInput.cycleNextPressed =
                    editorCycleNextKey.justPressed(window, kEditorCycleNextKey);
                editorInput.cyclePrevPressed =
                    editorCyclePrevKey.justPressed(window, kEditorCyclePrevKey);
                for (int i = 0; i < static_cast<int>(editorQuickKeys.size()); ++i) {
                    if (editorQuickKeys[static_cast<std::size_t>(i)].justPressed(
                            window, GLFW_KEY_1 + i)) {
                        editorInput.quickPick = i;
                    }
                }

                editorInput.holdRotateLeft = window.isKeyPressed(GLFW_KEY_LEFT);
                editorInput.holdRotateRight = window.isKeyPressed(GLFW_KEY_RIGHT);
                editorInput.holdScaleUp = window.isKeyPressed(GLFW_KEY_UP);
                editorInput.holdScaleDown = window.isKeyPressed(GLFW_KEY_DOWN);
                editorInput.holdRaise = window.isKeyPressed(GLFW_KEY_HOME);
                editorInput.holdLower = window.isKeyPressed(GLFW_KEY_END);
                editorInput.holdFineStep = window.isKeyPressed(kEditorFineKey);

                editorController.update(editorInput, *heightmap,
                                        0.5f * heightmap->worldSize());

                // Le controleur ne fait aucune entree/sortie : il leve des
                // drapeaux, on les traite ici ou le chemin de scene et le
                // georeferencement sont connus.
                if (editorController.consumeListRequest()) {
                    logInfo(editorCatalog.listing(editorController.brushIndex()));
                }
                if (editorController.consumeSaveRequest()) {
                    if (scenePath.empty()) {
                        logWarn("Scene : aucun chemin de sauvegarde (--no-scene ?).");
                    } else if (saveSceneFile(scenePath, editorScene.objects(),
                                             *heightmap, sceneTerrainLabel)) {
                        logInfo("Scene : " + std::to_string(editorScene.size()) +
                                " objet(s) ecrits dans " + scenePath + ".");
                    }
                }
                if (editorController.consumeReloadRequest()) {
                    logInfo("Scene : rechargement demande (F9). Les modifications "
                            "non sauvegardees sont perdues.");
                    loadScene();
                    editorController.resetInteraction();
                }
                if (editorController.consumeStatusChanged()) {
                    logInfo(editorController.statusLine());
                }

                // Les fronts descendants du JEU sont tenus a jour a vide : voir
                // syncAllKeyEdges pour la raison, qui vaut aussi image par image
                // et pas seulement au moment de la bascule.
                fellKey.justPressed(window, kFellKey);
                pickupKey.justPressed(window, kPickupKey);
                knapKey.justPressed(window, kKnapKey);
                putDownKey.justPressed(window, kPutDownKey);
                paraglideKey.justPressed(window, kParaglideKey);
                bikeKey.justPressed(window, kBikeKey);
                climbKey.justPressed(window, kClimbKey);
                vegetationKey.justPressed(window, kVegetationKey);
            }

            // --- Parapente : decollage/atterrissage, vol, camera chase-cam ---
            // PHASE 21 : hors sujet en mode demo (voir demoMode plus haut).
            if (!editorMode && !demoMode && heightmap &&
                paraglideKey.justPressed(window, kParaglideKey) &&
                !bike.riding() && !climber.climbing()) {
                if (paraglider.flying()) {
                    // Filet de securite : abandon volontaire du vol, pose
                    // immediate a l'endroit courant. Utile si le relief ne
                    // finit jamais par rejoindre l'altitude de vol (grand
                    // plateau, ou vol lance au-dessus d'un gouffre).
                    paraglider.abort();
                    camera.setPosition(glm::vec3(paraglider.position().x,
                                                 paraglider.position().y,
                                                 paraglider.position().z));
                    camera.setMode(CameraMode::Walk);
                    logInfo("Parapente : vol interrompu (atterrissage force).");
                } else if (camera.mode() == CameraMode::Free) {
                    const float groundY = heightmap->heightAt(camera.position().x,
                                                              camera.position().z);
                    const float aboveGround = camera.position().y - groundY;
                    if (aboveGround >= kMinLaunchHeightAboveTerrain) {
                        const float headingDeg =
                            glm::degrees(std::atan2(camera.front().z, camera.front().x));
                        paraglider.launch(camera.position(), headingDeg);
                        camera.setMode(CameraMode::Glide);
                        logInfo("Decollage en parapente, cap " +
                                std::to_string(headingDeg) + " degres.");
                    } else {
                        logInfo("Parapente : pas assez de hauteur pour decoller (" +
                                std::to_string(aboveGround) + " m au-dessus du sol, il en "
                                "faut " + std::to_string(kMinLaunchHeightAboveTerrain) + ").");
                    }
                } else {
                    logInfo("Parapente : decolle depuis le mode vol libre (F), en hauteur.");
                }
            }

            if (paraglider.flying()) {
                const float turnInput = (window.isKeyPressed(GLFW_KEY_D) ? 1.0f : 0.0f) -
                                        (window.isKeyPressed(GLFW_KEY_A) ? 1.0f : 0.0f);
                const float brakeInput = window.isKeyPressed(GLFW_KEY_S) ? 1.0f : 0.0f;
                // PHASE 23 : l'aile vole DANS une masse d'air. Le vent la fait
                // deriver, les ascendances la font monter, la turbulence de
                // sillage la chahute. L'air est celui deja echantillonne pour
                // cette image, au point du pilote.
                AirState air;
                air.wind = localWeather.windVector;
                air.vertical = localWeather.verticalMps;
                air.turbulence = localWeather.turbulence;
                paraglider.update(deltaTime, turnInput, brakeInput, air);

                if (heightmap && paraglider.checkLanding(*heightmap, kDefaultEyeHeight)) {
                    camera.setPosition(paraglider.position());
                    camera.setMode(CameraMode::Walk);
                    logInfo("Parapente : atterrissage.");
                } else {
                    // Chase-cam : l'oeil recule derriere le pilote le long de son
                    // cap et se souleve un peu, en visant le pilote lui-meme -
                    // c'est ce qui donne une vue a la troisieme personne lisible
                    // du mouvement (voir Camera::setChasePose).
                    const glm::vec3 back = -paraglider.forward();
                    const glm::vec3 eye = paraglider.position() +
                                          back * kGlideChaseDistance +
                                          glm::vec3(0.0f, kGlideChaseHeight, 0.0f);
                    camera.setChasePose(eye, paraglider.position());
                }
            }

            // --- Velo : monter/descendre, rouler, camera de poursuite (PHASE 13)
            //
            // Meme decoupage que le parapente juste au-dessus, et volontairement :
            // une touche a front descendant pour entrer/sortir de l'activite, la
            // physique dans world/, la camera pilotee depuis ici. Ce qui change
            // est que le velo n'a pas de condition d'altitude - on l'enfourche la
            // ou on marche - et qu'il ne se termine jamais tout seul : il n'y a
            // pas d'equivalent de l'atterrissage, c'est le joueur qui descend.
            if (!editorMode && !demoMode && heightmap && bikeKey.justPressed(window, kBikeKey) &&
                !paraglider.flying() && !climber.climbing()) {
                if (bike.riding()) {
                    bike.dismount();
                    camera.setPosition(bike.position());
                    camera.setMode(CameraMode::Walk);
                    logInfo("Velo : pied a terre.");
                } else if (camera.mode() == CameraMode::Walk) {
                    // Cap de depart = la direction du regard projetee a
                    // l'horizontale, comme au decollage en parapente : on part
                    // dans la direction ou on regardait, pas vers +X.
                    const float headingDeg =
                        glm::degrees(std::atan2(camera.front().z, camera.front().x));
                    bike.mount(camera.position(), headingDeg, *heightmap);
                    camera.setMode(CameraMode::Ride);
                    logInfo("Velo : en selle, cap " + std::to_string(headingDeg) +
                            " degres. Z pedaler, S freiner, Q/D tourner, B pour "
                            "descendre.");
                } else {
                    logInfo("Velo : on enfourche depuis la MARCHE (touche F pour y "
                            "revenir).");
                }
            }

            if (bike.riding()) {
                const float steerInput = (window.isKeyPressed(GLFW_KEY_D) ? 1.0f : 0.0f) -
                                         (window.isKeyPressed(GLFW_KEY_A) ? 1.0f : 0.0f);
                // PHASE 16 : la commande de pedalage est MULTIPLIEE par le
                // coefficient des besoins. Le modele du velo n'a pas ete touche
                // - il ecrete deja ses entrees dans [0, 1] et une commande plus
                // faible y produit exactement ce qu'on veut : moins
                // d'acceleration, donc une vitesse d'equilibre plus basse, donc
                // moins de distance ET une pente franchissable plus faible. Un
                // joueur epuise cale la ou il passait le matin.
                const float pedalInput =
                    (window.isKeyPressed(GLFW_KEY_W) ? 1.0f : 0.0f) *
                    needs.movementFactor();
                const float brakeInput = window.isKeyPressed(GLFW_KEY_S) ? 1.0f : 0.0f;

                // Le solveur composite est passe tel quel : c'est EXACTEMENT
                // le meme objet que celui de camera.setCollisionSolver() plus
                // haut, donc le velo bute sur les memes troncs, les memes souches
                // et - depuis la Phase 15 - les memes rochers poses que le joueur
                // a pied, par le meme moveBody().
                // PHASE 23 : etat du sol. La mouillure vient de la pluie des
                // dernieres heures de jeu, RELUE dans la fonction meteo (elle
                // n'est pas accumulee - voir WeatherSystem::groundWetness).
                bike.update(deltaTime, steerInput, pedalInput, brakeInput,
                            *heightmap, &worldSolver, localWeather.wetness);

                // Camera de poursuite : l'oeil recule derriere le velo le long de
                // son cap et se souleve, en visant le velo. Meme construction que
                // la chase-cam du parapente, a une contrainte pres : on refuse de
                // laisser l'oeil passer sous la surface (voir
                // kRideChaseMinClearance).
                const glm::vec3 back = -bike.forward();
                glm::vec3 eye = bike.position() + back * kRideChaseDistance +
                                glm::vec3(0.0f, kRideChaseHeight, 0.0f);
                const float minEyeY = heightmap->heightAt(eye.x, eye.z) +
                                      kRideChaseMinClearance;
                eye.y = std::max(eye.y, minEyeY);
                camera.setChasePose(eye, bike.position());
            }

            // --- Escalade : engagement, progression, sortie (PHASE 14) ------
            //
            // Meme decoupage que le parapente et le velo : une touche a front
            // descendant pour entrer/sortir, la physique dans world/, la camera
            // pilotee depuis ici. Deux choses la distinguent :
            //  - l'engagement peut ECHOUER (il faut une paroi a portee), donc
            //    Climber::tryStart renvoie un booleen la ou mount() ne pouvait
            //    pas refuser ;
            //  - l'escalade se termine toute seule de TROIS facons distinctes,
            //    la ou le velo ne se terminait jamais et le parapente d'une
            //    seule (l'atterrissage).
            if (!editorMode && !demoMode && heightmap && climbKey.justPressed(window, kClimbKey) &&
                !paraglider.flying() && !bike.riding()) {
                if (climber.climbing()) {
                    handleClimbExit(climber.release(), climber, camera);
                } else if (camera.mode() == CameraMode::Walk) {
                    if (climber.tryStart(*heightmap, camera.position(),
                                         camera.front())) {
                        camera.setMode(CameraMode::Climb);
                        logInfo("Escalade : engage sur la paroi (pente " +
                                std::to_string(100.0f * climber.slopeAt(
                                    *heightmap, climber.anchor().x,
                                    climber.anchor().z)) +
                                " %), energie " +
                                std::to_string(100.0f * climber.stamina()) +
                                " %. Z monter, S descendre, Q/D traverser, X "
                                "pour lacher.");
                    } else if (climber.stamina() < Climber::minStaminaToStart()) {
                        logInfo("Escalade : trop epuise pour s'engager (energie " +
                                std::to_string(100.0f * climber.stamina()) +
                                " %, il en faut " +
                                std::to_string(100.0f * Climber::minStaminaToStart()) +
                                " %).");
                    } else {
                        logInfo("Escalade : aucune paroi grimpable a portee "
                                "(il faut une pente d'au moins " +
                                std::to_string(100.0f * climber.params().wallSlope) +
                                " % a moins de " +
                                std::to_string(climber.params().reach) +
                                " m devant soi).");
                    }
                } else {
                    logInfo("Escalade : on s'engage depuis la MARCHE (touche F pour "
                            "y revenir).");
                }
            }

            if (climber.climbing()) {
                // Commandes RELATIVES A LA PAROI, pas au regard : Z/S suivent la
                // ligne de plus grande pente, Q/D la ligne de niveau. C'est ce
                // qui permet a la souris de rester libre en mode Climb (voir
                // Camera::update).
                // PHASE 16 : meme multiplication que pour le pedalage, et la
                // desescalade est ralentie AUSSI (le coefficient s'applique a la
                // commande entiere, signe compris). C'est voulu : redescendre
                // une paroi epuise autant que la monter, et une desescalade qui
                // resterait a pleine vitesse serait la faille evidente du
                // systeme - on monterait vite fait, on redescendrait gratis.
                const float climbInput =
                    ((window.isKeyPressed(GLFW_KEY_W) ? 1.0f : 0.0f) -
                     (window.isKeyPressed(GLFW_KEY_S) ? 1.0f : 0.0f)) *
                    needs.movementFactor();
                const float traverseInput = (window.isKeyPressed(GLFW_KEY_D) ? 1.0f : 0.0f) -
                                            (window.isKeyPressed(GLFW_KEY_A) ? 1.0f : 0.0f);

                // Le solveur composite est passe tel quel : le grimpeur bute
                // sur les memes obstacles que le joueur a pied et que le velo -
                // troncs, souches et objets poses - par le meme moveBody(). Rien
                // n'est reecrit.
                // PHASE 23 : adherence de la paroi. Une paroi mouillee se
                // grimpe plus lentement ET coute plus cher au metre - les deux,
                // parce que ralentir seul ferait paradoxalement DEPENSER moins
                // d'energie sur une paroi difficile.
                const ClimbExit exit =
                    climber.update(deltaTime, climbInput, traverseInput, *heightmap,
                                   &worldSolver,
                                   climber.gripForWetness(localWeather.wetness));
                if (exit == ClimbExit::None) {
                    // Premiere personne : la camera EST le grimpeur. On n'impose
                    // que la position - l'orientation reste au joueur, c'est tout
                    // l'interet du mode Climb.
                    camera.setPosition(climber.position());
                } else {
                    handleClimbExit(exit, climber, camera);
                }
            } else {
                // Recuperation : uniquement au sol, jamais en paroi. C'est ce qui
                // fait de la jauge une contrainte de trajet et pas un compteur.
                climber.restOnGround(deltaTime);
            }

            // --- Bascule de la vegetation de decor --------------------------
            if (!editorMode && vegetation &&
                vegetationKey.justPressed(window, kVegetationKey)) {
                vegetationMode = vegetationMode == VegetationMode::All
                    ? VegetationMode::TreesOnly
                    : (vegetationMode == VegetationMode::TreesOnly ? VegetationMode::None
                                                                  : VegetationMode::All);
                logInfo(vegetationMode == VegetationMode::All
                            ? "Vegetation : arbres et herbe."
                            : (vegetationMode == VegetationMode::TreesOnly
                                   ? "Vegetation : arbres seuls."
                                   : "Vegetation : aucune."));
            }

            // --- Bascule orthophoto / coloration procedurale ----------------
            // Disponible meme en vol : c'est un reglage d'affichage, pas une
            // action de jeu.
            if (orthoKey.justPressed(window, kOrthoKey) && renderer.hasTerrainTexture()) {
                const float next = renderer.orthoMix() > 0.5f ? 0.0f : 1.0f;
                renderer.setOrthoMix(next);
                logInfo(next > 0.5f ? "Affichage : orthophoto."
                                    : "Affichage : coloration procedurale (altitude/pente).");
            }

            // PHASE 21 : abattage/ramassage/percussion coupes en mode demo -
            // l'artisanat n'est pas interactif, mais foret et sol restent en
            // place, visibles et solides (voir demoMode plus haut).
            if (!editorMode && !demoMode && forest && ground && !paraglider.flying() &&
                !bike.riding() && !climber.climbing()) {
                // --- Abattage -------------------------------------------
                if (fellKey.justPressed(window, kFellKey)) {
                    const int target = forest->pickTree(camera.position(), camera.front(),
                                                        kFellingReach);
                    if (target == kNoTree) {
                        logInfo("Abattage : aucun arbre dans la ligne de visee.");
                    } else {
                        Clearance clearance;
                        const glm::vec3 stump =
                            forest->trees()[static_cast<std::size_t>(target)].base;
                        forest->fell(target, &clearance);

                        // PHASE 17 : la coupe est INSCRITE dans le milieu, au
                        // pied de l'arbre et non a la position du joueur - on
                        // peut abattre a trente metres, et c'est l'arbre qui
                        // manque, pas le bucheron.
                        //
                        // Rien n'est dit au joueur. La consequence se voit sur
                        // place, plus tard : le sol s'y denude et la faune n'y
                        // revient pas (voir NOTES-PHASE17.md).
                        habitat.recordFelling(stump);

                        logInfo("Abattage de l'arbre " + std::to_string(target) +
                                " : chute vers (" +
                                std::to_string(clearance.direction.x) + ", " +
                                std::to_string(clearance.direction.z) + "), " +
                                std::to_string(clearance.distance) +
                                " unites degagees, obstacle le plus proche : " +
                                clearance.obstacle +
                                (clearance.cramped ? " [ZONE ETROITE]" : ""));
                    }
                }

                // --- Ramassage ------------------------------------------
                if (pickupKey.justPressed(window, kPickupKey)) {
                    const int target = ground->findNearest(camera.position(), kPickupReach);
                    if (target == kNoGroundItem) {
                        logInfo("Ramassage : rien a portee de main.");
                    } else {
                        GroundItemKind picked = GroundItemKind::Log;
                        ground->pickUp(target, &picked);
                        inventory.add(picked);
                        lastPickedKind = picked;
                        inventoryText = inventory.summary();
                        titleDirty = true;
                        logInfo(std::string("Ramasse : ") +
                                groundItemLabelIndefinite(picked) +
                                ". Inventaire : " + inventoryText);
                    }
                }

                // --- Taillage de pierre par percussion -------------------
                // PHASE 28 : la meme touche C devient le geste generique de
                // FACONNAGE (world/Crafting). La percussion y est la derniere
                // recette de la table : avec deux pierres en poche et rien de
                // pose autour, le comportement et les messages sont ceux de la
                // Phase 6, mot pour mot.
                if (knapKey.justPressed(window, kKnapKey)) {
                    const CraftOutcome outcome =
                        craftAt(inventory, *ground, hearths, camera.position(),
                                camera.front(), clock.totalHours());
                    logInfo(outcome.message);
                    if (outcome.result == CraftResult::Success) {
                        inventoryText = inventory.summary();
                        titleDirty = true;
                        const Recipe& done = kRecipes[static_cast<std::size_t>(outcome.recipe)];
                        if (done.hasOutput) {
                            logInfo("Inventaire : " + inventoryText + " (" +
                                    groundItemLabelIndefinite(done.out) +
                                    (groundItemIsPortable(done.out)
                                         ? " est au sol, R pour le ramasser)."
                                         : " est construit ici, il ne se deplace pas)."));
                        } else {
                            logInfo("Inventaire : " + inventoryText + ".");
                        }
                        const int nearHearth =
                            hearths.nearest(camera.position(), kStationReach, true);
                        if (nearHearth >= 0) {
                            logInfo("  foyer allume, combustion restante : " +
                                    std::to_string(hearths.remainingHours(
                                        nearHearth, clock.totalHours())) +
                                    " h de jeu.");
                        }
                    }
                }

                // --- PHASE 28 : reposer un objet porte (P) ------------------
                if (putDownKey.justPressed(window, kPutDownKey)) {
                    GroundItemKind kind = GroundItemKind::Stone;
                    if (!chooseKindToPutDown(inventory, lastPickedKind, &kind)) {
                        logInfo("Poser : vous ne portez rien.");
                    } else if (putDown(inventory, *ground, kind, camera.position(),
                                       camera.front())) {
                        inventoryText = inventory.summary();
                        titleDirty = true;
                        logInfo(std::string("Pose : ") + groundItemLabelIndefinite(kind) +
                                ". Inventaire : " + inventoryText);
                    }
                }
            }

            // Independant du vol : une chute d'arbre ou un debitage en cours ne
            // doit pas se figer parce que le joueur a decolle entre-temps.
            if (forest) {
                // PHASE 28 (Agent 3) : la hache (ou la hache renforcee) PORTEE
                // donne des buches en plus au prochain debitage. Jamais une
                // condition d'abattage. 0 en mode demo (inventaire vide).
                forest->setBuckingBonusLogs(axeBonusLogs(inventory));
                forest->update(deltaTime);
            }

            // PHASE 28 : combustion des foyers, en heures de JEU (horloge gelee =>
            // feu fige, comme la meteo). Un foyer qui s'eteint laisse une cendre.
            if (ground && !hearths.hearths().empty()) {
                const int out = hearths.update(clock.totalHours(), *ground);
                if (out > 0) {
                    logInfo("Foyer : le feu s'eteint, il ne reste qu'un peu de cendre "
                            "au milieu des pierres.");
                }
            }

            if (fpsCounter.update(deltaTime)) {
                displayedFps = std::lround(fpsCounter.fps());
                titleDirty = true;

                // Une ligne de streaming par seconde, au meme rythme que le
                // compteur de FPS : de quoi lire apres coup combien de tuiles ont
                // ete reellement dessinees pour ces images-la, sans noyer la
                // sortie. C'est la mesure qui dit si le LOD travaille.
                if (terrainStreamer) {
                    logInfo(terrainStreamer->statsLine());
                }
                // Orientation de la camera, une fois par seconde. PHASE 11 :
                // ajoute parce que deux captures automatisees lancees avec les
                // MEMES arguments ne cadraient pas la meme chose. Sans cette
                // trace, impossible de dire si la camera demarre differemment ou
                // si elle derive pendant la pose.
                // PHASE 15 : la POSITION s'ajoute au cap. Le harnais automatise
                // pilote le joueur a l'estime (il compte les millisecondes de
                // touche maintenue) et n'avait donc aucun moyen de constater ou
                // le joueur avait REELLEMENT fini - or c'est exactement ce qu'il
                // faut savoir pour prouver qu'un rocher pose l'a arrete. Sans
                // cette trace, un test de collision ne peut que verifier qu'il
                // ne s'est rien passe de mal, pas que quelque chose a bloque.
                logInfo("Camera : yaw " + std::to_string(camera.yawDegrees()) +
                        " pitch " + std::to_string(camera.pitchDegrees()) +
                        " position " + std::to_string(camera.position().x) + " " +
                        std::to_string(camera.position().y) + " " +
                        std::to_string(camera.position().z));
                // PHASE 13. Une ligne de velo par seconde tant qu'on roule.
                // C'est la SEULE facon pour le harnais automatise de constater
                // que la physique a bien tourne sur le VRAI relief : la vitesse
                // et la pente sous les roues ne se devinent pas depuis
                // l'exterieur, et un velo bloque a 0 km/h donnerait par ailleurs
                // un run parfaitement vert.
                if (climber.climbing() && heightmap) {
                    // PHASE 14. Journal periodique de l'escalade. Les memes
                    // grandeurs que le velo, plus les deux qui n'existent qu'ici :
                    // l'energie (qui decide de la fin) et l'altitude gagnee (qui
                    // dit si l'escalade a servi a quelque chose). C'est cette
                    // ligne que le scenario F de run_vulkan_check.ps1 depouille.
                    logInfo("Escalade : energie " +
                            std::to_string(100.0f * climber.stamina()) +
                            " %, pente " +
                            std::to_string(100.0f * climber.slopeAt(
                                *heightmap, climber.anchor().x, climber.anchor().z)) +
                            " %, altitude " + std::to_string(climber.anchor().y) +
                            " m, gain " + std::to_string(climber.heightGained()) +
                            " m.");
                }

                if (bike.riding() && heightmap) {
                    logInfo("Velo : " + std::to_string(bike.speedKmh()) +
                            " km/h, pente " +
                            std::to_string(100.0f * bike.slopeAlongHeading(*heightmap)) +
                            " %, cap " + std::to_string(bike.headingDegrees()) +
                            " deg, assiette " + std::to_string(bike.pitchDegrees()) +
                            "/" + std::to_string(bike.rollDegrees()) + " deg, altitude " +
                            std::to_string(bike.position().y) + " m.");
                }
                // PHASE 16. Une ligne de besoins par seconde. C'est la seule
                // trace exploitable par le harnais Vulkan : le titre de la
                // fenetre ne se depouille pas depuis un script, et les jauges
                // bougent trop lentement pour qu'une capture d'image dise quoi
                // que ce soit.
                if (!editorMode) {
                    // PHASE 23. UNE ligne de meteo toutes les TRENTE SECONDES, et
                    // non une par seconde comme les jauges juste en dessous.
                    //
                    // LA CADENCE EST UN CHOIX SUBI, ET IL FAUT DIRE POURQUOI :
                    // le harnais Vulkan paie un script-block PowerShell PAR
                    // LIGNE DE JOURNAL, et son retard CROIT avec le volume (voir
                    // l'en-tete de tools/run_vulkan_check.ps1, dette ouverte en
                    // Phase 17 et comprise en Phase 22). Une premiere version de
                    // cette phase journalisait les deux lignes chaque seconde,
                    // donc TRIPLAIT le debit de journal : le scenario H a echoue
                    // sur "pas de sortie 20 s apres WM_CLOSE" et n'a collecte
                    // que 87 releves la ou il en attendait 220. Le jeu etait
                    // correct, c'est le collecteur qui n'arrivait plus a suivre.
                    //
                    // La cadence a ete resserree DEUX FOIS. Une premiere baisse
                    // a dix secondes et deux lignes a suffi a faire repasser le
                    // scenario H, mais de justesse : un run ulterieur n'a collecte
                    // que 181 releves sur 220 secondes au lieu de 235, et la
                    // recuperation de la jauge - qui se lit dans les DERNIERS
                    // releves - est passee a la trappe. A trente secondes et une
                    // seule ligne, la Phase 23 ajoute sept lignes a un scenario
                    // qui en produit plus de deux cents : le volume de journal
                    // redevient celui d'avant.
                    //
                    // Trente secondes suffisent largement au depouillement : la
                    // meteo bouge a l'echelle de l'heure de jeu, pas de la
                    // seconde reelle.
                    //
                    // Les deux couches restent lisibles SEPAREMENT dans la ligne,
                    // de part et d'autre du separateur - la synoptique dit le
                    // temps qu'il fait sur le massif, la locale dit ce que le
                    // relief en fait la ou est le joueur, et c'est justement
                    // l'ecart entre les deux qui doit se voir.
                    static int weatherLogTick = 0;
                    if (weatherLogTick % 30 == 0) {
                        logInfo(skyState.describe() + " | " + localWeather.describe());
                    }
                    ++weatherLogTick;

                    logInfo(needs.statusLine());
                }
                // PHASE 17. Une ligne d'empreinte par seconde, au meme titre
                // que celle des besoins et pour la meme raison : c'est la SEULE
                // trace depouillable par le harnais Vulkan. Rien de tout ceci
                // n'apparait a l'ecran - ni titre, ni pop-up - conformement au
                // "pas de message moralisateur". Le journal est un instrument de
                // mesure, pas une interface de jeu.
                if (!editorMode && wildlife) {
                    logInfo("Faune : " + std::to_string(wildlife->animalCount()) +
                            " animaux dont " + std::to_string(wildlife->fleeingCount()) +
                            " en fuite, " +
                            std::to_string(wildlife->countWithin(
                                camera.position(), habitat.params().noiseRadius)) +
                            " a moins de " +
                            std::to_string(habitat.params().noiseRadius) + " m (" + std::to_string(wildlife->spawnCount()) +
                            " apparitions, " + std::to_string(wildlife->despawnCount()) +
                            " disparitions). Milieu : frequentation ici " +
                            std::to_string(100.0f * habitat.disturbanceAt(
                                camera.position().x, camera.position().z)) +
                            " %, deforestation ici " +
                            std::to_string(100.0f * habitat.loggingAt(
                                camera.position().x, camera.position().z)) +
                            " % (" + std::to_string(habitat.treesFelledAt(
                                camera.position().x, camera.position().z)) +
                            " arbres), couvert au sol " +
                            std::to_string(100.0f * habitat.groundCover(
                                camera.position().x, camera.position().z)) +
                            " %, " + std::to_string(habitat.trackedCellCount()) +
                            " cellules memorisees, pointes " +
                            std::to_string(100.0f * habitat.peakDisturbance()) + "/" +
                            std::to_string(100.0f * habitat.peakLogging()) + " %.");
                }
                if (vegetation) {
                    // PHASE 26 : la COMPOSITION du peuplement, une fois par
                    // seconde. C'est la seule trace depouillable qui dise si
                    // l'essence suit reellement le versant : une capture montre
                    // une difference, elle ne la CHIFFRE pas.
                    logInfo("Essences : " + std::to_string(vegetation->lastConiferCount()) +
                            " resineux et " + std::to_string(vegetation->lastBroadleafCount()) +
                            " feuillus a l'ecran, exposition ici " +
                            std::to_string(aspectExposure.aspectExposureAt(
                                camera.position().x, camera.position().z)) + ".");
                    logInfo("Vegetation : " + std::to_string(vegetation->lastTreeCount()) +
                            " arbres et " + std::to_string(vegetation->lastGrassCount()) +
                            " touffes soumis, " +
                            std::to_string(vegetation->cachedCellCount()) +
                            " cellules d'arbres en cache." +
                            // PHASE 22 : le compte de plantes a fibres est
                            // journalise ICI et non a part, parce que c'est le
                            // SEUL chiffre qui dise si le semis produit
                            // reellement quelque chose - un `gather` qui coute
                            // du temps et ne rend rien se lit sinon comme un
                            // semis qui marche.
                            (fiberPlants
                                 ? " Plantes a fibres : " +
                                       std::to_string(fiberPlants->lastPlantCount()) +
                                       " soumises, " +
                                       std::to_string(fiberPlants->pulledPlantCount()) +
                                       " arrachee(s) en repousse."
                                 : std::string()));
                }
            }

            if (titleDirty) {
                // Le mode editeur se voit dans le TITRE. C'est le seul retour
                // permanent a l'ecran : le reste de l'etat de l'outil passe par
                // le journal, et rien dans la vue elle-meme ne distingue une
                // camera de vol libre d'edition d'un simple mode vol libre.
                // PHASE 21 : en mode demo, le titre se reduit a son minimum -
                // ni inventaire (l'artisanat est coupe), ni jauges (les besoins
                // sont coupes), ni horloge. Juste de quoi confirmer que c'est la
                // demo et lire le FPS, qui est tout le retour utile ici.
                std::string title = (editorMode ? "[EDITEUR] " : "") + kBaseTitle +
                                    " - " + std::to_string(displayedFps) + " FPS";
                if (!demoMode) {
                    if (terrainScene) {
                        title += " - " + inventoryText;
                    }
                    // PHASE 16 : les quatre jauges et l'heure. C'est l'affichage
                    // minimal demande - le jeu n'a toujours aucune interface
                    // graphique, et la barre de titre reste le seul retour
                    // permanent a l'ecran, comme pour l'inventaire depuis la
                    // Phase 6.
                    title += " - " + needs.summary() + " - " + clock.clockText();
                }
                window.setTitle(title);
                titleDirty = false;
            }

            // --- Construction de la liste d'objets a dessiner ----------------
            drawItems.clear();

            if (terrainScene) {
                if (terrainStreamer) {
                    // --- Terrain par quadtree (Phase 10) --------------------
                    // Le tronc de vision est reconstruit CHAQUE frame a partir
                    // de la projection du Renderer (qui connait l'aspect courant
                    // de la swapchain) et de la vue de la camera. Le streamer
                    // s'en sert pour ecarter les tuiles hors champ, choisir un
                    // niveau de detail par distance, et construire ce qui manque
                    // sous budget.
                    const auto tf0 = ProfileClock::now();
                    const glm::mat4 viewProjection =
                        renderer.projectionMatrix() * camera.viewMatrix();
                    const Frustum frustum = Frustum::fromViewProjection(viewProjection);
                    const auto tf1 = ProfileClock::now();
                    terrainStreamer->appendDrawItems(camera.position(), frustum, drawItems);
                    const auto tf2 = ProfileClock::now();
                    frameProfile.frustumMicros += profileMicros(tf0, tf1);
                    frameProfile.streamingMicros += profileMicros(tf1, tf2);
                } else if (sceneMeshHandle != kInvalidMesh) {
                    // Terrain d'un seul bloc : sommets deja en coordonnees monde
                    // -> matrice identite, et pas de bande de morphing.
                    drawItems.push_back({ sceneMeshHandle, glm::mat4(1.0f),
                                          Material::Terrain });
                }

                for (const Tree& tree : forest->trees()) {
                    if (tree.state != TreeState::Standing) {
                        // Des que l'arbre est coupe, sa souche apparait : c'est
                        // elle qui marque le trait de coupe pendant la chute.
                        drawItems.push_back({ stumpMeshHandle, tree.stumpMatrix(),
                                              Material::Object });
                    }
                    // L'arbre reste dessine pendant Settling : il est couche au
                    // sol, en train de s'immobiliser, et ne disparait qu'au
                    // moment ou il se debite.
                    if (tree.state != TreeState::Fallen) {
                        // PHASE 26 : la silhouette suit l'essence. C'est ce qui
                        // rend la resine LISIBLE - le joueur voit un cone ou un
                        // houppier rond avant d'appuyer, au lieu de le decouvrir
                        // en essayant (dette de la Phase 22).
                        const MeshHandle mesh =
                            (tree.species == TreeSpecies::Conifer) ? treeMeshHandle
                                                                   : broadleafMeshHandle;
                        drawItems.push_back({ mesh, tree.modelMatrix(),
                                              Material::Object });
                    }
                }

                for (const GroundItem& item : ground->items()) {
                    const MeshHandle mesh = groundMeshes[static_cast<std::size_t>(item.kind)];
                    if (mesh != kInvalidMesh) {
                        drawItems.push_back({ mesh, item.modelMatrix(), Material::Object });
                    }
                }

                // --- Objets poses par l'editeur (PHASE 15) -----------------
                // Dessines DANS LES DEUX MODES, et c'est le point : ce qui a ete
                // pose fait partie du monde, pas de l'outil. Le mode edition
                // n'ajoute que les deux teintes de temoin - selection et fantome.
                //
                // Un DrawItem par objet, sans frustum culling : le culling de la
                // Phase 10 vit dans le quadtree du terrain et celui de la
                // Phase 11 dans la vegetation instanciee, aucun des deux ne
                // s'applique ici. Assume pour cette session - une scene
                // d'editeur compte des dizaines d'objets, pas des dizaines de
                // milliers - et note comme limite dans NOTES-PHASE15.md.
                {
                    const uint32_t hiddenId =
                        editorMode ? editorController.hiddenObjectId() : kNoPlacedObject;
                    const uint32_t selectedId =
                        editorMode ? editorController.selection() : kNoPlacedObject;

                    for (const PlacedObject& object : editorScene.objects()) {
                        if (object.id == hiddenId) {
                            continue; // en main : dessine par la previsualisation
                        }
                        const auto found = assetByName.find(object.asset);
                        if (found == assetByName.end() ||
                            found->second->meshHandle == kInvalidMesh) {
                            continue; // asset absent, deja signale au chargement
                        }
                        DrawItem item{ found->second->meshHandle, object.modelMatrix(),
                                       Material::Object };
                        if (object.id == selectedId) {
                            item.params = kSelectionTint;
                        }
                        drawItems.push_back(item);
                    }

                    if (editorMode && editorController.hasPreview()) {
                        const PlacedObject& ghost = editorController.preview();
                        const auto found = assetByName.find(ghost.asset);
                        if (found != assetByName.end() &&
                            found->second->meshHandle != kInvalidMesh) {
                            drawItems.push_back({ found->second->meshHandle,
                                                  ghost.modelMatrix(),
                                                  Material::Object, kGhostTint });
                        }
                    }
                }

                // --- Faune (PHASE 17) --------------------------------------
                // Un DrawItem par animal, comme les arbres abattables et les
                // objets poses : ils se comptent en dizaines, pas en dizaines de
                // milliers, donc la voie instanciee de la Phase 11 serait du
                // travail pour rien. Aucun frustum culling non plus, meme
                // raisonnement et meme limite assumee que pour les objets poses.
                if (wildlife && animalMeshHandle != kInvalidMesh) {
                    for (const Animal& animal : wildlife->animals()) {
                        drawItems.push_back({ animalMeshHandle, animal.modelMatrix(),
                                              Material::Object });
                    }
                }

                if (paraglider.flying() && avatarMeshHandle != kInvalidMesh) {
                    // Marqueur du pilote : voir le commentaire de avatarMeshHandle
                    // plus haut. Oriente sur le cap de vol, aplati un peu pour ne
                    // pas ressembler a une caisse qui tombe du ciel.
                    glm::mat4 model =
                        glm::translate(glm::mat4(1.0f), paraglider.position());
                    model = glm::rotate(model, glm::radians(-paraglider.headingDegrees()),
                                        glm::vec3(0.0f, 1.0f, 0.0f));
                    model = glm::scale(model, glm::vec3(0.5f, 0.3f, 0.8f));
                    drawItems.push_back({ avatarMeshHandle, model, Material::Object });
                }

                if (bike.riding() && avatarMeshHandle != kInvalidMesh) {
                    // Marqueur du cycliste. Contrairement au parapente, on prend
                    // la peine d'orienter le marqueur CORRECTEMENT sur le cap :
                    // c'est le seul moyen de LIRE a l'ecran l'assiette calculee
                    // par Bike (tangage sur la pente, roulis en devers et en
                    // virage), donc de verifier a l'oeil ce que disent les tests.
                    //
                    // Convention : une rotation de theta autour de +Y envoie le
                    // +Z local sur (sin theta, cos theta) dans le plan (x, z). On
                    // veut y envoyer le cap (cos h, sin h), d'ou theta = 90 - h.
                    glm::mat4 model = glm::translate(glm::mat4(1.0f), bike.position());
                    model = glm::rotate(model, glm::radians(90.0f - bike.headingDegrees()),
                                        glm::vec3(0.0f, 1.0f, 0.0f));
                    // Le +Z local etant maintenant l'avant, le tangage tourne
                    // autour du X local, et le roulis autour du Z local.
                    model = glm::rotate(model, glm::radians(-bike.pitchDegrees()),
                                        glm::vec3(1.0f, 0.0f, 0.0f));
                    model = glm::rotate(model, glm::radians(bike.rollDegrees()),
                                        glm::vec3(0.0f, 0.0f, 1.0f));
                    // Etroit, bas et long : une silhouette de deux-roues plutot
                    // qu'un cube, ce qui rend le cap et l'assiette lisibles.
                    model = glm::scale(model, glm::vec3(0.35f, 0.5f, 1.4f));
                    drawItems.push_back({ avatarMeshHandle, model, Material::Object });
                }
            } else {
                // Objet isole des Phases 1 a 3 : rotation animee autour d'un axe
                // incline. Rotation PURE, donc mat3(model) reste une normal
                // matrix valide (voir cube.vert).
                const float elapsed = std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - startTime).count();
                const glm::mat4 spin =
                    glm::rotate(glm::mat4(1.0f), elapsed * glm::radians(45.0f),
                                glm::normalize(glm::vec3(0.3f, 1.0f, 0.0f)));
                if (sceneMeshHandle != kInvalidMesh) {
                    drawItems.push_back({ sceneMeshHandle, spin, Material::Object });
                }
            }

            // --- Vegetation de decor instanciee (Phase 11) ------------------
            instancedItems.clear();
            if (vegetation && vegetationMode != VegetationMode::None) {
                const auto tv0 = ProfileClock::now();
                const glm::mat4 viewProjection =
                    renderer.projectionMatrix() * camera.viewMatrix();
                const Frustum frustum = Frustum::fromViewProjection(viewProjection);
                const auto tv1 = ProfileClock::now();

                // PHASE 26 : le semis rend les deux essences SEPAREMENT. Deux
                // geometries distinctes = deux appels de dessin instancies, et
                // c'est irreductible : l'instanciation, par definition, tient UNE
                // geometrie pour N instances. Les melanger demanderait une
                // matrice par arbre, c'est-a-dire de defaire la Phase 11.
                //
                // Le surcout est d'UN appel de dessin par image, pas d'un par
                // arbre : le nombre d'instances, lui, est inchange.
                vegetation->gatherTrees(camera.position(), frustum,
                                        treeInstances, broadleafInstances);
                const auto tv2 = ProfileClock::now();
                frameProfile.frustumMicros += profileMicros(tv0, tv1);
                frameProfile.treesMicros += profileMicros(tv1, tv2);

                const auto pushTrees = [&](const std::vector<InstanceData>& instances,
                                           MeshHandle mesh) {
                    if (instances.empty() || mesh == kInvalidMesh) { return; }
                    InstancedDrawItem item;
                    item.mesh = mesh;
                    item.instances = instances.data();
                    item.count = static_cast<uint32_t>(instances.size());
                    item.material = Material::Foliage;
                    // Amplitude d'ondulation NULLE : un tronc ne plie pas.
                    item.params = { 0.0f, elapsedSeconds, 0.0f, 0.0f };
                    instancedItems.push_back(item);
                };
                pushTrees(treeInstances, sceneryTreeMeshHandle);
                pushTrees(broadleafInstances, sceneryBroadleafMeshHandle);

                if (useGrass && vegetationMode == VegetationMode::All &&
                    grassMeshHandle != kInvalidMesh) {
                    const auto tg0 = ProfileClock::now();
                    vegetation->gatherGrass(camera.position(), frustum, grassInstances);
                    frameProfile.grassMicros += profileMicros(tg0, ProfileClock::now());
                    if (!grassInstances.empty()) {
                        InstancedDrawItem item;
                        item.mesh = grassMeshHandle;
                        item.instances = grassInstances.data();
                        item.count = static_cast<uint32_t>(grassInstances.size());
                        item.material = Material::Foliage;
                        // w = force de la decoupe alpha (PHASE 18). C'est le
                        // seul materiau qui s'en sert : un arbre pousse 0 et
                        // traverse le meme pipeline, inchange.
                        item.params = { vegetation->params().grassSway,
                                        elapsedSeconds, 0.0f,
                                        vegetation->params().grassCutout };
                        instancedItems.push_back(item);
                    }
                }

                // PHASE 22 : les plantes a fibres. Meme voie de rendu que
                // l'herbe - une geometrie, un appel de dessin, N instances -
                // et donc rien de nouveau cote Renderer. Elles suivent la
                // bascule de vegetation (touche G) comme le reste du decor.
                if (vegetationMode == VegetationMode::All && fiberPlants &&
                    fiberMeshHandle != kInvalidMesh) {
                    // Chronometre au meme titre que l'herbe et les arbres : un
                    // poste de semis qu'on n'aurait pas mesure serait le seul du
                    // frame dans ce cas, et c'est exactement comme ca qu'on se
                    // retrouve a optimiser le mauvais (voir NOTES-PHASE19.md).
                    const auto tf0 = ProfileClock::now();
                    fiberPlants->gather(camera.position(), frustum, fiberInstances);
                    frameProfile.fiberMicros += profileMicros(tf0, ProfileClock::now());
                    if (!fiberInstances.empty()) {
                        InstancedDrawItem item;
                        item.mesh = fiberMeshHandle;
                        item.instances = fiberInstances.data();
                        item.count = static_cast<uint32_t>(fiberInstances.size());
                        item.material = Material::Foliage;
                        // Ondulation plus faible que l'herbe : une tige ligneuse
                        // de ronce plie moins qu'un brin. Decoupe alpha NULLE
                        // (w = 0) : la silhouette est portee par la geometrie,
                        // pas par un masque - ces plantes sont peu nombreuses,
                        // le budget de sommets n'est pas le sujet.
                        item.params = { 0.35f * vegetation->params().grassSway,
                                        elapsedSeconds, 0.0f, 0.0f };
                        instancedItems.push_back(item);
                    }
                }
            }

            // --- PHASE 29 : rochers et pierriers, instancies ---------------
            // Meme voie que les arbres : trois geometries, trois appels de
            // dessin, N instances. params.w = -1 active le mode ROCHER de
            // foliage.vert (inclinaison selon la pente, voir Rocks.hpp) ; ni
            // ondulation ni decoupe. Suit la bascule de vegetation (touche G).
            if (rocks && rocks->enabled() && vegetationMode != VegetationMode::None) {
                const auto tr0 = ProfileClock::now();
                const Frustum rockFrustum = Frustum::fromViewProjection(
                    renderer.projectionMatrix() * camera.viewMatrix());
                rocks->gather(camera.position(), rockFrustum, boulderInstances,
                              stoneInstances, gravelInstances);
                frameProfile.rocksMicros += profileMicros(tr0, ProfileClock::now());
                const auto pushRocks = [&](const std::vector<InstanceData>& instances,
                                           MeshHandle mesh) {
                    if (instances.empty() || mesh == kInvalidMesh) { return; }
                    InstancedDrawItem item;
                    item.mesh = mesh;
                    item.instances = instances.data();
                    item.count = static_cast<uint32_t>(instances.size());
                    item.material = Material::Foliage;
                    item.params = { 0.0f, elapsedSeconds, 0.0f, -1.0f };
                    instancedItems.push_back(item);
                };
                pushRocks(boulderInstances, boulderMeshHandle);
                pushRocks(stoneInstances, rockMeshHandle);
                pushRocks(gravelInstances, gravelMeshHandle);
            }

            // --- PHASE 29 (Agent 3) : buissons et petits arbres, instancies --
            // Chemin ARBRE de foliage.vert (params.w = 0 : ni decoupe ni mode
            // rocher), avec une ondulation tres faible (x) comme les plantes a
            // fibres : un buisson bouge un peu au vent, pas comme l'herbe.
            if (bushes && bushes->enabled() && vegetationMode != VegetationMode::None) {
                const auto tb0 = ProfileClock::now();
                const Frustum bushFrustum = Frustum::fromViewProjection(
                    renderer.projectionMatrix() * camera.viewMatrix());
                bushes->gather(camera.position(), bushFrustum, shrubInstances, smallTreeInstances);
                frameProfile.bushesMicros += profileMicros(tb0, ProfileClock::now());
                const auto pushBushes = [&](const std::vector<InstanceData>& instances,
                                            MeshHandle mesh, float sway) {
                    if (instances.empty() || mesh == kInvalidMesh) { return; }
                    InstancedDrawItem item;
                    item.mesh = mesh;
                    item.instances = instances.data();
                    item.count = static_cast<uint32_t>(instances.size());
                    item.material = Material::Foliage;
                    item.params = { sway, elapsedSeconds, 0.0f, 0.0f };
                    instancedItems.push_back(item);
                };
                pushBushes(shrubInstances, shrubMeshHandle, 0.04f);
                pushBushes(smallTreeInstances, smallTreeMeshHandle, 0.0f);
            }

            // ===============================================================
            // PHASE 23 : CYCLE JOUR/NUIT VISUEL
            // ===============================================================
            // C'est la correction de l'incoherence assumee en Phase 16 : le jeu
            // simulait une horloge acceleree douze fois, et l'image n'en montrait
            // rien - la lumiere directionnelle etait une constante ecrite en dur
            // dans les shaders depuis la Phase 2.
            //
            // La lumiere vient de la MEME position solaire que l'adret/ubac et
            // les thermiques (voir sunLightFor). Il n'y a donc pas deux soleils
            // qui pourraient diverger, ce qui est la meme exigence que celle qui
            // a fait sortir la temperature de Needs.
            //
            // Appele meme en mode EDITEUR : l'horloge y est gelee, donc la
            // lumiere l'est aussi - une scene d'edition eclairee de facon stable
            // est ce qu'on veut d'un outil.
            {
                // ===========================================================
                // PHASE 27 : METEO VISIBLE
                // ===========================================================
                // weatherVisualsFor() part de la MEME lumiere que sunLightFor()
                // et la corrige de ce que la meteo a DEJA calcule pour cette
                // image, au point du joueur : nebulosite locale, precipitation
                // locale, neige, vent local, mouillure. Elle n'invente rien -
                // par ciel clair et sol sec, elle rend sunLightFor() au bit pres
                // (verifie par testWeatherVisuals).
                //
                // --no-weather-visuals revient exactement a la Phase 26.
                const auto tw0 = ProfileClock::now();
                WeatherVisuals visuals;
                if (weatherVisuals) {
                    visuals = weatherVisualsFor(skyState, localWeather, weather.params());
                } else {
                    visuals.light = sunLightFor(skyState, weather.params());
                    visuals.fogColor = visuals.light.skyColor;
                }
                const SunLight& light = visuals.light;
                renderer.setSun(light.direction, light.intensity, light.color,
                                light.ambient);
                renderer.setClearColor(light.skyColor);
                // PHASE 27, chantier 3 : micro-relief. Motif de base = le TEXEL
                // de l'orthophoto (0,61 m sur la zone) : le grain ajoute ne
                // commence qu'en dessous de ce que la photo sait porter. Fondu
                // de 6 a 35 m : la portee du regard d'un marcheur sur le sol a
                // ses pieds ; au-dela, l'orthophoto seule a assez de pixels. Le
                // fondu PAR OCTAVE (Nyquist) du shader protege en plus contre le
                // moire, quelle que soit la distance.
                renderer.setGroundDetail(groundDetail ? kGroundDetailStrength : 0.0f,
                                         kGroundDetailFadeStart, kGroundDetailFadeEnd,
                                         groundDetailPattern);
                renderer.setWeatherVisuals(visuals.fogColor, visuals.fogDensityPerMeter,
                                           visuals.precipitation, visuals.snow,
                                           visuals.fallVelocity, visuals.groundDarkening,
                                           elapsedSeconds);
                frameProfile.weatherVisualsMicros += profileMicros(tw0, ProfileClock::now());

                // Journal BORNE PAR LE TEMPS (lecon de la Phase 24 : "sur
                // changement d'etat" oscille autour d'un seuil). Une ligne a la
                // premiere image, puis une par minute reelle au plus - de quoi
                // relire, pour chaque capture, le temps qu'il faisait.
                if (elapsedSeconds - lastWeatherVisualsLog >= 60.0f ||
                    lastWeatherVisualsLog < 0.0f) {
                    lastWeatherVisualsLog = elapsedSeconds;
                    const float visibilityKm =
                        visuals.fogDensityPerMeter > 0.0f
                            ? 3.912f / (visuals.fogDensityPerMeter * 1000.0f)
                            : -1.0f;
                    char line[320];
                    std::snprintf(line, sizeof(line),
                                  "Visuels meteo%s : nuages locaux %.2f, precipitation "
                                  "%.2f mm/h (%s, ecran %.2f), mouillure %.2f, soleil "
                                  "direct %.2f, ambiante x%.2f, visibilite %s",
                                  weatherVisuals ? "" : " (DESACTIVES, --no-weather-visuals)",
                                  localWeather.cloudCover,
                                  localWeather.precipitationMmPerHour,
                                  localWeather.snowing ? "neige" : "pluie",
                                  visuals.precipitation, localWeather.wetness,
                                  light.intensity, light.ambient,
                                  visibilityKm > 0.0f
                                      ? (std::to_string(visibilityKm).substr(0, 5) + " km").c_str()
                                      : "illimitee");
                    logInfo(line);
                }

                // ===========================================================
                // PHASE 25 : force de l'indice visuel ADRET / UBAC
                // ===========================================================
                // L'ecart adret/ubac etait mesure depuis la Phase 23 (14,7 C
                // contre 10,7 C a midi) et parfaitement INVISIBLE a l'ecran.
                //
                // Ce scalaire porte les deux facteurs de l'ecart qui ne
                // dependent PAS de la geometrie locale, et il les calcule avec
                // les MEMES parametres que WeatherSystem::sampleAt :
                //
                //   - la hauteur du soleil. Le soleil doit etre leve, et
                //     l'effet s'eteint progressivement quand il rase
                //     l'horizon. C'est la condition qui, en Phase 23, avait
                //     manque et rendait un versant sud plus chaud a 2 h du
                //     matin ; elle ne sera pas oubliee ici ;
                //   - la nebulosite. Sous un ciel couvert il n'y a pas de
                //     rayonnement direct, donc pas d'adret - ni thermique, ni
                //     visuel.
                //
                // Le cosinus d'incidence par fragment, lui, reste au shader :
                // c'est la seule part qui a besoin de la normale du relief.
                const WeatherParams& wp = weather.params();
                const float aspectSunStrength =
                    std::clamp(skyState.daylight /
                                   std::max(1e-3f, wp.aspectFullDaylightSin),
                               0.0f, 1.0f);
                const float aspectCloudDamping =
                    std::max(0.0f, 1.0f - wp.cloudAspectDamping * skyState.cloudCover);
                renderer.setAspectTint(kAspectTintIntensity * aspectSunStrength *
                                       aspectCloudDamping);
            }

            const auto td0 = ProfileClock::now();
            renderer.drawFrame(camera, drawItems, instancedItems);
            frameProfile.drawMicros += profileMicros(td0, ProfileClock::now());
            ++frameProfile.frames;
        }

        // S'assurer que le GPU a fini avant de detruire quoi que ce soit.
        renderer.waitIdle();

        if (frameProfile.frames > 0) {
            const auto per = [&](uint64_t total) {
                const double value = static_cast<double>(total) /
                                     static_cast<double>(frameProfile.frames);
                return std::to_string(static_cast<int>(value * 10.0 + 0.5) / 10.0);
            };
            logInfo("Cout par image (PHASE 19) : " +
                    std::to_string(frameProfile.frames) + " images - streaming terrain " +
                    per(frameProfile.streamingMicros) + " us, arbres " +
                    per(frameProfile.treesMicros) + " us, herbe " +
                    per(frameProfile.grassMicros) + " us, plantes a fibres " +
                    per(frameProfile.fiberMicros) + " us, tronc de vision " +
                    per(frameProfile.frustumMicros) + " us, drawFrame " +
                    per(frameProfile.drawMicros) + " us, visuels meteo " +
                    per(frameProfile.weatherVisualsMicros) + " us.");
            const double loopSeconds =
                std::chrono::duration<double>(ProfileClock::now() - loopStart).count();
            if (loopSeconds > 0.0) {
                char line[200];
                std::snprintf(line, sizeof(line),
                              "Cadence de boucle (PHASE 27) : %llu images en %.2f s, soit "
                              "%.1f images/s, %.1f us par image.",
                              static_cast<unsigned long long>(frameProfile.frames),
                              loopSeconds,
                              static_cast<double>(frameProfile.frames) / loopSeconds,
                              1e6 * loopSeconds / static_cast<double>(frameProfile.frames));
                logInfo(line);
            }
            if (rocks && rocks->enabled()) {
                logInfo("Rochers (PHASE 29) : semis " + per(frameProfile.rocksMicros) +
                        " us par image ; derniere image " +
                        std::to_string(rocks->lastBoulderCount()) + " gros blocs, " +
                        std::to_string(rocks->lastStoneCount()) + " rochers, " +
                        std::to_string(rocks->lastGravelCount()) +
                        " plaques de pierraille.");
            }
            if (bushes && bushes->enabled()) {
                logInfo("Buissons (PHASE 29) : semis " + per(frameProfile.bushesMicros) +
                        " us par image ; derniere image " +
                        std::to_string(bushes->lastShrubCount()) + " buissons, " +
                        std::to_string(bushes->lastSmallTreeCount()) + " petits arbres.");
            }
        }
        logInfo("Fermeture propre.");
    } catch (const std::exception& e) {
        logError(std::string("Exception fatale : ") + e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
