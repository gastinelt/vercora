#pragma once

#include "core/CollisionSolver.hpp"
#include "editor/PlacedObject.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vercora {

class AssetCatalog;

// (kNoPlacedObject est declare dans editor/PlacedObject.hpp, avec le champ id
// qu'il qualifie.)

// ---------------------------------------------------------------------------
// PHASE 15 : la scene editable.
// ---------------------------------------------------------------------------
// Trois responsabilites, et pas une de plus :
//   1. detenir la liste des objets poses et leurs identites stables ;
//   2. repondre aux questions du monde a leur sujet - visee (pick) et COLLISION
//      (elle implemente CollisionSolver, comme la Forest depuis la Phase 6) ;
//   3. tenir la pile d'ANNULATION.
//
// Ce qu'elle ne fait PAS : lire le clavier (c'est EditorController), toucher a
// Vulkan (c'est main.cpp qui traduit en DrawItem), ou lire/ecrire un fichier
// (c'est SceneFile). Cette classe ne depend que de GLM, donc elle est EXECUTEE
// dans les tests headless - annulation, collision et visee comprises.
//
// --- COLLISION : le meme contrat que la Forest ------------------------------
// Chaque objet SOLIDE est un cylindre vertical, de rayon `collisionRadius x
// echelle` (rayon du cylindre CONTENANT l'asset, voir AssetCatalog) et de
// hauteur `height x echelle` au-dessus de son point d'ancrage.
//
// Ce cylindre est un PROXY assume. Il est exact pour un rocher ou un cairn -
// c'est-a-dire pour ce que l'editeur sert d'abord a poser - et il est FAUX pour
// tout objet a interieur : l'abri de pierre a une porte, mais son cylindre est
// plein, donc on ne peut pas y entrer. D'ou le drapeau `solid` par objet
// (PlacedObject::solid) : le remede immediat est de le poser traversable. La
// vraie reponse - une collision par volume ou par maillage - est un chantier a
// part, documente comme limite dans NOTES-PHASE15.md.
//
// --- ANNULATION -------------------------------------------------------------
// Pile d'actions INVERSIBLES, LIFO, profondeur bornee. Trois natures d'action :
// pose, suppression, transformation. Chacune retient de quoi revenir en arriere
// (l'etat AVANT) et l'indice ou l'objet se trouvait, ce qui permet a une
// suppression annulee de reprendre sa place dans la liste plutot que d'atterrir
// a la fin - l'ordre de la liste est ce que voit l'utilisateur.
//
// Les retouches de transformation SUCCESSIVES sur le meme objet sont
// REGROUPEES tant qu'elles s'enchainent a moins de `transformGroupSeconds` l'une
// de l'autre. Sans ce regroupement, tourner un rocher d'un quart de tour au pas
// de 15 degres empilerait six annulations, et "annuler" ne rendrait jamais
// l'etat d'avant la manipulation - ce qui est pourtant la seule chose qu'on
// veut. Le temps est FOURNI par l'appelant plutot que lu d'une horloge : c'est
// ce qui rend le regroupement testable.
class EditorScene : public CollisionSolver {
public:
    // Le catalogue n'est PAS possede et peut etre nul : sans lui la scene reste
    // parfaitement utilisable, seule la collision devient inerte (on ne connait
    // plus les dimensions des assets). C'est le cas des tests qui n'exercent que
    // la pile d'annulation.
    explicit EditorScene(const AssetCatalog* catalog = nullptr) : catalog_(catalog) {}

    void setCatalog(const AssetCatalog* catalog) { catalog_ = catalog; }
    const AssetCatalog* catalog() const { return catalog_; }

    // --- Contenu ------------------------------------------------------------
    const std::vector<PlacedObject>& objects() const { return objects_; }
    std::size_t size() const { return objects_.size(); }
    bool empty() const { return objects_.empty(); }

    const PlacedObject* find(uint32_t id) const;

    // Indice dans objects(), ou -1.
    int indexOf(uint32_t id) const;

    // --- Edition ------------------------------------------------------------
    // Pose un objet et renvoie son identifiant. L'identifiant du parametre est
    // ignore : c'est la scene qui les attribue.
    uint32_t place(PlacedObject object);

    // Supprime un objet. Renvoie false si l'identifiant n'existe pas.
    bool erase(uint32_t id);

    // Remplace la transformation d'un objet (position / cap / echelle / solidite ;
    // l'asset, lui, n'est jamais modifie - changer d'asset, c'est poser un autre
    // objet). `now` est un temps en secondes, croissant, servant au regroupement
    // des retouches successives.
    bool transform(uint32_t id, const PlacedObject& updated, float now);

    // --- Annulation ---------------------------------------------------------
    // Annule la derniere action. Renvoie false si la pile est vide.
    bool undo();

    std::size_t undoDepth() const { return undoStack_.size(); }

    // Libelle de la derniere action annulable, pour le journal ("pose d'un
    // rocher", ...). Vide si la pile est vide.
    std::string lastActionLabel() const;

    // --- Chargement en bloc -------------------------------------------------
    // Remplace tout le contenu et VIDE la pile d'annulation. Annuler par-dessus
    // un chargement de fichier n'aurait aucun sens defini : la pile decrit des
    // actions sur un contenu qui vient de disparaitre.
    void replaceAll(std::vector<PlacedObject> objects);

    void clear() { replaceAll({}); }

    // --- Interrogation ------------------------------------------------------
    // Premier objet touche par le rayon (cylindre englobant), ou kNoPlacedObject.
    // Meme construction que Forest::pickTree, y compris le test des DEUX racines
    // du cylindre pour que viser depuis l'interieur fonctionne.
    uint32_t pick(const glm::vec3& origin, const glm::vec3& direction,
                  float maxDistance) const;

    // --- CollisionSolver ----------------------------------------------------
    glm::vec3 resolve(const BodyCylinder& body) const override;
    bool overlaps(const BodyCylinder& body) const override;

    // Nombre d'objets solides. Journalise au chargement : c'est le seul moyen de
    // constater qu'une scene est entierement traversable par megarde.
    std::size_t solidCount() const;

    // Fenetre de regroupement des retouches de transformation, en secondes.
    void setTransformGroupSeconds(float seconds) { transformGroupSeconds_ = seconds; }
    float transformGroupSeconds() const { return transformGroupSeconds_; }

    // Profondeur maximale de la pile d'annulation. Bornee parce qu'un editeur
    // ouvert une heure accumulerait sinon des milliers d'etats ; 128 couvre
    // largement une session de retouche.
    static constexpr std::size_t kMaxUndoDepth = 128;

private:
    enum class ActionKind { Place, Erase, Transform };

    struct Action {
        ActionKind kind = ActionKind::Place;
        // Etat AVANT l'action, pour Erase et Transform. Pour Place, seul `id`
        // compte (l'objet a annuler est celui qui vient d'etre cree).
        PlacedObject before;
        // Indice qu'occupait l'objet, pour qu'une suppression annulee reprenne
        // sa place au lieu de sauter en fin de liste.
        std::size_t index = 0;
        // Horodatage de la derniere retouche, pour le regroupement.
        float lastTouch = 0.0f;
    };

    void pushAction(Action action);

    // Rayon et hauteur EFFECTIFS d'un objet (dimensions de l'asset x echelle).
    // Renvoie false si le catalogue est absent ou l'asset introuvable - un objet
    // dont on ne connait pas les dimensions ne peut pas bloquer le joueur, et le
    // faire bloquer "au cas ou" avec des dimensions inventees serait pire.
    bool footprint(const PlacedObject& object, float* radius, float* height) const;

    const AssetCatalog* catalog_ = nullptr; // non possede
    std::vector<PlacedObject> objects_;
    std::vector<Action> undoStack_;
    uint32_t nextId_ = 1;
    float transformGroupSeconds_ = 0.75f;
};

} // namespace vercora
