// ES: lobby_manager.h - Gestor del lobby del lado cliente: guarda la facción y el slot de jugador
//     que asigna el servidor y (si no está activo el modo co-op) parchea el string de facción del
//     jugador en la sección .rdata de kenshi_x64.exe antes de cargar la partida.
// EN: lobby_manager.h - Client-side lobby manager: stores the faction and player slot assigned
//     by the server and (unless co-op mode is active) patches the player faction string in the
//     .rdata section of kenshi_x64.exe before the game loads.
#pragma once
#include "kmp/types.h"
#include <string>
#include <atomic>

namespace kmp {

// ES: Estado de asignación de facción del jugador local y parche de su string en memoria.
// EN: Local player's faction assignment state and in-memory patch of its string.
class LobbyManager {
public:
    // ES: Guarda la facción (string-id FCS, p.ej. "10-kenshi-online.mod") y el slot recibidos del servidor.
    // EN: Stores the faction (FCS string id, e.g. "10-kenshi-online.mod") and slot received from the server.
    void OnFactionAssigned(const std::string& factionString, int playerSlot);
    // ES: Aplica el parche del string de facción (no-op que devuelve true en modo co-op).
    // EN: Applies the faction string patch (a no-op returning true in co-op mode).
    bool ApplyFactionPatch();
    // ES: Getters del estado de asignación.
    // EN: Assignment state getters.
    bool HasFaction() const { return m_hasAssignment; }
    int GetPlayerSlot() const { return m_playerSlot; }
    const std::string& GetFactionString() const { return m_factionString; }

private:
    std::string m_factionString;
    int m_playerSlot = -1;
    bool m_hasAssignment = false;
    // ES: Localiza en memoria el literal "204-gamedata.base" (facción vanilla del jugador).
    // EN: Locates the "204-gamedata.base" literal (vanilla player faction) in memory.
    uintptr_t FindFactionStringAddress();
};

} // namespace kmp
