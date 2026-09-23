// ES: dllmain.cpp — Punto de entrada de KenshiMP.Core.dll.
//     Kenshi carga la DLL como plugin de Ogre (línea "Plugin=KenshiMP.Core" en Plugins_x64.cfg)
//     y Ogre llama a los exports dllStartPlugin/dllStopPlugin, que arrancan y paran el singleton
//     kmp::Core (todo el mod cuelga de ahí). DllMain solo hace limpieza de emergencia.
// EN: dllmain.cpp — Entry point of KenshiMP.Core.dll.
//     Kenshi loads the DLL as an Ogre plugin ("Plugin=KenshiMP.Core" line in Plugins_x64.cfg)
//     and Ogre calls the dllStartPlugin/dllStopPlugin exports, which start and stop the
//     kmp::Core singleton (the whole mod hangs off it). DllMain only does emergency cleanup.
#include "core.h"
#include <Windows.h>

// ES: ── Interfaz de plugin de Ogre ──
//     Kenshi carga los plugins de Ogre desde Plugins_x64.cfg; al añadir "Plugin=KenshiMP.Core"
//     Ogre invoca estos exports.
// ── Ogre Plugin Interface ──
// Kenshi loads Ogre plugins via Plugins_x64.cfg.
// By adding "Plugin=KenshiMP.Core" to that file, Ogre calls these exports.

// ES: Puntero al singleton Core mientras el plugin está arrancado (nullptr cuando está parado).
// EN: Pointer to the Core singleton while the plugin is running (nullptr when stopped).
static kmp::Core* g_core = nullptr;

extern "C" {

// ES: Export que Ogre llama al cargar el plugin: obtiene el singleton y lanza Core::Initialize()
//     (logging, escáner de patrones, hooks, red, UI e hilo de red). Corre en el hilo de arranque de Ogre.
// EN: Export called by Ogre when the plugin is loaded: gets the singleton and runs Core::Initialize()
//     (logging, pattern scanner, hooks, network, UI and network thread). Runs on Ogre's startup thread.
__declspec(dllexport) void dllStartPlugin() {
    g_core = &kmp::Core::Get();
    g_core->Initialize();
}

// ES: Export que Ogre llama al descargar el plugin: apaga Core (para el hilo de red, quita hooks,
//     guarda config) y olvida el puntero para que no se apague dos veces.
// EN: Export called by Ogre when the plugin is unloaded: shuts Core down (stops the network thread,
//     removes hooks, saves config) and forgets the pointer so it is not shut down twice.
__declspec(dllexport) void dllStopPlugin() {
    if (g_core) {
        g_core->Shutdown();
        g_core = nullptr;
    }
}

} // extern "C"

// ES: DllMain estándar de Windows. En ATTACH desactiva las notificaciones por hilo (no las usamos);
//     en DETACH apaga Core como red de seguridad por si Ogre no llamó a dllStopPlugin.
// EN: Standard Windows DllMain. On ATTACH it disables per-thread notifications (unused);
//     on DETACH it shuts Core down as a safety net in case Ogre did not call dllStopPlugin.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        // ES: Ogre debería haber llamado a dllStopPlugin, pero por si acaso.
        // Ogre should call dllStopPlugin, but just in case
        if (g_core) {
            g_core->Shutdown();
            g_core = nullptr;
        }
        break;
    }
    return TRUE;
}
