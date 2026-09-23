// ES: game_building.cpp - Implementación de BuildingAccessor: lee campos de un edificio del juego
//     (nombre, posición, salud, construcción, dueño, inventario) con los offsets de BuildingOffsets.
//     OJO: esos offsets son heredados y no están verificados en 1.0.68.
// EN: game_building.cpp - BuildingAccessor implementation: reads fields of a game building
//     (name, position, health, construction, owner, inventory) using BuildingOffsets.
//     NOTE: those offsets are inherited and not verified on 1.0.68.
#include "game_types.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>

namespace kmp::game {

// ES: Los edificios usan el mismo layout de std::string de MSVC que los personajes.
// EN:
// ── BuildingAccessor method implementations ──
// Buildings in Kenshi use the same MSVC std::string layout as characters.

// ES: Lee el nombre (std::string de MSVC: size en +0x10, capacity en +0x18; si capacity > 15
//     el texto está en heap y +0x00 es el puntero; si no, está inline (SSO)). Máx 256 caracteres.
// EN: Reads the name (MSVC std::string: size at +0x10, capacity at +0x18; if capacity > 15 the
//     text lives on the heap and +0x00 is the pointer; otherwise it is inline (SSO)). Max 256 chars.
std::string BuildingAccessor::GetName() const {
    auto& offsets = GetOffsets().building;
    if (offsets.name < 0) return "Unknown Building";

    uintptr_t strAddr = m_ptr + offsets.name;
    uint64_t size = 0, capacity = 0;
    Memory::Read(strAddr + 0x10, size);
    Memory::Read(strAddr + 0x18, capacity);
    if (size == 0 || size > 256) return "Unknown Building";

    char buffer[257] = {};
    if (capacity > 15) {
        uintptr_t dataPtr = 0;
        Memory::Read(strAddr, dataPtr);
        if (dataPtr == 0) return "Unknown Building";
        for (size_t i = 0; i < size && i < 256; i++) {
            Memory::Read(dataPtr + i, buffer[i]);
        }
    } else {
        for (size_t i = 0; i < size && i < 256; i++) {
            Memory::Read(strAddr + i, buffer[i]);
        }
    }
    return std::string(buffer, size);
}

// ES: Posición del edificio en el mundo (Vec3).
// EN: Building world position (Vec3).
Vec3 BuildingAccessor::GetPosition() const {
    Vec3 pos;
    auto& offsets = GetOffsets().building;
    if (offsets.position >= 0) {
        Memory::ReadVec3(m_ptr + offsets.position, pos.x, pos.y, pos.z);
    }
    return pos;
}

// ES: Salud actual y máxima (floats).
// EN: Current and max health (floats).
float BuildingAccessor::GetHealth() const {
    auto& offsets = GetOffsets().building;
    if (offsets.health < 0) return 0.f;

    float hp = 0.f;
    Memory::Read(m_ptr + offsets.health, hp);
    return hp;
}

float BuildingAccessor::GetMaxHealth() const {
    auto& offsets = GetOffsets().building;
    if (offsets.maxHealth < 0) return 0.f;

    float maxHp = 0.f;
    Memory::Read(m_ptr + offsets.maxHealth, maxHp);
    return maxHp;
}

// ES: Flag de destruido; si no hay offset, se deduce de salud <= 0.
// EN: Destroyed flag; without an offset it is derived from health <= 0.
bool BuildingAccessor::IsDestroyed() const {
    auto& offsets = GetOffsets().building;
    if (offsets.isDestroyed < 0) {
        // Fallback: check if health <= 0
        float hp = GetHealth();
        float maxHp = GetMaxHealth();
        return maxHp > 0.f && hp <= 0.f;
    }

    bool destroyed = false;
    Memory::Read(m_ptr + offsets.isDestroyed, destroyed);
    return destroyed;
}

// ES: Progreso de construcción 0-1 (1 si el offset es desconocido).
// EN: Construction progress 0-1 (1 when the offset is unknown).
float BuildingAccessor::GetBuildProgress() const {
    auto& offsets = GetOffsets().building;
    if (offsets.buildProgress < 0) return 1.0f; // Assume complete if unknown

    float progress = 0.f;
    Memory::Read(m_ptr + offsets.buildProgress, progress);
    return progress;
}

// ES: Construido del todo; sin offset se deduce de progreso >= 1.
// EN: Fully constructed; without an offset it is derived from progress >= 1.
bool BuildingAccessor::IsConstructed() const {
    auto& offsets = GetOffsets().building;
    if (offsets.isConstructed < 0) {
        // Fallback: check if build progress >= 1.0
        return GetBuildProgress() >= 1.0f;
    }

    bool constructed = false;
    Memory::Read(m_ptr + offsets.isConstructed, constructed);
    return constructed;
}

// ES: Faction* dueño; solo se devuelve si parece un puntero de usuario válido (si no, 0).
// EN: Owner Faction*; returned only if it looks like a valid user-mode pointer (else 0).
uintptr_t BuildingAccessor::GetOwnerFaction() const {
    auto& offsets = GetOffsets().building;
    if (offsets.ownerFaction < 0) return 0;

    uintptr_t ptr = 0;
    Memory::Read(m_ptr + offsets.ownerFaction, ptr);
    return (ptr > 0x10000 && ptr < 0x00007FFFFFFFFFFF) ? ptr : 0;
}

// ES: Inventory* del edificio (si tiene), validado igual que arriba.
// EN: The building's Inventory* (if any), validated as above.
uintptr_t BuildingAccessor::GetInventoryPtr() const {
    auto& offsets = GetOffsets().building;
    if (offsets.inventory < 0) return 0;

    uintptr_t ptr = 0;
    Memory::Read(m_ptr + offsets.inventory, ptr);
    return (ptr > 0x10000 && ptr < 0x00007FFFFFFFFFFF) ? ptr : 0;
}

} // namespace kmp::game
