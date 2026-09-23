// ES: Manejador de paquetes del cliente. Recibe cada paquete del servidor (callback de
//     NetworkClient, en el hilo que bombea ENet), lo decodifica según su MessageType y
//     lo despacha al sistema que toca: conexión/handshake, jugadores, chat, spawns y
//     despawns de entidades, posiciones, combate, salud, equipo, inventario, escuadrones,
//     facciones, edificios, puertas, tiempo, comercio, lobby y depurador de pipeline.
//     Todo lo que escribe memoria del motor o llama funciones nativas del juego se
//     encola en la CommandQueue para que lo ejecute el hilo del juego (evita carreras
//     con el render). Incluye ayudas SEH para escribir posición/salud en personajes.
//     Formato de cada mensaje: docs/PROTOCOL.md y docs/architecture/05-network-protocol.md.
// EN: Client packet handler. It receives every packet from the server (NetworkClient
//     callback, on the thread that pumps ENet), decodes it by MessageType and dispatches
//     it to the right system: connection/handshake, players, chat, entity spawns and
//     despawns, positions, combat, health, equipment, inventory, squads, factions,
//     buildings, doors, time, trade, lobby and the pipeline debugger.
//     Anything that writes engine memory or calls native game functions is pushed to
//     the CommandQueue so the game thread runs it (avoids races with rendering). It
//     includes SEH helpers to write position/health into characters.
//     Per-message format: docs/PROTOCOL.md and docs/architecture/05-network-protocol.md.
#include "../core.h"
#include "../game/game_types.h"
#include "../game/game_inventory.h"
#include "../game/spawn_manager.h"
#include "../game/player_controller.h"
#include "../game/lobby_manager.h"
#include "../game/shared_save_sync.h"
#include "../hooks/entity_hooks.h"
#include "../hooks/combat_hooks.h"
#include "../hooks/time_hooks.h"
#include "../hooks/squad_hooks.h"
#include "../hooks/faction_hooks.h"
#include "../hooks/ai_hooks.h"
#include "../sync/authority_validator.h"
#include "../sync/pending_snapshot_queue.h"
#include "../sync/deferred_spawn_queue.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/memory.h"
#include "kmp/string_convert.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <unordered_set>
#include <array>  // FIX CRASH 2º JUGADOR: capturar healthData[7] por VALOR en la lambda del CommandQueue

namespace kmp {

// ES: Tipos de puntero a funciones nativas del juego que se llaman desde aquí
//     (__fastcall = convención x64 de MSVC). Aplicar daño y muerte de personaje.
//     NOTA: se quitó CharacterMoveTo: el escáner de patrones encontró una dirección a
//     mitad de función, no es seguro llamarla.
// Forward declarations for game function call types
// NOTE: CharacterMoveTo removed — pattern scanner found mid-function address, not safe to call
using ApplyDamageFn = void(__fastcall*)(void* target, void* attacker,
                                         int bodyPart, float cut, float blunt, float pierce);
using CharacterDeathFn = void(__fastcall*)(void* character, void* killer);

// ES: ── Ayudas SEH para inicializar personajes del mod enlazados ──
//     MSVC C2712: __try no puede convivir con objetos C++ con destructor. Estas
//     funciones planas aíslan el SEH de std::string/unordered_map.
// ── SEH helpers for mod character link initialization ──
// MSVC C2712: __try cannot coexist with C++ objects that have destructors.
// These plain-function wrappers isolate SEH from std::string/unordered_map.

// ES: Escribe una posición en el personaje (false si hubo excepción).
// EN: Writes a position into the character (false on exception).
static bool SEH_WritePositionToChar(void* character, float x, float y, float z) {
    __try {
        game::CharacterAccessor accessor(character);
        Vec3 pos(x, y, z);
        accessor.WritePosition(pos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Avisa a PlayerController de que un personaje remoto ya existe (renombrado y
//     seguimiento), protegido con SEH.
// EN: Notifies PlayerController that a remote character now exists (rename and
//     tracking), SEH-protected.
static bool SEH_OnRemoteCharSpawned(EntityID entityId, void* character, PlayerID owner) {
    __try {
        Core::Get().GetPlayerController().OnRemoteCharacterSpawned(entityId, character, owner);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Programa la sonda diferida de la clase de animación del personaje, protegida con SEH.
// EN: Schedules the deferred anim-class probe for the character, SEH-protected.
static bool SEH_ScheduleAnimProbe(void* character) {
    __try {
        game::ScheduleDeferredAnimClassProbe(reinterpret_cast<uintptr_t>(character));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Pone en alianza (relación 100) la facción local y la del personaje remoto,
//     llamando a la función original de relación de facciones (sin pasar por nuestro
//     hook, marcándolo como "venido del servidor"). DESACTIVADA: ya no se llama (ver
//     HandleEntitySpawn, punto 6): era el disparador del crash por uso tras liberar de
//     la facción (game+0x927E94).
// EN: Allies (relation 100) the local faction and the remote character's faction,
//     calling the original faction-relation function (bypassing our hook, flagged as
//     "server sourced"). DISABLED: no longer called (see HandleEntitySpawn, step 6): it
//     was the trigger of the faction use-after-free crash (game+0x927E94).
static bool SEH_AllyModFaction(void* character) {
    __try {
        auto origFn = faction_hooks::GetOriginal();
        if (!origFn) return false;

        game::CharacterAccessor accessor(character);
        if (!accessor.IsValid()) return false;
        uintptr_t remoteFaction = accessor.GetFactionPtr();
        if (remoteFaction < 0x10000 || remoteFaction >= 0x00007FFFFFFFFFFF) return false;

        uintptr_t localFaction = Core::Get().GetPlayerController().GetLocalFactionPtr();
        if (localFaction == 0 || localFaction == remoteFaction) return false;

        faction_hooks::SetServerSourced(true);
        origFn(reinterpret_cast<void*>(localFaction), reinterpret_cast<void*>(remoteFaction), 100.0f);
        origFn(reinterpret_cast<void*>(remoteFaction), reinterpret_cast<void*>(localFaction), 100.0f);
        faction_hooks::SetServerSourced(false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faction_hooks::SetServerSourced(false);
        return false;
    }
}

// ── Recalcula el flag de colapso médico (brazos rotos / leftArmOk) tras escribir salud ──
// El motor cachea en MedicalSystem un BOOL "brazo OK" (char+0x458+0x166 = char+0x5BE) que
// SOLO se refresca dentro de MedicalSystem::reassessCollapseMode. Como el mod escribe la
// salud de las extremidades a pelo en memoria (SEH_WriteLimbHealth*), ese bool se queda
// CONGELADO: aunque curemos/sincronicemos el brazo al 100%, el gate de "cargar a cuestas"
// (Hook_AddOrderBackend, lee char+0x5BD/+0x5BE) lo sigue viendo roto y traga la orden.
// Esta llamada fuerza el recálculo del flag a partir de la salud real ya escrita.
//   RVA 0x649320: VERIFICADA en bytes sobre kenshi_x64.exe Steam 1.0.68 (prólogo de función
//   real; el cuerpo lee partCount@MedicalSystem+0x198 y partArray@+0x1A0, lo que confirma
//   que rcx = char+0x458). El 0x648BA0 que aparece en KenshiLib está OBSOLETO para esta build
//   (cae a mitad de otra función). Firma: void __fastcall(MedicalSystem* this, bool medic, bool agony).
// SOLO game thread: todos los callers de los helpers de salud pushean a CommandQueue o
// corren en el procesamiento de spawn del game thread. SEH-safe: solo POD, nunca propaga.
// clearStun: true cuando esta llamada sigue a una escritura de salud REAL (limpia el fleshStun
// rancio que corrompería el cálculo de colapso); false para recálculos periódicos sin datos
// nuevos (ver core.cpp SEH_ReassessCollapse — mismo hallazgo de la revisión adversarial: borrar
// stun sin datos nuevos anula stun real de combate legítimo). Ambos call-sites de este fichero
// (SEH_WriteLimbHealthToChar/SEH_WriteOnePartHealth) SIEMPRE acaban de escribir salud real, así
// que ambos pasan true.
// EN: ── Recomputes the medical collapse flag (broken arms / leftArmOk) after writing health ──
//     The engine caches in MedicalSystem a "arm OK" BOOL (char+0x458+0x166 = char+0x5BE)
//     that is ONLY refreshed inside MedicalSystem::reassessCollapseMode. Since the mod
//     writes limb health raw into memory (SEH_WriteLimbHealth*), that bool stays FROZEN:
//     even if we heal/sync the arm to 100%, the "carry on shoulders" gate
//     (Hook_AddOrderBackend, reads char+0x5BD/+0x5BE) still sees it broken and swallows
//     the order. This call forces the flag to be recomputed from the real health.
//       RVA 0x649320 (address relative to the kenshi_x64.exe base): VERIFIED in bytes on
//       Steam 1.0.68 (real function prologue; the body reads partCount@MedicalSystem+0x198
//       and partArray@+0x1A0, confirming rcx = char+0x458). KenshiLib's 0x648BA0 is
//       OBSOLETE for this build (lands mid-function).
//       Signature: void __fastcall(MedicalSystem* this, bool medic, bool agony).
//     Game thread ONLY: every caller of the health helpers pushes to CommandQueue or runs
//     in game-thread spawn processing. SEH-safe: POD only, never propagates.
//     clearStun: true when this follows a REAL health write (clears stale fleshStun that
//     would corrupt the collapse computation); false for periodic recomputes with no new
//     data (see core.cpp SEH_ReassessCollapse). Both call sites in this file always just
//     wrote real health, so both pass true.
static void SEH_ReassessCollapse(void* character, bool clearStun) {
    __try {
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);
        if (charPtr == 0) return;
        // SALTAR si el char está MUERTO (char+0x5BC, byte isDead: 0=vivo, 1=muerto). La rama
        // interna dead=1 de reassessCollapseMode puede tener efectos colaterales sobre un
        // cadáver; NO se debe tocar +0x5BC (ver docs/reverse-engineering/kenshi-re-memory.md).
        // EN: SKIP if the character is DEAD (char+0x5BC, isDead byte: 0=alive, 1=dead). The
        //     dead=1 branch of reassessCollapseMode may have side effects on a corpse; +0x5BC
        //     must NOT be touched.
        uint8_t isDead = 0;
        if (!Memory::Read(charPtr + 0x5BC, isDead) || isDead != 0) return;
        // FIX-STUNSEED: limpiar fleshStun (part+0x44) rancio antes del recálculo — ver comentario
        // gemelo en core.cpp para el razonamiento completo (getCollapseStage resta flesh-fleshStun).
        // Escritura LOCAL, no toca red/protocolo. Solo fleshStun; flesh (salud real) intacto.
        // EN: FIX-STUNSEED: clear stale fleshStun (part+0x44 = healthBase+4) before recomputing
        //     (getCollapseStage subtracts flesh-fleshStun). LOCAL write, no network/protocol.
        //     Only fleshStun; flesh (real health) untouched.
        if (clearStun) {
            auto& offsets = game::GetOffsets().character;
            if (offsets.healthPartArray >= 0 && offsets.healthBase >= 0) {
                uintptr_t partArray = 0;
                int count = 0;
                if (Memory::Read(charPtr + offsets.healthPartArray, partArray) && partArray != 0 &&
                    Memory::Read(charPtr + offsets.healthPartCount, count) && count > 0 && count <= 32) {
                    for (int i = 0; i < count; i++) {
                        uintptr_t part = 0;
                        if (!Memory::Read(partArray + i * offsets.healthStride, part) || part == 0)
                            continue;
                        Memory::Write(part + offsets.healthBase + 4, 0.0f);
                    }
                }
            }
        }
        // MedicalSystem vive INLINE dentro del Character en char+0x458 (NO es puntero).
        // EN: MedicalSystem lives INLINE inside the Character at char+0x458 (it is NOT a pointer).
        void* medicalSystem = reinterpret_cast<void*>(charPtr + 0x458);
        using ReassessCollapseFn = void(__fastcall*)(void* medical, bool medic, bool agony);
        auto fn = reinterpret_cast<ReassessCollapseFn>(Memory::GetModuleBase() + 0x649320);
        fn(medicalSystem, false, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Una llamada nativa fallida jamás debe tumbar el game thread.
    // EN: A failed native call must never bring down the game thread.
    }
}

// Escribe la salud (flesh) de las 7 partes por la cadena CANÓNICA MedicalSystem:
//   partArray = *(void**)(char + 0x5F8)  →  part_i = *(void**)(partArray + i*8)  →  flesh @ part_i+0x40
// (La cadena vieja [char+0x2B8]→[+0x5F8]→+0x40 tenía un deref de más — ver game_types.h.)
// EN: Writes flesh health of the 7 parts through the CANONICAL MedicalSystem chain:
//       partArray = *(void**)(char + 0x5F8) → part_i = *(void**)(partArray + i*8) → flesh @ part_i+0x40
//     (The old chain [char+0x2B8]→[+0x5F8]→+0x40 had one dereference too many; see game_types.h.)
//     The real offsets come from GetOffsets().character (healthPartArray/Count/Stride/Base).
static bool SEH_WriteLimbHealthToChar(void* character, const float health[7]) {
    __try {
        auto& offsets = game::GetOffsets().character;
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);
        if (offsets.healthPartArray < 0 || offsets.healthBase < 0) return false;
        uintptr_t partArray = 0;
        if (!Memory::Read(charPtr + offsets.healthPartArray, partArray) || partArray == 0)
            return false;
        int count = 0;
        if (!Memory::Read(charPtr + offsets.healthPartCount, count)) return false;
        if (count <= 0 || count > 32) return false;
        int n = (count < 7) ? count : 7;
        for (int i = 0; i < n; i++) {
            uintptr_t part = 0;
            if (!Memory::Read(partArray + i * offsets.healthStride, part) || part == 0)
                continue;
            Memory::Write(part + offsets.healthBase, health[i]);
        }
        // Refrescar el flag de colapso UNA sola vez, tras escribir TODAS las partes
        // (fuera del bucle — evita 7 llamadas nativas seguidas por char).
        // EN: Refresh the collapse flag ONCE, after writing ALL parts
        //     (outside the loop, avoiding 7 native calls in a row per character).
        SEH_ReassessCollapse(character, /*clearStun=*/true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Escribe la salud (flesh) de UNA sola parte por la cadena canónica. SEH-safe, solo POD.
// EN: Writes flesh health of ONE part through the canonical chain. SEH-safe, POD only.
static bool SEH_WriteOnePartHealth(void* character, int partIndex, float value) {
    __try {
        auto& offsets = game::GetOffsets().character;
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);
        if (offsets.healthPartArray < 0 || partIndex < 0) return false;
        int count = 0;
        if (!Memory::Read(charPtr + offsets.healthPartCount, count) || partIndex >= count)
            return false;
        uintptr_t partArray = 0;
        if (!Memory::Read(charPtr + offsets.healthPartArray, partArray) || partArray == 0)
            return false;
        uintptr_t part = 0;
        if (!Memory::Read(partArray + partIndex * offsets.healthStride, part) || part == 0)
            return false;
        if (!Memory::Write(part + offsets.healthBase, value))
            return false;
        // Refrescar el flag de colapso tras cambiar la salud de esta parte.
        // EN: Refresh the collapse flag after changing this part's health.
        SEH_ReassessCollapse(character, /*clearStun=*/true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Maneja los paquetes que llegan del servidor y los reparte a los sistemas
//     correspondientes. Clase con solo métodos estáticos.
// Handles incoming packets from the server and dispatches to appropriate systems.
class PacketHandler {
public:
    // ES: Registra HandlePacket como callback de paquetes del cliente de red.
    // EN: Registers HandlePacket as the network client's packet callback.
    static void Initialize() {
        Core::Get().GetClient().SetPacketCallback(
            [](const uint8_t* data, size_t size, int channel) {
                HandlePacket(data, size, channel);
            });
    }

    // ES: ── Todos los jugadores listos (servidor → cliente) ──
    //     El servidor lo manda cuando TODOS los jugadores conectados han cargado su
    //     partida. AHORA ya se pueden crear personajes y activar la sincronización
    //     completa: se procesan los spawns que se habían diferido durante la carga.
    // ── All Players Ready (Server → Client) ──
    // Server sends this when ALL connected players have loaded their games
    // NOW we can safely spawn characters and activate full sync
    static void HandleAllPlayersReady() {
        auto& core = Core::Get();

        spdlog::info("PacketHandler: ALL PLAYERS READY - Server says we can spawn now!");
        core.GetNativeHud().AddSystemMessage("All players ready - spawning characters!");
        core.GetNativeHud().LogStep("SYNC", "All players ready - activating spawn");

        // ES: ARREGLADO: procesar los paquetes de spawn encolados durante la carga.
        // FIXED: Process queued spawn packets that were deferred during load
        size_t queueSize = DeferredSpawnQueue::Size();
        if (queueSize > 0) {
            spdlog::info("PacketHandler: Processing {} deferred spawns", queueSize);
            DeferredSpawnQueue::ProcessAll();
        }
    }

    // ES: Punto de entrada de cada paquete: lee la cabecera y despacha. Primero los
    //     mensajes "seguros" (no tocan el mundo del juego); luego, si el juego no está
    //     cargado, solo acepta spawns, snapshot del mundo y hora; si está cargado,
    //     despacha el resto de mensajes del mundo.
    // EN: Entry point for every packet: reads the header and dispatches. First the "safe"
    //     messages (do not touch the game world); then, if the game is not loaded, only
    //     spawns, world snapshot and time are accepted; if it is loaded, the remaining
    //     world messages are dispatched.
    static void HandlePacket(const uint8_t* data, size_t size, int channel) {
        if (size < sizeof(PacketHeader)) {
            spdlog::warn("PacketHandler: Packet too small ({} bytes)", size);
            return;
        }

        PacketReader reader(data, size);
        PacketHeader header;
        if (!reader.ReadHeader(header)) return;

        // ES: Depuración: log de cada paquete los 100 primeros, luego cada 50.
        // Debug: log every packet for first 100, then every 50th
        static int s_packetNum = 0;
        s_packetNum++;
        if (s_packetNum <= 100 || s_packetNum % 50 == 0) {
            spdlog::debug("PacketHandler: pkt #{} type={} size={} ch={}",
                          s_packetNum, static_cast<int>(header.type), size, channel);
        }

        // ES: ── Mensajes SEGUROS (funcionan sin mundo de juego) ──
        //     Son mensajes de conexión/interfaz que no acceden a objetos del juego.
        // ── SAFE messages (work without game world) ──
        // These are pure connection/UI messages that don't access game objects.
        switch (header.type) {
        case MessageType::S2C_HandshakeAck:
            HandleHandshakeAck(reader);
            return;
        case MessageType::S2C_HandshakeReject:
            HandleHandshakeReject(reader);
            return;
        case MessageType::S2C_PlayerJoined:
            HandlePlayerJoined(reader);
            return;
        case MessageType::S2C_PlayerLeft:
            // ES: PlayerLeft teletransporta entidades bajo tierra: necesita el juego cargado.
            // PlayerLeft teleports entities underground — needs game loaded
            if (!Core::Get().IsGameLoaded()) {
                spdlog::debug("PacketHandler: Deferring PlayerLeft (game not loaded)");
                return;
            }
            HandlePlayerLeft(reader);
            return;
        case MessageType::S2C_ChatMessage:
            HandleChatMessage(reader);
            return;
        case MessageType::S2C_SystemMessage:
            HandleSystemMessage(reader);
            return;
        case MessageType::S2C_AdminResponse:
            HandleAdminResponse(reader);
            return;
        // ES: El servidor indica qué jugador es el host.
        // EN: The server says which player is the host.
        case MessageType::S2C_HostAssignment: {
            MsgHostAssignment msg{};
            if (!reader.ReadRaw(&msg, sizeof(msg))) {
                spdlog::warn("PacketHandler: Malformed S2C_HostAssignment");
                return;
            }
            Core::Get().SetLocalHostPlayerId(msg.newHostPlayerId);
            if (Core::Get().IsHost()) {
                spdlog::info("PacketHandler: You are now the host");
                Core::Get().GetNativeHud().AddSystemMessage("You are now the host.");
            } else {
                spdlog::info("PacketHandler: Host is now player {}", msg.newHostPlayerId);
            }
            return;
        }
        case MessageType::S2C_TradeResult:
            HandleTradeResult(reader);
            return;
        case MessageType::S2C_KeepaliveAck:
            // ES: El servidor confirmó nuestro keepalive: nada que hacer.
            // Server acknowledged our keepalive — nothing to do
            return;
        case MessageType::S2C_AllPlayersReady:
            HandleAllPlayersReady();
            return;
        case MessageType::S2C_FactionAssignment:
            HandleFactionAssignment(reader);
            return;
        case MessageType::S2C_LobbyStart:
            HandleLobbyStart(reader);
            return;
        // ES: Snapshot / eventos del depurador de pipeline de otro jugador (reenviados por el
        //     servidor con el id del emisor delante).
        // EN: Another player's pipeline debugger snapshot / events (relayed by the server
        //     with the sender id in front).
        case MessageType::S2C_PipelineSnapshot: {
            PlayerID sender;
            if (!reader.ReadU32(sender)) return;
            Core::Get().GetPipelineOrch().OnRemoteSnapshot(sender, reader.Current(), reader.Remaining());
            return;
        }
        case MessageType::S2C_PipelineEvent: {
            PlayerID sender;
            if (!reader.ReadU32(sender)) return;
            Core::Get().GetPipelineOrch().OnRemoteEvent(sender, reader.Current(), reader.Remaining());
            return;
        }
        default:
            // ES: Seguir con los mensajes que dependen del mundo del juego (abajo).
            break; // Fall through to game-world-dependent messages below
        }

        // ES: ── Mensajes del MUNDO DE JUEGO (requieren IsGameLoaded) ──
        //     Todos los demás acceden a objetos, memoria o punteros a funciones del juego.
        //     NO deben ejecutarse antes de que exista el mundo.
        // ── GAME-WORLD messages (require IsGameLoaded) ──
        // All remaining messages access game objects, memory, or function pointers.
        // They MUST NOT run before the game world exists.
        if (!Core::Get().IsGameLoaded()) {
            // ES: Los spawns de entidades y los snapshots del mundo se pueden encolar
            //     (SpawnManager / cola diferida los guardan); todo lo demás se descarta.
            // Entity spawns and world snapshots are safe to queue (SpawnManager holds them)
            // but everything else must be dropped.
            switch (header.type) {
            case MessageType::S2C_EntitySpawn:
                HandleEntitySpawn(reader);
                return;
            case MessageType::S2C_WorldSnapshot:
                HandleWorldSnapshot(reader);
                return;
            case MessageType::S2C_TimeSync:
                HandleTimeSync(reader);
                return;
            default:
                spdlog::debug("PacketHandler: Dropping message 0x{:02X} (game not loaded)",
                              static_cast<uint8_t>(header.type));
                return;
            }
        }

        // ES: Despacho de los mensajes del mundo de juego, agrupados por área.
        // EN: Dispatch of game-world messages, grouped by area.
        switch (header.type) {
        // ── Entity ──
        case MessageType::S2C_EntitySpawn:
            HandleEntitySpawn(reader);
            break;
        case MessageType::S2C_EntityDespawn:
            HandleEntityDespawn(reader);
            break;

        // ── Movement ──
        case MessageType::S2C_PositionUpdate:
            HandlePositionUpdate(reader);
            break;
        case MessageType::S2C_MoveCommand:
            HandleMoveCommand(reader);
            break;

        // ── Combat ──
        case MessageType::S2C_CombatHit:
            HandleCombatHit(reader);
            break;
        case MessageType::S2C_CombatDeath:
            HandleCombatDeath(reader);
            break;
        case MessageType::S2C_CombatKO:
            HandleCombatKO(reader);
            break;

        // ── World ──
        case MessageType::S2C_TimeSync:
            HandleTimeSync(reader);
            break;
        case MessageType::S2C_WorldSnapshot:
            HandleWorldSnapshot(reader);
            break;
        case MessageType::S2C_EntityHeartbeat:
            HandleEntityHeartbeat(reader);
            break;
        case MessageType::S2C_BuildPlaced:
            HandleBuildPlaced(reader);
            break;

        // ── Stats ──
        case MessageType::S2C_StatUpdate:
            HandleStatUpdate(reader);
            break;
        case MessageType::S2C_HealthUpdate:
            HandleHealthUpdate(reader);
            break;
        case MessageType::S2C_EquipmentUpdate:
            HandleEquipmentUpdate(reader);
            break;
        case MessageType::S2C_LimbHealth:
            HandleLimbHealth(reader);
            break;
        case MessageType::S2C_StatusEffect:
            HandleStatusEffect(reader);
            break;

        // ── Inventory ──
        case MessageType::S2C_InventoryUpdate:
            HandleInventoryUpdate(reader);
            break;

        // ── Squad ──
        case MessageType::S2C_SquadCreated:
            HandleSquadCreated(reader);
            break;
        case MessageType::S2C_SquadMemberUpdate:
            HandleSquadMemberUpdate(reader);
            break;

        // ── Faction ──
        case MessageType::S2C_FactionRelation:
            HandleFactionRelation(reader);
            break;

        // ── Building sync ──
        case MessageType::S2C_BuildDestroyed:
            HandleBuildDestroyed(reader);
            break;
        case MessageType::S2C_BuildProgress:
            HandleBuildProgressUpdate(reader);
            break;
        case MessageType::S2C_DoorState:
            HandleDoorState(reader);
            break;

        // ES: ── Postura de combate (reutiliza el tipo de mensaje CombatBlock) ──
        // ── Combat stance (reuses CombatBlock message type) ──
        case MessageType::S2C_CombatBlock:
            HandleCombatStance(reader);
            break;

        default:
            spdlog::debug("PacketHandler: Unknown message type 0x{:02X}", static_cast<uint8_t>(header.type));
            break;
        }
    }

private:
    // ES: Handshake aceptado: guarda nuestro id, pasa a Connected, inicializa
    //     PlayerController, SyncOrchestrator y el depurador de pipeline, reactiva los hooks
    //     de entidades si el juego ya está cargado y aplica la hora inicial del servidor.
    // EN: Handshake accepted: stores our id, moves to Connected, initializes
    //     PlayerController, SyncOrchestrator and the pipeline debugger, re-enables entity
    //     hooks if the game is already loaded and applies the server's initial time.
    static void HandleHandshakeAck(PacketReader& reader) {
        MsgHandshakeAck msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        auto& core = Core::Get();
        core.SetLocalPlayerId(msg.playerId);
        core.SetConnected(true);
        core.TransitionTo(ClientPhase::Connected);

        // ES: Quién es el host llega con S2C_HostAssignment; NO se deduce de currentPlayers.
        //     El servidor lo manda justo después de este ack cuando somos el host.
        // Host identity arrives via S2C_HostAssignment — do NOT guess from currentPlayers.
        // The server sends S2C_HostAssignment immediately after this ack when we're the host.

        // ES: Inicializar el controlador de jugadores con nuestro id y nombre.
        // Initialize the player controller with our ID and name
        core.GetPlayerController().InitializeLocalPlayer(msg.playerId, core.GetConfig().playerName);

        // ES: Inicializar el orquestador de sincronización.
        // Initialize sync orchestrator
        if (auto* so = core.GetSyncOrchestrator()) {
            so->Initialize(msg.playerId, core.GetConfig().playerName);
        }

        // ES: Reinicializar el depurador de pipeline con el id correcto
        //     (se construyó con id 0 en Core::Initialize, antes del handshake).
        // Re-initialize pipeline debugger with correct player ID
        // (it was constructed with ID=0 during Core::Initialize before handshake)
        core.GetPipelineOrch().Shutdown();
        core.GetPipelineOrch().Initialize(msg.playerId, core.GetEntityRegistry(),
            core.GetSpawnManager(), core.GetLoadingOrch(), core.GetClient(), core.GetNativeHud());

        // ES: Reactivar los hooks de entidades, pero SOLO si el mundo ya está cargado.
        //     Si se conecta desde el menú principal, activar ahora el hook CharacterCreate
        //     haría crashear durante las 130+ creaciones de la carga (corrupción del wrapper
        //     MovRaxRsp). En ese caso se deja para OnGameLoaded().
        // Re-enable entity hooks — but ONLY if the game world is already loaded.
        // If connecting from the main menu (game not loaded yet), enabling the
        // CharacterCreate hook now would crash during the 130+ loading creates
        // (MovRaxRsp wrapper corruption). Instead, defer to OnGameLoaded().
        if (core.IsGameLoaded()) {
            entity_hooks::ResumeForNetwork();
            spdlog::info("PacketHandler: Entity hooks resumed (game already loaded)");
        } else {
            spdlog::info("PacketHandler: Entity hooks DEFERRED (game not loaded yet — will resume on game load)");
            core.GetNativeHud().LogStep("NET", "Connected! Sync starts when you load a save.");
        }

        spdlog::info("PacketHandler: Handshake accepted! Player ID: {}, Players: {}/{}",
                     msg.playerId, msg.currentPlayers, msg.maxPlayers);

        core.GetNativeHud().LogStep("NET", "Connected! Player " + std::to_string(msg.playerId)
                              + " (" + std::to_string(msg.currentPlayers) + "/"
                              + std::to_string(msg.maxPlayers) + ")");
        core.GetOverlay().AddSystemMessage(
            "Connected to server! Player " + std::to_string(msg.playerId) +
            " (" + std::to_string(msg.currentPlayers) + "/" +
            std::to_string(msg.maxPlayers) + ")");

        // ES: Aplicar la sincronización de hora inicial que trae el handshake.
        // Apply initial time sync from handshake
        time_hooks::SetServerTime(msg.timeOfDay, 1.0f);
    }

    // ES: Handshake rechazado: muestra el motivo y vuelve a GameReady para reintentar.
    // EN: Handshake rejected: shows the reason and goes back to GameReady to retry.
    static void HandleHandshakeReject(PacketReader& reader) {
        MsgHandshakeReject msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        // ES: Asegurar el '\0' final: el servidor puede mandar el buffer de 128 bytes lleno
        //     sin terminador, y las operaciones de string leerían fuera de la estructura.
        // Ensure null-termination — server may send a full 128-byte buffer
        // without a trailing '\0', causing string ops to read past the struct.
        msg.reasonText[sizeof(msg.reasonText) - 1] = '\0';

        spdlog::warn("PacketHandler: Connection rejected (code={}): {}", msg.reasonCode, msg.reasonText);
        auto& core = Core::Get();

        // ES: Handshake rechazado: volver a GameReady para que el usuario pueda reintentar.
        // Handshake rejected — drop back to GameReady so user can retry
        if (core.IsGameLoaded()) {
            core.TransitionTo(ClientPhase::GameReady);
        }

        core.GetOverlay().AddSystemMessage(
            std::string("Connection rejected: ") + msg.reasonText);
        core.GetNativeHud().LogStep("ERR", std::string("Rejected: ") + msg.reasonText);
    }

    // ES: Un jugador remoto ha entrado: lo añade al overlay, a PlayerController y a
    //     PlayerEngine (ignorando nuestro propio aviso).
    // EN: A remote player joined: adds them to the overlay, PlayerController and
    //     PlayerEngine (ignoring our own notice).
    static void HandlePlayerJoined(PacketReader& reader) {
        MsgPlayerJoined msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        // ES: Asegurar el '\0' final: el servidor puede llenar los 32 bytes sin terminador.
        // Ensure null-termination — server may fill the entire 32-byte buffer
        // without a trailing '\0', causing string ops to read past the struct.
        msg.playerName[sizeof(msg.playerName) - 1] = '\0';

        auto& core = Core::Get();

        // ES: Saltarse a uno mismo: el servidor manda PlayerJoined a TODOS, incluido el emisor.
        //     Sin esto nos registraríamos como jugador "remoto".
        // Skip self — server broadcasts PlayerJoined to ALL clients including the sender.
        // Without this guard, we'd register ourselves as a "remote" player.
        if (msg.playerId == core.GetLocalPlayerId()) {
            spdlog::debug("PacketHandler: Ignoring own PlayerJoined (ID: {})", msg.playerId);
            return;
        }

        spdlog::info("PacketHandler: Player '{}' joined (ID: {})", msg.playerName, msg.playerId);
        core.GetOverlay().AddSystemMessage(
            std::string(msg.playerName) + " joined the game");
        core.GetNativeHud().AddSystemMessage(std::string(msg.playerName) + " joined the game");
        core.GetOverlay().AddPlayer({msg.playerId, msg.playerName, 0, false});
        core.GetPlayerController().RegisterRemotePlayer(msg.playerId, msg.playerName);

        // ES: Registrar en los motores del orquestador de sincronización.
        // Register with sync orchestrator engines
        if (auto* so = core.GetSyncOrchestrator()) {
            so->GetPlayerEngine().OnRemotePlayerJoined(msg.playerId, msg.playerName);
        }
    }

    // ES: Un jugador se ha ido: lo quita de overlay/PlayerController/motores, borra sus
    //     spawns pendientes y encola en el hilo del juego la limpieza de sus entidades.
    // EN: A player left: removes them from overlay/PlayerController/engines, clears their
    //     pending spawns and queues the cleanup of their entities on the game thread.
    static void HandlePlayerLeft(PacketReader& reader) {
        MsgPlayerLeft msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::info("PacketHandler: Player {} left (reason: {})", msg.playerId, msg.reason);

        auto& core = Core::Get();
        // ES: Obtener el nombre antes de borrarlo.
        // Get player name before removing
        auto* rp = core.GetPlayerController().GetRemotePlayer(msg.playerId);
        std::string leftName = rp ? rp->playerName : ("Player_" + std::to_string(msg.playerId));
        core.GetNativeHud().AddSystemMessage(leftName + " left the game");
        core.GetOverlay().RemovePlayer(msg.playerId);
        core.GetPlayerController().RemoveRemotePlayer(msg.playerId);

        // ES: Avisar a los motores del orquestador de sincronización.
        // Notify sync orchestrator engines
        if (auto* so = core.GetSyncOrchestrator()) {
            so->GetPlayerEngine().OnRemotePlayerLeft(msg.playerId);
            so->GetZoneEngine().RemovePlayer(msg.playerId);
            so->GetResolver().ClearInterest(msg.playerId);
        }

        // ES: Borrar las peticiones de spawn pendientes del jugador ANTES de limpiar sus
        //     entidades. Si no, esas peticiones huérfanas se procesarían después para un
        //     jugador que ya no existe.
        // Clear pending spawn requests for the departing player BEFORE cleaning
        // up spawned entities. Without this, orphaned spawn requests would be
        // processed later for a player who no longer exists.
        int clearedSpawns = core.GetSpawnManager().ClearSpawnsForOwner(msg.playerId);
        if (clearedSpawns > 0) {
            spdlog::info("PacketHandler: Cleared {} pending spawn(s) for departed player {}",
                         clearedSpawns, msg.playerId);
        }

        // ES: Limpiar todas las entidades del jugador desconectado. Como el hook
        //     CharacterDestroy está diferido (vtable[0]), no se puede confiar en el destructor
        //     del juego: se teletransportan los personajes bajo tierra (y = -10000) para que no
        //     se vean y se quitan del registro.
        // Clean up all entities owned by the disconnected player.
        // Since CharacterDestroy hook is deferred (vtable[0]), we can't rely on the game's
        // destructor here. Instead, teleport stale characters underground so they're not
        // visible, then unregister from tracking.
        // [Oleada A] Migrado a CommandQueue: WritePlayerControlled/WritePosition/Unmark tocan
        // memoria del motor → game thread. Captura SOLO POD (playerId); las entidades se
        // resuelven POR ID dentro de la lambda (registro vaciado → lista vacía, skip limpio).
        // EN: [Wave A] Moved to CommandQueue: WritePlayerControlled/WritePosition/Unmark touch
        //     engine memory → game thread. Captures ONLY POD (playerId); entities are resolved
        //     BY ID inside the lambda (emptied registry → empty list, clean skip).
        auto& registry = core.GetEntityRegistry();
        size_t entityCount = registry.GetPlayerEntities(msg.playerId).size();  // solo para el HUD
        const PlayerID leftPlayerId = msg.playerId;
        core.GetCommandQueue().Push({[leftPlayerId]() {
            auto& core = Core::Get();
            auto& registry = core.GetEntityRegistry();
            auto entities = registry.GetPlayerEntities(leftPlayerId);
            for (EntityID eid : entities) {
                void* gameObj = registry.GetGameObject(eid);
                if (gameObj) {
                    // ES: Quitar isPlayerControlled para que el personaje salga del panel de escuadrón del
                    //     host; este es el arreglo clave de "el host controla ambos personajes cuando se
                    //     desconecta el segundo cliente".
                    // Clear isPlayerControlled so the character is removed from the
                    // host's squad panel — this is the key fix for "host controls both
                    // characters after second client disconnects".
                    game::WritePlayerControlled(reinterpret_cast<uintptr_t>(gameObj), false);
                    // ES: Moverlo muy bajo tierra para que no se vea.
                    // Move character far underground so it's invisible
                    game::CharacterAccessor accessor(gameObj);
                    Vec3 underground(0.f, -10000.f, 0.f);
                    accessor.WritePosition(underground);
                    // ES: Quitar el seguimiento de control remoto de la IA (evita punteros obsoletos).
                    // Clear AI remote-control tracking to prevent stale pointer issues
                    ai_hooks::UnmarkRemoteControlled(gameObj);
                    spdlog::debug("PacketHandler: Cleared control + teleported entity {} underground", eid);
                }
                // ES: ARREGLO BUG 2: decrementar el límite de spawns por cada entidad remota quitada.
                // BUG 2 FIX: Decrement spawn cap for each removed remote entity
                entity_hooks::DecrementSpawnCount(leftPlayerId);
                core.GetInterpolation().RemoveEntity(eid);
                registry.Unregister(eid);
            }
            if (!entities.empty()) {
                spdlog::info("PacketHandler: Removed {} entities from player {}",
                             entities.size(), leftPlayerId);
            }
        }});
        if (entityCount > 0) {
            core.GetOverlay().AddSystemMessage(
                "Cleaned up " + std::to_string(entityCount) + " remote entities");
        }
    }

    // ES: ── Spawn de entidad ──
    //     Lee el mensaje (id, tipo, dueño, plantilla, posición, rotación comprimida,
    //     facción, nombre de plantilla opcional y estado extendido opcional: salud de 7
    //     partes + vivo). Si es nuestra, remapea el id local al del servidor. Si el juego no
    //     está listo, lo encola en DeferredSpawnQueue. Si no, registra la entidad remota y
    //     la enlaza con el personaje "Player N" del mod si existe; si no, pide un spawn a
    //     SpawnManager.
    // EN: ── Entity spawn ──
    //     Reads the message (id, type, owner, template, position, compressed rotation,
    //     faction, optional template name and optional extended state: 7-part health +
    //     alive). If it is ours, remaps the local id to the server one. If the game is not
    //     ready, queues it in DeferredSpawnQueue. Otherwise registers the remote entity and
    //     links it to the mod's "Player N" character if present; if not, requests a spawn
    //     from SpawnManager.
    // ── Entity Spawn ──
    static void HandleEntitySpawn(PacketReader& reader) {
        uint32_t entityId, templateId, factionId;
        uint8_t type;
        uint32_t ownerId;
        float px, py, pz;
        uint32_t compQuat;

        if (!reader.ReadU32(entityId)) return;
        if (!reader.ReadU8(type)) return;
        if (!reader.ReadU32(ownerId)) return;
        if (!reader.ReadU32(templateId)) return;
        if (!reader.ReadVec3(px, py, pz)) return;
        if (!reader.ReadU32(compQuat)) return;
        if (!reader.ReadU32(factionId)) return;

        // ES: Leer el nombre de plantilla opcional (string con prefijo de longitud u16 tras los campos fijos).
        // Read optional template name (length-prefixed string appended after fixed fields)
        std::string templateName;
        uint16_t nameLen = 0;
        if (reader.Remaining() >= 2) {
            reader.ReadU16(nameLen);
            if (nameLen > 0 && nameLen <= 255 && reader.Remaining() >= nameLen) {
                templateName.resize(nameLen);
                reader.ReadRaw(templateName.data(), nameLen);
            }
        }

        // ES: Leer el estado extendido opcional (flag = 1, 7 floats de salud y flag de vivo).
        // Read optional extended state (health + alive flag)
        bool hasExtended = false;
        float healthData[7] = {100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f};
        bool isAlive = true;
        if (reader.Remaining() >= 1) {
            uint8_t extFlag = 0;
            reader.ReadU8(extFlag);
            if (extFlag == 1 && reader.Remaining() >= 7 * 4 + 1) {
                hasExtended = true;
                for (int i = 0; i < 7; i++) reader.ReadF32(healthData[i]);
                uint8_t aliveFlag = 1;
                reader.ReadU8(aliveFlag);
                isAlive = (aliveFlag != 0);
            }
        }

        auto& core = Core::Get();
        auto& registry = core.GetEntityRegistry();
        Vec3 spawnPos(px, py, pz);
        Quat rot = Quat::Decompress(compQuat);

        // ES: Si es nuestra propia entidad confirmada por el servidor, remapear el id local al
        //     asignado por el servidor en vez de crear un duplicado.
        // If this is our own entity being confirmed by the server, remap the
        // local entity ID to the server-assigned ID instead of spawning a duplicate.
        if (ownerId == core.GetLocalPlayerId()) {
            EntityID localId = registry.FindLocalEntityNear(spawnPos, ownerId);
            if (localId != INVALID_ENTITY && localId != entityId) {
                if (registry.RemapEntityId(localId, entityId)) {
                    spdlog::info("PacketHandler: Remapped own entity {} -> server ID {}",
                                 localId, entityId);
                } else {
                    spdlog::warn("PacketHandler: Failed to remap own entity {} -> {}",
                                 localId, entityId);
                }
            } else if (localId == entityId) {
                // ES: Ya tiene el id correcto (poco probable pero posible).
                // Already has the correct ID (unlikely but possible)
                spdlog::debug("PacketHandler: Own entity {} already has correct server ID", entityId);
            } else {
                spdlog::warn("PacketHandler: No local entity found near ({:.1f},{:.1f},{:.1f}) to remap for server ID {}",
                             px, py, pz, entityId);
            }
            // ES: No crear nada: ya tenemos el personaje en el juego.
            return; // Don't spawn — we already have the character in-game
        }

        // ES: CRÍTICO: no crear jugadores remotos hasta que AMBOS clientes estén en partida.
        //     Se comprueba: juego cargado y fase >= GameReady.
        // CRITICAL: Don't spawn remote players until BOTH clients are in-game
        // Check: game loaded, world ready, local player exists
        if (!core.IsGameLoaded() || core.GetClientPhase() < ClientPhase::GameReady) {
            spdlog::warn("PacketHandler: Deferring entity spawn {} - game not ready (phase={})",
                         entityId, ClientPhaseToString(core.GetClientPhase()));

            // ES: ARREGLADO: encolar el spawn para procesarlo cuando el juego esté listo.
            // FIXED: Queue spawn for later processing when game ready
            DeferredSpawn ds;
            ds.entityId = entityId;
            // ES: generation se deja a 0 (el seguimiento de generaciones no está terminado).
            ds.generation = 0; // Will be populated once generation tracking is complete
            ds.type = type;
            ds.ownerId = ownerId;
            ds.templateId = templateId;
            ds.posX = px;
            ds.posY = py;
            ds.posZ = pz;
            ds.compressedQuat = compQuat;
            ds.factionId = factionId;
            ds.templateName = templateName;
            ds.hasExtendedHealth = hasExtended;
            ds.isAlive = isAlive;
            if (hasExtended) {
                for (int i = 0; i < 7; i++) ds.health[i] = healthData[i];
            }
            ds.timestamp = SessionTime();
            DeferredSpawnQueue::Queue(ds);
            return;
        }

        spdlog::info("PacketHandler: Entity spawn id={} type={} owner={} template='{}' at ({:.1f}, {:.1f}, {:.1f})",
                     entityId, type, ownerId, templateName, px, py, pz);

        // ES: Log en el HUD para verlo.
        // Log to HUD for visibility
        core.GetNativeHud().LogStep("NET", "Remote entity spawn: id=" + std::to_string(entityId)
                              + " owner=" + std::to_string(ownerId)
                              + " '" + templateName + "'");

        // ES: Guardar la posición del primer personaje remoto como punto de aparición del host.
        //     Cuando un cliente que se une recibe los spawns del host, se teletransporta ahí.
        // Save the first remote player character's position as host spawn point.
        // When a joiner receives entity spawns from the host, they'll teleport there.
        if (!core.HasHostSpawnPoint() && spawnPos.x != 0.f && spawnPos.z != 0.f) {
            core.SetHostSpawnPoint(spawnPos);
            spdlog::info("PacketHandler: Host spawn point set to ({:.1f}, {:.1f}, {:.1f})",
                         spawnPos.x, spawnPos.y, spawnPos.z);
            core.GetNativeHud().LogStep("GAME", "Host position: ("
                                  + std::to_string((int)spawnPos.x) + ","
                                  + std::to_string((int)spawnPos.y) + ","
                                  + std::to_string((int)spawnPos.z) + ")");
        }

        // ES: Registrar como entidad remota (gameObject=nullptr hasta que exista en el juego).
        // Register in entity registry as remote (gameObject=nullptr until spawned)
        registry.RegisterRemote(entityId, static_cast<EntityType>(type), ownerId, spawnPos);

        // ES: Volcar las posiciones encoladas que llegaron antes de este spawn.
        // Flush any queued position updates that arrived before this spawn message
        PendingSnapshotQueue::FlushForEntity(entityId);

        // ES: Añadir un snapshot inicial de interpolación.
        // Add initial interpolation snapshot
        float now = SessionTime();
        core.GetInterpolation().AddSnapshot(entityId, now, spawnPos, rot);

        // ES: ── Intentar primero con un personaje del mod ya existente ──
        //     kenshi-online.mod crea "Player 1" a "Player 16" al cargar la partida. Si el
        //     personaje ya está en el mundo se enlaza directamente, sin llamar a FactoryCreate
        //     (poco fiable y propenso a crashes).
        // ── Try to find existing mod character first ──
        // The kenshi-online.mod creates "Player 1" through "Player 16" on game load.
        // If the character already exists in the world, link it directly — no need
        // to call FactoryCreate (which is unreliable and crash-prone).
        bool linkedExisting = false;
        {
            // ES: Buscar el personaje del mod correspondiente al PlayerID del dueño.
            // Map owner PlayerID to mod character name
            void* existingChar = core.FindModCharacterBySlot(static_cast<int>(ownerId));

            // ES: Si ya está enlazado a otra entidad, no enlazarlo (evita referencias colgantes).
            // Check if already linked to a different entity — skip if so to avoid dangling refs
            EntityID existingLinkedId = existingChar ? registry.GetNetId(existingChar) : INVALID_ENTITY;
            if (existingChar && existingLinkedId != INVALID_ENTITY && existingLinkedId != entityId) {
                spdlog::warn("PacketHandler: Mod char 'Player {}' already linked to entity {}, "
                             "cannot link to new entity {}",
                             ownerId, existingLinkedId, entityId);
                existingChar = nullptr; // Don't link; fall through to spawn queue
            }

            if (existingChar) {
                // ES: ¡Encontrado! Enlazar el personaje del juego con esta entidad de red.
                // Found it! Link the existing game character to this network entity.
                registry.SetGameObject(entityId, existingChar);
                registry.UpdatePosition(entityId, spawnPos);

                // ES: Desactivar su IA para que lo controle la red.
                // Suppress AI so network controls this character
                ai_hooks::MarkRemoteControlled(existingChar);

                // ES: ── Inicialización completa (igual que en el spawn directo) ──
                //     Se usan ayudas SEH porque MSVC C2712 prohíbe __try con destructores C++.
                // ── Full initialization (same setup as direct spawn path) ──
                // SEH helpers used because MSVC C2712 forbids __try with C++ destructors.

                // ES: 1. Teletransportar a la última posición conocida (escribe en memoria del juego).
                //     Pueden haber llegado posiciones después del spawn: se usa la interpolada si la
                //     hay, si no la del spawn. SE ENCOLA: solo el hilo del juego escribe su memoria.
                // 1. Teleport to most recent known position (writes to actual game memory)
                // Position updates may have arrived after the spawn message — use the
                // latest interpolation data if available, falling back to spawn position.
                // QUEUE THIS: Write to game memory from game thread only
                {
                    Vec3 bestPos = spawnPos;
                    Quat bestRot = rot;
                    uint8_t ms = 0, as = 0;
                    float now = SessionTime();
                    if (core.GetInterpolation().GetInterpolated(entityId, now, bestPos, bestRot, ms, as)) {
                        // ES: Usar la posición interpolada, más reciente.
                        // Use the more recent interpolated position
                        registry.UpdatePosition(entityId, bestPos);
                    }
                    if (core.IsGameLoaded() && (bestPos.x != 0.f || bestPos.y != 0.f || bestPos.z != 0.f)) {
                        // ES: ARREGLADO: encolar la escritura de posición para el hilo del juego; se vuelve a
                        //     comprobar dentro de la lambda que el juego sigue cargado.
                        // FIXED: Enqueue position write for game thread execution
                        // Check game loaded AGAIN in lambda (may have changed)
                        // Resolución POR ID dentro de la lambda (no capturar el puntero crudo):
                        // en desconexión, SetConnected(false) vacía el registry desde otro hilo;
                        // con el puntero capturado por valor eso era un use-after-free potencial.
                        // EN: Resolve BY ID inside the lambda (do not capture the raw pointer): on disconnect,
                        //     SetConnected(false) empties the registry from another thread; capturing the
                        //     pointer by value was a potential use-after-free.
                        core.GetCommandQueue().Push({[bestPos, entityId]() {
                            auto& core = Core::Get();
                            if (core.IsGameLoaded() && core.GetClientPhase() >= ClientPhase::GameReady) {
                                void* ch = core.GetEntityRegistry().GetGameObject(entityId);
                                if (!ch) return;  // registro vaciado/entidad eliminada → skip limpio
                                if (!SEH_WritePositionToChar(ch, bestPos.x, bestPos.y, bestPos.z)) {
                                    spdlog::warn("PacketHandler: WritePosition failed for linked mod char entity {}", entityId);
                                }
                            }
                        }});
                    }
                }

                // ── FIX CRASH 2º JUGADOR: migración a GameCommandQueue ──
                // Antes, los puntos 2,3,4,6 tocaban memoria del motor / llamaban
                // funciones nativas DIRECTAMENTE desde el hilo de RED mientras el
                // hilo de render usaba esa misma memoria -> data race -> crash.
                // Ahora se encolan en UN solo Push que el game thread drena en
                // core.cpp:6680. Captura SIEMPRE por VALOR: el frame de red ya
                // ha muerto cuando el comando se ejecuta en el game thread.
                // EN: ── 2nd PLAYER CRASH FIX: moved to GameCommandQueue ──
                //     Previously steps 2,3,4,6 touched engine memory / called native functions
                //     DIRECTLY from the NETWORK thread while the render thread used that memory ->
                //     data race -> crash. Now they go into ONE Push drained by the game thread (in
                //     core.cpp; the "core.cpp:6680" line number may be out of date). ALWAYS capture BY
                //     VALUE: the network frame is gone when the command runs on the game thread.

                // El array C float[7] no es capturable por valor en una lambda,
                // así que lo copiamos a un std::array (sí capturable).
                // EN: A C float[7] array cannot be captured by value in a lambda, so it is copied into
                //     a std::array (which can).
                std::array<float, 7> healthCopy{};
                for (int i = 0; i < 7; i++) healthCopy[i] = healthData[i];

                // 4a. Actualizar registry con la salud (registry tiene su propio
                //     lock interno -> seguro en hilo de red, queda FUERA del Push).
                // EN: 4a. Update the registry with health (the registry has its own lock -> safe on
                //     the network thread, stays OUTSIDE the Push).
                if (hasExtended) {
                    registry.UpdateLimbHealth(entityId, healthData);
                }

                // Encolar TODO el trabajo que toca memoria del motor / nativo.
                // Resolución POR ID dentro de la lambda — no capturar el puntero crudo.
                // EN: Queue ALL the work that touches engine memory / native code.
                //     Resolve BY ID inside the lambda; do not capture the raw pointer.
                core.GetCommandQueue().Push({[entityId, ownerId, healthCopy, hasExtended]() {
                    auto& core = Core::Get();
                    auto& registry = core.GetEntityRegistry();

                    // REVALIDAR dentro del game thread: el juego pudo descargarse,
                    // desconectarse o el char pudo re-vincularse a otra entidad
                    // entre el encolado (hilo red) y el drenaje (hilo juego).
                    // GetGameObject(id) devuelve null si el registry fue vaciado.
                    // EN: REVALIDATE on the game thread: the game may have unloaded, disconnected, or the
                    //     character may have been relinked to another entity between queueing (network
                    //     thread) and draining (game thread). GetGameObject(id) returns null if the
                    //     registry was emptied.
                    void* existingChar = registry.GetGameObject(entityId);
                    if (!core.IsGameLoaded() ||
                        core.GetClientPhase() < ClientPhase::GameReady ||
                        existingChar == nullptr) {
                        spdlog::warn("PacketHandler: Spawn-init cmd descartado (revalidación falló) entity {}", entityId);
                        return;
                    }

                    // 2. Renombrar al nombre del jugador + track en PlayerController
                    // EN: 2. Rename to the player's name + track in PlayerController.
                    if (!SEH_OnRemoteCharSpawned(entityId, existingChar, ownerId)) {
                        spdlog::warn("PacketHandler: OnRemoteCharacterSpawned failed for linked mod char entity {}", entityId);
                    }
                    // Escribir nombre de plantilla GameData (chars mod tienen plantilla única por slot)
                    // EN: Write the GameData template name (mod chars have a unique template per slot).
                    core.GetPlayerController().WriteGameDataNameForModLink(existingChar, ownerId);

                    // 3. Programar la sonda diferida de AnimClass (pipeline de animación)
                    // EN: 3. Schedule the deferred AnimClass probe (animation pipeline).
                    if (!SEH_ScheduleAnimProbe(existingChar)) {
                        spdlog::warn("PacketHandler: AnimClassProbe failed for linked mod char entity {}", entityId);
                    }

                    // 4b. Escribir salud por miembro a la memoria del char
                    // EN: 4b. Write per-limb health into the character's memory.
                    if (hasExtended) {
                        if (!SEH_WriteLimbHealthToChar(existingChar, healthCopy.data())) {
                            spdlog::warn("PacketHandler: Health write failed for linked mod char entity {}", entityId);
                        }
                    }

                    // 6. SEH_AllyModFaction DESACTIVADO — es el faction-UAF (game+0x927E94),
                    //    el gatillo más letal del crash del 2º jugador. El char remoto
                    //    queda con su facción de fábrica (nameplate neutral, cosmético).
                    // SEH_AllyModFaction(existingChar);  // <-- NO LLAMAR
                // EN: 6. SEH_AllyModFaction DISABLED: it is the faction UAF (game+0x927E94), the most
                //     lethal trigger of the 2nd player crash. The remote char keeps its factory faction
                //     (neutral nameplate, cosmetic only).
                }});

                // 5. Crear VisualProxy (tiene su propio estado, hilo de red OK -> FUERA del Push)
                // EN: 5. Create the VisualProxy (has its own state, network thread OK -> OUTSIDE the Push).
                {
                    auto* rp = core.GetPlayerController().GetRemotePlayer(ownerId);
                    std::string displayName = rp ? rp->playerName : ("Player " + std::to_string(ownerId));
                    core.GetVisualProxy().CreateProxy(entityId, ownerId, displayName,
                        "", spawnPos, rot);
                }

                linkedExisting = true;
                spdlog::info("PacketHandler: LINKED existing mod character 'Player {}' "
                             "to entity {} — full init (pos, name, anim, health={}, alive={})",
                             ownerId, entityId, hasExtended, isAlive);
                core.GetNativeHud().AddSystemMessage(
                    "Player " + std::to_string(ownerId) + " linked!");
            }
        }

        // ES: ── Reserva: pedir un spawn a SpawnManager ──
        //     Si no hay personaje del mod (mod no cargado o la partida no los tiene), se usa
        //     el pipeline de spawn con FactoryCreate. El nombre de plantilla se pasa de UTF-8
        //     a ANSI (codificación que usa el juego).
        // ── Fallback: queue a spawn via SpawnManager ──
        // If no existing mod character found (maybe mod not loaded, or save doesn't
        // have the characters), fall back to the FactoryCreate spawn pipeline.
        if (!linkedExisting) {
            auto& spawnMgr = core.GetSpawnManager();
            SpawnRequest req;
            req.netId        = entityId;
            req.owner        = ownerId;
            req.type         = static_cast<EntityType>(type);
            req.templateName = Utf8ToAnsi(templateName.c_str(), (int)templateName.size());
            req.position     = spawnPos;
            req.rotation     = rot;
            req.templateId   = templateId;
            req.factionId    = factionId;
            req.hasExtendedState = hasExtended;
            if (hasExtended) {
                for (int i = 0; i < 7; i++) req.health[i] = healthData[i];
                req.alive = isAlive;
            }
            spawnMgr.QueueSpawn(req);

            auto* rp = core.GetPlayerController().GetRemotePlayer(ownerId);
            std::string ownerName = rp ? rp->playerName : ("Player_" + std::to_string(ownerId));
            if (spawnMgr.IsReady()) {
                core.GetNativeHud().AddSystemMessage("Spawning " + ownerName + "'s character...");
            } else {
                core.GetNativeHud().AddSystemMessage("Queued " + ownerName + "'s character (waiting for game event)...");
                spdlog::info("PacketHandler: SpawnManager not ready yet — entity {} queued for deferred spawn", entityId);
            }
        }
    }

    // ES: Limpieza de despawn protegida con SEH: teletransporta bajo tierra y quita la marca
    //     de control remoto. El comentario original dice que corre en el hilo de red, pero
    //     ahora se llama desde la lambda de la CommandQueue (hilo del juego). Sin SEH, una
    //     violación de acceso aquí mataría el hilo que la ejecuta.
    // SEH-protected despawn cleanup — runs on network thread where the game may
    // have already freed the character object. Without SEH, an AV here cascades
    // through enet_host_service and kills the network thread permanently.
    static void SEH_DespawnCleanup(void* gameObj) {
        __try {
            // ES: Teletransportar bajo tierra para que no se vea.
            // Teleport underground so the character is not visible
            game::CharacterAccessor accessor(gameObj);
            Vec3 underground(0.f, -10000.f, 0.f);
            accessor.WritePosition(underground);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // ES: El personaje ya estaba liberado: nada que limpiar.
            // Character already freed — nothing to clean up
        }
        __try {
            ai_hooks::UnmarkRemoteControlled(gameObj);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ES: Despawn de entidad: encola en el hilo del juego la limpieza (liberar hueco de
    //     spawn, esconder el personaje, quitarlo de interpolación y registro).
    // EN: Entity despawn: queues on the game thread the cleanup (release spawn slot, hide
    //     the character, remove it from interpolation and the registry).
    static void HandleEntityDespawn(PacketReader& reader) {
        uint32_t entityId;
        uint8_t reason = 0;
        if (!reader.ReadU32(entityId)) return;
        // ES: El motivo es opcional.
        reader.ReadU8(reason); // optional

        spdlog::info("PacketHandler: Entity despawn id={} reason={}", entityId, reason);

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;

        // [Oleada A] Migrado a CommandQueue: SEH_DespawnCleanup toca memoria del motor →
        // game thread. Captura SOLO POD (entityId); resolución POR ID dentro de la lambda.
        // Todo el bloque va junto para preservar el orden (cleanup ANTES de Unregister).
        // EN: [Wave A] Moved to CommandQueue: SEH_DespawnCleanup touches engine memory → game
        //     thread. Captures ONLY POD (entityId); resolved BY ID inside the lambda. The whole
        //     block stays together to keep the order (cleanup BEFORE Unregister).
        core.GetCommandQueue().Push({[entityId]() {
            auto& core = Core::Get();
            auto& registry = core.GetEntityRegistry();

            // ES: ARREGLO BUG 2: decrementar el límite de spawns del jugador ANTES de borrar la
            //     entidad, mientras aún se conoce su dueño.
            // BUG 2 FIX: Decrement per-player spawn cap BEFORE unregistering
            // so the entity info (owner) is still available.
            auto info = registry.GetInfo(entityId);
            if (info.has_value() && info->isRemote) {
                entity_hooks::DecrementSpawnCount(info->ownerPlayerId);
            }

            void* gameObj = registry.GetGameObject(entityId);
            if (gameObj) {
                SEH_DespawnCleanup(gameObj);
            }
            registry.SetGameObject(entityId, nullptr);
            core.GetInterpolation().RemoveEntity(entityId);
            registry.Unregister(entityId);
        }});
    }

    // ES: ── Actualización de posiciones ──
    //     Formato: jugador origen (u32), número (u8) y N CharacterPosition crudos. Cada uno
    //     pasa por AuthorityValidator: se aplica al interpolador (remotas), se ignora
    //     (propias, sin reconciliación todavía), se encola (entidad aún no creada) o se rechaza.
    // EN: ── Position update ──
    //     Format: source player (u32), count (u8) and N raw CharacterPosition. Each goes
    //     through AuthorityValidator: fed to the interpolator (remote), ignored (own, no
    //     reconciliation yet), queued (entity not created yet) or rejected.
    // ── Position Update ──
    static void HandlePositionUpdate(PacketReader& reader) {
        uint32_t sourcePlayer;
        uint8_t count;
        if (!reader.ReadU32(sourcePlayer)) return;
        if (!reader.ReadU8(count)) return;

        auto& core = Core::Get();
        auto& registry = core.GetEntityRegistry();
        auto& interp = core.GetInterpolation();
        uint32_t myPlayerId = core.GetLocalPlayerId();
        float now = SessionTime();

        // ES: Estadísticas para depuración.
        // Stats for debugging
        uint32_t appliedRemote = 0;
        uint32_t reconciledLocal = 0;
        uint32_t queuedPending = 0;
        uint32_t rejected = 0;

        bool fedSharedSync = false;
        for (uint8_t i = 0; i < count; i++) {
            CharacterPosition pos;
            if (!reader.ReadRaw(&pos, sizeof(pos))) break;

            // ES: Validar la posición: rechazar NaN/Inf para no corromper la interpolación.
            // Validate position: reject NaN/Inf to prevent corrupting interpolation state
            if (std::isnan(pos.posX) || std::isnan(pos.posY) || std::isnan(pos.posZ) ||
                std::isinf(pos.posX) || std::isinf(pos.posY) || std::isinf(pos.posZ)) {
                spdlog::warn("PacketHandler: Skipping entity {} — position contains NaN/Inf", pos.entityId);
                rejected++;
                continue;
            }

            // ES: === VALIDACIÓN DE AUTORIDAD ===
            // === AUTHORITY VALIDATION ===
            SnapshotDecision decision = AuthorityValidator::ValidateInboundSnapshot(
                pos, sourcePlayer, myPlayerId, registry
            );

            Vec3 position(pos.posX, pos.posY, pos.posZ);
            Quat rotation = Quat::Decompress(pos.compressedQuat);

            switch (decision) {
                case SnapshotDecision::ApplyRemote:
                    interp.AddSnapshot(pos.entityId, now, position, rotation,
                                       pos.moveSpeed, pos.animStateId);
                    appliedRemote++;

                    // ES: Alimentar la sincronización de partida compartida: solo la PRIMERA entidad de
                    //     OTRO jugador. sourcePlayer=0 es un envío del servidor; solo interesan posiciones de
                    //     jugadores remotos reales, y solo su primera entidad (su personaje principal).
                    // Feed shared-save sync: only the FIRST entity from a DIFFERENT player.
                    // sourcePlayer=0 means server-broadcast. We only want positions from
                    // actual remote players, and only the first entity (their main character).
                    if (!fedSharedSync && sourcePlayer != 0 &&
                        sourcePlayer != myPlayerId &&
                        (position.x != 0.f || position.y != 0.f || position.z != 0.f)) {
                        shared_save_sync::OnRemotePositionReceived(position);
                        fedSharedSync = true;
                    }
                    break;

                case SnapshotDecision::ReconcileLocal:
                    // ES: PENDIENTE: reconciliación de la predicción (Fase 7). De momento se ignora
                    //     para evitar tirones ("rubber-banding").
                    // TODO: Implement prediction reconciliation (Phase 7)
                    // For now, skip to prevent rubber-banding
                    reconciledLocal++;
                    break;

                case SnapshotDecision::QueuePendingSpawn:
                    PendingSnapshotQueue::Queue(pos, sourcePlayer);
                    queuedPending++;
                    break;

                default:
                    // ES: Rechazar el resto de casos.
                    // Reject all other cases
                    rejected++;
                    break;
            }
        }

        // ES: Log de estadísticas.
        // Log stats
        if ((appliedRemote + rejected) > 0) {
            spdlog::debug("HandlePositionUpdate: applied={} reconciled={} queued={} rejected={}",
                          appliedRemote, reconciledLocal, queuedPending, rejected);
        }

        // ES: Actualizar la actividad del jugador en PlayerEngine (el comentario original dice
        //     "posición + actividad", pero solo se registra actividad).
        // Update player engine with position + activity
        if (auto* so = core.GetSyncOrchestrator()) {
            so->GetPlayerEngine().RecordActivity(sourcePlayer);
        }
    }

    // ES: ── Orden de movimiento ── Como no se puede llamar a MoveTo del juego, se mete el
    //     destino como snapshot de interpolación 1 s en el futuro (movimiento suave).
    // EN: ── Move command ── Since the game's MoveTo cannot be called, the target is added
    //     as an interpolation snapshot 1 s in the future (smooth movement).
    // ── Move Command ──
    static void HandleMoveCommand(PacketReader& reader) {
        MsgMoveCommand msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::debug("PacketHandler: Move command for entity {} to ({:.1f}, {:.1f}, {:.1f})",
                      msg.entityId, msg.targetX, msg.targetY, msg.targetZ);

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;
        auto& registry = core.GetEntityRegistry();
        auto& funcs = core.GetGameFunctions();

        // ES: La dirección de MoveTo está a mitad de función (no es seguro llamarla).
        //     Se usa el sistema de interpolación para un movimiento suave.
        // MoveTo function address is mid-function (not safe to call).
        // Use interpolation system to handle smooth movement instead.
        {
            float now = SessionTime();
            Vec3 target(msg.targetX, msg.targetY, msg.targetZ);
            core.GetInterpolation().AddSnapshot(msg.entityId, now + 1.0f, target, Quat());
        }
    }

    // ES: ── Golpe de combate ── Aplica daño a la entidad objetivo en el hilo del juego:
    //     con la función nativa ApplyDamage si hay atacante válido, o como alternativa
    //     escribiendo la salud resultante de la parte golpeada.
    // EN: ── Combat hit ── Applies damage to the target entity on the game thread: with the
    //     native ApplyDamage function if there is a valid attacker, or otherwise by writing
    //     the resulting health of the hit body part.
    // ── Combat Hit ──
    static void HandleCombatHit(PacketReader& reader) {
        MsgCombatHit msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        // ES: Validar la parte del cuerpo para evitar escrituras fuera de rango.
        // Validate body part to prevent out-of-bounds memory writes
        if (msg.bodyPart >= static_cast<uint8_t>(BodyPart::Count)) return;

        spdlog::debug("PacketHandler: Combat hit {} -> {} part={} dmg=({:.1f},{:.1f},{:.1f}) hp={:.1f}",
                      msg.attackerId, msg.targetId, msg.bodyPart,
                      msg.cutDamage, msg.bluntDamage, msg.pierceDamage, msg.resultHealth);

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;
        auto& registry = core.GetEntityRegistry();
        auto& funcs = core.GetGameFunctions();

        // Early-out barato en hilo de red (el registry tiene lock propio); la lambda
        // vuelve a resolver por ID en el game thread (nunca captura el puntero crudo).
        // EN: Cheap early-out on the network thread (the registry has its own lock); the lambda
        //     resolves by ID again on the game thread (never captures the raw pointer).
        if (!registry.GetGameObject(msg.targetId)) return;

        // ES: Intentar aplicar el daño con la función nativa del juego. ApplyDamage desreferencia
        //     al atacante, así que solo se llama con un atacante válido.
        //     ARREGLADO: todas las escrituras en memoria del juego se encolan para el hilo del juego.
        // Try to apply damage via the game's native damage function.
        // ApplyDamage dereferences attacker, so only call with a valid attacker.
        // FIXED: Queue all game memory writes for game thread execution.
        // Captura SOLO POD por valor; punteros resueltos POR ID dentro de la lambda.
        core.GetCommandQueue().Push({[msg]() {
            auto& core = Core::Get();
            auto& funcs = core.GetGameFunctions();
            auto& registry = core.GetEntityRegistry();
            bool appliedViaFunction = false;

            // Resolver el target POR ID dentro del game thread — registro vaciado → skip limpio.
            // EN: Resolve the target BY ID on the game thread; emptied registry → clean skip.
            void* targetObj = registry.GetGameObject(msg.targetId);
            if (!targetObj) return;

            if (funcs.ApplyDamage) {
                void* attackerObj = (msg.attackerId != INVALID_ENTITY)
                    ? registry.GetGameObject(msg.attackerId) : nullptr;
                if (attackerObj) {
                    auto damageFn = reinterpret_cast<ApplyDamageFn>(funcs.ApplyDamage);
                    __try {
                        damageFn(targetObj, attackerObj, msg.bodyPart,
                                 msg.cutDamage, msg.bluntDamage, msg.pierceDamage);
                        appliedViaFunction = true;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        spdlog::warn("PacketHandler: Native ApplyDamage crashed for {} -> {} — using fallback",
                                     msg.attackerId, msg.targetId);
                    }
                }
            }

            // Fallback: escribir la salud de UNA parte por la cadena canónica MedicalSystem.
            // EN: Fallback: write the health of ONE part through the canonical MedicalSystem chain.
            if (!appliedViaFunction) {
                SEH_WriteOnePartHealth(targetObj, static_cast<int>(msg.bodyPart), msg.resultHealth);
            }
        }});

        // ES: Actualizar en el registro de entidades la salud nueva de esa parte.
        // Update entity registry with the new limb health value
        {
            auto info = registry.GetInfo(msg.targetId);
            if (info.has_value()) {
                float limbs[7];
                for (int i = 0; i < 7; i++) limbs[i] = info->limbs.hp[i];
                if (msg.bodyPart < 7) limbs[msg.bodyPart] = msg.resultHealth;
                registry.UpdateLimbHealth(msg.targetId, limbs);
            }
        }
    }

    // ES: ── Muerte en combate ── En el hilo del juego llama a la función nativa
    //     CharacterDeath (marcando el eco para que nuestro hook no la reenvíe al servidor);
    //     si falla, pone las 7 partes a -100 para que el propio juego detecte la muerte.
    // EN: ── Combat death ── On the game thread calls the native CharacterDeath function
    //     (flagging the echo so our hook does not resend it to the server); if it fails,
    //     sets all 7 parts to -100 so the game itself detects the death.
    // ── Combat Death ──
    static void HandleCombatDeath(PacketReader& reader) {
        MsgCombatDeath msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::info("PacketHandler: Entity {} killed by {}", msg.entityId, msg.killerId);

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;
        auto& registry = core.GetEntityRegistry();
        auto& funcs = core.GetGameFunctions();

        // Early-out barato en hilo de red; la lambda re-resuelve por ID en el game thread.
        // EN: Cheap early-out on the network thread; the lambda re-resolves by ID on the game thread.
        if (!registry.GetGameObject(msg.entityId)) return;

        if (funcs.CharacterDeath) {
            // ES: ARREGLADO: encolar la muerte para el hilo del juego.
            // FIXED: Queue death execution for game thread.
            // Captura SOLO POD (msg); entityObj/killerObj resueltos POR ID dentro de la lambda.
            // EN: Captures ONLY POD (msg); entityObj/killerObj resolved BY ID inside the lambda.
            core.GetCommandQueue().Push({[msg]() {
                auto& core = Core::Get();
                auto& funcs = core.GetGameFunctions();
                auto& registry = core.GetEntityRegistry();
                bool appliedNative = false;

                // Resolver POR ID dentro del game thread — registro vaciado → skip limpio.
                // EN: Resolve BY ID on the game thread; emptied registry → clean skip.
                void* entityObj = registry.GetGameObject(msg.entityId);
                if (!entityObj) return;
                void* killerObj = (msg.killerId != INVALID_ENTITY)
                    ? registry.GetGameObject(msg.killerId) : nullptr;

                // ES: Intentar la función nativa de muerte. Se pasa killerObj aunque sea nullptr: la
                //     CharacterDeath del juego (en 0x7A6200) acepta asesino nulo (muertes por entorno o
                //     desangrado lo usan). Se activa la supresión de eco para que Hook_CharacterDeath
                //     no reencole esta muerte como un evento C2S nuevo.
                // Try native death fn. Pass killerObj even if nullptr — the
                // game's CharacterDeath at 0x7A6200 accepts nullptr killer
                // (environment/bleed-out deaths use nullptr internally).
                // Set echo suppression flag so Hook_CharacterDeath doesn't
                // re-queue this death as a new C2S event.
                auto deathFn = reinterpret_cast<CharacterDeathFn>(funcs.CharacterDeath);
                bool deathCrashed = false;
                combat_hooks::SetServerSourcedDeath(true);
                __try {
                    deathFn(entityObj, killerObj);
                    appliedNative = true;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    deathCrashed = true;
                }
                combat_hooks::SetServerSourcedDeath(false);
                if (deathCrashed) {
                    spdlog::warn("PacketHandler: Native CharacterDeath crashed for entity {} — using fallback",
                                 msg.entityId);
                }

                if (!appliedNative) {
                    // Fallback: poner las 7 partes a -100 por la cadena canónica MedicalSystem.
                    // Esto dispara la detección de muerte propia del juego (salud < 0 en parte vital).
                    // EN: Fallback: set all 7 parts to -100 through the canonical MedicalSystem chain. This
                    //     triggers the game's own death detection (health < 0 on a vital part).
                    float deathHealth[7];
                    for (int i = 0; i < 7; i++) deathHealth[i] = -100.f;
                    if (SEH_WriteLimbHealthToChar(entityObj, deathHealth)) {
                        spdlog::info("PacketHandler: Death fallback — set all body parts to -100 for entity {}", msg.entityId);
                    }

                    // ES: Probar también el flag isAlive si algún día se descubre su offset.
                    // Also try isAlive flag if we ever discover it
                    auto& offsets = game::GetOffsets().character;
                    if (offsets.isAlive >= 0) {
                        bool dead = false;
                        Memory::Write(reinterpret_cast<uintptr_t>(entityObj) + offsets.isAlive, dead);
                    }
                }
            }});
        }

        // ES: Actualizar el registro: todas las partes a -100 por la muerte.
        // Update entity registry — all limbs to -100 for death
        {
            float limbs[7];
            for (int i = 0; i < 7; i++) limbs[i] = -100.f;
            registry.UpdateLimbHealth(msg.entityId, limbs);
        }
    }

    // ES: ── KO en combate ── En el hilo del juego llama a la función nativa CharacterKO
    //     (con supresión de eco); si falla, escribe la salud del pecho recibida.
    // EN: ── Combat KO ── On the game thread calls the native CharacterKO function (with
    //     echo suppression); if it fails, writes the received chest health.
    // ── Combat KO ──
    static void HandleCombatKO(PacketReader& reader) {
        MsgCombatKO msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        // ES: Validar la parte del cuerpo para evitar escrituras fuera de rango.
        // Validate body part to prevent out-of-bounds memory writes
        if (msg.bodyPart >= static_cast<uint8_t>(BodyPart::Count)) return;

        spdlog::info("PacketHandler: Entity {} KO'd by {} (part={}, hp={:.1f})",
                     msg.entityId, msg.attackerId, msg.bodyPart, msg.resultHealth);

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;
        auto& registry = core.GetEntityRegistry();
        auto& funcs = core.GetGameFunctions();

        // [Oleada A] Migrado a CommandQueue: la llamada nativa KO y el fallback de salud
        // tocaban memoria del motor DIRECTAMENTE desde el hilo de RED. Ahora todo corre en
        // el game thread, con captura SOLO POD (msg) y resolución POR ID dentro de la lambda.
        // EN: [Wave A] Moved to CommandQueue: the native KO call and the health fallback used to
        //     touch engine memory DIRECTLY from the NETWORK thread. Now everything runs on the
        //     game thread, capturing ONLY POD (msg) and resolving BY ID inside the lambda.
        if (registry.GetGameObject(msg.entityId)) {
            core.GetCommandQueue().Push({[msg]() {
                auto& core = Core::Get();
                auto& funcs = core.GetGameFunctions();
                auto& registry = core.GetEntityRegistry();

                // Resolver POR ID dentro del game thread — registro vaciado → skip limpio.
                // EN: Resolve BY ID on the game thread; emptied registry → clean skip.
                void* entityObj = registry.GetGameObject(msg.entityId);
                if (!entityObj) return;

                bool appliedNativeKO = false;
                if (funcs.CharacterKO) {
                    void* attackerObj = (msg.attackerId != INVALID_ENTITY)
                        ? registry.GetGameObject(msg.attackerId) : nullptr;
                    // ES: Activar la supresión de eco para que Hook_CharacterKO no reencole este KO como
                    //     un evento C2S nuevo.
                    // Set echo suppression flag so Hook_CharacterKO doesn't
                    // re-queue this KO as a new C2S event.
                    auto koFn = reinterpret_cast<game::func_types::CharacterKOFn>(funcs.CharacterKO);
                    bool koCrashed = false;
                    combat_hooks::SetServerSourcedKO(true);
                    __try {
                        koFn(entityObj, attackerObj, static_cast<int>(msg.bodyPart));
                        appliedNativeKO = true;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        koCrashed = true;
                    }
                    combat_hooks::SetServerSourcedKO(false);
                    if (koCrashed) {
                        spdlog::warn("PacketHandler: Native CharacterKO crashed for entity {} — using fallback",
                                     msg.entityId);
                    }
                }
                if (!appliedNativeKO) {
                    // Fallback: msg.bodyPart es en realidad el MOTIVO del KO (0=blood loss,
                    // 1=head trauma, 2=other), NO un índice de parte. La salud enviada es
                    // siempre la del pecho → escribir la parte 0 (chest) por la cadena canónica.
                    // EN: Fallback: msg.bodyPart is actually the KO REASON (0=blood loss, 1=head trauma,
                    //     2=other), NOT a part index. The health sent is always the chest's → write part 0
                    //     (chest) through the canonical chain.
                    SEH_WriteOnePartHealth(entityObj, 0, msg.resultHealth);
                }
            }});
        }

        // ES: Actualizar el registro: salud del KO en el pecho (parte 0).
        // Update entity registry — write KO health to chest (body part 0)
        {
            auto info = registry.GetInfo(msg.entityId);
            if (info.has_value()) {
                float limbs[7];
                for (int i = 0; i < 7; i++) limbs[i] = info->limbs.hp[i];
                limbs[static_cast<int>(BodyPart::Chest)] = msg.resultHealth;
                registry.UpdateLimbHealth(msg.entityId, limbs);
            }
        }
    }

    // ES: ── Postura de combate ── Solo se registra en el log: la postura afecta a la IA, que
    //     es autoritativa en el servidor, así que no se escribe en memoria del juego.
    // EN: ── Combat stance ── Only logged: stance affects AI, which is server-authoritative,
    //     so nothing is written to game memory.
    // ── Combat Stance ──
    static void HandleCombatStance(PacketReader& reader) {
        // ES: El servidor reutiliza el tipo S2C_CombatBlock para mandar MsgCombatStance.
        // Server reuses S2C_CombatBlock message type to carry MsgCombatStance data
        MsgCombatStance msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::debug("PacketHandler: Entity {} stance -> {}", msg.entityId, msg.stance);

        // Stance changes don't require writing to game memory — they affect AI
        // behavior which is server-authoritative. Log for awareness.
    }

    // ES: ── Actualización de estadística ── Traduce el índice de estadística (0-22) al
    //     offset de ese campo dentro de la estructura de estadísticas del personaje y encola
    //     la escritura del valor en el hilo del juego.
    // EN: ── Stat update ── Maps the stat index (0-22) to that field's offset inside the
    //     character's stats struct and queues writing the value on the game thread.
    // ── Stat Update ──
    static void HandleStatUpdate(PacketReader& reader) {
        MsgStatUpdate msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::debug("PacketHandler: Stat update entity={} stat={} value={:.1f}",
                      msg.entityId, msg.statIndex, msg.statValue);

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;
        if (!core.GetEntityRegistry().GetGameObject(msg.entityId)) return;

        // ES: Cada estadística es un float en statsPtr + offset; el índice se corresponde con
        //     los campos de StatsOffsets.
        // Each stat is a float at statsPtr + (statIndex * 4)
        // The stat index maps to the StatsOffsets fields
        auto& offsets = game::GetOffsets().stats;
        int statOffset = -1;

        switch (msg.statIndex) {
            case 0:  statOffset = offsets.meleeAttack;   break;
            case 1:  statOffset = offsets.meleeDefence;  break;
            case 2:  statOffset = offsets.dodge;         break;
            case 3:  statOffset = offsets.martialArts;   break;
            case 4:  statOffset = offsets.strength;      break;
            case 5:  statOffset = offsets.toughness;     break;
            case 6:  statOffset = offsets.dexterity;     break;
            case 7:  statOffset = offsets.athletics;     break;
            case 8:  statOffset = offsets.crossbows;     break;
            case 9:  statOffset = offsets.turrets;       break;
            case 10: statOffset = offsets.precision;     break;
            case 11: statOffset = offsets.stealth;       break;
            case 12: statOffset = offsets.assassination; break;
            case 13: statOffset = offsets.lockpicking;   break;
            case 14: statOffset = offsets.thievery;      break;
            case 15: statOffset = offsets.science;       break;
            case 16: statOffset = offsets.engineering;   break;
            case 17: statOffset = offsets.medic;         break;
            case 18: statOffset = offsets.farming;       break;
            case 19: statOffset = offsets.cooking;       break;
            case 20: statOffset = offsets.weaponsmith;   break;
            case 21: statOffset = offsets.armoursmith;   break;
            case 22: statOffset = offsets.labouring;     break;
            default:
                spdlog::warn("PacketHandler: Unknown stat index {}", msg.statIndex);
                return;
        }

        if (statOffset >= 0) {
            // [Oleada A] Migrado a CommandQueue: la lectura del stats-ptr y la escritura del
            // stat tocan memoria del motor → game thread. Captura SOLO POD; el personaje se
            // resuelve POR ID dentro de la lambda (registro vaciado → skip limpio).
            // EN: [Wave A] Moved to CommandQueue: reading the stats pointer and writing the stat
            //     touch engine memory → game thread. Captures ONLY POD; the character is resolved
            //     BY ID inside the lambda (emptied registry → clean skip).
            const EntityID statEntityId = msg.entityId;
            const float statValue = msg.statValue;
            core.GetCommandQueue().Push({[statEntityId, statOffset, statValue]() {
                auto& core = Core::Get();
                if (!core.IsGameLoaded()) return;
                void* gameObj = core.GetEntityRegistry().GetGameObject(statEntityId);
                if (!gameObj) return;
                game::CharacterAccessor accessor(gameObj);
                uintptr_t statsPtr = accessor.GetStatsPtr();
                if (statsPtr == 0) return;
                Memory::Write(statsPtr + statOffset, statValue);
            }});
        }
    }

    // ES: ── Sincronización de hora ── Aplica la hora del día y la velocidad del juego del servidor.
    // EN: ── Time sync ── Applies the server's time of day and game speed.
    // ── Time Sync ──
    static void HandleTimeSync(PacketReader& reader) {
        MsgTimeSync msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::debug("PacketHandler: TimeSync tick={} tod={:.2f} speed={}",
                      msg.serverTick, msg.timeOfDay, msg.gameSpeed);

        // ES: Aplicar la hora mediante el sistema de hooks de tiempo.
        // Apply time sync via the time hooks system
        time_hooks::SetServerTime(msg.timeOfDay, msg.gameSpeed);
    }

    // ES: ── Latido de entidades ── El servidor manda la lista de ids que conoce; las
    //     entidades remotas locales que no estén en ella son "fantasmas" y se borran
    //     (interpolación y registro). Ojo: a diferencia de HandleEntityDespawn, aquí no se
    //     esconde el personaje del juego ni se pasa por la CommandQueue.
    // EN: ── Entity heartbeat ── The server sends the list of ids it knows; local remote
    //     entities not in it are "ghosts" and are removed (interpolation and registry).
    //     Note: unlike HandleEntityDespawn, the game character is not hidden here and the
    //     CommandQueue is not used.
    // ── Entity Heartbeat ──
    static void HandleEntityHeartbeat(PacketReader& reader) {
        uint32_t serverTick;
        uint16_t entityCount;
        if (!reader.ReadU32(serverTick) || !reader.ReadU16(entityCount)) return;

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;
        auto& registry = core.GetEntityRegistry();

        // ES: Leer la lista de ids de entidad del servidor.
        // Read the server's entity ID list
        std::unordered_set<EntityID> serverEntities;
        serverEntities.reserve(entityCount);
        for (uint16_t i = 0; i < entityCount; i++) {
            EntityID id;
            if (!reader.ReadU32(id)) break;
            serverEntities.insert(id);
        }

        // ES: Buscar entidades fantasma: las que tenemos y el servidor no conoce.
        // Check for ghost entities: entities we have locally that the server doesn't know about
        auto localIds = registry.GetRemoteEntities();
        int cleaned = 0;
        for (EntityID localId : localIds) {
            if (serverEntities.find(localId) == serverEntities.end()) {
                spdlog::warn("PacketHandler: Heartbeat — entity {} not in server list, cleaning up", localId);
                // ES: ARREGLO BUG 2: liberar el hueco de spawn antes de borrarla.
                // BUG 2 FIX: Decrement spawn cap before unregistering
                auto ghostInfo = registry.GetInfo(localId);
                if (ghostInfo.has_value() && ghostInfo->isRemote) {
                    entity_hooks::DecrementSpawnCount(ghostInfo->ownerPlayerId);
                }
                core.GetInterpolation().RemoveEntity(localId);
                registry.Unregister(localId);
                cleaned++;
            }
        }

        if (cleaned > 0) {
            spdlog::info("PacketHandler: Heartbeat cleaned {} ghost entities", cleaned);
        }
    }

    // ES: ── Snapshot del mundo ── Formato en bloque: número de entidades (u32) y para cada
    //     una 33 bytes fijos + nombre de plantilla opcional. Las propias se remapean; las
    //     remotas se registran, se añaden a la interpolación y SIEMPRE se encola su spawn.
    // EN: ── World snapshot ── Bulk format: entity count (u32) and for each one 33 fixed
    //     bytes + optional template name. Own entities are remapped; remote ones are
    //     registered, added to interpolation and their spawn is ALWAYS queued.
    // ── World Snapshot ──
    static void HandleWorldSnapshot(PacketReader& reader) {
        spdlog::info("PacketHandler: Receiving world snapshot...");

        auto& core = Core::Get();
        auto& registry = core.GetEntityRegistry();

        // ES: Leer la lista de entidades del snapshot. El servidor también manda paquetes
        //     S2C_EntitySpawn individuales, así que el snapshot es en esencia un lote de spawns.
        //     Si viene en formato en bloque, se procesa aquí.
        // Parse entity list from the snapshot
        // The server sends individual S2C_EntitySpawn packets for each entity,
        // so the world snapshot is essentially a batch of spawns.
        // If the server sends a bulk format, we parse it here:
        uint32_t entityCount = 0;
        if (!reader.ReadU32(entityCount)) {
            // ES: Sin contador: probablemente siguen paquetes de spawn individuales.
            // If no entity count, this might be individual spawn packets following
            spdlog::info("PacketHandler: World snapshot contains individual spawn packets");
            return;
        }

        spdlog::info("PacketHandler: World snapshot with {} entities", entityCount);

        float now = SessionTime();

        auto& spawnMgr = core.GetSpawnManager();

        // ES: Campos fijos por entidad: u32+u8+u32+u32+f32*3+u32+u32 = 33 bytes (+ nombre variable).
        // Fixed fields per entity: u32+u8+u32+u32+f32*3+u32+u32 = 33 bytes (+ variable name)
        for (uint32_t i = 0; i < entityCount && reader.Remaining() >= 33; i++) {
            uint32_t entityId, templateId, factionId;
            uint8_t type;
            uint32_t ownerId;
            float px, py, pz;
            uint32_t compQuat;

            if (!reader.ReadU32(entityId)) break;
            if (!reader.ReadU8(type)) break;
            if (!reader.ReadU32(ownerId)) break;
            if (!reader.ReadU32(templateId)) break;
            if (!reader.ReadVec3(px, py, pz)) break;
            if (!reader.ReadU32(compQuat)) break;
            if (!reader.ReadU32(factionId)) break;

            // ES: Leer el nombre de plantilla opcional.
            // Read optional template name
            std::string templateName;
            uint16_t nameLen = 0;
            if (reader.Remaining() >= 2) {
                reader.ReadU16(nameLen);
                if (nameLen > 0 && nameLen <= 255 && reader.Remaining() >= nameLen) {
                    templateName.resize(nameLen);
                    reader.ReadRaw(templateName.data(), nameLen);
                }
            }

            Vec3 pos(px, py, pz);
            Quat rot = Quat::Decompress(compQuat);

            // ES: Saltar nuestras propias entidades (ya existen en el juego); remapear el id local
            //     al del servidor si hace falta.
            // Skip our own entities — they already exist in-game.
            // Remap local ID to server ID if needed.
            if (ownerId == core.GetLocalPlayerId()) {
                EntityID localId = registry.FindLocalEntityNear(pos, ownerId);
                if (localId != INVALID_ENTITY && localId != entityId) {
                    registry.RemapEntityId(localId, entityId);
                    spdlog::debug("PacketHandler: World snapshot remapped own entity {} -> {}", localId, entityId);
                }
                continue;
            }

            // ES: Guardar el punto de aparición del host a partir de la primera entidad remota.
            // Save host spawn point from first remote entity in world snapshot
            if (!core.HasHostSpawnPoint() && pos.x != 0.f && pos.z != 0.f) {
                core.SetHostSpawnPoint(pos);
                spdlog::info("PacketHandler: Host spawn point set from snapshot to ({:.1f}, {:.1f}, {:.1f})",
                             pos.x, pos.y, pos.z);
            }

            // ES: Registrar como entidad remota.
            // Register as remote entity
            registry.RegisterRemote(entityId, static_cast<EntityType>(type), ownerId, pos);
            core.GetInterpolation().AddSnapshot(entityId, now, pos, rot);

            // ES: Encolar SIEMPRE el spawn, aunque SpawnManager no esté listo. La reproducción in
            //     situ no necesita el nombre de plantilla: reproduce la estructura "pre-call" de la
            //     factoría. Saltarlo aquí dejaría entidades fantasma para siempre.
            // ALWAYS queue spawn — even if SpawnManager isn't ready yet.
            // In-place replay doesn't need the template name; it replays the factory's
            // pre-call struct. Skipping here would create permanent ghost entities.
            {
                SpawnRequest req;
                req.netId        = entityId;
                req.owner        = ownerId;
                req.type         = static_cast<EntityType>(type);
                // ES: Pasar el nombre de plantilla de UTF-8 a ANSI local para que coincida con la caché de SpawnManager.
                // Convert UTF-8 template name back to local ANSI for SpawnManager cache matching
                req.templateName = Utf8ToAnsi(templateName.c_str(), (int)templateName.size());
                req.position     = pos;
                req.rotation     = rot;
                req.templateId   = templateId;
                req.factionId    = factionId;
                spawnMgr.QueueSpawn(req);
            }
        }

        spdlog::info("PacketHandler: World snapshot processed");

        // ES: Avisar al orquestador de sincronización (el jugador local pasa a InGame).
        // Notify sync orchestrator
        if (auto* so = core.GetSyncOrchestrator()) {
            so->GetPlayerEngine().OnWorldSnapshotReceived(static_cast<int>(entityCount));
        }
    }

    // ES: ── Edificio colocado ── Solo registra el edificio como entidad remota (fantasma).
    // EN: ── Building placed ── Only registers the building as a remote (ghost) entity.
    // ── Build Placed ──
    static void HandleBuildPlaced(PacketReader& reader) {
        MsgBuildPlaced msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::info("PacketHandler: Building placed by player {} at ({:.1f}, {:.1f}, {:.1f})",
                     msg.builderId, msg.posX, msg.posY, msg.posZ);

        auto& core = Core::Get();
        auto& registry = core.GetEntityRegistry();
        auto& funcs = core.GetGameFunctions();

        // ES: Registrar el edificio en el registro de entidades.
        // Register the building in the entity registry
        Vec3 pos(msg.posX, msg.posY, msg.posZ);
        registry.RegisterRemote(msg.entityId, EntityType::Building, msg.builderId, pos);

        // ES: Nota: NO se llama a la función BuildingPlace del juego porque necesita punteros
        //     válidos al mundo y a la plantilla del edificio que no se pueden construir solo con
        //     los datos de red. El edificio queda como entidad fantasma.
        // Note: We do NOT call the game's BuildingPlace function here because it
        // requires valid world and building template pointers we cannot construct
        // from network data alone. The building is tracked as a ghost entity.
    }

    // ES: ── Actualización de salud ── Guarda la salud de las 7 partes en el registro y
    //     encola su escritura en el personaje (hilo del juego).
    // EN: ── Health update ── Stores the 7-part health in the registry and queues writing
    //     it into the character (game thread).
    // ── Health Update ──
    static void HandleHealthUpdate(PacketReader& reader) {
        MsgHealthUpdate msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;
        auto& registry = core.GetEntityRegistry();
        if (!registry.GetGameObject(msg.entityId)) return;

        // [Oleada A] Migrado a CommandQueue: la escritura de salud toca memoria del motor →
        // game thread. Captura SOLO POD (msg); el personaje se resuelve POR ID dentro de la
        // lambda. La escritura usa la cadena canónica MedicalSystem (SEH_WriteLimbHealthToChar).
        // EN: [Wave A] Moved to CommandQueue: the health write touches engine memory → game
        //     thread. Captures ONLY POD (msg); the character is resolved BY ID inside the
        //     lambda. The write uses the canonical MedicalSystem chain (SEH_WriteLimbHealthToChar).
        core.GetCommandQueue().Push({[msg]() {
            auto& core = Core::Get();
            if (!core.IsGameLoaded()) return;
            void* entityObj = core.GetEntityRegistry().GetGameObject(msg.entityId);
            if (!entityObj) return;
            if (!SEH_WriteLimbHealthToChar(entityObj, msg.health)) {
                spdlog::warn("PacketHandler: health update write failed for entity {}", msg.entityId);
            }
        }});

        // ES: Actualizar el registro con la salud (tiene su propio lock → válido en el hilo de red).
        // Update entity registry with the health values (lock interno propio → hilo de red OK)
        registry.UpdateLimbHealth(msg.entityId, msg.health);
    }

    // ES: ── Salud por miembro ── Igual que la actualización de salud, pero siempre guarda en
    //     el registro aunque no haya objeto del juego enlazado.
    // EN: ── Limb health ── Same as the health update, but always stores it in the registry
    //     even if no game object is linked.
    // ── Limb Health ──
    static void HandleLimbHealth(PacketReader& reader) {
        MsgLimbHealth msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;

        // ES: Guardar la salud por miembro en el registro (tiene su propio lock → válido en el hilo de red).
        // Store limb health in entity registry (lock interno propio → hilo de red OK)
        auto& registry = core.GetEntityRegistry();
        registry.UpdateLimbHealth(msg.entityId, msg.health);

        // [Oleada A] Migrado a CommandQueue: la escritura de salud toca memoria del motor →
        // game thread. Captura SOLO POD (msg); el personaje se resuelve POR ID dentro de la
        // lambda. La escritura usa la cadena canónica MedicalSystem (SEH_WriteLimbHealthToChar).
        // EN: [Wave A] Moved to CommandQueue: the health write touches engine memory → game
        //     thread. Captures ONLY POD (msg); the character is resolved BY ID inside the
        //     lambda. The write uses the canonical MedicalSystem chain (SEH_WriteLimbHealthToChar).
        if (registry.GetGameObject(msg.entityId)) {
            core.GetCommandQueue().Push({[msg]() {
                auto& core = Core::Get();
                if (!core.IsGameLoaded()) return;
                void* entityObj = core.GetEntityRegistry().GetGameObject(msg.entityId);
                if (!entityObj) return;
                if (!SEH_WriteLimbHealthToChar(entityObj, msg.health)) {
                    spdlog::warn("PacketHandler: limb health write failed for entity {}", msg.entityId);
                }
            }});
        }

        spdlog::debug("PacketHandler: Limb health for entity {} (chest={:.1f})",
                      msg.entityId, msg.health[static_cast<int>(BodyPart::Chest)]);
    }

    // ES: ── Efecto de estado ── Solo se guarda en el registro (no se escribe en el juego).
    // EN: ── Status effect ── Only stored in the registry (nothing is written to the game).
    // ── Status Effect ──
    static void HandleStatusEffect(PacketReader& reader) {
        MsgStatusEffect msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;

        // ES: Guardar el efecto en el registro (solo datos, sin escribir en objetos del juego).
        // Store status effect in entity registry (data-only, no game object writes)
        auto& registry = core.GetEntityRegistry();
        registry.UpdateStatusEffect(msg.entityId, msg.effectType, msg.active != 0);

        spdlog::debug("PacketHandler: Status effect for entity {} type={} active={}",
                      msg.entityId, msg.effectType, msg.active);
    }

    // ES: ── Actualización de equipo ── Encola en el hilo del juego poner el objeto indicado
    //     en la ranura de equipo del inventario del personaje.
    // EN: ── Equipment update ── Queues on the game thread putting the given item into the
    //     equipment slot of the character's inventory.
    // ── Equipment Update ──
    static void HandleEquipmentUpdate(PacketReader& reader) {
        MsgEquipmentUpdate msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::debug("PacketHandler: Equipment update entity={} slot={} item={}",
                      msg.entityId, msg.slot, msg.itemTemplateId);

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;
        if (!core.GetEntityRegistry().GetGameObject(msg.entityId)) return;
        if (msg.slot >= static_cast<uint8_t>(EquipSlot::Count)) return;

        // [Oleada A] Migrado a CommandQueue: SetEquipment escribe memoria del motor →
        // game thread. Captura SOLO POD (msg); resolución POR ID dentro de la lambda.
        // EN: [Wave A] Moved to CommandQueue: SetEquipment writes engine memory → game thread.
        //     Captures ONLY POD (msg); resolved BY ID inside the lambda.
        core.GetCommandQueue().Push({[msg]() {
            auto& core = Core::Get();
            if (!core.IsGameLoaded()) return;
            void* gameObj = core.GetEntityRegistry().GetGameObject(msg.entityId);
            if (!gameObj) return;
            game::CharacterAccessor accessor(gameObj);
            uintptr_t invPtr = accessor.GetInventoryPtr();
            if (invPtr == 0) return;
            game::InventoryAccessor inventory(invPtr);
            inventory.SetEquipment(static_cast<EquipSlot>(msg.slot), msg.itemTemplateId);
        }});
    }

    // ES: ── Chat ── Mensaje de chat de otro jugador: se muestra en el overlay y en el HUD.
    // EN: ── Chat ── Another player's chat message: shown in the overlay and the HUD.
    // ── Chat ──
    static void HandleChatMessage(PacketReader& reader) {
        uint32_t senderId;
        if (!reader.ReadU32(senderId)) return;
        std::string message;
        if (!reader.ReadString(message)) return;

        Core::Get().GetOverlay().AddChatMessage(senderId, message);

        // ES: Mostrarlo también en el HUD, con el nombre del emisor.
        // Also show on HUD overlay
        auto& pc = Core::Get().GetPlayerController();
        auto* remote = pc.GetRemotePlayer(senderId);
        std::string senderName = remote ? remote->playerName : ("Player_" + std::to_string(senderId));
        Core::Get().GetNativeHud().AddChatMessage(senderName, message);
    }

    // ES: Mensaje del sistema: un u32 que no se usa y el texto; se muestra en overlay y HUD.
    // EN: System message: an unused u32 and the text; shown in the overlay and the HUD.
    static void HandleSystemMessage(PacketReader& reader) {
        uint32_t unused;
        if (!reader.ReadU32(unused)) return;
        std::string message;
        if (!reader.ReadString(message)) return;

        Core::Get().GetOverlay().AddSystemMessage(message);
        Core::Get().GetNativeHud().AddSystemMessage(message);
    }

    // ES: ── Inventario / comercio ──
    // ── Inventory / Trade ──

    // ES: Actualización de inventario: action 0 = añadir objeto, 1 = quitar objeto; se
    //     encola en el hilo del juego.
    // EN: Inventory update: action 0 = add item, 1 = remove item; queued on the game thread.
    static void HandleInventoryUpdate(PacketReader& reader) {
        MsgInventoryUpdate msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::debug("PacketHandler: Inventory update entity={} action={} item={} qty={}",
                      msg.entityId, msg.action, msg.itemTemplateId, msg.quantity);

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;
        if (!core.GetEntityRegistry().GetGameObject(msg.entityId)) return;

        // [Oleada A] Migrado a CommandQueue: AddItem/RemoveItem escriben memoria del motor →
        // game thread. Captura SOLO POD (msg); resolución POR ID dentro de la lambda.
        // EN: [Wave A] Moved to CommandQueue: AddItem/RemoveItem write engine memory → game
        //     thread. Captures ONLY POD (msg); resolved BY ID inside the lambda.
        core.GetCommandQueue().Push({[msg]() {
            auto& core = Core::Get();
            if (!core.IsGameLoaded()) return;
            void* gameObj = core.GetEntityRegistry().GetGameObject(msg.entityId);
            if (!gameObj) return;
            game::CharacterAccessor accessor(gameObj);
            uintptr_t invPtr = accessor.GetInventoryPtr();
            if (invPtr == 0) return;
            game::InventoryAccessor inventory(invPtr);
            if (msg.action == 0) {
                // ES: Añadir objeto (escritura directa en la memoria del inventario).
                // Add item - write directly to inventory memory
                inventory.AddItem(msg.itemTemplateId, msg.quantity);
            } else if (msg.action == 1) {
                // ES: Quitar objeto.
                // Remove item
                inventory.RemoveItem(msg.itemTemplateId, msg.quantity);
            }
        }});
    }

    // ES: Resultado de un intercambio: solo se avisa en el HUD si se completó o se denegó.
    // EN: Trade result: only reported in the HUD as completed or denied.
    static void HandleTradeResult(PacketReader& reader) {
        MsgTradeResult msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::info("PacketHandler: Trade result buyer={} item={} qty={} success={}",
                     msg.buyerEntityId, msg.itemTemplateId, msg.quantity, msg.success);

        if (msg.success) {
            Core::Get().GetNativeHud().AddSystemMessage("Trade completed successfully");
        } else {
            Core::Get().GetNativeHud().AddSystemMessage("Trade denied by server");
        }
    }

    // ES: ── Escuadrones ──
    // ── Squad ──

    // ES: El servidor asignó un id de red a un escuadrón creado: se asocia al escuadrón
    //     pendiente en squad_hooks y se avisa en el HUD.
    // EN: The server assigned a net id to a created squad: it is mapped to the pending
    //     squad in squad_hooks and reported in the HUD.
    static void HandleSquadCreated(PacketReader& reader) {
        uint32_t creatorEntityId, squadNetId;
        if (!reader.ReadU32(creatorEntityId)) return;
        if (!reader.ReadU32(squadNetId)) return;
        std::string squadName;
        if (!reader.ReadString(squadName)) return;

        spdlog::info("PacketHandler: Squad '{}' created (netId={}, creator={})",
                     squadName, squadNetId, creatorEntityId);

        // ES: Asociar el puntero del escuadrón pendiente con el id de red del servidor.
        // Map the pending squad pointer to the server-assigned net ID
        squad_hooks::OnSquadNetIdAssigned(squadNetId);

        Core::Get().GetNativeHud().AddSystemMessage("Squad created: " + squadName);
    }

    // ES: Cambio de miembro de escuadrón (0 = añadido, 1 = quitado). Hoy solo se registra
    //     en el log; no cambia nada en el juego ni en el registro.
    // EN: Squad member change (0 = added, 1 = removed). Today it is only logged; nothing
    //     changes in the game or the registry.
    static void HandleSquadMemberUpdate(PacketReader& reader) {
        MsgSquadMemberUpdate msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::info("PacketHandler: Squad {} member {} action={}",
                     msg.squadNetId, msg.memberEntityId, msg.action);

        auto& core = Core::Get();
        auto& registry = core.GetEntityRegistry();

        // ES: Seguimiento: si una entidad remota entra o sale de un escuadrón, se anota.
        // Update entity tracking: if a member was added to a squad, ensure
        // our registry knows this entity belongs to the squad's owner
        auto infoCopy = registry.GetInfo(msg.memberEntityId);
        if (infoCopy && infoCopy->isRemote) {
            if (msg.action == 0) {
                // ES: Miembro añadido.
                // Member added — entity is now active in this squad
                spdlog::debug("PacketHandler: Remote entity {} added to squad {}",
                              msg.memberEntityId, msg.squadNetId);
            } else if (msg.action == 1) {
                // ES: Miembro quitado (muerte, KO, intercambio, etc.).
                // Member removed — entity left squad (death, knockout, trade, etc.)
                spdlog::debug("PacketHandler: Remote entity {} removed from squad {}",
                              msg.memberEntityId, msg.squadNetId);
            }
        }
    }

    // ES: ── Facciones ──
    // ── Faction ──

    // ES: Relación entre dos facciones: busca los objetos de facción por su id (la del
    //     jugador local y las de todos los personajes) y llama a la función original del
    //     juego para fijar la relación, marcando el origen "servidor" para que nuestro hook
    //     no la reenvíe. Ojo: esto se ejecuta en el hilo de red, sin CommandQueue, a
    //     diferencia de los demás manejadores que tocan el juego.
    // EN: Relation between two factions: finds the faction objects by id (the local
    //     player's and every character's) and calls the game's original function to set
    //     the relation, flagging it as "server sourced" so our hook does not resend it.
    //     Note: this runs on the network thread, without CommandQueue, unlike the other
    //     handlers that touch the game.
    static void HandleFactionRelation(PacketReader& reader) {
        MsgFactionRelation msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::info("PacketHandler: Faction relation {} <-> {} = {:.1f}",
                     msg.factionIdA, msg.factionIdB, msg.relation);

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;

        // ES: Llamar a la función FactionRelation del juego mediante el puntero original del
        //     hook. Antes hay que encontrar los objetos de facción por su id.
        // Try to call the game's FactionRelation function via the hook's original pointer.
        // We need to find the faction objects by their IDs first.
        auto origFn = faction_hooks::GetOriginal();
        if (!origFn) {
            spdlog::debug("PacketHandler: No FactionRelation function — cannot apply relation");
            return;
        }

        // ES: Buscar los punteros de facción recorriendo personajes. Cada personaje tiene un
        //     puntero a su facción en CharacterOffsets::faction y cada facción su id en
        //     FactionOffsets::id.
        // Find faction pointers by scanning remote player entities and the local player.
        // Each character has a faction pointer at CharacterOffsets::faction.
        // Factions have an ID at FactionOffsets::id. We scan to find matching factions.
        const int fIdOff = game::GetOffsets().faction.id;
        if (fIdOff < 0) {
            spdlog::warn("PacketHandler: faction.id offset not resolved (-1), cannot apply relation");
            return;
        }

        uintptr_t factionPtrA = 0, factionPtrB = 0;

        // ES: Primero la facción del jugador local.
        // Check local player's faction first
        uintptr_t localFaction = core.GetPlayerController().GetLocalFactionPtr();
        if (localFaction != 0) {
            uint32_t localFactionId = 0;
            Memory::Read(localFaction + fIdOff, localFactionId);
            if (localFactionId == msg.factionIdA) factionPtrA = localFaction;
            if (localFactionId == msg.factionIdB) factionPtrB = localFaction;
        }

        // ES: Recorrer todos los personajes buscando las facciones.
        // Scan all entities for faction pointers
        if (factionPtrA == 0 || factionPtrB == 0) {
            game::CharacterIterator iter;
            while (iter.HasNext() && (factionPtrA == 0 || factionPtrB == 0)) {
                game::CharacterAccessor character = iter.Next();
                if (!character.IsValid()) continue;

                uintptr_t fPtr = character.GetFactionPtr();
                if (fPtr == 0) continue;

                uint32_t fId = 0;
                Memory::Read(fPtr + fIdOff, fId);
                if (fId == msg.factionIdA && factionPtrA == 0) factionPtrA = fPtr;
                if (fId == msg.factionIdB && factionPtrB == 0) factionPtrB = fPtr;
            }
        }

        if (factionPtrA == 0 || factionPtrB == 0) {
            spdlog::debug("PacketHandler: Could not find faction pointers for IDs {} and {}",
                         msg.factionIdA, msg.factionIdB);
            return;
        }

        // ES: Llamar a la función del juego con la marca "venido del servidor" para evitar un bucle.
        // Call the game function with server-sourced guard to prevent feedback loop
        faction_hooks::SetServerSourced(true);
        __try {
            origFn(reinterpret_cast<void*>(factionPtrA),
                   reinterpret_cast<void*>(factionPtrB),
                   msg.relation);
            spdlog::info("PacketHandler: Applied faction relation {} <-> {} = {:.1f}",
                        msg.factionIdA, msg.factionIdB, msg.relation);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            spdlog::error("PacketHandler: FactionRelation call crashed");
        }
        faction_hooks::SetServerSourced(false);
    }

    // ES: ── Edificios ──
    // ── Building ──

    // ES: Edificio destruido/desmontado (reason 2 = desmontado): se quita del registro y se avisa.
    // EN: Building destroyed/dismantled (reason 2 = dismantled): removed from the registry and reported.
    static void HandleBuildDestroyed(PacketReader& reader) {
        uint32_t buildingId;
        uint8_t reason = 0;
        if (!reader.ReadU32(buildingId)) return;
        reader.ReadU8(reason); // optional

        spdlog::info("PacketHandler: Building {} destroyed (reason={})", buildingId, reason);

        auto& core = Core::Get();
        core.GetEntityRegistry().Unregister(buildingId);

        const char* reasonStr = reason == 2 ? "dismantled" : "destroyed";
        core.GetNativeHud().AddSystemMessage(std::string("Building ") + reasonStr);
    }

    // ES: Progreso de construcción de un edificio remoto: solo se avisa al 50 % (una vez) y
    //     al 100 %; no se refleja visualmente en el juego.
    // EN: Construction progress of a remote building: only reported at 50% (once) and
    //     100%; not reflected visually in the game.
    static void HandleBuildProgressUpdate(PacketReader& reader) {
        MsgBuildProgress msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        spdlog::debug("PacketHandler: Building {} progress={:.2f}", msg.entityId, msg.progress);

        // ES: Anotar el progreso para mantener el estado local al día.
        // Update the entity registry with the build progress
        // This keeps our local state in sync so /entities can report accurately
        auto& core = Core::Get();
        auto infoCopy = core.GetEntityRegistry().GetInfo(msg.entityId);
        if (infoCopy && infoCopy->isRemote) {
            // ES: El progreso se sigue, pero el cambio visual requeriría escribir en la GameData del
            //     edificio (offset por descubrir con RE). De momento solo se registran los hitos.
            // Building progress is tracked — visual update would require
            // writing to the building's GameData (offset TBD from RE).
            // For now, log at info level when milestones are hit.
            if (msg.progress >= 1.0f) {
                spdlog::info("PacketHandler: Remote building {} construction complete!", msg.entityId);
                core.GetNativeHud().AddSystemMessage("Remote building completed.");
            } else if (msg.progress >= 0.5f) {
                static std::unordered_set<EntityID> notified50;
                if (notified50.insert(msg.entityId).second) {
                    spdlog::info("PacketHandler: Remote building {} 50% complete", msg.entityId);
                }
            }
        }
    }

    // ES: ── Estado de puertas ──
    // ── Door State ──

    // ES: Estado de puerta/portón (0 abierta, 1 cerrada, 2 con llave, 3 rota): se encola en
    //     el hilo del juego escribirlo en functionality+0x10 del edificio.
    // EN: Door/gate state (0 opened, 1 closed, 2 locked, 3 broken): queued on the game
    //     thread to be written at the building's functionality+0x10.
    static void HandleDoorState(PacketReader& reader) {
        MsgDoorState msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        const char* stateNames[] = {"opened", "closed", "locked", "broken"};
        const char* stateName = (msg.state < 4) ? stateNames[msg.state] : "unknown";
        spdlog::info("PacketHandler: Door/gate {} state -> {}", msg.entityId, stateName);

        auto& core = Core::Get();
        if (!core.IsGameLoaded()) return;
        if (!core.GetEntityRegistry().GetGameObject(msg.entityId)) return;

        // [Oleada A] Migrado a CommandQueue: la lectura del functionality-ptr y la escritura
        // del estado de puerta tocan memoria del motor → game thread. Captura SOLO POD (msg);
        // el edificio se resuelve POR ID dentro de la lambda.
        // EN: [Wave A] Moved to CommandQueue: reading the functionality pointer and writing the
        //     door state touch engine memory → game thread. Captures ONLY POD (msg); the
        //     building is resolved BY ID inside the lambda.
        core.GetCommandQueue().Push({[msg]() {
            auto& core = Core::Get();
            if (!core.IsGameLoaded()) return;
            void* gameObj = core.GetEntityRegistry().GetGameObject(msg.entityId);
            if (!gameObj) return;
            // ES: Los edificios de Kenshi tienen un puntero "functionality" que contiene el estado de
            //     la puerta: BuildingOffsets::functionality = 0xC0 → estado en functionality+0x10
            //     (offset fijo en el código, probablemente sin verificar del todo).
            // Buildings in Kenshi have a functionality pointer that contains door state.
            // BuildingOffsets::functionality = 0xC0 → door state is at functionality+0x10
            auto& offsets = game::GetOffsets().building;
            uintptr_t bldPtr = reinterpret_cast<uintptr_t>(gameObj);
            uintptr_t funcPtr = 0;
            if (offsets.functionality >= 0) {
                Memory::Read(bldPtr + offsets.functionality, funcPtr);
            }
            if (funcPtr != 0 && funcPtr > 0x10000) {
                // ES: Escribir el estado de la puerta en functionality + 0x10.
                // Write door state at functionality + 0x10 (open/closed/locked flag)
                Memory::Write(funcPtr + 0x10, msg.state);
                spdlog::debug("PacketHandler: Wrote door state {} to building 0x{:X}", msg.state, bldPtr);
            } else {
                spdlog::debug("PacketHandler: No functionality ptr for building {} — door state not written", msg.entityId);
            }
        }});
    }

    // ES: ── Respuesta de administración ──
    // ── Admin Response ──

    // ES: Respuesta a un comando de admin: muestra el texto como éxito o error en el HUD.
    // EN: Admin command response: shows the text as success or error in the HUD.
    static void HandleAdminResponse(PacketReader& reader) {
        MsgAdminResponse msg;
        if (!reader.ReadRaw(&msg, sizeof(msg))) return;

        // ES: Asegurar el '\0' final: el servidor puede llenar los 128 bytes sin terminador.
        // Ensure null-termination — server may fill the entire 128-byte buffer
        // without a trailing '\0', causing string ops to read past the struct.
        msg.responseText[sizeof(msg.responseText) - 1] = '\0';

        auto& core = Core::Get();
        std::string text = msg.responseText;
        if (msg.success) {
            spdlog::info("PacketHandler: Admin response: {}", text);
            core.GetNativeHud().AddSystemMessage("[Admin] " + text);
        } else {
            spdlog::warn("PacketHandler: Admin denied: {}", text);
            core.GetNativeHud().AddSystemMessage("[Admin Error] " + text);
        }
    }

    // ES: ── Lobby: inicio ── Todos listos en el lobby: avisa al jugador de su hueco y de
    //     que pulse NEW GAME.
    // EN: ── Lobby: start ── Everyone ready in the lobby: tells the player their slot and
    //     to press NEW GAME.
    // ── Lobby: Start ──
    static void HandleLobbyStart(PacketReader& reader) {
        uint8_t playerCount = 0;
        reader.ReadU8(playerCount);

        auto& core = Core::Get();
        int slot = core.GetLobbyManager().GetPlayerSlot();

        spdlog::info("PacketHandler: LobbyStart! {} players, my slot = {}", playerCount, slot);
        core.GetNativeHud().AddSystemMessage("All players ready! Click NEW GAME to start.");
        core.GetNativeHud().AddSystemMessage("You are Player " + std::to_string(slot));
    }

    // ES: ── Lobby: asignación de facción ── Recibe el nombre de facción (máx. 32) y el hueco,
    //     se lo pasa a LobbyManager y, si aún estamos en el menú o en GameReady, parchea en
    //     memoria la cadena de facción antes de cargar la partida.
    // EN: ── Lobby: faction assignment ── Receives the faction name (max 32) and slot,
    //     passes them to LobbyManager and, if still in the menu or GameReady, patches the
    //     faction string in memory before loading the save.
    // ── Lobby: Faction Assignment ──
    static void HandleFactionAssignment(PacketReader& reader) {
        uint16_t strLen = 0;
        if (!reader.ReadU16(strLen) || strLen == 0 || strLen > 32) return;

        char factionBuf[36] = {};
        if (!reader.ReadRaw(factionBuf, strLen)) return;
        factionBuf[strLen] = '\0';
        std::string factionStr(factionBuf, strLen);

        int32_t slot = 0;
        reader.ReadI32(slot);

        auto& core = Core::Get();
        core.GetLobbyManager().OnFactionAssigned(factionStr, slot);

        spdlog::info("PacketHandler: Faction assigned: '{}' slot {}", factionStr, slot);
        core.GetNativeHud().AddSystemMessage("Faction assigned: " + factionStr + " (slot " + std::to_string(slot) + ")");

        // ES: Aplicar el parche de facción ya si estamos en el menú principal (antes de cargar).
        // Apply faction patch immediately if we're on main menu (before save load)
        if (core.GetClientPhase() == ClientPhase::MainMenu ||
            core.GetClientPhase() == ClientPhase::GameReady) {
            if (core.GetLobbyManager().ApplyFactionPatch()) {
                core.GetNativeHud().AddSystemMessage("Faction string patched in memory");
            }
        }
    }

    // ES: Comentario sobrante: HandleAllPlayersReady está definido más arriba en la clase.
    // EN: Leftover comment: HandleAllPlayersReady is defined earlier in the class.
    // ── All Players Ready (Server → Client) ──
    // Server sends this when ALL connected players have loaded their games
};

// ES: Se llama desde core.cpp para iniciar el manejo de paquetes.
// Called from core.cpp to initialize packet handling
void InitPacketHandler() {
    PacketHandler::Initialize();
}

// ES: Procesa un spawn diferido (lo llama DeferredSpawnQueue::ProcessAll desde
//     HandleAllPlayersReady). Repite la lógica de HandleEntitySpawn sin la comprobación
//     de "juego listo". Diferencias con HandleEntitySpawn: no crea VisualProxy y, en la
//     rama de reserva, no convierte el nombre de plantilla a ANSI ni pasa la salud extendida.
// EN: Differences from HandleEntitySpawn: no VisualProxy is created and, in the
//     fallback branch, the template name is not converted to ANSI and extended health
//     is not passed.
// Process a deferred spawn packet (called from DeferredSpawnQueue::ProcessAll)
void ProcessDeferredSpawn(const DeferredSpawn& ds) {
    // ES: Reconstruye la lógica de spawn de HandleEntitySpawn, pero sin la puerta de "juego
    //     listo" porque ya se procesa después de AllPlayersReady.
    // Reconstruct the spawn processing logic from HandleEntitySpawn
    // but skip the "game ready" gate since we're now processing after AllPlayersReady

    auto& core = Core::Get();
    auto& registry = core.GetEntityRegistry();

    Vec3 spawnPos(ds.posX, ds.posY, ds.posZ);
    Quat rot = Quat::Decompress(ds.compressedQuat);

    spdlog::info("ProcessDeferredSpawn: Entity id={} type={} owner={} template='{}' at ({:.1f}, {:.1f}, {:.1f})",
                 ds.entityId, ds.type, ds.ownerId, ds.templateName, ds.posX, ds.posY, ds.posZ);

    // ES: Saltar las entidades propias (la cola diferida solo procesa remotas; las propias
    //     ya se remapearon en HandleEntitySpawn).
    // Skip own entity remapping (only process remote entities in deferred queue)
    if (ds.ownerId == core.GetLocalPlayerId()) {
        spdlog::debug("ProcessDeferredSpawn: Skipping own entity {}", ds.entityId);
        return;
    }

    // ES: Registrar en el registro de entidades.
    // Register in entity registry
    registry.RegisterRemote(ds.entityId, static_cast<EntityType>(ds.type), ds.ownerId, spawnPos);

    // ES: Volcar las posiciones encoladas.
    // Flush any queued position updates
    PendingSnapshotQueue::FlushForEntity(ds.entityId);

    // ES: Snapshot inicial de interpolación.
    // Add initial interpolation snapshot
    float now = SessionTime();
    core.GetInterpolation().AddSnapshot(ds.entityId, now, spawnPos, rot);

    // ES: Intentar enlazar el personaje del mod (solo si no está enlazado a otra entidad).
    // Try mod character linking
    void* existingChar = core.FindModCharacterBySlot(static_cast<int>(ds.ownerId));
    EntityID existingLinkedId = existingChar ? registry.GetNetId(existingChar) : INVALID_ENTITY;

    if (existingChar && existingLinkedId == INVALID_ENTITY) {
        registry.SetGameObject(ds.entityId, existingChar);
        registry.UpdatePosition(ds.entityId, spawnPos);
        ai_hooks::MarkRemoteControlled(existingChar);

        // ── FIX CRASH 2º JUGADOR: mismo bug que HandleEntitySpawn ──
        // Las llamadas SEH (OnRemoteCharSpawned, WriteGameDataName, AnimProbe,
        // WriteLimbHealth) tocaban memoria del motor DIRECTAMENTE desde el hilo
        // de red. Ahora todo va en UN solo Push drenado por el game thread, con
        // captura por VALOR y revalidación dentro del lambda.
        // EN: ── 2nd PLAYER CRASH FIX: same bug as HandleEntitySpawn ──
        //     The SEH calls (OnRemoteCharSpawned, WriteGameDataName, AnimProbe, WriteLimbHealth)
        //     touched engine memory DIRECTLY from the network thread. Now everything goes into
        //     ONE Push drained by the game thread, captured BY VALUE and revalidated in the lambda.

        // Copiar salud a std::array (capturable por valor; ds.health es C-array).
        // EN: Copy health into a std::array (capturable by value; ds.health is a C array).
        std::array<float, 7> healthCopy{};
        for (int i = 0; i < 7; i++) healthCopy[i] = ds.health[i];

        const EntityID dsEntityId  = ds.entityId;
        const PlayerID dsOwnerId   = ds.ownerId;
        const bool     dsHasHealth = ds.hasExtendedHealth;

        // Actualizar registry (lock interno propio -> seguro en hilo de red).
        // EN: Update the registry (has its own lock -> safe on the network thread).
        if (dsHasHealth) {
            registry.UpdateLimbHealth(dsEntityId, ds.health);
        }

        // Posición + inicialización completa en UN solo comando de game thread.
        // Resolución POR ID dentro de la lambda — no capturar el puntero crudo.
        // EN: Position + full initialization in ONE game-thread command.
        //     Resolve BY ID inside the lambda; do not capture the raw pointer.
        core.GetCommandQueue().Push({[spawnPos, dsEntityId, dsOwnerId,
                                      healthCopy, dsHasHealth]() {
            auto& core = Core::Get();
            auto& registry = core.GetEntityRegistry();

            // REVALIDAR en el game thread (ver HandleEntitySpawn). GetGameObject(id)
            // devuelve null si el registry fue vaciado (desconexión) → skip limpio.
            // EN: REVALIDATE on the game thread (see HandleEntitySpawn). GetGameObject(id) returns
            //     null if the registry was emptied (disconnect) → clean skip.
            void* existingChar = registry.GetGameObject(dsEntityId);
            if (!core.IsGameLoaded() ||
                core.GetClientPhase() < ClientPhase::GameReady ||
                existingChar == nullptr) {
                spdlog::warn("ProcessDeferredSpawn: cmd descartado (revalidación falló) entity {}", dsEntityId);
                return;
            }

            if (spawnPos.x != 0.f || spawnPos.y != 0.f || spawnPos.z != 0.f) {
                if (!SEH_WritePositionToChar(existingChar, spawnPos.x, spawnPos.y, spawnPos.z)) {
                    spdlog::warn("ProcessDeferredSpawn: WritePosition failed for entity {}", dsEntityId);
                }
            }

            SEH_OnRemoteCharSpawned(dsEntityId, existingChar, dsOwnerId);
            core.GetPlayerController().WriteGameDataNameForModLink(existingChar, dsOwnerId);
            SEH_ScheduleAnimProbe(existingChar);

            if (dsHasHealth) {
                SEH_WriteLimbHealthToChar(existingChar, healthCopy.data());
            }

            // SEH_AllyModFaction DESACTIVADO (faction-UAF). No se llama aquí.
        // EN: SEH_AllyModFaction DISABLED (faction UAF). Not called here.
        }});

        spdlog::info("ProcessDeferredSpawn: Linked mod character for entity {}", dsEntityId);
    } else {
        // ES: Reserva: spawn por la factoría (SpawnManager).
        // Fallback to factory spawn
        SpawnRequest req;
        req.netId = ds.entityId;
        req.owner = ds.ownerId;
        req.type = static_cast<EntityType>(ds.type);
        req.templateName = ds.templateName;
        req.position = spawnPos;
        req.rotation = rot;
        req.templateId = ds.templateId;
        req.factionId = ds.factionId;
        core.GetSpawnManager().QueueSpawn(req);
        spdlog::info("ProcessDeferredSpawn: Queued factory spawn for entity {}", ds.entityId);
    }
}

} // namespace kmp
