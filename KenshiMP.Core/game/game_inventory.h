#pragma once
// ES: game_inventory.h - Tipos auxiliares de inventario. La clase InventoryAccessor se declara
//     en game_types.h; aquí solo hay tipos de apoyo usados por el código de inventario.
// EN:
// game_inventory.h - Inventory helper types and implementations.
// The InventoryAccessor class itself is declared in game_types.h.
// This header provides additional utility types used by inventory code.

#include "game_types.h"
#include <cstdint>
#include <vector>

namespace kmp::game {

// ES: Descripción simple de un item: id de plantilla, cantidad y estado (1.0 = nuevo).
// EN: Simple item description: template id, quantity and condition (1.0 = new).
struct InventoryItem {
    uint32_t templateId = 0;
    int      quantity   = 0;
    float    condition  = 1.0f;
};

} // namespace kmp::game
