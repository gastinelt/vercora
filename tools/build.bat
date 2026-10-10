@echo off
rem ===========================================================================
rem Build complet de Vercora (configuration + compilation).
rem ===========================================================================
rem CMake et Ninja AUTONOMES + generateur Ninja, volontairement. Raison :
rem
rem   L'installation Visual Studio 18 Community de cette machine est
rem   PARTIELLEMENT CORROMPUE : 1695 fichiers sur 9692 examines sont remplis de
rem   zeros binaires au lieu de leur contenu (constate le 2026-08-15).
rem     - CMake livre avec VS : 1590 fichiers sur 4103 detruits, dont des modules
rem       indispensables comme CMakeDetermineSystem.cmake -> inutilisable.
rem     - Ninja livre avec VS : binaire entierement a zero -> inutilisable.
rem     - MSBuild : 105 fichiers detruits, dont Linux.TargetNames.props, ce qui
rem       fait echouer TOUT projet C++ (MSB4024) et donc le generateur
rem       "Visual Studio 18 2026" des la configuration.
rem
rem   En revanche le COMPILATEUR est intact : 0 fichier corrompu dans
rem   VC\Tools\MSVC\...\bin, \include et \lib. cl.exe, ses en-tetes et ses libs
rem   fonctionnent. C'est pour ca qu'on garde vcvars64 de VS pour l'environnement
rem   de compilation, mais qu'on prend CMake et Ninja ailleurs.
rem
rem   Correctif machine recommande : "Reparer" depuis Visual Studio Installer, et
rem   verifier la sante du disque - une corruption massive et silencieuse de ce
rem   type vient generalement du stockage ou d'un arret brutal.
rem
rem Usage : tools\build.bat [Debug|Release]      (defaut : Debug)
rem
rem Debug est le defaut a dessein : c'est la configuration qui definit
rem VERCORA_DEBUG, donc la seule ou les COUCHES DE VALIDATION Vulkan sont
rem actives (voir CMakeLists.txt et VulkanContext.cpp).
rem ===========================================================================

setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"

set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat"

rem CMake et Ninja autonomes (winget : Kitware.CMake, Ninja-build.Ninja), et NON
rem ceux de Visual Studio, qui sont corrompus sur cette machine - voir l'en-tete.
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "NINJA=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe"

if not exist "%VCVARS%" (
  echo [ERREUR] vcvars64.bat introuvable : "%VCVARS%"
  exit /b 1
)
if not exist "%CMAKE%" (
  echo [ERREUR] cmake.exe introuvable : "%CMAKE%"
  echo          Installer avec : winget install --id Kitware.CMake
  exit /b 1
)
if not exist "%NINJA%" (
  echo [ERREUR] ninja.exe introuvable : "%NINJA%"
  echo          Installer avec : winget install --id Ninja-build.Ninja
  exit /b 1
)

call "%VCVARS%" >nul 2>&1

rem --- Prechauffage de rc.exe --------------------------------------------
rem Sur cette machine, la PREMIERE execution de rc.exe (compilateur de
rem ressources du Windows SDK) apres un moment d'inactivite echoue avec le code
rem -1 et AUCUNE sortie ; les suivantes passent. Mesure : 1 echec suivi de 7
rem succes sur 8 essais consecutifs. C'est la signature d'un antivirus temps
rem reel qui bloque la creation du processus le temps d'analyser le binaire.
rem
rem CMake appelle rc.exe pour embarquer le manifeste a chaque edition de liens :
rem sans ce prechauffage, la configuration ou le build echouent au hasard, avec
rem un message trompeur du genre "le compilateur ne peut pas compiler un
rem programme simple".
rem
rem Correctif machine (a faire par l'utilisateur, cote securite - pas par ce
rem script) : ajouter une exclusion Windows Defender sur le dossier du Windows
rem Kit, ou sur le dossier de build.
rem Plusieurs appels, et pas un seul : un prechauffage unique s'est revele
rem insuffisant (une configuration propre a encore echoue une fois sur deux).
rem Ces appels sont instantanes et leur code de retour est sans importance - le
rem but est uniquement que le binaire soit deja analyse et en cache quand CMake
rem s'en servira pour de bon.
for %%R in ("%WindowsSdkVerBinPath%x64\rc.exe") do if exist %%R (
  %%R /? >nul 2>&1
  %%R /? >nul 2>&1
  %%R /? >nul 2>&1
)

rem --- Vulkan SDK ------------------------------------------------------------
rem find_package(Vulkan) s'appuie sur la variable VULKAN_SDK. L'installateur
rem LunarG la pose au niveau MACHINE, mais un shell deja ouvert avant
rem l'installation ne la voit pas. On la recupere donc explicitement : d'abord
rem l'environnement, puis le registre, puis le dossier d'installation par defaut.
if not defined VULKAN_SDK (
  for /f "tokens=2,*" %%A in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v VULKAN_SDK 2^>nul ^| find "VULKAN_SDK"') do set "VULKAN_SDK=%%B"
)
if not defined VULKAN_SDK (
  for /f "delims=" %%D in ('dir /b /ad /o-n "C:\VulkanSDK" 2^>nul') do (
    if not defined VULKAN_SDK set "VULKAN_SDK=C:\VulkanSDK\%%D"
  )
)
if not defined VULKAN_SDK (
  echo [ERREUR] SDK Vulkan introuvable. Installer avec :
  echo          winget install --id KhronosGroup.VulkanSDK
  exit /b 1
)
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
  echo [ERREUR] VULKAN_SDK pointe sur "%VULKAN_SDK%" mais Include\vulkan\vulkan.h y est absent.
  exit /b 1
)
echo Vulkan SDK : %VULKAN_SDK%

set "ROOT=%~dp0.."
pushd "%ROOT%"

rem FETCHCONTENT_FULLY_DISCONNECTED=ON : interdit tout acces reseau. GLFW, GLM et
rem tinyobjloader sont deja en cache dans external\, il n'y a rien a
rem retelecharger. Si cette option fait echouer la configuration, c'est que le
rem cache est reellement inutilisable - et seulement dans ce cas il faudra la
rem retirer pour laisser FetchContent refaire son travail.
call :configure
if errorlevel 1 (
  rem Une seule reprise, et uniquement pour ce cas precis : l'antivirus peut
  rem encore bloquer rc.exe pendant le test du compilateur, ce qui fait echouer
  rem la configuration avec un message trompeur ("le compilateur ne peut pas
  rem compiler un programme simple"). Une vraie erreur de projet, elle,
  rem echouerait aussi a la seconde tentative.
  echo.
  echo [ATTENTION] Configuration echouee, nouvelle tentative ^(rc.exe est
  echo             probablement encore analyse par l'antivirus^)...
  rmdir /s /q build 2>nul
  call :configure
  if errorlevel 1 (
    echo.
    echo [ECHEC] Configuration CMake ^(deux tentatives^).
    popd
    exit /b 1
  )
)

"%CMAKE%" --build build
if errorlevel 1 (
  echo.
  echo [ECHEC] Compilation.
  popd
  exit /b 1
)

echo.
echo [OK] Build %CONFIG% termine : %ROOT%\build\vercora.exe
popd
exit /b 0

:configure
"%CMAKE%" -S . -B build -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_BUILD_TYPE=%CONFIG% ^
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON
exit /b %ERRORLEVEL%
