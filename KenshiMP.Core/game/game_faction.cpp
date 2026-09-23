// ES: game_faction.cpp - Implementación de FactionAccessor: nombre (+0x1A8), nº de miembros,
//     si es la facción del jugador (+0x250) y dinero, usando FactionOffsets.
// EN: game_faction.cpp - FactionAccessor implementation: name (+0x1A8), member count,
//     whether it is the player faction (+0x250) and money, using FactionOffsets.
#include "game_types.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>

namespace kmp::game {

// ── FactionAccessor method implementations ──

// ES: Lee el nombre de la facción (std::string de MSVC con SSO, igual que en edificios).
// EN: Reads the faction name (MSVC std::string with SSO, same as buildings).
std::string FactionAccessor::GetName() const {
    auto& offsets = GetOffsets().faction;
    if (offsets.name < 0) return "Unknown Faction";

    uintptr_t strAddr = m_ptr + offsets.name;
    uint64_t size = 0, capacity = 0;
    Memory::Read(strAddr + 0x10, size);
    Memory::Read(strAddr + 0x18, capacity);
    if (size == 0 || size > 256) return "Unknown Faction";

    char buffer[257] = {};
    if (capacity > 15) {
        uintptr_t dataPtr = 0;
        Memory::Read(strAddr, dataPtr);
        if (dataPtr == 0) return "Unknown Faction";
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

// ES: Nº de miembros (offset memberCount 0x38 marcado como DUDOSO); acota a 0..9999.
// EN: Member count (memberCount offset 0x38 flagged as DOUBTFUL); clamps to 0..9999.
int FactionAccessor::GetMemberCount() const {
    auto& offsets = GetOffsets().faction;
    if (offsets.memberCount < 0) return 0;

    int count = 0;
    Memory::Read(m_ptr + offsets.memberCount, count);
    return (count >= 0 && count < 10000) ? count : 0;
}

// ES: True si la facción pertenece a un jugador (PlayerInterface* en +0x250 distinto de 0).
// EN: True if the faction belongs to a player (PlayerInterface* at +0x250 is non-zero).
bool FactionAccessor::IsPlayerFaction() const {
    auto& offsets = GetOffsets().faction;
    if (offsets.isPlayerFaction < 0) return false;

    // audit-14: isPlayerFaction (0x250) es un PlayerInterface* (8 bytes), NO un bool.
    // Una facción es de jugador si ese puntero != 0. Leer 1 byte daría falsos negativos.
    // EN: audit-14: +0x250 is an 8-byte PlayerInterface*, not a bool; reading 1 byte gives false negatives.
    uintptr_t playerIface = 0;
    Memory::Read(m_ptr + offsets.isPlayerFaction, playerIface);
    return playerIface != 0;
}

// ES: Dinero de la facción (offset 0xA0 marcado como DUDOSO).
// EN: Faction money (offset 0xA0 flagged as DOUBTFUL).
int FactionAccessor::GetMoney() const {
    auto& offsets = GetOffsets().faction;
    if (offsets.money < 0) return 0;

    int money = 0;
    Memory::Read(m_ptr + offsets.money, money);
    return money;
}

} // namespace kmp::game
