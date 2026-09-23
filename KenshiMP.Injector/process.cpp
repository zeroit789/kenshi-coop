// ES: Implementación de process.h: búsqueda de la DLL del mod y de la instalación de
//     Kenshi, y arranque del juego (Steam o ejecución directa).
// EN: process.h implementation: finding the mod DLL and the Kenshi install,
//     and launching the game (Steam or direct run).
#include "process.h"
#include <Windows.h>
#include <Shlwapi.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <string>

#pragma comment(lib, "shlwapi.lib")

namespace kmp {

// ES: Busca KenshiMP.Core.dll junto al lanzador, un nivel más arriba o en la salida de
//     compilación dentro de la carpeta del juego, y la copia (sobrescribiendo) a la
//     carpeta del juego. Devuelve false si no la encuentra o si la copia falla.
// EN: Looks for KenshiMP.Core.dll next to the launcher, one level up or in the build
//     output inside the game folder, and copies it (overwriting) into the game folder.
//     Returns false if not found or if the copy fails.
bool CopyPluginDll(const std::wstring& gamePath) {
    // ES: Carpeta desde la que se ejecuta el lanzador
    // EN: Get the directory where the injector exe is running from
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // ES: Quitar el nombre de fichero para quedarnos con la carpeta
    // EN: Strip filename to get directory
    std::wstring exeDir(exePath);
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        exeDir = exeDir.substr(0, lastSlash);
    }

    std::wstring srcDll = exeDir + L"\\KenshiMP.Core.dll";
    std::wstring dstDll = gamePath + L"\\KenshiMP.Core.dll";

    // ES: Comprobar que existe el origen, probando varias ubicaciones en orden
    // EN: Check source exists
    if (!PathFileExistsW(srcDll.c_str())) {
        // ES: Probar un nivel más arriba (por si el lanzador está en una subcarpeta)
        // EN: Try one directory up (in case injector is in a subfolder)
        srcDll = exeDir + L"\\..\\KenshiMP.Core.dll";
        if (!PathFileExistsW(srcDll.c_str())) {
            // ES: Probar la salida de compilación relativa al proyecto KenshiMP
            // EN: Try the build output directory relative to KenshiMP project
            srcDll = gamePath + L"\\KenshiMP\\build\\bin\\Release\\KenshiMP.Core.dll";
            if (!PathFileExistsW(srcDll.c_str())) {
                return false;
            }
        }
    }

    return CopyFileW(srcDll.c_str(), dstDll.c_str(), FALSE) != 0;
}

// ES: Localiza Kenshi: primero lee InstallPath de Steam en el registro y mira
//     steamapps\common\Kenshi; si no, prueba la ruta por defecto de Program Files (x86).
//     No revisa otras bibliotecas de Steam (libraryfolders.vdf).
// EN: Locates Kenshi: first reads Steam's InstallPath from the registry and checks
//     steamapps\common\Kenshi; otherwise tries the default Program Files (x86) path.
//     Doesn't scan other Steam libraries (libraryfolders.vdf).
std::wstring FindKenshiPath() {
    // ES: Probar el registro de Steam
    // EN: Try Steam registry
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t steamPath[MAX_PATH] = {};
        DWORD size = sizeof(steamPath);
        if (RegQueryValueExW(hKey, L"InstallPath", nullptr, nullptr,
                            reinterpret_cast<LPBYTE>(steamPath), &size) == ERROR_SUCCESS) {
            // ES: Ojo: aquí se cierra la clave y, si Kenshi no está en la ruta común, se
            //     vuelve a cerrar más abajo (doble RegCloseKey, inofensivo pero redundante).
            // EN: Note: the key is closed here and, if Kenshi isn't at the common path, it is
            //     closed again below (double RegCloseKey, harmless but redundant).
            RegCloseKey(hKey);

            // ES: Comprobar la ubicación habitual de Kenshi
            // EN: Check common Kenshi location
            std::wstring kenshiPath = std::wstring(steamPath) +
                L"\\steamapps\\common\\Kenshi";
            if (PathFileExistsW((kenshiPath + L"\\kenshi_x64.exe").c_str())) {
                return kenshiPath;
            }
        }
        RegCloseKey(hKey);
    }

    // ES: Probar directamente Program Files (x86)
    // EN: Check Program Files (x86) directly
    std::wstring defaultPath = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Kenshi";
    if (PathFileExistsW((defaultPath + L"\\kenshi_x64.exe").c_str())) {
        return defaultPath;
    }

    return L"";
}

// ES: Arranca Kenshi. Primero vía protocolo steam:// (App ID 233860), que gestiona el DRM
//     de Steam; si ShellExecute falla, lanza kenshi_x64.exe directamente con CreateProcess.
// EN: Launches Kenshi. First via the steam:// protocol (App ID 233860), which handles
//     Steam DRM; if ShellExecute fails, runs kenshi_x64.exe directly with CreateProcess.
bool LaunchKenshi(const std::wstring& gamePath) {
    // ES: Método 1: protocolo de Steam (preferido - gestiona el DRM de Steam)
    // EN: Method 1: Launch via Steam protocol (preferred - handles Steam DRM)
    HINSTANCE result = ShellExecuteW(nullptr, L"open",
        L"steam://rungameid/233860", nullptr, nullptr, SW_SHOWNORMAL);

    // ES: ShellExecute devuelve un valor > 32 si tuvo éxito.
    // EN: ShellExecute returns a value > 32 on success.
    if (reinterpret_cast<intptr_t>(result) > 32) {
        return true; // Steam launch successful
    }

    // ES: Método 2: ejecución directa (alternativa)
    // EN: Method 2: Direct launch (fallback)
    std::wstring exePath = gamePath + L"\\kenshi_x64.exe";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (CreateProcessW(exePath.c_str(), nullptr, nullptr, nullptr, FALSE,
                       0, nullptr, gamePath.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    return false;
}

} // namespace kmp
