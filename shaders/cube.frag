#version 450

// Meme bloc uniforme que le vertex shader : on n'y lit que viewPos, mais un bloc
// uniforme se declare a l'identique dans les deux etages (meme binding, meme
// layout). Cote C++, le descriptor set layout doit donc exposer ce binding au
// stage FRAGMENT en plus du stage VERTEX.
layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
    vec4 terrain;
    // PHASE 23 : cycle jour/nuit. xyz = direction VERS la lumiere, w = intensite
    // directe dans [0, 1]. Voir renderer/UniformBufferObject.hpp.
    vec4 sun;
    // rgb = couleur de la lumiere, a = MULTIPLICATEUR de l'ambiante ci-dessous.
    vec4 sunColor;
    // PHASE 27 : meteo visible et micro-relief. Voir
    // renderer/UniformBufferObject.hpp pour le sens de chaque composante.
    vec4 fog;     // rgb = couleur, a = extinction en 1/m (0 = aucun brouillard)
    vec4 precip;  // x = intensite, y = neige, z = temps, w = assombrissement sol mouille
    vec4 fall;    // xyz = vitesse de chute en monde (m/s)
    vec4 detail;  // micro-relief : x = force, y/z = fondu (m), w = motif (m)
} ubo;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;
// PHASE 15 : teinte de temoin de l'editeur. x = force (0 = aucune), yzw =
// couleur. Voir le commentaire du push constant dans cube.vert.
layout(location = 3) in vec4 fragTint;

layout(location = 0) out vec4 outColor;

// --- Lumiere directionnelle, fixe en dur pour la Phase 2 ---
// Direction de PROPAGATION de la lumiere (celle vers laquelle elle voyage) :
// venant d'en haut, legerement de la gauche et de l'avant. Une directionnelle
// n'a pas de position : tous les rayons sont paralleles, comme le soleil.
// PHASE 23 : la direction et la couleur de la lumiere ne sont plus des
// constantes. Elles viennent de l'UBO, donc de la position solaire calculee
// par world/Weather.hpp a partir de l'heure de jeu. Il n'y a qu'UN soleil
// dans le jeu, et c'est celui-la.

const float kAmbientStrength  = 0.15; // evite que les faces a l'ombre soient noires
const float kSpecularStrength = 0.35;
const float kShininess        = 32.0; // plus grand = tache speculaire plus petite

// --- PHASE 27 : brouillard de distance ------------------------------------
// Loi d'extinction exponentielle (Beer-Lambert) : la part de la couleur de la
// surface qui atteint l'oeil vaut exp(-sigma * d). sigma = ubo.fog.a vient de
// weatherVisualsFor(), qui le tire de la PRECIPITATION LOCALE calculee par la
// meteo ; il vaut exactement zero par temps sec, et alors cette fonction rend
// la couleur inchangee au bit pres. La couleur du voile est celle du ciel.
// MEME fonction dans terrain.frag, foliage.frag et cube.frag : un arbre et le
// sol sur lequel il pousse doivent disparaitre dans la brume ensemble.
vec3 applyFog(vec3 color, vec3 worldPos) {
    float sigma = ubo.fog.a;
    if (sigma <= 0.0) {
        return color;
    }
    float d = length(ubo.viewPos - worldPos);
    return mix(ubo.fog.rgb, color, exp(-sigma * d));
}

void main() {
    // Les varyings sont interpoles entre sommets : leur longueur n'est plus
    // garantie, d'ou le normalize().
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(ubo.sun.xyz);           // surface -> lumiere
    vec3 V = normalize(ubo.viewPos - fragWorldPos); // surface -> observateur

    // Blinn-Phong : on utilise le "half vector" entre L et V plutot que le
    // vecteur reflechi de Phong. Moins cher (pas de reflect()) et sans la
    // coupure abrupte de Phong quand l'angle depasse 90 degres.
    vec3 H = normalize(L + V);

    // Lambert : l'intensite depend du cosinus de l'angle normale/lumiere.
    // max(..., 0) : une face qui tourne le dos a la lumiere ne recoit rien
    // (sans ca, le produit scalaire negatif retirerait de la lumiere).
    float diffuse = max(dot(N, L), 0.0);

    // Pas de reflet speculaire sur une face non eclairee.
    float specular = diffuse > 0.0
        ? kSpecularStrength * pow(max(dot(N, H), 0.0), kShininess)
        : 0.0;

    // L'ambiant et le diffus teintent la couleur de la face ; le speculaire est
    // un reflet de la lumiere elle-meme, il s'ajoute donc sans etre teinte.
    // PHASE 23 : l'intensite directe et le multiplicateur d'ambiante viennent du
    // cycle jour/nuit. En plein jour, sun.w = 1 et sunColor.a = 1 : l'expression
    // ci-dessous est alors mot pour mot celle de la Phase 2.
    vec3  kLightColor = ubo.sunColor.rgb;
    float ambient     = kAmbientStrength * ubo.sunColor.a;
    diffuse  *= ubo.sun.w;
    specular *= ubo.sun.w;

    vec3 lit = fragColor * kLightColor * (ambient + diffuse)
             + kLightColor * specular;

    // --- Teinte de temoin (PHASE 15) ---------------------------------------
    // L'editeur doit distinguer d'un coup d'oeil trois etats du MEME modele :
    // l'objet pose, le fantome qui suit la visee, et l'objet selectionne.
    //
    // Une teinte plutot qu'une TRANSPARENCE, ce qui serait pourtant le rendu
    // habituel d'un fantome : le melange alpha impose de trier les objets par
    // profondeur a chaque image, ou d'accepter des artefacts. C'est le choix que
    // le projet a deja fait deux fois - la vegetation de la Phase 11 fond ses
    // arbres par RETRECISSEMENT et non par transparence, et l'herbe est faite de
    // quads opaques - pour la meme raison. Une teinte forte plus un ambiant
    // releve donne un objet parfaitement identifiable, opaque, sans tri, sans
    // second pipeline et sans etat de blending.
    //
    // Force nulle (tout le reste du jeu) : la branche n'est meme pas prise, le
    // rendu est bit a bit celui d'avant la Phase 15.
    if (fragTint.x > 0.0) {
        vec3 tinted = fragTint.yzw * kLightColor * (0.55 + 0.45 * diffuse)
                    + kLightColor * specular;
        lit = mix(lit, tinted, clamp(fragTint.x, 0.0, 1.0));
    }

    outColor = vec4(applyFog(lit, fragWorldPos), 1.0);
}
