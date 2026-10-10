#pragma once

#include "renderer/MeshData.hpp"

#include <cstdint>

namespace vercora {

// Geometries synthetiques bas-poly de la Phase 5, generees EN CODE (aucun
// fichier, aucun telechargement), dans le meme MeshData que le cube en dur, le
// loader .obj et le terrain : la classe Mesh et le Renderer les consomment sans
// rien savoir de leur origine.
//
// --- Convention CANONIQUE, valable pour treeMeshData et stumpMeshData -------
// Le mesh est genere "unite" : pied du tronc a l'origine, axe le long de +Y,
// hauteur totale exactement 1.0, et toutes les largeurs exprimees en fraction
// de cette hauteur. Une instance se place donc par
//     translate(pied) * rotateY(orientation) * scale(hauteur reelle)
// c'est-a-dire une rotation suivie d'une echelle UNIFORME - exactement le
// contrat de DrawItem, qui autorise mat3(model) comme normal matrix. Un mesh
// deja a l'echelle obligerait au contraire a etirer separement la hauteur et le
// rayon pour varier la silhouette, donc a une echelle NON uniforme, donc a
// transporter une vraie normal matrix. Le choix du mesh unite est ce qui evite
// tout ce probleme.
//
// Consequence assumee : tous les arbres ont les memes PROPORTIONS, seule leur
// taille varie. La variete de silhouette viendra de plusieurs meshes distincts
// le jour ou ce sera un sujet, pas d'un etirement.

// Proportions communes a l'arbre et a sa souche, en fraction de la hauteur
// totale de l'arbre. Regroupees ici parce que la souche doit imperativement
// avoir le meme rayon de tronc et la meme hauteur de coupe que l'arbre dont
// elle prend la place, sinon le raccord se voit.
struct TreeProportions {
    // Rayon du tronc au pied.
    float trunkRadius = 0.045f;
    // Rayon du tronc juste sous le houppier (un tronc legerement conique se lit
    // beaucoup mieux qu'un cylindre parfait, pour zero triangle de plus).
    float topRadius = 0.028f;
    // Hauteur a laquelle le tronc sort du sol nu et ou commence le houppier.
    float crownBase = 0.35f;
    // Rayon maximal du houppier.
    float crownRadius = 0.22f;
    // Hauteur de la souche laissee au sol apres l'abattage. C'est aussi la
    // hauteur de la CHARNIERE autour de laquelle l'arbre bascule.
    float stumpHeight = 0.045f;

    // Nombre de secteurs autour de l'axe. 8 = bas-poly assume, silhouette
    // encore lisible, et un arbre complet reste sous les 200 triangles.
    uint32_t sides = 8;
};

// Un conifere : tronc legerement conique + houppier en deux cones empiles.
// Le sommet du cone superieur est exactement a y = 1.0.
MeshData treeMeshData(const TreeProportions& proportions = {});

// ---------------------------------------------------------------------------
// PHASE 26 : le FEUILLU, seconde silhouette.
// ---------------------------------------------------------------------------
// Le paragraphe de convention ci-dessus annoncait que « la variete de silhouette
// viendra de plusieurs meshes distincts le jour ou ce sera un sujet, pas d'un
// etirement ». C'est ce jour-la : world/TreeSpecies.hpp decide desormais quelle
// essence pousse ou, et il lui faut deux formes a opposer.
//
// LE TRONC EST LE MEME, ET CE N'EST PAS UN RACCOURCI. La souche
// (stumpMeshData) interpole son rayon de coupe sur la droite du tronc de
// l'arbre ; donner au feuillu un autre tronc obligerait a une seconde souche,
// et surtout ferait que la souche laissee par un feuillu abattu ne coinciderait
// plus avec le trait de coupe. UNE souche sert les deux essences parce que les
// deux partagent exactement le meme tronc. Ce qui les distingue est le
// HOUPPIER, et c'est aussi ce qu'un joueur lit de loin.
//
// La silhouette : un houppier ARRONDI, fait d'anneaux empiles dont le rayon
// suit un profil elliptique, et LISSE (normales radiales partagees) la ou le
// conifere est FACETTE. L'opposition ne tient donc pas qu'au contour : les deux
// arbres n'accrochent pas la lumiere de la meme facon.
//
// Meme convention unite que treeMeshData : pied a l'origine, hauteur totale
// exactement 1.0, donc meme matrice d'instance et meme echelle uniforme.
MeshData broadleafTreeMeshData(const TreeProportions& proportions = {});

// La souche qui reste apres l'abattage : le bas du tronc, coupe net a
// proportions.stumpHeight, avec un disque de bois clair en haut. Meme
// convention unite que treeMeshData, donc meme echelle d'instance.
MeshData stumpMeshData(const TreeProportions& proportions = {});

// --- Objets ramassables au sol ---------------------------------------------
// Ceux-ci ne sont PAS a l'echelle unite : leurs dimensions sont absolues et
// fixes (une buche fait la meme taille quel que soit l'arbre dont elle vient).
// Une instance se pose donc par translate * rotate SANS aucune echelle, ce qui
// rend la normal matrix exacte au sens strict.

// Une buche : cylindre couche le long de l'axe +X, centre sur l'origine, avec
// une face de coupe claire a chaque bout.
MeshData logMeshData();

// Une branche : meme forme, plus fine et plus courte, teinte plus sombre.
MeshData branchMeshData();

// Longueurs/rayons des objets ci-dessus, en unites monde. Exposes parce que le
// code de placement en a besoin pour poser les buches sur le sol sans qu'elles
// s'enfoncent ni flottent.
inline constexpr float kLogLength    = 1.6f;
inline constexpr float kLogRadius    = 0.16f;
inline constexpr float kBranchLength = 0.9f;
inline constexpr float kBranchRadius = 0.06f;

} // namespace vercora
