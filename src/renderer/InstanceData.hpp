#pragma once

#include <glm/glm.hpp>

namespace vercora {

// Une instance de vegetation : ce qui distingue un arbre (ou une touffe d'herbe)
// de tous les autres, envoye au GPU dans un buffer a part.
//
// PHASE 11. Jusqu'ici chaque objet du monde etait un DrawItem, donc UN appel de
// dessin et une matrice poussee en push constant. C'est parfait pour quelques
// dizaines d'arbres abattables ; ca ne l'est plus du tout pour les dizaines de
// milliers d'arbres et les centaines de milliers de touffes d'herbe que demande
// une zone de 25 km2. Un appel de dessin par touffe couterait, a lui seul, bien
// plus cher que tout le terrain.
//
// D'ou l'instanciation : UNE geometrie, UN appel de dessin, N instances lues
// dans un second binding de sommets cadence par instance
// (VK_VERTEX_INPUT_RATE_INSTANCE) au lieu de par sommet.
//
// POURQUOI PAS UNE MATRICE 4x4 PAR INSTANCE (64 octets). Une instance de
// vegetation ne subit qu'une rotation autour de Y, une echelle uniforme et une
// translation : sept flottants suffisent a la decrire (les deux derniers etant
// des variations d'aspect), contre seize pour la matrice generale. Sur 300 000
// touffes, la difference est de 11 Mo par frame ecrits sur un bus partage avec
// le CPU. La matrice est reconstituee dans le vertex shader, ou elle ne coute
// que quelques instructions.
//
// 28 octets : glm::vec3 s'aligne sur 4 comme un float, donc la struct est
// exactement la somme de ses membres, sans remplissage. Cette taille EST le
// stride du binding d'instances, d'ou l'assertion en fin de fichier.
struct InstanceData {
    // Pied de l'instance, en espace monde (le terrain est genere en espace monde
    // avec une model matrix identite, cf. TerrainMesh.cpp).
    glm::vec3 position{0.0f};

    // Echelle UNIFORME. Non uniforme serait possible mais obligerait le vertex
    // shader a corriger la normale par l'inverse transpose, pour un gain visuel
    // nul sur de la vegetation.
    float scale = 1.0f;

    // Rotation autour de Y, en radians. Le sinus et le cosinus sont calcules
    // dans le vertex shader : les precalculer ici couterait 4 octets de plus par
    // instance pour economiser deux instructions par sommet, ce qui est le
    // mauvais cote du compromis quand le poste limitant est la bande passante.
    float yaw = 0.0f;

    // Aleas propres a l'instance, dans [0,1] : sert a varier la teinte et, pour
    // l'herbe, a dephaser l'ondulation afin que deux touffes voisines ne bougent
    // pas a l'unisson.
    float variation = 0.0f;

    // Attenuation de distance, dans [0,1] : 1 en pleine visibilite, tendant vers
    // 0 a la limite de rendu. Le vertex shader s'en sert pour RETRECIR l'instance
    // plutot que pour la rendre transparente - une instance qui rapetisse
    // disparait sans avoir besoin de melange alpha, donc sans imposer de tri en
    // profondeur ni de seconde passe. C'est ce qui rend le fondu de l'herbe
    // gratuit.
    float fade = 1.0f;
};

static_assert(sizeof(InstanceData) == 28,
              "InstanceData doit rester compact et sans remplissage : sa taille "
              "est le stride du binding d'instances decrit dans VertexFormat.hpp.");

} // namespace vercora
