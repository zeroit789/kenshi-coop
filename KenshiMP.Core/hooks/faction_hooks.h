// ES: Interfaz del módulo de hooks de relaciones entre facciones. El detour previsto
//     interceptaba "FactionRelation" (RVA 0x872E00) para enviar C2S_FactionRelation,
//     pero hoy Install() es un NO-OP: esa RVA resultó ser una función de log, no el
//     setter de relaciones (ver faction_hooks.cpp).
// EN: Interface of the faction relation hooks module. The planned detour intercepted
//     "FactionRelation" (RVA 0x872E00) to send C2S_FactionRelation, but Install() is
//     currently a NO-OP: that RVA turned out to be a logging function, not the
//     relation setter (see faction_hooks.cpp).
#pragma once
#include <cstdint>

// ES: Espacio de nombres de los hooks de facciones.
// EN: Namespace for the faction hooks.
namespace kmp::faction_hooks {

// ES: No instala nada (decisión de diseño); devuelve true.
// EN: Installs nothing (design decision); returns true.
bool Install();
// ES: Quita el hook "FactionRelation" si llegó a instalarse y limpia el trampolín.
// EN: Removes the "FactionRelation" hook if it was installed and clears the trampoline.
void Uninstall();

// ES: Suprime los envíos de red mientras se carga una partida.
// EN: Suppress network sends during loading
void SetLoading(bool loading);

// ES: Suprime los envíos mientras se aplica un cambio de facción que viene del servidor
//     (evita el bucle de realimentación al aplicar un S2C_FactionRelation entrante).
// EN: Suppress network sends during server-sourced faction changes
// (prevents feedback loop when applying incoming S2C_FactionRelation)
void SetServerSourced(bool sourced);

// ES: Devuelve el puntero a la función original FactionRelation (para llamarla desde
//     packet_handler). Como el hook no se instala, hoy devuelve nullptr: quien lo use
//     debe comprobarlo.
// EN: Get the original FactionRelation function pointer (for calling from packet_handler).
//     Since the hook is not installed, it currently returns nullptr: callers must check it.
using FactionRelationFn = void(__fastcall*)(void* factionA, void* factionB, float relation);
FactionRelationFn GetOriginal();

} // namespace kmp::faction_hooks
