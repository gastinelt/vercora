#version 450

// ===========================================================================
// PHASE 27 - Pluie et neige a l'ecran
// ===========================================================================
// L'objectif est la LISIBILITE, pas le photorealisme : le joueur doit voir
// qu'il pleut ou qu'il neige, et a quel point. Un seul triangle plein ecran,
// trois couches de traits procéduraux, aucune texture, aucun buffer de
// particules.
//
// TOUT CE QUI VARIE vient de la meteo, via weatherVisualsFor() :
//   precip.x  intensite = precipitation LOCALE / precipitationMaxMmPerHour.
//             Elle regle la DENSITE des traits (proportion de cellules qui
//             portent une goutte) et leur opacite.
//   precip.y  neige ou pluie = WeatherSample::snowing, donc la temperature
//             locale comparee a snowThresholdC.
//   fall.xyz  vitesse de chute en monde : le VENT LOCAL (sondage amont, cols,
//             abris) plus une vitesse terminale de goutte ou de flocon.
//             Projetee dans le repere de la camera, elle donne l'INCLINAISON
//             des traits : la pluie se couche dans le sens ou le vent souffle
//             reellement a cet endroit, et se redresse a l'abri.
// La luminosite des traits suit la lumiere du cycle jour/nuit (sun, sunColor),
// pour qu'une pluie nocturne ne brille pas plus que le paysage.
//
// Ce que ce n'est PAS : des particules en monde. Les traits sont colles a
// l'ecran ; ils ne se cachent pas derriere un tronc et ne se rapprochent pas
// quand on avance. C'est le compromis assume de "particules basiques ou effet
// d'ecran", au cout d'une seule passe.
// ===========================================================================

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
    vec4 terrain;
    vec4 sun;
    vec4 sunColor;
    vec4 fog;
    vec4 precip;
    vec4 fall;
    vec4 detail;
} ubo;

layout(location = 0) in vec2 fragScreenUV;
layout(location = 0) out vec4 outColor;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// Une couche de precipitation. cells = nombre de cellules sur la HAUTEUR de
// l'ecran (plus il y en a, plus la couche parait lointaine) ; screenSpeed =
// hauteurs d'ecran parcourues par seconde.
float layerAlpha(vec2 q, float cells, float screenSpeed, float layerSeed,
                 float density, bool snow, float t) {
    vec2 g = q * cells;
    // Chaque colonne a sa propre phase : sans ca, toutes les gouttes d'une
    // rangee tomberaient alignees comme un rideau.
    float column = floor(g.x);
    g.y -= t * screenSpeed * cells + hash21(vec2(column, layerSeed)) * 17.0;

    vec2 cell = floor(g);
    vec2 f = fract(g);
    float h = hash21(cell + layerSeed * 31.7);
    if (h > density) {
        return 0.0;
    }
    float x0 = 0.2 + 0.6 * fract(h * 7.13);

    if (snow) {
        // Flocon : un disque flou qui oscille lateralement.
        float wobble = 0.15 * sin(t * 1.7 + h * 40.0);
        vec2 d = f - vec2(x0 + wobble, 0.5);
        return 1.0 - smoothstep(0.06, 0.16, length(d));
    }
    // Goutte : un trait fin et long dans le sens de la chute.
    float across = 1.0 - smoothstep(0.015, 0.045, abs(f.x - x0));
    float along = smoothstep(0.0, 0.15, f.y) * (1.0 - smoothstep(0.55, 0.75, f.y));
    return across * along;
}

void main() {
    float intensity = clamp(ubo.precip.x, 0.0, 1.0);
    if (intensity <= 0.0) {
        discard; // ne devrait pas arriver : la passe est sautee cote CPU
    }
    bool snow = ubo.precip.y > 0.5;
    float t = ubo.precip.z;

    // --- Direction de chute a l'ecran ---------------------------------------
    // Vitesse monde -> repere camera. En Vulkan l'axe Y de l'ecran descend,
    // d'ou le signe. Quand la chute est presque dans l'axe de la vue (regard a
    // la verticale), la direction degeneree retombe sur "vers le bas".
    vec3 v = mat3(ubo.view) * ubo.fall.xyz;
    vec2 screenDir = vec2(v.x, -v.y);
    screenDir = length(screenDir) > 1e-3 ? normalize(screenDir) : vec2(0.0, 1.0);

    // Coordonnees ecran isotropes (le pixel est carre), puis rotation qui
    // amene la direction de chute sur +Y.
    float aspect = abs(ubo.proj[1][1] / ubo.proj[0][0]);
    vec2 p = vec2(fragScreenUV.x * aspect, fragScreenUV.y);
    vec2 q = vec2(screenDir.y * p.x - screenDir.x * p.y,
                  screenDir.x * p.x + screenDir.y * p.y);

    // --- Trois couches -------------------------------------------------------
    // La densite suit l'intensite : une bruine ne montre que quelques traits,
    // une averse en montre beaucoup. Le plancher de 0,15 garde une faible
    // precipitation VISIBLE - sinon 0,3 mm/h serait mesure et invisible, ce qui
    // est exactement le defaut que cette phase corrige.
    float density = mix(0.15, 0.85, intensity);
    float alpha;
    if (snow) {
        alpha = layerAlpha(q, 9.0,  0.16, 1.0, density, true, t)
              + layerAlpha(q, 16.0, 0.11, 2.0, density, true, t) * 0.7
              + layerAlpha(q, 28.0, 0.07, 3.0, density, true, t) * 0.45;
    } else {
        alpha = layerAlpha(q, 7.0,  1.6, 1.0, density, false, t)
              + layerAlpha(q, 13.0, 1.2, 2.0, density, false, t) * 0.7
              + layerAlpha(q, 22.0, 0.9, 3.0, density, false, t) * 0.45;
    }

    // --- Teinte ---------------------------------------------------------------
    // Eclairee comme le paysage : ambiante et directe du cycle jour/nuit.
    float light = clamp(0.45 * ubo.sunColor.a + 0.55 * ubo.sun.w, 0.0, 1.0);
    vec3 base = snow ? vec3(0.97, 0.98, 1.0) : vec3(0.78, 0.82, 0.88);
    vec3 color = base * mix(ubo.sunColor.rgb, vec3(1.0), 0.5) * (0.25 + 0.75 * light);

    float opacity = snow ? mix(0.55, 0.9, intensity) : mix(0.22, 0.45, intensity);
    outColor = vec4(color, clamp(alpha, 0.0, 1.0) * opacity);
}
