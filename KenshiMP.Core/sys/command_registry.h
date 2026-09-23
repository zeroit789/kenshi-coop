// ES: Registro de comandos de chat/consola del cliente (los que empiezan por '/').
//     Define los argumentos parseados, la definición de cada comando y el
//     registro singleton que los guarda y los ejecuta.
// EN: Client chat/console command registry (commands starting with '/').
//     Defines the parsed arguments, each command's definition and the
//     singleton registry that stores and executes them.
#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace kmp {

// ES: Entrada de un comando ya troceada en tokens separados por espacios.
// EN: A command input already split into whitespace-separated tokens.
struct CommandArgs {
    std::string              raw;     // Full input (e.g., "/tp Bob")
    std::string              command; // First token without / (e.g., "tp")
    std::vector<std::string> args;    // Remaining tokens (e.g., ["Bob"])
};

// ES: Definición de un comando: nombre, descripción (para /help) y la función
//     que lo ejecuta y devuelve el texto de respuesta para el chat.
// EN: A command definition: name, description (for /help) and the handler
//     that runs it and returns the reply text for the chat.
struct CommandDef {
    std::string name;
    std::string description;
    std::function<std::string(const CommandArgs&)> handler;
};

// ES: Registro global (singleton) de comandos. Protegido con un mutex recursivo
//     porque un comando puede llamar a GetAll() (p.ej. /help) mientras se ejecuta.
// EN: Global (singleton) command registry. Guarded by a recursive mutex
//     because a command may call GetAll() (e.g. /help) while it is running.
class CommandRegistry {
public:
    // ES: Devuelve la instancia única del registro.
    // EN: Returns the single registry instance.
    static CommandRegistry& Get();

    // ES: Registra (o sustituye) un comando por nombre, sin la barra '/'.
    // EN: Registers (or replaces) a command by name, without the '/' slash.
    void Register(const std::string& name, const std::string& desc,
                  std::function<std::string(const CommandArgs&)> handler);

    // ES: Quita la '/', busca el comando, llama a su handler y devuelve el texto
    //     resultante. Devuelve cadena vacía si la entrada no empieza por '/'.
    // EN:
    // Parse "/" prefix, lookup command, call handler, return result string.
    // Returns empty string if input doesn't start with '/'.
    std::string Execute(const std::string& input);

    // ES: Devuelve todos los comandos registrados (para el listado de /help).
    // EN:
    // Get all registered commands (for /help listing)
    std::vector<const CommandDef*> GetAll() const;

    // ES: Registra todos los comandos integrados (se llama una vez en Core::Initialize).
    // EN:
    // Register all built-in commands (called once during Core::Initialize)
    void RegisterBuiltins();

// ES: Constructor privado: solo se accede mediante Get().
// EN: Private constructor: only reachable through Get().
private:
    CommandRegistry() = default;

    // ES: Mutex del mapa de comandos y el propio mapa nombre -> definición.
    // EN: Mutex for the command map and the map itself (name -> definition).
    mutable std::recursive_mutex m_mutex;
    std::unordered_map<std::string, CommandDef> m_commands;
};

} // namespace kmp
