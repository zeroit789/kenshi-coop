// ES: main.cpp - Punto de entrada de KenshiMP.Server.exe (servidor dedicado autoritativo).
//     Configura el log (consola + KenshiOnline_Server.log), carga server.json (o el que se pase
//     como argv[1]; si no existe escribe uno con valores por defecto), arranca GameServer,
//     restaura el mundo guardado, lanza un hilo que lee comandos de consola por stdin y entra
//     en el bucle principal de tick (~20 Hz por defecto). Al salir guarda el mundo y apaga.
// EN: main.cpp - Entry point of KenshiMP.Server.exe (authoritative dedicated server).
//     Sets up logging (console + KenshiOnline_Server.log), loads server.json (or argv[1]; if it
//     does not exist a default one is written), starts GameServer, restores the saved world,
//     spawns a thread that reads console commands from stdin and enters the main tick loop
//     (~20 Hz by default). On exit it saves the world and shuts down.
#include "server.h"
#include "kmp/config.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <csignal>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <Windows.h>
#endif

// ES: Bandera global de "seguir funcionando". La ponen a false Ctrl+C/SIGTERM o el comando 'stop';
//     la leen el bucle principal y el hilo de consola.
// EN: Global "keep running" flag. Cleared by Ctrl+C/SIGTERM or the 'stop' command;
//     read by the main loop and the console thread.
static std::atomic<bool> g_running{true};

// ES: Manejador de SIGINT/SIGTERM: solo registra la señal y pide el apagado ordenado.
// EN: SIGINT/SIGTERM handler: just logs the signal and requests an orderly shutdown.
void SignalHandler(int signal) {
    spdlog::info("Received signal {}, shutting down...", signal);
    g_running = false;
}

// ES: argv[1] opcional = ruta del fichero de configuración (por defecto "server.json").
//     Devuelve 1 si el servidor no puede arrancar (p. ej. puerto ocupado), 0 al cerrar bien.
// EN: Optional argv[1] = config file path (default "server.json").
//     Returns 1 if the server fails to start (e.g. port in use), 0 on a clean shutdown.
int main(int argc, char* argv[]) {
#ifdef _WIN32
    // ES: Activa UTF-8 en la consola para que los nombres no latinos (chino, japonés, coreano) se vean bien en los logs.
    // EN: Enable UTF-8 for console I/O so non-Latin characters (Chinese, Japanese, Korean)
    // display correctly in server logs and console output.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // ES: Log doble: consola con colores y fichero KenshiOnline_Server.log (se trunca en cada arranque).
    // EN: Setup logging
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("KenshiOnline_Server.log", true);
    auto logger = std::make_shared<spdlog::logger>("server",
        spdlog::sinks_init_list{console_sink, file_sink});
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);

    spdlog::info("+======================================+");
    spdlog::info("|    Kenshi-Online Dedicated Server     |");
    spdlog::info("|         v0.1.0 - Up to 16 Players    |");
    spdlog::info("+======================================+");

    // ES: Carga la configuración; si no existe se guarda la configuración por defecto para que el usuario la edite.
    // EN: Load server config
    kmp::ServerConfig config;
    std::string configPath = "server.json";
    if (argc > 1) configPath = argv[1];

    if (config.Load(configPath)) {
        spdlog::info("Loaded config from: {}", configPath);
    } else {
        spdlog::info("No config found at '{}', using defaults", configPath);
        config.Save(configPath);
        spdlog::info("Default config saved to: {}", configPath);
    }

    // ES: Registra los manejadores de señales para cerrar limpiamente con Ctrl+C.
    // EN: Signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // ES: Crea y arranca el servidor (UPnP/firewall, host ENet, baneos, facciones, master server).
    // EN: Create and start server
    kmp::GameServer server;
    // Informar al server de la ruta de config para poder persistir cambios en caliente
    // (p.ej. el comando 'factionmode' reescribe server.json).
    server.SetConfigPath(configPath);
    if (!server.Start(config)) {
        spdlog::error("Failed to start server!");
        return 1;
    }

    // ES: Intenta restaurar el mundo guardado (world.kmpsave por defecto).
    // EN: Try to load saved world state
    server.LoadWorld();

    spdlog::info("Server '{}' started on port {} (max {} players)",
                 config.serverName, config.port, config.maxPlayers);
    spdlog::info("PvP: {} | Game Speed: {}x | Tick Rate: {} Hz",
                 config.pvpEnabled ? "Enabled" : "Disabled",
                 config.gameSpeed, config.tickRate);
    // ES: OJO: este texto está fijo; el intervalo real es GameServer::m_autoSaveInterval (también 60 s hoy).
    // EN: NOTE: this text is hard-coded; the real interval is GameServer::m_autoSaveInterval (also 60 s today).
    spdlog::info("Auto-save: every 60 seconds");
    spdlog::info("Type 'help' for commands, 'stop' to shutdown");

    // ES: Hilo de comandos de consola: lee líneas de stdin y llama a métodos públicos de GameServer,
    //     que toman el mutex del servidor (m_mutex) para no chocar con el hilo del tick.
    //     Comandos: help, status, players, save, say, kick, speed, pause, resume, factionmode,
    //     setfaction, factions y stop/quit/exit.
    // EN: Console command thread — checks g_running before every server access
    std::thread consoleThread([&server]() {
        std::string line;
        while (g_running && std::getline(std::cin, line)) {
            if (!g_running) break; // Re-check after blocking getline returns
            if (line == "stop" || line == "quit" || line == "exit") {
                g_running = false;
                break;
            } else if (line == "help") {
                std::cout << "Commands:" << std::endl;
                std::cout << "  status  - Show server status" << std::endl;
                std::cout << "  players - List connected players" << std::endl;
                std::cout << "  kick <id> - Kick a player" << std::endl;
                std::cout << "  say <msg> - Broadcast system message" << std::endl;
                std::cout << "  speed <v> - Set global game speed (0-10, e.g. 'speed 2')" << std::endl;
                std::cout << "  pause   - Pause the global world (sends speed=0 to clients)" << std::endl;
                std::cout << "  resume  - Resume the global world at configured speed" << std::endl;
                std::cout << "  factionmode <single|teams|per-player> - Change faction assignment mode (persists)" << std::endl;
                std::cout << "  setfaction <playerId> <slot> - Assign a faction slot (1-6) to a connected player" << std::endl;
                std::cout << "  factions - List faction slots from manifest and who owns each" << std::endl;
                std::cout << "  save    - Save world state" << std::endl;
                std::cout << "  stop    - Shutdown server" << std::endl;
            } else if (line == "status") {
                server.PrintStatus();
            } else if (line == "players") {
                server.PrintPlayers();
            } else if (line == "save") {
                server.SaveWorld();
            } else if (line.substr(0, 4) == "say ") {
                server.BroadcastSystemMessage(line.substr(4));
            } else if (line.substr(0, 5) == "kick ") {
                try {
                    uint32_t id = std::stoul(line.substr(5));
                    server.KickPlayer(id, "Kicked by admin");
                } catch (...) {
                    std::cout << "Usage: kick <player_id>" << std::endl;
                }
            } else if (line.substr(0, 6) == "speed ") {
                // ES: Velocidad global autoritativa: la consola del server (host=admin) la fija.
                // Se broadcastea a todos los clientes vía TimeSync.
                // EN: Authoritative global speed: the server console (host = admin) sets it and it is broadcast via TimeSync.
                try {
                    float speed = std::stof(line.substr(6));
                    if (server.SetGameSpeed(speed)) {
                        std::cout << "Global game speed set to " << speed << "x" << std::endl;
                    } else {
                        std::cout << "Invalid speed. Range is 0-10 (e.g. 'speed 2')" << std::endl;
                    }
                } catch (...) {
                    std::cout << "Usage: speed <value>  (0-10, e.g. 'speed 2' or 'speed 0.25')" << std::endl;
                }
            } else if (line == "pause") {
                // ES: Pausa global: el server envía speed=0 a todos los clientes.
                // EN: Global pause: the server sends speed=0 to every client.
                server.PauseWorld();
                std::cout << "World PAUSED (global)." << std::endl;
            } else if (line == "resume") {
                // ES: Reanuda el mundo a la velocidad global configurada.
                // EN: Resumes the world at the configured global speed.
                server.ResumeWorld();
                std::cout << "World RESUMED at " << server.GetGameSpeed() << "x." << std::endl;
            } else if (line.substr(0, 12) == "factionmode ") {
                // ES: Cambia el modo de facciones en caliente (single/teams/per-player) y lo persiste.
                // EN: Changes the faction mode at runtime (single/teams/per-player) and persists it.
                std::string mode = line.substr(12);
                // ES: Trim de espacios sobrantes al final por si el usuario teclea de más.
                // EN: Trim trailing spaces/CR in case the user typed extra characters.
                while (!mode.empty() && (mode.back() == ' ' || mode.back() == '\r'))
                    mode.pop_back();
                if (server.SetFactionMode(mode)) {
                    std::cout << "Faction mode set to '" << mode << "' (saved to config)." << std::endl;
                } else {
                    std::cout << "Invalid mode. Use: single | teams | per-player" << std::endl;
                }
            } else if (line.substr(0, 11) == "setfaction ") {
                // ES: setfaction <playerId> <slot1-6>: asignación manual de facción a un jugador.
                // EN: setfaction <playerId> <slot1-6>: manually assign a faction to a player.
                try {
                    std::string args = line.substr(11);
                    size_t sp = args.find(' ');
                    if (sp == std::string::npos) throw std::invalid_argument("falta slot");
                    uint32_t pid  = std::stoul(args.substr(0, sp));
                    int      slot = std::stoi(args.substr(sp + 1));
                    if (server.SetPlayerFaction(pid, slot)) {
                        std::cout << "Player " << pid << " assigned to faction slot " << slot << "." << std::endl;
                    } else {
                        std::cout << "Failed: check that player " << pid
                                  << " is connected and slot is in range." << std::endl;
                    }
                } catch (...) {
                    std::cout << "Usage: setfaction <playerId> <slot>  (slot 1-6, e.g. 'setfaction 2 3')" << std::endl;
                }
            } else if (line == "factions") {
                // ES: Lista las facciones del manifiesto y qué jugador tiene cada una.
                // EN: Lists the manifest factions and which player holds each one.
                server.PrintFactions();
            } else if (!line.empty()) {
                std::cout << "Unknown command. Type 'help' for commands." << std::endl;
            }
        }
    });

    // ES: Bucle principal: cada tickIntervalMs (1000 / tickRate; 50 ms con 20 Hz) llama a
    //     server.Update con el tiempo real transcurrido. Duerme 1 ms entre comprobaciones para
    //     no quemar CPU. Si tickRate <= 0 se usa 50 ms.
    // EN: Main loop: every tickIntervalMs (1000 / tickRate; 50 ms at 20 Hz) calls server.Update
    //     with the real elapsed time. Sleeps 1 ms between checks to avoid burning CPU.
    //     If tickRate <= 0, 50 ms is used.
    // Main server loop
    auto lastTick = std::chrono::steady_clock::now();
    int tickIntervalMs = (config.tickRate > 0) ? (1000 / config.tickRate) : 50;

    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick);

        if (elapsed.count() >= tickIntervalMs) {
            float deltaTime = elapsed.count() / 1000.f;
            lastTick = now;

            server.Update(deltaTime);
        }

        // ES: Pequeña espera para no hacer espera activa.
        // EN: Sleep a bit to avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // ES: Apagado: guardar el mundo y parar el servidor (desregistro del master, quitar UPnP, desconectar jugadores).
    // EN: Shutdown: save the world and stop the server (master deregister, remove UPnP, disconnect players).
    spdlog::info("Shutting down...");
    server.SaveWorld();
    server.Stop();

    // ES: El hilo de consola está bloqueado en stdin; se hace detach en vez de join para no colgar
    //     el cierre. Comprueba g_running y saldrá con la siguiente entrada o EOF.
    // EN: Console thread blocks on stdin. Detach it so we don't hang.
    // The thread checks g_running, so it will exit on next input or EOF.
    // We've already called Stop() so there's nothing left to access.
    if (consoleThread.joinable()) {
        consoleThread.detach();
    }

    spdlog::info("Server stopped.");
    spdlog::shutdown();
    return 0;
}
