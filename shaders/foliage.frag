#version 450

// PHASE 11 : eclairage de la vegetation instanciee.
//
// Volontairement proche de cube.frag (meme lumiere, meme Blinn-Phong) : la
// vegetation doit s'eclairer comme le reste du monde, sinon les arbres de decor
// se detachent des arbres abattables de Forest, qui passent, eux, par le
// materiau Object.

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

layout(location = 0) in vec3 fragTint;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;

// PHASE 18 : de quoi decouper le quad en brins (voir foliage.vert).
layout(location = 3) in vec2 fragUV;
layout(location = 4) in float fragBladeSeed;
layout(location = 5) in float fragCutout;

layout(location = 0) out vec4 outColor;

// --- PHASE 18 : decoupe alpha des brins d'herbe -----------------------------
//
// POURQUOI UN MASQUE CALCULE ET NON UNE TEXTURE. Une planche de brins en RGBA
// serait la solution d'atelier habituelle ; elle demanderait un second
// echantillonneur, donc un binding de descriptor de plus sur CE pipeline
// seulement, un asset a produire et a charger, et son filtrage mipmap ferait
// FONDRE les brins fins en une bouillie grise a quelques metres (le probleme
// classique du masque alpha mipmape). Le masque analytique n'a aucun de ces
// couts, tient en une quinzaine de lignes, et sa finesse ne depend d'aucune
// resolution.
//
// POURQUOI `discard` ET NON DU MELANGE ALPHA. Le melange imposerait de trier
// les touffes en profondeur a chaque image - des dizaines de milliers d'objets,
// depuis une camera qui bouge - et une seconde passe de rendu. Le test alpha,
// lui, laisse le tampon de profondeur faire son travail : l'ordre de dessin
// reste sans importance, exactement comme avant la Phase 18. C'est ce qui rend
// cette amelioration presque gratuite. Sa contrepartie est un bord FRANC, sans
// demi-teinte ; c'est ce que compense l'effacement progressif de la decoupe
// avec la distance calcule dans le vertex shader.
const float kBladesPerQuad = 4.0;

// Renvoie true si le fragment tombe DANS un brin.
bool insideBlade(vec2 uv, float seed, float strength) {
    const float height = clamp(uv.y, 0.0, 1.0);

    // Le quad est partage en kBladesPerQuad bandes ; chaque bande porte un brin.
    const float scaled = uv.x * kBladesPerQuad;
    const float index = floor(scaled);
    const float local = scaled - index;

    // Alea propre a CE brin de CETTE touffe. Hachage trigonometrique : sans
    // texture ni table, et suffisant pour ce qu'on lui demande (decorreler des
    // brins voisins, pas resister a une analyse statistique).
    const float random = fract(sin(index * 12.9898 + seed * 78.233 + 1.7) * 43758.5453);

    // 1. Tous les brins ne montent pas a la meme hauteur. C'est CE detail qui
    //    distingue une touffe d'un peigne : une touffe reelle a un profil
    //    irregulier.
    const float tip = mix(0.55, 1.0, random);
    if (height > tip) {
        return false;
    }

    // 2. Largeur du brin : pleine au pied, effilee a sa propre pointe. Le carre
    //    donne un profil de lame, pas de triangle.
    const float alongBlade = height / tip;
    // 0,40 et non 0,50 : les brins d'une meme bande ne se TOUCHENT pas au pied.
    // Avec 0,50 ils se rejoignaient a la base et la touffe redevenait, sur son
    // premier tiers, l'aplat opaque que la decoupe devait supprimer - c'est
    // exactement ce que montrait la premiere capture.
    const float halfWidth = 0.40 * (1.0 - 0.88 * alongBlade * alongBlade);

    // 3. Le brin s'ecarte de l'axe de sa bande en montant : les brins d'une
    //    meme touffe s'ouvrent en gerbe au lieu de rester paralleles.
    const float lean = (random - 0.5) * 0.9;
    const float centre = 0.5 + lean * alongBlade * alongBlade * 0.5;

    // 4. `strength` referme la decoupe avec la distance : a 0 la bande est
    //    pleine d'un bord a l'autre et le quad redevient celui de la Phase 11.
    const float width = mix(0.5, halfWidth, strength);
    return abs(local - centre) <= width;
}

// PHASE 23 : la direction et la couleur de la lumiere ne sont plus des
// constantes. Elles viennent de l'UBO, donc de la position solaire calculee
// par world/Weather.hpp a partir de l'heure de jeu. Il n'y a qu'UN soleil
// dans le jeu, et c'est celui-la.

const float kAmbientStrength  = 0.24; // plus haut que le cube : le feuillage a
                                      // l'ombre ne doit pas virer au noir
const float kSpecularStrength = 0.04; // vegetation : mate
const float kShininess        = 12.0;

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
    // Test alpha AVANT tout calcul d'eclairage : un fragment rejete ne doit
    // rien couter de plus qu'un branchement.
    if (fragCutout > 0.0 && !insideBlade(fragUV, fragBladeSeed, fragCutout)) {
        discard;
    }

    vec3 N = normalize(fragNormal);

    // Le rendu se fait sans culling de faces (voir Pipeline.cpp), ce qui est
    // indispensable aux quads croises de l'herbe : sans cela la moitie des
    // brins serait invisible selon l'angle. Consequence : une face vue de dos
    // presente une normale qui pointe a l'oppose de l'observateur et
    // s'assombrirait completement. On la RETOURNE vers l'observateur - c'est le
    // traitement standard d'une geometrie a double face, et il n'a aucun effet
    // sur les troncs et houppiers, dont on ne voit jamais l'interieur.
    if (dot(N, ubo.viewPos - fragWorldPos) < 0.0) {
        N = -N;
    }

    vec3 L = normalize(ubo.sun.xyz);
    vec3 V = normalize(ubo.viewPos - fragWorldPos);
    vec3 H = normalize(L + V);

    float diffuse = max(dot(N, L), 0.0);
    float specular = diffuse > 0.0
        ? kSpecularStrength * pow(max(dot(N, H), 0.0), kShininess)
        : 0.0;

    // PHASE 23 : l'intensite directe et le multiplicateur d'ambiante viennent du
    // cycle jour/nuit. En plein jour, sun.w = 1 et sunColor.a = 1 : l'expression
    // ci-dessous est alors mot pour mot celle de la Phase 2.
    vec3  kLightColor = ubo.sunColor.rgb;
    float ambient     = kAmbientStrength * ubo.sunColor.a;
    diffuse  *= ubo.sun.w;
    specular *= ubo.sun.w;

    vec3 lit = fragTint * kLightColor * (ambient + diffuse)
             + kLightColor * specular;

    outColor = vec4(applyFog(lit, fragWorldPos), 1.0);
}
