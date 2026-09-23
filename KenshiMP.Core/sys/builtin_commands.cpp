// ES: Comandos integrados de chat/consola ('/help', '/tp', '/connect', '/sync',
//     comandos de host y un gran bloque de herramientas de depuración e
//     ingeniería inversa: volcado de offsets, lectura de memoria, estado de
//     hooks, inspección de personajes/escuadras, diagnóstico de spawn, etc.).
//     Todo se registra en CommandRegistry::RegisterBuiltins(), que se llama una
//     vez desde Core::Initialize. Cada handler devuelve el texto que se muestra
//     en el chat (cadena vacía = sin mensaje). Se ejecutan en el hilo que procesa
//     la entrada del chat (normalmente el hilo del juego).
// EN: Built-in chat/console commands ('/help', '/tp', '/connect', '/sync',
//     host commands and a large block of debugging and reverse-engineering
//     tools: offset dumps, memory reads, hook status, character/squad
//     inspection, spawn diagnostics, etc.). Everything is registered in
//     CommandRegistry::RegisterBuiltins(), called once from Core::Initialize.
//     Each handler returns the text shown in chat (empty string = no message).
//     They run on the thread that processes chat input (normally the game thread).
#include "command_registry.h"
#include "../core.h"
#include "../game/game_types.h"
#include "../game/spawn_manager.h"
#include "../game/player_controller.h"
#include "../game/asset_facilitator.h"
#include "../game/loading_orchestrator.h"
#include "../hooks/time_hooks.h"
#include "../hooks/entity_hooks.h"
#include "../hooks/ai_hooks.h"
#include "../hooks/char_tracker_hooks.h"
#include "../game/shared_save_sync.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/constants.h"
#include "kmp/memory.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace kmp {

// ES: Registra todos los comandos integrados en el registro.
// EN: Registers every built-in command in the registry.
void CommandRegistry::RegisterBuiltins() {
    // ES: /help — lista todos los comandos registrados.
    // /help — List all registered commands
    Register("help", "List all available commands", [](const CommandArgs&) -> std::string {
        auto cmds = CommandRegistry::Get().GetAll();
        std::string result = "--- Commands ---";
        for (auto* cmd : cmds) {
            result += "\n/" + cmd->name + " - " + cmd->description;
        }
        return result;
    });

    // ES: /tp [jugador] — teletransporta tu escuadra al jugador remoto más cercano
    //     (o al que coincida con el nombre dado).
    // /tp [player] — Teleport to nearest (or named) remote player
    Register("tp", "Teleport to player (/tp or /tp name)", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected to a server.";

        // ES: Si se da un nombre, buscar una entidad suya y teletransportarse a ella.
        // If a player name is given, find their entity and teleport to it
        if (!args.args.empty()) {
            std::string targetName = args.args[0];
            // ES: Unir todos los argumentos por si el nombre tiene espacios.
            // Join all args in case name has spaces
            for (size_t i = 1; i < args.args.size(); i++)
                targetName += " " + args.args[i];

            // ES: Buscar el jugador remoto por nombre (sin mayúsculas, primero exacto, luego prefijo).
            // Search remote players for a name match (case-insensitive partial)
            auto remotePlayers = core.GetPlayerController().GetAllRemotePlayers();
            PlayerID foundId = 0;
            std::string foundName;

            // ES: Primera pasada: coincidencia exacta.
            // First pass: exact match (case-insensitive)
            for (auto& rp : remotePlayers) {
                std::string rpLower = rp.playerName;
                std::string tgtLower = targetName;
                std::transform(rpLower.begin(), rpLower.end(), rpLower.begin(), ::tolower);
                std::transform(tgtLower.begin(), tgtLower.end(), tgtLower.begin(), ::tolower);
                if (rpLower == tgtLower) { foundId = rp.playerId; foundName = rp.playerName; break; }
            }
            // ES: Segunda pasada: coincidencia por prefijo.
            // Second pass: prefix match
            if (foundId == 0) {
                for (auto& rp : remotePlayers) {
                    std::string rpLower = rp.playerName;
                    std::string tgtLower = targetName;
                    std::transform(rpLower.begin(), rpLower.end(), rpLower.begin(), ::tolower);
                    std::transform(tgtLower.begin(), tgtLower.end(), tgtLower.begin(), ::tolower);
                    if (rpLower.find(tgtLower) == 0) { foundId = rp.playerId; foundName = rp.playerName; break; }
                }
            }

            if (foundId == 0) return "Player '" + targetName + "' not found.";

            // ES: Buscar una entidad de ese jugador con posición conocida (distinta de 0,0,0).
            // Find an entity owned by this player
            auto remoteEntities = core.GetEntityRegistry().GetRemoteEntities();
            Vec3 targetPos(0, 0, 0);
            bool foundPos = false;
            for (EntityID eid : remoteEntities) {
                auto info = core.GetEntityRegistry().GetInfo(eid);
                if (info.has_value() && info->ownerPlayerId == foundId) {
                    Vec3 pos = info->lastPosition;
                    if (pos.x != 0.f || pos.y != 0.f || pos.z != 0.f) {
                        targetPos = pos;
                        foundPos = true;
                        break;
                    }
                }
            }
            if (!foundPos) return "Player '" + foundName + "' has no visible entities.";

            // ES: Teletransportar la escuadra local al objetivo, repartida en una rejilla
            //     de 4 columnas separadas 3 unidades para que no se solapen.
            // EN: Teleport the local squad to the target, spread on a 4-column grid
            //     3 units apart so characters do not overlap.
            // Teleport local squad to target
            auto localEntities = core.GetEntityRegistry().GetPlayerEntities(core.GetLocalPlayerId());
            int teleported = 0;
            for (EntityID netId : localEntities) {
                void* gameObj = core.GetEntityRegistry().GetGameObject(netId);
                if (!gameObj) continue;
                game::CharacterAccessor accessor(gameObj);
                if (!accessor.IsValid()) continue;
                Vec3 tpPos = targetPos;
                tpPos.x += static_cast<float>(teleported % 4) * 3.0f;
                tpPos.z += static_cast<float>(teleported / 4) * 3.0f;
                if (accessor.WritePosition(tpPos)) {
                    core.GetEntityRegistry().UpdatePosition(netId, tpPos);
                    teleported++;
                }
            }
            if (teleported > 0) return "Teleported to " + foundName + "!";
            return "Teleport failed — no valid local characters.";
        }

        // ES: Sin nombre: teletransportarse al más cercano (el método ya muestra sus mensajes).
        // No name given — teleport to nearest
        if (core.TeleportToNearestRemotePlayer()) {
            return ""; // TeleportToNearestRemotePlayer already shows messages
        }
        return ""; // Error messages already shown by the method
    });

    // ES: /teleport — alias que reenvía los argumentos a /tp.
    // /teleport alias — forward args to /tp
    Register("teleport", "Teleport to player (/teleport or /teleport name)", [](const CommandArgs& args) -> std::string {
        std::string cmd = "/tp";
        for (auto& a : args.args) cmd += " " + a;
        return CommandRegistry::Get().Execute(cmd);
    });

    // ES: /pos — muestra la posición del primer personaje local.
    // /pos — Show current position
    Register("pos", "Show your current position", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto localEntities = core.GetEntityRegistry().GetPlayerEntities(core.GetLocalPlayerId());
        if (!localEntities.empty()) {
            void* obj = core.GetEntityRegistry().GetGameObject(localEntities[0]);
            if (obj) {
                game::CharacterAccessor accessor(obj);
                Vec3 pos = accessor.GetPosition();
                char buf[128];
                snprintf(buf, sizeof(buf), "Position: (%.0f, %.0f, %.0f)", pos.x, pos.y, pos.z);
                return buf;
            }
        }
        return "No local character found.";
    });

    // ES: /position — alias de /pos.
    // /position alias
    Register("position", "Show your current position", [](const CommandArgs&) -> std::string {
        return CommandRegistry::Get().Execute("/pos");
    });

    // ES: /players — lista los jugadores conectados con su ID.
    // /players — List connected players with IDs
    Register("players", "List connected players", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& pc = core.GetPlayerController();
        auto remotePlayers = pc.GetAllRemotePlayers();

        std::string result = "--- Players ---";
        result += "\nYou: " + pc.GetLocalPlayerName();
        for (auto& rp : remotePlayers) {
            result += "\n  " + rp.playerName + " (ID " + std::to_string(rp.playerId) + ")";
        }
        result += "\nTotal: " + std::to_string(1 + remotePlayers.size());
        return result;
    });

    // ES: /who — alias de /players.
    // /who alias
    Register("who", "List connected players", [](const CommandArgs&) -> std::string {
        return CommandRegistry::Get().Execute("/players");
    });

    // ES: /status — estado de conexión, entidades y spawns pendientes.
    // /status — Connection, entity, spawn stats
    Register("status", "Show connection and entity status", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& sm = core.GetSpawnManager();
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Connected: %s | Entities: %d | Remote: %d | PendingSpawns: %d | Templates: %d",
                 core.IsConnected() ? "yes" : "no",
                 (int)core.GetEntityRegistry().GetEntityCount(),
                 (int)core.GetEntityRegistry().GetRemoteCount(),
                 (int)sm.GetPendingSpawnCount(),
                 (int)sm.GetTemplateCount());
        return buf;
    });

    // ES: /connect [ip] [puerto] — conecta a un servidor o, si ya está conectado y
    //     sin argumentos, fuerza una sincronización (igual que /sync).
    // /connect [ip] [port] — Connect to a server, or trigger sync if already connected
    Register("connect", "Connect to a server (ip [port]), or trigger sync if already connected", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();

        // ES: Ya conectado y sin argumentos: forzar sync (reanudar hooks, enviar
        //     entidades locales y forzar el spawn de los jugadores remotos).
        // If already connected and no args: trigger sync (same as /sync)
        if (core.IsConnected() && args.args.empty()) {
            if (!core.IsGameLoaded()) return "Connected but game not loaded. Load a save first.";
            entity_hooks::ResumeForNetwork();
            core.SendExistingEntitiesToServer();
            core.ForceSpawnRemotePlayers();
            auto localCount = core.GetEntityRegistry().GetPlayerEntities(core.GetLocalPlayerId()).size();
            return "Sync triggered! " + std::to_string(localCount) + " local entities sent. Use /status for details.";
        }

        // ES: Ya conectado y con argumentos: desconectar primero.
        // If already connected with args: disconnect first
        if (core.IsConnected()) {
            core.GetClient().Disconnect();
            core.SetConnected(false);
        }

        if (args.args.empty()) return "Usage: /connect <ip> [port]";

        std::string ip = args.args[0];
        uint16_t port = KMP_DEFAULT_PORT; // 27800
        if (args.args.size() >= 2) {
            try {
                port = static_cast<uint16_t>(std::stoi(args.args[1]));
            } catch (...) {
                return "Invalid port number.";
            }
        }

        // ES: Guardar datos de conexión y nombre de jugador para el handshake.
        // Set player name for handshake
        core.GetOverlay().SetConnectionInfo(ip, port, core.GetConfig().playerName);

        // ES: Conexión asíncrona: la fase pasa a Connecting y el overlay muestra el estado.
        // EN: Asynchronous connect: phase becomes Connecting and the overlay shows the status.
        if (core.GetClient().ConnectAsync(ip, port)) {
            core.TransitionTo(ClientPhase::Connecting);
            core.GetOverlay().SetConnecting(true);
            if (core.IsGameLoaded()) {
                return "Connecting to " + ip + ":" + std::to_string(port) + "...";
            } else {
                return "Connecting to " + ip + ":" + std::to_string(port) + " — sync starts when you load a save.";
            }
        }
        return "Connection failed to start.";
    });

    // ES: /sync — fuerza el re-escaneo de entidades locales y el spawn de los remotos.
    // /sync — Manually trigger entity scan + spawn remote players
    Register("sync", "Rescan local squad and spawn remote players", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected. Use /connect <ip> first.";
        if (!core.IsGameLoaded()) return "Game not loaded yet. Load a save first.";

        // ES: Reactivar los hooks de entidades si estaban diferidos.
        // Re-enable entity hooks if they were deferred
        entity_hooks::ResumeForNetwork();

        // ES: Forzar re-escaneo (registra los personajes locales en el servidor).
        // Force entity rescan (registers local characters with server)
        core.SendExistingEntitiesToServer();
        auto localCount = core.GetEntityRegistry().GetPlayerEntities(core.GetLocalPlayerId()).size();

        // ES: Forzar el spawn de los personajes remotos pendientes.
        // Force spawn any pending remote characters
        core.ForceSpawnRemotePlayers();
        size_t pending = core.GetSpawnManager().GetPendingSpawnCount();

        std::string result = "Sync triggered! " + std::to_string(localCount) + " local entities sent.";
        if (pending > 0) {
            result += " " + std::to_string(pending) + " remote spawn(s) queued.";
        }

        // ES: Contar cuántos remotos ya tienen objeto del juego (visibles en el mundo).
        // EN: Count how many remotes already have a game object (visible in the world).
        auto remoteEntities = core.GetEntityRegistry().GetRemoteEntities();
        int spawned = 0;
        for (auto eid : remoteEntities) {
            if (core.GetEntityRegistry().GetGameObject(eid)) spawned++;
        }
        if (spawned > 0) {
            result += " " + std::to_string(spawned) + " remote player(s) visible.";
        }

        return result;
    });

    // ES: /disconnect — desconecta del servidor.
    // /disconnect — Disconnect from server
    Register("disconnect", "Disconnect from server", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected.";

        // ES: Mandar los remotos bajo tierra (y=-10000) antes de limpiar el registro:
        //     SetConnected(false) vacía el registro pero no oculta los objetos del juego.
        // Teleport remote entities underground before clearing registry
        // (SetConnected(false) clears the registry but doesn't hide the game objects)
        auto& registry = core.GetEntityRegistry();
        auto remoteEntities = registry.GetRemoteEntities();
        int cleaned = 0;
        for (EntityID eid : remoteEntities) {
            void* gameObj = registry.GetGameObject(eid);
            if (gameObj) {
                game::CharacterAccessor accessor(gameObj);
                Vec3 underground(0.f, -10000.f, 0.f);
                accessor.WritePosition(underground);
                cleaned++;
            }
        }

        core.GetClient().Disconnect();
        core.SetConnected(false);

        std::string msg = "Disconnected from server.";
        if (cleaned > 0)
            msg += " Cleaned up " + std::to_string(cleaned) + " remote entities.";
        return msg;
    });

    // ES: /time [valor] — muestra o fija la hora del día (0.0 = medianoche, 0.5 = mediodía).
    //     Necesita que el hook TimeUpdate haya capturado el TimeManager del juego.
    // /time [value] — Show or set time of day (0.0=midnight, 0.5=noon)
    Register("time", "Show/set time (/time or /time 0.5)", [](const CommandArgs& args) -> std::string {
        if (!time_hooks::HasTimeManager())
            return "Time manager not captured yet (TimeUpdate hook hasn't fired).";

        // ES: Con argumento: intentar escribir la hora.
        // If argument given, try to set time
        if (!args.args.empty()) {
            try {
                float newTime = std::stof(args.args[0]);
                if (newTime < 0.f || newTime >= 1.f) return "Time must be between 0.0 and 1.0 (0=midnight, 0.5=noon).";
                if (time_hooks::WriteTimeOfDay(newTime)) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "Time set to %.2f", newTime);
                    return buf;
                }
                return "Failed to write time.";
            } catch (...) {
                return "Invalid time value. Use 0.0-1.0 (0=midnight, 0.5=noon).";
            }
        }

        // ES: Sin argumento: mostrar la hora actual leída del TimeManager capturado.
        // Show current time (read from captured TimeManager)
        float tod = time_hooks::GetTimeOfDay();
        float speed = time_hooks::GetGameSpeed();

        float hours24 = tod * 24.f;
        int hour = static_cast<int>(hours24) % 24;
        int minute = static_cast<int>((hours24 - std::floor(hours24)) * 60.f);

        char buf[128];
        snprintf(buf, sizeof(buf), "Time: %02d:%02d (%.2f) | Speed: %.1fx",
                 hour, minute, tod, speed);
        return buf;
    });

    // ES: /debug — alterna el panel de log/depuración del HUD nativo.
    // /debug — Toggle debug info overlay
    Register("debug", "Toggle debug info overlay", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        core.GetNativeHud().ToggleLogPanel();
        return "Debug overlay toggled.";
    });

    // ES: /entities — resumen de entidades rastreadas (locales, remotas, spawneadas).
    // /entities — List all tracked entities by type
    Register("entities", "List all tracked entities", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& er = core.GetEntityRegistry();

        size_t total = er.GetEntityCount();
        size_t remote = er.GetRemoteCount();
        size_t spawned = er.GetSpawnedRemoteCount();

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Entities: %d total | %d local | %d remote (%d spawned in world)",
                 (int)total, (int)(total - remote), (int)remote, (int)spawned);
        return buf;
    });

    // ES: /ping — muestra el ping actual al servidor.
    // /ping — Show current ping to server
    Register("ping", "Show current ping to server", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected.";

        uint32_t ping = core.GetClient().GetPing();
        return "Ping: " + std::to_string(ping) + " ms";
    });

    // ES: /kick <jugador> [motivo] — expulsa a un jugador (solo host). Envía
    //     C2S_AdminCommand con commandType 0; el servidor decide.
    // /kick <player> [reason] — Kick a player (host only)
    Register("kick", "Kick a player (host only)", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected.";
        if (!core.IsHost()) return "Only the host can kick players.";
        if (args.args.empty()) return "Usage: /kick <name> [reason]";

        std::string targetName = args.args[0];
        std::string reason;
        for (size_t i = 1; i < args.args.size(); i++)
            reason += (i > 1 ? " " : "") + args.args[i];

        auto remotePlayers = core.GetPlayerController().GetAllRemotePlayers();
        PlayerID targetId = 0;
        for (auto& rp : remotePlayers) {
            std::string rpLower = rp.playerName;
            std::string tgtLower = targetName;
            std::transform(rpLower.begin(), rpLower.end(), rpLower.begin(), ::tolower);
            std::transform(tgtLower.begin(), tgtLower.end(), tgtLower.begin(), ::tolower);
            if (rpLower.find(tgtLower) == 0) { targetId = rp.playerId; break; }
        }
        if (targetId == 0) return "Player '" + targetName + "' not found.";

        // ES: Mensaje de comando de administración (commandType 0 = kick).
        // EN: Admin command message (commandType 0 = kick).
        MsgAdminCommand msg{};
        msg.commandType = 0; // kick
        msg.targetPlayerId = targetId;
        if (!reason.empty()) strncpy(msg.textParam, reason.c_str(), sizeof(msg.textParam) - 1);

        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_AdminCommand);
        writer.WriteRaw(&msg, sizeof(msg));
        core.GetClient().SendReliable(writer.Data(), writer.Size());
        return "Kick request sent.";
    });

    // ES: /announce <mensaje> — difunde un mensaje de sistema (solo host, commandType 4).
    // /announce <message> — Broadcast system message (host only)
    Register("announce", "Broadcast system message (host only)", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected.";
        if (!core.IsHost()) return "Only the host can announce.";
        if (args.args.empty()) return "Usage: /announce <message>";

        std::string message;
        for (size_t i = 0; i < args.args.size(); i++)
            message += (i > 0 ? " " : "") + args.args[i];

        MsgAdminCommand msg{};
        msg.commandType = 4; // announce
        strncpy(msg.textParam, message.c_str(), sizeof(msg.textParam) - 1);

        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_AdminCommand);
        writer.WriteRaw(&msg, sizeof(msg));
        core.GetClient().SendReliable(writer.Data(), writer.Size());
        return "Announcement sent.";
    });

    // ES: /gamespeed <valor> — pide al servidor cambiar la velocidad del juego
    //     (solo host, commandType 5, rango 0.1-10.0).
    // /gamespeed <value> — Set game speed (host only)
    Register("gamespeed", "Set game speed 0.1-10.0 (host only)", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected.";
        if (!core.IsHost()) return "Only the host can change game speed.";
        if (args.args.empty()) return "Usage: /gamespeed <value>   (e.g. /gamespeed 2.0)";

        float newSpeed = 1.0f;
        try { newSpeed = std::stof(args.args[0]); }
        catch (...) { return "Invalid number: " + args.args[0]; }

        if (newSpeed < 0.1f || newSpeed > 10.0f)
            return "Game speed must be between 0.1 and 10.0.";

        MsgAdminCommand msg{};
        msg.commandType = 5; // setSpeed
        msg.floatParam  = newSpeed;

        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_AdminCommand);
        writer.WriteRaw(&msg, sizeof(msg));
        core.GetClient().SendReliable(writer.Data(), writer.Size());

        char buf[64];
        snprintf(buf, sizeof(buf), "Game speed request sent (%.2fx).", newSpeed);
        return std::string(buf);
    });

    // ES: HERRAMIENTAS DE DEPURACIÓN / INGENIERÍA INVERSA
    // ═══════════════════════════════════════════════════════════════════
    // DEBUG / REVERSE ENGINEERING TOOLS
    // ═══════════════════════════════════════════════════════════════════

    // ES: /offsets — vuelca todos los offsets conocidos de las estructuras del juego
    //     (posición del campo dentro de cada clase) con su estado de verificación.
    // /offsets — Dump all known offsets with verification status
    Register("offsets", "Dump all game offsets and their status", [](const CommandArgs&) -> std::string {
        auto& co = game::GetOffsets().character;
        auto& wo = game::GetOffsets().world;

        // ES: Formatea una línea "nombre 0xOFF OK" o "-1 UNKNOWN" si el offset no se conoce (<0).
        // EN: Formats a "name 0xOFF OK" line, or "-1 UNKNOWN" when the offset is unknown (<0).
        auto fmtOff = [](const char* name, int val) -> std::string {
            char buf[64];
            if (val >= 0)
                snprintf(buf, sizeof(buf), "\n  %-22s 0x%03X  OK", name, val);
            else
                snprintf(buf, sizeof(buf), "\n  %-22s  -1    UNKNOWN", name);
            return buf;
        };

        std::string r = "--- Character Offsets ---";
        r += fmtOff("name", co.name);
        r += fmtOff("faction", co.faction);
        r += fmtOff("position (read)", co.position);
        r += fmtOff("rotation", co.rotation);
        r += fmtOff("gameDataPtr", co.gameDataPtr);
        r += fmtOff("inventory", co.inventory);
        r += fmtOff("stats", co.stats);
        r += fmtOff("animClassOffset", co.animClassOffset);
        r += fmtOff("charMovementOffset", co.charMovementOffset);
        r += fmtOff("writablePosOffset", co.writablePosOffset);
        r += fmtOff("writablePosVecOff", co.writablePosVecOffset);
        r += fmtOff("squad", co.squad);
        r += fmtOff("equipment", co.equipment);
        r += fmtOff("isPlayerControlled", co.isPlayerControlled);
        r += fmtOff("health (direct)", co.health);
        r += fmtOff("healthPartArray", co.healthPartArray);
        r += fmtOff("healthPartCount", co.healthPartCount);
        r += fmtOff("healthBase", co.healthBase);
        r += fmtOff("moneyChain1", co.moneyChain1);
        r += fmtOff("moneyChain2", co.moneyChain2);
        r += fmtOff("moneyBase", co.moneyBase);
        r += fmtOff("sceneNode", co.sceneNode);
        r += fmtOff("aiPackage", co.aiPackage);
        r += "\n--- World Offsets ---";
        r += fmtOff("gameSpeed", wo.gameSpeed);
        r += fmtOff("characterList", wo.characterList);
        r += fmtOff("zoneManager", wo.zoneManager);
        r += fmtOff("timeOfDay", wo.timeOfDay);

        // ES: Resumen: cuántos de los offsets clave se conocen y cuántos no.
        // EN: Summary: how many key offsets are known and how many are not.
        int known = 0, unknown = 0;
        auto count = [&](int v) { if (v >= 0) known++; else unknown++; };
        count(co.name); count(co.faction); count(co.position); count(co.rotation);
        count(co.animClassOffset); count(co.squad); count(co.equipment);
        count(co.isPlayerControlled); count(co.health); count(co.sceneNode);
        count(co.aiPackage);

        char summary[128];
        snprintf(summary, sizeof(summary), "\n--- %d known | %d unknown ---", known, unknown);
        r += summary;
        return r;
    });

    // ES: /dump <dir_hex> [líneas] — volcado hexadecimal + ASCII de memoria del proceso
    //     (16 bytes por línea, máx. 32 líneas). Usa Memory::Read, que no revienta con
    //     direcciones inválidas: marca '??' y corta en el primer fallo de lectura.
    // EN: Hex + ASCII dump of process memory (16 bytes per line, max 32 lines). Uses
    //     Memory::Read, which does not crash on invalid addresses: prints '??' and
    //     stops at the first failed read.
    // /dump <hex_addr> [lines] — Hex dump memory at address
    Register("dump", "Hex dump memory (/dump <addr> [lines])", [](const CommandArgs& args) -> std::string {
        if (args.args.empty()) return "Usage: /dump <hex_address> [lines=4]";

        uintptr_t addr = 0;
        try {
            addr = std::stoull(args.args[0], nullptr, 16);
        } catch (...) {
            return "Invalid hex address.";
        }

        int lines = 4;
        if (args.args.size() >= 2) {
            try { lines = std::stoi(args.args[1]); } catch (...) {}
        }
        if (lines < 1) lines = 1;
        if (lines > 32) lines = 32;

        std::string r = "--- Memory dump at 0x" + args.args[0] + " ---";
        for (int line = 0; line < lines; line++) {
            uintptr_t lineAddr = addr + line * 16;
            char hexPart[80] = {};
            char asciiPart[20] = {};
            bool anyFail = false;

            for (int b = 0; b < 16; b++) {
                uint8_t byte = 0;
                if (Memory::Read(lineAddr + b, byte)) {
                    sprintf_s(hexPart + b * 3, 4, "%02X ", byte);
                    asciiPart[b] = (byte >= 0x20 && byte <= 0x7E) ? (char)byte : '.';
                } else {
                    sprintf_s(hexPart + b * 3, 4, "?? ");
                    asciiPart[b] = '?';
                    anyFail = true;
                }
            }
            asciiPart[16] = '\0';

            char lineBuf[160];
            snprintf(lineBuf, sizeof(lineBuf), "\n  %012llX  %s |%s|",
                     (unsigned long long)lineAddr, hexPart, asciiPart);
            r += lineBuf;

            if (anyFail) { r += " [READ FAIL]"; break; }
        }
        return r;
    });

    // ES: /probe — lee todos los campos conocidos del personaje principal (el que
    //     controla el jugador) para comprobar los offsets a mano.
    // /probe — Read all known fields of the primary character
    Register("probe", "Probe primary character's memory fields", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        void* primaryChar = core.GetPlayerController().GetPrimaryCharacter();
        if (!primaryChar) return "No primary character found.";

        uintptr_t ptr = reinterpret_cast<uintptr_t>(primaryChar);
        game::CharacterAccessor accessor(primaryChar);
        auto& co = game::GetOffsets().character;

        char buf[128];
        std::string r = "--- Primary Character Probe ---";
        snprintf(buf, sizeof(buf), "\n  Address:  0x%012llX", (unsigned long long)ptr);
        r += buf;

        // ES: Nombre / posición / rotación / facción leídos con CharacterAccessor.
        // EN: Name / position / rotation / faction read through CharacterAccessor.
        // Name
        std::string name = accessor.GetName();
        r += "\n  Name:     " + (name.empty() ? "(empty)" : name);

        // Position
        Vec3 pos = accessor.GetPosition();
        snprintf(buf, sizeof(buf), "\n  Position: (%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);
        r += buf;

        // Rotation
        Quat rot = accessor.GetRotation();
        snprintf(buf, sizeof(buf), "\n  Rotation: (%.2f, %.2f, %.2f, %.2f)", rot.w, rot.x, rot.y, rot.z);
        r += buf;

        // Faction
        uintptr_t factionPtr = accessor.GetFactionPtr();
        if (factionPtr) {
            game::FactionAccessor faction(reinterpret_cast<void*>(factionPtr));
            std::string factionName = faction.GetName();
            snprintf(buf, sizeof(buf), "\n  Faction:  0x%llX '%s'",
                     (unsigned long long)factionPtr, factionName.c_str());
        } else {
            snprintf(buf, sizeof(buf), "\n  Faction:  (null)");
        }
        r += buf;

        // ES: GameData: plantilla de datos del personaje; en GameData+0x28 está su nombre
        //     (std::string de Kenshi, según docs/architecture/06-game-offsets.md, sin verificar del todo).
        // EN: GameData: the character's data template; its name lives at GameData+0x28
        //     (Kenshi std::string, per docs/architecture/06-game-offsets.md, not fully verified).
        // GameData
        uintptr_t gdPtr = accessor.GetGameDataPtr();
        if (gdPtr) {
            std::string gdName = SpawnManager::ReadKenshiString(gdPtr + 0x28);
            snprintf(buf, sizeof(buf), "\n  GameData: 0x%llX '%s'",
                     (unsigned long long)gdPtr, gdName.c_str());
        } else {
            snprintf(buf, sizeof(buf), "\n  GameData: (null)");
        }
        r += buf;

        // ES: Escuadra (puntero), salud de la cabeza y dinero (cats).
        // EN: Squad (pointer), head health and money (cats).
        // Squad
        uintptr_t squadPtr = accessor.GetSquadPtr();
        snprintf(buf, sizeof(buf), "\n  Squad:    0x%llX", (unsigned long long)squadPtr);
        r += buf;

        // Health chain
        float hp = accessor.GetHealth(BodyPart::Head);
        snprintf(buf, sizeof(buf), "\n  Health:   %.1f (head)", hp);
        r += buf;

        // Money
        int money = accessor.GetMoney();
        snprintf(buf, sizeof(buf), "\n  Money:    %d cats", money);
        r += buf;

        // ES: Offsets de AnimClass e isPlayerControlled (o UNKNOWN).
        // EN: AnimClass and isPlayerControlled offsets (or UNKNOWN).
        // AnimClass offset
        snprintf(buf, sizeof(buf), "\n  AnimClass offset: %s",
                 co.animClassOffset >= 0 ? std::to_string(co.animClassOffset).c_str() : "UNKNOWN");
        r += buf;

        // isPlayerControlled offset
        snprintf(buf, sizeof(buf), "\n  PlayerControlled offset: %s",
                 co.isPlayerControlled >= 0 ? std::to_string(co.isPlayerControlled).c_str() : "UNKNOWN");
        r += buf;

        // ES: Prueba de la cadena de escritura de posición: char+animClassOffset -> AnimClass,
        //     AnimClass+charMovementOffset -> CharMovement, y en CharMovement+writablePosOffset
        //     +writablePosVecOffset está el Vec3 que el juego usa como posición escribible
        //     (la que usa WritePosition para teletransportar).
        // EN: Write-position chain test: char+animClassOffset -> AnimClass,
        //     AnimClass+charMovementOffset -> CharMovement, and at CharMovement+writablePosOffset
        //     +writablePosVecOffset lies the Vec3 the game uses as the writable position
        //     (the one WritePosition uses to teleport).
        // Write-position chain test
        if (co.animClassOffset >= 0) {
            uintptr_t animClass = 0;
            Memory::Read(ptr + co.animClassOffset, animClass);
            uintptr_t charMov = 0;
            if (animClass) Memory::Read(animClass + co.charMovementOffset, charMov);
            snprintf(buf, sizeof(buf), "\n  WritePos chain: animClass=0x%llX charMov=0x%llX",
                     (unsigned long long)animClass, (unsigned long long)charMov);
            r += buf;
            if (charMov) {
                uintptr_t posAddr = charMov + co.writablePosOffset + co.writablePosVecOffset;
                float wx = 0, wy = 0, wz = 0;
                Memory::Read(posAddr, wx);
                Memory::Read(posAddr + 4, wy);
                Memory::Read(posAddr + 8, wz);
                snprintf(buf, sizeof(buf), "\n  WritablePos:    (%.1f, %.1f, %.1f)", wx, wy, wz);
                r += buf;
            }
        }

        return r;
    });

    // ES: /chars — lista las entidades del registro (locales [L] y remotas [R]) y
    //     cuántos personajes ve el CharacterIterator del juego.
    // /chars — List all characters visible to the mod
    Register("chars", "List all known characters", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& registry = core.GetEntityRegistry();
        std::string r = "--- Registry Entities ---";

        auto localEntities = registry.GetPlayerEntities(core.GetLocalPlayerId());
        auto remoteEntities = registry.GetRemoteEntities();

        char buf[256];
        snprintf(buf, sizeof(buf), "\n  Local: %d  Remote: %d",
                 (int)localEntities.size(), (int)remoteEntities.size());
        r += buf;

        // ES: Entidades locales.
        // Local entities
        for (EntityID eid : localEntities) {
            void* obj = registry.GetGameObject(eid);
            if (obj) {
                game::CharacterAccessor accessor(obj);
                Vec3 pos = accessor.GetPosition();
                std::string name = accessor.GetName();
                snprintf(buf, sizeof(buf), "\n  [L] #%u 0x%llX '%s' (%.0f,%.0f,%.0f)",
                         eid, (unsigned long long)obj, name.c_str(), pos.x, pos.y, pos.z);
            } else {
                snprintf(buf, sizeof(buf), "\n  [L] #%u (no game object)", eid);
            }
            r += buf;
        }

        // ES: Entidades remotas (sin objeto del juego = spawn pendiente).
        // Remote entities
        for (EntityID eid : remoteEntities) {
            void* obj = registry.GetGameObject(eid);
            auto info = registry.GetInfo(eid);
            PlayerID owner = info.has_value() ? info->ownerPlayerId : 0;
            if (obj) {
                game::CharacterAccessor accessor(obj);
                Vec3 pos = accessor.GetPosition();
                std::string name = accessor.GetName();
                snprintf(buf, sizeof(buf), "\n  [R] #%u owner=%u 0x%llX '%s' (%.0f,%.0f,%.0f)",
                         eid, owner, (unsigned long long)obj, name.c_str(), pos.x, pos.y, pos.z);
            } else {
                snprintf(buf, sizeof(buf), "\n  [R] #%u owner=%u (no game object — pending spawn)",
                         eid, owner);
            }
            r += buf;
        }

        // ES: Número de personajes que hay en el mundo según CharacterIterator.
        // CharacterIterator count
        game::CharacterIterator iter;
        snprintf(buf, sizeof(buf), "\n--- CharacterIterator: %d characters in world ---", iter.Count());
        r += buf;

        // ES: La caché de carga se eliminó: CharacterIterator es la única vía de descubrimiento.
        // Loading cache removed — CharacterIterator is the sole discovery path

        return r;
    });

    // ES: /spawn — estado del sistema de spawn (SpawnManager): si la factoría está lista,
    //     plantillas capturadas, rutas de spawn disponibles y la "puerta" de spawn del
    //     LoadingOrchestrator (fase de carga, ráfagas, motivo de bloqueo).
    // /spawn — SpawnManager readiness and state
    Register("spawn", "Show spawn system status", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& sm = core.GetSpawnManager();

        char buf[256];
        std::string r = "--- Spawn System Status ---";

        snprintf(buf, sizeof(buf), "\n  Factory ready:     %s", sm.IsReady() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Pre-call data:     %s", sm.HasPreCallData() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Request struct:    %s", sm.HasRequestStruct() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Pending spawns:    %d", (int)sm.GetPendingSpawnCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Total templates:   %d", (int)sm.GetTemplateCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Factory templates: %d", (int)sm.GetFactoryTemplateCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Char templates:    %d", (int)sm.GetCharacterTemplateCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  GDM pointer:       0x%llX",
                 (unsigned long long)sm.GetManagerPointer());
        r += buf;

        // ES: Rutas de spawn: "in-place replay" (reutilizar una llamada capturada a la
        //     factoría) y spawn directo; ambas necesitan los datos pre-llamada capturados.
        // EN: Spawn paths: "in-place replay" (reusing a captured factory call) and
        //     direct spawn; both need the captured pre-call data.
        // Spawn path readiness
        bool inPlace = sm.IsReady() && sm.HasPreCallData();
        bool direct = sm.HasPreCallData();
        snprintf(buf, sizeof(buf), "\n  --- Spawn Paths ---");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  In-place replay:   %s", inPlace ? "READY" : "NOT READY");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Direct spawn:      %s", direct ? "READY" : "NOT READY");
        r += buf;

        // ES: Estadísticas de spawns in-place hechos por el hook de entidades.
        // In-place spawn stats
        int inPlaceCount = entity_hooks::GetInPlaceSpawnCount();
        bool recentSpawn = entity_hooks::HasRecentInPlaceSpawn(30);
        snprintf(buf, sizeof(buf), "\n  In-place spawns:   %d (recent: %s)",
                 inPlaceCount, recentSpawn ? "yes" : "no");
        r += buf;

        // ES: Estado de juego cargado y conexión.
        // Game loaded state
        snprintf(buf, sizeof(buf), "\n  Game loaded:       %s", core.IsGameLoaded() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Connected:         %s", core.IsConnected() ? "YES" : "NO");
        r += buf;

        // ES: Estado del LoadingOrchestrator (la puerta que decide si se puede spawnear ahora).
        // Loading orchestrator state (spawn gating)
        auto& orch = core.GetLoadingOrch();
        const char* phaseName = "?";
        switch (orch.GetPhase()) {
            case LoadingPhase::Idle: phaseName = "Idle"; break;
            case LoadingPhase::InitialLoad: phaseName = "InitialLoad"; break;
            case LoadingPhase::ZoneTransition: phaseName = "ZoneTransition"; break;
            case LoadingPhase::SpawnLoad: phaseName = "SpawnLoad"; break;
        }
        r += "\n  --- Spawn Gate ---";
        snprintf(buf, sizeof(buf), "\n  Phase:             %s", phaseName);
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Orch game loaded:  %s", orch.IsGameLoaded() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  In burst:          %s", orch.IsInBurst() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Burst count:       %d", orch.GetBurstCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Can spawn now:     %s", AssetFacilitator::Get().CanSpawn() ? "YES" : "NO");
        r += buf;
        std::string blockReason = orch.GetSpawnBlockReason();
        snprintf(buf, sizeof(buf), "\n  Block reason:      %s", blockReason.c_str());
        r += buf;

        return r;
    });

    // ES: /verify — comprueba los offsets leyendo el personaje principal en vivo; cada
    //     campo pasa (PASS), falla (FAIL) o se omite (SKIP, offset desconocido) según
    //     una validación heurística del valor leído.
    // /verify — Cross-verify offsets by reading a live character
    Register("verify", "Verify offsets against live character data", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        void* primaryChar = core.GetPlayerController().GetPrimaryCharacter();
        if (!primaryChar) return "No primary character — load a game first.";

        uintptr_t ptr = reinterpret_cast<uintptr_t>(primaryChar);
        auto& co = game::GetOffsets().character;
        std::string r = "--- Offset Verification ---";
        char buf[256];
        int pass = 0, fail = 0, skip = 0;

        // ES: Aplica el validador a la dirección char+offset y acumula el resultado.
        // EN: Runs the validator on the char+offset address and tallies the result.
        auto check = [&](const char* name, int offset, auto validator) {
            if (offset < 0) { skip++; r += "\n  SKIP " + std::string(name); return; }
            if (validator(ptr + offset)) {
                pass++;
                snprintf(buf, sizeof(buf), "\n  PASS %-20s +0x%03X", name, offset);
            } else {
                fail++;
                snprintf(buf, sizeof(buf), "\n  FAIL %-20s +0x%03X", name, offset);
            }
            r += buf;
        };

        // ES: Posición: no debe ser (0,0,0).
        // Position: should be non-zero
        check("position", co.position, [](uintptr_t addr) {
            float x = 0, y = 0, z = 0;
            Memory::Read(addr, x); Memory::Read(addr + 4, y); Memory::Read(addr + 8, z);
            return (x != 0.f || y != 0.f || z != 0.f);
        });

        // ES: Rotación: cuaternión (w,x,y,z) con magnitud al cuadrado cerca de 1.
        // Rotation: w should be near 1.0 for identity, and magnitude ~1
        check("rotation", co.rotation, [](uintptr_t addr) {
            float w = 0, x = 0, y = 0, z = 0;
            Memory::Read(addr, w); Memory::Read(addr + 4, x);
            Memory::Read(addr + 8, y); Memory::Read(addr + 12, z);
            float mag = w * w + x * x + y * y + z * z;
            return (mag > 0.5f && mag < 1.5f);
        });

        // ES: Facción: debe ser un puntero de modo usuario válido.
        // Faction: should be a valid pointer
        check("faction", co.faction, [](uintptr_t addr) {
            uintptr_t val = 0;
            Memory::Read(addr, val);
            return (val > 0x10000 && val < 0x00007FFFFFFFFFFF);
        });

        // ES: Nombre: std::string de MSVC legible (en +0x10 la longitud, en +0x18 la capacidad).
        // Name: should be a readable string (check SSO layout)
        check("name", co.name, [](uintptr_t addr) {
            uint64_t length = 0, capacity = 0;
            Memory::Read(addr + 0x10, length);
            Memory::Read(addr + 0x18, capacity);
            return (length > 0 && length < 200 && capacity >= length);
        });

        // ES: GameData: debe ser un puntero válido.
        // GameData: should be a valid pointer
        check("gameDataPtr", co.gameDataPtr, [](uintptr_t addr) {
            uintptr_t val = 0;
            Memory::Read(addr, val);
            return (val > 0x10000 && val < 0x00007FFFFFFFFFFF);
        });

        // ES: Inventario: debe ser un puntero válido.
        // Inventory: should be a valid pointer
        check("inventory", co.inventory, [](uintptr_t addr) {
            uintptr_t val = 0;
            Memory::Read(addr, val);
            return (val > 0x10000 && val < 0x00007FFFFFFFFFFF);
        });

        // ES: Stats: puntero o bloque en línea; basta con que no sea cero.
        // Stats: pointer or inline — should be non-zero region
        check("stats", co.stats, [](uintptr_t addr) {
            uintptr_t val = 0;
            Memory::Read(addr, val);
            return (val != 0);
        });

        // Health chain — cadena CANÓNICA MedicalSystem (ver game_types.h):
        //   partArray = [char+0x5F8] (HealthPartStatus**), partCount = [char+0x5F0],
        //   part_0 = [partArray], flesh = [part_0+0x40]
        // EN: Health chain — CANONICAL MedicalSystem chain (see game_types.h):
        //   partArray = [char+0x5F8] (HealthPartStatus**), partCount = [char+0x5F0],
        //   part_0 = [partArray], flesh = [part_0+0x40]
        {
            r += "\n  --- Health Chain (canonica MedicalSystem) ---";
            uintptr_t partArray = 0, part0 = 0;
            int partCount = 0;
            float hp = 0;
            bool ok = false;
            Memory::Read(ptr + co.healthPartArray, partArray);
            Memory::Read(ptr + co.healthPartCount, partCount);
            if (partArray > 0x10000 && partArray < 0x00007FFFFFFFFFFF &&
                partCount > 0 && partCount <= 32) {
                Memory::Read(partArray, part0);
                if (part0 > 0x10000 && part0 < 0x00007FFFFFFFFFFF) {
                    Memory::Read(part0 + co.healthBase, hp);
                    ok = (hp >= -100.f && hp <= 200.f);
                }
            }
            snprintf(buf, sizeof(buf),
                     "\n  %s health chain: +%X -> partArray=0x%llX (count=%d) -> part0=0x%llX -> +%X -> %.1f",
                     ok ? "PASS" : "FAIL",
                     co.healthPartArray, (unsigned long long)partArray, partCount,
                     (unsigned long long)part0, co.healthBase, hp);
            r += buf;
            if (ok) pass++; else fail++;
        }

        // ES: Cadena AnimClass -> CharMovement -> posición escribible.
        // AnimClass chain
        if (co.animClassOffset >= 0) {
            uintptr_t animClass = 0, charMov = 0;
            Memory::Read(ptr + co.animClassOffset, animClass);
            bool ok = false;
            if (animClass > 0x10000 && animClass < 0x00007FFFFFFFFFFF) {
                Memory::Read(animClass + co.charMovementOffset, charMov);
                if (charMov > 0x10000 && charMov < 0x00007FFFFFFFFFFF) {
                    float wx = 0;
                    Memory::Read(charMov + co.writablePosOffset + co.writablePosVecOffset, wx);
                    // ES: Nota: solo comprueba que x != 0, no que coincida con la posición cacheada.
                    // EN: Note: it only checks x != 0, not that it matches the cached position.
                    ok = (wx != 0.f); // writable position should match cached
                }
            }
            snprintf(buf, sizeof(buf), "\n  %s writePos chain: anim=0x%llX charMov=0x%llX",
                     ok ? "PASS" : "FAIL",
                     (unsigned long long)animClass, (unsigned long long)charMov);
            r += buf;
            if (ok) pass++; else fail++;
        } else {
            r += "\n  SKIP writePos chain (animClassOffset unknown)";
            skip++;
        }

        snprintf(buf, sizeof(buf), "\n--- %d PASS | %d FAIL | %d SKIP ---", pass, fail, skip);
        r += buf;
        return r;
    });

    // ES: /scan <dir> [inicio] [fin] — recorre una estructura de 8 en 8 bytes (offsets en
    //     hex, máx. 0x400 bytes / 64 líneas y fin <= 0x1000) y clasifica cada qword: nulo,
    //     puntero (intentando leer un nombre en +0x28 como GameData o en +0x10), par de
    //     floats plausibles, o valor crudo. Sirve para buscar offsets a mano.
    // EN: Walks a struct in 8-byte steps (hex offsets, max 0x400 bytes / 64 lines and
    //     end <= 0x1000) and classifies each qword: null, pointer (trying to read a name
    //     at +0x28 like GameData, or at +0x10), plausible float pair, or raw value.
    //     Used to hunt offsets by hand.
    // /scan <charptr> [start] [end] — Scan character memory for pointers/values
    Register("scan", "Scan char struct for pointers (/scan <addr> [start] [end])", [](const CommandArgs& args) -> std::string {
        if (args.args.empty()) return "Usage: /scan <hex_addr> [start_offset=0] [end_offset=0x200]";

        uintptr_t addr = 0;
        try { addr = std::stoull(args.args[0], nullptr, 16); }
        catch (...) { return "Invalid hex address."; }

        int startOff = 0, endOff = 0x200;
        if (args.args.size() >= 2)
            try { startOff = std::stoi(args.args[1], nullptr, 16); } catch (...) {}
        if (args.args.size() >= 3)
            try { endOff = std::stoi(args.args[2], nullptr, 16); } catch (...) {}

        if (endOff > 0x1000) endOff = 0x1000;
        if (endOff - startOff > 0x400) endOff = startOff + 0x400; // Max 64 lines

        std::string r = "--- Pointer scan 0x" + args.args[0] + " ---";
        char buf[256];

        for (int off = startOff; off < endOff; off += 8) {
            uintptr_t val = 0;
            if (!Memory::Read(addr + off, val)) {
                snprintf(buf, sizeof(buf), "\n  +0x%03X: READ FAIL", off);
                r += buf;
                break;
            }

            // ES: Clasificar el valor leído.
            // Classify the value
            const char* tag = "";
            if (val == 0) {
                tag = "(null)";
            } else if (val > 0x10000 && val < 0x00007FFFFFFFFFFF) {
                // ES: Parece un puntero: probar a leer un string en val+0x28 (nombre de GameData).
                // Looks like a pointer — try to read a string at val+0x28 (GameData name)
                std::string name = SpawnManager::ReadKenshiString(val + 0x28);
                if (!name.empty() && name.length() > 1 && name.length() < 100) {
                    snprintf(buf, sizeof(buf), "\n  +0x%03X: 0x%012llX  PTR -> name='%s'",
                             off, (unsigned long long)val, name.c_str());
                    r += buf;
                    continue;
                }
                // ES: Probar el nombre en val+0x10 (std::string de Kenshi con otro layout).
                // Try reading name at val+0x10 (Kenshi std::string at different layout)
                name = SpawnManager::ReadKenshiString(val + 0x10);
                if (!name.empty() && name.length() > 1 && name.length() < 100) {
                    snprintf(buf, sizeof(buf), "\n  +0x%03X: 0x%012llX  PTR -> +10='%s'",
                             off, (unsigned long long)val, name.c_str());
                    r += buf;
                    continue;
                }
                tag = "PTR";
            } else {
                // ES: Probar a interpretarlo como dos floats.
                // Try interpreting as float pair
                float f1 = 0, f2 = 0;
                memcpy(&f1, &val, 4);
                memcpy(&f2, reinterpret_cast<const char*>(&val) + 4, 4);
                if (std::abs(f1) > 0.001f && std::abs(f1) < 1e6f &&
                    std::abs(f2) > 0.001f && std::abs(f2) < 1e6f) {
                    snprintf(buf, sizeof(buf), "\n  +0x%03X: 0x%016llX  float(%.2f, %.2f)",
                             off, (unsigned long long)val, f1, f2);
                    r += buf;
                    continue;
                }
                tag = "";
            }

            snprintf(buf, sizeof(buf), "\n  +0x%03X: 0x%016llX  %s",
                     off, (unsigned long long)val, tag);
            r += buf;
        }
        return r;
    });

    // ES: /hooks — panel de estado de todos los hooks instalados: dirección destino,
    //     activo o no, primeros 8 bytes del prólogo de la función original, modo
    //     (trampolín, vtable, o trampolín con prólogo "mov rax, rsp" = 48 8B C4, que
    //     requiere un tratamiento especial en el trampolín) y contadores de llamadas/crashes.
    // EN: Status panel for every installed hook: target address, enabled or not,
    //     first 8 bytes of the original function prologue, mode (trampoline, vtable,
    //     or trampoline with a "mov rax, rsp" = 48 8B C4 prologue, which needs special
    //     handling in the trampoline) and call/crash counters.
    // /hooks — Hook status dashboard (debug tool)
    Register("hooks", "Show all hook status and prologue bytes", [](const CommandArgs&) -> std::string {
        auto diags = HookManager::Get().GetDiagnostics();
        if (diags.empty()) return "No hooks installed.";

        std::string result = "--- Hook Status ---";
        int active = 0, movRaxCount = 0;

        for (auto& d : diags) {
            char line[256];
            char prologueStr[32];
            snprintf(prologueStr, sizeof(prologueStr),
                     "%02X %02X %02X %02X %02X %02X %02X %02X",
                     d.prologue[0], d.prologue[1], d.prologue[2], d.prologue[3],
                     d.prologue[4], d.prologue[5], d.prologue[6], d.prologue[7]);

            const char* mode = "trampoline";
            if (d.isVtable) mode = "vtable";
            // ES: ¿Empieza el prólogo por mov rax, rsp (48 8B C4)?
            // Check if prologue starts with mov rax, rsp (48 8B C4)
            bool isMovRax = (d.prologue[0] == 0x48 && d.prologue[1] == 0x8B && d.prologue[2] == 0xC4);
            if (isMovRax) { mode = "tramp+movrax"; movRaxCount++; }

            snprintf(line, sizeof(line), "\n%-20s 0x%012llX  %s  [%s]  %s  calls:%d crash:%d",
                     d.name.c_str(),
                     static_cast<unsigned long long>(d.targetAddr),
                     d.enabled ? "ON " : "OFF",
                     prologueStr,
                     mode,
                     d.callCount,
                     d.crashCount);
            result += line;
            if (d.enabled) active++;
        }

        char summary[128];
        snprintf(summary, sizeof(summary),
                 "\n--- %d total | %d active | %d mov-rax-rsp ---",
                 (int)diags.size(), active, movRaxCount);
        result += summary;
        return result;
    });

    // ES: /pipeline — depurador del pipeline de sincronización: sin argumentos alterna
    //     su HUD; "status" vuelca el estado; "entity <id>" muestra la traza de una entidad.
    // EN: Sync pipeline debugger: no args toggles its HUD; "status" dumps the state;
    //     "entity <id>" shows one entity's trace.
    // ── Pipeline debugger ──
    Register("pipeline", "Pipeline debugger (/pipeline [status|entity <id>])",
        [](const CommandArgs& args) -> std::string {
            auto& pipe = Core::Get().GetPipelineOrch();

            if (args.args.empty()) {
                pipe.ToggleHud();
                return pipe.IsHudVisible() ? "Pipeline HUD enabled." : "Pipeline HUD disabled.";
            }

            if (args.args[0] == "status") {
                return pipe.FormatStatusDump();
            }

            if (args.args[0] == "entity" && args.args.size() >= 2) {
                try {
                    EntityID eid = static_cast<EntityID>(std::stoul(args.args[1]));
                    return pipe.FormatEntityTrack(eid);
                } catch (...) {
                    return "Invalid entity ID. Usage: /pipeline entity <id>";
                }
            }

            return "Usage: /pipeline [status|entity <id>]";
        });

    // ES: /discover — descubrimiento de offsets en tiempo de ejecución por anclas:
    //     recorre la estructura del personaje principal buscando DÓNDE están de verdad
    //     los campos (posición, nombre, facción, GameData, salud...) en lugar de fiarse
    //     de offsets fijos (originalmente de la versión GOG). Compara lo encontrado con
    //     los offsets fijos y deja un volcado hex en el log.
    // /discover — Runtime offset discovery using anchor-based scanning
    // Scans the character struct to FIND where fields actually live,
    // instead of assuming hardcoded GOG offsets are correct.
    Register("discover", "Discover character offsets by scanning struct memory", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();
        void* primaryChar = core.GetPlayerController().GetPrimaryCharacter();
        if (!primaryChar) return "No primary character — load a game first.";

        uintptr_t charPtr = reinterpret_cast<uintptr_t>(primaryChar);
        auto& co = game::GetOffsets().character;
        char buf[512];
        std::string r = "--- Offset Discovery ---";
        snprintf(buf, sizeof(buf), "\n  Character at 0x%012llX", (unsigned long long)charPtr);
        r += buf;

        // ES: PASO 0: volcado hex de los primeros 0x500 bytes del personaje al fichero de log.
        // ══════════════════════════════════════════════════════════════
        //  STEP 0: Hex dump first 0x500 bytes to LOG FILE for analysis
        // ══════════════════════════════════════════════════════════════
        spdlog::info("=== CHARACTER STRUCT HEX DUMP (0x500 bytes) at 0x{:012X} ===", charPtr);
        for (int row = 0; row < 0x500; row += 16) {
            char hexLine[128] = {};
            char asciiLine[20] = {};
            int pos = 0;
            for (int b = 0; b < 16; b++) {
                uint8_t byte = 0;
                Memory::Read(charPtr + row + b, byte);
                pos += sprintf_s(hexLine + pos, sizeof(hexLine) - pos, "%02X ", byte);
                asciiLine[b] = (byte >= 0x20 && byte <= 0x7E) ? (char)byte : '.';
            }
            asciiLine[16] = '\0';
            spdlog::info("  +{:03X}: {} |{}|", row, hexLine, asciiLine);
        }
        spdlog::info("=== END HEX DUMP ===");
        r += "\n  Hex dump (0x500 bytes) written to log file.";

        // ES: Auxiliar: ¿parece un puntero válido al heap? (alineado a 8, en rango de modo
        //     usuario y fuera de la imagen del exe, que se aproxima como base + 64 MB).
        // EN: Helper: does it look like a valid heap pointer? (8-aligned, user-mode range
        //     and outside the exe image, approximated as base + 64 MB).
        // Helper: check if value looks like a valid heap pointer
        uintptr_t modBase = Memory::GetModuleBase();
        auto isHeapPtr = [modBase](uintptr_t val) -> bool {
            if (val < 0x10000 || val >= 0x00007FFFFFFFFFFF) return false;
            if ((val & 0x7) != 0) return false;
            if (val >= modBase && val < modBase + 0x4000000) return false;
            return true;
        };

        // ES: Leer las anclas conocidas: posición y nombre vía el accessor actual.
        // Read known anchor: position from the existing accessor
        game::CharacterAccessor accessor(primaryChar);
        Vec3 knownPos = accessor.GetPosition();
        std::string knownName = accessor.GetName();

        snprintf(buf, sizeof(buf), "\n  Known position: (%.1f, %.1f, %.1f)", knownPos.x, knownPos.y, knownPos.z);
        r += buf;
        r += "\n  Known name: " + knownName;

        int matches = 0, mismatches = 0;

        // ES: PASO 1: buscar la POSICIÓN (3 floats seguidos que coincidan con la conocida, ±0.5).
        //     Nota: el texto "OK: Matches hardcoded +0x" imprime el offset en decimal.
        // EN: STEP 1 note: the "OK: Matches hardcoded +0x" text prints the offset in decimal.
        // ══════════════════════════════════════════════════════════════
        //  STEP 1: Scan for POSITION (3 consecutive floats)
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Position Scan ---";
        bool posNonZero = (knownPos.x != 0.f || knownPos.y != 0.f || knownPos.z != 0.f);
        int discoveredPosOffset = -1;
        if (posNonZero) {
            for (int off = 0; off < 0x400; off += 4) {
                float fx = 0, fy = 0, fz = 0;
                Memory::Read(charPtr + off, fx);
                Memory::Read(charPtr + off + 4, fy);
                Memory::Read(charPtr + off + 8, fz);
                if (std::abs(fx - knownPos.x) < 0.5f &&
                    std::abs(fy - knownPos.y) < 0.5f &&
                    std::abs(fz - knownPos.z) < 0.5f) {
                    snprintf(buf, sizeof(buf), "\n  FOUND position at +0x%03X (%.1f, %.1f, %.1f)", off, fx, fy, fz);
                    r += buf;
                    spdlog::info("DISCOVER: Position at +0x{:03X} = ({:.1f}, {:.1f}, {:.1f})", off, fx, fy, fz);
                    if (discoveredPosOffset == -1) discoveredPosOffset = off;
                }
            }
            if (discoveredPosOffset == co.position) {
                r += "\n  OK: Matches hardcoded +0x" + std::to_string(co.position);
                matches++;
            } else if (discoveredPosOffset >= 0) {
                snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.position, discoveredPosOffset);
                r += buf;
                mismatches++;
            } else {
                r += "\n  NOT FOUND";
                mismatches++;
            }
        } else {
            r += "\n  SKIP (position is zero — character may not be loaded)";
        }

        // ES: PASO 2: buscar el NOMBRE (patrón de std::string de MSVC: en +0x10 el tamaño y en
        //     +0x18 la capacidad; si capacidad <= 15 el texto va en línea (SSO), si no, en +0x00
        //     hay un puntero al texto en el heap).
        // ══════════════════════════════════════════════════════════════
        //  STEP 2: Scan for NAME (MSVC std::string pattern)
        //  Look for size/capacity pair where size matches known name length
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Name Scan ---";
        int discoveredNameOffset = -1;
        if (!knownName.empty() && knownName != "Unknown") {
            uint64_t expectedSize = knownName.size();
            for (int off = 0; off < 0x400; off += 8) {
                uint64_t size = 0, capacity = 0;
                Memory::Read(charPtr + off + 0x10, size);
                Memory::Read(charPtr + off + 0x18, capacity);
                if (size != expectedSize || capacity < size || capacity > 256) continue;

                // ES: Leer el texto real para confirmar (SSO en línea o puntero al heap).
                // Try reading the actual string to verify
                char testBuf[257] = {};
                bool readable = true;
                if (capacity <= 15) {
                    // SSO: inline
                    for (size_t i = 0; i < size && i < 256; i++) {
                        if (!Memory::Read(charPtr + off + i, testBuf[i])) { readable = false; break; }
                    }
                } else {
                    // Heap: pointer at +0x00
                    uintptr_t dataPtr = 0;
                    Memory::Read(charPtr + off, dataPtr);
                    if (dataPtr < 0x10000 || dataPtr >= 0x00007FFFFFFFFFFF) continue;
                    for (size_t i = 0; i < size && i < 256; i++) {
                        if (!Memory::Read(dataPtr + i, testBuf[i])) { readable = false; break; }
                    }
                }
                if (!readable) continue;
                std::string foundName(testBuf, (size_t)size);
                if (foundName == knownName) {
                    snprintf(buf, sizeof(buf), "\n  FOUND name at +0x%03X = '%s'", off, foundName.c_str());
                    r += buf;
                    spdlog::info("DISCOVER: Name at +0x{:03X} = '{}'", off, foundName);
                    if (discoveredNameOffset == -1) discoveredNameOffset = off;
                }
            }
            if (discoveredNameOffset == co.name) {
                matches++;
                r += "\n  OK: Matches hardcoded";
            } else if (discoveredNameOffset >= 0) {
                snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.name, discoveredNameOffset);
                r += buf;
                mismatches++;
            } else {
                r += "\n  NOT FOUND";
                mismatches++;
            }
        } else {
            r += "\n  SKIP (name unknown)";
        }

        // ES: PASO 3: buscar la FACCIÓN (puntero al heap cuyo objeto tiene un std::string
        //     legible con el nombre en +0x10).
        // ══════════════════════════════════════════════════════════════
        //  STEP 3: Scan for FACTION (valid heap pointer with name at +0x10)
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Faction Pointer Scan ---";
        int discoveredFactionOffset = -1;
        for (int off = 0; off < 0x100; off += 8) {
            uintptr_t candidate = 0;
            Memory::Read(charPtr + off, candidate);
            if (!isHeapPtr(candidate)) continue;

            // ES: Una facción debe tener un nombre legible en +0x10.
            // A faction should have a readable name at +0x10
            uint64_t fNameSize = 0, fNameCap = 0;
            Memory::Read(candidate + 0x10 + 0x10, fNameSize);
            Memory::Read(candidate + 0x10 + 0x18, fNameCap);
            if (fNameSize < 1 || fNameSize > 100 || fNameCap < fNameSize || fNameCap > 256) continue;

            // ES: Leer el nombre de la facción.
            // Read the faction name
            char fNameBuf[101] = {};
            bool fReadable = true;
            if (fNameCap <= 15) {
                for (size_t i = 0; i < fNameSize && i < 100; i++)
                    if (!Memory::Read(candidate + 0x10 + i, fNameBuf[i])) { fReadable = false; break; }
            } else {
                uintptr_t fDataPtr = 0;
                Memory::Read(candidate + 0x10, fDataPtr);
                if (fDataPtr < 0x10000) continue;
                for (size_t i = 0; i < fNameSize && i < 100; i++)
                    if (!Memory::Read(fDataPtr + i, fNameBuf[i])) { fReadable = false; break; }
            }
            if (!fReadable) continue;

            // ES: Validar: el nombre debe ser ASCII imprimible.
            // Validate: name should be printable ASCII
            bool allAscii = true;
            for (size_t i = 0; i < fNameSize; i++)
                if (fNameBuf[i] < 0x20 || fNameBuf[i] > 0x7E) { allAscii = false; break; }
            if (!allAscii) continue;

            snprintf(buf, sizeof(buf), "\n  FOUND faction ptr at +0x%03X -> 0x%llX name='%s'",
                     off, (unsigned long long)candidate, fNameBuf);
            r += buf;
            spdlog::info("DISCOVER: Faction at +0x{:03X} -> 0x{:X} name='{}'", off, candidate, fNameBuf);

            // ES: Leer también el ID de facción (offset faction.id de la tabla; el comentario
            //     original dice +0x08 pero se usa el valor de la tabla de offsets).
            // EN: Also read the faction ID (faction.id offset from the table; the original
            //     comment says +0x08 but the offset-table value is what is used).
            // Also try reading faction ID at candidate+0x08
            uint32_t factionId = 0;
            const int fIdOff = game::GetOffsets().faction.id;
            if (fIdOff >= 0) {
                Memory::Read(candidate + fIdOff, factionId);
            }
            snprintf(buf, sizeof(buf), " (id=%u)", factionId);
            r += buf;

            if (discoveredFactionOffset == -1) discoveredFactionOffset = off;
        }
        if (discoveredFactionOffset == co.faction) {
            matches++;
            r += "\n  OK: Matches hardcoded +0x" + std::to_string(co.faction);
        } else if (discoveredFactionOffset >= 0) {
            snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.faction, discoveredFactionOffset);
            r += buf;
            mismatches++;
        } else {
            r += "\n  NOT FOUND (no pointer with readable name at +0x10)";
            mismatches++;
        }

        // ES: PASO 4: buscar GAMEDATA (puntero a un objeto con su nombre en +0x28).
        // ══════════════════════════════════════════════════════════════
        //  STEP 4: Scan for GAMEDATA (pointer to object with name at +0x28)
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- GameData Pointer Scan ---";
        int discoveredGDOffset = -1;
        for (int off = 0; off < 0x200; off += 8) {
            uintptr_t candidate = 0;
            Memory::Read(charPtr + off, candidate);
            if (!isHeapPtr(candidate)) continue;

            std::string gdName = SpawnManager::ReadKenshiString(candidate + 0x28);
            if (gdName.empty() || gdName.size() < 2 || gdName.size() > 64) continue;

            // ES: Validar: ASCII imprimible.
            // Validate: printable ASCII
            bool ok = true;
            for (char c : gdName) if (c < 0x20 || c > 0x7E) { ok = false; break; }
            if (!ok) continue;

            // ES: En candidate+0x10 debería estar el GameDataManager* (debe ser coherente).
            // Check for GameDataManager* at candidate+0x10 (should be consistent)
            uintptr_t gdmPtr = 0;
            Memory::Read(candidate + 0x10, gdmPtr);

            snprintf(buf, sizeof(buf), "\n  FOUND GameData at +0x%03X -> 0x%llX name='%s' mgr=0x%llX",
                     off, (unsigned long long)candidate, gdName.c_str(), (unsigned long long)gdmPtr);
            r += buf;
            spdlog::info("DISCOVER: GameData at +0x{:03X} -> 0x{:X} name='{}' mgr=0x{:X}", off, candidate, gdName, gdmPtr);
            if (discoveredGDOffset == -1) discoveredGDOffset = off;
        }
        if (discoveredGDOffset == co.gameDataPtr) {
            matches++;
            r += "\n  OK: Matches hardcoded";
        } else if (discoveredGDOffset >= 0) {
            snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.gameDataPtr, discoveredGDOffset);
            r += buf;
            mismatches++;
        } else {
            r += "\n  NOT FOUND";
            mismatches++;
        }

        // ══════════════════════════════════════════════════════════════
        //  STEP 5: Verify HEALTH CHAIN (cadena canónica MedicalSystem)
        //  [Oleada A 2026-07-12] El scan viejo buscaba "7 floats stride 8" tras
        //  una cadena de 2 derefs — modelo que NO existe en 1.0.68 (la salud vive
        //  en HealthPartStatus* individuales). Ahora se recorre la cadena real:
        //    partArray = [char+0x5F8] → part_i = [partArray + i*8] → flesh @ +0x40
        // ══════════════════════════════════════════════════════════════
        // EN: STEP 5: verify the canonical HEALTH chain (MedicalSystem). The old scan looked
        //     for "7 floats with stride 8" after two dereferences, a model that does NOT exist
        //     in 1.0.68 (health lives in individual HealthPartStatus* objects). Now it walks:
        //     partArray = [char+0x5F8] -> part_i = [partArray + i*8] -> flesh at +0x40.
        r += "\n\n  --- Health Chain (canonica MedicalSystem) ---";
        spdlog::info("DISCOVER: Verifying canonical health chain...");
        {
            uintptr_t partArray = 0;
            int partCount = 0;
            Memory::Read(charPtr + co.healthPartArray, partArray);
            Memory::Read(charPtr + co.healthPartCount, partCount);

            int goodParts = 0;
            float healthVals[7] = {};
            if (isHeapPtr(partArray) && partCount > 0 && partCount <= 32) {
                int n = (partCount < 7) ? partCount : 7;
                for (int p = 0; p < n; p++) {
                    uintptr_t partPtr = 0;
                    if (!Memory::Read(partArray + p * co.healthStride, partPtr)) break;
                    if (!isHeapPtr(partPtr)) continue;
                    float hp = 0;
                    if (!Memory::Read(partPtr + co.healthBase, hp)) continue;
                    healthVals[p] = hp;
                    // Salud plausible para un char cargado: -100..300
                    // EN: Plausible health for a loaded character: -100..300
                    if (hp >= -100.f && hp < 300.f) goodParts++;
                }
            }

            if (goodParts >= 5) {
                matches++;
                snprintf(buf, sizeof(buf),
                         "\n  OK: canonical chain +0x%X (count=%d) -> +0x%X"
                         "\n    [%.0f, %.0f, %.0f, %.0f, %.0f, %.0f, %.0f]",
                         co.healthPartArray, partCount, co.healthBase,
                         healthVals[0], healthVals[1], healthVals[2], healthVals[3],
                         healthVals[4], healthVals[5], healthVals[6]);
                r += buf;
                spdlog::info("DISCOVER: canonical health chain OK — partArray=0x{:X} count={} "
                             "[{:.0f}, {:.0f}, {:.0f}, {:.0f}, {:.0f}, {:.0f}, {:.0f}]",
                             partArray, partCount,
                             healthVals[0], healthVals[1], healthVals[2], healthVals[3],
                             healthVals[4], healthVals[5], healthVals[6]);
            } else {
                snprintf(buf, sizeof(buf),
                         "\n  FAIL: canonical chain — partArray=0x%llX count=%d goodParts=%d",
                         (unsigned long long)partArray, partCount, goodParts);
                r += buf;
                mismatches++;
            }
        }

        // ES: PASO 6: buscar el puntero al INVENTARIO (puntero al heap cuyo objeto tiene
        //     items en +0x10, itemCount en +0x18 y un puntero de vuelta al dueño en +0x28
        //     que debe apuntar a ESTE personaje).
        // ══════════════════════════════════════════════════════════════
        //  STEP 6: Scan for INVENTORY pointer
        //  Valid heap pointer, with owner backpointer and item count
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Inventory Pointer Scan ---";
        int discoveredInvOffset = -1;
        for (int off = 0x200; off < 0x400; off += 8) {
            uintptr_t candidate = 0;
            Memory::Read(charPtr + off, candidate);
            if (!isHeapPtr(candidate)) continue;

            // ES: Inventario: items en +0x10, número de items en +0x18 y dueño en +0x28
            //     (debe apuntar a nuestro personaje).
            // Inventory should have: items ptr at +0x10, itemCount at +0x18
            // and owner backpointer at +0x28 should point back to our character
            uintptr_t ownerPtr = 0;
            Memory::Read(candidate + 0x28, ownerPtr);
            if (ownerPtr != charPtr) continue;  // Owner must be THIS character

            int itemCount = 0;
            Memory::Read(candidate + 0x18, itemCount);
            if (itemCount < 0 || itemCount > 10000) continue;

            snprintf(buf, sizeof(buf), "\n  FOUND inventory at +0x%03X -> 0x%llX (owner=self, items=%d)",
                     off, (unsigned long long)candidate, itemCount);
            r += buf;
            spdlog::info("DISCOVER: Inventory at +0x{:03X} -> 0x{:X} (items={})", off, candidate, itemCount);
            if (discoveredInvOffset == -1) discoveredInvOffset = off;
        }
        if (discoveredInvOffset == co.inventory) {
            matches++;
            r += "\n  OK: Matches hardcoded";
        } else if (discoveredInvOffset >= 0) {
            snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.inventory, discoveredInvOffset);
            r += buf;
            mismatches++;
        } else {
            // ES: Sin coincidencias: repetir sin exigir el dueño (su offset podría ser otro)
            //     y buscar un contador razonable con un puntero de items justo antes.
            // EN:
            // Try without owner check (owner offset might be different)
            r += "\n  NOT FOUND with owner check. Scanning without...";
            for (int off = 0x200; off < 0x400; off += 8) {
                uintptr_t candidate = 0;
                Memory::Read(charPtr + off, candidate);
                if (!isHeapPtr(candidate)) continue;

                // Look for reasonable item count at any nearby offset
                for (int countOff = 0x10; countOff <= 0x30; countOff += 8) {
                    int itemCount = 0;
                    Memory::Read(candidate + countOff, itemCount);
                    if (itemCount >= 0 && itemCount < 500) {
                        // Check if items pointer looks valid
                        uintptr_t itemsPtr = 0;
                        Memory::Read(candidate + countOff - 8, itemsPtr);
                        if (itemCount == 0 || isHeapPtr(itemsPtr)) {
                            snprintf(buf, sizeof(buf), "\n  CANDIDATE inv at +0x%03X -> 0x%llX (count=%d at +0x%02X)",
                                     off, (unsigned long long)candidate, itemCount, countOff);
                            r += buf;
                            spdlog::info("DISCOVER: Inventory candidate at +0x{:03X} -> 0x{:X} (count={} at +0x{:02X})",
                                         off, candidate, itemCount, countOff);
                            if (discoveredInvOffset == -1) discoveredInvOffset = off;
                            break;
                        }
                    }
                }
            }
        }

        // ES: PASO 7: buscar las STATS (bloque en línea de floats en rango de habilidad 1-100;
        //     al menos 7 de 10 floats seguidos). Nota: el texto de OK tiene "+0x450" fijo.
        // EN: STEP 7 note: the OK text has "+0x450" hardcoded.
        // ══════════════════════════════════════════════════════════════
        //  STEP 7: Scan for STATS (inline block of floats in skill range)
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Stats Block Scan ---";
        int discoveredStatsOffset = -1;
        for (int off = 0x300; off < 0x500; off += 4) {
            // Check 10 consecutive floats in reasonable stat range (1-100)
            int statCount = 0;
            for (int s = 0; s < 10; s++) {
                float val = 0;
                Memory::Read(charPtr + off + s * 4, val);
                if (val >= 1.f && val <= 100.f) statCount++;
            }
            if (statCount >= 7) {
                // ES: Leer las 5 primeras stats para mostrarlas.
                // Read first 5 stats for display
                float s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0;
                Memory::Read(charPtr + off, s0);
                Memory::Read(charPtr + off + 4, s1);
                Memory::Read(charPtr + off + 8, s2);
                Memory::Read(charPtr + off + 12, s3);
                Memory::Read(charPtr + off + 16, s4);
                snprintf(buf, sizeof(buf), "\n  FOUND stats block at +0x%03X [%.1f, %.1f, %.1f, %.1f, %.1f, ...]",
                         off, s0, s1, s2, s3, s4);
                r += buf;
                spdlog::info("DISCOVER: Stats block at +0x{:03X} [{:.1f}, {:.1f}, {:.1f}, {:.1f}, {:.1f}]",
                             off, s0, s1, s2, s3, s4);
                if (discoveredStatsOffset == -1) discoveredStatsOffset = off;
            }
        }
        if (discoveredStatsOffset == co.stats) {
            matches++;
            r += "\n  OK: Matches hardcoded +0x450";
        } else if (discoveredStatsOffset >= 0) {
            snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.stats, discoveredStatsOffset);
            r += buf;
            mismatches++;
        } else {
            r += "\n  NOT FOUND";
        }

        // ES: PASO 8: cadena del DINERO. Solo muestra el valor actual y los punteros de la
        //     cadena fija char+moneyChain1 -> +moneyChain2 -> +moneyBase (no busca otra).
        // EN: STEP 8 only shows the current value and the pointers of the fixed chain
        //     char+moneyChain1 -> +moneyChain2 -> +moneyBase (it does not search for another).
        // ══════════════════════════════════════════════════════════════
        //  STEP 8: Scan for MONEY CHAIN
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Money Chain Scan ---";
        int knownMoney = accessor.GetMoney();
        snprintf(buf, sizeof(buf), "\n  Current money (via hardcoded chain): %d cats", knownMoney);
        r += buf;

        // Also dump raw pointers at the hardcoded offsets for debugging
        {
            uintptr_t mc1 = 0, mc2 = 0;
            int moneyVal = 0;
            Memory::Read(charPtr + co.moneyChain1, mc1);
            if (mc1) Memory::Read(mc1 + co.moneyChain2, mc2);
            if (mc2) Memory::Read(mc2 + co.moneyBase, moneyVal);
            snprintf(buf, sizeof(buf), "\n  Chain check: +0x%X->0x%llX, +0x%X->0x%llX, +0x%X->%d",
                     co.moneyChain1, (unsigned long long)mc1,
                     co.moneyChain2, (unsigned long long)mc2,
                     co.moneyBase, moneyVal);
            r += buf;
        }

        // ES: RESUMEN de coincidencias y discrepancias.
        // ══════════════════════════════════════════════════════════════
        //  SUMMARY
        // ══════════════════════════════════════════════════════════════
        snprintf(buf, sizeof(buf), "\n\n--- SUMMARY: %d matches | %d mismatches ---", matches, mismatches);
        r += buf;

        if (mismatches > 0) {
            r += "\n  WARNING: Some offsets do not match hardcoded values!";
            r += "\n  Check log file for hex dump and details.";
            r += "\n  Update offsets in game_types.h if mismatches are confirmed.";
        } else if (matches > 0) {
            r += "\n  All verified offsets match hardcoded values.";
        }

        spdlog::info("DISCOVER: Complete — {} matches, {} mismatches", matches, mismatches);
        return r;
    });

    // ES: /discover_apply — pensado para aplicar los offsets descubiertos a la tabla en vivo.
    //     NO está implementado: solo devuelve un aviso (hay que editar game_types.h a mano).
    // EN: Meant to apply discovered offsets to the live table. NOT implemented: it only
    //     returns a notice (game_types.h must be edited by hand).
    // /discover update — Apply discovered offsets to the live offset table
    Register("discover_apply", "Apply discovered offsets (run /discover first)", [](const CommandArgs& args) -> std::string {
        // This is a placeholder — after /discover confirms correct offsets,
        // this command would update GetOffsets() with the discovered values.
        // For now, users should manually update game_types.h defaults.
        return "Not yet implemented. Run /discover first, check log, then update game_types.h.";
    });

    // ES: /forcespawn — salta todas las puertas de spawn y fuerza el spawn de hasta 16
    //     personajes remotos pendientes (re-encola los remotos "atascados" sin objeto).
    // /forcespawn — Bypass all gates and force-spawn pending remote characters
    Register("forcespawn", "Force-spawn pending remote characters (bypass gates)", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& sm = core.GetSpawnManager();

        if (!core.IsConnected()) return "Not connected to a server.";
        if (!core.IsGameLoaded()) return "Game not loaded yet.";

        size_t pending = sm.GetPendingSpawnCount();

        // ES: Sin spawns pendientes: re-encolar las entidades remotas que no tienen objeto del juego.
        // If no pending spawns, check for stuck remote entities and re-queue them
        if (pending == 0) {
            auto remoteEntities = core.GetEntityRegistry().GetRemoteEntities();
            int requeued = 0;
            for (auto eid : remoteEntities) {
                if (!core.GetEntityRegistry().GetGameObject(eid)) {
                    auto infoCopy = core.GetEntityRegistry().GetInfo(eid);
                    if (infoCopy) {
                        SpawnRequest req;
                        req.netId = eid;
                        req.owner = infoCopy->ownerPlayerId;
                        req.type = infoCopy->type;
                        req.position = infoCopy->lastPosition;
                        sm.QueueSpawn(req);
                        requeued++;
                    }
                }
            }
            if (requeued > 0) {
                pending = requeued;
            } else {
                return "No pending spawns and no stuck remote entities.";
            }
        }

        if (!sm.IsReady()) {
            // ES: Si la factoría no está lista, usar ForceSpawnRemotePlayers (activa el bypass
            //     para el siguiente tick).
            // Try to use ForceSpawnRemotePlayers which sets the bypass flag
            core.ForceSpawnRemotePlayers();
            return "SpawnManager not fully ready — forcing bypass for next tick.";
        }

        int spawned = 0, failed = 0;
        for (size_t i = 0; i < pending && i < 16; i++) {
            SpawnRequest req;
            if (!sm.PopNextSpawn(req)) break;

            // ES: Asignar al dueño un hueco de plantilla del mod ((owner-1) % nº plantillas).
            // Map owner to mod template slot
            int templateCount = sm.GetModTemplateCount();
            int modSlot = 0;
            if (templateCount > 0 && req.owner > 0) {
                modSlot = (static_cast<int>(req.owner) - 1) % templateCount;
            }
            if (modSlot < 0 || modSlot >= templateCount) modSlot = 0;

            // ES: Probar primero la plantilla del mod y, si falla, createRandomChar de la factoría.
            // Try mod template first, then createRandomChar fallback
            void* newChar = nullptr;
            if (templateCount > 0) {
                newChar = sm.SpawnCharacterDirect(&req.position, modSlot);
            }
            if (!newChar) {
                newChar = entity_hooks::CallFactoryCreateRandom(sm.GetFactory());
            }

            uintptr_t addr = reinterpret_cast<uintptr_t>(newChar);
            // ES: Validar el puntero devuelto (rango de modo usuario y alineado a 8).
            // EN: Validate the returned pointer (user-mode range and 8-aligned).
            if (newChar && addr > 0x10000 && addr < 0x00007FFFFFFFFFFF && (addr & 0x7) == 0) {
                core.GetEntityRegistry().SetGameObject(req.netId, newChar);
                core.GetEntityRegistry().UpdatePosition(req.netId, req.position);

                // ES: Configuración completa tras el spawn (posición, facción, IA, sonda de AnimClass).
                // Full post-spawn setup (position, rename, AI suppress, faction fix)
                game::CharacterAccessor accessor(newChar);
                if (req.position.x != 0.f || req.position.y != 0.f || req.position.z != 0.f) {
                    accessor.WritePosition(req.position);
                }

                // ES: Poner la facción del jugador local para evitar el crash cuando el juego
                //     accede a faction+0x250 con una facción inválida.
                // Fix faction pointer to prevent crash on faction+0x250 access
                uintptr_t localFaction = entity_hooks::GetEarlyPlayerFaction();
                if (localFaction == 0) localFaction = entity_hooks::GetFallbackFaction();
                if (localFaction != 0) {
                    accessor.WriteFaction(localFaction);
                }

                core.GetPlayerController().OnRemoteCharacterSpawned(req.netId, newChar, req.owner);
                ai_hooks::MarkRemoteControlled(newChar);
                game::ScheduleDeferredAnimClassProbe(addr);

                spdlog::info("forcespawn: Spawned entity {} at 0x{:X}", req.netId, addr);
                spawned++;
            } else {
                spdlog::warn("forcespawn: All spawn methods returned null for entity {}", req.netId);
                failed++;
            }
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "Force-spawned %d characters (%d failed, %d remaining)",
                 spawned, failed, (int)sm.GetPendingSpawnCount());
        return buf;
    });

    // ES: /fulldiag — volcado de diagnóstico de todos los sistemas (conexión, entidades,
    //     spawn, puerta de spawn, hooks y personaje principal).
    // /fulldiag — Comprehensive diagnostic dump (all systems)
    Register("fulldiag", "Full diagnostic dump of all systems", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& sm = core.GetSpawnManager();
        auto& reg = core.GetEntityRegistry();
        auto& orch = core.GetLoadingOrch();
        char buf[256];
        std::string r;

        // ES: Conexión.
        // ── Connection ──
        r += "=== CONNECTION ===";
        snprintf(buf, sizeof(buf), "\n  Connected: %s  |  Player ID: %u  |  Game loaded: %s",
                 core.IsConnected() ? "YES" : "NO",
                 core.GetLocalPlayerId(),
                 core.IsGameLoaded() ? "YES" : "NO");
        r += buf;

        // ES: Registro de entidades.
        // ── Entity Registry ──
        r += "\n=== ENTITIES ===";
        size_t total = reg.GetEntityCount();
        size_t remote = reg.GetRemoteCount();
        size_t spawned = reg.GetSpawnedRemoteCount();
        snprintf(buf, sizeof(buf), "\n  Total: %d  |  Remote: %d  |  Remote spawned: %d",
                 (int)total, (int)remote, (int)spawned);
        r += buf;

        // ES: Sistema de spawn.
        // ── Spawn System ──
        r += "\n=== SPAWN SYSTEM ===";
        snprintf(buf, sizeof(buf), "\n  Factory: %s  |  PreCall: %s  |  Pending: %d",
                 sm.IsReady() ? "YES" : "NO",
                 sm.HasPreCallData() ? "YES" : "NO",
                 (int)sm.GetPendingSpawnCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Templates: %d total, %d char, %d factory",
                 (int)sm.GetTemplateCount(),
                 (int)sm.GetCharacterTemplateCount(),
                 (int)sm.GetFactoryTemplateCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  In-place spawns: %d", entity_hooks::GetInPlaceSpawnCount());
        r += buf;

        // ES: Puerta de spawn (LoadingOrchestrator).
        // ── Spawn Gate ──
        r += "\n=== SPAWN GATE ===";
        const char* phaseName = "?";
        switch (orch.GetPhase()) {
            case LoadingPhase::Idle: phaseName = "Idle"; break;
            case LoadingPhase::InitialLoad: phaseName = "InitialLoad"; break;
            case LoadingPhase::ZoneTransition: phaseName = "ZoneTransition"; break;
            case LoadingPhase::SpawnLoad: phaseName = "SpawnLoad"; break;
        }
        snprintf(buf, sizeof(buf), "\n  Phase: %s  |  GameLoaded: %s  |  Burst: %s  |  CanSpawn: %s",
                 phaseName,
                 orch.IsGameLoaded() ? "Y" : "N",
                 orch.IsInBurst() ? "Y" : "N",
                 AssetFacilitator::Get().CanSpawn() ? "Y" : "N");
        r += buf;
        std::string blockReason = orch.GetSpawnBlockReason();
        snprintf(buf, sizeof(buf), "\n  Block reason: %s", blockReason.c_str());
        r += buf;

        // ES: Hooks: activos, con prólogo mov rax,rsp y los que han crasheado.
        // ── Hooks ──
        r += "\n=== HOOKS ===";
        auto diags = HookManager::Get().GetDiagnostics();
        int active = 0, movrax = 0;
        for (auto& d : diags) {
            if (d.enabled) active++;
            if (d.prologue[0] == 0x48 && d.prologue[1] == 0x8B && d.prologue[2] == 0xC4) movrax++;
        }
        snprintf(buf, sizeof(buf), "\n  Total: %d  |  Active: %d  |  MovRaxRsp: %d",
                 (int)diags.size(), active, movrax);
        r += buf;

        // ES: Mostrar los hooks que han crasheado alguna vez.
        // Show any crashed hooks
        for (auto& d : diags) {
            if (d.crashCount > 0) {
                snprintf(buf, sizeof(buf), "\n  CRASHED: %s (crashes: %d)", d.name.c_str(), d.crashCount);
                r += buf;
            }
        }

        // ES: Personaje principal. Ojo: lee la posición en +0x48/+0x4C/+0x50 y la facción en
        //     +0x10 con offsets fijos en lugar de la tabla GetOffsets() (hoy coinciden).
        // EN: Primary character. Note: reads position at +0x48/+0x4C/+0x50 and faction at
        //     +0x10 with hardcoded offsets instead of the GetOffsets() table (they match today).
        // ── Primary Character ──
        r += "\n=== PRIMARY CHAR ===";
        void* primary = core.GetPlayerController().GetPrimaryCharacter();
        if (primary) {
            uintptr_t p = reinterpret_cast<uintptr_t>(primary);
            float px = 0, py = 0, pz = 0;
            uintptr_t fac = 0;
            Memory::Read(p + 0x48, px); Memory::Read(p + 0x4C, py); Memory::Read(p + 0x50, pz);
            Memory::Read(p + 0x10, fac);
            snprintf(buf, sizeof(buf), "\n  Addr: 0x%llX  Pos: (%.0f, %.0f, %.0f)  Faction: 0x%llX",
                     (unsigned long long)p, px, py, pz, (unsigned long long)fac);
            r += buf;
        } else {
            r += "\n  (none)";
        }

        r += "\n--- Use /forcespawn to bypass gates ---";
        return r;
    });

    // ES: /instance — lanza una segunda instancia de Kenshi para probar el multijugador
    //     en el mismo PC (primero con -nosteam para saltar el bloqueo de instancia única).
    // /instance — Launch a second Kenshi instance for testing multiplayer
    Register("instance", "Launch a second Kenshi instance (bypasses Steam single-instance lock)", [](const CommandArgs&) -> std::string {
        // ES: Ruta del ejecutable del proceso actual (kenshi_x64.exe).
        // Find kenshi_x64.exe relative to our DLL
        char modulePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, modulePath, MAX_PATH);

        // ES: Lanzar con -nosteam para saltar la comprobación de instancia única de Steam.
        // Launch with -nosteam flag to bypass Steam's single-instance check
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};

        std::string cmdLine = std::string("\"") + modulePath + "\" -nosteam";
        if (CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                           0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return "Launched second Kenshi instance (PID: " + std::to_string(pi.dwProcessId) + ")";
        }

        // ES: Alternativa: lanzar sin -nosteam.
        // Fallback: try without -nosteam
        cmdLine = std::string("\"") + modulePath + "\"";
        if (CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                           0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return "Launched second Kenshi instance (PID: " + std::to_string(pi.dwProcessId) + ")";
        }

        return "Failed to launch Kenshi: " + std::to_string(GetLastError());
    });

    // ES: /syncstatus — estado de la sincronización de partida compartida (shared-save):
    //     si se han encontrado el personaje propio y el del otro jugador.
    // /syncstatus — Show shared-save sync status
    Register("syncstatus", "Show shared-save sync status", [](const CommandArgs&) -> std::string {
        std::string r = "=== SHARED-SAVE SYNC ===";
        r += "\nOwn character: " + shared_save_sync::GetOwnCharacterName();
        r += " — " + std::string(shared_save_sync::IsOwnCharacterFound() ? "FOUND" : "searching...");
        r += "\nOther character: " + shared_save_sync::GetOtherCharacterName();
        r += " — " + std::string(shared_save_sync::IsOtherCharacterFound() ? "FOUND" : "searching...");
        r += "\nTracked characters: " + std::to_string(char_tracker_hooks::GetTrackedCount());
        return r;
    });

    // ES: /ready — marcarse como listo en el lobby (envía C2S_LobbyReady; la partida
    //     empieza cuando todos están listos).
    // /ready — Mark as ready in lobby (triggers game start when all ready)
    Register("ready", "Mark as ready in lobby", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected to a server.";
        if (!core.GetLobbyManager().HasFaction()) return "No faction assigned yet — wait for server.";

        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_LobbyReady);
        core.GetClient().SendReliable(writer.Data(), writer.Size());

        int slot = core.GetLobbyManager().GetPlayerSlot();
        return "Ready! You are Player " + std::to_string(slot) + ". Waiting for other players...";
    });

    // ES: /claim — busca a mano los personajes del mod ("Player 1-16") y reclama los tuyos.
    // /claim — Manually scan for mod characters and claim yours
    Register("claim", "Scan for mod characters (Player 1-16) and claim yours", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected to a server.";
        if (!core.GetLobbyManager().HasFaction()) return "No faction assigned — connect first.";

        core.FindAndClaimModCharacters();

        auto localCount = core.GetEntityRegistry().GetPlayerEntities(core.GetLocalPlayerId()).size();
        if (localCount > 0) {
            return "Claimed " + std::to_string(localCount) + " character(s) as Player " +
                   std::to_string(core.GetLobbyManager().GetPlayerSlot());
        }
        return "No mod characters found — is kenshi-online.mod active?";
    });

    // ES: Log con el número de comandos registrados.
    // EN: Log the number of registered commands.
    spdlog::info("CommandRegistry: {} built-in commands registered", GetAll().size());
}

} // namespace kmp
