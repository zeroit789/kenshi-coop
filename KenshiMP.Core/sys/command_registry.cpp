// ES: Implementación del registro de comandos '/': alta, parseo y ejecución.
// EN: Implementation of the '/' command registry: registration, parsing and execution.
#include "command_registry.h"
#include <sstream>
#include <spdlog/spdlog.h>

namespace kmp {

// ES: Singleton de Meyers (inicialización perezosa y segura entre hilos).
// EN: Meyers singleton (lazy, thread-safe initialization).
CommandRegistry& CommandRegistry::Get() {
    static CommandRegistry instance;
    return instance;
}

// ES: Guarda la definición del comando bajo su nombre (sobrescribe si ya existía).
// EN: Stores the command definition under its name (overwrites if it existed).
void CommandRegistry::Register(const std::string& name, const std::string& desc,
                               std::function<std::string(const CommandArgs&)> handler) {
    std::lock_guard lock(m_mutex);
    CommandDef def;
    def.name = name;
    def.description = desc;
    def.handler = std::move(handler);
    m_commands[name] = std::move(def);
    spdlog::debug("CommandRegistry: Registered /{}", name);
}

// ES: Parsea la entrada "/cmd arg1 arg2", busca el comando y lo ejecuta.
//     Las excepciones C++ del handler se capturan y se devuelven como texto de error.
// EN: Parses the "/cmd arg1 arg2" input, looks up the command and runs it.
//     C++ exceptions thrown by the handler are caught and returned as error text.
std::string CommandRegistry::Execute(const std::string& input) {
    if (input.empty() || input[0] != '/') return "";

    // ES: Parseo: saltar la '/' y trocear en tokens.
    // EN:
    // Parse: skip '/', split into tokens
    CommandArgs args;
    args.raw = input;

    std::istringstream iss(input.substr(1)); // Skip '/'
    std::string token;

    if (!(iss >> args.command)) return "Empty command.";

    while (iss >> token) {
        args.args.push_back(token);
    }

    // ES: Búsqueda del comando en el mapa (bajo el mutex).
    // EN:
    // Lookup
    std::lock_guard lock(m_mutex);
    auto it = m_commands.find(args.command);
    if (it == m_commands.end()) {
        return "Unknown command: /" + args.command + ". Type /help for commands.";
    }

    // ES: Ejecutar el handler (el mutex sigue tomado; es recursivo).
    // EN:
    // Execute handler
    try {
        return it->second.handler(args);
    } catch (const std::exception& e) {
        spdlog::error("CommandRegistry: Exception in /{}: {}", args.command, e.what());
        return "Command error: " + std::string(e.what());
    }
}

// ES: Copia los punteros a todas las definiciones (válidos mientras no se re-registren).
// EN: Copies pointers to all definitions (valid as long as nothing is re-registered).
std::vector<const CommandDef*> CommandRegistry::GetAll() const {
    std::lock_guard lock(m_mutex);
    std::vector<const CommandDef*> result;
    result.reserve(m_commands.size());
    for (auto& [name, def] : m_commands) {
        result.push_back(&def);
    }
    return result;
}

} // namespace kmp
