#version 450

// Meme bloc uniforme que les shaders du cube, avec un membre supplementaire en
// fin de bloc (terrain). Cote C++ il n'y a qu'une seule struct
// UniformBufferObject : les shaders du cube declarent simplement une version
// tronquee du bloc, ce qui est legal (la plage du descriptor peut etre plus
// grande que le bloc declare, jamais l'inverse).
layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
    // x = altitude minimale du terrain, y = altitude maximale, zw = reserve.
    // Passe par l'UBO plutot que code en dur : la coloration reste correcte
    // quels que soient les parametres de generation (et demain, les vraies
    // altitudes LiDAR, qui seront en metres NGF et pas du tout dans [0,16]).
    vec4 terrain;
} ubo;

// Meme push constant que cube.vert : les deux pipelines partagent la meme
// signature de layout, ce qui permet au Renderer de pousser la model matrix de
// la meme facon quel que soit le materiau.
// params.x = debut de la bande de geomorphing, params.y = fin, en metres depuis
// la camera. Bande vide (y <= x) = pas de morphing : c'est le cas du terrain
// dessine d'un seul bloc (option --no-lod).
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 params;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
// UV du sommet sur TOUT le terrain (0,0 au coin nord-ouest, 1,1 au sud-est),
// et non sur la tuile de quadtree : c'est ce qui rend la coordonnee continue
// d'une tuile a l'autre et independante du niveau de detail. Voir
// TerrainNodeMesh.cpp. PHASE 11 : desormais consomme, pour l'orthophoto.
layout(location = 3) in vec2 inUV;

// PHASE 10 - cible de geomorphing : position et normale de ce sommet dans la
// grille du niveau de detail SUPERIEUR (voir renderer/TerrainNodeMesh.hpp).
layout(location = 4) in vec3 inMorphPos;
layout(location = 5) in vec3 inMorphNormal;

layout(location = 0) out vec3 fragTint;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec2 fragUV;

void main() {
    // --- Geomorphing (CDLOD) ------------------------------------------------
    // La distance est mesuree sur la position NON MORPHEE : c'est ce qui garantit
    // qu'un sommet partage par deux tuiles voisines obtient le meme facteur des
    // deux cotes, donc que le bord reste soude. Prendre la position morphee
    // rendrait le calcul recursif et instable.
    //
    // Le terrain etant genere directement en espace monde avec une model matrix
    // identite, inPosition EST deja la position monde ; le pc.model ci-dessous
    // reste applique pour ne rien casser si un jour une tuile etait transformee.
    float morphSpan = pc.params.y - pc.params.x;
    float morph = 0.0;
    if (morphSpan > 0.0) {
        float distance = length(ubo.viewPos - inPosition);
        morph = clamp((distance - pc.params.x) / morphSpan, 0.0, 1.0);
    }

    vec3 position = mix(inPosition, inMorphPos, morph);
    vec3 normal   = mix(inNormal, inMorphNormal, morph);

    vec4 worldPos = pc.model * vec4(position, 1.0);

    gl_Position  = ubo.proj * ubo.view * worldPos;
    fragWorldPos = worldPos.xyz;
    fragTint     = inColor;

    // L'UV n'est PAS morphe. Il est deja une fonction affine de la position dans
    // la grille du terrain entier, donc la cible de morphing d'un sommet - qui
    // est un autre noeud de cette meme grille - a naturellement l'UV de sa
    // propre position. Interpoler entre les deux decalerait la photo par rapport
    // au relief pendant la transition de niveau, exactement ce qu'on cherche a
    // eviter. Le leger glissement de la texture qu'on pourrait attendre pendant
    // un morph est en realite le comportement CORRECT : la surface se deforme,
    // la photo reste accrochee a ses coordonnees geographiques.
    fragUV       = inUV;

    // Le terrain est genere DIRECTEMENT en espace monde et son DrawItem porte la
    // matrice IDENTITE : mat3(model) est donc exact ici sans discussion. Il
    // redeviendrait faux si on appliquait au terrain une echelle verticale
    // differente de l'horizontale - dans ce cas il faudrait transposer l'inverse
    // cote CPU. Voir TerrainMesh.cpp.
    fragNormal = mat3(pc.model) * normal;
}
