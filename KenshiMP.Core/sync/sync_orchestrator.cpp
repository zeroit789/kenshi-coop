// ES: Implementación de SyncOrchestrator (ver sync_orchestrator.h): envoltorios SEH
//     para leer/escribir memoria de personajes del juego, ciclo de vida, el tick con
//     sus 7 etapas, los trabajos en segundo plano y la prioridad de sincronización.
// EN: SyncOrchestrator implementation (see sync_orchestrator.h): SEH wrappers to
//     read/write game character memory, lifecycle, the tick with its 7 stages, the
//     background workers and sync priority.
#include "sync_orchestrator.h"
#include "../core.h"
#include "../game/game_types.h"
#include "../game/asset_facilitator.h"
#include "../hooks/entity_hooks.h"
#include "../hooks/ai_hooks.h"
#include "../hooks/squad_hooks.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <Windows.h>

namespace kmp {

// ES: Envoltorios protegidos con SEH (deben ser funciones libres: __try no admite
//     objetos C++ con destructor en la misma función). Capturan violaciones de acceso
//     al tocar punteros del juego que pueden estar liberados.
// ════════════════════════════════════════════════════════════════════════════
// SEH-protected wrappers (must be free functions — __try forbids C++ unwind)
// ════════════════════════════════════════════════════════════════════════════

// ES: Escribe posición y rotación en un personaje del juego. La rotación solo se
//     escribe si el offset es conocido, el cuaternión es válido y el valor actual en
//     ese offset no parece un puntero. Devuelve false si hubo una excepción.
// EN: Writes position and rotation into a game character. Rotation is only written
//     if the offset is known, the quaternion is valid and the current value at that
//     offset does not look like a pointer. Returns false on an exception.
static bool SEH_WritePositionRotation(void* gameObj, Vec3 pos, Quat rot) {
    __try {
        game::CharacterAccessor accessor(gameObj);
        accessor.WritePosition(pos);

        auto& offsets = game::GetOffsets().character;
        if (offsets.rotation >= 0) {
            // ES: Validar el cuaternión antes de escribir: evita un crash de Ogre en el que un
            //     w=1.0 (0x3F800000) se leía como puntero. Comprueba NaN/Inf y que la magnitud
            //     al cuadrado esté entre 0.5 y 1.5.
            // Validate quaternion before writing — prevents Ogre crash where
            // a quaternion w=1.0 (0x3F800000) was read as a pointer.
            // NaN/Inf check + magnitude sanity check.
            bool quatValid = true;
            float magSq = rot.w * rot.w + rot.x * rot.x + rot.y * rot.y + rot.z * rot.z;
            if (std::isnan(magSq) || std::isinf(magSq) || magSq < 0.5f || magSq > 1.5f) {
                quatValid = false;
            }
            if (std::isnan(rot.w) || std::isnan(rot.x) || std::isnan(rot.y) || std::isnan(rot.z) ||
                std::isinf(rot.w) || std::isinf(rot.x) || std::isinf(rot.y) || std::isinf(rot.z)) {
                quatValid = false;
            }

            if (quatValid) {
                uintptr_t charPtr = reinterpret_cast<uintptr_t>(gameObj);

                // ES: Seguridad: leer primero el valor actual; si en el offset de rotación hay algo
                //     que parece un puntero (>0x10000, alineado), NO sobrescribirlo: podría ser un
                //     SceneNode* u otro puntero de Ogre y no un cuaternión.
                // Safety: read current value first — if the existing value at the
                // rotation offset looks like a pointer (>0x10000), DON'T overwrite
                // it. It might be a SceneNode* or other Ogre pointer, not a quaternion.
                uintptr_t existingVal = 0;
                Memory::Read(charPtr + offsets.rotation, existingVal);
                bool looksLikePointer = (existingVal > 0x10000 && existingVal < 0x00007FFFFFFFFFFF
                                          && (existingVal & 0x3) == 0);
                if (!looksLikePointer) {
                    Memory::Write(charPtr + offsets.rotation, rot);
                } else {
                    static int s_skipCount = 0;
                    if (++s_skipCount <= 5) {
                        spdlog::warn("SyncOrch SEH_WritePositionRotation: SKIPPED rotation write — "
                                     "char+0x{:X} = 0x{:X} looks like pointer, not quaternion",
                                     offsets.rotation, existingVal);
                    }
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_count = 0;
        if (++s_count <= 10) {
            char buf[128];
            sprintf_s(buf, "KMP: SyncOrch SEH_WritePositionRotation CRASHED for 0x%p\n", gameObj);
            OutputDebugStringA(buf);
        }
        return false;
    }
}

// ES: Lee posición y rotación de un personaje (false si hubo excepción).
// EN: Reads a character's position and rotation (false on exception).
static bool SEH_ReadPosition(void* gameObj, Vec3& outPos, Quat& outRot) {
    __try {
        game::CharacterAccessor accessor(gameObj);
        outPos = accessor.GetPosition();
        outRot = accessor.GetRotation();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Arregla el puntero de facción de un personaje creado con SpawnCharacterDirect.
//     Ese spawn usa una estructura "pre-call" antigua cuya referencia de facción
//     (offset character.faction, char+0x10) puede apuntar a un objeto ya liberado →
//     crash por uso tras liberar. Se escribe la facción del jugador local (siempre
//     válida) o 0. También coloca al personaje en spawnPos si no es (0,0,0).
// Fix the faction pointer on a directly-spawned character.
// SpawnCharacterDirect creates from a stale pre-call struct whose faction
// reference (char+0x10) may point to a freed object → use-after-free crash.
// Writing the local player's faction (always valid) prevents this.
static bool SEH_FixFactionAndSetup(void* character, uintptr_t localFactionPtr,
                                    Vec3 spawnPos) {
    __try {
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);
        if (charPtr < 0x10000 || charPtr >= 0x00007FFFFFFFFFFF) return false;

        // ES: Comprobación de la vtable: debe estar dentro de los primeros 64 MB del módulo
        //     kenshi_x64.exe; si no, el puntero no es un objeto válido del juego.
        // Vtable sanity check
        uintptr_t vtable = 0;
        Memory::Read(charPtr, vtable);
        uintptr_t modBase = Memory::GetModuleBase();
        if (vtable < modBase || vtable >= modBase + 0x4000000) return false;

        // ES: ARREGLO DEL CRASH: sobrescribir el puntero de facción antiguo con la facción del
        //     jugador local (persistente: el jugador local siempre está cargado). Si aún no la
        //     tenemos, se escribe 0 (sin facción, pero seguro).
        // FIX THE CRASH: Overwrite stale faction pointer with the local player's
        // faction (persistent — local player is always loaded). If we don't have
        // the local faction yet, write 0 (factionless but safe).
        int factionOffset = game::GetOffsets().character.faction;
        Memory::Write(charPtr + factionOffset, localFactionPtr);

        // ES: Colocar la posición.
        // Set position
        game::CharacterAccessor accessor(character);
        if (spawnPos.x != 0.f || spawnPos.y != 0.f || spawnPos.z != 0.f) {
            accessor.WritePosition(spawnPos);
        }

        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: SEH_FixFactionAndSetup CRASHED\n");
        return false;
    }
}

// ES: Escribe velocidad de movimiento y estado de animación en el objeto de un
//     personaje remoto. Necesita que el sistema de sondeo haya descubierto esos offsets;
//     si valen -1 (desconocidos) no hace nada.
// Write moveSpeed and animState to a remote character's game object.
// Requires the corresponding offsets to be discovered by the probe system.
// Safe no-op when offsets are -1 (unknown).
static bool SEH_WriteMoveData(void* gameObj, uint8_t moveSpeedU8, uint8_t animState) {
    __try {
        auto& offsets = game::GetOffsets().character;
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(gameObj);

        // ES: Velocidad: decodificar uint8 (0-255) a float (0.0-15.0 m/s).
        // Write move speed: decode uint8 (0-255) back to float (0.0-15.0 m/s)
        if (offsets.moveSpeed >= 0) {
            float speed = (moveSpeedU8 / 255.f) * 15.f;
            Memory::Write(charPtr + offsets.moveSpeed, speed);
        }

        // ES: Estado de animación.
        // Write animation state
        if (offsets.animState >= 0) {
            Memory::Write(charPtr + offsets.animState, animState);
        }

        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_count = 0;
        if (++s_count <= 5) {
            char buf[128];
            sprintf_s(buf, "KMP: SyncOrch SEH_WriteMoveData CRASHED for 0x%p\n", gameObj);
            OutputDebugStringA(buf);
        }
        return false;
    }
}

// ES: Resultado de leer un personaje desde el trabajo en segundo plano.
// EN: Result of reading a character from the background worker.
struct BGReadResult {
    Vec3 pos;
    Quat rot;
    float speed;
    uint8_t animState;
    bool valid;
};

// ES: Lee posición, rotación, velocidad y animación de un personaje con protección SEH.
// EN: Reads a character's position, rotation, speed and animation with SEH protection.
static BGReadResult SEH_ReadCharacterBG(void* gameObj) {
    BGReadResult r = {};
    __try {
        game::CharacterAccessor character(gameObj);
        if (!character.IsValid()) return r;
        r.pos = character.GetPosition();
        r.rot = character.GetRotation();
        r.speed = character.GetMoveSpeed();
        r.animState = character.GetAnimState();
        r.valid = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_count = 0;
        if (++s_count <= 10) {
            char buf[128];
            sprintf_s(buf, "KMP: SyncOrch SEH_ReadCharacterBG CRASHED for 0x%p\n", gameObj);
            OutputDebugStringA(buf);
        }
        r.valid = false;
    }
    return r;
}

// ES: Arregla el puntero de facción de un personaje recién creado copiando la facción
//     VIVA del personaje principal del host (offset 0x10 escrito a mano). Debe
//     llamarse ANTES del siguiente tick de actualización de personajes del motor para
//     evitar el crash por uso tras liberar en game+0x927E94 (lee faction+0x250 de cada
//     personaje). Nota: esta copia estática no se llama desde este fichero (código
//     muerto aquí); core.cpp tiene su propia versión SEH_FixUpFaction_Core.
// EN: Note: this static copy is not called from this file (dead code here); core.cpp
//     has its own SEH_FixUpFaction_Core.
// Fix stale faction pointer on a spawned character by copying the LIVE
// faction from the host's primary character.  Must be called BEFORE the
// game engine's next character-update tick to prevent a use-after-free
// crash at game+0x927E94 (reads faction+0x250 on every character).
static bool SEH_FixUpFaction(void* spawnedChar) {
    __try {
        void* primaryChar = Core::Get().GetPlayerController().GetPrimaryCharacter();
        if (!primaryChar) return false;

        uintptr_t primaryPtr = reinterpret_cast<uintptr_t>(primaryChar);
        uintptr_t spawnedPtr = reinterpret_cast<uintptr_t>(spawnedChar);

        // ES: Leer la facción VIVA del personaje principal del host (siempre en zona cargada).
        // Read LIVE faction from the host's primary character (always in a loaded zone)
        uintptr_t faction = 0;
        Memory::Read(primaryPtr + 0x10, faction);
        if (faction == 0 || faction < 0x10000 || faction > 0x00007FFFFFFFFFFF)
            return false;

        // ES: Validar que el objeto de facción se puede leer (no está liberado).
        // Validate: the faction object should be readable (not freed)
        uintptr_t vtable = 0;
        Memory::Read(faction, vtable);
        if (vtable == 0) return false;

        // ES: Escribir en el personaje creado.
        // Write to spawned character
        Memory::Write(spawnedPtr + 0x10, faction);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Configuración tras el spawn de reserva: posición, aviso a PlayerController, marcar
//     como controlado remotamente (IA desactivada), añadir al escuadrón local, marcar
//     "controlado por jugador" y programar la sonda de clase de animación. Cada paso va
//     en su propio __try para que un fallo no impida los demás. Nota: esta copia
//     estática no se llama desde este fichero (código muerto aquí); core.cpp tiene la suya.
// EN: Post-spawn setup for the fallback path: position, PlayerController notification,
//     mark as remote-controlled (AI disabled), add to local squad, mark as
//     "player controlled" and schedule the anim-class probe. Each step has its own
//     __try so one failure does not block the others. Note: this static copy is not
//     called from this file (dead code here); core.cpp has its own.
static bool SEH_FallbackPostSpawnSetup(void* character, EntityID netId,
                                        PlayerID owner, Vec3 pos) {
    bool allOk = true;

    __try {
        game::CharacterAccessor accessor(character);
        if (pos.x != 0.f || pos.y != 0.f || pos.z != 0.f) {
            accessor.WritePosition(pos);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: SyncOrch FallbackPostSpawn — WritePosition AV\n");
        allOk = false;
    }

    __try {
        Core::Get().GetPlayerController().OnRemoteCharacterSpawned(
            netId, character, owner);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: SyncOrch FallbackPostSpawn — OnRemoteCharacterSpawned AV\n");
        allOk = false;
    }

    __try {
        ai_hooks::MarkRemoteControlled(character);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        allOk = false;
    }

    __try {
        squad_hooks::AddCharacterToLocalSquad(character);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        allOk = false;
    }

    __try {
        game::WritePlayerControlled(
            reinterpret_cast<uintptr_t>(character), true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        allOk = false;
    }

    __try {
        game::ScheduleDeferredAnimClassProbe(
            reinterpret_cast<uintptr_t>(character));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        allOk = false;
    }

    if (!allOk) {
        static int s_count = 0;
        if (++s_count <= 10) {
            char buf[256];
            sprintf_s(buf, "KMP: SyncOrch SEH_FallbackPostSpawnSetup partial failure entity %u char=0x%p\n",
                      netId, character);
            OutputDebugStringA(buf);
        }
    }
    return allOk;
}

// ES: Construcción / ciclo de vida
// ════════════════════════════════════════════════════════════════════════════
// Construction / Lifecycle
// ════════════════════════════════════════════════════════════════════════════

// ES: Guarda las referencias a los subsistemas y construye los motores propios.
// EN: Stores subsystem references and builds the owned engines.
SyncOrchestrator::SyncOrchestrator(EntityRegistry& registry,
                                   PlayerController& playerCtrl,
                                   Interpolation& interp,
                                   SpawnManager& spawnMgr,
                                   NetworkClient& client,
                                   TaskOrchestrator& taskOrch)
    : m_registry(registry)
    , m_playerController(playerCtrl)
    , m_interpolation(interp)
    , m_spawnManager(spawnMgr)
    , m_client(client)
    , m_taskOrchestrator(taskOrch)
    , m_resolver(registry)
    , m_zoneEngine(registry)
    , m_playerEngine(playerCtrl)
{
    spdlog::info("SyncOrchestrator: Constructed");
}

// ES: Activa el orquestador para el jugador local, conecta el envío de ZoneEngine al
//     cliente de red y arranca los relojes.
// EN: Activates the orchestrator for the local player, wires ZoneEngine sending to the
//     network client and starts the clocks.
void SyncOrchestrator::Initialize(PlayerID localId, const std::string& playerName) {
    m_localPlayerId = localId;
    m_active = true;
    m_tickCount = 0;

    m_playerEngine.OnHandshakeAck(localId, playerName);

    // ES: Conectar el callback de envío de ZoneEngine con el cliente de red (el parámetro
    //     'channel' se ignora: fiable → SendReliable, no fiable → SendUnreliable).
    // Wire ZoneEngine's send callback to the network client
    m_zoneEngine.SetSendCallback([this](const uint8_t* data, size_t size, int channel, bool reliable) {
        if (reliable) {
            m_client.SendReliable(data, size);
        } else {
            m_client.SendUnreliable(data, size);
        }
    });

    auto now = std::chrono::steady_clock::now();
    m_lastZoneRebuild = now;
    m_lastPollTime = now;
    m_lastEquipmentPollTime = now;
    m_lastSpawnLog = now;
    m_lastDiagLog = now;

    spdlog::info("SyncOrchestrator: Initialized for player {} '{}'", localId, playerName);
}

// ES: Desactiva el orquestador (no limpia estado; eso lo hace Reset).
// EN: Deactivates the orchestrator (does not clear state; Reset does that).
void SyncOrchestrator::Shutdown() {
    m_active = false;
    spdlog::info("SyncOrchestrator: Shutdown");
}

// ES: Reinicio al desconectar: espera a los trabajos de fondo, vacía el doble buffer y
//     reinicia motores y estado de spawns.
// EN: Reset on disconnect: waits for background work, clears the double buffer and
//     resets engines and spawn state.
void SyncOrchestrator::Reset() {
    m_active = false;

    // CRÍTICO: esperar a que terminen los workers de fondo (2 hilos del TaskOrchestrator)
    // ANTES de tocar m_frameData/m_writeBuffer/m_readBuffer. Si no, el Clear() de más abajo
    // corre concurrente con los push_back() de los workers (BackgroundReadEntities /
    // BackgroundInterpolate) → corrupción de heap / iteradores colgando → crash aleatorio al
    // desconectar o reconectar. El wait normal (StageSwapBuffers del siguiente tick) no llega a
    // ejecutarse tras Reset() porque m_pipelineStarted pasa a false, dejando el trabajo de los
    // workers huérfano. Comprobamos m_pipelineStarted ANTES de ponerlo a false.
    // EN: CRITICAL: wait for the background workers (2 TaskOrchestrator threads) BEFORE
    //     touching m_frameData/m_writeBuffer/m_readBuffer. Otherwise Clear() below runs
    //     concurrently with the workers' push_back() (BackgroundReadEntities /
    //     BackgroundInterpolate) → heap corruption / dangling iterators → random crash on
    //     disconnect or reconnect. The normal wait (next tick's StageSwapBuffers) never
    //     runs after Reset() because m_pipelineStarted becomes false, orphaning the
    //     workers' job. We check m_pipelineStarted BEFORE setting it to false.
    if (m_pipelineStarted) {
        m_taskOrchestrator.WaitForFrameWork();
    }

    m_pipelineStarted = false;
    m_tickCount = 0;
    m_localPlayerId = INVALID_PLAYER;
    m_writeBuffer = 0;
    m_readBuffer = 1;

    m_frameData[0].Clear();
    m_frameData[1].Clear();

    // ES: Ojo: m_localPlayerId ya vale INVALID_PLAYER aquí (se puso arriba), así que se
    //     limpia la caché de interés de INVALID_PLAYER y no la del jugador que había.
    // EN: Note: m_localPlayerId is already INVALID_PLAYER here (set above), so this clears
    //     the interest cache of INVALID_PLAYER, not the previous player's.
    m_resolver.ClearInterest(m_localPlayerId);
    m_zoneEngine.Reset();
    m_playerEngine.Reset();

    // ES: Reiniciar el estado de spawns.
    // Reset spawn state
    m_hasPendingTimer = false;
    m_directSpawnAttempts = 0;
    m_shownWaitingMsg = false;
    m_shownTimeoutMsg = false;
    m_heapScanned = false;
    m_heapScanAttempts = 0;
    m_lastHeapScan = std::chrono::steady_clock::time_point{};

    spdlog::info("SyncOrchestrator: Reset");
}

// ES: Tick principal
// ════════════════════════════════════════════════════════════════════════════
// Main Tick
// ════════════════════════════════════════════════════════════════════════════

// ES: Ejecuta las etapas del pipeline en orden. Se llama cada frame desde Core
//     (envuelto en SEH en core.cpp). Devuelve false si no está activo o no hay jugador.
// EN: Runs the pipeline stages in order. Called every frame from Core (wrapped in SEH
//     in core.cpp). Returns false if inactive or there is no player.
bool SyncOrchestrator::Tick(float deltaTime) {
    if (!m_active || m_localPlayerId == INVALID_PLAYER) return false;

    // ES: Depuración: log en cada tick los 50 primeros, luego cada 100.
    // Debug: log every tick for first 50, then every 100th
    if (m_tickCount <= 50 || m_tickCount % 100 == 0) {
        spdlog::debug("SyncOrch::Tick #{} dt={:.4f} entities={} active={}",
                      m_tickCount, deltaTime,
                      m_registry.GetPlayerEntities(m_localPlayerId).size(),
                      m_active);
    }

    // ES: Etapa 1: actualizar el estado de zonas.
    // Stage 1: Update zone state
    if (m_tickCount <= 5) spdlog::debug("SyncOrch: Stage1 UpdateZones");
    StageUpdateZones();

    // ES: Etapa 2: esperar el trabajo en segundo plano del frame anterior + intercambiar buffers.
    // Stage 2: Wait for previous frame's background work + swap buffers
    if (m_tickCount <= 5) spdlog::debug("SyncOrch: Stage2 SwapBuffers");
    StageSwapBuffers();

    // ES: Etapa 3: aplicar las posiciones interpoladas a los objetos del juego remotos.
    // Stage 3: Apply interpolated positions to remote game objects
    if (m_tickCount <= 5) spdlog::debug("SyncOrch: Stage3 ApplyRemotePositions");
    StageApplyRemotePositions();

    // ES: Etapa 4: leer las posiciones de las entidades locales y enviarlas.
    // Stage 4: Poll local entity positions and send updates
    if (m_tickCount <= 5) spdlog::debug("SyncOrch: Stage4 PollAndSendPositions");
    StagePollAndSendPositions();

    // ES: Etapa 4b: leer el equipo de las entidades locales y enviar los cambios.
    // Stage 4b: Poll local entity equipment and send changes
    StagePollAndSendEquipment();

    // ES: Etapa 5: procesar la cola de spawns.
    // Stage 5: Process spawn queue
    if (m_tickCount <= 5) spdlog::debug("SyncOrch: Stage5 ProcessSpawns");
    StageProcessSpawns();

    // ES: Etapa 6: lanzar el trabajo en segundo plano de este frame.
    // Stage 6: Kick background work for this frame
    if (m_tickCount <= 5) spdlog::debug("SyncOrch: Stage6 KickBackgroundWork");
    StageKickBackgroundWork();

    // ES: Etapa 7: actualizar estados de jugadores + diagnóstico.
    // Stage 7: Update player states + diagnostics
    if (m_tickCount <= 5) spdlog::debug("SyncOrch: Stage7 UpdatePlayers");
    StageUpdatePlayers(deltaTime);

    m_tickCount++;
    return true;
}

// ES: Etapas del pipeline
// ════════════════════════════════════════════════════════════════════════════
// Pipeline Stages
// ════════════════════════════════════════════════════════════════════════════

// ES: Etapa 1: lee la posición del personaje principal local, actualiza la zona (y avisa
//     al servidor si cambia) y reconstruye el índice de zonas cada 500 ms o al cambiar.
// EN: Stage 1: reads the local primary character position, updates the zone (and
//     notifies the server if it changed) and rebuilds the zone index every 500 ms or on change.
void SyncOrchestrator::StageUpdateZones() {
    // ES: Posición del jugador local a partir de su personaje principal.
    // Get local player position from primary character
    void* primaryChar = m_playerController.GetPrimaryCharacter();
    if (!primaryChar) return;

    Vec3 pos;
    Quat rot;
    if (!SEH_ReadPosition(primaryChar, pos, rot)) return;
    if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) return;

    // ES: Actualizar la zona del jugador local.
    // Update local player zone
    bool zoneChanged = m_zoneEngine.UpdateLocalPlayerZone(pos);
    if (zoneChanged) {
        m_playerEngine.SetLocalZone(m_zoneEngine.GetLocalZone());
    }

    // ES: Reconstruir el índice de zonas periódicamente.
    // Rebuild zone index periodically
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastZoneRebuild);
    if (elapsed.count() >= ZONE_REBUILD_INTERVAL_MS || zoneChanged) {
        m_zoneEngine.RebuildZoneIndex();
        m_lastZoneRebuild = now;
    }
}

// ES: Etapa 2: si ya hay trabajo lanzado, espera a que acabe y cambia el buffer de
//     lectura por el de escritura.
// EN: Stage 2: if work was already posted, waits for it and swaps the read and write buffers.
void SyncOrchestrator::StageSwapBuffers() {
    if (m_pipelineStarted) {
        m_taskOrchestrator.WaitForFrameWork();
        std::swap(m_readBuffer, m_writeBuffer);
    }
}

// ES: Etapa 3: escribe en los personajes remotos del juego las posiciones/rotaciones
//     interpoladas que calculó BackgroundInterpolate (buffer de lectura).
// EN: Stage 3: writes into the remote game characters the interpolated
//     positions/rotations computed by BackgroundInterpolate (read buffer).
void SyncOrchestrator::StageApplyRemotePositions() {
    // ES: El dt de la interpolación lo gestiona Interpolation::Update, que se llama desde
    //     Core; aquí solo se aplican los resultados del buffer de lectura.
    // Update interpolation timers
    // Note: deltaTime is managed by Interpolation::Update which is called from Core
    // We just apply the results from the read buffer.

    if (!m_pipelineStarted) return;

    auto& readFrame = m_frameData[m_readBuffer];
    if (!readFrame.ready) return;

    int applied = 0;

    for (auto& result : readFrame.remoteResults) {
        if (!result.valid) continue;

        // ES: Guarda: rechazar posiciones o rotaciones NaN/Inf (corrompen memoria del juego y
        //     tumban el motor).
        // Guard: reject NaN/Inf positions or rotations — they corrupt game memory and crash the engine
        if (!std::isfinite(result.position.x) || !std::isfinite(result.position.y) || !std::isfinite(result.position.z)) continue;
        if (!std::isfinite(result.rotation.w) || !std::isfinite(result.rotation.x) ||
            !std::isfinite(result.rotation.y) || !std::isfinite(result.rotation.z)) continue;

        void* gameObj = m_registry.GetGameObject(result.netId);
        if (!gameObj) continue;

        // ES: Validar el puntero: rechazar datos de strings SSO (p.ej. "one" = 0x656E6F) y
        //     otras direcciones que no son de heap. Aquí el umbral es 0x1000000 (16 MB), más
        //     estricto que el 0x10000 de EntityRegistry.
        // Validate pointer: reject SSO string data (e.g., "one" = 0x656E6F)
        // and other non-heap addresses that would crash on dereference.
        uintptr_t objAddr = reinterpret_cast<uintptr_t>(gameObj);
        if (objAddr < 0x1000000 || objAddr > 0x00007FFFFFFFFFFF) {
            m_registry.SetGameObject(result.netId, nullptr);
            spdlog::error("SyncOrch: Rejected invalid gameObject 0x{:X} for entity {} "
                         "(SSO string or bad pointer)", objAddr, result.netId);
            continue;
        }

        if (SEH_WritePositionRotation(gameObj, result.position, result.rotation)) {
            m_registry.UpdatePosition(result.netId, result.position);
            m_registry.UpdateRotation(result.netId, result.rotation);
            applied++;

            // ES: Escribir estado de animación y velocidad cuando se conocen los offsets
            //     (no hace nada si valen -1).
            // Write animation state and move speed when offsets are known.
            // Safe no-op when offsets are -1 (probing system hasn't found them yet).
            SEH_WriteMoveData(gameObj, result.moveSpeed, result.animState);
        } else {
            // ES: Desenlazar el objeto del juego malo.
            // Unlink bad game object
            m_registry.SetGameObject(result.netId, nullptr);
        }
    }

    if (applied > 0 && (m_tickCount <= 5 || m_tickCount % 100 == 0)) {
        spdlog::info("SyncOrch::ApplyRemote: applied {} this frame (tick={})",
                     applied, m_tickCount);
    }
}

// ES: Etapa 4: cada KMP_TICK_INTERVAL_MS lee cada entidad local que se haya movido lo
//     suficiente y envía un C2S_PositionUpdate (una entidad por paquete, no fiable).
//     Posible problema (sin verificar): BackgroundReadEntities también actualiza
//     lastPosition en el registro, así que esta comparación puede ver una distancia
//     casi nula y saltarse envíos.
// EN: Stage 4: every KMP_TICK_INTERVAL_MS reads each local entity that moved enough
//     and sends a C2S_PositionUpdate (one entity per packet, unreliable).
//     Possible issue (unverified): BackgroundReadEntities also updates lastPosition in
//     the registry, so this comparison may see an almost-zero distance and skip sends.
void SyncOrchestrator::StagePollAndSendPositions() {
    if (m_localPlayerId == INVALID_PLAYER) return;

    // ES: Limitar a la frecuencia de tick.
    // Throttle to tick rate
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPollTime);
    if (elapsed.count() < KMP_TICK_INTERVAL_MS) return;
    m_lastPollTime = now;

    auto localEntities = m_registry.GetPlayerEntities(m_localPlayerId);
    if (localEntities.empty()) return;

    for (EntityID netId : localEntities) {
        auto infoCopy = m_registry.GetInfo(netId);
        if (!infoCopy) continue;

        void* gameObj = m_registry.GetGameObject(netId);
        if (!gameObj) continue;

        // ES: Leer posición, rotación, velocidad y animación del personaje. SEH_ReadCharacterBG
        //     devuelve 0 en velocidad/animación si los offsets valen -1; en ese caso se usan
        //     los valores derivados de abajo.
        // Read position, rotation, moveSpeed, and animState from the character.
        // SEH_ReadCharacterBG reads all fields via CharacterAccessor (returns 0
        // for moveSpeed/animState when offsets are -1, which is fine — we fall
        // back to derived values below).
        BGReadResult rd = SEH_ReadCharacterBG(gameObj);
        if (!rd.valid) {
            m_registry.SetGameObject(netId, nullptr);
            continue;
        }
        Vec3 pos = rd.pos;
        Quat rotation = rd.rot;

        if (pos.DistanceTo(infoCopy->lastPosition) < KMP_POS_CHANGE_THRESHOLD) continue;

        float elapsedSec = elapsed.count() / 1000.f;
        float dist = pos.DistanceTo(infoCopy->lastPosition);
        float computedSpeed = (elapsedSec > 0.001f) ? dist / elapsedSec : 0.f;

        // ES: Preferir la velocidad leída del juego; si no, la calculada con el desplazamiento.
        // Prefer game-read moveSpeed; fall back to computed speed from position delta
        float moveSpeed = rd.speed;
        if (moveSpeed <= 0.f && computedSpeed > 0.f) {
            moveSpeed = computedSpeed;
        }

        uint32_t compQuat = rotation.Compress();

        // ES: Preferir la animación leída del juego; si no, heurística por velocidad
        //     (1 = andar, 2 = correr).
        // Prefer game-read animState; fall back to speed-derived heuristic
        uint8_t animState = rd.animState;
        if (animState == 0 && moveSpeed > 0.5f) {
            animState = (moveSpeed > 5.0f) ? 2 : 1;
        }

        // ES: Codificar velocidad a uint8 (0-15 m/s → 0-255); flag 0x01 = corriendo (>3 m/s).
        // EN: Encode speed as uint8 (0-15 m/s → 0-255); flag 0x01 = running (>3 m/s).
        uint8_t moveSpeedU8 = static_cast<uint8_t>(
            std::min(255.f, moveSpeed / 15.f * 255.f));
        uint16_t flags = (moveSpeed > 3.0f) ? 0x01 : 0x00;

        // ES: Paquete: cabecera, número de entidades (1), id, x, y, z, rotación comprimida,
        //     animación, velocidad, flags.
        // EN: Packet: header, entity count (1), id, x, y, z, compressed rotation, animation,
        //     speed, flags.
        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_PositionUpdate);
        writer.WriteU8(1);
        writer.WriteU32(netId);
        writer.WriteF32(pos.x);
        writer.WriteF32(pos.y);
        writer.WriteF32(pos.z);
        writer.WriteU32(compQuat);
        writer.WriteU8(animState);
        writer.WriteU8(moveSpeedU8);
        writer.WriteU16(flags);

        m_client.SendUnreliable(writer.Data(), writer.Size());

        m_registry.UpdatePosition(netId, pos);
        m_registry.UpdateRotation(netId, rotation);
    }

    // ES: NOTA: el hilo de fondo también construye un paquete en packetBytes desde
    //     BackgroundReadEntities(), pero aquí ya se han enviado actualizaciones frescas por
    //     entidad en el hilo principal. Enviar packetBytes duplicaría cada actualización y
    //     el ancho de banda de salida. Se omite.
    // NOTE: Background thread also builds a cached packet in packetBytes via
    // BackgroundReadEntities(), but we already sent fresh per-entity updates
    // above on the main thread.  Sending packetBytes here would duplicate
    // every position update, doubling outbound bandwidth.  Skipped.
}

// ES: Lectura de una ranura de equipo protegida con SEH. Devuelve el id de plantilla del
//     objeto, o 0 si falla o está vacía. Tiene que ser función libre porque __try no
//     convive con objetos C++ con destructor.
// SEH-protected equipment slot read.  Returns the item template ID for a
// given equipment slot, or 0 on failure/empty.  Must be a free function
// because __try cannot coexist with C++ unwind objects.
static uint32_t SEH_ReadEquipmentSlot(void* gameObj, EquipSlot slot) {
    uint32_t result = 0;
    __try {
        game::CharacterAccessor accessor(gameObj);
        uintptr_t itemPtr = accessor.GetEquipmentSlot(slot);
        if (itemPtr == 0) return 0;
        // ES: Leer el id de plantilla desde el puntero al objeto (ItemOffsets::templateId =
        //     0x40 = data/GameData*; corregido en audit-14, antes 0x20 caía dentro de
        //     displayName y daba basura).
        // Read template ID from item pointer (ItemOffsets::templateId = 0x40 = data/GameData*,
        // corregido en audit-14; antes 0x20 caía dentro de displayName → basura).
        static const game::ItemOffsets itemOffsets;
        Memory::Read(itemPtr + itemOffsets.templateId, result);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0;
    }
    return result;
}

// ES: Etapa 4b: cada 2 s compara el equipo de cada entidad local con el guardado y
//     envía un C2S_EquipmentUpdate fiable por cada ranura que haya cambiado.
// EN: Stage 4b: every 2 s compares each local entity's equipment with the cached one
//     and sends a reliable C2S_EquipmentUpdate for each slot that changed.
void SyncOrchestrator::StagePollAndSendEquipment() {
    if (m_localPlayerId == INVALID_PLAYER) return;

    // ES: Limitar: el equipo cambia poco, se sondea cada 2 segundos.
    // Throttle: equipment changes are infrequent, poll every 2 seconds
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastEquipmentPollTime);
    if (elapsed.count() < EQUIPMENT_POLL_INTERVAL_MS) return;
    m_lastEquipmentPollTime = now;

    auto localEntities = m_registry.GetPlayerEntities(m_localPlayerId);
    if (localEntities.empty()) return;

    for (EntityID netId : localEntities) {
        auto infoCopy = m_registry.GetInfo(netId);
        if (!infoCopy) continue;

        void* gameObj = m_registry.GetGameObject(netId);
        if (!gameObj) continue;

        // ES: Leer todas las ranuras y comparar con el estado guardado.
        // Read all equipment slots and diff against cached state
        constexpr int SLOT_COUNT = static_cast<int>(EquipSlot::Count);
        for (int s = 0; s < SLOT_COUNT; s++) {
            uint32_t currentId = SEH_ReadEquipmentSlot(gameObj, static_cast<EquipSlot>(s));
            uint32_t cachedId  = infoCopy->lastEquipment[s];

            if (currentId == cachedId) continue;

            // ES: La ranura cambió: guardar el valor nuevo y enviar la actualización.
            // Slot changed — send update and cache new value
            m_registry.UpdateEquipment(netId, s, currentId);

            PacketWriter writer;
            writer.WriteHeader(MessageType::C2S_EquipmentUpdate);
            MsgEquipmentUpdate msg{};
            msg.entityId = netId;
            msg.slot = static_cast<uint8_t>(s);
            msg.itemTemplateId = currentId;
            writer.WriteRaw(&msg, sizeof(msg));
            m_client.SendReliable(writer.Data(), writer.Size());

            spdlog::debug("SyncOrch: Equipment change entity={} slot={} {} -> {}",
                          netId, s, cachedId, currentId);
        }
    }
}

// ES: Etapa 5: gestiona los spawns de jugadores remotos pendientes: escaneo del heap de
//     GameData para encontrar plantillas, mensajes al usuario mientras espera y, tras
//     15 s sin que el hook de creación de NPC los haya resuelto, spawn directo de reserva.
// EN: Stage 5: handles pending remote player spawns: GameData heap scan to find
//     templates, user messages while waiting and, after 15 s without the NPC creation
//     hook resolving them, a fallback direct spawn.
void SyncOrchestrator::StageProcessSpawns() {
    auto& nativeHud = Core::Get().GetNativeHud();

    // ES: Escaneo del heap con reintentos: hasta 5 veces si no se encuentran plantillas del
    //     mod. El primer escaneo puede fallar si aún no se había capturado GameDataManager.
    // Heap scan with retry — runs up to 5 times if mod templates aren't found.
    // The first scan may miss templates if GameDataManager wasn't captured yet.
    bool needsScan = !m_heapScanned ||
        (m_heapScanned && m_spawnManager.GetModTemplateCount() == 0 && m_heapScanAttempts < 5);
    // ES: Espera mínima: no volver a escanear más de una vez cada 5 segundos.
    // Cooldown: don't re-scan more than once every 5 seconds
    auto timeSinceLastScan = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_lastHeapScan);
    if (needsScan && m_spawnManager.IsReady() && timeSinceLastScan.count() >= 5) {
        if (m_spawnManager.GetManagerPointer() != 0 || m_spawnManager.GetTemplateCount() < 10) {
            m_heapScanAttempts++;
            m_lastHeapScan = std::chrono::steady_clock::now();
            spdlog::info("SyncOrch: Triggering GameData heap scan #{} (manager=0x{:X}, templates={})...",
                         m_heapScanAttempts, m_spawnManager.GetManagerPointer(),
                         m_spawnManager.GetTemplateCount());
            m_spawnManager.ScanGameDataHeap();
            m_spawnManager.FindModTemplates();
            // ES: Solo se marca como terminado si se encontraron plantillas del mod o se agotaron
            //     los reintentos.
            // Only mark as fully done if we found mod templates or exhausted retries
            if (m_spawnManager.GetModTemplateCount() > 0 || m_heapScanAttempts >= 5) {
                m_heapScanned = true;
            }
            spdlog::info("SyncOrch: Heap scan #{} complete, {} templates ({} mod templates)",
                         m_heapScanAttempts, m_spawnManager.GetTemplateCount(),
                         m_spawnManager.GetModTemplateCount());
        }
    }

    size_t pending = m_spawnManager.GetPendingSpawnCount();

    // ES: Control del temporizador de espera.
    // Timer tracking
    if (pending > 0 && !m_hasPendingTimer) {
        m_firstPendingTime = std::chrono::steady_clock::now();
        m_hasPendingTimer = true;
        m_shownWaitingMsg = false;
        m_shownTimeoutMsg = false;
        spdlog::info("SyncOrch: {} spawn(s) queued", pending);
        nativeHud.LogStep("GAME", std::to_string(pending) + " spawn(s) queued");
        nativeHud.AddSystemMessage("Waiting for game to create an NPC for remote player...");
    } else if (pending == 0) {
        m_hasPendingTimer = false;
        m_directSpawnAttempts = 0;
        m_shownWaitingMsg = false;
        m_shownTimeoutMsg = false;
    }

    // ES: Mensajes de estado mientras se espera (3 s y 15 s).
    // Status updates while waiting
    if (pending > 0 && m_hasPendingTimer) {
        auto pendingDuration = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_firstPendingTime);

        if (pendingDuration.count() >= 3 && !m_shownWaitingMsg) {
            m_shownWaitingMsg = true;
            nativeHud.AddSystemMessage("Spawning remote player...");
        }
        if (pendingDuration.count() >= 15 && !m_shownTimeoutMsg) {
            m_shownTimeoutMsg = true;
            spdlog::warn("SyncOrch: Spawn queue waiting 15s+ — hook may not be firing");
            nativeHud.AddSystemMessage("Spawn delayed — walk near a town or camp to trigger NPC creation.");
        }
    }

    // ES: Log periódico (cada 5 s).
    // Periodic log
    auto sinceLog = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_lastSpawnLog);
    if (sinceLog.count() >= 5 && pending > 0) {
        m_lastSpawnLog = std::chrono::steady_clock::now();
        spdlog::info("SyncOrch: {} pending spawns (inPlace={}, charTemplates={}, ready={}, preCall={})",
                     pending, entity_hooks::GetInPlaceSpawnCount(),
                     m_spawnManager.GetCharacterTemplateCount(),
                     m_spawnManager.IsReady(), m_spawnManager.HasPreCallData());
    }

    // ES: Spawn directo de reserva: si la reproducción in situ no ha saltado, se usa
    //     SpawnCharacterDirect. El crash original (uso tras liberar en faction+0x250) se
    //     evita sobrescribiendo char+0x10 con el puntero de facción del jugador local
    //     (siempre válido) justo tras crearlo. (El comentario original dice 5 s, pero el
    //     código espera 15 s, ver abajo.)
    // Direct spawn fallback: if in-place replay hasn't fired after 5s,
    // use SpawnCharacterDirect. The original crash (use-after-free on faction+0x250)
    // is fixed by overwriting char+0x10 with the local player's faction pointer
    // (always valid) immediately after creation.
    if (pending > 0 && m_hasPendingTimer) {
        auto pendingDuration = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_firstPendingTime);

        // ES: Log del estado de espera.
        // Log waiting status
        if (pendingDuration.count() >= 5 && pendingDuration.count() % 5 == 0) {
            static int64_t s_lastLogSec = 0;
            if (pendingDuration.count() != s_lastLogSec) {
                s_lastLogSec = pendingDuration.count();
                spdlog::info("SyncOrch: {} spawns pending for {}s", pending, pendingDuration.count());
            }
        }

        // ES: Tras 15 s, intentar el spawn directo (máx. 5 intentos, separados 3 s). Se espera
        //     15 s para dar tiempo al secuestro de NPC (Hook_CharacterCreate): al andar cerca de
        //     una ciudad se crean NPCs que el hook puede tomar. El spawn directo clona una
        //     estructura que necesita datos "pre-call" recientes de un spawn de NPC.
        // After 15s, try direct spawn (max 5 attempts, 3s apart).
        // Wait 15s to give NPC hijack (Hook_CharacterCreate) time to fire — walking
        // near a town will spawn NPCs that the hook can take over. Direct spawn uses
        // struct clone which needs fresh pre-call data from a recent NPC spawn.
        if (pendingDuration.count() >= 15 && m_directSpawnAttempts < 5 &&
            m_spawnManager.HasPreCallData()) {
            auto sinceLastAttempt = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - m_lastDirectAttempt);
            if (m_directSpawnAttempts == 0 || sinceLastAttempt.count() >= 3) {
                m_lastDirectAttempt = std::chrono::steady_clock::now();
                m_directSpawnAttempts++;

                SpawnRequest spawnReq;
                if (m_spawnManager.PopNextSpawn(spawnReq)) {
                    // ES: Asignar el dueño a una ranura de plantilla del mod (misma lógica que
                    //     ProcessSpawnQueueFromHook).
                    // Map owner to mod template slot (same logic as ProcessSpawnQueueFromHook)
                    int templateCount = m_spawnManager.GetModTemplateCount();
                    int modSlot = 0;
                    if (templateCount > 1 && spawnReq.owner > 0) {
                        modSlot = (spawnReq.owner - 1) % templateCount;
                    }
                    if (modSlot >= templateCount) modSlot = 0;

                    spdlog::info("SyncOrch: DIRECT SPAWN attempt #{} for entity {} owner={} slot={}",
                                 m_directSpawnAttempts, spawnReq.netId, spawnReq.owner, modSlot);
                    nativeHud.AddSystemMessage("Attempting direct spawn...");

                    void* character = m_spawnManager.SpawnCharacterDirect(&spawnReq.position, modSlot);
                    if (character) {
                        // ES: ARREGLO: comprobar que la entidad sigue en el registro antes de SetGameObject.
                        //     Si el jugador dueño se desconectó durante el spawn, la entidad ya se habrá
                        //     borrado; sin esta comprobación el objeto creado existiría en el mundo sin
                        //     seguimiento (huérfano).
                        // BUG FIX: Check if the entity still exists in the registry before
                        // calling SetGameObject. If the owning player disconnected while the
                        // spawn was in progress, the entity will have been unregistered. Without
                        // this check the spawned game object would exist in the world but not be
                        // tracked, becoming an orphan.
                        auto info = m_registry.GetInfo(spawnReq.netId);
                        if (!info.has_value()) {
                            spdlog::warn("SyncOrch: Entity {} no longer exists (owner disconnected?), "
                                         "skipping direct spawn setup", spawnReq.netId);
                            entity_hooks::DecrementSpawnCount(spawnReq.owner);
                            // ES: El personaje queda en el mundo sin seguimiento: un NPC inofensivo.
                            // character is leaked into the world but not tracked — harmless NPC
                        } else {
                            // ES: Arreglar el uso tras liberar de la facción: escribir la facción del jugador local.
                            // Fix the faction use-after-free: write local player's faction
                            uintptr_t localFaction = Core::Get().GetPlayerController().GetLocalFactionPtr();
                            if (!SEH_FixFactionAndSetup(character, localFaction, spawnReq.position)) {
                                spdlog::error("SyncOrch: Faction fix failed for entity {}", spawnReq.netId);
                            }

                            // ES: Registrar en el registro de entidades.
                            // Register in entity registry
                            m_registry.SetGameObject(spawnReq.netId, character);
                            m_registry.UpdatePosition(spawnReq.netId, spawnReq.position);

                            // ES: Configuración completa tras el spawn (nombre, IA, escuadrón, control, sonda de animación).
                            // Full post-spawn setup (name, AI, squad)
                            Core::Get().GetPlayerController().OnRemoteCharacterSpawned(
                                spawnReq.netId, character, spawnReq.owner);
                            ai_hooks::MarkRemoteControlled(character);
                            squad_hooks::AddCharacterToLocalSquad(character);
                            game::WritePlayerControlled(reinterpret_cast<uintptr_t>(character), true);
                            game::ScheduleDeferredAnimClassProbe(reinterpret_cast<uintptr_t>(character));

                            spdlog::info("SyncOrch: DIRECT SPAWN SUCCESS — entity {} at ({:.0f},{:.0f},{:.0f})",
                                         spawnReq.netId, spawnReq.position.x, spawnReq.position.y,
                                         spawnReq.position.z);
                            nativeHud.AddSystemMessage("Remote player spawned!");
                        }
                    } else {
                        // ES: La factoría devolvió null → reencolar para reintentar.
                        // Factory returned null — re-queue for retry
                        spdlog::warn("SyncOrch: DIRECT SPAWN FAILED — factory returned null");
                        spawnReq.retryCount++;
                        if (spawnReq.retryCount < MAX_SPAWN_RETRIES) {
                            m_spawnManager.RequeueSpawn(spawnReq);
                        } else {
                            // ES: ARREGLO: liberar el hueco del límite de spawns al agotar los reintentos. Sin esto
                            //     el hueco queda ocupado para siempre y bloquea futuros spawns.
                            // BUG FIX: Release the spawn cap slot when retries are exhausted.
                            // Without this, the slot stays held forever and blocks future spawns.
                            spdlog::warn("SyncOrch: DIRECT SPAWN retries exhausted for entity {} owner={}, "
                                         "releasing spawn cap", spawnReq.netId, spawnReq.owner);
                            entity_hooks::DecrementSpawnCount(spawnReq.owner);
                        }
                        nativeHud.AddSystemMessage("Direct spawn failed, will retry...");
                    }
                }
            }
        }
    }
}

// ES: Etapa 6: vacía el buffer de escritura y lanza en el pool los dos trabajos de fondo
//     (leer entidades locales e interpolar remotas).
// EN: Stage 6: clears the write buffer and posts the two background jobs to the pool
//     (read local entities and interpolate remote ones).
void SyncOrchestrator::StageKickBackgroundWork() {
    m_frameData[m_writeBuffer].Clear();

    m_taskOrchestrator.PostFrameWork([this] { BackgroundReadEntities(); });
    m_taskOrchestrator.PostFrameWork([this] { BackgroundInterpolate(); });

    m_pipelineStarted = true;
}

// ES: Etapa 7: comprobación de AFK y log de diagnóstico.
// EN: Stage 7: AFK check and diagnostic log.
void SyncOrchestrator::StageUpdatePlayers(float deltaTime) {
    // ES: Comprobación de AFK cada 200 ticks ("cada 10 s" solo si el tick va a 20 Hz; en
    //     realidad Tick se llama una vez por frame).
    // AFK check every 10 seconds
    if (m_tickCount % 200 == 0) {
        auto afkPlayers = m_playerEngine.CheckAFK();
        for (PlayerID id : afkPlayers) {
            auto* session = m_playerEngine.GetSession(id);
            if (session) {
                spdlog::info("SyncOrch: Player {} '{}' is now AFK", id, session->name);
            }
        }
    }

    // ES: Diagnóstico cada 5 segundos.
    // Diagnostics every 5 seconds
    m_diagTickCount++;
    auto diagNow = std::chrono::steady_clock::now();
    auto diagElapsed = std::chrono::duration_cast<std::chrono::seconds>(diagNow - m_lastDiagLog);
    if (diagElapsed.count() >= 5) {
        spdlog::info("SyncOrch::Tick: {} ticks in {}s, entities={}, remote={}, zone=({},{})",
                     m_diagTickCount, diagElapsed.count(),
                     m_registry.GetEntityCount(), m_registry.GetRemoteCount(),
                     m_zoneEngine.GetLocalZone().x, m_zoneEngine.GetLocalZone().y);
        m_diagTickCount = 0;
        m_lastDiagLog = diagNow;
    }
}

// ES: Trabajos en segundo plano (corren en hilos del TaskOrchestrator)
// ════════════════════════════════════════════════════════════════════════════
// Background Workers
// ════════════════════════════════════════════════════════════════════════════

// ES: Lee todas las entidades locales que se han movido, las guarda en el buffer de
//     escritura y arma un paquete C2S_PositionUpdate en packetBytes (que no se envía, ver
//     nota en la etapa 4). También actualiza posición/rotación en el registro.
//     La velocidad calculada asume un frame de 16 ms.
// EN: Reads every local entity that moved, stores them in the write buffer and builds
//     a C2S_PositionUpdate packet in packetBytes (not sent, see the stage 4 note). It
//     also updates position/rotation in the registry. The computed speed assumes a 16 ms frame.
void SyncOrchestrator::BackgroundReadEntities() {
    auto& writeFrame = m_frameData[m_writeBuffer];

    auto localEntities = m_registry.GetPlayerEntities(m_localPlayerId);

    // ES: Posición pendiente de empaquetar.
    // EN: Position pending packing.
    struct PendingPos {
        CharacterPosition cp;
        EntityID netId;
        Vec3 pos;
        Quat rot;
    };
    std::vector<PendingPos> pendingPositions;

    for (EntityID netId : localEntities) {
        void* gameObj = m_registry.GetGameObject(netId);
        if (!gameObj) continue;

        auto infoCopy = m_registry.GetInfo(netId);
        if (!infoCopy || infoCopy->isRemote) continue;
        Vec3 lastPos = infoCopy->lastPosition;

        BGReadResult rd = SEH_ReadCharacterBG(gameObj);
        if (!rd.valid) continue;

        Vec3 pos = rd.pos;
        Quat rot = rd.rot;

        if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) continue;

        float dist = pos.DistanceTo(lastPos);
        if (dist < KMP_POS_CHANGE_THRESHOLD) continue;

        float computedSpeed = dist / 0.016f;
        float speed = rd.speed;
        if (speed <= 0.f && computedSpeed > 0.f) {
            speed = computedSpeed;
        }

        uint8_t animState = rd.animState;
        if (animState == 0 && speed > 0.5f) {
            animState = (speed > 5.0f) ? 2 : 1;
        }

        CachedEntityPos cached;
        cached.netId = netId;
        cached.position = pos;
        cached.rotation = rot;
        cached.speed = speed;
        cached.animState = animState;
        cached.dirty = true;
        writeFrame.localEntities.push_back(cached);

        PendingPos pp;
        pp.cp.entityId = netId;
        pp.cp.posX = pos.x;
        pp.cp.posY = pos.y;
        pp.cp.posZ = pos.z;
        pp.cp.compressedQuat = rot.Compress();
        pp.cp.animStateId = animState;
        pp.cp.moveSpeed = static_cast<uint8_t>(std::min(255.f, speed / 15.f * 255.f));
        pp.cp.flags = (speed > 3.0f) ? 0x01 : 0x00;
        pp.netId = netId;
        pp.pos = pos;
        pp.rot = rot;
        pendingPositions.push_back(pp);
    }

    // ES: Construir el paquete con todas las posiciones pendientes.
    // EN: Build the packet with all pending positions.
    if (!pendingPositions.empty()) {
        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_PositionUpdate);
        writer.WriteU8(static_cast<uint8_t>(pendingPositions.size()));
        for (auto& pp : pendingPositions) {
            writer.WriteRaw(&pp.cp, sizeof(pp.cp));
            m_registry.UpdatePosition(pp.netId, pp.pos);
            m_registry.UpdateRotation(pp.netId, pp.rot);
        }
        writeFrame.packetBytes = std::move(writer.Buffer());
    }

    writeFrame.ready = true;
}

// ES: Calcula la posición interpolada de cada entidad remota para "ahora" y la guarda
//     en el buffer de escritura; la etapa 3 del siguiente frame la aplica.
// EN: Computes the interpolated position of each remote entity for "now" and stores it
//     in the write buffer; stage 3 of the next frame applies it.
void SyncOrchestrator::BackgroundInterpolate() {
    auto& writeFrame = m_frameData[m_writeBuffer];

    auto remoteEntities = m_registry.GetRemoteEntities();
    float now = SessionTime();

    for (EntityID remoteId : remoteEntities) {
        CachedRemoteResult result;
        result.netId = remoteId;

        uint8_t moveSpeed = 0;
        uint8_t animState = 0;
        if (m_interpolation.GetInterpolated(remoteId, now,
                                             result.position, result.rotation,
                                             moveSpeed, animState)) {
            result.moveSpeed = moveSpeed;
            result.animState = animState;
            result.valid = true;
        }

        writeFrame.remoteResults.push_back(result);
    }
}

// ES: Gestión de prioridades
// ════════════════════════════════════════════════════════════════════════════
// Priority Management
// ════════════════════════════════════════════════════════════════════════════

// ES: Critical si está en la zona del jugador local, Normal si es adyacente, None si no.
//     (Low nunca se devuelve.)
// EN: Critical if in the local player's zone, Normal if adjacent, None otherwise.
//     (Low is never returned.)
SyncPriority SyncOrchestrator::ComputePriority(EntityID entityId) const {
    auto infoCopy = m_registry.GetInfo(entityId);
    if (!infoCopy) return SyncPriority::None;

    ZoneCoord localZone = m_zoneEngine.GetLocalZone();

    // ES: Misma zona que el jugador local.
    // Same zone as local player
    if (infoCopy->zone == localZone) return SyncPriority::Critical;

    // ES: Zona adyacente (rejilla 3x3).
    // Adjacent zone (3x3 grid)
    if (localZone.IsAdjacent(infoCopy->zone)) return SyncPriority::Normal;

    // ES: Todo lo demás.
    // Everything else
    return SyncPriority::None;
}

// ES: ¿Toca sincronizar esta entidad en este tick? Critical/Normal siempre, Low en
//     ticks pares, None nunca.
// EN: Should this entity be synced this tick? Critical/Normal always, Low on even
//     ticks, None never.
bool SyncOrchestrator::ShouldSyncThisTick(EntityID entityId) const {
    SyncPriority prio = ComputePriority(entityId);
    switch (prio) {
    case SyncPriority::Critical: return true;                     // Every tick (20Hz)
    case SyncPriority::Normal:   return true;                     // Every tick (20Hz)
    case SyncPriority::Low:      return (m_tickCount % 2) == 0;  // Every other tick (10Hz)
    case SyncPriority::None:     return false;
    }
    return false;
}

} // namespace kmp
