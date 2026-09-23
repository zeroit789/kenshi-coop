// ES: Implementación de los hooks de guardado/carga. Los detours Hook_SaveGame y
//     Hook_LoadGame existen pero NO se instalan (Install() solo loguea): las firmas
//     de las funciones del SaveManager no están verificadas y llamar por un trampolín
//     incorrecto crasheaba al cargar. Los guardados locales pasan sin tocar; el estado
//     multijugador lo pone el servidor por encima con su snapshot.
// EN: Implementation of the save/load hooks. The Hook_SaveGame and Hook_LoadGame
//     detours exist but are NOT installed (Install() only logs): the SaveManager
//     function signatures are unverified and calling through a wrong trampoline crashed
//     during load. Local saves pass through untouched; multiplayer state is layered on
//     top by the server via its snapshot.
#include "save_hooks.h"
#include "../core.h"
#include "kmp/hook_manager.h"
#include "kmp/protocol.h"
#include <spdlog/spdlog.h>

namespace kmp::save_hooks {

// ES: Firmas supuestas (sin verificar) de las funciones de guardar/cargar del SaveManager
//     de Kenshi: (this = saveManager, nombre de la partida).
// EN: Assumed (unverified) signatures of Kenshi's SaveManager save/load functions:
//     (this = saveManager, save name).
using SaveGameFn = void(__fastcall*)(void* saveManager, const char* saveName);
using LoadGameFn = void(__fastcall*)(void* saveManager, const char* saveName);

// ES: Trampolines a las funciones originales (nullptr mientras no se instalen los hooks).
// EN: Trampolines to the original functions (nullptr while the hooks are not installed).
static SaveGameFn s_origSave = nullptr;
static LoadGameFn s_origLoad = nullptr;
// ES: Bandera "cargando partida", atómica porque la leen hooks de otros hilos.
// EN: "Loading save" flag, atomic because hooks on other threads read it.
static std::atomic<bool> s_loading{false};

// ES: Devuelve la bandera de carga (ver save_hooks.h).
// EN: Returns the loading flag (see save_hooks.h).
bool IsLoading() { return s_loading.load(); }

// ES: Detour de SaveGame (no instalado). Siempre deja guardar en local y, si hay
//     conexión, solo anota en el log que el estado del servidor va aparte.
// EN: SaveGame detour (not installed). Always lets the local save happen and, when
//     connected, only logs that the server state is separate.
static void __fastcall Hook_SaveGame(void* saveManager, const char* saveName) {
    auto& core = Core::Get();

    // ES: Permitir siempre el guardado local. En multijugador actúa como punto de
    //     control de los personajes propios; el servidor persiste aparte el estado
    //     autoritativo del mundo.
    // EN: Always allow local saves. In multiplayer the local save acts as a
    // checkpoint for the player's own characters; the server independently
    // persists the authoritative world state.
    s_origSave(saveManager, saveName);

    if (core.IsConnected()) {
        spdlog::info("save_hooks: Local save '{}' completed (server state is separate)",
                     saveName ? saveName : "unnamed");
    }
}

// ES: Detour de LoadGame (no instalado). Envuelve la carga original activando la
//     bandera s_loading antes y desactivándola después.
// EN: LoadGame detour (not installed). Wraps the original load, raising the
//     s_loading flag before and clearing it afterwards.
static void __fastcall Hook_LoadGame(void* saveManager, const char* saveName) {
    // ES: Activa la bandera de carga para que los hooks de entidades/combate no hagan
    //     operaciones de red durante la carga (los personajes aún no están inicializados).
    // EN: Set loading flag so entity/combat hooks skip network operations
    // during save load (characters aren't fully initialized yet).
    s_loading = true;
    spdlog::info("save_hooks: Loading local save (loading guard ON)");
    s_origLoad(saveManager, saveName);
    s_loading = false;
    spdlog::info("save_hooks: Save load complete (loading guard OFF)");
}

// ES: No engancha nada (ver cabecera del fichero); solo lo registra en el log.
// EN: Hooks nothing (see file header); it only logs.
bool Install() {
    // ES: Hooks de guardar/cargar desactivados: las firmas no están del todo verificadas
    //     y llamar por trampolines malos crashea al cargar. Guardar y cargar en local pasa
    //     sin modificar, que es lo deseado: cada jugador conserva sus partidas y el estado
    //     multijugador se superpone con el snapshot del servidor.
    // EN: Save/Load hooks are disabled — the function signatures are not fully
    // verified and calling through bad trampolines crashes during save load.
    // Local saves and loads pass through unmodified, which is the desired
    // behavior: players keep their own save files, multiplayer state is
    // layered on top via the server snapshot.
    spdlog::info("save_hooks: Skipped (pass-through mode)");
    return true;
}

// ES: Retira los hooks por nombre (inofensivo si nunca se instalaron).
// EN: Removes the hooks by name (harmless if they were never installed).
void Uninstall() {
    HookManager::Get().Remove("SaveGame");
    HookManager::Get().Remove("LoadGame");
}

} // namespace kmp::save_hooks
