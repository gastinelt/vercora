#version 450

// PHASE 11 : vegetation INSTANCIEE (arbres de decor et touffes d'herbe).
//
// Une geometrie, un appel de dessin, N instances. Chaque instance lit sa
// transformation dans le binding 1, cadence par instance et non par sommet (voir
// renderer/InstanceData.hpp et renderer/VertexFormat.hpp).

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
    vec4 terrain;
} ubo;

// x = amplitude d'ondulation en metres (0 pour un arbre : un tronc ne plie pas),
// y = temps en secondes, z = distance de rendu, w = reserve.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 params;
} pc;

// --- Attributs de SOMMET (binding 0, cadence par sommet) --------------------
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUV;

// --- Attributs d'INSTANCE (binding 1, cadence par instance) -----------------
layout(location = 6) in vec3 inInstancePosition;
layout(location = 7) in vec2 inScaleYaw;      // x = echelle uniforme, y = lacet
layout(location = 8) in vec2 inVariationFade; // x = alea [0,1], y = attenuation

layout(location = 0) out vec3 fragTint;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPos;

// PHASE 18 : ce qu'il faut au fragment shader pour DECOUPER le quad en brins.
//
//  fragUV     : les memes UV que le maillage (x en travers du quad, y du pied a
//               la pointe). Le masque de brins est dessine dans cet espace.
//  fragBladeSeed : l'alea de l'instance, transmis tel quel. Deux touffes
//               voisines n'ont donc pas leurs brins au meme endroit ; sans lui,
//               le meme decoupage se repeterait a l'identique sur tout le pre.
//  fragCutout : FORCE de la decoupe, entre 0 et 1, calculee ICI parce que la
//               distance a la camera y est deja connue. 1 = brins decoupes,
//               0 = quad plein (le comportement d'avant la Phase 18).
//
// POURQUOI LA DECOUPE S'EFFACE AVEC LA DISTANCE. Un brin fait quelques pixels a
// 15 m et moins d'un pixel a 30 : le decouper produit alors un scintillement -
// le pixel tombe tantot dans un brin, tantot entre deux, et l'image bouge des
// que la camera bouge. Sans MSAA (rasterizationSamples = 1, voir Pipeline.cpp)
// il n'y a aucun filtrage pour l'absorber. On ELARGIT donc les brins jusqu'a
// refermer le quad quand la touffe s'eloigne : de pres on voit des brins, de
// loin une silhouette pleine, et entre les deux la transition est continue.
layout(location = 3) out vec2 fragUV;
layout(location = 4) out float fragBladeSeed;
layout(location = 5) out float fragCutout;

// Distances, en metres, entre lesquelles la decoupe des brins se referme.
// Reglees sur la portee de l'herbe (34 m, voir world/Vegetation.hpp) : la
// decoupe est entiere sur le premier tiers, la ou on distingue reellement un
// brin, et le quad est de nouveau plein bien avant que la touffe ne disparaisse.
const float kCutoutFullRange = 7.0;
const float kCutoutNoneRange = 17.0;

void main() {
    const float scale = inScaleYaw.x;
    const float yaw = inScaleYaw.y;
    const float variation = inVariationFade.x;
    const float fade = inVariationFade.y;

    // ATTENUATION PAR RETRECISSEMENT, et non par transparence. Une instance qui
    // rapetisse jusqu'a zero disparait sans melange alpha, donc sans imposer de
    // tri en profondeur ni de seconde passe de rendu. C'est ce qui rend le fondu
    // de l'herbe gratuit, alors qu'une disparition en transparence couterait bien
    // plus cher que l'herbe elle-meme. La contrepartie est visible de pres si la
    // bande de fondu est trop courte : les touffes ont l'air de pousser quand on
    // avance. C'est pourquoi la bande est large (voir world/Vegetation.hpp).
    const float effectiveScale = scale * fade;

    // Rotation autour de Y reconstituee depuis le lacet : 6 flottants par
    // instance suffisent, la matrice 4x4 n'a pas a voyager (voir InstanceData.hpp).
    const float s = sin(yaw);
    const float c = cos(yaw);
    const mat3 rotation = mat3( c, 0.0, -s,
                              0.0, 1.0, 0.0,
                                s, 0.0,  c);

    vec3 local = inPosition * effectiveScale;

    // --- Ondulation ---------------------------------------------------------
    // Ne s'applique qu'a l'herbe (amplitude nulle pour un arbre). Le
    // deplacement croit avec inUV.y, qui vaut 0 au pied du brin et 1 a sa
    // pointe : le pied reste donc PLANTE dans le sol, ce qu'un deplacement
    // uniforme de l'instance ne donnerait pas. La variation dephase chaque
    // touffe, sans quoi tout le pre ondulerait a l'unisson comme un seul objet.
    if (pc.params.x > 0.0) {
        const float phase = pc.params.y * 1.7 + variation * 6.2831853;
        const float bend = pc.params.x * inUV.y * inUV.y * sin(phase);
        local.x += bend;
        local.z += bend * 0.6;
    }

    // --- PHASE 29 : mode ROCHER (params.w < 0) --------------------------------
    // Un bloc d'eboulis s'incline avec la pente, ce que la rotation en lacet
    // seule ne sait pas faire. InstanceData etant plein (28 o), le gradient du
    // sol est ENCODE dans la partie entiere de `variation` (64 x 64 niveaux dans
    // [-1, 1], voir world/Rocks.hpp packRockVariation) et la teinte dans sa
    // partie fractionnaire. La rotation amene l'axe Y du mesh sur la normale
    // n = normalize(-gx, 1, -gz) (formule de Rodrigues). Arbres, herbe et
    // plantes a fibres poussent w >= 0 : leur chemin est strictement inchange.
    float tintSeed = variation;
    mat3 tilt = mat3(1.0);
    if (pc.params.w < 0.0) {
        const float packedIndex = floor(variation);
        tintSeed = variation - packedIndex;
        const float qx = floor(packedIndex / 64.0);
        const float qz = packedIndex - qx * 64.0;
        const vec2 g = vec2(qx, qz) / 63.0 * 2.0 - 1.0;
        const vec3 n = normalize(vec3(-g.x, 1.0, -g.y));
        const vec3 axis = vec3(n.z, 0.0, -n.x); // cross((0,1,0), n)
        const float sine = length(axis);
        if (sine > 1e-4) {
            const vec3 k = axis / sine;
            const float cosine = n.y;
            const mat3 K = mat3(0.0, k.z, -k.y,
                                -k.z, 0.0, k.x,
                                k.y, -k.x, 0.0);
            tilt = mat3(cosine) + sine * K + (1.0 - cosine) * outerProduct(k, k);
        }
    }

    const vec3 worldPos = inInstancePosition + tilt * (rotation * local);

    gl_Position  = ubo.proj * ubo.view * vec4(worldPos, 1.0);
    fragWorldPos = worldPos;

    // L'echelle etant UNIFORME, la rotation seule suffit a transporter la
    // normale : pas d'inverse transpose a calculer (voir DrawItem.hpp).
    fragNormal = tilt * (rotation * inNormal);

    // Variation de teinte par instance : sans elle, dix mille arbres du meme
    // vert forment un aplat qui trahit immediatement la copie. Le facteur reste
    // resserre autour de 1 pour que ce soit une nuance, pas un arc-en-ciel.
    fragTint = inColor * (0.82 + 0.30 * tintSeed);

    // --- Decoupe en brins (PHASE 18) ---------------------------------------
    // params.w porte la force demandee par l'appelant : 0 pour un arbre (un
    // houppier n'a pas a etre troue), > 0 pour l'herbe. C'est le meme bloc de
    // push constants pour tous les materiaux, et w y etait justement reserve
    // (voir renderer/DrawItem.hpp).
    fragUV = inUV;
    fragBladeSeed = variation;
    if (pc.params.w > 0.0) {
        const float distance = length(ubo.viewPos - worldPos);
        fragCutout = pc.params.w * (1.0 - smoothstep(kCutoutFullRange, kCutoutNoneRange,
                                                     distance));
    } else {
        fragCutout = 0.0;
    }
}
