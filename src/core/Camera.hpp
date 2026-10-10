#pragma once

#include "core/SlopeLimits.hpp"

#include <glm/glm.hpp>

namespace vercora {

class Window;
class HeightSampler;
class CollisionSolver;

// Quatre modes :
//
//  - Free (Phase 1) : "fly camera". ZQSD dans la direction du regard,
//    espace/shift pour monter/descendre. C'est le mode d'inspection d'un objet,
//    bascule avec Walk par la touche F.
//  - Walk (Phase 4) : la camera colle au terrain. ZQSD deplace uniquement dans
//    le plan horizontal, et Y est impose par le terrain sous les pieds, plus
//    une hauteur d'oeil fixe. Espace/shift sont sans effet (pas encore de saut).
//  - Glide (Phase 8) : vol en parapente. La camera ne se pilote plus toute
//    seule ici - voir world/Paraglider, qui possede la position et le cap du
//    pilote - elle se contente d'afficher une vue chase-cam derriere lui via
//    setChasePose(), appelee une fois par frame par l'appelant. update() est
//    alors un no-op : ni clavier ni souris n'agissent sur la camera pendant le
//    vol, cf. la remarque sur la lisibilite dans setChasePose().
//  - Climb (Phase 14) : escalade. Le seul mode a la PREMIERE personne pilote de
//    l'exterieur, et donc un contrat different des deux precedents : la POSITION
//    vient de world/Climber (via setPosition(), appelee une fois par frame),
//    mais l'ORIENTATION reste au joueur - la souris continue de fonctionner.
//    Voir le commentaire de update() pour la raison, qui est de gameplay et non
//    de commodite.
//  - Ride (Phase 13) : sortie a velo. EXACTEMENT le meme contrat que Glide -
//    la camera est passive et recoit sa pose de l'exterieur (world/Bike) - mais
//    c'est un mode distinct et non un alias, pour deux raisons : les journaux
//    doivent dire lequel des deux est actif, et une activite future pourra
//    vouloir un cadrage different sans toucher a l'autre. Le choix de la
//    troisieme personne est le meme que pour le parapente : la lisibilite du
//    mouvement, ici l'assiette du velo sur le relief.
//
// Le mode Walk n'est disponible que si un HeightSampler a ete fourni ; sinon la
// bascule est refusee avec un message, plutot que de laisser la camera dans un
// etat ou elle ne peut plus bouger verticalement.
//
// Note clavier : on interroge les tokens GLFW W/A/S/D. Sur un clavier AZERTY
// francais, ces tokens correspondent PHYSIQUEMENT aux touches Z/Q/S/D (GLFW
// raisonne en positions physiques facon QWERTY US). Donc ZQSD "sous les doigts".
enum class CameraMode { Free, Walk, Glide, Ride, Climb };

// Hauteur des yeux au-dessus du sol en mode marche, en unites monde (= metres).
//
// Nommee ici plutot que codee en dur dans Camera : le code qui doit PREDIRE ce
// que verra le joueur - le mode --dump-world de main.cpp, qui cherche depuis
// quel recul un arbre est reellement visible - en a besoin sans avoir de Camera
// sous la main, l'oeil n'existant pas encore au moment ou il calcule.
inline constexpr float kDefaultEyeHeight = 1.7f;

class Camera {
public:
    explicit Camera(glm::vec3 position = glm::vec3(0.0f, 0.0f, 3.0f),
                    float yawDegrees = -90.0f,
                    float pitchDegrees = 0.0f);

    // A appeler une fois par frame. deltaTime en secondes.
    void update(const Window& window, float deltaTime);

    glm::mat4 viewMatrix() const;
    glm::vec3 position() const { return position_; }

    // Direction du regard, normalisee. C'est l'origine et la direction du rayon
    // de visee du joueur (Phase 5 : designer l'arbre a abattre).
    glm::vec3 front() const { return front_; }

    // Terrain sur lequel marcher. Le pointeur n'est PAS possede : il doit rester
    // valide aussi longtemps que la camera. nullptr desactive le mode Walk.
    void setHeightSampler(const HeightSampler* sampler) { heightSampler_ = sampler; }

    // Obstacles que la camera ne doit pas traverser. Le pointeur n'est PAS
    // possede : il doit rester valide aussi longtemps que la camera. nullptr
    // (le defaut) = on traverse tout, exactement comme avant la Phase 6.
    //
    // La collision n'agit QU'EN MODE MARCHE. Le vol libre reste un mode
    // d'inspection : pouvoir traverser un tronc pour aller regarder une souche
    // de pres y est une fonctionnalite, pas un defaut.
    void setCollisionSolver(const CollisionSolver* solver) { collisionSolver_ = solver; }

    // Bascule refusee (et signalee) vers Walk sans HeightSampler.
    void setMode(CameraMode mode);
    CameraMode mode() const { return mode_; }

    // Deplace la camera de force a une position donnee, sans toucher a son
    // orientation. Sert au retour d'atterrissage du parapente : le pilote se
    // pose loin d'ou la camera se trouvait (elle suivait en chase-cam), il faut
    // donc la ramener sur lui avant de rebasculer en mode Walk.
    void setPosition(const glm::vec3& position) { position_ = position; }

    // Pose imposee depuis l'exterieur en mode Glide : eye = ou placer l'oeil de
    // la camera, lookAtPoint = ce qu'elle regarde (le pilote). Recalcule aussi
    // yaw_/pitch_ a partir du regard obtenu, sans quoi sortir du vol ferait
    // sauter la camera a l'orientation d'avant le decollage - souvent tres
    // differente - au lieu de continuer dans a peu pres la direction du vol.
    void setChasePose(const glm::vec3& eye, const glm::vec3& lookAtPoint);

    // Demi-largeur du joueur, en unites monde. Sert au test de collision.
    void setBodyRadius(float radius) { bodyRadius_ = radius; }
    float bodyRadius() const { return bodyRadius_; }

    // Vitesse de deplacement en unites/seconde. Un terrain de 100 unites de cote
    // ne se parcourt pas a la meme vitesse qu'un cube d'une unite d'arete.
    void setMoveSpeed(float unitsPerSecond) { moveSpeed_ = unitsPerSecond; }

    // PHASE 15 : le mode editeur vole beaucoup plus vite que la marche et doit
    // rendre EXACTEMENT sa vitesse a la camera en sortant, sans supposer laquelle
    // c'etait (elle differe entre une scene de terrain et un objet isole).
    float moveSpeed() const { return moveSpeed_; }

    // PHASE 11. Coupe TOUTE lecture du clavier et de la souris par la camera.
    //
    // Raison d'etre : le jeu lit le clavier et la souris SYSTEME (GLFW ne voit
    // pas les messages postes, cf. l'en-tete de run_vulkan_check.ps1).
    // Une capture automatisee lancee pendant que la machine sert a autre chose
    // se fait donc bouger la camera par la vraie souris - constate directement :
    // deux executions du meme script ont donne yaw -90 et yaw -50, donc deux
    // cadrages differents, ce qui rend toute comparaison avant/apres sans
    // valeur. Verrouiller l'entree rend la vue entierement determinee par
    // --start et --look.
    void setInputEnabled(bool enabled) { inputEnabled_ = enabled; }
    bool inputEnabled() const { return inputEnabled_; }

    // PHASE 15. Interdit a la touche F de basculer Free <-> Walk.
    //
    // Le mode EDITEUR exige une camera de vol libre : ni gravite, ni collision,
    // pour survoler les 25 km2. C'est exactement ce que fait deja
    // CameraMode::Free - resolveCollisions() et snapToTerrain() ne sont appeles
    // qu'en mode Walk - il ne manquait donc rien, sauf la garantie que le mode
    // ne change pas sous les pieds de l'editeur des qu'une touche traine.
    //
    // Un verrou plutot qu'une valeur d'enum "Editor" de plus : la camera d'edition
    // n'a AUCUN comportement propre, elle est un vol libre. Lui inventer un mode
    // ajouterait une branche a traiter dans chaque switch de ce fichier et dans
    // main.cpp, pour un comportement identique a un cas existant.
    void setModeLocked(bool locked) { modeLocked_ = locked; }
    bool modeLocked() const { return modeLocked_; }

    // PHASE 24. Reglage de la limite de pente franchissable a pied, et dernier
    // resultat du filtre. Voir core/SlopeLimits.hpp.
    //
    // Le resultat est expose parce qu'il est la SEULE facon de constater apres
    // coup pourquoi le joueur n'avancait plus : sans lui, "je pousse la touche
    // et il ne se passe rien" est indiscernable d'une touche perdue par le
    // harnais - un diagnostic que ce projet a deja paye plusieurs fois.
    void setWalkSlopeParams(const WalkSlopeParams& params) { walkSlopeParams_ = params; }
    const WalkSlopeParams& walkSlopeParams() const { return walkSlopeParams_; }
    const WalkStepResult& lastWalkStep() const { return lastWalkStep_; }

    // Oriente la camera directement, en degres.
    void setOrientation(float yawDegrees, float pitchDegrees);

    // Orientation courante, en degres. Exposee pour le journal de diagnostic :
    // c'est la seule facon de constater apres coup qu'une capture automatisee
    // ne cadrait pas ce qu'on croyait.
    float yawDegrees() const { return yaw_; }
    float pitchDegrees() const { return pitch_; }

private:
    void updateVectors();

    // Applique la contrainte du mode Walk : Y = terrain + hauteur d'oeil.
    void snapToTerrain();

    // Repousse la camera hors des obstacles, a l'horizontale uniquement.
    // previousPosition = position d'avant le pas, utilisee comme repli quand
    // aucune position libre n'existe.
    void resolveCollisions(const glm::vec3& previousPosition);

    glm::vec3 position_;
    glm::vec3 front_;
    glm::vec3 up_;
    glm::vec3 right_;
    glm::vec3 worldUp_ = glm::vec3(0.0f, 1.0f, 0.0f);

    bool inputEnabled_ = true;
    bool modeLocked_ = false;

    float yaw_;   // degres
    float pitch_; // degres

    float moveSpeed_ = 3.0f;         // unites/seconde
    float mouseSensitivity_ = 0.1f;  // degres/pixel

    // Hauteur des yeux au-dessus du sol en mode Walk. La convention 1 unite =
    // 1 metre n'est plus une hypothese depuis la Phase 7 : le terrain vient de
    // donnees LiDAR dont les coordonnees sont en metres.
    float eyeHeight_ = kDefaultEyeHeight;

    // Demi-largeur du corps du joueur. 0.35 unite = 0.35 m : un peu plus qu'une
    // demi-largeur d'epaules, ce qui evite de raser les troncs a les frotter.
    float bodyRadius_ = 0.35f;

    WalkSlopeParams walkSlopeParams_{};
    WalkStepResult  lastWalkStep_{};

    CameraMode mode_ = CameraMode::Free;
    const HeightSampler* heightSampler_ = nullptr;     // non possede
    const CollisionSolver* collisionSolver_ = nullptr; // non possede

    // Front descendant de la touche de bascule : sans ca, maintenir F une demi
    // seconde ferait basculer le mode a chaque frame.
    bool toggleKeyWasDown_ = false;

    // Etat du drag souris (clic droit).
    bool   rotating_ = false;
    double lastX_ = 0.0;
    double lastY_ = 0.0;
};

} // namespace vercora
