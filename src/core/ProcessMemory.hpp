#pragma once

#include <cstdint>
#include <string>

namespace vercora {

// PHASE 30. Memoire RESIDENTE du processus, lue au systeme : ce que le
// gestionnaire des taches appelle "jeu de travail" sous Windows, VmRSS sous
// Linux. Le pic est celui de toute la vie du processus (PeakWorkingSetSize,
// VmHWM).
//
// Jusqu'ici la RAM ne se mesurait que de l'EXTERIEUR (scripts PowerShell qui
// interrogent le processus a intervalles fixes, donc qui ratent les pics
// courts). Le jeu la journalise desormais lui-meme, au demarrage, en cours de
// partie et a la fermeture - le meme principe que le bilan de streaming.
//
// Sur une memoire UNIFIEE (GPU integre, lavapipe), la memoire GPU allouee peut
// apparaitre dans ce chiffre : le lire a cote de VulkanContext::gpuMemoryLine.
struct ProcessMemory {
    uint64_t residentBytes = 0;
    uint64_t peakResidentBytes = 0;
    bool available = false; // false = plateforme non prise en charge
};

ProcessMemory processMemory();

// "RAM 512.3 Mo (pic 734.0 Mo)", ou "RAM non mesuree" hors plateforme connue.
std::string processMemoryLine();

} // namespace vercora
