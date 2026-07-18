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

// Defined in core.cpp — resets keepalive timer on reconnect
void ResetKeepaliveTimer();

// Defined in core.cpp — re-arma el fix de facción del host en disconnect / nueva carga
void ResetHostFactionFix();

// Defined in core.cpp — purga los cachés de Nameless (s_cachedNamelessFaction/FR,
// s_namelessResolveState) en disconnect / nueva carga. Sin esto, tras recargar un save el motor
// libera/recrea las facciones y esos punteros quedan colgando — los hooks isAlly/isEnemy seguirían
// "sustituyendo por Nameless" contra memoria reciclada (hallazgo de la revisión de interacciones,
// 2026-07-13). ResolveNamelessFactionOnce es idempotente y se re-arma solo tras esto.
void ResetNamelessResolve();

// Defined in core.cpp — re-arma el [FIX-HOSTREL]: pasada one-shot que reescribe a -100 las
// relaciones de facción CORRUPTAS del host (37 entries explícitas que deberían ser hostiles pero
// quedaron en neutral 0.00/-10, heredadas de un clon corrupto, y que ANULAN el defaultRelation=-100
// → ningún NPC contraataca al host, "todos huyen"). Se re-arma en disconnect / nueva carga por si el
// motor recarga las relaciones al cargar un save (hallazgo RE en vivo, 2026-07-13).
void ResetHostFactionRelationsFix();

// Defined in core.cpp — re-arma el SEED de char+0xD0 (lastProcessed) del host
// (Fase 4: desbloqueo del AI tick de combate) en disconnect / nueva carga.
void ResetHostSimSeedFix();

// Defined in core.cpp — re-arma el SEED de char+0x4C0 (timestamp medico) del host
// (FIX-MEDSEED: desbloquea el consumo de comida/necesidades evitando dt<0 en
// MedicalSystem::periodicUpdate 0x64DA70) en disconnect / nueva carga.
void ResetHostMedSeedFix();

// Defined in core.cpp — re-arma el FIX-CONTROL (SetControlledChar 0x802520: vincula la
// facción del host como "controlada por el jugador", faction+0x250=PI → el gate de la rama
// viva 0x5CD1E3 exime al char del host del umbral 0.75 → su "think" de combate/cura corre
// siempre). CAUSA 2 del combate congelado. Se re-arma en disconnect / nueva carga.
void ResetHostControlledCharFix();

// Defined in core.cpp — re-arma el [FIX-COMBATCLASS] (CharBody::create 0x621460: crea la
// CombatClass = *(CharBody+0x8) del host si nació NULL porque el spawn del mod saltó
// giveBirth). CAUSA RAÍZ del combate (el char camina pero no ataca). Detrás de toggle
// kEnableCombatClassFix (OFF por defecto). Se re-arma en disconnect / nueva carga.
void ResetHostCombatClassFix();

// Defined in core.cpp — re-arma el [FIX-COMBATARM] (arranca la máquina de estados de combate del
// host llamando a CombatClass::startupState() [vtable slot +0x50] una vez, para que nextMove
// (CC+0x1F4) / combatState (CC+0x1F0) dejen de contener basura sin inicializar y el host pueda
// iniciar combate "en frío" sin usar antes un muñeco de entrenamiento). Incluye red de seguridad
// (saneado defensivo de nextMove/combatState). Se re-arma en disconnect / nueva carga.
void ResetHostCombatArmFix();

// Defined in core.cpp — re-arma el [FIX-ARMHAND] (repara el handle de brazo del AI del host:
// AI+0x318 = Character+0x458, escritura de puntero de 8 bytes a una región que ya existe dentro
// del propio Character). Sin esto el chequeo GOAP dice "brazo en estado pésimo" aunque los brazos
// estén sanos → el host no puede cargar/secuestrar, hacer primeros auxilios ni autocurarse.
// Se re-arma en disconnect / nueva carga.
void ResetHostArmHandFix();

// Defined in core.cpp — re-arma el [AUTOTEST] de combate (dispara attackTarget 0x5CB0A0 sobre
// el host contra un NPC enemigo cercano, una vez por carga) en disconnect / nueva carga.
void ResetHostCombatAutotest();

// Defined in core.cpp — re-arma el [FIX-PLATOON] (setActivePlatoon 0x6213F0: re-enlaza el
// ActivePlatoon del host (char+0x658) registrando AI+0x10=platoon->me, que el spawn del clon
// del mod omite → su Tasker queda huérfano y el AI tick no consume las órdenes de combate).
// CAUSA RAÍZ Fase 4 (orden encolada pero amIdle=1). Se re-arma en disconnect / nueva carga.
void ResetHostPlatoonFix();

// Defined in core.cpp — re-arma el [DIAG-CLONESQUAD] (detección del CharacterHuman DUPLICADO
// en el squad del host: mismo char+0x40 template + char+0x0C==1 + mismo ActivePlatoon que el
// char real → clon que reencola GET_OUT_OF_BED sin fin, causa raíz del bug de la cama, wiki
// secc. 21). Solo diagnóstico (no despawnea: no hay función nativa segura). Se re-arma en
// disconnect / nueva carga.
void ResetHostSquadCloneDetect();

// Defined in core.cpp — re-arma el [FIX-CLONESQUAD-DESPAWN] (despawn REAL del clon del squad
// del host: erase de la lektor PlayerInterface + GameWorld::removeObject 0x799AF0). Gating
// anti-falso-positivo por estabilidad (≥3 ticks con el mismo clon). Acción DESTRUCTIVA y
// one-shot; se re-arma en disconnect / nueva carga (junto a ResetHostSquadCloneDetect).
void ResetHostSquadCloneDespawn();

class Core {
public:
    static Core& Get();

    bool Initialize();
    void Shutdown();

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

    bool IsConnected() const { return m_connected; }
    bool IsHost() const { return m_isHost; }
    PlayerID GetHostPlayerId() const { return m_hostPlayerId; }
    bool IsSteamVersion() const { return m_isSteamVersion; }
    bool IsGameLoaded() const { return m_gameLoaded; }
    bool IsLoading() const { return m_clientPhase == ClientPhase::Loading; }
    void SetLoading(bool) {} // No-op — phase transitions handled by state machine

    // ── Client phase state machine ──
    ClientPhase GetClientPhase() const { return m_clientPhase.load(std::memory_order_acquire); }
    void TransitionTo(ClientPhase newPhase);

    // Called by HookPresent when a long frame gap (>2s) is detected,
    // indicating the game was blocking on a load screen.
    void OnLoadingGapDetected();
    PlayerID GetLocalPlayerId() const { return m_localPlayerId.load(); }

    void SetConnected(bool connected) {
        m_connected = connected;
        if (connected) {
            // Phase transition handled by PacketHandler (TransitionTo(Connected))
            ResetKeepaliveTimer(); // Reset so first keepalive fires 5s after connect, not immediately

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
                HookManager::Get().Enable("PushOrder");
                // [DIAG-COMBATSEED] Activa el hook de CombatClass::update 0x60D650 (AI tick de
                // combate) para capturar el estado del CombatClass del host (frío vs tras entrenar).
                HookManager::Get().Enable("CombatClassUpdate");
                spdlog::info("Core: Combat hooks ENABLED (game loaded, incl. PushOrder + CombatSeed DIAG)");
            } else {
                spdlog::info("Core: Combat hooks DEFERRED (game not loaded — will enable on load)");
            }

            // shared_save_sync::Init() is called lazily from Update() after
            // faction assignment arrives (which comes AFTER handshake ack).
        }
        if (!connected) {
            // Drop back to GameReady so user can reconnect
            if (m_gameLoaded) {
                TransitionTo(ClientPhase::GameReady);
            }

            // Disable CharacterCreate hook — zone-load bursts while disconnected
            // would go through MovRaxRsp and corrupt the heap
            entity_hooks::SuspendForDisconnect();

            // Disable combat hooks — no need to sync while disconnected
            HookManager::Get().Disable("CharacterDeath");
            HookManager::Get().Disable("CharacterKO");

            // Clean up remote entities, interpolation, and visual proxies
            size_t removed = m_entityRegistry.ClearRemoteEntities();
            m_interpolation.Clear();
            m_visualProxy.DestroyAll();
            m_playerController.Reset();
            if (removed > 0) {
                spdlog::info("Core: Cleared {} remote entities on disconnect", removed);
            }

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

            // Reset sync orchestrator state
            if (m_syncOrchestrator) {
                m_syncOrchestrator->Reset();
            }

            // Reset pipeline debugger state
            m_pipelineOrch.Shutdown();

            // Reset shared-save sync
            shared_save_sync::Reset();

            // Cambio 2.4 / 3.2: purgar cachés de personajes al desconectar. Los Character de la
            // sesión anterior serán liberados por el motor → dejar entradas colgadas provocaría
            // UAF (tracker) o suprimir la IA de NPCs locales nuevos (remote-controlled).
            char_tracker_hooks::Clear();
            ai_hooks::ClearRemoteControlled();

            // Clear stale spawn queue from previous session
            m_spawnManager.ClearSpawnQueue();

            // ── FIX CRASH 2º JUGADOR ──
            // Vaciar comandos de game thread pendientes: capturan punteros a
            // chars/entidades de la sesión que muere -> si se drenan tras el
            // disconnect tocarían memoria liberada (use-after-free).
            m_commandQueue.Clear();
            // Soltar plantillas/factory cacheados: en reconexión sin recarga
            // del juego apuntarían a GameData liberado por el motor.
            m_spawnManager.ResetForReconnect();

            // Signal HandleSpawnQueue to reset its static timers on next call
            m_needSpawnQueueReset = true;

            // Reset game_character probe statics so animClassOffset can be
            // re-discovered on next connection (s_totalAttempts, s_animClassProbed, etc.)
            game::ResetProbeState();

            // Close chat input — prevents keyboard being consumed by invisible chat
            m_nativeHud.CloseChatInput();

            // Reset overlay auto-connect state so user can reconnect
            m_overlay.ResetForReconnect();
        }
    }
    void SetLocalPlayerId(PlayerID id) { m_localPlayerId.store(id); }
    void SetIsHost(bool host) { m_isHost = host; }
    void SetLocalHostPlayerId(PlayerID id) {
        m_hostPlayerId = id;
        m_isHost = (id != 0 && id == m_localPlayerId.load());
    }

    // Called once when the game world has loaded
    void OnGameLoaded();

    // Called from render thread to detect game load completion.
    // Retries global discovery, checks CharacterIterator, triggers OnGameLoaded if ready.
    void PollForGameLoad();

    // Called from game thread hooks
    void OnGameTick(float deltaTime);

    // Called after handshake: scan existing local characters and send them to server
    void SendExistingEntitiesToServer();

    // Scan for mod characters ("Player 1" through "Player 16") by name and claim them.
    // Local player's slot → local entity. Other slots → available for remote players.
    void FindAndClaimModCharacters();

    // Find a specific mod character by slot number (returns game object or nullptr).
    void* FindModCharacterBySlot(int slot);

    // Reclama el personaje PRIMARIO del host por la cadena directa del motor
    // (GameWorld+0x580 -> +0x2B0 playerCharacters -> data[0]), SIN depender del nombre
    // "Player N". Imprescindible en el flujo "connected-then-load", donde el char del host
    // conserva el nombre del save (no "Player N") y el hook CharacterCreate estaba en
    // passthrough durante la carga. Devuelve true si quedó reclamado/registrado.
    bool ClaimHostPrimaryCharacter();

    // Called by entity_hooks after faction bootstrap to trigger a re-scan of existing characters
    void RequestEntityRescan() { m_needsEntityRescan.store(true); }

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

    // Teleport local player's squad to the nearest remote player character.
    // Returns true if teleport succeeded. Can be called from chat command "/tp".
    bool TeleportToNearestRemotePlayer();

    // Force-spawn: bypass all gates and immediately spawn pending remote players.
    // Used by /forcespawn chat command as a playtest escape hatch.
    void ForceSpawnRemotePlayers();

    // Set by time_hooks when TimeUpdate hook is active.
    // When true, render_hooks skips its fallback OnGameTick call.
    void SetTimeHookActive(bool active) { m_timeHookActive = active; }
    bool IsTimeHookActive() const { return m_timeHookActive; }

    // SDK access (polling-based game state + visual proxy)
    sdk::KenshiSDK&   GetSDK()          { return m_sdk; }
    sdk::VisualProxy&  GetVisualProxy()  { return m_visualProxy; }

    // Game command queue (thread-safe network → game thread marshalling)
    GameCommandQueue& GetCommandQueue() { return m_commandQueue; }

    // Task orchestrator access
    TaskOrchestrator& GetOrchestrator() { return m_orchestrator; }

    // Crash breadcrumb: last completed pipeline step (for SEH crash diagnostics)
    int GetLastCompletedStep() const { return m_lastCompletedStep.load(std::memory_order_relaxed); }
    void SetLastCompletedStep(int step) { m_lastCompletedStep.store(step, std::memory_order_relaxed); }

    // Sync orchestrator (new engine pipeline)
    SyncOrchestrator* GetSyncOrchestrator() { return m_syncOrchestrator.get(); }

    // Pipeline debugger (network-replicated pipeline state)
    PipelineOrchestrator& GetPipelineOrch() { return m_pipelineOrch; }

private:
    // Staged pipeline methods (called from OnGameTick)
    void ApplyRemotePositions();
    void ApplyRemotePositionsDirect(); // Direct interpolation → game character (no double-buffer)
    void PollLocalPositions();
    void SendCachedPackets();
    void HandleSpawnQueue();
    void HandleHostTeleport();
    void KickBackgroundWork();
    void UpdateDiagnostics(float deltaTime);

    // Background worker methods (run on orchestrator threads)
    void BackgroundReadEntities();
    void BackgroundInterpolate();

    Core() = default;
    ~Core() = default;

    bool InitScanner();
    bool InitHooks();
    bool InitNetwork();
    bool InitUI();

    // Network thread
    void NetworkThreadFunc();

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

    // Host teleport timer (member instead of static so it resets on reconnect)
    std::chrono::steady_clock::time_point m_hostTpTimer;
    bool              m_hostTpTimerStarted = false;

    std::thread m_networkThread;

    // Background task orchestrator + double-buffered frame data
    TaskOrchestrator  m_orchestrator;
    FrameData         m_frameData[2];
    std::atomic<int>  m_writeBuffer{0}; // Workers fill this
    std::atomic<int>  m_readBuffer{1};  // Game thread reads this
    bool              m_pipelineStarted = false; // True after first KickBackgroundWork

    // Sync orchestrator (owns EntityResolver, ZoneEngine, PlayerEngine)
    std::unique_ptr<SyncOrchestrator> m_syncOrchestrator;
    bool              m_useSyncOrchestrator = false; // Feature flag for incremental rollout

    // Pipeline debugger (network-replicated pipeline state)
    PipelineOrchestrator m_pipelineOrch;

    // SDK: polling-based game state abstraction
    sdk::KenshiSDK    m_sdk;

    // Visual proxy: Ogre SceneNode rendering for remote players
    sdk::VisualProxy   m_visualProxy;

    // Game command queue: marshals network thread work to game thread
    GameCommandQueue m_commandQueue;
};

} // namespace kmp
