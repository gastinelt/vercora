#include "assets/Heightmap.hpp"

#include "core/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace vercora {
namespace {

// --- Bruit de valeur (value noise) --------------------------------------
// Choix : bruit de VALEUR plutot que Perlin/Simplex. Il ne demande aucune
// table de permutation ni gradients pre-calcules (donc zero etat, zero
// allocation, et un resultat identique sur toutes les plateformes), et une
// fois empile en fBm il est visuellement indiscernable de Perlin a cette
// echelle. Le cout par echantillon est plus eleve que Perlin, mais on genere
// 16384 points UNE fois au demarrage : c'est sans importance.

// Hash entier -> [0,1). Melange multiplicatif + decalages facon "integer
// finalizer" (meme famille que murmur3) : deux entrees voisines donnent des
// sorties decorrelees, ce qui est tout ce qu'on demande a un bruit de valeur.
float hash01(int64_t x, int64_t z, uint32_t seed) {
    uint32_t h = seed;
    h ^= static_cast<uint32_t>(static_cast<uint64_t>(x)) * 0x9E3779B1u;
    h ^= static_cast<uint32_t>(static_cast<uint64_t>(z)) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    // 24 bits suffisent largement et tiennent exactement dans la mantisse d'un
    // float (pas d'arrondi, donc resultat parfaitement reproductible).
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

// Courbe de Hermite : derivee nulle aux extremites, donc pas de cassure de
// pente visible aux frontieres des cellules du bruit (une interpolation
// lineaire donnerait un relief en facettes losangees, tres reconnaissable).
float smoothCurve(float t) {
    return t * t * (3.0f - 2.0f * t);
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float valueNoise(float x, float z, uint32_t seed) {
    const float fx = std::floor(x);
    const float fz = std::floor(z);
    const int64_t ix = static_cast<int64_t>(fx);
    const int64_t iz = static_cast<int64_t>(fz);

    const float tx = smoothCurve(x - fx);
    const float tz = smoothCurve(z - fz);

    const float n00 = hash01(ix,     iz,     seed);
    const float n10 = hash01(ix + 1, iz,     seed);
    const float n01 = hash01(ix,     iz + 1, seed);
    const float n11 = hash01(ix + 1, iz + 1, seed);

    return lerp(lerp(n00, n10, tx), lerp(n01, n11, tx), tz);
}

// fBm : somme d'octaves de frequence doublee et d'amplitude halvee.
// Renvoie une valeur dans [0,1] (division par la somme des amplitudes).
//
// 'ridged' transforme chaque octave en 1 - |2n - 1| : la valeur culmine la ou
// le bruit passait par sa moyenne, ce qui remplace les dômes arrondis par des
// LIGNES de crete continues, avec une derivee discontinue au sommet (arete
// vive). C'est la recette classique pour un relief de montagne, et c'est ce qui
// cree les versants raides dont le fragment shader a besoin pour montrer sa
// coloration par pente.
float fbm(float x, float z, const Heightmap::Params& params, bool ridged) {
    float sum = 0.0f;
    float normalization = 0.0f;
    float amplitude = 1.0f;
    float frequency = params.baseFrequency;

    for (uint32_t octave = 0; octave < params.octaves; ++octave) {
        // Graine decalee par octave : sans ca, toutes les octaves seraient la
        // meme image a des echelles differentes et leurs maxima s'aligneraient.
        // Le decalage differe aussi entre les deux variantes, pour que les
        // cretes ne tombent pas systematiquement sur les sommets du fBm doux.
        const uint32_t octaveSeed =
            params.seed + octave * 7919u + (ridged ? 104729u : 0u);

        float n = valueNoise(x * frequency, z * frequency, octaveSeed);
        if (ridged) {
            n = 1.0f - std::fabs(2.0f * n - 1.0f);
        }

        sum += amplitude * n;
        normalization += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }

    return normalization > 0.0f ? sum / normalization : 0.0f;
}

} // namespace

Heightmap::Heightmap(const Params& params) : params_(params) {
    if (params_.resolution < 2) {
        throw std::runtime_error(
            "Heightmap : resolution de " + std::to_string(params_.resolution) +
            " echantillons, il en faut au moins 2 pour former une surface.");
    }
    if (!(params_.worldSize > 0.0f) || !(params_.amplitude >= 0.0f)) {
        throw std::runtime_error(
            "Heightmap : worldSize doit etre > 0 et amplitude >= 0.");
    }
    if (params_.octaves == 0) {
        throw std::runtime_error("Heightmap : il faut au moins une octave de bruit.");
    }

    const uint32_t res = params_.resolution;
    // resolution-1 cellules couvrent worldSize -> pas de la grille.
    spacing_ = params_.worldSize / static_cast<float>(res - 1);
    halfSize_ = params_.worldSize * 0.5f;

    heights_.resize(static_cast<std::size_t>(res) * res);

    for (uint32_t iz = 0; iz < res; ++iz) {
        // Coordonnees normalisees [0,1] : la frequence du bruit est ainsi
        // exprimee en "motifs sur toute la largeur", independamment de la
        // resolution. Changer resolution affine donc le maillage SANS changer
        // la forme du relief - pratique pour comparer deux resolutions.
        const float v = static_cast<float>(iz) / static_cast<float>(res - 1);

        for (uint32_t ix = 0; ix < res; ++ix) {
            const float u = static_cast<float>(ix) / static_cast<float>(res - 1);

            // Deux reliefs superposes puis melanges : le fBm doux donne les
            // grandes ondulations et les fonds de vallee, le ridged donne les
            // aretes. std::pow sur une base garantie >= 0 : pas de NaN possible.
            const float smooth = std::pow(fbm(u, v, params_, /*ridged=*/false),
                                          params_.reliefExponent);
            const float ridge = fbm(u, v, params_, /*ridged=*/true);
            // Elevation au carre : accentue les cretes et aplatit le bas des
            // versants, ce qui evite le relief "en toile de tente" uniforme.
            const float shaped = lerp(smooth, ridge * ridge,
                                      std::clamp(params_.ridgeWeight, 0.0f, 1.0f));

            heights_[static_cast<std::size_t>(iz) * res + ix] =
                shaped * params_.amplitude;
        }
    }

    label_ = "terrain synthetique " + std::to_string(res) + "x" + std::to_string(res);
    finalize();

    logInfo("Heightmap synthetique generee : " + std::to_string(res) + "x" +
            std::to_string(res) + " echantillons sur " +
            std::to_string(params_.worldSize) + " unites (pas de " +
            std::to_string(spacing_) + "), altitudes " +
            std::to_string(minHeight_) + " a " + std::to_string(maxHeight_) + ".");
}

Heightmap::Heightmap(std::vector<float> heights, uint32_t resolution,
                     float spacing, std::string label, const GeoReference& geo)
    : heights_(std::move(heights)), label_(std::move(label)), geo_(geo) {
    if (resolution < 2) {
        throw std::runtime_error(
            "Heightmap : grille de " + std::to_string(resolution) +
            " echantillons par cote, il en faut au moins 2 pour former une surface.");
    }
    if (!std::isfinite(spacing) || spacing <= 0.0f) {
        throw std::runtime_error(
            "Heightmap : pas de grille invalide (" + std::to_string(spacing) + ").");
    }

    const std::size_t expected =
        static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
    if (heights_.size() != expected) {
        throw std::runtime_error(
            "Heightmap : " + std::to_string(heights_.size()) + " altitudes fournies "
            "pour une grille " + std::to_string(resolution) + "x" +
            std::to_string(resolution) + " qui en attend " + std::to_string(expected) + ".");
    }

    params_.resolution = resolution;
    // La taille monde DECOULE du pas et du nombre d'echantillons : pour une
    // grille mesuree c'est le pas qui est la donnee (1 m sur la dalle LiDAR),
    // pas l'emprise. L'ordre est donc inverse de celui du generateur procedural,
    // ou l'on impose l'emprise et ou le pas s'en deduit.
    params_.worldSize = spacing * static_cast<float>(resolution - 1);
    spacing_ = spacing;
    halfSize_ = params_.worldSize * 0.5f;

    finalize();
}

void Heightmap::finalize() {
    minHeight_ = std::numeric_limits<float>::max();
    maxHeight_ = std::numeric_limits<float>::lowest();

    for (std::size_t i = 0; i < heights_.size(); ++i) {
        const float height = heights_[i];
        // Un seul NaN suffit a produire un triangle degenere et un terrain qui
        // clignote, sans le moindre message d'erreur. On refuse ici plutot que
        // de le decouvrir a l'ecran : c'est le risque numero un avec des donnees
        // mesurees, ou une cellule sans aucun point de retour est frequente.
        if (!std::isfinite(height)) {
            const uint32_t res = params_.resolution;
            throw std::runtime_error(
                "Heightmap : altitude non finie (NaN ou infini) a l'echantillon (" +
                std::to_string(i % res) + ", " + std::to_string(i / res) + ").");
        }
        minHeight_ = std::min(minHeight_, height);
        maxHeight_ = std::max(maxHeight_, height);
    }
}

float Heightmap::at(int64_t ix, int64_t iz) const {
    const int64_t last = static_cast<int64_t>(params_.resolution) - 1;
    const int64_t cx = std::clamp(ix, static_cast<int64_t>(0), last);
    const int64_t cz = std::clamp(iz, static_cast<int64_t>(0), last);
    return heights_[static_cast<std::size_t>(cz) * params_.resolution +
                    static_cast<std::size_t>(cx)];
}

float Heightmap::worldCoord(uint32_t index) const {
    return -halfSize_ + static_cast<float>(index) * spacing_;
}

std::string Heightmap::describe() const {
    return label_;
}

void Heightmap::toProjected(float worldX, float worldZ,
                            double* east, double* north) const {
    // Inverse de worldCoord() : worldX = -halfSize + ix * spacing, donc la
    // distance a l'echantillon 0 vaut worldX + halfSize.
    //
    // Le signe differe entre les deux axes et ce n'est pas une faute de frappe :
    // ix croissant va vers l'EST (meme sens que la coordonnee projetee), tandis
    // que iz croissant va vers le SUD, c'est-a-dire dans le sens DECROISSANT du
    // nord. Voir l'orientation documentee dans Heightmap.hpp.
    if (east)  { *east  = geo_.originEast  + static_cast<double>(worldX + halfSize_); }
    if (north) { *north = geo_.originNorth - static_cast<double>(worldZ + halfSize_); }
}

void Heightmap::fromProjected(double east, double north,
                              float* worldX, float* worldZ) const {
    // Inversion terme a terme de toProjected ci-dessus. La soustraction se fait
    // en DOUBLE avant la conversion en float, et c'est le point qui compte : un
    // Lambert 93 vaut ~6,4 millions en nord, ou l'ecart entre deux float
    // consecutifs atteint deja 0,5 m. Convertir d'abord puis soustraire
    // perdrait un demi-metre sur chaque objet pose.
    if (worldX) {
        *worldX = static_cast<float>(east - geo_.originEast) - halfSize_;
    }
    if (worldZ) {
        *worldZ = static_cast<float>(geo_.originNorth - north) - halfSize_;
    }
}

float Heightmap::heightAt(float worldX, float worldZ) const {
    const float last = static_cast<float>(params_.resolution - 1);

    // Passage en coordonnees de grille (unite = un echantillon), ecretees sur
    // le domaine : au-dela du bord, on prolonge la hauteur du bord.
    // std::clamp sur des NaN renverrait NaN ; on s'en protege en amont plutot
    // que de propager une position de camera invalide.
    const float gx = std::isfinite(worldX)
        ? std::clamp((worldX + halfSize_) / spacing_, 0.0f, last) : 0.0f;
    const float gz = std::isfinite(worldZ)
        ? std::clamp((worldZ + halfSize_) / spacing_, 0.0f, last) : 0.0f;

    const float fx = std::floor(gx);
    const float fz = std::floor(gz);
    const int64_t ix = static_cast<int64_t>(fx);
    const int64_t iz = static_cast<int64_t>(fz);

    // Position dans la cellule, dans [0,1].
    const float tx = gx - fx;
    const float tz = gz - fz;

    const float h00 = at(ix,     iz);
    const float h10 = at(ix + 1, iz);
    const float h01 = at(ix,     iz + 1);
    const float h11 = at(ix + 1, iz + 1);

    // TerrainMesh coupe chaque cellule le long de la diagonale h00-h11.
    // Interpoler bilineairement ici donnerait une hauteur legerement au-dessus
    // ou en dessous des triangles reellement affiches ; on reproduit donc le
    // decoupage exact, ce qui colle la camera pile sur la surface visible.
    if (tz > tx) {
        // Triangle (h00, h01, h11) : affine, exact aux trois sommets.
        return h00 + (h01 - h00) * tz + (h11 - h01) * tx;
    }
    // Triangle (h00, h10, h11).
    return h00 + (h10 - h00) * tx + (h11 - h10) * tz;
}

} // namespace vercora
