// ES: scanner.cpp — implementación del PatternScanner clásico (ver kmp/scanner.h).
//     Escaneo byte a byte (sin SIMD); el motor rápido SSE2 está en scanner_engine.cpp.
// EN: scanner.cpp — implementation of the classic PatternScanner (see kmp/scanner.h).
//     Byte-by-byte scanning (no SIMD); the fast SSE2 engine lives in scanner_engine.cpp.
#include "kmp/scanner.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <cctype>
#include <sstream>

namespace kmp {

// ES: Inicializa: obtiene el HMODULE, valida las firmas DOS ("MZ") y NT ("PE"),
//     guarda SizeOfImage y busca la sección .text.
// EN: Init: gets the HMODULE, validates the DOS ("MZ") and NT ("PE") signatures,
//     stores SizeOfImage and looks up the .text section.
bool PatternScanner::Init(const char* moduleName) {
    HMODULE hModule = moduleName ? GetModuleHandleA(moduleName) : GetModuleHandleA(nullptr);
    if (!hModule) {
        spdlog::error("PatternScanner: Failed to get module handle for '{}'",
                      moduleName ? moduleName : "main");
        return false;
    }

    m_moduleBase = reinterpret_cast<uintptr_t>(hModule);

    // Parse PE headers to get module size
    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(m_moduleBase);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        spdlog::error("PatternScanner: Invalid DOS signature");
        return false;
    }

    auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(m_moduleBase + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        spdlog::error("PatternScanner: Invalid NT signature");
        return false;
    }

    m_moduleSize = ntHeaders->OptionalHeader.SizeOfImage;

    FindTextSection();

    spdlog::info("PatternScanner: Module '{}' at 0x{:X}, size 0x{:X}, .text at 0x{:X} size 0x{:X}",
                 moduleName ? moduleName : "main",
                 m_moduleBase, m_moduleSize, m_textBase, m_textSize);

    return true;
}

// ES: Recorre la tabla de secciones del PE buscando ".text". Si no existe,
//     usa el módulo completo como región de escaneo y devuelve false.
// EN: Walks the PE section table looking for ".text". If missing, uses the
//     whole module as scan region and returns false.
bool PatternScanner::FindTextSection() {
    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(m_moduleBase);
    auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(m_moduleBase + dosHeader->e_lfanew);

    auto* section = IMAGE_FIRST_SECTION(ntHeaders);
    for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++, section++) {
        if (std::memcmp(section->Name, ".text", 5) == 0) {
            m_textBase = m_moduleBase + section->VirtualAddress;
            m_textSize = section->Misc.VirtualSize;
            return true;
        }
    }

    // Fallback: use entire module
    m_textBase = m_moduleBase;
    m_textSize = m_moduleSize;
    return false;
}

// ES: Parsea el patrón token a token: "?"/"??" = comodín, lo demás = byte hex fijo.
// EN: Parses the pattern token by token: "?"/"??" = wildcard, anything else = fixed hex byte.
std::optional<PatternScanner::ParsedPattern> PatternScanner::Parse(const char* pattern) {
    ParsedPattern result;
    std::istringstream stream(pattern);
    std::string token;

    while (stream >> token) {
        if (token == "?" || token == "??") {
            result.bytes.push_back(0);
            result.mask.push_back(false);
        } else {
            unsigned long byte = std::strtoul(token.c_str(), nullptr, 16);
            result.bytes.push_back(static_cast<uint8_t>(byte));
            result.mask.push_back(true);
        }
    }

    if (result.bytes.empty()) return std::nullopt;
    return result;
}

// ES: Búsqueda lineal: filtra rápido por el primer byte (si no es comodín) y luego
//     compara el resto respetando la máscara. Devuelve la primera coincidencia o 0.
// EN: Linear search: fast filter on the first byte (if not a wildcard), then
//     compares the rest honoring the mask. Returns the first match or 0.
uintptr_t PatternScanner::ScanRegion(uintptr_t start, size_t size,
                                     const ParsedPattern& pattern) const {
    if (pattern.bytes.empty() || size < pattern.bytes.size()) return 0;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(start);
    const size_t patternLen = pattern.bytes.size();
    const size_t scanEnd = size - patternLen;

    const uint8_t firstByte = pattern.bytes[0];
    const bool firstIsWild = !pattern.mask[0];

    for (size_t i = 0; i <= scanEnd; i++) {
        if (!firstIsWild && data[i] != firstByte) continue;

        bool found = true;
        for (size_t j = 1; j < patternLen; j++) {
            if (pattern.mask[j] && data[i + j] != pattern.bytes[j]) {
                found = false;
                break;
            }
        }

        if (found) {
            return start + i;
        }
    }

    return 0;
}

// ES: Find sin offset: delega en la sobrecarga con offset 0.
// EN: Find without offset: delegates to the overload with offset 0.
PatternResult PatternScanner::Find(const char* pattern) const {
    return Find(pattern, 0);
}

// ES: Parsea el patrón, escanea .text (o el módulo entero) y devuelve la
//     dirección de la coincidencia + offset. Loguea si no lo encuentra.
// EN: Parses the pattern, scans .text (or the whole module) and returns the
//     match address + offset. Logs when not found.
PatternResult PatternScanner::Find(const char* pattern, int offset) const {
    auto parsed = Parse(pattern);
    if (!parsed) {
        spdlog::error("PatternScanner: Failed to parse pattern '{}'", pattern);
        return {};
    }

    uintptr_t scanStart = m_textOnly ? m_textBase : m_moduleBase;
    size_t scanSize = m_textOnly ? m_textSize : m_moduleSize;

    uintptr_t addr = ScanRegion(scanStart, scanSize, *parsed);
    if (addr == 0) {
        spdlog::warn("PatternScanner: Pattern not found: '{}'", pattern);
        return {};
    }

    PatternResult result;
    result.address = addr + offset;
    result.valid = true;

    spdlog::info("PatternScanner: Found pattern at 0x{:X} (offset {} -> 0x{:X})",
                 addr, offset, result.address);
    return result;
}

// ES: Encuentra todas las coincidencias avanzando un byte tras cada hallazgo.
// EN: Finds all matches, advancing one byte after each hit.
std::vector<uintptr_t> PatternScanner::FindAll(const char* pattern) const {
    auto parsed = Parse(pattern);
    if (!parsed) return {};

    std::vector<uintptr_t> results;

    uintptr_t scanStart = m_textOnly ? m_textBase : m_moduleBase;
    size_t scanSize = m_textOnly ? m_textSize : m_moduleSize;

    uintptr_t current = scanStart;
    size_t remaining = scanSize;

    while (remaining > parsed->bytes.size()) {
        uintptr_t addr = ScanRegion(current, remaining, *parsed);
        if (addr == 0) break;

        results.push_back(addr);

        size_t advance = (addr - current) + 1;
        current += advance;
        remaining -= advance;
    }

    spdlog::info("PatternScanner: Found {} occurrences of pattern", results.size());
    return results;
}

// ES: destino = instrucción + longitud + rel32 (direccionamiento relativo a RIP de x64).
// EN: target = instruction + length + rel32 (x64 RIP-relative addressing).
uintptr_t PatternScanner::ResolveRIP(uintptr_t instructionAddr, int operandOffset,
                                     int instructionLength) {
    int32_t relative;
    std::memcpy(&relative, reinterpret_cast<void*>(instructionAddr + operandOffset), sizeof(int32_t));
    return instructionAddr + instructionLength + relative;
}

// ES: CALL rel32: opcode E8, operando en +1, longitud 5.
// EN: CALL rel32: opcode E8, operand at +1, length 5.
uintptr_t PatternScanner::FollowCall(uintptr_t callAddr) {
    uint8_t opcode = *reinterpret_cast<uint8_t*>(callAddr);
    if (opcode != 0xE8) return 0;
    return ResolveRIP(callAddr, 1, 5);
}

// ES: JMP rel32: opcode E9, operando en +1, longitud 5.
// EN: JMP rel32: opcode E9, operand at +1, length 5.
uintptr_t PatternScanner::FollowJmp(uintptr_t jmpAddr) {
    uint8_t opcode = *reinterpret_cast<uint8_t*>(jmpAddr);
    if (opcode != 0xE9) return 0;
    return ResolveRIP(jmpAddr, 1, 5);
}

} // namespace kmp
