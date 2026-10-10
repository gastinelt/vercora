#pragma once

#include "editor/PlacedObject.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vercora {

class Heightmap;

// ---------------------------------------------------------------------------
// PHASE 15 : le format de scene .vcsc (Vercora SCene).
// ---------------------------------------------------------------------------
// Fichier TEXTE, une directive par ligne, champs separes par des espaces,
// commentaires apres '#'. Encodage ASCII. Fins de ligne LF ecrites, LF et CRLF
// acceptees en lecture.
//
//     # Vercora scene v1
//     version 1
//     terrain vercora_zone_1m.vchm
//     crs 2154
//     object rocher 907512.250 6440337.750 0.000 135.000 1.400 1
//     object cairn  907518.900 6440341.100 0.000   0.000 1.000 1
//
// Directives :
//   version <n>     obligatoire, en premier. 1 pour cette session.
//   terrain <nom>   INFORMATIF : le nom du terrain sur lequel la scene a ete
//                   composee. Sert au diagnostic, jamais a refuser un
//                   chargement - la meme zone existe au pas de 1 m et de 2 m,
//                   et une scene doit passer de l'un a l'autre (c'est meme la
//                   raison d'etre du georeferencement ci-dessous).
//   crs <epsg>      2154 = Lambert 93 : les objets portent des coordonnees
//                   PROJETEES. 0 = terrain sans reference geographique (le
//                   terrain synthetique) : coordonnees LOCALES du moteur.
//   object ...      un objet pose. Sept champs, dans cet ordre :
//                     1. identifiant d'asset (voir editor/AssetCatalog.hpp)
//                     2. est   (m) ou x local si crs 0
//                     3. nord  (m) ou z local si crs 0
//                     4. dz    (m) altitude RELATIVE au terrain (voir plus bas)
//                     5. cap   (degres) rotation autour de la verticale
//                     6. echelle (facteur uniforme, 1 = taille reelle)
//                     7. solide (1 = bloque le joueur, 0 = traversable)
//
// --- POURQUOI DES COORDONNEES PROJETEES ET NON LOCALES ----------------------
// Le repere du moteur est CENTRE sur le terrain charge : (0, 0) est le milieu de
// l'emprise. Une position locale ne veut donc rien dire sans savoir QUELLE
// emprise etait chargee. Cette zone existe deja sous quatre emprises differentes
// (la dalle de Phase 7, la zone de 25 km2, ses versions a 1 m et a 2 m) et
// d'autres decoupages viendront. Enregistrer du Lambert 93 rend la scene
// independante de tout ca : un rocher pose sur la dalle isolee se retrouve au
// meme endroit du massif quand on charge la zone entiere.
//
// --- POURQUOI UNE ALTITUDE RELATIVE ET NON ABSOLUE --------------------------
// Meme raisonnement, applique a la verticale. La surface au pas de 2 m n'est pas
// exactement celle au pas de 1 m (c'est le meme relief re-echantillonne, pas le
// meme maillage), et le repli sur l'ancetre du quadtree peut afficher une tuile
// plus grossiere pendant une frame. Une altitude ABSOLUE ferait donc flotter ou
// enterrer chaque objet a la moindre variation. `dz` est l'ecart au terrain
// SOUS l'objet : un dz nul veut dire "pose au sol", et ca reste vrai partout.
// L'altitude absolue se recalcule au chargement par heightAt(x, z) + dz.
//
// --- CE QUE LE FORMAT NE STOCKE PAS, ET POURQUOI ----------------------------
//   * L'IDENTIFIANT d'objet : il est attribue a la volee au chargement. C'est
//     une identite de SESSION (selection, annulation), pas une propriete de la
//     scene. La stocker rendrait tout deplacement de ligne visible dans un diff
//     sans rien apporter.
//   * La GEOMETRIE : la scene ne contient que des references d'assets. Un
//     fichier de scene fait quelques centaines d'octets et se relit a l'oeil.
//
// --- ROBUSTESSE -------------------------------------------------------------
// Une directive inconnue est IGNOREE avec un avertissement plutot que de faire
// echouer le chargement : c'est ce qui permettra a une version ulterieure
// d'ajouter des champs sans qu'un fichier plus recent devienne illisible. Une
// ligne `object` mal formee est comptee et sautee - perdre un objet vaut mieux
// que perdre la scene.

// Ce que rend un chargement.
struct SceneLoadResult {
    // Le fichier existe-t-il ? Un fichier absent n'est PAS une erreur : c'est le
    // cas normal du premier lancement, et le jeu doit demarrer sans scene.
    bool fileExisted = false;

    // Le fichier a-t-il ete lu jusqu'au bout avec un en-tete valide ?
    bool ok = false;

    std::vector<PlacedObject> objects;

    // Lignes `object` refusees (champs manquants, nombre illisible).
    uint32_t skippedObjects = 0;

    // Directives inconnues rencontrees.
    uint32_t unknownDirectives = 0;

    // Objets tombant hors de l'emprise du terrain charge. Ils sont CONSERVES
    // (la scene peut avoir ete composee sur une emprise plus large), mais
    // comptes : c'est le premier symptome d'une scene associee au mauvais
    // terrain.
    uint32_t outsideExtent = 0;

    // Ce que disait l'en-tete.
    std::string terrainLabel;
    uint32_t version = 0;
    uint32_t epsg = 0;

    // Message d'echec, vide si ok.
    std::string error;
};

// Charge une scene. `terrain` fournit a la fois le georeferencement (pour
// convertir les coordonnees projetees) et la surface (pour recalculer les
// altitudes absolues depuis les dz).
//
// Ne LANCE PAS : toute defaillance ressort dans le resultat. Un editeur qui
// refuse de demarrer parce qu'une ligne est fautive serait le pire des
// comportements pour un outil dont le fichier s'edite aussi a la main.
SceneLoadResult loadSceneFile(const std::string& path, const Heightmap& terrain);

// Ecrit une scene. Cree les dossiers parents si besoin.
// `terrainLabel` est recopie tel quel dans la directive `terrain`.
// Renvoie false et journalise en cas d'echec d'ecriture.
//
// L'ecriture passe par un fichier TEMPORAIRE renomme a la fin : une coupure au
// milieu d'un `save` laisserait sinon un fichier tronque a la place de la scene,
// c'est-a-dire detruirait le travail qu'on etait en train de sauvegarder.
bool saveSceneFile(const std::string& path, const std::vector<PlacedObject>& objects,
                   const Heightmap& terrain, const std::string& terrainLabel);

// Chemin de scene associe PAR CONVENTION a un terrain :
//   assets/terrain/vercora_zone_1m.vchm -> assets/scenes/vercora_zone.vcsc
// Le suffixe de resolution est retire, exactement comme pour l'orthophoto
// (main.cpp, orthoPathFor) et pour la meme raison : 1 m et 2 m sont deux
// echantillonnages du MEME endroit, ils partagent donc leur scene.
//
// `scenesDirectory` est le dossier ou ranger les scenes (assets/scenes/).
std::string scenePathFor(const std::string& terrainPath,
                         const std::string& scenesDirectory);

} // namespace vercora
