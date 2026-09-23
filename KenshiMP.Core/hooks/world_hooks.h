// ES: Interfaz de los hooks de mundo/zonas: carga ("ZoneLoad", RVA 0x377710) y descarga
//     ("ZoneUnload", RVA 0x2EF1F0) de celdas de zona del mapa de Kenshi. Los detours solo
//     encolan eventos; el trabajo pesado se hace en ProcessDeferredZoneEvents().
// EN: Interface of the world/zone hooks: load ("ZoneLoad", RVA 0x377710) and unload
//     ("ZoneUnload", RVA 0x2EF1F0) of Kenshi map zone cells. The detours only queue events;
//     the heavy work happens in ProcessDeferredZoneEvents().
#pragma once
// ES: Espacio de nombres de los hooks de zonas.
// EN: Namespace for the zone hooks.
namespace kmp::world_hooks {
    // ES: Instala ZoneLoad/ZoneUnload si se resolvieron; siempre devuelve true.
    // EN: Installs ZoneLoad/ZoneUnload if resolved; always returns true.
    bool Install();
    // ES: Quita ambos hooks de zona.
    // EN: Removes both zone hooks.
    void Uninstall();
    // ES: Procesa los eventos de zona encolados. Se llama desde Core::OnGameTick (contexto seguro).
    // EN: Processes queued zone events. Called from Core::OnGameTick (safe context).
    void ProcessDeferredZoneEvents();
}
