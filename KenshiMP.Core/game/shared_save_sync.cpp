#include "shared_save_sync.h"
#include "game_types.h"
#include "../core.h"
#include "../hooks/char_tracker_hooks.h"
#include "../hooks/ai_hooks.h"
#include "kmp/protocol.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <mutex>
#include <chrono>
#include <Windows.h>

namespace kmp::shared_save_sync {

// ═══════════════════════════════════════════════════════════════════════════
//  SHARED-SAVE SYNC
//
//  Both players load the SAME save with kenshi-online.mod.
//  The mod defines "Player 1" and "Player 2" factions + characters.
//  Server assigns each player a faction string:
//    "10-kenshi-online" → you control "Player 1", other player is "Player 2"
//    "12-kenshi-online" → you control "Player 2", other player is "Player 1"
//  Characters already exist in the save — no factory spawning needed.
//  We just find them by name and sync positions.
// ═══════════════════════════════════════════════════════════════════════════

// ── State ──
static std::string s_ownCharName;
static std::string s_otherCharName;

static void* s_ownAnimClass = nullptr;
static void* s_otherAnimClass = nullptr;
static void* s_ownCharPtr = nullptr;
static void* s_otherCharPtr = nullptr;

static bool s_initialized = false;
static bool s_ownFound = false;
static bool s_otherFound = false;
// s_otherRequired: el modelo binario "other" solo aplica con 2 jugadores (slot 0/1).
// Con 3+ facciones (slot >= 2) NO hay "other" derivable por nombre → false. En ese caso el
// gating de "ambos encontrados" no debe esperar a un other que nunca llegará (audit-05 §1).
static bool s_otherRequired = false;

// Position sending throttle
static auto s_lastPosSend = std::chrono::steady_clock::time_point{};
static constexpr int POS_SEND_INTERVAL_MS = 50; // 20 Hz position updates

// Discovery retry
static auto s_lastDiscoveryLog = std::chrono::steady_clock::time_point{};
static int s_discoveryAttempts = 0;

// Remote position — mutex-protected because OnRemotePositionReceived is called
// from the network thread while Update reads from the game thread.
static std::mutex s_remoteMutex;
static Vec3 s_remotePosition{0, 0, 0};
static bool s_hasRemotePosition = false;
static std::atomic<float> s_remoteGameSpeed{-1.f};

// ── Faction string → character name mapping ──
// El string real que llega del lobby es del tipo "10-kenshi-online.mod" (con sufijo .mod),
// pero las comparaciones se hacen sin él. Normalizamos quitando el ".mod" final antes de
// comparar para evitar el spam "Unknown faction".
static std::string StripModSuffix(const std::string& faction) {
    const std::string suffix = ".mod";
    if (faction.size() >= suffix.size() &&
        faction.compare(faction.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return faction.substr(0, faction.size() - suffix.size());
    }
    return faction;
}

// ── Tabla StringId de facción → slot 0-based ──
// FUENTE DE VERDAD: E:\Aplicaciones\Kenshi-Online\faction-slots.json (generado por ModGen,
// leído por el server). Mientras el cliente no lea ese JSON directamente, mantenemos esta
// tabla espejo de los 6 StringIds del manifiesto. Si ModGen genera más facciones, hay que
// ampliar esta tabla O (mejor) leer el manifiesto desde disco (ver audit-05 §1, Camino B).
//
// IMPORTANTE: esta tabla SOLO se usa como respaldo cuando el slot del paquete no llega
// (slot < 0). En operación normal el slot SÍ viene en el S2C_FactionAssignment y se usa
// directamente (ver SlotToCharName), lo que cubre cualquier nº de facciones sin tocar tabla.
static int FactionToSlot(const std::string& faction) {
    const std::string f = StripModSuffix(faction);
    if (f == "10-kenshi-online") return 0; // Player 1
    if (f == "12-kenshi-online") return 1; // Player 2
    if (f == "45-kenshi-online") return 2; // Player 3
    if (f == "46-kenshi-online") return 3; // Player 4
    if (f == "47-kenshi-online") return 4; // Player 5
    if (f == "48-kenshi-online") return 5; // Player 6
    return -1; // Desconocida
}

// Deriva el nombre del Character a controlar a partir del slot 0-based.
// El .mod nombra a los personajes de jugador como "Player N" (1-based), así que:
//   slot 0 → "Player 1", slot 1 → "Player 2", ... slot 5 → "Player 6".
// Esto funciona para CUALQUIER nº de facciones sin tabla rígida (audit-05 §2).
// Devuelve "" si el slot está fuera de rango razonable (1..16 personajes de jugador).
static std::string SlotToCharName(int slot0Based) {
    if (slot0Based < 0 || slot0Based > 15) return "";
    return "Player " + std::to_string(slot0Based + 1);
}

// Resuelve el nombre del personaje PROPIO (Own).
// Estrategia: usar el SLOT que llega en el paquete (lo más limpio y escalable). Si el slot
// no es válido (paquete viejo / -1), se cae a derivarlo del StringId vía la tabla espejo.
static std::string ResolveOwnName(const std::string& faction, int slot) {
    // 1) Vía slot del paquete (preferida — escala a N facciones).
    std::string byName = SlotToCharName(slot);
    if (!byName.empty()) return byName;

    // 2) Respaldo: derivar slot desde el StringId conocido.
    int fallbackSlot = FactionToSlot(faction);
    return SlotToCharName(fallbackSlot);
}

// Resuelve el nombre del personaje "Other" (modelo binario heredado de 2 jugadores).
//
// LIMITACIÓN CONOCIDA (audit-05 §1, problema del modelo "Own/Other"):
// El concepto "Other" SOLO tiene sentido con EXACTAMENTE 2 jugadores (tú y "el otro").
// Con 3+ facciones (Player 3..6) este modelo binario está ROTO: no se puede enumerar a
// todos los remotos por un único nombre. La solución correcta (Camino B) es descubrir a
// los remotos por su entidad de red (EntityRegistry/ownerId), no por facción.
//
// Como puente, mantenemos el comportamiento de 2 jugadores:
//   slot 0 (Player 1) → other = Player 2
//   slot 1 (Player 2) → other = Player 1
// Para slot >= 2 NO hay "other" derivable → devolvemos "" y el sync de Other queda inactivo
// (los remotos se sincronizan por el sistema de entidades, no por este atajo de 2 jugadores).
static std::string ResolveOtherName(int ownSlot) {
    if (ownSlot == 0) return "Player 2"; // Player 1 ↔ Player 2 (caso 2 jugadores)
    if (ownSlot == 1) return "Player 1";
    return ""; // 3+ facciones: sin "other" binario (ver Camino B)
}

void Init() {
    auto& lobby = Core::Get().GetLobbyManager();
    if (!lobby.HasFaction()) {
        spdlog::warn("shared_save_sync: Init — no faction yet (will retry in Update)");
        return;
    }

    std::string faction = lobby.GetFactionString();
    int slot = lobby.GetPlayerSlot(); // slot 0-based que envió el server en S2C_FactionAssignment

    // ── Matching por SLOT (audit-05 §1-§2) ──
    // Own: derivado del slot (escala a Player 1..6+). Other: modelo binario de 2 jugadores
    // (heredado; vacío para slot >= 2, ver ResolveOtherName).
    s_ownCharName = ResolveOwnName(faction, slot);
    int ownSlot = SlotToCharName(slot).empty() ? FactionToSlot(faction) : slot; // slot efectivo usado
    s_otherCharName = ResolveOtherName(ownSlot);

    // ── DIAG-FACMATCH: volcado del matching de facción ──
    // Permite verificar de un vistazo (log + HUD) qué slot/facción llegó y si own/other
    // resolvieron. Útil para depurar per-player/teams con 6 facciones.
    spdlog::info("[DIAG-FACMATCH] faction='{}' slot={} ownSlot={} -> own='{}' (resolved={}) other='{}' (resolved={})",
                 faction, slot, ownSlot,
                 s_ownCharName, (s_ownCharName.empty() ? "NO" : "YES"),
                 s_otherCharName, (s_otherCharName.empty() ? "NO" : "YES"));

    // Own es OBLIGATORIO: sin él no sabemos qué personaje controla el jugador local → abortamos.
    if (s_ownCharName.empty()) {
        spdlog::error("[DIAG-FACMATCH] Unknown faction '{}' slot {} — no se pudo determinar el personaje propio",
                      faction, slot);
        Core::Get().GetNativeHud().AddSystemMessage(
            "ERROR: facción desconocida (slot " + std::to_string(slot) + ") — no puedo asignar tu personaje");
        return;
    }

    // Other es OPCIONAL: con 3+ facciones (slot >= 2) no hay "other" binario y es NORMAL.
    // No abortamos; el sync de la posición remota por este atajo simplemente queda inactivo
    // (los remotos se sincronizan vía el sistema de entidades, no por nombre de facción).
    if (s_otherCharName.empty()) {
        spdlog::warn("[DIAG-FACMATCH] Sin 'other' binario para slot {} (normal con 3+ facciones / per-player). "
                     "El sync de posición remota por nombre queda inactivo para este modo.", slot);
    }

    s_initialized = true;
    s_ownFound = false;
    s_otherFound = false;
    s_otherRequired = !s_otherCharName.empty(); // solo esperamos "other" si lo pudimos derivar
    s_ownAnimClass = nullptr;
    s_otherAnimClass = nullptr;
    s_ownCharPtr = nullptr;
    s_otherCharPtr = nullptr;
    {
        std::lock_guard lock(s_remoteMutex);
        s_hasRemotePosition = false;
        s_remotePosition = {0, 0, 0};
    }
    s_remoteGameSpeed.store(-1.f);
    s_discoveryAttempts = 0;

    spdlog::info("shared_save_sync: Initialized — own='{}' other='{}' (otherRequired={})",
                 s_ownCharName, s_otherCharName, s_otherRequired);
    if (s_otherRequired) {
        Core::Get().GetNativeHud().AddSystemMessage(
            "Shared save sync: you are " + s_ownCharName + ", looking for " + s_otherCharName + "...");
    } else {
        Core::Get().GetNativeHud().AddSystemMessage(
            "Shared save sync: you are " + s_ownCharName + " (remotos via sistema de entidades)");
    }
}

void Reset() {
    s_initialized = false;
    s_ownFound = false;
    s_otherFound = false;
    s_otherRequired = false;
    s_ownAnimClass = nullptr;
    s_otherAnimClass = nullptr;
    s_ownCharPtr = nullptr;
    s_otherCharPtr = nullptr;
    s_ownCharName.clear();
    s_otherCharName.clear();
    {
        std::lock_guard lock(s_remoteMutex);
        s_hasRemotePosition = false;
    }
    s_remoteGameSpeed.store(-1.f);
    spdlog::info("shared_save_sync: Reset");
}

// ── SEH-protected position read from AnimClass chain ──
static bool SEH_ReadAnimClassPosition(void* animClass, Vec3& out) {
    __try {
        uintptr_t animPtr = reinterpret_cast<uintptr_t>(animClass);
        if (animPtr < 0x10000 || animPtr > 0x00007FFFFFFFFFFF) return false;

        uintptr_t charMovement = 0;
        if (!Memory::Read(animPtr + 0xC0, charMovement) || charMovement == 0) return false;
        if (charMovement < 0x10000 || charMovement > 0x00007FFFFFFFFFFF) return false;

        uintptr_t posStruct = 0;
        if (!Memory::Read(charMovement + 0x320, posStruct) || posStruct == 0) return false;
        if (posStruct < 0x10000 || posStruct > 0x00007FFFFFFFFFFF) return false;

        Memory::Read(posStruct + 0x20, out.x);
        Memory::Read(posStruct + 0x24, out.y);
        Memory::Read(posStruct + 0x28, out.z);

        return (out.x != 0.f || out.y != 0.f || out.z != 0.f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── SEH-protected position write to AnimClass chain ──
static bool SEH_WriteAnimClassPosition(void* animClass, const Vec3& pos) {
    __try {
        uintptr_t animPtr = reinterpret_cast<uintptr_t>(animClass);
        if (animPtr < 0x10000 || animPtr > 0x00007FFFFFFFFFFF) return false;

        uintptr_t charMovement = 0;
        if (!Memory::Read(animPtr + 0xC0, charMovement) || charMovement == 0) return false;
        if (charMovement < 0x10000 || charMovement > 0x00007FFFFFFFFFFF) return false;

        uintptr_t posStruct = 0;
        if (!Memory::Read(charMovement + 0x320, posStruct) || posStruct == 0) return false;
        if (posStruct < 0x10000 || posStruct > 0x00007FFFFFFFFFFF) return false;

        Memory::Write(posStruct + 0x20, pos.x);
        Memory::Write(posStruct + 0x24, pos.y);
        Memory::Write(posStruct + 0x28, pos.z);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void SEH_WriteCachedPosition(void* charPtr, const Vec3& pos) {
    if (!charPtr) return;
    __try {
        uintptr_t charAddr = reinterpret_cast<uintptr_t>(charPtr);
        Memory::Write(charAddr + 0x48, pos.x);
        Memory::Write(charAddr + 0x4C, pos.y);
        Memory::Write(charAddr + 0x50, pos.z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Update(float deltaTime) {
    auto& core = Core::Get();
    if (!core.IsConnected() || !core.IsGameLoaded()) return;

    // ── LAZY INIT: faction assignment arrives AFTER SetConnected(true) ──
    // Init() is called from SetConnected but faction isn't assigned yet.
    // Retry here every tick until the faction arrives.
    if (!s_initialized) {
        auto& lobby = core.GetLobbyManager();
        if (lobby.HasFaction()) {
            Init(); // Now the faction is available
        }
        if (!s_initialized) return;
    }

    // "other" se considera satisfecho si NO se requiere (3+ facciones / per-player) o si ya
    // se encontró. Esto evita que el gating de discovery se bloquee esperando a un personaje
    // "other" que no existe en modos con más de 2 facciones (audit-05 §1).
    const bool otherSatisfied = (!s_otherRequired) || s_otherFound;

    // ── STEP 1: Discover characters by name ──
    if (!s_ownFound || !otherSatisfied) {
        s_discoveryAttempts++;

        // Re-validate cached pointers on every discovery tick (handles zone changes)
        if (!s_ownFound) {
            auto* tc = char_tracker_hooks::FindByName(s_ownCharName);
            if (tc && tc->animClassPtr) {
                s_ownAnimClass = tc->animClassPtr;
                s_ownCharPtr = tc->characterPtr;
                s_ownFound = true;
                spdlog::info("shared_save_sync: Found OWN character '{}' animClass=0x{:X}",
                             s_ownCharName, reinterpret_cast<uintptr_t>(s_ownAnimClass));
                core.GetNativeHud().AddSystemMessage("Found your character: " + s_ownCharName);
            }
        }

        // Solo buscamos "other" si el modelo binario aplica (2 jugadores). Con 3+ facciones
        // s_otherRequired==false y nos saltamos esta búsqueda (s_otherCharName está vacío).
        if (s_otherRequired && !s_otherFound) {
            auto* tc = char_tracker_hooks::FindByName(s_otherCharName);
            if (tc && tc->animClassPtr) {
                s_otherAnimClass = tc->animClassPtr;
                s_otherCharPtr = tc->characterPtr;
                s_otherFound = true;

                if (s_otherCharPtr) {
                    ai_hooks::MarkRemoteControlled(s_otherCharPtr);
                }

                spdlog::info("shared_save_sync: Found OTHER character '{}' animClass=0x{:X}",
                             s_otherCharName, reinterpret_cast<uintptr_t>(s_otherAnimClass));
                core.GetNativeHud().AddSystemMessage("Found remote player: " + s_otherCharName);
            }
        }

        // Log periódico de estado.
        // FIX #2 (Zero): antes esto spameaba el HUD cada 5s con
        // "Looking for characters...". Muy molesto para el jugador.
        // Ahora va SOLO al log de archivo (spdlog) y throttleado a 1 vez
        // cada 30s. NO se toca la lógica del tracker, solo el mensaje.
        auto now = std::chrono::steady_clock::now();
        auto sinceLog = std::chrono::duration_cast<std::chrono::seconds>(now - s_lastDiscoveryLog);
        if (sinceLog.count() >= 30) { // throttle subido de 5s a 30s
            s_lastDiscoveryLog = now;
            if (!s_ownFound || !otherSatisfied) {
                // Solo a archivo, NO al HUD (antes era AddSystemMessage).
                spdlog::info("shared_save_sync: Looking for characters... (tracked: {})",
                             char_tracker_hooks::GetTrackedCount());
            }
        }

        if (!s_ownFound || !otherSatisfied) return;

        core.GetNativeHud().AddSystemMessage("Both players found! Position sync active.");
        spdlog::info("shared_save_sync: BOTH CHARACTERS FOUND — sync active");
    } else {
        // Re-validate AnimClass pointers periodically (handles zone-load recreation)
        static int s_revalidateCounter = 0;
        if (++s_revalidateCounter % 300 == 0) { // Every ~5 seconds at 60fps
            auto* tc = char_tracker_hooks::FindByName(s_ownCharName);
            if (tc && tc->animClassPtr) {
                // Encontrado: actualizar cachés solo si el animClass cambió (recreación de zona).
                if (tc->animClassPtr != s_ownAnimClass) {
                    s_ownAnimClass = tc->animClassPtr;
                    s_ownCharPtr = tc->characterPtr;
                    spdlog::debug("shared_save_sync: Own animClass updated to 0x{:X}",
                                  reinterpret_cast<uintptr_t>(s_ownAnimClass));
                }
            } else {
                // Cambio 1: la búsqueda del propio falló (char destruido/recargado o aún no existe).
                // Invalidar cachés own para no leer/escribir sobre un puntero reciclado por el motor;
                // s_ownFound=false fuerza la re-búsqueda (STEP 1) en el siguiente tick.
                s_ownAnimClass = nullptr;
                s_ownCharPtr = nullptr;
                s_ownFound = false;
            }
            // Revalidar "other" solo si el modelo binario aplica (2 jugadores).
            if (s_otherRequired) {
                auto* tc2 = char_tracker_hooks::FindByName(s_otherCharName);
                if (tc2 && tc2->animClassPtr) {
                    // Encontrado: actualizar cachés solo si el animClass cambió.
                    if (tc2->animClassPtr != s_otherAnimClass) {
                        // Cambio 3.1: desmarcar el puntero "other" ANTERIOR antes de sustituirlo por
                        // el nuevo, para que no quede marcado como remote-controlled para siempre si
                        // el motor recicla esa dirección para un NPC local nuevo.
                        if (s_otherCharPtr && s_otherCharPtr != tc2->characterPtr) {
                            ai_hooks::UnmarkRemoteControlled(s_otherCharPtr);
                        }
                        s_otherAnimClass = tc2->animClassPtr;
                        s_otherCharPtr = tc2->characterPtr;
                        if (s_otherCharPtr) ai_hooks::MarkRemoteControlled(s_otherCharPtr);
                        spdlog::debug("shared_save_sync: Other animClass updated to 0x{:X}",
                                      reinterpret_cast<uintptr_t>(s_otherAnimClass));
                    }
                } else {
                    // Cambio 1: la búsqueda del ajeno falló → invalidar cachés other (evita UAF sobre
                    // memoria reciclada). Desmarcamos el puntero viejo para no dejarlo remote-controlled.
                    if (s_otherCharPtr) ai_hooks::UnmarkRemoteControlled(s_otherCharPtr);
                    s_otherAnimClass = nullptr;
                    s_otherCharPtr = nullptr;
                    s_otherFound = false;
                }
            }
        }
    }

    // ── STEP 2: Read own position and send to server ──
    // Uses the EXISTING C2S_PositionUpdate format that the server already handles.
    // The server stores position on the ConnectedPlayer and broadcasts via
    // S2C_PositionUpdate to other clients.
    auto now = std::chrono::steady_clock::now();
    auto sinceSend = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastPosSend);
    if (sinceSend.count() >= POS_SEND_INTERVAL_MS && s_ownAnimClass) {
        s_lastPosSend = now;

        Vec3 myPos;
        if (SEH_ReadAnimClassPosition(s_ownAnimClass, myPos)) {
            // Use the existing position update format — the server reads:
            // U32(sourcePlayer) [handled by server from peer], U8(count), then
            // CharacterPosition structs. We need to match this EXACTLY.
            PacketWriter writer;
            writer.WriteHeader(MessageType::C2S_PositionUpdate);
            // The server reads sourcePlayer as U32 first, but the canonical client
            // code (core.cpp PollLocalPositions) writes U8(count) first, then
            // CharacterPosition structs. Let me match the canonical format.
            writer.WriteU8(1); // count = 1 (FIX: was U32, must be U8)

            // CharacterPosition struct — must match the server's ReadRaw size
            CharacterPosition cp{};
            cp.entityId = 0; // Shared-save mode uses entityId 0 as "player avatar"
            cp.posX = myPos.x;
            cp.posY = myPos.y;
            cp.posZ = myPos.z;
            cp.compressedQuat = 0;
            cp.animStateId = 0;
            cp.moveSpeed = 0;
            cp.flags = 0;
            writer.WriteRaw(&cp, sizeof(cp));

            core.GetClient().SendUnreliable(writer.Data(), writer.Size());
        }
    }

    // ── STEP 3: Write received position to other player's character ──
    {
        Vec3 remotePos;
        bool hasRemote;
        {
            std::lock_guard lock(s_remoteMutex);
            remotePos = s_remotePosition;
            hasRemote = s_hasRemotePosition;
        }
        if (hasRemote && s_otherAnimClass) {
            SEH_WriteAnimClassPosition(s_otherAnimClass, remotePos);
            SEH_WriteCachedPosition(s_otherCharPtr, remotePos);
        }
    }

    // ── STEP 4: Game speed sync ──
    float speed = s_remoteGameSpeed.load();
    if (speed >= 0.f) {
        uintptr_t gwSingleton = core.GetGameFunctions().GameWorldSingleton;
        if (gwSingleton != 0) {
            game::GameWorldAccessor gw(gwSingleton);
            if (gw.IsValid()) {
                float currentSpeed = gw.GetGameSpeed();
                if (std::abs(currentSpeed - speed) > 0.01f) {
                    gw.WriteGameSpeed(speed);
                }
            }
        }
        s_remoteGameSpeed.store(-1.f);
    }
}

void OnRemotePositionReceived(const Vec3& pos) {
    std::lock_guard lock(s_remoteMutex);
    s_remotePosition = pos;
    s_hasRemotePosition = true;
}

void OnRemoteGameSpeedReceived(float speed) {
    s_remoteGameSpeed.store(speed);
}

bool IsOwnCharacterFound() { return s_ownFound; }
bool IsOtherCharacterFound() { return s_otherFound; }
const std::string& GetOwnCharacterName() { return s_ownCharName; }
const std::string& GetOtherCharacterName() { return s_otherCharName; }

} // namespace kmp::shared_save_sync
