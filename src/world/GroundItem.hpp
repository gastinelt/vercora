#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace vercora {

// Nature d'un objet pose au sol. Un enum ferme, et non un booleen "isBranch"
// comme en Phase 5 : des que la pierre et l'eclat sont apparus, le booleen ne
// pouvait plus rien exprimer. Chaque valeur a son mesh (voir TreeMesh/StoneMesh)
// et son rayon de repos (voir groundItemRestRadius).
enum class GroundItemKind : uint8_t {
    Log,    // buche debitee dans un tronc abattu
    Branch, // branche cassee du houppier
    Stone,  // galet ramassable, matiere premiere du taillage
    Flake,  // eclat tranchant obtenu par percussion de deux pierres
    // --- PHASE 22 : les trois matieres du document de cadrage qui manquaient --
    // Elles arrivent toutes les trois PAR LE SOL, comme l'eclat de percussion :
    // une interaction dans le monde depose l'objet aux pieds du joueur, qui doit
    // ensuite le ramasser (touche R). Aucune ne tombe directement dans
    // l'inventaire - c'est la meme regle qu'en Phase 6, et c'est ce qui fait
    // qu'une recolte se VOIT.
    Clay,   // motte d'argile creusee dans un gisement (world/Deposits)
    Sand,   // poignee de sable creusee dans un gisement (world/Deposits)
    Resin,  // resine recoltee sur un resineux SANS l'abattre (world/Resin)
    Fiber,  // tige a fibres arrachee (ronce, ortie) (world/FiberPlants)
    // --- PHASE 28 : les objets FACONNES (world/Crafting) ---------------------
    // Ajoutes EN FIN d'enum : les valeurs 0..7 gardent leur numero, donc tout
    // ce qui indexe par GroundItemKind (Inventory, groundMeshes de main.cpp)
    // reste valable pour les matieres existantes. Meme regle que la Phase 22 :
    // tout objet faconne nait AU SOL.
    Cord,   // ligature : trois fibres torsadees a la main
    Axe,    // hache emmanchee : eclat + branche + ligature, assemblee au sol
    Hearth, // foyer : cercle de pierres + petit bois. FIXE (non ramassable)
    Ash,    // cendre laissee par un foyer qui s'eteint
    Pitch,  // poix : resine + cendre chauffees pres d'un foyer allume
    // --- PHASE 28, Agent 3 : les objets qui BRANCHENT un effet -----------------
    PitchedAxe, // hache renforcee : hache + poix (rendement +2 buches au lieu de +1)
    Shelter,    // abri de branches : buches + branches + ligatures. FIXE (non ramassable)
};

inline constexpr uint32_t kGroundItemKindCount = 15;

// PHASE 28. Faux pour les objets FIXES (le foyer) : Ground::findNearest les
// ignore, ils ne peuvent donc ni etre ramasses ni entrer dans l'inventaire.
// Vrai pour toutes les autres natures (comportement anterieur inchange).
inline constexpr bool groundItemIsPortable(GroundItemKind kind) {
    return kind != GroundItemKind::Hearth && kind != GroundItemKind::Shelter;
}

// Demi-epaisseur de l'objet couche, en unites monde. Sert a le poser sur le sol
// sans qu'il s'enfonce ni qu'il flotte : son centre est a terrain + ce rayon.
float groundItemRestRadius(GroundItemKind kind);

// Libelle francais au singulier ("buche"), pour les messages a l'unite.
const char* groundItemLabel(GroundItemKind kind);

// Libelle au singulier PRECEDE de son article indefini ("une buche", "un
// eclat"). Le genre fait partie du libelle et non de la phrase appelante : en
// francais il depend du mot, et coller un "une " devant groundItemLabel donnait
// "une eclat" - faute visible dans le journal des la premiere percussion.
const char* groundItemLabelIndefinite(GroundItemKind kind);

// Libelle au pluriel ("buches"), pour les compteurs d'inventaire.
const char* groundItemLabelPlural(GroundItemKind kind);

// Un objet ramassable pose au sol. Son mesh est genere a dimensions ABSOLUES et
// n'est donc jamais mis a l'echelle : la transformation se reduit a une
// translation et deux rotations, strictement rigide.
struct GroundItem {
    glm::vec3 position{0.0f}; // centre de l'objet
    float yaw = 0.0f;         // orientation dans le plan horizontal, radians
    float roll = 0.0f;        // rotation autour de l'axe de l'objet, radians
    GroundItemKind kind = GroundItemKind::Log;

    glm::mat4 modelMatrix() const;
};

} // namespace vercora
