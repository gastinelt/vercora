#pragma once

#include "renderer/DrawItem.hpp" // MeshHandle / kInvalidMesh
#include "renderer/MeshData.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vercora {

// ---------------------------------------------------------------------------
// PHASE 15 : le catalogue des modeles placables.
// ---------------------------------------------------------------------------
// Deux provenances, un seul type :
//   - les assets GENERES EN CODE (editor/EditorAssets.hpp), toujours presents ;
//   - les fichiers trouves dans un dossier scanne au demarrage de l'editeur
//     (assets/editor/ par defaut).
// Rien en aval ne distingue les deux : c'est le meme principe qu'en Phase 7,
// ou une heightmap procedurale et une dalle LiDAR entrent toutes deux par
// `Heightmap` et ou plus rien ensuite ne sait laquelle est laquelle.
//
// Le catalogue ne connait PAS Vulkan. Il produit des `MeshData` ; c'est
// l'appelant qui les enregistre aupres du Renderer et redepose la poignee
// obtenue dans `meshHandle`. C'est ce qui permet de tester tout le catalogue -
// scan, normalisation, empreintes de collision, doublons - sans GPU.

// Une entree du catalogue.
struct AssetEntry {
    // Identifiant ecrit dans le fichier de scene. Sans espace ni accent (voir
    // sanitizeAssetId). Unique dans un catalogue.
    std::string id;

    // Libelle lisible, affiche dans la liste. Jamais serialise.
    std::string description;

    // Chemin du fichier d'origine, vide pour un asset genere en code. Sert au
    // journal et au diagnostic ("d'ou vient ce modele ?").
    std::string sourcePath;

    // Geometrie cote CPU, DEJA normalisee (voir normalizeAssetMesh).
    MeshData mesh;

    // --- Empreinte de collision, a l'echelle 1 ------------------------------
    // Deduite de la boite englobante du mesh, et non decrite a la main : un
    // fichier telecharge n'apporte aucune information de collision, et exiger
    // une fiche par asset rendrait le catalogue inutilisable pour son but
    // premier (poser vite des choses pour voir).
    //
    // `collisionRadius` est la demi-diagonale horizontale de la boite, donc le
    // rayon du plus petit cylindre vertical qui CONTIENT l'objet. Volontairement
    // le contenant et non l'inscrit : un obstacle un peu trop gros se contourne,
    // un obstacle trop petit se traverse par les coins - et traverser un rocher
    // est bien plus visible que le raser d'un demi-metre.
    float collisionRadius = 0.0f;

    // Hauteur totale (le mesh part de y = 0 par convention).
    float height = 0.0f;

    // Demi-etendues horizontales de la boite englobante. Exposees pour le
    // journal et pour les tests : c'est la seule facon de verifier qu'un mesh
    // charge a bien ete recentre.
    float halfExtentX = 0.0f;
    float halfExtentZ = 0.0f;

    // Poignee de geometrie, remplie par l'appelant APRES `Renderer::addMesh`.
    // kInvalidMesh tant que le catalogue n'a pas ete televerse (et toujours,
    // dans les tests headless).
    MeshHandle meshHandle = kInvalidMesh;
};

// Rend un identifiant utilisable dans le fichier de scene : minuscules ASCII,
// chiffres, '_', '-' et '.' conserves ; tout le reste devient '_'. Une chaine
// vide devient "asset".
//
// Necessaire parce que le format de scene est a champs separes par des espaces :
// un asset nomme "gros rocher.obj" produirait une ligne a un champ de trop, et
// le fichier ne se relirait pas. Normaliser a l'entree plutot que citer les
// champs garde le format lisible et diffable a l'oeil.
std::string sanitizeAssetId(const std::string& raw);

// Realigne un mesh sur la convention d'ancrage des assets d'editeur :
// centre en X et Z sur sa boite englobante, pied a y = 0.
//
// Appliquee a TOUT mesh charge depuis un fichier, jamais aux builtins (qui la
// respectent deja par construction, et le test le verifie). Un .obj pris dans
// une bibliotheque libre a son origine ou l'auteur l'a laissee : au centre du
// volume, dans un coin, ou a plusieurs dizaines de metres si la scene d'origine
// en contenait plusieurs. Sans ce recentrage, "poser au point vise" poserait
// l'objet a cote du point vise, d'un ecart different pour chaque modele.
void normalizeAssetMesh(MeshData& mesh);

// Renseigne collisionRadius / height / halfExtent* depuis la geometrie.
void computeAssetBounds(AssetEntry& entry);

class AssetCatalog {
public:
    // Ajoute les assets generes en code. A appeler en premier : ils ont priorite
    // en cas de collision d'identifiant avec un fichier (voir scanDirectory).
    void addBuiltins();

    // Scanne un dossier (non recursif) et ajoute un asset par fichier reconnu.
    // Renvoie le nombre d'assets REELLEMENT ajoutes.
    //
    // - Un dossier absent n'est pas une erreur : l'editeur doit demarrer sur une
    //   installation qui n'a que les builtins. Le chemin attendu est journalise,
    //   pour qu'on sache ou deposer des modeles.
    // - Les fichiers sont tries par NOM avant traitement. Pas par gout de
    //   l'ordre : les touches 1..9 de l'editeur designent des rangs dans cette
    //   liste, et `std::filesystem::directory_iterator` ne garantit aucun ordre.
    //   Sans le tri, la meme touche pourrait poser un modele different d'une
    //   execution a l'autre.
    // - Un fichier illisible ou vide est SIGNALE et saute, il n'interrompt pas
    //   le scan : un modele casse ne doit pas priver l'editeur des autres.
    uint32_t scanDirectory(const std::string& directory);

    // Extensions reconnues par scanDirectory, en minuscules et avec le point.
    static const std::vector<std::string>& supportedExtensions();

    const std::vector<AssetEntry>& entries() const { return entries_; }
    std::vector<AssetEntry>& entries() { return entries_; }

    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    // Indice de l'asset d'identifiant `id`, ou -1.
    int indexOf(const std::string& id) const;

    // Liste texte multi-lignes, prete a etre journalisee. C'est L'INTERFACE du
    // catalogue pour cette session : la consigne demandait explicitement une
    // liste texte plutot qu'un navigateur graphique, l'editeur etant a usage
    // developpeur. `selected` marque l'entree courante d'un chevron.
    std::string listing(int selected = -1) const;

private:
    // Ajoute une entree en refusant les identifiants deja pris. Renvoie false
    // (et journalise) sur doublon ou geometrie vide.
    bool addEntry(AssetEntry entry);

    std::vector<AssetEntry> entries_;
};

} // namespace vercora
