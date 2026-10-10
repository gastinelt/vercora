cmake_minimum_required(VERSION 3.20)
project(Vercora LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Standard C++20
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type par defaut = Debug (pour activer les validation layers)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Vulkan SDK (LunarG) : suppose installe sur la machine (variable VULKAN_SDK)
# ---------------------------------------------------------------------------
find_package(Vulkan 1.2 REQUIRED)

# ---------------------------------------------------------------------------
# Dependances externes via FetchContent (telechargees dans external/)
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external)

# --- GLFW (fenetrage cross-platform) ---
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.4
  GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# --- GLM (maths, header-only, pas encore utilise mais deja en place) ---
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# --- tinyobjloader (parsing des fichiers .obj, header-only, licence MIT) ---
# SOURCE_SUBDIR pointe volontairement vers un sous-dossier inexistant :
# FetchContent_MakeAvailable n'appelle add_subdirectory() que s'il y trouve un
# CMakeLists.txt. On recupere donc uniquement les sources, sans configurer le
# projet CMake de tinyobjloader (dont le cmake_minimum_required est trop ancien
# pour CMake 4.x, qui a supprime la compatibilite avec les versions < 3.5).
# La bibliotheque etant header-only, un simple include directory suffit.
#
# Tag v2.0.0rc13 (le plus recent publie, celui que distribue vcpkg) et NON
# v1.0.6 : ce dernier date de 2017 et expose une API plus pauvre (un seul
# message d'erreur au lieu de warning + erreur separes, et pas de lecture des
# couleurs de sommet).
FetchContent_Declare(
  tinyobjloader
  GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
  GIT_TAG        v2.0.0rc13
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  cmake-volontairement-absent
)
FetchContent_MakeAvailable(tinyobjloader)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(vercora
  src/main.cpp
  src/core/Window.cpp
  src/core/Camera.cpp
  src/core/Paths.cpp
  src/assets/ObjLoader.cpp
  src/assets/Heightmap.cpp
  src/assets/TerrainFile.cpp
  src/assets/TextureFile.cpp
  src/assets/DetailNoise.cpp
  src/world/TreeMesh.cpp
  src/world/StoneMesh.cpp
  src/world/Tree.cpp
  src/world/Ground.cpp
  src/world/Knapping.cpp
  src/world/Crafting.cpp
  src/world/CraftMesh.cpp
  src/world/Forest.cpp
  src/world/Habitat.cpp
  src/world/Wildlife.cpp
  src/world/AnimalMesh.cpp
  src/world/Vegetation.cpp
  src/world/GrassMesh.cpp
  src/world/Paraglider.cpp
  src/world/Bike.cpp
  src/world/Climber.cpp
  src/world/Needs.cpp
  src/world/Weather.cpp
  src/world/WaterSource.cpp
  src/world/Deposits.cpp
  src/world/Resin.cpp
  src/world/TreeSpecies.cpp
  src/world/FiberPlants.cpp
  src/world/Rocks.cpp
  src/world/RockMesh.cpp
  src/world/Bushes.cpp
  src/world/BushMesh.cpp
  src/world/HarvestMesh.cpp
  src/world/TerrainQuadtree.cpp
  src/editor/EditorAssets.cpp
  src/editor/AssetCatalog.cpp
  src/editor/EditorScene.cpp
  src/editor/EditorController.cpp
  src/editor/SceneFile.cpp
  src/renderer/VulkanContext.cpp
  src/renderer/VulkanBuffer.cpp
  src/renderer/Mesh.cpp
  src/renderer/TerrainMesh.cpp
  src/renderer/TerrainNodeMesh.cpp
  src/renderer/TerrainStreamer.cpp
  src/renderer/SwapChain.cpp
  src/renderer/Pipeline.cpp
  src/renderer/Renderer.cpp
  src/renderer/Texture.cpp
)

target_include_directories(vercora PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${tinyobjloader_SOURCE_DIR}
)

target_link_libraries(vercora PRIVATE
  Vulkan::Vulkan
  glfw
  glm::glm
)

# GLFW_INCLUDE_VULKAN : GLFW inclut les headers Vulkan automatiquement.
# VERCORA_DEBUG : active les validation layers uniquement en configuration Debug.
# GLM_FORCE_RADIANS : GLM travaille en radians (evite les surprises d'unites).
# GLM_FORCE_DEPTH_ZERO_TO_ONE : Vulkan attend une profondeur clip dans [0,1]
#   (et non [-1,1] comme OpenGL) -> indispensable pour un depth buffer correct.
target_compile_definitions(vercora PRIVATE
  GLFW_INCLUDE_VULKAN
  GLM_FORCE_RADIANS
  GLM_FORCE_DEPTH_ZERO_TO_ONE
  $<$<CONFIG:Debug>:VERCORA_DEBUG>
)

# ---------------------------------------------------------------------------
# Compilation automatique des shaders GLSL -> SPIR-V via glslc
# ---------------------------------------------------------------------------
# find_package(Vulkan) fournit normalement Vulkan_GLSLC_EXECUTABLE.
# Fallback manuel si absent.
if(NOT Vulkan_GLSLC_EXECUTABLE)
  find_program(Vulkan_GLSLC_EXECUTABLE
    NAMES glslc
    HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
endif()

if(NOT Vulkan_GLSLC_EXECUTABLE)
  message(FATAL_ERROR "glslc introuvable. Verifiez l'installation du Vulkan SDK (variable VULKAN_SDK).")
endif()

set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BINARY_DIR})

set(SHADER_SOURCES
  cube.vert
  cube.frag
  terrain.vert
  terrain.frag
  foliage.vert
  foliage.frag
  precip.vert
  precip.frag
)

set(SPIRV_BINARIES "")
foreach(SHADER ${SHADER_SOURCES})
  set(SRC ${SHADER_SOURCE_DIR}/${SHADER})
  set(SPV ${SHADER_BINARY_DIR}/${SHADER}.spv)
  add_custom_command(
    OUTPUT  ${SPV}
    COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${SRC} -o ${SPV}
    DEPENDS ${SRC}
    COMMENT "Compilation shader ${SHADER} -> ${SHADER}.spv"
    VERBATIM
  )
  list(APPEND SPIRV_BINARIES ${SPV})
endforeach()

add_custom_target(shaders DEPENDS ${SPIRV_BINARIES})
add_dependencies(vercora shaders)

# Chemin des SPIR-V compiles, passe au code C++ pour le chargement runtime.
target_compile_definitions(vercora PRIVATE
  SHADERS_OUTPUT_DIR="${SHADER_BINARY_DIR}")

# ---------------------------------------------------------------------------
# Assets
# ---------------------------------------------------------------------------
# Meme principe que pour les shaders : le chemin absolu du dossier assets/ est
# passe au code, ce qui evite de dependre du repertoire courant a l'execution.
# A remplacer par une copie des assets a cote de l'executable le jour ou le jeu
# sera distribue.
target_compile_definitions(vercora PRIVATE
  ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
