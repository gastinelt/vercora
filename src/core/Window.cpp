#include "core/Window.hpp"

#include <stdexcept>

namespace vercora {

Window::Window(int width, int height, std::string title) {
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("Echec de l'initialisation de GLFW.");
    }

    // Pas de contexte OpenGL : on gere le rendu nous-memes via Vulkan.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        glfwTerminate();
        throw std::runtime_error("Echec de la creation de la fenetre GLFW.");
    }

    // Permet de retrouver l'instance Window depuis le callback C de GLFW.
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    glfwSetScrollCallback(window_, scrollCallback);
}

Window::~Window() {
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(window_) == GLFW_TRUE;
}

void Window::pollEvents() const {
    glfwPollEvents();
}

void Window::waitEvents() const {
    glfwWaitEvents();
}

void Window::setTitle(const std::string& title) const {
    glfwSetWindowTitle(window_, title.c_str());
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation de la surface Vulkan.");
    }
    return surface;
}

void Window::getFramebufferSize(int& width, int& height) const {
    glfwGetFramebufferSize(window_, &width, &height);
}

bool Window::isKeyPressed(int key) const {
    return glfwGetKey(window_, key) == GLFW_PRESS;
}

bool Window::isMouseButtonPressed(int button) const {
    return glfwGetMouseButton(window_, button) == GLFW_PRESS;
}

void Window::getCursorPos(double& x, double& y) const {
    glfwGetCursorPos(window_, &x, &y);
}

double Window::consumeScroll() {
    const double accumulated = scrollAccumulator_;
    scrollAccumulator_ = 0.0;
    return accumulated;
}

void Window::framebufferResizeCallback(GLFWwindow* window, int /*width*/, int /*height*/) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self != nullptr) {
        self->framebufferResized_ = true;
    }
}

void Window::scrollCallback(GLFWwindow* window, double /*xOffset*/, double yOffset) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self != nullptr) {
        self->scrollAccumulator_ += yOffset;
    }
}

} // namespace vercora
