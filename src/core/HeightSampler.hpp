#pragma once

namespace vercora {

// Ce que la camera a besoin de savoir d'un terrain pour "marcher" dessus :
// une hauteur en fonction d'une position horizontale, rien de plus.
//
// L'interface existe pour que core/Camera ne depende PAS de assets/Heightmap
// (ni, demain, du futur terrain LiDAR streame par tuiles) : la camera consomme
// ce contrat, l'implementation vit du cote des donnees.
class HeightSampler {
public:
    virtual ~HeightSampler() = default;

    // Altitude (Y monde) de la surface a la position horizontale (worldX, worldZ).
    // Contrat : la fonction est TOTALE - hors du domaine couvert par les donnees,
    // l'implementation doit renvoyer la hauteur du point valide le plus proche
    // plutot que d'echouer. Une camera ne doit jamais tomber dans le vide parce
    // qu'elle a depasse le bord d'une tuile.
    virtual float heightAt(float worldX, float worldZ) const = 0;
};

} // namespace vercora
