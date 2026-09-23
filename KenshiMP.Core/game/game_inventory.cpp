// ES: game_inventory.cpp - Implementación de InventoryAccessor: leer items del inventario de un
//     personaje/edificio (lista de Item* en +0x10, nº en +0x18) y ajustar pilas ya existentes.
//     No puede crear items nuevos: para eso haría falta el asignador del propio juego.
// EN: game_inventory.cpp - InventoryAccessor implementation: reads items of a character/building
//     inventory (Item* list at +0x10, count at +0x18) and adjusts already existing stacks.
//     It cannot create new items: that would need the game's own allocator.
#include "game_inventory.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>

namespace kmp::game {

// ── InventoryAccessor base method implementations ──
// ES: Métodos básicos de lectura.
// EN: Basic read methods.

// ES: Nº de items del inventario (acotado a 0..9999; 0 si el offset es desconocido).
// EN: Number of items in the inventory (clamped to 0..9999; 0 when the offset is unknown).
int InventoryAccessor::GetItemCount() const {
    if (!IsValid()) return 0;
    auto& offsets = GetOffsets().inventory;
    if (offsets.itemCount < 0) return 0;

    int count = 0;
    Memory::Read(m_ptr + offsets.itemCount, count);
    return (count >= 0 && count < 10000) ? count : 0;
}

// ES: Devuelve el Item* en la posición index del array de punteros (0 si no es válido).
// EN: Returns the Item* at position index of the pointer array (0 if invalid).
uintptr_t InventoryAccessor::GetItem(int index) const {
    if (!IsValid() || index < 0 || index >= GetItemCount()) return 0;
    auto& offsets = GetOffsets().inventory;
    if (offsets.items < 0) return 0;

    uintptr_t listPtr = 0;
    Memory::Read(m_ptr + offsets.items, listPtr);
    if (listPtr == 0) return 0;

    uintptr_t itemPtr = 0;
    Memory::Read(listPtr + index * sizeof(uintptr_t), itemPtr);
    return itemPtr;
}

// ES: Ancho y alto de la rejilla del inventario (en celdas).
// EN: Inventory grid width and height (in cells).
int InventoryAccessor::GetWidth() const {
    if (!IsValid()) return 0;
    auto& offsets = GetOffsets().inventory;
    if (offsets.width < 0) return 0;

    int width = 0;
    Memory::Read(m_ptr + offsets.width, width);
    return width;
}

int InventoryAccessor::GetHeight() const {
    if (!IsValid()) return 0;
    auto& offsets = GetOffsets().inventory;
    if (offsets.height < 0) return 0;

    int height = 0;
    Memory::Read(m_ptr + offsets.height, height);
    return height;
}

// ES: Manipulación de items: busca una pila existente con la misma plantilla y cambia su
//     cantidad (Item+0x12C). Crear objetos Item nuevos requiere el asignador del juego.
// EN:
// ── InventoryAccessor item manipulation ──
// These modify inventory items by finding existing stacks and adjusting quantities.
// Creating entirely new item objects requires the game's allocator, which we don't have.

// ES: Suma 'quantity' a la primera pila cuya plantilla coincida. False si no hay pila.
//     OJO: ItemOffsets.templateId es ahora +0x40 (un GameData*, 8 bytes) pero aquí se lee como
//     uint32 y se compara con un id numérico, así que probablemente nunca coincide (sin verificar).
// EN: Adds 'quantity' to the first stack whose template matches. False if there is no stack.
//     NOTE: ItemOffsets.templateId is now +0x40 (a GameData*, 8 bytes) but it is read here as a
//     uint32 and compared with a numeric id, so it probably never matches (unverified).
bool InventoryAccessor::AddItem(uint32_t templateId, int quantity) {
    if (!IsValid() || quantity <= 0) return false;

    auto& offsets = GetOffsets().inventory;
    int count = GetItemCount();

    uintptr_t listPtr = 0;
    if (offsets.items >= 0) {
        Memory::Read(m_ptr + offsets.items, listPtr);
    }
    if (listPtr == 0) return false;

    auto& itemOffsets = GetOffsets().item;

    for (int i = 0; i < count; i++) {
        uintptr_t itemPtr = 0;
        Memory::Read(listPtr + i * sizeof(uintptr_t), itemPtr);
        if (itemPtr == 0) continue;

        uint32_t existingId = 0;
        if (itemOffsets.templateId >= 0) {
            Memory::Read(itemPtr + itemOffsets.templateId, existingId);
        }

        if (existingId == templateId) {
            int existingQty = 0;
            if (itemOffsets.stackCount >= 0) {
                Memory::Read(itemPtr + itemOffsets.stackCount, existingQty);
                return Memory::Write(itemPtr + itemOffsets.stackCount, existingQty + quantity);
            }
        }
    }
    return false; // Item type not found in inventory
}

// ES: Resta 'quantity' a la primera pila que coincida (sin bajar de 0). Misma limitación que AddItem.
// EN: Subtracts 'quantity' from the first matching stack (not below 0). Same limitation as AddItem.
bool InventoryAccessor::RemoveItem(uint32_t templateId, int quantity) {
    if (!IsValid() || quantity <= 0) return false;

    auto& offsets = GetOffsets().inventory;
    int count = GetItemCount();

    uintptr_t listPtr = 0;
    if (offsets.items >= 0) {
        Memory::Read(m_ptr + offsets.items, listPtr);
    }
    if (listPtr == 0) return false;

    auto& itemOffsets = GetOffsets().item;

    for (int i = 0; i < count; i++) {
        uintptr_t itemPtr = 0;
        Memory::Read(listPtr + i * sizeof(uintptr_t), itemPtr);
        if (itemPtr == 0) continue;

        uint32_t existingId = 0;
        if (itemOffsets.templateId >= 0) {
            Memory::Read(itemPtr + itemOffsets.templateId, existingId);
        }

        if (existingId == templateId) {
            int existingQty = 0;
            if (itemOffsets.stackCount >= 0) {
                Memory::Read(itemPtr + itemOffsets.stackCount, existingQty);
                int newQty = existingQty - quantity;
                if (newQty < 0) newQty = 0;
                return Memory::Write(itemPtr + itemOffsets.stackCount, newQty);
            }
        }
    }
    return false; // Item type not found
}

// ES: Equipa en 'slot' un item ya presente en el inventario escribiendo su Item* en el array de
//     equipo del personaje dueño (owner +0x88). Requiere character.equipment (se sondea en caliente).
// EN: Equips into 'slot' an item already in the inventory by writing its Item* into the owner
//     character's equipment array (owner +0x88). Requires character.equipment (runtime probed).
bool InventoryAccessor::SetEquipment(EquipSlot slot, uint32_t templateId) {
    if (!IsValid()) return false;

    int slotIndex = static_cast<int>(slot);
    if (slotIndex < 0 || slotIndex >= static_cast<int>(EquipSlot::Count)) return false;

    auto& offsets = GetOffsets().character;
    if (offsets.equipment < 0) {
        spdlog::debug("InventoryAccessor::SetEquipment: equipment offset unknown, skipping");
        return false;
    }

    // ES: El array de equipo está en el offset de equipo del personaje; cada ranura es un Item*.
    //     Se busca en el inventario el item con esa plantilla y se escribe su puntero (mejor esfuerzo).
    // EN:
    // Equipment array is at the character's equipment offset.
    // Each slot is a pointer to an item object. We scan existing inventory items
    // to find one matching the templateId and write its pointer to the equipment slot.
    // This is best-effort — if the item isn't in inventory, we can't equip it.
    auto& itemOffsets = GetOffsets().item;
    int count = GetItemCount();
    uintptr_t listPtr = 0;
    if (GetOffsets().inventory.items >= 0) {
        Memory::Read(m_ptr + GetOffsets().inventory.items, listPtr);
    }

    if (listPtr != 0 && count > 0 && itemOffsets.templateId >= 0) {
        for (int i = 0; i < count; i++) {
            uintptr_t itemPtr = 0;
            Memory::Read(listPtr + i * sizeof(uintptr_t), itemPtr);
            if (itemPtr == 0) continue;

            uint32_t existingId = 0;
            Memory::Read(itemPtr + itemOffsets.templateId, existingId);
            if (existingId == templateId) {
                // Found matching item — write its pointer to the equipment slot
                // Equipment is relative to the character, not the inventory
                // We need the character pointer, which is the inventory owner
                uintptr_t ownerPtr = 0;
                if (GetOffsets().inventory.owner >= 0) {
                    Memory::Read(m_ptr + GetOffsets().inventory.owner, ownerPtr);
                }
                if (ownerPtr != 0 && offsets.equipment >= 0) {
                    return Memory::Write(ownerPtr + offsets.equipment + slotIndex * sizeof(uintptr_t), itemPtr);
                }
                break;
            }
        }
    }

    spdlog::debug("InventoryAccessor::SetEquipment slot={} templateId={} — item not found in inventory",
                   slotIndex, templateId);
    return false;
}

} // namespace kmp::game
