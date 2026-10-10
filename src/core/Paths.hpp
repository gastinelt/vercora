#pragma once

#include <string>

namespace vercora {

// ---------------------------------------------------------------------------
// Ou l'executable trouve ses donnees (PHASE 21)
// ---------------------------------------------------------------------------
// Jusqu'ici, deux macros definies par CMake - ASSETS_DIR et SHADERS_OUTPUT_DIR -
// portaient des chemins ABSOLUS vers l'arborescence de developpement. C'est
// exactement ce qu'il fallait pour developper (l'executable trouve ses assets
// depuis n'importe quel repertoire courant) et exactement ce qui rend le binaire
// INDISTRIBUABLE : sur une autre machine, ces chemins ne designent rien.
//
// La regle appliquee ici, dans cet ordre :
//   1. le dossier a COTE DE L'EXECUTABLE, s'il existe -> cas du package ;
//   2. sinon la macro CMake -> cas du developpement, comportement inchange.
//
// Les deux cas coexistent donc sans commutateur : un vercora.exe compile ici et
// lance depuis build\ continue de lire assets\ a la racine du depot, et le meme
// binaire copie dans un dossier qui contient assets\ lit celui-la. C'est ce qui
// permet de TESTER le package sans build special (voir NOTES-PHASE21.md).
//
// Les chemins sont resolus une fois et memorises : la resolution appelle le
// systeme et touche le disque, et elle est interrogee a chaque chargement de
// dalle.

// Dossier de l'executable en cours, sans separateur final. Chaine vide si le
// systeme refuse de le donner - les appelants retombent alors sur la macro.
const std::string& exeDir();

// Dossier des donnees ("assets"), sans separateur final.
const std::string& assetsDir();

// Dossier des shaders SPIR-V compiles ("shaders"), sans separateur final.
const std::string& shadersDir();

}  // namespace vercora
