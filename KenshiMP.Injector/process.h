// ES: Utilidades del lanzador relacionadas con el proceso del juego: localizar la
//     instalación de Kenshi, copiar la DLL del mod y arrancar el juego.
// EN: Launcher helpers related to the game process: locate the Kenshi install,
//     copy the mod DLL and start the game.
#pragma once
#include <string>

namespace kmp {

// ES: Detecta la ruta de instalación de Kenshi a partir del registro de Steam
//     (o la ruta por defecto de Program Files). Devuelve "" si no la encuentra.
// EN: Auto-detect Kenshi installation path from Steam registry
//     (or the default Program Files path). Returns "" if not found.
std::wstring FindKenshiPath();

// ES: Copia KenshiMP.Core.dll desde la carpeta del lanzador a la carpeta del juego.
// EN: Copy KenshiMP.Core.dll from the injector's directory to the Kenshi game directory
bool CopyPluginDll(const std::wstring& gamePath);

// ES: Arranca Kenshi vía Steam o, si falla, ejecutando el .exe directamente.
// EN: Launch Kenshi via Steam or direct executable
bool LaunchKenshi(const std::wstring& gamePath);

} // namespace kmp
