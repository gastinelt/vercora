#pragma once

// GLFW_INCLUDE_VULKAN est defini par CMake : GLFW tire alors les headers Vulkan.
#include <GLFW/glfw3.h>

#include <string>

namespace vercora {

// Encapsule la fenetre GLFW et l'init/terminate de la bibliotheque.
// Cross-platform : aucune API Win32/X11 directe n'est utilisee ici.
class Window {
public:
    Window(int width, int height, std::string title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const;
    void pollEvents() const;

    // Bloque tant que la fenetre est minimisee (taille 0). Utilise lors du resize.
    void waitEvents() const;

    // Change le titre affiche (utilise pour le compteur de FPS).
    void setTitle(const std::string& title) const;

    // Cree la surface Vulkan associee a cette fenetre (VK_KHR_surface).
    VkSurfaceKHR createSurface(VkInstance instance) const;

    // Taille reelle du framebuffer en pixels (peut differer de la taille logique
    // sur ecrans HiDPI).
    void getFramebufferSize(int& width, int& height) const;

    bool wasResized() const { return framebufferResized_; }
    void resetResizedFlag() { framebufferResized_ = false; }

    // --- Entrees (utilisees par la camera) ---
    // 'key' / 'button' sont des constantes GLFW (GLFW_KEY_*, GLFW_MOUSE_BUTTON_*).
    bool isKeyPressed(int key) const;
    bool isMouseButtonPressed(int button) const;
    void getCursorPos(double& x, double& y) const;

    // PHASE 15. Crans de molette accumules depuis le dernier appel, puis remis a
    // zero. Positif = vers l'avant.
    //
    // ACCUMULE et non lu a la demande, parce que la molette est le seul peripherique
    // du jeu qui n'a pas d'etat : GLFW n'offre aucun `glfwGetScroll()` equivalent a
    // `glfwGetKey()`, il n'y a que des EVENEMENTS. Un cran survenu entre deux images
    // serait donc simplement perdu si on ne le retenait pas.
    //
    // La lecture est destructive (d'ou "consume") : deux appelants dans la meme image
    // se voleraient les crans l'un a l'autre. Il n'y en a qu'un, l'editeur, et c'est
    // volontaire - la molette n'a aucun role dans le jeu normal.
    double consumeScroll();

    GLFWwindow* handle() const { return window_; }

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset);

    GLFWwindow* window_ = nullptr;
    bool framebufferResized_ = false;
    double scrollAccumulator_ = 0.0;
};

} // namespace vercora
