#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
    // x = altitude min, y = altitude max, z = dosage orthophoto,
    // w = force de l'indice adret/ubac (PHASE 25, voir plus bas)
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

// PHASE 11 : orthophoto BD ORTHO drapee sur le relief. Toujours reliee a une
// image valide - un texel blanc quand aucune photo n'est chargee - donc
// echantillonnable sans condition. C'est ubo.terrain.z qui decide si le resultat
// compte, et il vaut zero dans ce cas.
layout(binding = 1) uniform sampler2D orthoTexture;

layout(location = 0) in vec3 fragTint;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

// --- Lumiere directionnelle, identique a celle de la Phase 2 ---------------
// PHASE 23 : la direction et la couleur de la lumiere ne sont plus des
// constantes. Elles viennent de l'UBO, donc de la position solaire calculee
// par world/Weather.hpp a partir de l'heure de jeu. Il n'y a qu'UN soleil
// dans le jeu, et c'est celui-la.

const float kAmbientStrength  = 0.18;
const float kSpecularStrength = 0.06; // roche/herbe : quasi mat, contrairement au cube
const float kShininess        = 16.0;

// --- Etagement par altitude ------------------------------------------------
// Trois teintes suffisent a rendre le relief lisible sans texturing : c'est le
// but de cette phase (valider que la geometrie et les normales sont correctes),
// pas de faire joli.
const vec3 kLowColor  = vec3(0.22, 0.32, 0.17); // alpage
const vec3 kMidColor  = vec3(0.45, 0.42, 0.34); // roche / eboulis
const vec3 kHighColor = vec3(0.93, 0.95, 0.98); // neve
const vec3 kCliffColor = vec3(0.34, 0.31, 0.29); // falaise (roche nue)

// --- PHASE 25 : indice visuel adret / ubac ---------------------------------
// L'ecart adret/ubac existait depuis la Phase 23 - mesure, journalise, et
// invisible : 14,7 C contre 10,7 C a midi sans que RIEN a l'ecran ne le montre.
//
// CE SHADER NE CALCULE PAS UNE DEUXIEME EXPOSITION SOLAIRE. Il reprend
// exactement la grandeur de WeatherSystem::sampleAt - l'ecart entre le cosinus
// d'incidence du point et celui qu'aurait du terrain PLAT au meme instant -
// avec la MEME direction solaire, celle de ubo.sun.xyz, qui vient de
// world/Weather.hpp. Il n'y a qu'un soleil dans ce jeu, et c'est celui de la
// meteo.
//
// Le partage des roles est le suivant, et il est deliberé :
//   - ce shader fait ce que lui seul peut faire : le cosinus d'incidence PAR
//     FRAGMENT, a partir de la normale du relief ;
//   - le CPU (main.cpp) porte tout le reste - hauteur du soleil, nebulosite -
//     dans ubo.terrain.w, parce que ces deux regles appartiennent a la meteo et
//     n'ont pas a etre re-derivees ici, ou elles pourraient diverger.
//
// Consequence directe : l'effet suit le cycle jour/nuit tout seul. Le soleil
// tourne, l'incidence de chaque versant change avec lui ; il se couche, et
// ubo.terrain.w tombe a zero. Aucune regle horaire n'est ecrite nulle part.

// Ecart de cosinus d'incidence qui vaut un effet COMPLET.
// DOIT rester egal a WeatherParams::aspectIncidenceReference (0,35). main.cpp
// verifie cette egalite au demarrage et previent si elle est rompue : un shader
// ne peut pas lire un parametre C++, donc la seule protection possible est de
// rendre la divergence BRUYANTE plutot que silencieuse.
const float kAspectIncidenceReference = 0.35;

// Les deux teintes, en MULTIPLICATEUR d'albedo.
//
// ELLES SONT EXACTEMENT SYMETRIQUES autour de 1, canal par canal
// ((1,14 + 0,86) / 2 = 1, etc.), et ce n'est pas de l'esthetique : sur du
// terrain qui n'est ni adret ni ubac le signal vaut 0, donc le melange rend le
// MILIEU des deux teintes. Un milieu qui ne serait pas (1,1,1) teinterait tout
// le plat du massif - un premier jeu de valeurs le faisait, d'un demi pour cent
// vers le chaud.
//
// AMPLITUDE : +/- 14 % sur le rouge, +/- 12 % sur le bleu, au signal PLEIN.
// La premiere version montait a 7 % et c'etait mesurablement trop peu : a 13 h,
// l'heure ou l'ecart thermique est maximal, 93,5 % des pixels ne bougeaient pas
// d'un cran sur 255 - donc un indice invisible, ce qui lui faisait manquer sa
// seule raison d'etre. Le signal, lui, n'a pas change : c'est toujours celui de
// la meteo. Seule l'AMPLITUDE D'AFFICHAGE a ete revue, et c'est un choix de
// rendu, legitimement distinct du modele physique.
//
// Le plein signal demande |incidence - incidence a plat| >= 0,35, donc un
// versant franchement oriente : sur du relief ordinaire l'effet reste bien en
// dessous de ces bornes.
const vec3 kAdretTint = vec3(1.14, 1.04, 0.88); // versant au soleil : plus dore
const vec3 kUbacTint  = vec3(0.86, 0.96, 1.12); // versant a l'ombre : plus bleute

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

// ===========================================================================
// PHASE 27 - MICRO-RELIEF DU SOL VU DE PRES (chantier 3)
// ===========================================================================
// LE PROBLEME : l'orthophoto a 0,61 m/texel, vue a pied, est agrandie jusqu'a
// ce qu'un texel couvre des dizaines de pixels. Le filtrage bilineaire en fait
// un aplat flou - le sol sous les pieds n'a aucun grain.
//
// CE QUI EST AJOUTE : de l'information dans la SEULE bande de frequences que la
// photo ne porte pas, celle qui est plus fine que son texel. Les longueurs
// d'onde du motif partent de ubo.detail.w (le texel de l'orthophoto, passe par
// main.cpp) et descendent par octaves ; rien au-dessus du texel, donc rien qui
// contredise la photo.
//
// CE QUI EST REEL ET CE QUI NE L'EST PAS, sans detour : le MOTIF est un bruit,
// il ne represente aucun caillou reel. Mais son AMPLITUDE, elle, est pilotee
// par deux donnees reelles du projet :
//   - le CONTRASTE LOCAL de l'orthophoto (ecart entre la photo et sa version
//     deux niveaux de mipmap plus floue) : un eboulis photographie est
//     contraste, une prairie rase est lisse, et le grain ajoute suit ;
//   - la PENTE du relief LiDAR : un versant raide est de la roche, plus rugueuse
//     qu'un replat.
//
// DEUX FONDUS, et c'est ce qui le garde compatible avec le LOD :
//   1. par distance (ubo.detail.y -> ubo.detail.z) : l'effet n'existe qu'a
//      portee de marche, et le cout est nul au-dela (branche sautee) ;
//   2. par OCTAVE, sur l'empreinte du pixel au sol (fwidth) : une octave dont
//      la longueur d'onde tombe sous deux pixels est eteinte AVANT de
//      scintiller. C'est la regle de Nyquist appliquee au motif, et c'est elle
//      qui interdit le moire en vue rasante - la ou le fondu par distance seul
//      echouerait.
//
// La normale perturbee ne sert QU'A l'eclairage : l'indice adret/ubac garde la
// normale du relief, parce qu'il doit dire la meme chose que la meteo, qui ne
// connait pas ce motif.

// PHASE 27, binding 2 : bruit de valeur PERIODIQUE precalcule (voir
// assets/DetailNoise.hpp). r = valeur, gb = derivees par unite de cellule,
// codees sur [-1,5 ; +1,5]. 32 cellules sur la largeur de la texture.
//
// HISTORIQUE MESURE, pour ne pas y revenir : la premiere version calculait ce
// bruit dans le shader (4 hash + interpolation par octave). +1 490 us par
// image au ras du sol en Release sur l'UHD 770, ~200 us PAR OCTAVE (bissection :
// 0 octave +330 us, 2 octaves +720 us) ; passer d'un hash entier a un hash
// flottant n'y changeait rien. C'etait l'arithmetique elle-meme. Une lecture
// de texture la remplace, et les mipmaps apportent en prime le filtrage des
// grains sous-pixel.
layout(binding = 2) uniform sampler2D detailNoise;
const float kDetailNoiseCells = 32.0;
const float kDetailNoiseSlopeRange = 1.5; // = kDetailNoiseSlopeRange (C++)

// textureGrad et non texture : l'appel vit dans une boucle a sortie NON
// uniforme (break de Nyquist) et une branche de distance, ou les derivees
// implicites sont indefinies. Les derivees d'ecran de la position sont prises
// hors branche par l'appelant, puis mises a l'echelle de l'octave.
vec3 valueNoiseD(vec2 x, vec2 dxdx, vec2 dxdy) {
    vec3 t = textureGrad(detailNoise, x / kDetailNoiseCells,
                         dxdx / kDetailNoiseCells, dxdy / kDetailNoiseCells).rgb;
    return vec3(t.r, (t.gb * 2.0 - 1.0) * kDetailNoiseSlopeRange);
}

// Octaves du motif : longueur d'onde de base, puis divisee par 2,2 a chaque
// octave (un rapport non entier evite que les octaves s'alignent en grille).
// SEPT octaves, donc de 0,61 m jusqu'a 5 mm. La premiere version en avait 4
// (finesse 6 cm) et la mesure l'a condamnee : a 2 m du regard, 6 cm couvrent
// encore une quinzaine de pixels, et l'energie haute frequence du sol proche
// n'avait PAS bouge (5,92 contre 5,91) - de l'ondulation, pas du grain. Les
// octaves fines ne coutent que de pres : le fondu de Nyquist les eteint des
// qu'elles passent sous deux pixels, et la boucle les saute alors.
const int   kDetailOctaves    = 7;
const float kDetailLacunarity = 2.2;
// Pente du relief MACRO (1 - N.y) qui vaut une rugosite complete : 0,35, le
// debut des versants ou la tuile passe deja a la teinte de falaise.
const float kDetailFullSlope = 0.35;
// Contraste local de l'orthophoto (distance RGB entre photo nette et floue) qui
// vaut une rugosite complete : 0,12, soit ~30 niveaux sur 255.
const float kDetailFullContrast = 0.12;
// Pente maximale du micro-relief, sans unite (derivee de hauteur par metre, a
// force 1). 0,6 : assez pour que chaque grain accroche la lumiere rasante,
// pas assez pour retourner une normale.
const float kDetailSlope = 1.3;
// Amplitude de couleur a force 1 : au plus +/- 30 % autour de la photo sur
// les pics du motif, +/- 10 % typiquement.
const float kDetailAlbedo = 0.30;

void main() {
    vec3 N = normalize(fragNormal);

    // Altitude ramenee dans [0,1] sur la plage reelle du terrain. Le max(...)
    // protege du cas degenere "terrain parfaitement plat" (division par zero).
    float span = max(ubo.terrain.y - ubo.terrain.x, 1e-4);
    float altitude = clamp((fragWorldPos.y - ubo.terrain.x) / span, 0.0, 1.0);

    // Degrade en deux segments : bas -> milieu, puis milieu -> haut.
    // smoothstep plutot que mix lineaire : les transitions de teinte ne
    // dessinent pas de bande nette a l'endroit du raccord.
    vec3 albedo = altitude < 0.5
        ? mix(kLowColor, kMidColor, smoothstep(0.0, 0.5, altitude))
        : mix(kMidColor, kHighColor, smoothstep(0.5, 1.0, altitude));

    // Pente : N.y vaut 1 a plat et tend vers 0 sur une paroi verticale. Une
    // pente forte ne retient ni terre ni neige -> roche nue. C'est ce terme qui
    // rend les versants raides immediatement visibles, donc qui valide que les
    // normales sont bien calculees.
    float slope = 1.0 - clamp(N.y, 0.0, 1.0);
    albedo = mix(albedo, kCliffColor, smoothstep(0.25, 0.6, slope));

    // --- Orthophoto ---------------------------------------------------------
    // L'UV couvre EXACTEMENT l'emprise du terrain (0,0 = coin nord-ouest). Le
    // niveau de mipmap est choisi par le materiel a partir des derivees de
    // fragUV : c'est ce qui evite le fourmillement en vue lointaine, ou un texel
    // de 0,61 m se projette bien en dessous du pixel.
    //
    // La photo remplace l'albedo mais PAS l'eclairage : le Blinn-Phong ci-dessous
    // continue de s'appliquer avec la normale du relief. Une orthophoto est prise
    // a la verticale par temps clair, donc deja a peu pres uniformement eclairee ;
    // la relancer dans le modele d'eclairage est ce qui redonne du volume aux
    // versants, qu'une photo plaquee telle quelle aplatit completement.
    vec3 ortho = texture(orthoTexture, fragUV).rgb;
    albedo = mix(albedo, ortho, clamp(ubo.terrain.z, 0.0, 1.0));

    // --- PHASE 27 : sol mouille --------------------------------------------
    // Pilote par WeatherSample::wetness (relecture de la pluie des dernieres
    // heures, Phase 23) via weatherVisualsFor() : ubo.precip.w vaut
    // wetGroundDarkening * wetness, donc exactement 0 sur un sol sec. Le sol
    // fonce apres l'averse et SECHE avec le modele, sans aucune regle ici.
    albedo *= 1.0 - clamp(ubo.precip.w, 0.0, 1.0);

    // fragTint vaut (1,1,1) pour le terrain synthetique : reserve a une future
    // classification par sommet (sol / vegetation / bati venant du LiDAR).
    albedo *= fragTint;

    vec3 L = normalize(ubo.sun.xyz);

    // --- PHASE 27 : micro-relief (voir l'en-tete du bloc au-dessus de main) --
    vec3 Nlit = N;
    float detailStrength = clamp(ubo.detail.x, 0.0, 1.0);
    float viewDistance = length(ubo.viewPos - fragWorldPos);
    float distanceFade = detailStrength > 0.0
        ? 1.0 - smoothstep(ubo.detail.y, ubo.detail.z, viewDistance)
        : 0.0;
    // Derivees d'ecran calculees HORS de la branche : fwidth() et le choix du
    // mipmap exigent un flot de controle uniforme dans le quad de pixels.
    vec2 footprint = fwidth(fragWorldPos.xz);
    vec2 posDx = dFdx(fragWorldPos.xz);
    vec2 posDy = dFdy(fragWorldPos.xz);
    float orthoLod = textureQueryLod(orthoTexture, fragUV).x;
    if (distanceFade > 0.0) {
        // Rugosite pilotee par les donnees : contraste local de la photo et
        // pente LiDAR. Plancher de 0,5 : meme une prairie lisse a du grain,
        // sinon le sol le plus courant du jeu resterait l'aplat a corriger.
        vec3 orthoBlur = textureLod(orthoTexture, fragUV, orthoLod + 2.0).rgb;
        float photoContrast = length(ortho - orthoBlur) * clamp(ubo.terrain.z, 0.0, 1.0);
        float roughness = clamp(max(photoContrast / kDetailFullContrast,
                                    slope / kDetailFullSlope), 0.0, 1.0);
        float amplitude = detailStrength * distanceFade * mix(0.5, 1.0, roughness);

        // Empreinte d'un pixel au sol, en metres (le plus grand des deux axes :
        // en vue rasante, c'est l'axe de profondeur qui s'etire).
        float pixelMeters = max(footprint.x, footprint.y);

        float wavelength = max(ubo.detail.w, 0.01);
        float height = 0.0;
        vec2 gradient = vec2(0.0);
                for (int o = 0; o < kDetailOctaves; ++o) {
            // Nyquist : plein poids tant que la longueur d'onde couvre au moins
            // 4 pixels, eteint a 2 pixels. (Resserre a 8 -> 4 pendant la
            // chasse au cout, puis RETABLI : depuis que le bruit est une
            // texture mipmappee, une octave ne coute qu'une lecture et les
            // mipmaps filtrent deja le sous-pixel. La coupure a 8 px ne faisait
            // plus que retirer le grain fin.) HISTORIQUE : la premiere
            // version (4 -> 2 pixels) faisait tourner les 7 octaves sur tout le
            // sol proche et coutait +1 683 us par image au ras du sol (x1,94 sur
            // l'image entiere, Release) - pour des octaves de 5 et 11 mm qui
            // tombaient a 2-4 pixels, donc invisibles ou scintillantes.
            float nyquist = 1.0 - smoothstep(0.25, 0.5, pixelMeters / wavelength);
            // Les longueurs d'onde decroissent : une octave eteinte eteint
            // toutes les suivantes, inutile de continuer la boucle.
            if (nyquist <= 0.0) {
                break;
            }
            // Poids de hauteur proportionnel a la longueur d'onde : la PENTE de
            // chaque octave est alors comparable (n' / lambda * lambda), d'ou un
            // sol grenu a toutes les echelles plutot qu'ondule.
            float octaveWeight = wavelength / max(ubo.detail.w, 0.01);
            if (nyquist > 0.0) {
                // Decalage propre a chaque octave : sans lui, toutes les octaves
                // partageraient l'origine de la texture periodique.
                vec3 n = valueNoiseD(fragWorldPos.xz / wavelength + float(o) * vec2(7.31, 3.17),
                                     posDx / wavelength, posDy / wavelength);
                height += nyquist * octaveWeight * (n.x - 0.5);
                gradient += nyquist * n.yz; // = octaveWeight * lambda0 * (n'/lambda)
            }
            wavelength /= kDetailLacunarity;
        }
        // Pas de normalisation par la somme des poids ni par le nombre
        // d'octaves : la premiere version faisait les deux, et divisait ainsi
        // l'effet par ~7 avant meme d'appliquer sa force. La hauteur reste dans
        // [-0,9, 0,9] environ (somme geometrique des poids), la pente par
        // octave est comparable a toutes les echelles.
        height = clamp(height, -1.0, 1.0);
        gradient *= 0.5;

        // Normale : la surface y = h(x, z) a pour normale (-dh/dx, 1, -dh/dz).
        // On incline la normale du relief de ce gradient (approximation exacte
        // a plat, bonne jusqu'aux pentes de falaise).
        Nlit = normalize(N + amplitude * kDetailSlope * vec3(-gradient.x, 0.0, -gradient.y));

        // Couleur : modulation autour de la photo, jamais remplacement.
        albedo *= 1.0 + amplitude * kDetailAlbedo * height;
    }

    // --- PHASE 25 : adret / ubac -------------------------------------------
    // MODULATION de l'albedo (donc de l'orthophoto), jamais un remplacement :
    // la photo continue de porter tout le detail, elle est seulement rechauffee
    // ou refroidie.
    //
    // Pourquoi ca ne fait PAS double emploi avec le diffus de Blinn-Phong, qui
    // assombrit deja les versants a l'ombre : l'eclairage joue sur la
    // LUMINOSITE, cet indice joue sur la TEINTE. Un ubac faiblement eclaire et
    // un adret vu de loin dans la brume peuvent avoir la meme luminosite ; la
    // difference d'exposition, elle, reste lisible dans la couleur. C'est aussi
    // ce qui rend l'indice perceptible sur une orthophoto, qui est prise a la
    // verticale par temps clair et arrive donc deja "a plat".
    float aspectStrength = clamp(ubo.terrain.w, 0.0, 1.0);
    if (aspectStrength > 0.0) {
        // Les deux cosinus sont ecretes a zero, exactement comme dans
        // WeatherSystem::sampleAt : une surface qui tourne le dos au soleil ne
        // recoit pas un rayonnement negatif, elle n'en recoit pas.
        float incidence     = max(dot(N, L), 0.0);
        float flatIncidence = max(L.y, 0.0);
        float aspectSignal  = clamp((incidence - flatIncidence) / kAspectIncidenceReference,
                                    -1.0, 1.0);

        // -1 -> ubac franc, 0 -> ni l'un ni l'autre, +1 -> adret franc.
        vec3 tint = mix(kUbacTint, kAdretTint, 0.5 + 0.5 * aspectSignal);
        albedo *= mix(vec3(1.0), tint, aspectStrength);
    }

    // --- Blinn-Phong, identique a la Phase 2 -------------------------------
    vec3 V = normalize(ubo.viewPos - fragWorldPos);
    vec3 H = normalize(L + V);

    // PHASE 27 : Nlit = normale du relief + micro-relief de pres (egale a N
    // partout ailleurs). L'indice adret/ubac ci-dessus garde N.
    float diffuse = max(dot(Nlit, L), 0.0);
    float specular = diffuse > 0.0
        ? kSpecularStrength * pow(max(dot(Nlit, H), 0.0), kShininess)
        : 0.0;

    // PHASE 23 : l'intensite directe et le multiplicateur d'ambiante viennent du
    // cycle jour/nuit. En plein jour, sun.w = 1 et sunColor.a = 1 : l'expression
    // ci-dessous est alors mot pour mot celle de la Phase 2.
    vec3  kLightColor = ubo.sunColor.rgb;
    float ambient     = kAmbientStrength * ubo.sunColor.a;
    diffuse  *= ubo.sun.w;
    specular *= ubo.sun.w;

    vec3 lit = albedo * kLightColor * (ambient + diffuse)
             + kLightColor * specular;

    outColor = vec4(applyFog(lit, fragWorldPos), 1.0);
}
