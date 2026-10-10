#version 450

// Uniform Buffer Object : ce qui est constant sur toute la frame (matrices View
// et Projection, position camera).
layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
} ubo;

// Push constant : la transformation propre a l'objet en cours de dessin. Elle
// change a chaque draw d'une meme frame, contrairement au contenu de l'UBO.
//
// `params` a une signification PROPRE A CHAQUE MATERIAU - c'est le meme bloc de
// push constants pour tous, mais chaque paire de shaders l'interprete a sa
// facon (bande de geomorphing pour le terrain en Phase 10, ondulation pour la
// vegetation en Phase 11). Pour le materiau Object, il etait inutilise jusqu'a
// la Phase 15, qui s'en sert pour la TEINTE DE TEMOIN de l'editeur :
//   params.x   = force de la teinte, 0 = aucune (le cas de tout le jeu normal)
//   params.yzw = couleur de la teinte
// Un zero laisse donc le rendu strictement inchange, ce qui etait la condition
// pour ne pas toucher au pipeline ni ajouter un materiau.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 params;
} pc;

// Attributs de sommet (voir Vertex.hpp cote C++).
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
// UV lu par le loader .obj (Phase 3) mais pas encore consomme : declare ici pour
// que l'attribut fourni par le vertex buffer corresponde a une entree du shader.
// Rien ne l'utilise tant qu'il n'y a pas de texturing.
layout(location = 3) in vec2 inUV;

// Vers le fragment shader. Tout est exprime en espace MONDE : c'est l'espace le
// plus simple pour l'eclairage ici, puisque la lumiere est definie en monde.
layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec4 fragTint;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);

    gl_Position  = ubo.proj * ubo.view * worldPos;
    fragColor    = inColor;
    fragWorldPos = worldPos.xyz;
    fragTint     = pc.params;

    // Normal matrix : la regle generale est transpose(inverse(M)). Ici la model
    // matrix est une ROTATION eventuellement suivie d'une echelle UNIFORME
    // (contrat documente sur DrawItem). Une rotation est orthogonale, donc
    // transpose(inverse(M)) se simplifie en M ; une echelle uniforme ne fait que
    // changer la LONGUEUR de la normale, ce que le normalize() du fragment
    // shader absorbe. mat3(model) suffit donc, et evite une inversion de matrice
    // par sommet.
    // A revoir des qu'un objet aura une mise a l'echelle NON UNIFORME : il
    // faudra alors calculer la vraie normal matrix cote CPU et la pousser en
    // plus (le push constant a encore 64 octets de marge sur les 128 garantis).
    fragNormal = mat3(pc.model) * inNormal;
}
