// ES: lobby_manager.cpp - Implementación de LobbyManager: recibe la facción asignada por el
//     servidor y, si se desactiva el modo co-op, sobrescribe el literal de facción del jugador
//     en .rdata para que la partida nueva arranque en la facción "Player N" del mod.
// EN: lobby_manager.cpp - LobbyManager implementation: receives the faction assigned by the
//     server and, if co-op mode is disabled, overwrites the player faction literal in .rdata so
//     the new game starts in the mod's "Player N" faction.
#include "lobby_manager.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <cstring>
#include <algorithm>

namespace kmp {

// ══════════════════════════════════════════════════════════════════════════════════════
//  TOGGLE MAESTRO — Modo CO-OP (no parchear la facción del jugador)  [Fase 4 — combate]
// ══════════════════════════════════════════════════════════════════════════════════════
// CONTEXTO (RE + runtime, 2026-06-18): el combate del host estaba ROTO porque ApplyFactionPatch
// reescribe el literal .rdata "204-gamedata.base" (facción vanilla "Nameless", hacia la que
// TODO el mundo tiene cableadas sus relaciones de hostilidad) por "10-kenshi-online.mod"
// (facción "Player N", clon de ModGen SIN relaciones declaradas). Resultado: el jugador se muda
// a una facción HUÉRFANA → neutral con todos → ni ataca ni le atacan (combate PvE muerto).
//
// OPCIÓN C (la elegida para CO-OP cooperativo de Zero): NO parchear → el jugador permanece en
// "Nameless" (204-gamedata.base), que YA tiene toda la red de relaciones del mundo → combate
// PvE restaurado al instante. Coste: todos los jugadores comparten Nameless (aliados, sin PvP).
//
// SEGURIDAD del modo co-op (verificado en el código del mod, 2026-06-18):
//   - La identificación del char del HOST NO depende del nombre "Player N": usa la cadena directa
//     del motor (GetPlayerPrimaryCharacterDirect, GW+0x580->+0x2B0->data[0]) + GetPlayerFactionDirect
//     (GW+0x580->+0x2A0), que ya resuelve "Nameless" correctamente. Por tanto, para 1 jugador host
//     es 100% seguro NO parchear.
//   - ⚠ Para 2+ jugadores SÍ rompe: la vinculación de remotos (FindModCharacterBySlot) busca
//     "Player N" por nombre, que solo existe si se parchea. El co-op multijugador real necesita
//     OPCIÓN A (mantener Player N + copiar relaciones de Nameless en runtime, FIX-HOSTILITY en
//     core.cpp ya corregido a nodo+0x1C). Para activar PvP: poner kCoopNoFactionPatch = false.
//
// PALANCA: kCoopNoFactionPatch
//   true  → modo CO-OP: NO se parchea, host en Nameless, combate PvE nativo. (DEFAULT para Zero)
//   false → modo Player N (PvP/facciones separadas): se parchea como antes; requiere la Opción A.
// EN: MASTER TOGGLE - co-op mode (do not patch the player faction). Context (RE + runtime
//     2026-06-18): ApplyFactionPatch replaced the .rdata literal "204-gamedata.base" (vanilla
//     "Nameless" faction, which the whole world has hostility relations wired to) with
//     "10-kenshi-online.mod" ("Player N", a ModGen clone with no relations), leaving the player in
//     an orphan faction that is neutral to everyone (PvE combat dead).
//     OPTION C (chosen): do not patch; the player stays in Nameless and PvE works natively.
//     Cost: all players share Nameless (allies, no PvP). Host identification does not depend on
//     "Player N" (direct engine chains GW+0x580), but with 2+ players remote binding
//     (FindModCharacterBySlot) searches "Player N" by name and needs OPTION A.
//     true = co-op (default), false = Player N mode (patch as before, needs Option A).
static constexpr bool kCoopNoFactionPatch = true;

// ES: Guarda la asignación recibida del servidor (no parchea nada todavía).
// EN: Stores the assignment received from the server (does not patch anything yet).
void LobbyManager::OnFactionAssigned(const std::string& factionString, int playerSlot) {
    m_factionString = factionString;
    m_playerSlot = playerSlot;
    m_hasAssignment = true;
    spdlog::info("LobbyManager: Assigned faction '{}' slot {}", factionString, playerSlot);
}

// ES: Ayudantes SEH: MSVC no permite __try en funciones con objetos C++ (spdlog, std::string),
//     así que estos envoltorios solo usan tipos POD y el código protegido. SEH captura
//     violaciones de acceso al leer direcciones que podrían no ser válidas.
// EN:
// SEH helpers — MSVC forbids __try in functions with C++ objects (spdlog, std::string).
// These thin wrappers contain ONLY POD types and the SEH-protected code.
// ES: Copia 17 bytes de addr a outBuf (terminado en nulo); false si la lectura falla.
// EN: Copies 17 bytes from addr into outBuf (null-terminated); false if the read faults.
static bool SEH_CheckFactionCandidate(uintptr_t addr, char* outBuf) {
    __try {
        memcpy(outBuf, reinterpret_cast<void*>(addr), 17);
        outBuf[17] = '\0';
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Compara len bytes en addr con searchStr; false si no coinciden o la lectura falla.
// EN: Compares len bytes at addr with searchStr; false if they differ or the read faults.
static bool SEH_MemcmpAt(uintptr_t addr, const char* searchStr, size_t len) {
    __try {
        return memcmp(reinterpret_cast<void*>(addr), searchStr, len) == 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Busca la dirección del literal de facción del jugador. Estrategia 1: RVAs conocidos
//     (RVA = dirección relativa a la base del módulo kenshi_x64.exe) para Steam y GOG 1.0.68,
//     validando que el texto tenga '-' y '.'. Estrategia 2: recorrer la sección .rdata del PE
//     byte a byte buscando "204-gamedata.base". Devuelve 0 si no lo encuentra.
// EN: Finds the address of the player faction literal. Strategy 1: known RVAs (RVA = address
//     relative to the kenshi_x64.exe module base) for Steam and GOG 1.0.68, checking the text
//     has '-' and '.'. Strategy 2: walk the PE .rdata section byte by byte looking for
//     "204-gamedata.base". Returns 0 if not found.
uintptr_t LobbyManager::FindFactionStringAddress() {
    uintptr_t moduleBase = Memory::GetModuleBase();

    // ES: Estrategia 1: probar RVAs conocidos.
    // EN:
    // Strategy 1: Try known offsets (Steam v1.0.68, GOG v1.0.68)
    uintptr_t candidates[] = {
        moduleBase + 0x16C4258,  // Steam v1.0.68
        moduleBase + 0x16C2F68,  // GOG v1.0.68
    };

    for (auto addr : candidates) {
        char buf[20] = {};
        if (SEH_CheckFactionCandidate(addr, buf)) {
            bool hasDash = false, hasDot = false;
            for (int i = 0; i < 17; i++) {
                if (buf[i] == '-') hasDash = true;
                if (buf[i] == '.') hasDot = true;
            }
            if (hasDash && hasDot) {
                spdlog::info("LobbyManager: Found faction string at 0x{:X}: '{}'",
                             addr, std::string(buf, 17));
                return addr;
            }
        }
    }

    // ES: Estrategia 2: parsear cabeceras DOS/NT del módulo y buscar el literal en .rdata.
    // EN:
    // Strategy 2: Search .rdata for "204-gamedata.base" (default faction string)
    const char* searchStr = "204-gamedata.base";
    size_t searchLen = 17;

    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto* sections = IMAGE_FIRST_SECTION(ntHeaders);

    for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
        if (strncmp(reinterpret_cast<const char*>(sections[i].Name), ".rdata", 6) == 0) {
            uintptr_t start = moduleBase + sections[i].VirtualAddress;
            uintptr_t end = start + sections[i].Misc.VirtualSize;

            for (uintptr_t addr = start; addr < end - searchLen; addr++) {
                if (SEH_MemcmpAt(addr, searchStr, searchLen)) {
                    spdlog::info("LobbyManager: Found faction string via search at 0x{:X}", addr);
                    return addr;
                }
            }
        }
    }

    spdlog::error("LobbyManager: Could not find faction string in memory");
    return 0;
}

// ES: Sobrescribe el literal de facción en .rdata con la facción asignada (máx 24 caracteres),
//     cambiando temporalmente la protección de página con VirtualProtect. En modo co-op no hace
//     nada y devuelve true. False si no hay asignación, no se encuentra el literal o falla VirtualProtect.
// EN: Overwrites the .rdata faction literal with the assigned faction (max 24 chars), temporarily
//     changing page protection with VirtualProtect. In co-op mode it does nothing and returns true.
//     False if there is no assignment, the literal is not found or VirtualProtect fails.
bool LobbyManager::ApplyFactionPatch() {
    // ── Modo CO-OP (Opción C): NO parchear → jugador permanece en "Nameless" (combate PvE) ──
    // Palanca maestra del lado cliente: ignora la factionString asignada por el server y NO
    // escribe nada en .rdata. Devuelve true (NO es un error: es la decisión de diseño co-op),
    // para que los logs/HUD no muestren fallo. Cubre AMBOS call sites (core.cpp:1329 pre-carga
    // y packet_handler.cpp:1849 al recibir la asignación), porque el no-op vive aquí dentro.
    // EN: Co-op mode (Option C): ignore the server-assigned factionString and write nothing to .rdata.
    //     Returns true on purpose (design decision, not an error). Covers both call sites.
    if (kCoopNoFactionPatch) {
        spdlog::info("LobbyManager: modo CO-OP activo (kCoopNoFactionPatch) → NO se parchea la "
                     "facción. El jugador permanece en 'Nameless' (204-gamedata.base), que conserva "
                     "la red de relaciones del mundo → combate PvE nativo. (factionString asignada "
                     "ignorada: '{}')", m_factionString);
        return true;
    }

    if (!m_hasAssignment || m_factionString.empty()) {
        spdlog::warn("LobbyManager: No faction assigned, cannot patch");
        return false;
    }

    // ── Defensa extra: si la facción asignada YA es "Nameless" (204-gamedata.base), no hay nada
    //    que parchear (parcharía el string sobre sí mismo). No-op natural, evita un write inútil.
    // EN: Extra guard: if the assigned faction already is Nameless there is nothing to patch.
    if (m_factionString == "204-gamedata.base") {
        spdlog::info("LobbyManager: facción asignada ya es 'Nameless' (204-gamedata.base) → no-op "
                     "(el jugador ya está en la facción con relaciones del mundo).");
        return true;
    }

    uintptr_t addr = FindFactionStringAddress();
    if (addr == 0) return false;

    // ES: El original ocupa 18 bytes (17 + nulo); los strings del mod pueden llegar a 24, así que se
    //     pisan unos bytes de más en .rdata. Se asume seguro porque son literales adyacentes no críticos
    //     (sin verificar de forma exhaustiva).
    // EN:
    // The original string "204-gamedata.base" is 17 chars + null = 18 bytes.
    // Mod faction strings like "10-kenshi-online.mod" are up to 24 chars.
    // We write the full string + null, overwriting a few bytes past the original.
    // This is safe because .rdata strings are packed with other string literals
    // and the adjacent bytes are not critical (research mod also overwrites 1+ bytes).
    static constexpr size_t MAX_FACTION_WRITE = 24;
    size_t writeLen = std::min(m_factionString.size(), MAX_FACTION_WRITE);
    size_t protectLen = writeLen + 1; // +1 for null terminator

    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), protectLen,
                        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        spdlog::error("LobbyManager: VirtualProtect failed at 0x{:X}", addr);
        return false;
    }

    // ES: Escribir el string con su terminador nulo y restaurar la protección original.
    // EN:
    // Write faction string with null terminator
    char factionBuf[MAX_FACTION_WRITE + 1] = {};
    memcpy(factionBuf, m_factionString.c_str(), writeLen);
    factionBuf[writeLen] = '\0';
    memcpy(reinterpret_cast<void*>(addr), factionBuf, writeLen + 1);

    VirtualProtect(reinterpret_cast<void*>(addr), protectLen, oldProtect, &oldProtect);

    spdlog::info("LobbyManager: Patched faction string at 0x{:X} to '{}' ({} bytes)",
                 addr, m_factionString, writeLen);
    return true;
}

} // namespace kmp
