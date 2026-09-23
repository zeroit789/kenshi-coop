// ES: Implementación (esqueleto) de los hooks de recursos de Ogre3D. La idea era
//     hookear por vtable los métodos load() de los singletons de Ogre para saber
//     cuándo termina de cargar el juego. El descubrimiento no está implementado:
//     TryDiscoverOgreManagers() siempre devuelve false y Install() deja el módulo
//     inactivo. Corre en el hilo que llame a Install() (arranque del plugin).
// EN: Skeleton implementation of the Ogre3D resource hooks. The idea was to hook,
//     via vtable, the load() methods of Ogre's singletons to know when the game has
//     finished loading. Discovery is not implemented: TryDiscoverOgreManagers()
//     always returns false and Install() leaves the module inactive. Runs on whatever
//     thread calls Install() (plugin startup).
#include "resource_hooks.h"
#include "../core.h"
#include "../game/loading_orchestrator.h"
#include "../game/asset_facilitator.h"
#include "kmp/hook_manager.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <Windows.h>

namespace kmp::resource_hooks {

// ES: Estado: true si se llegaron a instalar hooks de Ogre (hoy nunca).
// EN: State: true if Ogre hooks were actually installed (never, today).
// ── State ──
static bool s_active = false;

// ES: HOOKING POR VTABLE DE LOS RESOURCE MANAGERS DE OGRE
//     Kenshi usa Ogre3D 1.x. La carga de recursos pasa por MeshManager::load,
//     TextureManager::load y MaterialManager::load, métodos virtuales de objetos
//     singleton. El plan era encontrar esos singletons buscando cadenas de Ogre en
//     .rdata, sus referencias (xrefs) en .text y el patrón de acceso al singleton.
//     Si falla, el LoadingOrchestrator vuelve a la temporización por ráfagas.
// EN: See the English block below.
// ═══════════════════════════════════════════════════════════════════════════
// OGRE RESOURCE MANAGER VTable HOOKING
// ═══════════════════════════════════════════════════════════════════════════
//
// Kenshi uses Ogre3D 1.x. Resource loading goes through:
//   - Ogre::MeshManager::load(name, group, ...)
//   - Ogre::TextureManager::load(name, group, ...)
//   - Ogre::MaterialManager::load(name, group, ...)
//
// These are virtual methods on singleton manager objects. We discover the
// singletons by scanning for known Ogre strings in .rdata, finding their
// xrefs in .text, and walking back to the singleton access pattern.
//
// If discovery fails, the LoadingOrchestrator gracefully degrades to
// burst-detection timing (the existing behavior before this system).
// ═══════════════════════════════════════════════════════════════════════════

// ES: Ogre::ResourceManager::load suele estar en el índice 5-8 de la vtable (sin
//     verificar en Kenshi). El índice se detectaría por la firma: RCX = this
//     (ResourceManager*), RDX = const String& name, devuelve un ResourcePtr.
// EN: Ogre::ResourceManager::load is typically at VTable index 5-8.
// We detect the correct index by looking for the pattern of:
//   - First param (RCX) = this (ResourceManager*)
//   - Second param (RDX) = const String& name
//   - Returns ResourcePtr

// ES: Hueco para el futuro descubrimiento de vtables. Haría falta: 1) localizar
//     OgreMain.dll o el código de Ogre enlazado estáticamente en kenshi_x64.exe,
//     2) localizar MeshManager::getSingletonPtr() o el global del singleton, 3) leer
//     el puntero a la vtable del singleton y 4) instalar los hooks en los índices
//     correctos. Es trabajo de ingeniería inversa con el juego en marcha; de momento
//     solo existe el armazón y se degrada con gracia.
// EN: Placeholder for future VTable discovery.
// The actual implementation requires:
//   1. Finding OgreMain.dll or the static Ogre code in kenshi_x64.exe
//   2. Locating MeshManager::getSingletonPtr() or the singleton global
//   3. Reading the VTable pointer from the singleton
//   4. Installing VTable hooks at the correct indices
//
// This is non-trivial RE work that should be done with the game running
// (runtime VTable dump + method call tracing). For now, we provide the
// framework and gracefully degrade.

// ES: Intenta localizar los managers de Ogre. Hoy solo registra en el log si existe
//     OgreMain.dll o la base de kenshi_x64.exe y devuelve false siempre.
// EN: Tries to locate the Ogre managers. Today it only logs whether OgreMain.dll
//     exists or the kenshi_x64.exe base, and always returns false.
static bool TryDiscoverOgreManagers() {
    // ES: Paso 1: ¿está cargada OgreMain.dll? (algunas versiones enlazan Ogre dinámicamente)
    // EN: Step 1: Check if OgreMain.dll is loaded (some Kenshi versions link dynamically)
    HMODULE hOgre = GetModuleHandleA("OgreMain.dll");
    if (hOgre) {
        spdlog::info("resource_hooks: Found OgreMain.dll at 0x{:X}",
                     reinterpret_cast<uintptr_t>(hOgre));
        // TODO: Use GetProcAddress for Ogre::MeshManager::getSingletonPtr()
        // and hook via VTable from there.
        return false; // Not yet implemented
    }

    // ES: Paso 2: Ogre enlazado estáticamente; habría que buscar el patrón del singleton
    //     dentro de kenshi_x64.exe (buscar la cadena "MeshManager" en .rdata y sus xrefs
    //     en .text). No implementado: solo se registra la base del módulo y se devuelve false.
    // EN: Step 2: Ogre is statically linked — scan for singleton patterns in kenshi_x64.exe
    HMODULE hGame = GetModuleHandleA("kenshi_x64.exe");
    if (!hGame) {
        hGame = GetModuleHandleA(nullptr);
    }
    uintptr_t gameBase = reinterpret_cast<uintptr_t>(hGame);

    // Scan .rdata for "MeshManager" string
    // Then find xrefs to it in .text to locate the singleton accessor
    // This requires the PatternScanner infrastructure with PE section enumeration.
    //
    // For now, log and return false — the infrastructure is ready but the
    // Ogre VTable indices need to be determined through runtime analysis.
    spdlog::info("resource_hooks: Game base at 0x{:X} — Ogre VTable discovery not yet implemented",
                 gameBase);

    return false;
}

// ES: Lanza el descubrimiento y actualiza s_active según el resultado (ver .h).
// EN: Runs discovery and updates s_active according to the result (see .h).
bool Install() {
    spdlog::info("resource_hooks: Attempting Ogre resource manager discovery...");

    bool discovered = TryDiscoverOgreManagers();
    if (discovered) {
        s_active = true;
        spdlog::info("resource_hooks: Ogre resource hooks installed successfully");
    } else {
        s_active = false;
        spdlog::info("resource_hooks: Ogre discovery failed — graceful degradation "
                     "(LoadingOrchestrator will use burst-detection timing)");
    }

    return s_active;
}

// ES: Si el módulo estaba activo, retira los hooks "OgreMeshLoad"/"OgreTextureLoad".
// EN: If the module was active, removes the "OgreMeshLoad"/"OgreTextureLoad" hooks.
void Uninstall() {
    if (s_active) {
        // ES: Quitar los hooks de vtable si se instaló alguno.
        // EN: Remove VTable hooks if any were installed
        auto& hookMgr = HookManager::Get();
        if (hookMgr.IsInstalled("OgreMeshLoad")) {
            hookMgr.Remove("OgreMeshLoad");
        }
        if (hookMgr.IsInstalled("OgreTextureLoad")) {
            hookMgr.Remove("OgreTextureLoad");
        }
        s_active = false;
        spdlog::info("resource_hooks: Uninstalled");
    }
}

// ES: Devuelve s_active.
// EN: Returns s_active.
bool IsActive() {
    return s_active;
}

} // namespace kmp::resource_hooks
