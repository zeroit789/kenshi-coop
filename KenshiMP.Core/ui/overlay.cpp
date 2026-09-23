// ES: Implementación del Overlay (controlador de conexión): carga de la config en el
//     primer frame, sondeo de partida cargada, auto-conexión, reintentos, handshake,
//     limpieza al desconectar y refresco del navegador de servidores.
// EN: Overlay (connection controller) implementation: config load on the first frame,
//     game-load polling, auto-connect, retries, handshake, cleanup on disconnect and
//     server browser refresh.
#include "overlay.h"
#include "mygui_bridge.h"
#include "../core.h"
#include "../hooks/ai_hooks.h"
#include "../hooks/entity_hooks.h"
#include "../game/game_types.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/constants.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <string>
#include <algorithm>
#include <Windows.h>

namespace kmp {

// ES: Update() — se llama cada frame desde el hook de Present (hilo de render). Gestiona:
//     carga de config, detección de partida cargada, auto-conexión, estado de conexión,
//     reintentos y desconexión.
// EN:
// ── Update() — called every frame from Present hook ──
// Handles: config load, game load detection, auto-connect, connection state, retry, disconnect.
void Overlay::Update() {
    // ES: Cargar la config en los campos en la primera llamada. La auto-conexión se deja
    //     desactivada a propósito (se conecta a mano con /connect).
    // EN: (Auto-connect is deliberately left disabled; connect manually with /connect.)
    // Load config into UI fields on first call
    if (m_firstFrame) {
        m_firstFrame = false;
        auto& config = Core::Get().GetConfig();
        strncpy(m_playerName, config.playerName.c_str(), sizeof(m_playerName) - 1);
        strncpy(m_settingsName, config.playerName.c_str(), sizeof(m_settingsName) - 1);
        strncpy(m_serverAddress, config.lastServer.c_str(), sizeof(m_serverAddress) - 1);
        snprintf(m_serverPort, sizeof(m_serverPort), "%d", config.lastPort);
        m_settingsAutoConnect = false; // Auto-connect disabled — use /connect manually
        m_autoConnectPending = false;
        OutputDebugStringA("KMP: Overlay::Update() — first frame config loaded\n");
    }

    // ES: Detección de partida cargada guiada por fases: PollForGameLoad solo corre en la
    //     fase Loading (la activa HookPresent al ver un hueco de >2 s entre frames = el
    //     juego estaba bloqueado cargando). Así se evitan falsos positivos en el menú.
    // ── Phase-driven game-load detection ──
    // PollForGameLoad only runs during the Loading phase (set by HookPresent
    // when it detects a >2s gap between frames = game was blocking on load).
    // This prevents false game-load detection on the main menu.
    auto& coreRef = Core::Get();
    if (coreRef.GetClientPhase() == ClientPhase::Loading) {
        auto now = std::chrono::steady_clock::now();

        // ES: Sondear cada 2 s mientras dure la fase Loading.
        // Poll every 2 seconds while in Loading phase
        if (!m_playerBaseCheckedOnce ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPlayerBaseCheck).count() > 2000) {
            m_playerBaseCheckedOnce = true;
            m_lastPlayerBaseCheck = now;
            m_playerBasePollCount++;

            // ES: PollForGameLoad reintenta el descubrimiento global, mira el CharacterIterator
            //     y llama a OnGameLoaded() si encuentra personajes.
            // PollForGameLoad: retries global discovery, checks CharacterIterator,
            // and calls OnGameLoaded() if characters are found.
            coreRef.PollForGameLoad();
        }
    }

    bool gameLoaded = coreRef.IsGameLoaded();

    // ES: Auto-conexión al cargar la partida (2 s de margen para que termine la carga). El
    //     hook CharacterCreate está desactivado durante la carga (para evitar corrupción del
    //     heap por la ráfaga de 130+ creaciones), así que GetTotalCreates() se queda en 0;
    //     por eso se usa IsGameLoaded(), que se activa en la transición Loading -> GameReady.
    // ── Auto-connect on game load (with delay for loading to finish) ──
    // The CharacterCreate hook is disabled during loading (to prevent heap corruption
    // from the 130+ creation burst), so GetTotalCreates() stays at 0.
    // Instead, rely on IsGameLoaded() which is set after the Loading -> GameReady
    // phase transition (loading gap detection + smooth frames + CharacterIterator).
    if (gameLoaded && m_autoConnectPending && !m_autoConnectDone && !m_connecting) {
        if (!m_gameLoadedTimerStarted) {
            m_gameLoadedTime = std::chrono::steady_clock::now();
            m_gameLoadedTimerStarted = true;
            OutputDebugStringA("KMP: Overlay — game loaded, waiting 2s before auto-connect...\n");
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_gameLoadedTime);
        if (elapsed.count() >= 2) {
            m_autoConnectDone = true;
            m_autoConnectPending = false;
            auto& core = Core::Get();
            uint16_t port = static_cast<uint16_t>(std::atoi(m_serverPort));

            OutputDebugStringA("KMP: Overlay — AUTO-CONNECT triggered (2s after game load)!\n");
            spdlog::info("Overlay: Auto-connecting to {}:{} (2s after game loaded)", m_serverAddress, port);

            if (core.GetClient().ConnectAsync(m_serverAddress, port)) {
                m_connecting = true;
                core.TransitionTo(ClientPhase::Connecting);
                m_nativeMenu.Hide();
                core.GetNativeHud().LogStep("NET", "Auto-connecting to " + std::string(m_serverAddress) + ":" + std::string(m_serverPort));
                core.GetNativeHud().AddSystemMessage("Auto-connecting to " + std::string(m_serverAddress) + ":" + std::string(m_serverPort) + "...");
                OutputDebugStringA("KMP: Overlay — ConnectAsync started\n");
            } else {
                core.GetNativeHud().LogStep("ERR", "Auto-connect failed");
                core.GetNativeHud().AddSystemMessage("Auto-connect failed. Press F1 to retry.");
                OutputDebugStringA("KMP: Overlay — ConnectAsync FAILED\n");
            }
        }
    }

    // ES: Reintento de conexión cada 5 s (el mapeo de puertos UPnP puede tardar 3-10 s).
    // ── Connection retry (UPnP port mapping can take 3-10s) ──
    if (m_retryPending && !m_connecting) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_retryTime);
        if (elapsed.count() >= 5) {
            m_retryPending = false;
            m_connectAttempt++;
            auto& core = Core::Get();
            uint16_t port = static_cast<uint16_t>(std::atoi(m_serverPort));

            core.GetNativeHud().LogStep("NET", "Retry attempt " + std::to_string(m_connectAttempt)
                                 + "/" + std::to_string(m_maxConnectAttempts));
            core.GetNativeHud().AddSystemMessage(
                "Retrying connection (attempt " + std::to_string(m_connectAttempt)
                + "/" + std::to_string(m_maxConnectAttempts) + ")...");

            if (core.GetClient().ConnectAsync(m_serverAddress, port)) {
                m_connecting = true;
                core.TransitionTo(ClientPhase::Connecting);
            } else {
                m_connectAttempt = m_maxConnectAttempts;
                core.GetNativeHud().LogStep("ERR", "ConnectAsync failed on retry");
            }
        }
    }

    // ES: Comprobar el resultado de la conexión asíncrona: si conecta, enviar el handshake
    //     (versión de protocolo + nombre) y guardar los datos en la config; si falla,
    //     programar un reintento o, agotados, volver a GameReady / reabrir el menú.
    // EN: Check the async connect result: on success send the handshake (protocol version
    //     + name) and store the data in config; on failure schedule a retry or, once
    //     exhausted, go back to GameReady / reopen the menu.
    // ── Check async connect result ──
    if (m_connecting) {
        auto& core = Core::Get();
        if (core.GetClient().IsConnected()) {
            m_connecting = false;
            m_connectAttempt = 0;
            m_retryPending = false;
            OutputDebugStringA("KMP: Overlay — connection ESTABLISHED, sending handshake\n");
            spdlog::info("Overlay: Connection established, sending handshake as '{}'", m_playerName);

            MsgHandshake hs{};
            hs.protocolVersion = KMP_PROTOCOL_VERSION;
            strncpy(hs.playerName, m_playerName, KMP_MAX_NAME_LENGTH);
            hs.playerName[KMP_MAX_NAME_LENGTH] = '\0';

            PacketWriter writer;
            writer.WriteHeader(MessageType::C2S_Handshake);
            writer.WriteRaw(&hs, sizeof(hs));
            core.GetClient().SendReliable(writer.Data(), writer.Size());

            core.GetConfig().playerName = m_playerName;
            core.GetConfig().lastServer = m_serverAddress;
            core.GetConfig().lastPort = static_cast<uint16_t>(std::atoi(m_serverPort));

            AddSystemMessage("Connected to server!");
            core.GetNativeHud().LogStep("NET", "TCP connected, handshake sent");
            core.GetNativeHud().AddSystemMessage("Connected! Handshake sent...");
        } else if (!core.GetClient().IsConnecting()) {
            m_connecting = false;
            OutputDebugStringA("KMP: Overlay — connection attempt failed\n");

            // ES: Reintentar si no se ha superado el máximo de intentos.
            // Retry if we haven't exceeded max attempts
            if (m_connectAttempt < m_maxConnectAttempts) {
                m_retryPending = true;
                m_retryTime = std::chrono::steady_clock::now();
                int remaining = m_maxConnectAttempts - m_connectAttempt;
                core.GetNativeHud().LogStep("NET", "Connection failed, retrying in 5s (UPnP may still be mapping)...");
                core.GetNativeHud().AddSystemMessage(
                    "Connection failed. Retrying in 5 seconds (" + std::to_string(remaining) + " attempts left)...");
                spdlog::info("Overlay: Connection failed, will retry ({}/{})",
                             m_connectAttempt + 1, m_maxConnectAttempts);
            } else {
                // ES: Reintentos agotados: volver a GameReady.
                // All retries exhausted — drop back to GameReady
                AddSystemMessage("Connection failed after all retry attempts.");
                core.GetNativeHud().LogStep("ERR", "Connection FAILED after " + std::to_string(m_maxConnectAttempts) + " attempts");
                core.GetNativeHud().AddSystemMessage("Connection failed after " + std::to_string(m_maxConnectAttempts) + " attempts.");
                OutputDebugStringA("KMP: Overlay — all connection retries exhausted\n");
                m_connectAttempt = 0;
                if (gameLoaded) {
                    core.TransitionTo(ClientPhase::GameReady);
                }
                if (!gameLoaded) {
                    if (m_nativeMenu.IsInitialized()) {
                        m_nativeMenu.Show();
                        m_nativeMenu.SetStatus("Connection failed. Check server address or try again.");
                    }
                }
            }
        }
    }

    // ES: Detección de desconexión (Core cree que está conectado pero el cliente ENet no).
    // EN: Disconnect detection (Core thinks it is connected but the ENet client does not).
    // ── Disconnect detection ──
    {
        auto& core = Core::Get();
        if (core.IsConnected() && !core.GetClient().IsConnected()) {
            spdlog::warn("Overlay: Server connection lost — resetting state");
            OutputDebugStringA("KMP: Overlay — DISCONNECTED from server\n");

            // ES: PRIMERO mandar las entidades remotas bajo tierra ANTES de limpiar el registro:
            //     SetConnected(false) llama a ClearRemoteEntities, que borra los punteros a los
            //     objetos del juego, así que hay que moverlos mientras el registro aún es válido.
            // FIRST: teleport all remote entities underground BEFORE clearing the registry.
            // SetConnected(false) calls ClearRemoteEntities which wipes game object pointers,
            // so we must teleport them while the registry still has valid data.
            auto& registry = core.GetEntityRegistry();
            auto remoteEntities = registry.GetRemoteEntities();
            for (EntityID eid : remoteEntities) {
                void* gameObj = registry.GetGameObject(eid);
                if (gameObj) {
                    // ES: Quitar isPlayerControlled para que los remotos salgan del panel de
                    //     escuadra (si no, el host podría seguir seleccionándolos/controlándolos).
                    // Clear isPlayerControlled so remote characters leave the squad panel.
                    // Without this, the host can still select/control departed characters.
                    game::WritePlayerControlled(reinterpret_cast<uintptr_t>(gameObj), false);
                    game::CharacterAccessor accessor(gameObj);
                    Vec3 underground(0.f, -10000.f, 0.f);
                    accessor.WritePosition(underground);
                    // ES: Quitar el seguimiento de control remoto de la IA (evita punteros obsoletos).
                    // Clear AI remote-control tracking to prevent stale pointer issues
                    ai_hooks::UnmarkRemoteControlled(gameObj);
                }
            }
            size_t teleported = remoteEntities.size();

            // ES: DESPUÉS, limpieza completa (registro, interpolación, controlador de jugador, estado).
            // THEN: full cleanup (clears registry, interpolation, player controller, resets state)
            core.SetConnected(false);
            m_connecting = false;

            AddSystemMessage("Disconnected from server.");
            core.GetNativeHud().LogStep("WARN", "Disconnected from server!");
            core.GetNativeHud().AddSystemMessage("Disconnected from server.");
            if (teleported > 0) {
                spdlog::info("Overlay: Teleported {} remote entities underground on disconnect", teleported);
                core.GetNativeHud().AddSystemMessage("Cleaned up " + std::to_string(teleported) + " remote entities.");
            }
            // ES: Nota: SetConnected(false) ya llama a ResetForReconnect.
            // Note: ResetForReconnect already called by SetConnected(false)
        }
    }

    // ES: Procesar el cliente de consulta de servidores.
    // ── Pump server query client ──
    m_queryClient.Update();

    // ES: Refrescar las filas del navegador si el menú está visible (cada 30 frames).
    // ── Update server browser display if visible (throttled to every 30 frames) ──
    m_browserFrameCounter++;
    if (m_nativeMenu.IsVisible() && m_browserFrameCounter % 30 == 0) {
        auto results = m_queryClient.GetResults();
        if (!results.empty()) {
            auto& bridge = MyGuiBridge::Get();
            size_t maxRows = static_cast<size_t>(NativeMenu::MAX_SERVER_ROWS);
            for (size_t i = 0; i < results.size() && i < maxRows; i++) {
                auto& r = results[i];
                std::string nameStr = r.serverName.empty() ? r.address : r.serverName;
                std::string infoStr;
                if (r.pending) {
                    infoStr = "...";
                } else if (r.online) {
                    infoStr = std::to_string(r.currentPlayers) + "/" +
                              std::to_string(r.maxPlayers) + " " +
                              std::to_string(r.ping) + "ms";
                } else {
                    infoStr = "OFFLINE";
                }
                void* nameWidget = m_nativeMenu.GetServerNameWidget(static_cast<int>(i));
                void* infoWidget = m_nativeMenu.GetServerInfoWidget(static_cast<int>(i));
                if (nameWidget) bridge.SetCaption(nameWidget, nameStr);
                if (infoWidget) bridge.SetCaption(infoWidget, infoStr);
            }
            // ES: Vaciar las filas sobrantes.
            // Clear unused rows
            for (size_t i = results.size(); i < maxRows; i++) {
                void* nameWidget = m_nativeMenu.GetServerNameWidget(static_cast<int>(i));
                void* infoWidget = m_nativeMenu.GetServerInfoWidget(static_cast<int>(i));
                if (nameWidget) bridge.SetCaption(nameWidget, "");
                if (infoWidget) bridge.SetCaption(infoWidget, "");
            }
        }
    }
}

// ES: Cierra menú y cliente de consulta y vuelca nombre y auto-conexión a la config en
//     memoria. Ojo: m_settingsAutoConnect se fuerza a false en el primer frame, así que
//     aquí siempre escribe autoConnect=false.
// EN: Closes the menu and query client and writes name and auto-connect back to the
//     in-memory config. Note: m_settingsAutoConnect is forced to false on the first frame,
//     so this always writes autoConnect=false.
void Overlay::Shutdown() {
    m_nativeMenu.Shutdown();
    m_queryClient.Shutdown();

    auto& core = Core::Get();
    core.GetConfig().playerName = m_playerName;
    core.GetConfig().autoConnect = m_settingsAutoConnect;
}

// ES: Resetea el estado de conexión tras una desconexión; si la auto-conexión está activa,
//     la re-programa para reconectar.
// EN: Resets connection state after a disconnect; if auto-connect is enabled, it re-queues
//     it to reconnect.
void Overlay::ResetForReconnect() {
    // ES: Resetear el estado de conexión para reconectar limpio.
    // Reset connection state for clean reconnect
    m_autoConnectDone = false;
    m_gameLoadedTimerStarted = false;
    m_connectAttempt = 0;
    m_retryPending = false;

    // ES: Si la auto-conexión está activa, reintentar automáticamente.
    // If auto-connect is enabled, automatically try to reconnect
    if (m_settingsAutoConnect) {
        m_autoConnectPending = true;
        spdlog::info("Overlay: Auto-reconnect enabled — will retry in 2 seconds");
        Core::Get().GetNativeHud().AddSystemMessage("Auto-reconnecting in 2 seconds...");
    } else {
        m_autoConnectPending = false;
    }
}

// ES: Auxiliares con los que NativeMenu controla el estado de conexión del overlay.
// ═══════════════════════════════════════════════════════════════
//  Helpers for NativeMenu to drive overlay connection state
// ═══════════════════════════════════════════════════════════════

// ES: Programa una auto-conexión a ip:puerto para cuando se cargue la partida.
// EN: Queues an auto-connect to ip:port for when the game is loaded.
void Overlay::SetAutoConnect(const std::string& ip, uint16_t port) {
    strncpy(m_serverAddress, ip.c_str(), sizeof(m_serverAddress) - 1);
    snprintf(m_serverPort, sizeof(m_serverPort), "%d", port);
    m_autoConnectPending = true;
    m_autoConnectDone = false;
    m_gameLoadedTimerStarted = false;
}

// ES: Guarda ip/puerto/nombre para la conexión en curso y resetea los reintentos.
// EN: Stores ip/port/name for the current connection and resets retries.
void Overlay::SetConnectionInfo(const std::string& ip, uint16_t port, const std::string& name) {
    strncpy(m_serverAddress, ip.c_str(), sizeof(m_serverAddress) - 1);
    snprintf(m_serverPort, sizeof(m_serverPort), "%d", port);
    strncpy(m_playerName, name.c_str(), sizeof(m_playerName) - 1);
    // ES: Resetear reintentos para un intento de conexión nuevo.
    // Reset retry state for fresh connection attempt
    m_connectAttempt = 0;
    m_retryPending = false;
}

// ES: Cambia el nombre del jugador (y el de ajustes).
// EN: Changes the player name (and the settings one).
void Overlay::SetPlayerName(const std::string& name) {
    strncpy(m_playerName, name.c_str(), sizeof(m_playerName) - 1);
    strncpy(m_settingsName, name.c_str(), sizeof(m_settingsName) - 1);
}

// ES: GESTIÓN DE DATOS (chat y lista de jugadores, protegidos por m_mutex).
// ═══════════════════════════════════════════════════════════════
//  DATA MANAGEMENT
// ═══════════════════════════════════════════════════════════════

// ES: Añade un mensaje de chat resolviendo el nombre del emisor por su ID.
// EN: Adds a chat message, resolving the sender name from its ID.
void Overlay::AddChatMessage(PlayerID sender, const std::string& message) {
    std::lock_guard lock(m_mutex);
    ChatEntry entry;
    entry.sender = sender;
    entry.message = message;
    entry.isSystem = false;
    entry.timestamp = m_uptime;

    for (auto& p : m_players) {
        if (p.id == sender) { entry.senderName = p.name; break; }
    }
    if (entry.senderName.empty()) entry.senderName = "Player " + std::to_string(sender);

    m_chatHistory.push_back(entry);
    while (m_chatHistory.size() > MAX_CHAT_HISTORY) m_chatHistory.pop_front();
    m_chatScrollToBottom = true;
}

// ES: Añade un mensaje de sistema.
// EN: Adds a system message.
void Overlay::AddSystemMessage(const std::string& message) {
    std::lock_guard lock(m_mutex);
    ChatEntry entry;
    entry.sender = 0;
    entry.senderName = "System";
    entry.message = message;
    entry.isSystem = true;
    entry.timestamp = m_uptime;

    m_chatHistory.push_back(entry);
    while (m_chatHistory.size() > MAX_CHAT_HISTORY) m_chatHistory.pop_front();
    m_chatScrollToBottom = true;
}

// ES: Añade (o actualiza si ya existe) un jugador.
// EN: Adds (or updates if already present) a player.
void Overlay::AddPlayer(const PlayerInfo& player) {
    std::lock_guard lock(m_mutex);
    for (auto& p : m_players) {
        if (p.id == player.id) { p = player; return; }
    }
    m_players.push_back(player);
}

// ES: Quita un jugador por ID.
// EN: Removes a player by ID.
void Overlay::RemovePlayer(PlayerID id) {
    std::lock_guard lock(m_mutex);
    m_players.erase(std::remove_if(m_players.begin(), m_players.end(),
        [id](const PlayerInfo& p) { return p.id == id; }), m_players.end());
}

// ES: Actualiza el ping de un jugador.
// EN: Updates a player's ping.
void Overlay::UpdatePlayerPing(PlayerID id, uint32_t ping) {
    std::lock_guard lock(m_mutex);
    for (auto& p : m_players) {
        if (p.id == id) { p.ping = ping; return; }
    }
}

} // namespace kmp
