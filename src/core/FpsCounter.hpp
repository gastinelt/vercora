#pragma once

namespace vercora {

// Compteur de FPS moyen sur une fenetre glissante d'un intervalle fixe.
//
// Pourquoi ne pas afficher 1/deltaTime a chaque frame : la valeur instantanee
// saute dans tous les sens (illisible), et changer le titre de la fenetre est un
// appel systeme qu'on ne veut pas payer 300 fois par seconde. On moyenne donc sur
// ~1 seconde et on ne signale une nouvelle valeur qu'a ce rythme.
class FpsCounter {
public:
    explicit FpsCounter(float updateIntervalSeconds = 1.0f)
        : updateInterval_(updateIntervalSeconds) {}

    // A appeler une fois par frame, avec le meme deltaTime que la camera.
    // Renvoie true quand une nouvelle valeur vient d'etre calculee (donc environ
    // une fois par intervalle) : c'est le moment de rafraichir l'affichage.
    bool update(float deltaTime) {
        accumulatedTime_ += deltaTime;
        ++frameCount_;

        if (accumulatedTime_ < updateInterval_) {
            return false;
        }

        // Division sure : accumulatedTime_ >= updateInterval_ > 0 a ce point.
        fps_ = static_cast<float>(frameCount_) / accumulatedTime_;

        accumulatedTime_ = 0.0f;
        frameCount_ = 0;
        return true;
    }

    // Derniere valeur calculee (0 tant que le premier intervalle n'est pas ecoule).
    float fps() const { return fps_; }

private:
    float updateInterval_;
    float accumulatedTime_ = 0.0f;
    int   frameCount_ = 0;
    float fps_ = 0.0f;
};

} // namespace vercora
