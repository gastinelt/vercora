#pragma once

#include "editor/PlacedObject.hpp"

#include <cstdint>
#include <string>

namespace vercora {

class AssetCatalog;
class EditorScene;
class HeightSampler;

// ---------------------------------------------------------------------------
// PHASE 15 : la machine a etats de l'editeur.
// ---------------------------------------------------------------------------
// Elle tient tout ce qui distingue "une liste d'objets" (EditorScene) d'un
// OUTIL : quel asset est arme, ou tombe la visee, quel objet est selectionne,
// est-on en train d'en deplacer un.
//
// --- POURQUOI ELLE NE LIT PAS LE CLAVIER ------------------------------------
// Elle recoit un `EditorInput` deja depouille par l'appelant. Trois raisons, la
// derniere etant la vraie :
//   1. elle ne depend alors ni de GLFW ni de Window, donc elle est EXECUTEE dans
//      les tests headless - toute la logique de placement, de selection et de
//      saisie est couverte sans GPU ni fenetre ;
//   2. les touches restent decrites en un seul endroit, dans main.cpp, a cote de
//      celles du jeu ;
//   3. le harnais automatise (tools/run_vulkan_check.ps1) ne peut PAS piloter la
//      souris - GLFW lit le curseur systeme, pas les messages postes. Un
//      controleur qui lirait lui-meme les entrees serait donc intestable
//      autrement qu'a la main. En le nourrissant d'une structure, un scenario
//      scripte peut lui soumettre exactement la meme sequence qu'un joueur.
//
// --- LE PINCEAU, LA SELECTION ET LA SAISIE ----------------------------------
// Trois etats coexistent, et l'ordre dans lequel les commandes de reglage
// (rotation, echelle, hauteur) les visent est fixe et documente :
//
//   SAISIE  > SELECTION > PINCEAU
//
//   * SAISIE : un objet deja pose a ete "decroche" (touche de saisie). Il suit
//     la visee jusqu'a ce qu'on le repose. Tant qu'il est en main, la scene
//     n'est PAS modifiee - c'est seulement en le reposant qu'une unique action
//     annulable est empilee. Sans ca, deplacer un rocher de trente metres
//     empilerait une annulation par image.
//   * SELECTION : un objet pose, designe par la visee. Les reglages
//     l'atteignent directement, et les retouches successives sont regroupees en
//     une seule annulation (voir EditorScene).
//   * PINCEAU : l'asset arme et SES reglages (cap, echelle, hauteur, solidite).
//     Ils sont CONSERVES d'une pose a l'autre, ce qui est le comportement utile
//     quand on aligne une serie de blocs : on regle une fois, on pose dix fois.
//
// --- LE FANTOME -------------------------------------------------------------
// L'objet previsualise avant la pose. Sa geometrie est celle de l'asset arme, sa
// position est le point ou le regard rencontre le terrain (core/TerrainRay.hpp),
// et il n'existe que quand la visee touche effectivement le sol dans l'emprise -
// viser le ciel ne pose rien et ne montre rien, plutot que de poser a une
// distance arbitraire.

// Ce que l'appelant doit fournir a chaque image.
//
// Les champs `...Pressed` sont des FRONTS DESCENDANTS deja resolus (la touche
// vient d'etre enfoncee) ; les champs `hold...` sont l'etat courant de la
// touche, parce que ces reglages-la sont continus - on maintient la fleche pour
// tourner, on ne la tapote pas quinze fois.
struct EditorInput {
    float deltaTime = 0.0f;

    // Temps courant en secondes, croissant. Sert au regroupement des
    // annulations (voir EditorScene::transform).
    float now = 0.0f;

    // Rayon de visee : oeil et direction NORMALISEE du regard. C'est le meme
    // rayon que celui de l'abattage (main.cpp appelle Forest::pickTree avec
    // camera.position() et camera.front()), donc le reticule de l'editeur est
    // exactement celui du jeu.
    glm::vec3 eye{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};

    // --- Fronts -------------------------------------------------------------
    bool placePressed = false;      // poser l'objet arme / reposer l'objet saisi
    bool pickPressed = false;       // selectionner l'objet vise
    bool deletePressed = false;     // supprimer la selection
    bool grabPressed = false;       // saisir / reposer la selection
    bool cancelPressed = false;     // annuler la saisie en cours, deselectionner
    bool nextAssetPressed = false;  // asset suivant du catalogue
    bool prevAssetPressed = false;  // asset precedent
    bool toggleSolidPressed = false; // solide <-> traversable
    bool undoPressed = false;
    bool savePressed = false;
    bool reloadPressed = false;
    bool listPressed = false;       // reafficher catalogue + inventaire

    // Selectionne l'objet POSE suivant / precedent (ordre de la scene), sans
    // passer par la visee. Ajoute apres la Phase 15 : sans gizmo ni reticule
    // permanent, retrouver un objet pose loin du regard courant n'avait qu'une
    // voie, revenir dessus a la souris. Ignore si un objet est en main, comme
    // pickPressed (voir EditorController::update).
    bool cycleNextPressed = false;
    bool cyclePrevPressed = false;

    // Rang 1..9 demande au clavier, -1 si aucune. Converti en indice de
    // catalogue par l'appelant ? NON : c'est fait ici, parce que le rang depend
    // de la PAGE courante, qui est un etat du controleur.
    int quickPick = -1;

    // --- Etats maintenus ----------------------------------------------------
    bool holdRotateLeft = false;
    bool holdRotateRight = false;
    bool holdScaleUp = false;
    bool holdScaleDown = false;
    bool holdRaise = false;
    bool holdLower = false;

    // Pas fin : divise les vitesses de reglage ci-dessous. Sur la touche de
    // modification, pour poser au centimetre quand ca compte.
    bool holdFineStep = false;
};

// Reglages de l'outil. Regroupes pour qu'un scenario de test puisse les fixer
// sans toucher au code.
struct EditorControllerParams {
    // Portee de la visee, en metres. 400 m : de quoi poser un rocher sur la
    // croupe d'en face depuis un promontoire, sans que le pas de marche du
    // raycast (voir core/TerrainRay.hpp) devienne grossier au point de rater
    // ce qu'on regarde.
    float aimReach = 400.0f;

    // Vitesses de reglage, par seconde de touche maintenue.
    float rotateDegreesPerSecond = 90.0f;
    float scalePerSecond = 1.8f;   // facteur multiplicatif par seconde
    float liftMetresPerSecond = 0.6f;

    // Diviseur applique aux trois vitesses avec la touche de pas fin.
    float fineStepDivider = 8.0f;

    // Bornes d'echelle. Un objet a 0,01 est invisible et un objet a 100 masque
    // tout l'ecran : dans les deux cas on ne peut plus le retrouver pour
    // corriger, ce qui est la seule vraie raison de borner.
    float minScale = 0.05f;
    float maxScale = 40.0f;

    // Bornes du decalage vertical par rapport au terrain, en metres.
    float minLift = -20.0f;
    float maxLift = 60.0f;
};

class EditorController {
public:
    // Ni le catalogue ni la scene ne sont possedes : tous deux doivent vivre
    // plus longtemps que le controleur.
    EditorController(const AssetCatalog& catalog, EditorScene& scene,
                     const EditorControllerParams& params = {});

    // Une image d'editeur. `halfExtent` est la demi-emprise horizontale du
    // terrain : au-dela, la visee ne touche rien (voir raycastTerrain).
    void update(const EditorInput& input, const HeightSampler& terrain,
                float halfExtent);

    // --- Ce que l'appelant doit dessiner ------------------------------------
    // Y a-t-il un objet a previsualiser (fantome de pose, ou objet saisi) ?
    bool hasPreview() const { return hasPreview_; }

    // La previsualisation, prete a etre transformee en DrawItem. Son `id` vaut
    // celui de l'objet saisi, ou kNoPlacedObject pour un fantome de pose.
    const PlacedObject& preview() const { return preview_; }

    // Objet dont le dessin NORMAL doit etre saute cette image, parce qu'il est
    // en cours de deplacement et deja dessine par la previsualisation.
    // kNoPlacedObject si aucun.
    uint32_t hiddenObjectId() const { return grabbedId_; }

    uint32_t selection() const { return selectedId_; }
    bool grabbing() const { return grabbedId_ != kNoPlacedObject; }

    // Indice du pinceau dans le catalogue, ou -1 si le catalogue est vide.
    int brushIndex() const { return brushIndex_; }

    // --- Requetes a traiter par l'appelant ----------------------------------
    // Le controleur ne fait AUCUNE entree/sortie : sauvegarder demande un chemin
    // et le georeferencement du terrain, qui ne le regardent pas. Il se contente
    // de lever un drapeau, que l'appelant consomme.
    bool consumeSaveRequest();
    bool consumeReloadRequest();
    bool consumeListRequest();

    // --- Journal ------------------------------------------------------------
    // Ligne d'etat compacte : pinceau, reglages, selection, nombre d'objets.
    std::string statusLine() const;

    // A-t-elle change depuis le dernier appel a consumeStatusChanged() ?
    // Sert a n'ecrire dans le journal que quand quelque chose bouge : l'editeur
    // tourne a plusieurs centaines d'images par seconde, une ligne par image
    // rendrait toute autre trace illisible.
    bool consumeStatusChanged();

    // Force l'affichage du prochain etat (apres un rechargement de fichier).
    void markStatusChanged() { statusChanged_ = true; }

    // Remet la selection et la saisie a zero. Appele en sortant du mode editeur :
    // laisser un objet "en main" pendant une partie normale le figerait sur le
    // reticule pour toujours.
    void resetInteraction();

    // Rang du pinceau dans la page courante et taille de page, pour l'aide.
    static constexpr int kPageSize = 9;
    int page() const { return page_; }

private:
    void selectBrush(int index);
    void applyAdjustments(const EditorInput& input, PlacedObject& target,
                          float* liftOut) const;

    const AssetCatalog& catalog_;
    EditorScene& scene_;
    EditorControllerParams params_;

    // --- Pinceau ------------------------------------------------------------
    int brushIndex_ = -1;
    int page_ = 0;
    float brushYaw_ = 0.0f;
    float brushScale_ = 1.0f;
    float brushLift_ = 0.0f;    // decalage vertical par rapport au terrain
    bool brushSolid_ = true;

    // --- Selection et saisie ------------------------------------------------
    uint32_t selectedId_ = kNoPlacedObject;
    uint32_t grabbedId_ = kNoPlacedObject;
    // Etat de TRAVAIL pendant la saisie : c'est lui que les reglages modifient
    // et lui qui est previsualise. L'etat d'AVANT la saisie n'est volontairement
    // pas recopie ici - il reste dans la scene, intacte tant que l'objet est en
    // main, et c'est elle qui le retiendra comme `before` de l'unique action
    // annulable empilee au moment de reposer.
    PlacedObject grabState_;
    // Decalage vertical de l'objet saisi, reevalue par rapport au terrain sous
    // sa nouvelle position a chaque image.
    float grabLift_ = 0.0f;

    // --- Sortie -------------------------------------------------------------
    bool hasPreview_ = false;
    PlacedObject preview_;

    bool saveRequested_ = false;
    bool reloadRequested_ = false;
    bool listRequested_ = false;
    bool statusChanged_ = true;
    std::string lastStatus_;
};

} // namespace vercora
