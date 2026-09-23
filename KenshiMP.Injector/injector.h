// ES: API de instalación del lanzador (Injector). Pese al nombre, no inyecta ninguna DLL
//     en memoria: registra KenshiMP.Core como plugin de Ogre en Plugins_x64.cfg (Kenshi
//     lo carga solo al arrancar), instala el fichero .mod del multijugador y escribe la
//     configuración de conexión que luego lee el Core.
// EN: Launcher (Injector) installation API. Despite the name, it doesn't inject any DLL
//     into memory: it registers KenshiMP.Core as an Ogre plugin in Plugins_x64.cfg (Kenshi
//     loads it by itself at startup), installs the multiplayer .mod file and writes the
//     connection config that Core reads later.
#pragma once
#include <string>

namespace kmp {

// ES: Añade la línea "Plugin=KenshiMP.Core" a Plugins_x64.cfg (si no estaba ya).
// EN: Install "Plugin=KenshiMP.Core" into Plugins_x64.cfg
bool InstallOgrePlugin(const std::wstring& gamePath);

// ES: Quita nuestra línea de plugin de Plugins_x64.cfg (desinstalación limpia).
// EN: Remove our plugin line from Plugins_x64.cfg (for clean uninstall)
bool RemoveOgrePlugin(const std::wstring& gamePath);

// ES: Escribe la configuración de conexión. Ojo: el comentario original decía connect.json,
//     pero el código escribe %APPDATA%/KenshiMP/client.json (el fichero de ClientConfig).
// EN: Write connection config to %APPDATA%/KenshiMP/connect.json
//     Note: the code actually writes %APPDATA%/KenshiMP/client.json (the ClientConfig file).
bool WriteConnectConfig(const char* address, const char* port, const char* playerName);

// ES: Instala el .mod del multijugador (kenshi-online.mod) en el directorio data del juego.
//     El .mod define las plantillas de personaje de jugador, facciones y escuadras que el
//     sistema de spawn usa para crear personajes remotos totalmente inicializados.
// EN: Install the multiplayer .mod file (kenshi-online.mod) into the game's data directory.
//     The .mod defines player character templates, factions, and squads that the spawn
//     system uses to create fully-initialized remote player characters.
bool InstallModFile(const std::wstring& gamePath);

} // namespace kmp
