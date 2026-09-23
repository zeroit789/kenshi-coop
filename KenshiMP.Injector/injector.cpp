// ES: Implementación de injector.h: edición de Plugins_x64.cfg, escritura de client.json
//     e instalación del fichero kenshi-online.mod en data/ y mods/.
// EN: injector.h implementation: editing Plugins_x64.cfg, writing client.json
//     and installing kenshi-online.mod into data/ and mods/.
#include "injector.h"
#include <Windows.h>
#include <ShlObj.h>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace kmp {

// ES: Línea que Ogre lee en Plugins_x64.cfg para cargar KenshiMP.Core.dll como plugin.
// EN: Line Ogre reads in Plugins_x64.cfg to load KenshiMP.Core.dll as a plugin.
static const char* PLUGIN_LINE = "Plugin=KenshiMP.Core";

// ES: Añade PLUGIN_LINE al final de Plugins_x64.cfg si no está. Reescribe el fichero con
//     finales de línea LF. Devuelve false si el fichero no existe o no se puede escribir.
// EN: Appends PLUGIN_LINE to Plugins_x64.cfg if missing. Rewrites the file with LF line
//     endings. Returns false if the file doesn't exist or can't be written.
bool InstallOgrePlugin(const std::wstring& gamePath) {
    std::wstring cfgPath = gamePath + L"\\Plugins_x64.cfg";

    // ES: Leer la configuración existente
    // EN: Read existing config
    std::ifstream inFile(cfgPath);
    if (!inFile.is_open()) return false;

    std::vector<std::string> lines;
    std::string line;
    bool alreadyInstalled = false;

    while (std::getline(inFile, line)) {
        // ES: Quitar el CR final si lo hay
        // EN: Remove trailing CR if present
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == PLUGIN_LINE) {
            alreadyInstalled = true;
        }
        lines.push_back(line);
    }
    inFile.close();

    if (alreadyInstalled) return true; // Already installed

    // ES: Añadir nuestra línea de plugin
    // EN: Add our plugin line
    lines.push_back(PLUGIN_LINE);

    // ES: Volver a escribir el fichero
    // EN: Write back
    std::ofstream outFile(cfgPath);
    if (!outFile.is_open()) return false;

    for (size_t i = 0; i < lines.size(); i++) {
        outFile << lines[i];
        if (i + 1 < lines.size()) outFile << "\n";
    }
    outFile.close();

    return true;
}

// ES: Reescribe Plugins_x64.cfg sin la línea PLUGIN_LINE. No se usa desde la UI del
//     lanzador (solo existe como API de desinstalación).
// EN: Rewrites Plugins_x64.cfg without PLUGIN_LINE. Not used by the launcher UI
//     (exists only as an uninstall API).
bool RemoveOgrePlugin(const std::wstring& gamePath) {
    std::wstring cfgPath = gamePath + L"\\Plugins_x64.cfg";

    std::ifstream inFile(cfgPath);
    if (!inFile.is_open()) return false;

    std::vector<std::string> lines;
    std::string line;

    // ES: Copiar todas las líneas menos la nuestra
    // EN: Copy every line except ours
    while (std::getline(inFile, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line != PLUGIN_LINE) {
            lines.push_back(line);
        }
    }
    inFile.close();

    std::ofstream outFile(cfgPath);
    if (!outFile.is_open()) return false;

    for (size_t i = 0; i < lines.size(); i++) {
        outFile << lines[i];
        if (i + 1 < lines.size()) outFile << "\n";
    }
    outFile.close();

    return true;
}

// ES: Sobrescribe %APPDATA%\KenshiMP\client.json con nombre, servidor, puerto,
//     autoConnect=true y overlayScale=1. Ojo: al reescribir el fichero entero se pierden
//     los demás campos de ClientConfig (favoritos, master server, useSyncOrchestrator),
//     que vuelven a sus valores por defecto al cargarlo.
// EN: Overwrites %APPDATA%\KenshiMP\client.json with name, server, port,
//     autoConnect=true and overlayScale=1. Note: rewriting the whole file drops the other
//     ClientConfig fields (favorites, master server, useSyncOrchestrator), which fall back
//     to their defaults when loaded.
bool WriteConnectConfig(const char* address, const char* port, const char* playerName) {
    // ES: Escribe en client.json — el mismo fichero que KenshiMP.Core lee vía ClientConfig
    // EN: Write to client.json — the same file that KenshiMP.Core reads via ClientConfig
    char appData[MAX_PATH];
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        return false;
    }

    std::string dir = std::string(appData) + "\\KenshiMP";
    CreateDirectoryA(dir.c_str(), nullptr);

    std::string path = dir + "\\client.json";

    // ES: Convertir el puerto a entero (27800 si no es un número válido)
    // EN: Parse port as integer
    int portNum = 27800;
    try { portNum = std::stoi(port); } catch (...) {}

    // ES: Construir el JSON con nlohmann::json para que las cadenas se escapen bien
    //     (comillas, barras invertidas y caracteres de control en playerName/address)
    // EN: Build JSON via nlohmann::json so strings are properly escaped
    //     (handles quotes, backslashes, control chars in playerName/address)
    nlohmann::json j;
    j["playerName"]  = std::string(playerName);
    j["lastServer"]  = std::string(address);
    j["lastPort"]    = portNum;
    j["autoConnect"] = true;
    j["overlayScale"] = 1.0;

    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << j.dump(2) << "\n";

    return true;
}

// ES: Busca kenshi-online.mod, lo copia a <juego>\data y a <juego>\mods\kenshi-online y
//     añade "kenshi-online" a data\__mods.list para que el juego lo active.
//     Devuelve false solo si no encuentra el .mod (no es fatal para el lanzador).
// EN: Finds kenshi-online.mod, copies it into <game>\data and <game>\mods\kenshi-online and
//     adds "kenshi-online" to data\__mods.list so the game enables it.
//     Returns false only if the .mod isn't found (not fatal for the launcher).
bool InstallModFile(const std::wstring& gamePath) {
    // ES: Localizar el fichero fuente kenshi-online.mod.
    //     Orden de búsqueda: junto al lanzador, un nivel arriba, ..\data, raíz KenshiMP en el juego.
    // EN: Find the kenshi-online.mod source file.
    //     Search order: next to injector, one dir up, build output, KenshiMP root.
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);

    std::wstring candidates[] = {
        exeDir + L"\\kenshi-online.mod",
        exeDir + L"\\..\\kenshi-online.mod",
        exeDir + L"\\..\\data\\kenshi-online.mod",
        gamePath + L"\\KenshiMP\\kenshi-online.mod",
    };

    std::wstring srcPath;
    for (auto& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            srcPath = c;
            break;
        }
    }

    if (srcPath.empty()) {
        // ES: No se encontró el .mod — no es fatal, el sistema de spawn tiene alternativas
        // EN: Mod file not found — not fatal, spawn system will fall back
        return false;
    }

    // ES: Copiar a data/ (se carga siempre junto a gamedata.base). El resultado de la copia no se comprueba.
    // EN: Copy to data/ directory (always loaded alongside gamedata.base)
    std::wstring dataPath = gamePath + L"\\data\\kenshi-online.mod";
    CopyFileW(srcPath.c_str(), dataPath.c_str(), FALSE);

    // ES: Copiar también a mods/kenshi-online/ (ubicación estándar de mods)
    // EN: Also copy to mods/kenshi-online/ directory (standard mod location)
    std::wstring modsDir = gamePath + L"\\mods";
    CreateDirectoryW(modsDir.c_str(), nullptr);
    std::wstring modSubDir = modsDir + L"\\kenshi-online";
    CreateDirectoryW(modSubDir.c_str(), nullptr);
    std::wstring modsDstPath = modSubDir + L"\\kenshi-online.mod";
    CopyFileW(srcPath.c_str(), modsDstPath.c_str(), FALSE);

    // ES: Añadir "kenshi-online" a __mods.list (lista de mods activos) si aún no está
    // EN: Add "kenshi-online" to __mods.list if not already present
    std::wstring modsListPath = gamePath + L"\\data\\__mods.list";
    std::ifstream modsListIn(modsListPath);
    std::vector<std::string> modLines;
    std::string modLine;
    bool alreadyListed = false;
    while (std::getline(modsListIn, modLine)) {
        if (!modLine.empty() && modLine.back() == '\r') modLine.pop_back();
        if (modLine == "kenshi-online") alreadyListed = true;
        modLines.push_back(modLine);
    }
    modsListIn.close();

    if (!alreadyListed) {
        modLines.push_back("kenshi-online");
        std::ofstream modsListOut(modsListPath);
        for (size_t i = 0; i < modLines.size(); i++) {
            modsListOut << modLines[i];
            if (i + 1 < modLines.size()) modsListOut << "\n";
        }
        modsListOut.close();
    }

    return true;
}

} // namespace kmp
