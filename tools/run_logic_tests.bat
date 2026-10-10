@echo off
rem ===========================================================================
rem Harnais de tests HEADLESS de la logique de jeu (aucun GPU, aucun Vulkan).
rem ===========================================================================
rem Pourquoi ce script existe alors que le projet a deja un CMakeLists :
rem   find_package(Vulkan REQUIRED) fait echouer la CONFIGURATION du projet
rem   entier quand le SDK Vulkan n'est pas installe sur la machine. Or toute la
rem   logique de jeu (src/world, src/assets, src/core hors Window) ne depend que
rem   de GLM. Ce script la compile donc seule, directement avec cl.exe, ce qui
rem   permet de tester le gameplay meme sans SDK ni carte graphique.
rem
rem Ce n'est PAS un substitut au build normal : il ne verifie ni le rendu, ni les
rem couches de validation Vulkan. Le build de reference reste CMake + cmake
rem --build sur le CMakeLists de la racine.
rem
rem Usage : tools\run_logic_tests.bat
rem Sortie : build-tests\logic_tests.exe (code de retour 0 = tout passe)
rem ===========================================================================

setlocal

rem --- Environnement compilateur ---------------------------------------------
rem VS 18 Community. Adapter ce chemin si l'edition installee change ; le script
rem echoue clairement plutot que de compiler avec un compilateur inattendu.
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo [ERREUR] vcvars64.bat introuvable : "%VCVARS%"
  echo          Adapter VCVARS dans tools\run_logic_tests.bat.
  exit /b 1
)
call "%VCVARS%" >nul 2>&1

rem --- Racine du projet (le script vit dans tools\) ---------------------------
set "ROOT=%~dp0.."
pushd "%ROOT%"

rem GLM est deja en cache dans external\ (recupere par FetchContent lors d'un
rem build CMake anterieur) : aucun telechargement n'est necessaire ici.
set "GLM_DIR=%ROOT%\external\glm-src"
if not exist "%GLM_DIR%\glm\glm.hpp" (
  echo [ERREUR] GLM introuvable dans "%GLM_DIR%".
  echo          Lancer une fois le build CMake complet pour peupler external\.
  popd
  exit /b 1
)

rem GLFW aussi est deja en cache : ses headers permettent de compiler Window.hpp,
rem donc core/Camera.cpp, ou vit la resolution des collisions.
set "GLFW_DIR=%ROOT%\external\glfw-src\include"
if not exist "%GLFW_DIR%\GLFW\glfw3.h" (
  echo [ERREUR] GLFW introuvable dans "%GLFW_DIR%".
  popd
  exit /b 1
)

rem Vulkan SDK : necessaire uniquement aux verifications de syntaxe ci-dessous
rem (Camera.cpp et main.cpp incluent des en-tetes Vulkan). Jusqu'au 2026-08-15 un
rem shim maison sous tests\shim\ remplacait ces declarations, faute de SDK
rem installe ; le SDK etant de retour, on utilise les VRAIS en-tetes et le shim a
rem ete supprime. Les tests d'execution, eux, n'ont jamais eu besoin de Vulkan.
if not defined VULKAN_SDK (
  for /f "tokens=2,*" %%A in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v VULKAN_SDK 2^>nul ^| find "VULKAN_SDK"') do set "VULKAN_SDK=%%B"
)
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
  echo [ERREUR] SDK Vulkan introuvable ^(VULKAN_SDK="%VULKAN_SDK%"^).
  echo          winget install --id KhronosGroup.VulkanSDK
  popd
  exit /b 1
)

if not exist "build-tests" mkdir "build-tests"

rem --- Etape 1 : verification de SYNTAXE -------------------------------------
rem Trois unites qui ne peuvent pas entrer dans l'executable de test mais dont on
rem veut au moins garantir qu'elles compilent (/Zs = syntaxe seule, aucun code
rem genere) :
rem   - Camera.cpp heberge la resolution des collisions, mais inclut Window.hpp
rem     donc GLFW donc <vulkan/vulkan.h>.
rem   - ObjLoader.cpp ne depend plus de Vulkan depuis que le format de sommet a
rem     ete sorti de Vertex.hpp (voir VertexFormat.hpp). Le verifier ici est ce
rem     qui empeche cette separation de se redegrader sans qu'on s'en apercoive.
rem   - main.cpp est le point ou tout le gameplay est cable au Renderer : c'est
rem     le fichier le plus expose a une faute de frappe, et le seul qu'aucun test
rem     d'execution ne couvre.
rem
rem TerrainStreamer.cpp (Phase 10) est verifie en SYNTAXE seulement : il pilote le
rem Renderer, donc Vulkan. Le quadtree lui-meme (TerrainQuadtree.cpp) et le
rem maillage par tuile (TerrainNodeMesh.cpp), eux, ne dependent que de GLM et sont
rem compiles ET executes dans les tests ci-dessous - c'est la meme separation qui
rem avait permis de tester la Phase 7 sans GPU.
rem
rem TerrainMesh.cpp etait ici jusqu'a la Phase 7 ; il est desormais COMPILE ET
rem LIE dans l'executable de test, ou les tests de heightmap l'exercent
rem reellement. Une verification de syntaxe serait redondante et plus faible.
echo [1/2] Verification de syntaxe
cl /nologo /std:c++20 /EHsc /W4 /Zs ^
   /DGLM_FORCE_RADIANS /DGLM_FORCE_DEPTH_ZERO_TO_ONE /DGLFW_INCLUDE_VULKAN ^
   /I "src" /I "%GLM_DIR%" /I "%GLFW_DIR%" /I "%VULKAN_SDK%\Include" ^
   "src\core\Camera.cpp"
if errorlevel 1 goto :syntax_failed

rem PHASE 15 : ObjLoader.cpp etait ici en verification de SYNTAXE seulement. Il
rem est desormais COMPILE ET LIE dans l'executable de test, parce que
rem editor\AssetCatalog.cpp l'appelle pour charger les modeles du dossier
rem d'assets - et que le scan du catalogue est justement l'une des choses que ces
rem tests exercent. Une verification de syntaxe serait redondante et plus faible,
rem exactement comme pour TerrainMesh.cpp depuis la Phase 7.

rem SHADERS_OUTPUT_DIR et ASSETS_DIR sont normalement fournis par CMake. Des
rem valeurs bidon suffisent : on ne verifie ici que la syntaxe, aucun chemin
rem n'est ouvert.
cl /nologo /std:c++20 /EHsc /W4 /Zs ^
   /DGLM_FORCE_RADIANS /DGLM_FORCE_DEPTH_ZERO_TO_ONE /DGLFW_INCLUDE_VULKAN ^
   /I "src" /I "%GLM_DIR%" /I "%GLFW_DIR%" /I "%VULKAN_SDK%\Include" ^
   "src\renderer\TerrainStreamer.cpp"
if errorlevel 1 goto :syntax_failed

rem PHASE 11 : Texture.cpp cree des images Vulkan, donc verification de
rem SYNTAXE seulement, comme TerrainStreamer.cpp juste au-dessus. Le CHARGEUR
rem du format (TextureFile.cpp), lui, ne depend que de la bibliotheque
rem standard : il est compile ET EXECUTE dans les tests plus bas.
cl /nologo /std:c++20 /EHsc /W4 /Zs ^
   /DGLM_FORCE_RADIANS /DGLM_FORCE_DEPTH_ZERO_TO_ONE /DGLFW_INCLUDE_VULKAN ^
   /I "src" /I "%GLM_DIR%" /I "%GLFW_DIR%" /I "%VULKAN_SDK%\Include" ^
   "src\renderer\Texture.cpp"
if errorlevel 1 goto :syntax_failed

cl /nologo /std:c++20 /EHsc /W4 /Zs ^
   /DGLM_FORCE_RADIANS /DGLM_FORCE_DEPTH_ZERO_TO_ONE /DGLFW_INCLUDE_VULKAN ^
   /DSHADERS_OUTPUT_DIR=\"shim\" /DASSETS_DIR=\"shim\" ^
   /I "src" /I "%GLM_DIR%" /I "%GLFW_DIR%" /I "%ROOT%\external\tinyobjloader-src" ^
   /I "%VULKAN_SDK%\Include" ^
   "src\main.cpp"
if errorlevel 1 goto :syntax_failed

goto :syntax_ok

:syntax_failed
echo.
echo [ECHEC] Verification de syntaxe.
popd
exit /b 1

:syntax_ok

rem --- Etape 2 : compilation et execution des tests ---------------------------
echo [2/2] Compilation et execution des tests
rem /W4 : on veut voir les avertissements sur du code neuf.
rem GLM_FORCE_* : memes definitions que la cible vercora, pour que la logique
rem testee soit bit a bit celle qui tournera dans le jeu.
rem ASSETS_DIR : meme definition que la cible CMake, pour que les tests puissent
rem retrouver la dalle LiDAR reelle si elle a ete produite. Son absence n'est pas
rem une erreur - le bloc de tests correspondant s'ignore de lui-meme.
cl /nologo /std:c++20 /EHsc /W4 /Zi ^
   /DGLM_FORCE_RADIANS /DGLM_FORCE_DEPTH_ZERO_TO_ONE ^
   /DASSETS_DIR="\"%ROOT:\=/%/assets\"" ^
   /I "src" /I "%GLM_DIR%" /I "%ROOT%\external\tinyobjloader-src" ^
   /Fo"build-tests\\" /Fd"build-tests\logic_tests.pdb" /Fe"build-tests\logic_tests.exe" ^
   "tests\logic_tests.cpp" ^
   "src\assets\Heightmap.cpp" ^
   "src\assets\TerrainFile.cpp" ^
   "src\renderer\TerrainMesh.cpp" ^
   "src\world\TreeMesh.cpp" ^
   "src\world\StoneMesh.cpp" ^
   "src\world\Tree.cpp" ^
   "src\world\Ground.cpp" ^
   "src\world\Knapping.cpp" ^
   "src\world\Crafting.cpp" ^
   "src\world\CraftMesh.cpp" ^
   "src\world\Forest.cpp" ^
   "src\world\Habitat.cpp" ^
   "src\world\Wildlife.cpp" ^
   "src\world\AnimalMesh.cpp" ^
   "src\world\Paraglider.cpp" ^
   "src\world\Bike.cpp" ^
   "src\world\Climber.cpp" ^
   "src\world\Needs.cpp" ^
   "src\world\Weather.cpp" ^
   "src\world\WaterSource.cpp" ^
   "src\world\Deposits.cpp" ^
   "src\world\Resin.cpp" ^
   "src\world\TreeSpecies.cpp" ^
   "src\world\FiberPlants.cpp" ^
   "src\world\Rocks.cpp" ^
   "src\world\RockMesh.cpp" ^
   "src\world\Bushes.cpp" ^
   "src\world\BushMesh.cpp" ^
   "src\world\HarvestMesh.cpp" ^
   "src\world\TerrainQuadtree.cpp" ^
   "src\world\Vegetation.cpp" ^
   "src\world\GrassMesh.cpp" ^
   "src\assets\TextureFile.cpp" ^
   "src\assets\DetailNoise.cpp" ^
   "src\renderer\TerrainNodeMesh.cpp" ^
   "src\assets\ObjLoader.cpp" ^
   "src\editor\EditorAssets.cpp" ^
   "src\editor\AssetCatalog.cpp" ^
   "src\editor\EditorScene.cpp" ^
   "src\editor\EditorController.cpp" ^
   "src\editor\SceneFile.cpp"

if errorlevel 1 (
  echo.
  echo [ECHEC] Compilation des tests.
  popd
  exit /b 1
)

echo.
"build-tests\logic_tests.exe"
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%


