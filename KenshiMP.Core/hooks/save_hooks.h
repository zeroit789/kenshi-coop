// ES: Interfaz del módulo de hooks de guardado/carga de partida (SaveManager de Kenshi).
//     Hoy está DESACTIVADO (modo pass-through): Install() no engancha nada porque las
//     firmas de SaveGame/LoadGame no están verificadas. Expone IsLoading() como puerta
//     de carga para otros módulos.
// EN: Interface of the save/load game hooks module (Kenshi's SaveManager).
//     Currently DISABLED (pass-through mode): Install() hooks nothing because the
//     SaveGame/LoadGame signatures are unverified. Exposes IsLoading() as a loading
//     gate for other modules.
#pragma once
#include <atomic>
// ES: Espacio de nombres de los hooks de guardado/carga.
// EN: Namespace for the save/load hooks.
namespace kmp::save_hooks {
    // ES: No instala hooks (pass-through); siempre devuelve true.
    // EN: Installs no hooks (pass-through); always returns true.
    bool Install();
    // ES: Quita los hooks "SaveGame"/"LoadGame" del HookManager si existieran.
    // EN: Removes the "SaveGame"/"LoadGame" hooks from the HookManager if present.
    void Uninstall();
    // ES: True mientras se carga una partida: los demás hooks deben saltarse las operaciones de red.
    //     Solo lo activa Hook_LoadGame, que hoy no se instala, así que en la práctica devuelve false.
    // EN: True while a save is being loaded — other hooks should skip network operations.
    //     Only Hook_LoadGame sets it, and that hook is not installed today, so in practice it returns false.
    bool IsLoading();
}
