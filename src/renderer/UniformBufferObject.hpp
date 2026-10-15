#pragma once

#include <glm/glm.hpp>

namespace vercora {

// Donnees uniformes passees aux shaders (binding = 0).
// alignas(16) : les mat4 doivent etre alignees sur 16 octets selon les regles
// std140/std430 de GLSL, sinon le layout C++ ne correspond pas au shader.
// Idem pour un vec3, qui s'aligne sur 16 (et non 12) en std140.
//
// Offsets resultants, identiques cote C++ et cote GLSL :
//   view 0 | proj 64 | viewPos 128 | terrain 144 | sun 160 | sunColor 176
//
// Ce bloc ne contient QUE ce qui est constant sur toute la frame. Depuis la
// Phase 5, la model matrix n'en fait plus partie : une frame dessine plusieurs
// objets (terrain, arbres, souches, buches) et chacun pousse la sienne par push
// constant au moment de son draw (voir DrawItem.hpp et Pipeline.cpp).
struct UniformBufferObject {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;

    // Position de la camera en espace monde. Necessaire au fragment shader pour
    // le terme speculaire (vecteur surface -> observateur).
    alignas(16) glm::vec3 viewPos;

    // Parametres du terrain : x = altitude minimale, y = altitude maximale,
    // zw reserves. Le fragment shader du terrain s'en sert pour normaliser sa
    // coloration par altitude.
    //
    // Un vec4 (et non un vec2) volontairement : son alignement std140 est 16,
    // exactement celui d'alignas(16) cote C++, donc aucune ambiguite de layout.
    // Les shaders du cube ne declarent pas ce membre et ignorent donc les 16
    // derniers octets du buffer : legal, la plage d'un descriptor peut depasser
    // la taille du bloc declare par un shader (l'inverse ne l'est pas).
    alignas(16) glm::vec4 terrain{0.0f, 1.0f, 0.0f, 0.0f};

    // PHASE 23 - CYCLE JOUR/NUIT.
    //
    // Jusqu'ici la lumiere directionnelle etait la constante
    // vec3(-0.4, -1.0, -0.6) ecrite en dur dans les TROIS shaders de fragment,
    // depuis la Phase 2. Le jeu simulait pourtant une horloge acceleree douze
    // fois depuis la Phase 16 : une session d'une heure traversait une tombee de
    // nuit dont l'image ne montrait rien. C'est l'incoherence notee dans
    // NOTES-PHASE16.md, et ces deux vec4 la corrigent.
    //
    // Les valeurs par defaut ci-dessous REPRODUISENT EXACTEMENT l'ancien
    // eclairage - direction normalisee de (0.4, 1.0, 0.6), pleine intensite,
    // meme teinte, ambiante inchangee. Un rendu qui n'appellerait pas
    // Renderer::setSun() est donc pixel pour pixel celui de la Phase 22, ce qui
    // rend la non-regression verifiable.
    //
    //   sun.xyz      direction du fragment VERS la lumiere, unitaire, en monde
    //   sun.w        intensite de la lumiere directe, dans [0, 1]
    //   sunColor.rgb couleur de la lumiere
    //   sunColor.a   MULTIPLICATEUR de l'ambiante propre a chaque shader (dans
    //                [0, 4] depuis la Phase 27 : > 1 sous un ciel couvert). Un
    //                multiplicateur, parce que les trois shaders ont des
    //                ambiantes differentes et justifiees (0,15 / 0,18 / 0,24) :
    //                les remplacer par une valeur unique aurait ete une
    //                regression de rendu deguisee en correction de cycle.
    alignas(16) glm::vec4 sun{0.3244f, 0.8111f, 0.4867f, 1.0f};
    alignas(16) glm::vec4 sunColor{1.0f, 0.97f, 0.90f, 1.0f};

    // PHASE 27 - METEO VISIBLE. Offsets 192 / 208 / 224 / 240.
    //
    // Toutes ces valeurs sont produites par weatherVisualsFor() (world/
    // Weather.hpp), qui ne fait que TRADUIRE des grandeurs deja calculees par
    // WeatherSystem. Les defauts ci-dessous sont NEUTRES : sans appel a
    // Renderer::setWeatherVisuals(), aucun brouillard, aucune pluie, aucun
    // assombrissement - l'image est exactement celle de la Phase 26.
    //
    //   fog.rgb     couleur du brouillard (= couleur du ciel)
    //   fog.a       coefficient d'extinction, en 1/m (0 = aucun brouillard)
    //   precip.x    intensite de precipitation a l'ecran, dans [0, 1]
    //   precip.y    1 = neige, 0 = pluie
    //   precip.z    temps ecoule en secondes (animation de la chute)
    //   precip.w    assombrissement du sol par la mouillure, dans [0, 1]
    //   fall.xyz    vitesse de chute apparente en monde, m/s (vent local compris)
    //   fall.w      reserve
    //   detail      micro-relief du sol (PHASE 27, chantier 3) :
    //               x = force (0 = desactive), y = debut du fondu, z = fin du
    //               fondu (metres depuis la camera), w = echelle du motif (m)
    alignas(16) glm::vec4 fog{0.52f, 0.66f, 0.82f, 0.0f};
    alignas(16) glm::vec4 precip{0.0f, 0.0f, 0.0f, 0.0f};
    alignas(16) glm::vec4 fall{0.0f, -6.5f, 0.0f, 0.0f};
    alignas(16) glm::vec4 detail{0.0f, 0.0f, 1.0f, 1.0f};

    // PHASE 32. Offset 256. Grille de la heightmap, pour que terrain.vert
    // recalcule la position horizontale et l'UV d'un sommet a partir de son
    // indice (voir renderer/GpuVertex.hpp, TerrainVertex) :
    //   x = origine (Heightmap::worldCoord(0)), y = pas en metres,
    //   z = nombre de CELLULES par cote (resolution - 1), w = reserve.
    // Constant sur toute la partie ; seuls les shaders du terrain le declarent.
    alignas(16) glm::vec4 terrainGrid{0.0f, 1.0f, 1.0f, 0.0f};
};

} // namespace vercora
