#pragma once

#include "world/TreeSpecies.hpp"

#include <cstdint>
#include <unordered_map>

namespace vercora {

// ===========================================================================
// PHASE 22 : la RESINE DE PIN, recoltee SANS abattre l'arbre.
// ===========================================================================
//
// PHASE 26 : L'ESSENCE EST DEVENUE UNE VRAIE PROPRIETE DU MONDE.
//
// Ce fichier portait, en Phase 22, le paragraphe suivant : « Le jeu n'a AUCUNE
// notion d'essence d'arbre [...] Il n'y a donc RIEN, ni dans la geometrie ni
// dans les donnees, qui permette de dire qu'un arbre donne est un resineux. »
// D'ou un MARQUAGE ARBITRAIRE - un hachage de l'indice de l'arbre - et la dette
// que la phase assumait explicitement : « un joueur ne peut pas VOIR quel arbre
// est resineux avant d'essayer, puisque tous les arbres ont le meme mesh ».
//
// CETTE DETTE EST REFERMEE. world/TreeSpecies.hpp decide desormais de l'essence
// en fonction de la POSITION et de l'exposition adret/ubac, world/TreeMesh.hpp
// donne a chaque essence sa silhouette, et Tree::species porte le resultat. Un
// joueur qui vise un conifere le RECONNAIT a sa forme conique avant d'appuyer.
//
// CE QUI CHANGE ICI, CONCRETEMENT : ResinStand ne decide plus de rien. Il ne
// tient plus de graine d'essence, plus de part de resineux, plus de hachage ;
// il RECOIT l'essence de l'arbre vise en argument de tap(). Il ne reste dans
// cette classe que ce qui lui appartient vraiment : la memoire des arbres deja
// saignes et les delais.
//
// C'ETAIT LA SEULE FACON D'ETRE SUR QU'IL N'Y A PAS DEUX VERITES. Faire lire a
// ResinStand la meme fonction que le rendu aurait suffi a les faire coincider
// aujourd'hui ; le lui faire DONNER par l'appelant garantit qu'ils ne peuvent
// pas diverger demain, parce qu'il n'y a plus de second calcul a maintenir. La
// meme exigence que la Phase 23 pour le soleil et la Phase 25 pour l'ecart
// d'incidence.
//
// CE QUI DIFFERE DE L'ABATTAGE, ET C'EST LE POINT. Le bois demande de TUER
// l'arbre : Forest::fell le couche, le debite, et la Phase 17 en fait payer le
// prix au milieu (Habitat::recordFelling). La resine, elle, se preleve sur un
// arbre VIVANT et DEBOUT : rien n'est abattu, rien n'est debite, l'arbre reste
// exactement dans l'etat Standing. En echange, il ne redonne pas de resine
// avant un long moment - c'est la seule contrainte du systeme, et elle est
// suffisante pour que saigner soit une decision et non un reflexe.
//
// Ne connait ni Vulkan, ni la Forest, ni le terrain : un indice d'arbre, un
// temps. Entierement testable sans GPU.
// ===========================================================================

// Issue d'une tentative de recolte.
enum class ResinResult {
    Success,      // la resine coule : un objet tombe au pied de l'arbre
    NotConifer,   // ce n'est pas un resineux, il n'y a rien a saigner
    StillDry,     // resineux, mais deja saigne trop recemment
    NoTree,       // aucun arbre vise
};

// Message francais pret a journaliser pour une issue donnee.
const char* resinResultMessage(ResinResult result);

struct ResinParams {
    // PHASE 26 : `coniferShare` et `seed` ont DISPARU d'ici. Elles ne decrivaient
    // pas la resine, elles decrivaient le peuplement ; elles vivent desormais
    // dans TreeSpeciesParams, ou elles servent AUSSI a ce qui est dessine. La
    // valeur 0,55 y a ete reprise telle quelle sous le nom coniferShareNeutral.

    // Delai, en secondes de jeu, avant qu'un arbre deja saigne redonne de la
    // resine. 600 s = dix minutes. C'est LONG a l'echelle d'une partie, et c'est
    // volontaire : la resine doit se recolter en se DEPLACANT d'un arbre a
    // l'autre, pas en restant plante devant le meme. L'ordre de grandeur est
    // celui de HabitatParams::loggingDecay (900 s), l'autre grandeur du jeu qui
    // dit "cette zone a donne ce qu'elle avait".
    float flowDelay = 600.0f;

    // Nombre de recoltes qu'un meme arbre peut donner avant d'etre epuise pour
    // de bon. 3 : au-dela, saigner un arbre le tuerait vraiment, et ce fichier
    // affirme exactement le contraire. Zero signifierait "sans limite".
    uint32_t maxTapsPerTree = 3;
};

// La memoire des arbres deja saignes, et elle seule. Le marquage des
// resineux a quitte cette classe en Phase 26 : voir l'en-tete.
class ResinStand {
public:
    explicit ResinStand(const ResinParams& params = {});

    // Tente une recolte sur l'arbre d'indice donne, a l'instant `now` (secondes
    // ecoulees depuis le debut de la partie, la meme horloge que le reste du
    // gameplay). treeIndex < 0 signifie "rien de vise".
    //
    // `species` est l'essence de CET arbre, telle que le monde la porte
    // (Tree::species). Elle est passee et non deduite : voir l'en-tete. Un
    // appelant qui se tromperait d'arbre ferait couler de la resine d'un feuillu
    // - mais il aurait aussi dessine un feuillu, donc l'incoherence serait
    // VISIBLE, ce qui est precisement ce qu'on cherchait.
    //
    // N'ABAT RIEN et ne touche a aucun arbre : cette classe ne connait meme pas
    // la Forest. C'est l'appelant qui, sur Success, depose l'objet au sol.
    ResinResult tap(int treeIndex, TreeSpecies species, float now);

    // Nombre de recoltes deja faites sur cet arbre. 0 pour un arbre jamais
    // saigne - et il n'est alors PAS memorise, exactement comme une cellule
    // d'Habitat revenue a zero.
    uint32_t tapCount(int treeIndex) const;

    // Secondes restantes avant que cet arbre redonne de la resine, a l'instant
    // `now`. 0 s'il est pret (ou jamais saigne), et 0 aussi s'il est epuise -
    // c'est tapCount qu'il faut lire pour distinguer les deux.
    float secondsUntilFlow(int treeIndex, float now) const;

    // Arbres reellement memorises. Expose pour les tests : une memoire qui
    // grossirait a chaque visee plutot qu'a chaque recolte passerait sinon
    // inapercue.
    std::size_t trackedTreeCount() const { return taps_.size(); }

    const ResinParams& params() const { return params_; }

private:
    struct Tap {
        uint32_t count = 0;
        float lastTapTime = 0.0f;
    };

    ResinParams params_;
    std::unordered_map<int, Tap> taps_;
};

} // namespace vercora
