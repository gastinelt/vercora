#include "core/Paths.hpp"

#include <filesystem>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace vercora {
namespace {

// Chemin complet de l'executable, ou chaine vide si indisponible.
std::string readExePath() {
#ifdef _WIN32
    // MAX_PATH ne suffit pas toujours (chemins longs actives) : on agrandit
    // tant que l'API dit que le tampon est trop petit. GetModuleFileNameW ne
    // termine PAS par zero en cas de troncature avant Windows 10, d'ou le test
    // sur la taille plutot que sur le contenu.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            return std::filesystem::path(buffer).string();
        }
        if (buffer.size() > 65536) {
            return {};  // garde-fou : quelque chose ne va pas, on abandonne.
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code error;
    const std::filesystem::path self =
        std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::string{} : self.string();
#endif
}

// Applique la regle documentee dans Paths.hpp : le sous-dossier `name` a cote de
// l'executable s'il existe, sinon `fallback` (la macro CMake).
std::string resolveDataDir(const char* name, const char* fallback) {
    const std::string& exe = exeDir();
    if (!exe.empty()) {
        const std::filesystem::path candidate = std::filesystem::path(exe) / name;
        std::error_code error;
        if (std::filesystem::is_directory(candidate, error)) {
            return candidate.string();
        }
    }
    return fallback;
}

}  // namespace

const std::string& exeDir() {
    static const std::string cached = [] {
        const std::string path = readExePath();
        if (path.empty()) {
            return std::string{};
        }
        return std::filesystem::path(path).parent_path().string();
    }();
    return cached;
}

const std::string& assetsDir() {
    static const std::string cached = resolveDataDir("assets", ASSETS_DIR);
    return cached;
}

const std::string& shadersDir() {
    static const std::string cached = resolveDataDir("shaders", SHADERS_OUTPUT_DIR);
    return cached;
}

}  // namespace vercora
