// ES: core.h — Declaración del singleton kmp::Core, el corazón del plugin KenshiMP.Core.dll.
//     Core posee todos los subsistemas (escáner de patrones, hooks, cliente de red ENet, registro
//     de entidades, interpolación, spawns, UI, orquestadores de sync) y gobierna el ciclo de vida
//     del cliente con la máquina de estados ClientPhase. Además declara las funciones Reset*() que
//     re-arman los fixes one-shot del personaje del host (definidos en core.cpp).
// EN: core.h — Declaration of the kmp::Core singleton, the heart of the KenshiMP.Core.dll plugin.
//     Core owns every subsystem (pattern scanner, hooks, ENet network client, entity registry,
//     interpolation, spawns, UI, sync orchestrators) and drives the client lifecycle through the
//     ClientPhase state machine. It also declares the Reset*() functions that re-arm the one-shot
//     fixes applied to the host character (defined in core.cpp).
#pragma once
#include "kmp/scanner.h"
#include "kmp/patterns.h"
#include "kmp/hook_manager.h"
#include "kmp/orchestrator.h"
#include "kmp/config.h"
#include "net/client.h"
#include "sync/entity_registry.h"
#include "sync/interpolation.h"
#include "game/game_types.h"
#include "game/spawn_manager.h"
#include "game/player_controller.h"
#include "game/loading_orchestrator.h"
#include "game/lobby_manager.h"
#include "game/shared_save_sync.h"
#include "ui/overlay.h"
#include "ui/native_hud.h"
#include "sync/sync_orchestrator.h"
#include "sync/sync_facilitator.h"
#include "sync/pipeline_orchestrator.h"
#include "sys/task_orchestrator.h"
#include "sys/frame_data.h"
#include "sdk/kenshi_sdk.h"
#include "sdk/visual_proxy.h"
#include "hooks/entity_hooks.h"
#include "hooks/char_tracker_hooks.h"
#include "hooks/ai_hooks.h"
#include "game_command_queue.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>

namespace kmp {

// ES: Fases del ciclo de vida del cliente — máquina de estados determinista.
//     Las transiciones ocurren en sitios concretos y bien definidos (Core::TransitionTo).
// Client lifecycle phases — deterministic state machine.
// Transitions happen in specific, well-defined places.
enum class ClientPhase : uint8_t {
    Startup,           // DLL loaded, pattern scan + hook install. Present not yet firing.
    MainMenu,          // Splash done, user on main menu. Present fires at high fps.
    Loading,           // User clicked New Game/Continue/Load. Game is blocking on load.
                       // Detected by a >2s gap between Present calls.
    GameReady,         // World loaded, characters exist. OnGameLoaded has fired.
                       // Auto-connect can proceed. CharacterCreate hook still disabled.
    Connecting,        // ConnectAsync called, waiting for handshake.
    Connected,         // Handshake done, CharacterCreate hook enabled, entities syncing.
};

// ES: Convierte una ClientPhase a texto para los logs.
// EN: Converts a ClientPhase to text for logging.
inline const char* ClientPhaseToString(ClientPhase p) {
    switch (p) {
        case ClientPhase::Startup:    return "Startup";
        case ClientPhase::MainMenu:   return "MainMenu";
        case ClientPhase::Loading:    return "Loading";
        case ClientPhase::GameReady:  return "GameReady";
        case ClientPhase::Connecting: return "Connecting";
        case ClientPhase::Connected:  return "Connected";
        default:                      return "Unknown";
    }
}

// ES: Definida en core.cpp — reinicia el temporizador de keepalive al reconectar.
// Defined in core.cpp — resets keepalive timer on reconnect
void ResetKeepaliveTimer();

// Defined in core.cpp — re-arma el fix de facción del host en disconnect / nueva carga
// EN: Defined in core.cpp — re-arms the host faction fix on disconnect / new load.
void ResetHostFactionFix();

// Defined in core.cpp — purga los cachés de Nameless (s_cachedNamelessFaction/FR,
// s_namelessResolveState) en disconnect / nueva carga. Sin esto, tras recargar un save el motor
// libera/recrea las facciones y esos punteros quedan colgando — los hooks isAlly/isEnemy seguirían
// "sustituyendo por Nameless" contra memoria reciclada (hallazgo de la revisión de interacciones,
// 2026-07-13). ResolveNamelessFactionOnce es idempotente y se re-arma solo tras esto.
// EN: Defined in core.cpp — purges the Nameless caches (s_cachedNamelessFaction/FR,
//     s_namelessResolveState) on disconnect / new load. Without it, after reloading a save the engine
//     frees/recreates the factions and those pointers dangle — the isAlly/isEnemy hooks would keep
//     "substituting Nameless" against recycled memory (finding from the interaction review,
//     2026-07-13). ResolveNamelessFactionOnce is idempotent and re-arms itself after this.
void ResetNamelessResolve();

// Defined in core.cpp — re-arma el [FIX-HOSTREL]: pasada one-shot que reescribe a -100 las
// relaciones de facción CORRUPTAS del host (37 entries explícitas que deberían ser hostiles pero
// quedaron en neutral 0.00/-10, heredadas de un clon corrupto, y que ANULAN el defaultRelation=-100
// → ningún NPC contraataca al host, "todos huyen"). Se re-arma en disconnect / nueva carga por si el
// motor recarga las relaciones al cargar un save (hallazgo RE en vivo, 2026-07-13).
// EN: Defined in core.cpp — re-arms [FIX-HOSTREL]: a one-shot pass that rewrites to -100 the host
//     CORRUPTED faction relations (37 explicit entries that should be hostile but were left at neutral
//     0.00/-10, inherited from a corrupted clone, and that OVERRIDE defaultRelation=-100 → no NPC fights
//     back against the host, "everyone flees"). Re-armed on disconnect / new load in case the engine
//     reloads relations when a save is loaded (live RE finding, 2026-07-13).
void ResetHostFactionRelationsFix();

// Defined in core.cpp — re-arma el SEED de char+0xD0 (lastProcessed) del host
// (Fase 4: desbloqueo del AI tick de combate) en disconnect / nueva carga.
// EN: Defined in core.cpp — re-arms the SEED of the host char+0xD0 (lastProcessed)
//     (Phase 4: unblocking the combat AI tick) on disconnect / new load.
void ResetHostSimSeedFix();

// Defined in core.cpp — re-arma el SEED de char+0x4C0 (timestamp medico) del host
// (FIX-MEDSEED: desbloquea el consumo de comida/necesidades evitando dt<0 en
// MedicalSystem::periodicUpdate 0x64DA70) en disconnect / nueva carga.
// EN: Defined in core.cpp — re-arms the SEED of the host char+0x4C0 (medical timestamp)
//     (FIX-MEDSEED: unblocks food/needs consumption by avoiding dt<0 in
//     MedicalSystem::periodicUpdate 0x64DA70) on disconnect / new load.
void ResetHostMedSeedFix();

// Defined in core.cpp — re-arma el FIX-CONTROL (SetControlledChar 0x802520: vincula la
// facción del host como "controlada por el jugador", faction+0x250=PI → el gate de la rama
// viva 0x5CD1E3 exime al char del host del umbral 0.75 → su "think" de combate/cura corre
// siempre). CAUSA 2 del combate congelado. Se re-arma en disconnect / nueva carga.
// EN: Defined in core.cpp — re-arms FIX-CONTROL (SetControlledChar 0x802520: marks the host
//     faction as "player controlled", faction+0x250=PI → the gate of the live branch 0x5CD1E3
//     exempts the host char from the 0.75 threshold → its combat/heal "think" always runs).
//     CAUSE 2 of the frozen combat. Re-armed on disconnect / new load.
void ResetHostControlledCharFix();

// Defined in core.cpp — re-arma el [FIX-COMBATCLASS] (CharBody::create 0x621460: crea la
// CombatClass = *(CharBody+0x8) del host si nació NULL porque el spawn del mod saltó
// giveBirth). CAUSA RAÍZ del combate (el char camina pero no ataca). Detrás de toggle
// kEnableCombatClassFix (OFF por defecto). Se re-arma en disconnect / nueva carga.
// EN: Defined in core.cpp — re-arms [FIX-COMBATCLASS] (CharBody::create 0x621460: creates the host
//     CombatClass = *(CharBody+0x8) if it was born NULL because the mod spawn skipped giveBirth).
//     ROOT CAUSE of combat (the char walks but never attacks). Behind toggle
//     kEnableCombatClassFix (OFF by default). Re-armed on disconnect / new load.
void ResetHostCombatClassFix();

// Defined in core.cpp — re-arma el [FIX-COMBATARM] (arranca la máquina de estados de combate del
// host llamando a CombatClass::startupState() [vtable slot +0x50] una vez, para que nextMove
// (CC+0x1F4) / combatState (CC+0x1F0) dejen de contener basura sin inicializar y el host pueda
// iniciar combate "en frío" sin usar antes un muñeco de entrenamiento). Incluye red de seguridad
// (saneado defensivo de nextMove/combatState). Se re-arma en disconnect / nueva carga.
// EN: Defined in core.cpp — re-arms [FIX-COMBATARM] (boots the host combat state machine by calling
//     CombatClass::startupState() [vtable slot +0x50] once, so nextMove (CC+0x1F4) / combatState
//     (CC+0x1F0) stop holding uninitialized garbage and the host can start combat "cold" without first
//     using a training dummy). Includes a safety net (defensive sanitizing of nextMove/combatState).
//     Re-armed on disconnect / new load.
void ResetHostCombatArmFix();

// Defined in core.cpp — re-arma el [FIX-ARMHAND] (repara el handle de brazo del AI del host:
// AI+0x318 = Character+0x458, escritura de puntero de 8 bytes a una región que ya existe dentro
// del propio Character). Sin esto el chequeo GOAP dice "brazo en estado pésimo" aunque los brazos
// estén sanos → el host no puede cargar/secuestrar, hacer primeros auxilios ni autocurarse.
// Se re-arma en disconnect / nueva carga.
// EN: Defined in core.cpp — re-arms [FIX-ARMHAND] (repairs the host AI arm handle:
//     AI+0x318 = Character+0x458, an 8-byte pointer write to a region that already exists inside the
//     Character itself). Without it the GOAP check says "arm in terrible condition" even with healthy
//     arms → the host cannot carry/kidnap, do first aid or heal itself. Re-armed on disconnect / new load.
void ResetHostArmHandFix();

// Defined in core.cpp — re-arma el [AUTOTEST] de combate (dispara attackTarget 0x5CB0A0 sobre
// el host contra un NPC enemigo cercano, una vez por carga) en disconnect / nueva carga.
// EN: Defined in core.cpp — re-arms the combat [AUTOTEST] (fires attackTarget 0x5CB0A0 on the host
//     against a nearby enemy NPC, once per load) on disconnect / new load.
void ResetHostCombatAutotest();

// Defined in core.cpp — re-arma el [FIX-PLATOON] (setActivePlatoon 0x6213F0: re-enlaza el
// ActivePlatoon del host (char+0x658) registrando AI+0x10=platoon->me, que el spawn del clon
// del mod omite → su Tasker queda huérfano y el AI tick no consume las órdenes de combate).
// CAUSA RAÍZ Fase 4 (orden encolada pero amIdle=1). Se re-arma en disconnect / nueva carga.
// EN: Defined in core.cpp — re-arms [FIX-PLATOON] (setActivePlatoon 0x6213F0: re-links the host
//     ActivePlatoon (char+0x658) by registering AI+0x10=platoon->me, which the mod clone spawn skips
//     → its Tasker is orphaned and the AI tick never consumes combat orders).
//     ROOT CAUSE of Phase 4 (order queued but amIdle=1). Re-armed on disconnect / new load.
void ResetHostPlatoonFix();

// Defined in core.cpp — re-arma el [DIAG-CLONESQUAD] (detección del CharacterHuman DUPLICADO
// en el squad del host: mismo char+0x40 template + char+0x0C==1 + mismo ActivePlatoon que el
// char real → clon que reencola GET_OUT_OF_BED sin fin, causa raíz del bug de la cama, wiki
// secc. 21). Solo diagnóstico (no despawnea: no hay función nativa segura). Se re-arma en
// disconnect / nueva carga.
// EN: Defined in core.cpp — re-arms [DIAG-CLONESQUAD] (detects the DUPLICATED CharacterHuman in the
//     host squad: same char+0x40 template + char+0x0C==1 + same ActivePlatoon as the real char →
//     a clone that re-queues GET_OUT_OF_BED forever, root cause of the bed bug, wiki section 21).
//     Diagnostic only (does not despawn: there is no safe native function). Re-armed on
//     disconnect / new load.
void ResetHostSquadCloneDetect();

// Defined in core.cpp — re-arma el [FIX-CLONESQUAD-DESPAWN] (despawn REAL del clon del squad
// del host: erase de la lektor PlayerInterface + GameWorld::removeObject 0x799AF0). Gating
// anti-falso-positivo por estabilidad (≥3 ticks con el mismo clon). Acción DESTRUCTIVA y
// one-shot; se re-arma en disconnect / nueva carga (junto a ResetHostSquadCloneDetect).
// EN: Defined in core.cpp — re-arms [FIX-CLONESQUAD-DESPAWN] (REAL despawn of the host squad clone:
//     erase from the PlayerInterface lektor + GameWorld::removeObject 0x799AF0). Anti-false-positive
//     gating by stability (≥3 ticks with the same clone). DESTRUCTIVE, one-shot action; re-armed on
//     disconnect / new load (together with ResetHostSquadCloneDetect).
void ResetHostSquadCloneDespawn();

// ES: Singleton central del mod (patrón Meyers: Core::Get()). Posee todos los subsistemas por valor
//     y expone accesores; corre en varios hilos: render (Present hook → PollForGameLoad), juego
//     (TimeUpdate hook → OnGameTick), red (NetworkThreadFunc) y workers del orquestador.
// EN: Central singleton of the mod (Meyers pattern: Core::Get()). Owns every subsystem by value and
//     exposes accessors; runs on several threads: render (Present hook → PollForGameLoad), game
//     (TimeUpdate hook → OnGameTick), network (NetworkThreadFunc) and orchestrator workers.
class Core {
public:
    static Core& Get();

    // ES: Initialize(): arranque completo (logging, VEH, escáner, hooks, red, UI, hilo de red); devuelve
    //     false si algo crítico falla (p.ej. MinHook). Shutdown(): parada ordenada protegida con SEH.
    // EN: Initialize(): full startup (logging, VEH, scanner, hooks, network, UI, network thread); returns
    //     false if something critical fails (e.g. MinHook). Shutdown(): orderly teardown guarded by SEH.
    bool Initialize();
    void Shutdown();

    // ES: Accesores a los subsistemas.
    // Accessors
    PatternScanner&      GetScanner()        { return m_scanner; }
    GameFunctions&       GetGameFunctions()   { return m_gameFuncs; }
    PatternOrchestrator& GetPatternOrchestrator() { return m_patternOrchestrator; }
    NetworkClient&    GetClient()          { return m_client; }
    EntityRegistry&   GetEntityRegistry()  { return m_entityRegistry; }
    Interpolation&    GetInterpolation()   { return m_interpolation; }
    SpawnManager&        GetSpawnManager()       { return m_spawnManager; }
    PlayerController&    GetPlayerController()  { return m_playerController; }
    LoadingOrchestrator& GetLoadingOrch()       { return m_loadingOrch; }
    LobbyManager&        GetLobbyManager()      { return m_lobbyManager; }
    Overlay&          GetOverlay()         { return m_overlay; }
    NativeHud&        GetNativeHud()       { return m_nativeHud; }
    ClientConfig&     GetConfig()          { return m_config; }

    // ES: Consultas de estado: conectado, soy host (asignado por el servidor), id del host, versión
    //     Steam vs GOG, partida cargada, cargando. SetLoading es un no-op heredado (la fase la lleva la
    //     máquina de estados).
    // EN: State queries: connected, am I host (server-assigned), host id, Steam vs GOG build,
    //     game loaded, loading. SetLoading is a legacy no-op (the phase is owned by the state machine).
    bool IsConnected() const { return m_connected; }
    bool IsHost() const { return m_isHost; }
    PlayerID GetHostPlayerId() const { return m_hostPlayerId; }
    bool IsSteamVersion() const { return m_isSteamVersion; }
    bool IsGameLoaded() const { return m_gameLoaded; }
    bool IsLoading() const { return m_clientPhase == ClientPhase::Loading; }
    void SetLoading(bool) {} // No-op — phase transitions handled by state machine

    // ES: ── Máquina de estados de fase del cliente ── TransitionTo cambia de fase y, al entrar/salir
    //     de Loading, activa/desactiva el guard de carga de CharacterIterator.
    // EN: TransitionTo changes phase and, when entering/leaving Loading, toggles the CharacterIterator
    //     loading guard.
    // ── Client phase state machine ──
    ClientPhase GetClientPhase() const { return m_clientPhase.load(std::memory_order_acquire); }
    void TransitionTo(ClientPhase newPhase);

    // ES: Lo llama HookPresent cuando detecta un hueco largo entre frames (>2s), señal de que el juego
    //     estaba bloqueado en una pantalla de carga.
    // Called by HookPresent when a long frame gap (>2s) is detected,
    // indicating the game was blocking on a load screen.
    void OnLoadingGapDetected();
    PlayerID GetLocalPlayerId() const { return m_localPlayerId.load(); }

    // ES: Punto único de entrada/salida de conexión.
    //     Al conectar: reinicia el keepalive y activa los hooks de combate/diagnóstico solo si la partida
    //     ya está cargada (si no, OnGameLoaded los activa después).
    //     Al desconectar: vuelve a GameReady, suspende CharacterCreate, desactiva hooks de combate y
    //     limpia TODO el estado de sesión (entidades remotas, colas, cachés) para evitar use-after-free
    //     y permitir reconectar.
    // EN: Single entry/exit point for connection state.
    //     On connect: resets the keepalive and enables combat/diagnostic hooks only if the game is already
    //     loaded (otherwise OnGameLoaded enables them later).
    //     On disconnect: goes back to GameReady, suspends CharacterCreate, disables combat hooks and
    //     clears ALL session state (remote entities, queues, caches) to avoid use-after-free and allow
    //     reconnecting.
    void SetConnected(bool connected) {
        m_connected = connected;
        if (connected) {
            // ES: La transición de fase la hace PacketHandler (TransitionTo(Connected)).
            // Phase transition handled by PacketHandler (TransitionTo(Connected))
            ResetKeepaliveTimer(); // Reset so first keepalive fires 5s after connect, not immediately

            // ES: Activar hooks de combate SOLO si la partida está cargada. Si se conecta desde el menú
            //     principal, se difiere a OnGameLoaded(): activarlos durante los 130+ creates de la carga
            //     provoca corrupción por el wrapper MovRaxRsp.
            // Enable combat hooks ONLY if game is loaded. If connecting from
            // main menu (game not loaded), defer to OnGameLoaded() — enabling
            // hooks during the 130+ loading creates causes MovRaxRsp corruption.
            if (m_gameLoaded) {
                HookManager::Get().Enable("CharacterDeath");
                HookManager::Get().Enable("CharacterKO");
                // [DIAG-PUSHORDER] Activa el hook de diagnóstico en Tasker::pushOrder 0x674300
                // (encolado real de órdenes; RE byte a byte 2026-06-19). Confirma si la orden de
                // ataque del HOST entra en su Tasker (char+0x658→+0x98). El slot "StartAttack"
                // (0x722EF0 UI/MyGUI) quedó refutado y su hook NO se instala — su Enable sería
                // no-op, así que ya no se invoca.
                // EN: [DIAG-PUSHORDER] Enables the diagnostic hook on Tasker::pushOrder 0x674300
                //     (the real order queue insert; byte-by-byte RE 2026-06-19). Confirms whether the HOST attack
                //     order enters its Tasker (char+0x658→+0x98). The "StartAttack" slot (0x722EF0 UI/MyGUI) was
                //     refuted and its hook is NOT installed — its Enable would be a no-op, so it is no longer called.
                HookManager::Get().Enable("PushOrder");
                // [DIAG-COMBATSEED] Activa el hook de CombatClass::update 0x60D650 (AI tick de
                // combate) para capturar el estado del CombatClass del host (frío vs tras entrenar).
                // EN: [DIAG-COMBATSEED] Enables the hook on CombatClass::update 0x60D650 (combat AI tick) to
                //     capture the state of the host CombatClass (cold vs after training).
                HookManager::Get().Enable("CombatClassUpdate");
                spdlog::info("Core: Combat hooks ENABLED (game loaded, incl. PushOrder + CombatSeed DIAG)");
            } else {
                spdlog::info("Core: Combat hooks DEFERRED (game not loaded — will enable on load)");
            }

            // ES: shared_save_sync::Init() se llama de forma perezosa desde Update() cuando llega la asignación
            //     de facción (que llega DESPUÉS del ack del handshake).
            // shared_save_sync::Init() is called lazily from Update() after
            // faction assignment arrives (which comes AFTER handshake ack).
        }
        if (!connected) {
            // ES: Volver a GameReady para que el usuario pueda reconectar.
            // Drop back to GameReady so user can reconnect
            if (m_gameLoaded) {
                TransitionTo(ClientPhase::GameReady);
            }

            // ES: Desactivar el hook CharacterCreate — las ráfagas de carga de zona estando desconectado
            //     pasarían por MovRaxRsp y corromperían el heap.
            // Disable CharacterCreate hook — zone-load bursts while disconnected
            // would go through MovRaxRsp and corrupt the heap
            entity_hooks::SuspendForDisconnect();

            // ES: Desactivar hooks de combate — no hay nada que sincronizar desconectado.
            // Disable combat hooks — no need to sync while disconnected
            HookManager::Get().Disable("CharacterDeath");
            HookManager::Get().Disable("CharacterKO");

            // ES: Limpiar entidades remotas, interpolación y proxies visuales.
            // Clean up remote entities, interpolation, and visual proxies
            size_t removed = m_entityRegistry.ClearRemoteEntities();
            m_interpolation.Clear();
            m_visualProxy.DestroyAll();
            m_playerController.Reset();
            if (removed > 0) {
                spdlog::info("Core: Cleared {} remote entities on disconnect", removed);
            }

            // ES: Reiniciar estado para una posible reconexión.
            // Reset state for potential reconnect
            m_initialEntityScanDone = false;
            m_spawnTeleportDone = false;
            m_hasHostSpawnPoint = false;
            m_isHost = false;
            m_hostPlayerId = 0;  // cleared on disconnect; repopulated via S2C_HostAssignment on reconnect
            m_hostTpTimerStarted = false;
            m_pipelineStarted = false;
            m_frameData[0].Clear();
            m_frameData[1].Clear();

            // ES: Reiniciar el estado del orquestador de sync.
            // Reset sync orchestrator state
            if (m_syncOrchestrator) {
                m_syncOrchestrator->Reset();
            }

            // ES: Reiniciar el estado del depurador de pipeline.
            // Reset pipeline debugger state
            m_pipelineOrch.Shutdown();

            // ES: Reiniciar la sincronización de partida compartida.
            // Reset shared-save sync
            shared_save_sync::Reset();

            // Cambio 2.4 / 3.2: purgar cachés de personajes al desconectar. Los Character de la
            // sesión anterior serán liberados por el motor → dejar entradas colgadas provocaría
            // UAF (tracker) o suprimir la IA de NPCs locales nuevos (remote-controlled).
            // EN: Change 2.4 / 3.2: purge character caches on disconnect. The Characters of the previous session
            //     will be freed by the engine → leaving dangling entries would cause UAF (tracker) or suppress the
            //     AI of new local NPCs (remote-controlled).
            char_tracker_hooks::Clear();
            ai_hooks::ClearRemoteControlled();

            // ES: Vaciar la cola de spawns obsoleta de la sesión anterior.
            // Clear stale spawn queue from previous session
            m_spawnManager.ClearSpawnQueue();

            // ── FIX CRASH 2º JUGADOR ──
            // Vaciar comandos de game thread pendientes: capturan punteros a
            // chars/entidades de la sesión que muere -> si se drenan tras el
            // disconnect tocarían memoria liberada (use-after-free).
            // EN: ── 2ND PLAYER CRASH FIX ──
            //     Drain pending game-thread commands: they capture pointers to chars/entities of the dying
            //     session -> draining them after the disconnect would touch freed memory (use-after-free).
            m_commandQueue.Clear();
            // Soltar plantillas/factory cacheados: en reconexión sin recarga
            // del juego apuntarían a GameData liberado por el motor.
            // EN: Release cached templates/factory: on reconnect without reloading the game they would point to
            //     GameData already freed by the engine.
            m_spawnManager.ResetForReconnect();

            // ES: Avisar a HandleSpawnQueue de que reinicie sus temporizadores estáticos en la siguiente llamada.
            // Signal HandleSpawnQueue to reset its static timers on next call
            m_needSpawnQueueReset = true;

            // ES: Reiniciar los estáticos de sondeo de game_character para poder redescubrir animClassOffset en
            //     la siguiente conexión (s_totalAttempts, s_animClassProbed, etc.).
            // Reset game_character probe statics so animClassOffset can be
            // re-discovered on next connection (s_totalAttempts, s_animClassProbed, etc.)
            game::ResetProbeState();

            // ES: Cerrar la entrada de chat — evita que un chat invisible se coma el teclado.
            // Close chat input — prevents keyboard being consumed by invisible chat
            m_nativeHud.CloseChatInput();

            // ES: Reiniciar el auto-connect del overlay para que el usuario pueda reconectar.
            // Reset overlay auto-connect state so user can reconnect
            m_overlay.ResetForReconnect();
        }
    }
    // ES: Setters de identidad: id del jugador local, flag de host, e id del host (eres host si el id
    //     asignado por el servidor coincide con tu propio id).
    // EN: Identity setters: local player id, host flag and host id (you are host if the server-assigned
    //     host id matches your own id).
    void SetLocalPlayerId(PlayerID id) { m_localPlayerId.store(id); }
    void SetIsHost(bool host) { m_isHost = host; }
    void SetLocalHostPlayerId(PlayerID id) {
        m_hostPlayerId = id;
        m_isHost = (id != 0 && id == m_localPlayerId.load());
    }

    // ES: Se llama una vez cuando el mundo del juego ha terminado de cargar.
    // Called once when the game world has loaded
    void OnGameLoaded();

    // ES: Llamado desde el hilo de render para detectar el fin de la carga: reintenta el descubrimiento
    //     de globales, comprueba CharacterIterator y dispara OnGameLoaded cuando está listo.
    // Called from render thread to detect game load completion.
    // Retries global discovery, checks CharacterIterator, triggers OnGameLoaded if ready.
    void PollForGameLoad();

    // ES: Tick principal por frame; lo llaman los hooks del hilo de juego (TimeUpdate y, de respaldo, Present).
    // EN: Main per-frame tick; called from game-thread hooks (TimeUpdate and, as fallback, Present).
    // Called from game thread hooks
    void OnGameTick(float deltaTime);

    // ES: Tras el handshake: recorre los personajes locales existentes y los envía al servidor.
    // Called after handshake: scan existing local characters and send them to server
    void SendExistingEntitiesToServer();

    // ES: Busca los personajes del mod ("Player 1" a "Player 16") por nombre y los reclama.
    //     El slot del jugador local → entidad local. Los demás slots → disponibles para remotos.
    // Scan for mod characters ("Player 1" through "Player 16") by name and claim them.
    // Local player's slot → local entity. Other slots → available for remote players.
    void FindAndClaimModCharacters();

    // ES: Busca un personaje del mod por número de slot (devuelve el objeto del juego o nullptr).
    // Find a specific mod character by slot number (returns game object or nullptr).
    void* FindModCharacterBySlot(int slot);

    // Reclama el personaje PRIMARIO del host por la cadena directa del motor
    // (GameWorld+0x580 -> +0x2B0 playerCharacters -> data[0]), SIN depender del nombre
    // "Player N". Imprescindible en el flujo "connected-then-load", donde el char del host
    // conserva el nombre del save (no "Player N") y el hook CharacterCreate estaba en
    // passthrough durante la carga. Devuelve true si quedó reclamado/registrado.
    // EN: Claims the host PRIMARY character through the engine direct chain
    //     (GameWorld+0x580 -> +0x2B0 playerCharacters -> data[0]), WITHOUT relying on the
    //     "Player N" name. Required in the "connected-then-load" flow, where the host char keeps the
    //     save name (not "Player N") and the CharacterCreate hook was in passthrough during the load.
    //     Returns true if it ended up claimed/registered.
    bool ClaimHostPrimaryCharacter();

    // ES: Lo llama entity_hooks tras el bootstrap de facción para pedir un re-escaneo de personajes.
    // Called by entity_hooks after faction bootstrap to trigger a re-scan of existing characters
    void RequestEntityRescan() { m_needsEntityRescan.store(true); }

    // ES: Punto de spawn del host: el joiner se teletransporta aquí. Lo escribe el hilo de red y lo lee
    //     el hilo de juego — se protege con mutex para evitar lecturas a medias.
    // Host spawn point: joiner teleports here
    // Set from network thread, read from game thread — use mutex to prevent torn reads
    void SetHostSpawnPoint(const Vec3& pos) {
        std::lock_guard lock(m_hostSpawnPointMutex);
        m_hostSpawnPoint = pos;
        m_hasHostSpawnPoint.store(true, std::memory_order_release);
    }
    bool HasHostSpawnPoint() const { return m_hasHostSpawnPoint.load(std::memory_order_acquire); }
    Vec3 GetHostSpawnPoint() const {
        std::lock_guard lock(m_hostSpawnPointMutex);
        return m_hostSpawnPoint;
    }

    // ES: Teletransporta el escuadrón local al personaje remoto más cercano. Devuelve true si lo logra.
    //     Lo usa el comando de chat "/tp".
    // Teleport local player's squad to the nearest remote player character.
    // Returns true if teleport succeeded. Can be called from chat command "/tp".
    bool TeleportToNearestRemotePlayer();

    // ES: Spawn forzado: salta todos los gates y spawnea ya los remotos pendientes. Escotilla de
    //     playtest para el comando /forcespawn.
    // Force-spawn: bypass all gates and immediately spawn pending remote players.
    // Used by /forcespawn chat command as a playtest escape hatch.
    void ForceSpawnRemotePlayers();

    // ES: Lo pone time_hooks cuando el hook TimeUpdate está activo; en ese caso render_hooks no hace
    //     su llamada de respaldo a OnGameTick.
    // Set by time_hooks when TimeUpdate hook is active.
    // When true, render_hooks skips its fallback OnGameTick call.
    void SetTimeHookActive(bool active) { m_timeHookActive = active; }
    bool IsTimeHookActive() const { return m_timeHookActive; }

    // ES: Acceso al SDK (estado del juego por polling) y al proxy visual (nodos Ogre de los remotos).
    // SDK access (polling-based game state + visual proxy)
    sdk::KenshiSDK&   GetSDK()          { return m_sdk; }
    sdk::VisualProxy&  GetVisualProxy()  { return m_visualProxy; }

    // ES: Cola de comandos (marshalling thread-safe red → hilo de juego).
    // Game command queue (thread-safe network → game thread marshalling)
    GameCommandQueue& GetCommandQueue() { return m_commandQueue; }

    // ES: Acceso al orquestador de tareas (pool de workers).
    // Task orchestrator access
    TaskOrchestrator& GetOrchestrator() { return m_orchestrator; }

    // ES: Miga de pan de crash: último paso completado del pipeline (para el diagnóstico SEH/VEH).
    // Crash breadcrumb: last completed pipeline step (for SEH crash diagnostics)
    int GetLastCompletedStep() const { return m_lastCompletedStep.load(std::memory_order_relaxed); }
    void SetLastCompletedStep(int step) { m_lastCompletedStep.store(step, std::memory_order_relaxed); }

    // ES: Orquestador de sync (pipeline nuevo del motor).
    // Sync orchestrator (new engine pipeline)
    SyncOrchestrator* GetSyncOrchestrator() { return m_syncOrchestrator.get(); }

    // ES: Depurador de pipeline (estado del pipeline replicado por red).
    // Pipeline debugger (network-replicated pipeline state)
    PipelineOrchestrator& GetPipelineOrch() { return m_pipelineOrch; }

private:
    // ES: Etapas del pipeline por fases (las llama OnGameTick): aplicar posiciones remotas, leer
    //     posiciones locales, enviar paquetes, cola de spawns, teletransporte del host, lanzar trabajo
    //     de fondo y diagnóstico.
    // EN: Staged pipeline methods (called from OnGameTick): apply remote positions, poll local
    //     positions, send packets, spawn queue, host teleport, kick background work and diagnostics.
    // Staged pipeline methods (called from OnGameTick)
    void ApplyRemotePositions();
    void ApplyRemotePositionsDirect(); // Direct interpolation → game character (no double-buffer)
    void PollLocalPositions();
    void SendCachedPackets();
    void HandleSpawnQueue();
    void HandleHostTeleport();
    void KickBackgroundWork();
    void UpdateDiagnostics(float deltaTime);

    // ES: Métodos de los workers de fondo (corren en hilos del orquestador; rama legacy de doble buffer).
    // Background worker methods (run on orchestrator threads)
    void BackgroundReadEntities();
    void BackgroundInterpolate();

    // ES: Constructor/destructor privados: solo existe la instancia de Core::Get().
    // EN: Private ctor/dtor: only the Core::Get() instance exists.
    Core() = default;
    ~Core() = default;

    // ES: Fases de Initialize(): escáner de patrones, instalación de hooks, red ENet y UI.
    // EN: Initialize() phases: pattern scanner, hook installation, ENet network and UI.
    bool InitScanner();
    bool InitHooks();
    bool InitNetwork();
    bool InitUI();

    // ES: Hilo de red: bucle que llama a m_client.Update() (protegido con SEH).
    // Network thread
    void NetworkThreadFunc();

    // ES: Subsistemas propiedad de Core (ver accesores arriba).
    // EN: Subsystems owned by Core (see accessors above).
    PatternScanner       m_scanner;
    GameFunctions        m_gameFuncs;
    PatternOrchestrator  m_patternOrchestrator;
    NetworkClient   m_client;
    EntityRegistry  m_entityRegistry;
    Interpolation   m_interpolation;
    SpawnManager       m_spawnManager;
    PlayerController   m_playerController;
    LoadingOrchestrator m_loadingOrch;
    LobbyManager       m_lobbyManager;
    Overlay           m_overlay;
    NativeHud         m_nativeHud;
    ClientConfig    m_config;

    // ES: Flags atómicos de estado. m_clientPhase es la verdad sobre la fase; m_hostPlayerId lo asigna
    //     el servidor (autoritativo).
    // EN: Atomic state flags. m_clientPhase is the source of truth for the phase; m_hostPlayerId is
    //     assigned by the server (authoritative).
    std::atomic<int>  m_lastCompletedStep{-1}; // Crash breadcrumb for SEH diagnostics
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_gameLoaded{false};
    std::atomic<ClientPhase> m_clientPhase{ClientPhase::Startup};
    bool              m_isHost = false;
    PlayerID          m_hostPlayerId = 0;  // Server-authoritative host player id
    bool              m_isSteamVersion = false;
    std::atomic<bool> m_timeHookActive{false};
    std::atomic<PlayerID> m_localPlayerId{0};

    // ES: Punto de spawn del host (escribe el hilo de red, lee el hilo de juego) y flags de sesión que
    //     se reinician al reconectar / recargar.
    // Host spawn point: where joiners teleport to (network thread writes, game thread reads)
    mutable std::mutex m_hostSpawnPointMutex;
    Vec3              m_hostSpawnPoint;
    std::atomic<bool> m_hasHostSpawnPoint{false};
    bool              m_spawnTeleportDone = false;
    bool              m_initialEntityScanDone = false;
    std::atomic<bool> m_needsEntityRescan{false}; // Set by entity_hooks after faction bootstrap
    bool              m_needSpawnQueueReset = false; // HandleSpawnQueue statics reset on reconnect
    bool              m_needPollReset = false;       // PollForGameLoad statics reset on second load
    std::atomic<bool> m_forceSpawnBypass{false};     // /forcespawn command bypass

    // ES: Temporizador de teletransporte del host (miembro y no static para que se reinicie al reconectar).
    // Host teleport timer (member instead of static so it resets on reconnect)
    std::chrono::steady_clock::time_point m_hostTpTimer;
    bool              m_hostTpTimerStarted = false;

    // ES: Hilo de red (NetworkThreadFunc).
    // EN: Network thread (NetworkThreadFunc).
    std::thread m_networkThread;

    // ES: Orquestador de tareas de fondo + datos de frame con doble buffer (los workers escriben uno
    //     mientras el hilo de juego lee el otro; OnGameTick los intercambia).
    // Background task orchestrator + double-buffered frame data
    TaskOrchestrator  m_orchestrator;
    FrameData         m_frameData[2];
    std::atomic<int>  m_writeBuffer{0}; // Workers fill this
    std::atomic<int>  m_readBuffer{1};  // Game thread reads this
    bool              m_pipelineStarted = false; // True after first KickBackgroundWork

    // ES: Orquestador de sync (posee EntityResolver, ZoneEngine, PlayerEngine). m_useSyncOrchestrator
    //     es el feature flag que elige la rama nueva o la legacy en OnGameTick.
    // EN: m_useSyncOrchestrator is the feature flag that picks the new or legacy branch in OnGameTick.
    // Sync orchestrator (owns EntityResolver, ZoneEngine, PlayerEngine)
    std::unique_ptr<SyncOrchestrator> m_syncOrchestrator;
    bool              m_useSyncOrchestrator = false; // Feature flag for incremental rollout

    // ES: Depurador de pipeline (estado replicado por red).
    // Pipeline debugger (network-replicated pipeline state)
    PipelineOrchestrator m_pipelineOrch;

    // ES: SDK: abstracción del estado del juego por polling.
    // SDK: polling-based game state abstraction
    sdk::KenshiSDK    m_sdk;

    // ES: Proxy visual: renderizado con SceneNode de Ogre para los jugadores remotos.
    // Visual proxy: Ogre SceneNode rendering for remote players
    sdk::VisualProxy   m_visualProxy;

    // ES: Cola de comandos: lleva el trabajo del hilo de red al hilo de juego.
    // Game command queue: marshals network thread work to game thread
    GameCommandQueue m_commandQueue;
};

} // namespace kmp
