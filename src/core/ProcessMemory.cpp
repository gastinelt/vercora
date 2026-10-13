#include "core/ProcessMemory.hpp"

#include <cstdio>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
// Avec PSAPI_VERSION 2 (le defaut depuis Windows 7), GetProcessMemoryInfo est
// K32GetProcessMemoryInfo, exportee par kernel32 : aucune bibliotheque de plus
// a lier.
#  include <psapi.h>
#elif defined(__linux__)
#  include <fstream>
#  include <sstream>
#endif

namespace vercora {

ProcessMemory processMemory() {
    ProcessMemory memory;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        memory.residentBytes = static_cast<uint64_t>(counters.WorkingSetSize);
        memory.peakResidentBytes = static_cast<uint64_t>(counters.PeakWorkingSetSize);
        memory.available = true;
    }
#elif defined(__linux__)
    // Lignes "VmRSS:   123456 kB" et "VmHWM:   234567 kB".
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        const bool rss = line.rfind("VmRSS:", 0) == 0;
        const bool hwm = line.rfind("VmHWM:", 0) == 0;
        if (!rss && !hwm) {
            continue;
        }
        std::istringstream fields(line.substr(6));
        uint64_t kilobytes = 0;
        fields >> kilobytes;
        (rss ? memory.residentBytes : memory.peakResidentBytes) = kilobytes * 1024u;
        memory.available = true;
    }
#endif
    return memory;
}

std::string processMemoryLine() {
    const ProcessMemory memory = processMemory();
    if (!memory.available) {
        return "RAM non mesuree (plateforme non prise en charge)";
    }
    char text[96];
    std::snprintf(text, sizeof(text), "RAM %.1f Mo (pic %.1f Mo)",
                  static_cast<double>(memory.residentBytes) / (1024.0 * 1024.0),
                  static_cast<double>(memory.peakResidentBytes) / (1024.0 * 1024.0));
    return text;
}

} // namespace vercora
