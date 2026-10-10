#pragma once

#include <iostream>
#include <string>
#include <string_view>

namespace vercora {

// Logger minimaliste pour la Phase 0. On garde ca volontairement simple :
// pas de dependance externe, sortie sur stdout/stderr.
enum class LogLevel { Info, Warn, Error };

inline void log(LogLevel level, std::string_view message) {
    const char* prefix = "[INFO]  ";
    std::ostream* out = &std::cout;
    switch (level) {
        case LogLevel::Info: prefix = "[INFO]  "; out = &std::cout; break;
        case LogLevel::Warn: prefix = "[WARN]  "; out = &std::cerr; break;
        case LogLevel::Error: prefix = "[ERROR] "; out = &std::cerr; break;
    }
    (*out) << prefix << message << std::endl;
}

inline void logInfo(std::string_view msg)  { log(LogLevel::Info, msg); }
inline void logWarn(std::string_view msg)  { log(LogLevel::Warn, msg); }
inline void logError(std::string_view msg) { log(LogLevel::Error, msg); }

} // namespace vercora
