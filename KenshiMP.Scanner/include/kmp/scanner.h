// ES: scanner.h — escáner de patrones AOB "clásico" (PatternScanner).
//     Un patrón AOB (array of bytes) es una firma de bytes con comodines '?' que se
//     busca en memoria para localizar una función del juego sin depender de una
//     dirección fija (las direcciones cambian entre versiones Steam/GOG).
//     Por defecto busca solo en la sección .text de kenshi_x64.exe. También ofrece
//     utilidades para resolver direcciones relativas a RIP y destinos de CALL/JMP.
//     Lo usa el camino B (ResolveGameFunctions en patterns.cpp).
// EN: scanner.h — "classic" AOB pattern scanner (PatternScanner).
//     An AOB (array of bytes) pattern is a byte signature with '?' wildcards that is
//     searched in memory to locate a game function without relying on a fixed
//     address (addresses change between Steam/GOG versions).
//     By default it only scans the .text section of kenshi_x64.exe. It also provides
//     helpers to resolve RIP-relative addresses and CALL/JMP targets.
//     Used by path B (ResolveGameFunctions in patterns.cpp).
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <optional>

namespace kmp {

// ES: Resultado de una búsqueda: dirección encontrada y si es válida.
//     Convertible a bool (¿se encontró?) y a uintptr_t (la dirección).
// EN: Result of a search: found address and whether it is valid.
//     Convertible to bool (found?) and to uintptr_t (the address).
struct PatternResult {
    uintptr_t address = 0;
    bool      valid   = false;

    operator bool() const { return valid; }
    operator uintptr_t() const { return address; }
};

// ES: Escáner lineal de patrones AOB sobre un módulo cargado en memoria.
// EN: Linear AOB pattern scanner over a module loaded in memory.
class PatternScanner {
public:
    // ES: Inicializa el escáner para un módulo cargado (nullptr = ejecutable principal).
    //     Lee las cabeceras PE para conocer el tamaño y localizar .text.
    // Initialize scanner for a loaded module (nullptr = main executable)
    bool Init(const char* moduleName = nullptr);

    // ES: Busca un patrón estilo IDA, p.ej. "48 89 5C 24 ? 57 48 83 EC 20".
    //     '?' o '??' = byte comodín. Devuelve la primera coincidencia.
    // Scan for an IDA-style byte pattern
    // Pattern format: "48 89 5C 24 ? 57 48 83 EC 20"
    // '?' or '??' = wildcard byte
    PatternResult Find(const char* pattern) const;

    // ES: Igual que Find pero suma 'offset' a la dirección de la coincidencia.
    // Find with offset from match start
    PatternResult Find(const char* pattern, int offset) const;

    // ES: Devuelve todas las coincidencias (útil para comprobar que un patrón es único).
    // Find all occurrences
    std::vector<uintptr_t> FindAll(const char* pattern) const;

    // ES: Resuelve una dirección relativa a RIP: dirección de la instrucción + longitud
    //     de la instrucción + desplazamiento int32 leído en 'operandOffset'.
    // Resolve a RIP-relative address
    // Example: LEA RAX, [RIP+0x12345] at address X with operand at offset 3, instruction length 7
    // Returns: X + 7 + *(int32_t*)(X + 3)
    static uintptr_t ResolveRIP(uintptr_t instructionAddr, int operandOffset, int instructionLength);

    // ES: Sigue una instrucción CALL rel32 (E8) y devuelve la función destino (0 si no es E8).
    // Follow a CALL instruction (E8 xx xx xx xx) to get the target
    static uintptr_t FollowCall(uintptr_t callAddr);

    // ES: Sigue un JMP rel32 (E9) y devuelve el destino (0 si no es E9).
    // Follow a JMP instruction (E9 xx xx xx xx) to get the target
    static uintptr_t FollowJmp(uintptr_t jmpAddr);

    // ES: Base y tamaño del módulo escaneado.
    // Get module info
    uintptr_t GetBase() const { return m_moduleBase; }
    size_t    GetSize() const { return m_moduleSize; }

    // ES: Limita el escaneo a .text (código) para ir más rápido; si es false escanea el módulo entero.
    // Restrict scanning to .text section for speed
    void SetTextOnly(bool textOnly) { m_textOnly = textOnly; }

private:
    // ES: Estado interno: base/tamaño del módulo y de la sección .text.
    // EN: Internal state: module and .text section base/size.
    uintptr_t m_moduleBase = 0;
    size_t    m_moduleSize = 0;
    uintptr_t m_textBase   = 0;
    size_t    m_textSize   = 0;
    bool      m_textOnly   = true;

    // ES: Patrón ya parseado: bytes y máscara (true = byte fijo, false = comodín).
    // EN: Parsed pattern: bytes and mask (true = fixed byte, false = wildcard).
    struct ParsedPattern {
        std::vector<uint8_t> bytes;
        std::vector<bool>    mask; // true = must match, false = wildcard
    };

    // ES: Convierte el texto del patrón en bytes + máscara; nullopt si está vacío.
    // EN: Turns the pattern text into bytes + mask; nullopt if empty.
    static std::optional<ParsedPattern> Parse(const char* pattern);

    // ES: Busca el patrón en una región [start, start+size) y devuelve la primera coincidencia o 0.
    // EN: Searches the pattern in region [start, start+size) and returns the first match or 0.
    uintptr_t ScanRegion(uintptr_t start, size_t size,
                         const ParsedPattern& pattern) const;

    // ES: Localiza la sección .text en las cabeceras PE (si no la encuentra usa el módulo entero).
    // EN: Locates the .text section in the PE headers (falls back to the whole module).
    bool FindTextSection();
};

} // namespace kmp
