#pragma once

#include "core/HeightSampler.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vercora {

// Reference geographique d'une grille d'altitudes.
//
// PHASE 7 : c'est la trace de la provenance des donnees. Le moteur travaille en
// coordonnees LOCALES (terrain centre sur l'origine, cf. plus bas) parce qu'un
// Lambert 93 brut - de l'ordre de 907 000 m en X et 6 440 000 m en Y - depasse
// largement la precision d'un float32 : a 6,4 millions, l'ecart entre deux
// float consecutifs vaut deja 0,5 m, ce qui ferait trembler la geometrie et
// ruinerait le depth buffer. Le georeferencement est donc CONSERVE ici en
// double, a cote de la grille, plutot que porte par les sommets.
struct GeoReference {
    // Coordonnees projetees de l'echantillon (ix = 0, iz = 0), c'est-a-dire le
    // coin NORD-OUEST de la grille (voir l'orientation ci-dessous).
    double originEast = 0.0;
    double originNorth = 0.0;

    // Code EPSG de la projection. 2154 = Lambert 93 (metropole). 0 = grille
    // sans reference geographique, ce qui est le cas du terrain synthetique.
    uint32_t epsg = 0;

    // true si les altitudes sont les altitudes ABSOLUES mesurees (IGN69 pour la
    // metropole), false si elles ont ete ramenees a une base nulle.
    bool absoluteAltitude = false;

    bool isGeoreferenced() const { return epsg != 0; }
};

// Grille reguliere d'altitudes.
//
// DEUX PROVENANCES, une seule classe :
//   - bruit procedural genere en memoire (Phase 4), par le constructeur Params ;
//   - dalle LiDAR HD reelle chargee depuis un fichier .vchm (Phase 7), par
//     assets/TerrainFile.hpp, qui passe par le constructeur "grille toute
//     faite" ci-dessous.
// TerrainMesh, les shaders, la camera et la foret ne voient AUCUNE difference :
// c'etait tout l'interet de passer par cette classe des la Phase 4 plutot que
// de generer le mesh directement, et c'est ce qui a rendu la Phase 7 possible
// sans toucher au rendu.
//
// Le terrain est CENTRE sur l'origine XZ : x et z varient dans
// [-worldSize/2, +worldSize/2]. Y est l'altitude (repere du moteur : Y up).
//
// ORIENTATION (n'a de sens que pour une grille georeferencee) : ix croissant va
// vers l'EST, iz croissant va vers le SUD. Le moteur etant en repere main droite
// avec Y vers le haut, X ^ Y = Z ; en posant X = Est on obtient
// Z = Est ^ Haut = -Nord. Le nord est donc en -Z, et ranger les lignes du nord
// vers le sud est la seule facon d'afficher le relief sans le mettre en MIROIR.
class Heightmap : public HeightSampler {
public:
    struct Params {
        // Echantillons par cote. 128x128 = 16384 hauteurs = 64 Ko de float :
        // volontairement modeste (contrainte RAM de la machine de dev).
        uint32_t resolution = 128;

        // Cote du terrain en unites monde.
        float worldSize = 100.0f;

        // Denivele total vise entre le point le plus bas et le plus haut.
        float amplitude = 30.0f;

        // Graine du bruit : deux Heightmap de meme graine sont identiques bit a
        // bit, ce qui rend les tests reproductibles.
        uint32_t seed = 20260724u;

        // Nombre d'octaves du fBm : chacune ajoute un detail deux fois plus fin
        // et deux fois moins haut. Au-dela de ~6, le detail passe sous la taille
        // d'une cellule de la grille et n'est plus representable.
        uint32_t octaves = 5;

        // Nombre de "grandes formes" du relief sur toute la largeur du terrain.
        float baseFrequency = 3.0f;

        // Exposant applique au bruit normalise avant mise a l'echelle. > 1
        // creuse les vallees et garde les sommets pointus (relief plus lisible
        // qu'un bruit brut, qui donne une houle uniforme).
        float reliefExponent = 1.4f;

        // Dosage entre relief doux (0 : collines arrondies, fBm classique) et
        // relief de cretes (1 : bruit "ridged", aretes marquees et versants
        // raides). Une valeur elevee produit de vraies pentes fortes, seule
        // facon d'exercer la coloration par pente du shader - et accessoirement
        // plus proche de ce que donneront les falaises du Vercors.
        float ridgeWeight = 0.6f;
    };

    // Genere immediatement la grille par bruit fractal. Lance
    // std::runtime_error si les parametres ne peuvent pas produire une surface
    // (resolution < 2, taille <= 0, ...).
    explicit Heightmap(const Params& params);

    // Memes parametres par defaut. Une SURCHARGE plutot qu'un argument par
    // defaut `= {}` : GCC et Clang refusent de construire une struct IMBRIQUEE
    // dotee d'initialiseurs de membres dans un argument par defaut de la classe
    // englobante (CWG 1397), la ou MSVC l'accepte. Le corps d'une fonction
    // membre, lui, voit la classe complete : la delegation compile partout.
    explicit Heightmap() : Heightmap(Params{}) {}

    // Adopte une grille DEJA CONSTRUITE, sans rien generer : c'est par ici
    // qu'entrent les donnees LiDAR (voir assets/TerrainFile.hpp).
    //   heights   : resolution*resolution altitudes, indexees [iz * res + ix],
    //               lignes du nord vers le sud pour une grille georeferencee.
    //   spacing   : distance monde entre deux echantillons voisins, en metres.
    //   label     : ce que renverra describe(), affiche dans le titre.
    // Lance std::runtime_error si la grille n'est pas exploitable (taille
    // incoherente, pas nul ou negatif, valeur non finie). La validation est
    // faite ICI et non chez l'appelant : toute autre source de donnees future
    // en beneficiera sans avoir a y penser.
    Heightmap(std::vector<float> heights, uint32_t resolution, float spacing,
              std::string label, const GeoReference& geo = {});

    uint32_t resolution() const { return params_.resolution; }
    float    worldSize()  const { return params_.worldSize; }

    // Provenance geographique. epsg == 0 pour le terrain synthetique.
    const GeoReference& geo() const { return geo_; }

    // Coordonnees projetees (Lambert 93 pour une dalle IGN) d'un point du monde
    // moteur. Inverse exact du recentrage applique au chargement : c'est le seul
    // endroit qui documente le lien entre les deux reperes, et il est verifie
    // par les tests headless.
    // N'a de sens que si geo().isGeoreferenced() ; renvoie sinon les
    // coordonnees locales telles quelles.
    void toProjected(float worldX, float worldZ,
                     double* east, double* north) const;

    // PHASE 15 : l'INVERSE de toProjected, des coordonnees projetees vers le
    // repere du moteur.
    //
    // Cette formule existait deja, recopiee a la main dans main.cpp pour
    // l'option --start de la Phase 11, et l'editeur en avait besoin une
    // troisieme fois (le fichier de scene stocke des coordonnees projetees).
    // Deux copies d'une convention d'axes dont une seule serait corrigee le jour
    // ou elle changerait, c'est exactement le genre de piege que le commentaire
    // de toProjected ci-dessus decrit : la conversion vit desormais ICI, dans
    // les deux sens, a cote de l'orientation qu'elle applique.
    //
    // AUCUN ecretage : un point hors emprise rend des coordonnees hors emprise,
    // a charge de l'appelant de decider ce qu'il en fait (main.cpp refuse un
    // --start dehors, l'editeur signale un objet dehors et le garde).
    void fromProjected(double east, double north,
                       float* worldX, float* worldZ) const;

    // Distance monde entre deux echantillons voisins.
    float spacing() const { return spacing_; }

    // Bornes reelles des altitudes generees (et non l'amplitude demandee) :
    // le shader s'en sert pour normaliser sa coloration par altitude.
    float minHeight() const { return minHeight_; }
    float maxHeight() const { return maxHeight_; }

    // Altitude d'un echantillon de la grille. Les indices sont ecretes dans
    // [0, resolution-1] : appeler avec ix = -1 renvoie la colonne 0.
    float at(int64_t ix, int64_t iz) const;

    // Position monde d'un echantillon (le long de X pour ix, de Z pour iz).
    float worldCoord(uint32_t index) const;

    // Nom lisible, repris dans MeshData et le titre de la fenetre.
    std::string describe() const;

    // --- HeightSampler ---
    // Interpolation AFFINE sur le triangle qui contient (worldX, worldZ), avec
    // la meme diagonale de decoupe que TerrainMesh : la valeur renvoyee est
    // donc exactement l'altitude de la surface affichee, pas une approximation.
    // Hors domaine, les coordonnees sont ecretees sur le bord.
    float heightAt(float worldX, float worldZ) const override;

private:
    // Facteurs communs aux deux constructeurs : derive spacing_/halfSize_ de la
    // resolution, puis balaye la grille pour en tirer minHeight_/maxHeight_ en
    // refusant toute valeur non finie.
    void finalize();

    Params params_;
    std::vector<float> heights_; // resolution * resolution, indexe [iz * res + ix]

    // Ce que renvoie describe(). Porte par une chaine plutot que reconstruit a
    // la demande : une grille chargee n'a pas de "parametres de bruit" a decrire,
    // seulement le nom de sa dalle d'origine.
    std::string label_;

    GeoReference geo_{};

    float spacing_ = 0.0f;
    float halfSize_ = 0.0f;
    float minHeight_ = 0.0f;
    float maxHeight_ = 0.0f;
};

} // namespace vercora
