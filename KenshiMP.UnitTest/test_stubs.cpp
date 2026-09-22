// Stubs solo para KenshiMP.UnitTest.
//
// El test compila game_character.cpp directamente, y ese fichero llama a dos funciones
// que viven en unidades del Core que NO se meten en el test (spawn_manager.cpp y
// hooks/ai_hooks.cpp): arrastrarían MinHook, los hooks y el singleton del Core.
// Aquí se dan implementaciones mínimas y deterministas para que el test enlace.

#include "game/spawn_manager.h"
#include "hooks/ai_hooks.h"

#include <string>

namespace kmp {

// En el test no hay memoria real de Kenshi que leer: devolver cadena vacía es lo mismo
// que hace la versión real cuando la lectura protegida por SEH falla.
std::string SpawnManager::ReadKenshiString(uintptr_t /*addr*/) {
    return "";
}

namespace ai_hooks {

// En el test no hay peers conectados, así que ningún personaje está controlado en remoto.
bool IsRemoteControlled(void* /*character*/) {
    return false;
}

} // namespace ai_hooks
} // namespace kmp
