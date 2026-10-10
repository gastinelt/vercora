#version 450

// PHASE 27 - Passe plein ecran de precipitation.
//
// Aucun buffer de sommets : un seul triangle qui deborde de l'ecran, fabrique a
// partir de gl_VertexIndex (0, 1, 2). C'est la facon la moins chere de couvrir
// tous les pixels - pas de diagonale partagee entre deux triangles, donc pas de
// pixels ombres deux fois le long de cette diagonale.

layout(location = 0) out vec2 fragScreenUV; // 0,0 en haut a gauche, 1,1 en bas a droite

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    fragScreenUV = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
