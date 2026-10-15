#version 450

// Meme bloc uniforme que les shaders du cube, avec des membres supplementaires
// en fin de bloc. Cote C++ il n'y a qu'une seule struct UniformBufferObject :
// les shaders declarent chacun la version tronquee du bloc qui leur suffit, ce
// qui est legal (la plage du descriptor peut etre plus grande que le bloc
// declare, jamais l'inverse).
layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
    // x = altitude minimale du terrain, y = altitude maximale, zw = reserve.
    // Passe par l'UBO plutot que code en dur : la coloration reste correcte
    // quels que soient les parametres de generation (et demain, les vraies
    // altitudes LiDAR, qui seront en metres NGF et pas du tout dans [0,16]).
    vec4 terrain;
    vec4 sun;
    vec4 sunColor;
    vec4 fog;
    vec4 precip;
    vec4 fall;
    vec4 detail;
    // PHASE 32 : la grille de la heightmap. x = origine (Heightmap::
    // worldCoord(0)), y = pas en metres, z = nombre de CELLULES par cote.
    vec4 terrainGrid;
} ubo;

// Meme push constant que cube.vert : les pipelines partagent la meme
// signature de layout, ce qui permet au Renderer de pousser la model matrix de
// la meme facon quel que soit le materiau.
// params.x = debut de la bande de geomorphing, params.y = fin, en metres depuis
// la camera. Bande vide (y <= x) = pas de morphing : c'est le cas du terrain
// dessine d'un seul bloc (option --no-lod).
// grid (PHASE 32) : place du maillage dans la grille de la heightmap -
// x, y = indices (ix, iz) du premier sommet, z = pas en echantillons entre deux
// sommets voisins, w = sommets par rangee. Voir renderer/DrawItem.hpp.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 params;
    ivec4 grid;
} pc;

// PHASE 32 : 16 octets par sommet au lieu de 68 (renderer/GpuVertex.hpp,
// TerrainVertex). Le buffer ne porte plus que ce qui ne se DEDUIT pas de
// l'indice du sommet : les deux altitudes et les deux normales (x et z en
// snorm16, y reconstruit). Position horizontale, cible de morphing horizontale
// et UV sont recalcules ci-dessous ; la couleur de sommet valait toujours blanc.
layout(location = 0) in float inHeight;
layout(location = 1) in float inMorphHeight;
layout(location = 2) in vec2 inNormalXZ;
layout(location = 3) in vec2 inMorphNormalXZ;

layout(location = 0) out vec3 fragTint;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec2 fragUV;

// Une normale de champ de hauteur pointe toujours vers le haut : y se deduit de
// x et z. Meme formule que unpackTerrainNormal (renderer/GpuVertex.hpp).
vec3 decodeNormal(vec2 xz) {
    return vec3(xz.x, sqrt(max(0.0, 1.0 - xz.x * xz.x - xz.y * xz.y)), xz.y);
}

void main() {
    // --- Place du sommet dans la grille -----------------------------------
    // gl_VertexIndex est l'indice LOCAL du sommet dans son maillage : le
    // Renderer lie chaque maillage a son decalage dans le bloc partage plutot
    // que de passer un vertexOffset au draw (voir Renderer::recordCommandBuffer).
    // Les indices de grille sont ECRETES sur le bord du terrain exactement comme
    // le fait TerrainNodeMesh.cpp, et la cible de morphing est le sommet aux
    // indices locaux arrondis au pair inferieur (grille du parent).
    const int perRow = pc.grid.w;
    const int i = gl_VertexIndex % perRow;
    const int j = gl_VertexIndex / perRow;
    const int cells = int(ubo.terrainGrid.z);
    const int gx = min(pc.grid.x + i * pc.grid.z, cells);
    const int gz = min(pc.grid.y + j * pc.grid.z, cells);
    const int mx = min(pc.grid.x + (i & ~1) * pc.grid.z, cells);
    const int mz = min(pc.grid.y + (j & ~1) * pc.grid.z, cells);

    // `precise` : interdit au compilateur de fusionner la multiplication et
    // l'addition en un FMA. Sans lui le resultat pourrait differer d'un ULP de
    // Heightmap::worldCoord ; avec lui il est identique AU BIT PRES (addition et
    // multiplication flottantes sont correctement arrondies en Vulkan).
    precise float x = ubo.terrainGrid.x + float(gx) * ubo.terrainGrid.y;
    precise float z = ubo.terrainGrid.x + float(gz) * ubo.terrainGrid.y;
    precise float morphX = ubo.terrainGrid.x + float(mx) * ubo.terrainGrid.y;
    precise float morphZ = ubo.terrainGrid.x + float(mz) * ubo.terrainGrid.y;

    const vec3 inPosition = vec3(x, inHeight, z);
    const vec3 inMorphPos = vec3(morphX, inMorphHeight, morphZ);

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
    vec3 normal   = mix(decodeNormal(inNormalXZ), decodeNormal(inMorphNormalXZ), morph);

    vec4 worldPos = pc.model * vec4(position, 1.0);

    gl_Position  = ubo.proj * ubo.view * worldPos;
    fragWorldPos = worldPos.xyz;
    // Teinte neutre, reservee a une future classification LiDAR par sommet :
    // c'est la valeur que portaient tous les sommets de terrain jusqu'ici.
    fragTint     = vec3(1.0);

    // L'UV n'est PAS morphe. Il est une fonction affine de la position dans la
    // grille du terrain entier, donc la cible de morphing d'un sommet - qui est
    // un autre noeud de cette meme grille - a naturellement l'UV de sa propre
    // position. Interpoler entre les deux decalerait la photo par rapport au
    // relief pendant la transition de niveau, exactement ce qu'on cherche a
    // eviter. Le leger glissement de la texture qu'on pourrait attendre pendant
    // un morph est en realite le comportement CORRECT : la surface se deforme,
    // la photo reste accrochee a ses coordonnees geographiques. Meme expression
    // que TerrainNodeMesh.cpp (indice de grille / nombre de cellules).
    fragUV       = vec2(float(gx), float(gz)) / float(cells);

    // Le terrain est genere DIRECTEMENT en espace monde et son DrawItem porte la
    // matrice IDENTITE : mat3(model) est donc exact ici sans discussion. Il
    // redeviendrait faux si on appliquait au terrain une echelle verticale
    // differente de l'horizontale - dans ce cas il faudrait transposer l'inverse
    // cote CPU. Voir TerrainMesh.cpp.
    fragNormal = mat3(pc.model) * normal;
}
