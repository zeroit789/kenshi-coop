// ES: scanner_engine.cpp — implementación del motor de escaneo avanzado (ver
//     scanner_engine.h): enumeración de secciones PE, parseo de patrones AOB estilo IDA,
//     escaneo SSE2, patrones compuestos, escaneo por lotes y caché de resultados.
// EN: scanner_engine.cpp — advanced scanning engine implementation (see
//     scanner_engine.h): PE section enumeration, IDA-style AOB pattern parsing,
//     SSE2 scanning, composite patterns, batch scanning and result caching.
#include "kmp/scanner_engine.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <intrin.h>  // SSE2 intrinsics

namespace kmp {

// ES: Inicialización.
// ═══════════════════════════════════════════════════════════════════════════
//  INITIALIZATION
// ═══════════════════════════════════════════════════════════════════════════

// ES: Obtiene el HMODULE, valida cabeceras DOS/NT, guarda SizeOfImage, enumera las
//     secciones y las loguea con sus permisos (X/R/W).
// EN: Gets the HMODULE, validates DOS/NT headers, stores SizeOfImage, enumerates the
//     sections and logs them with their permissions (X/R/W).
bool ScannerEngine::Init(const char* moduleName) {
    HMODULE hModule = moduleName ? GetModuleHandleA(moduleName) : GetModuleHandleA(nullptr);
    if (!hModule) {
        spdlog::error("ScannerEngine: Failed to get module handle for '{}'",
                      moduleName ? moduleName : "main");
        return false;
    }

    m_moduleBase = reinterpret_cast<uintptr_t>(hModule);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m_moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(m_moduleBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    m_moduleSize = nt->OptionalHeader.SizeOfImage;

    if (!EnumerateSections()) return false;

    spdlog::info("ScannerEngine: Module '{}' at 0x{:X}, size 0x{:X}, {} sections",
                 moduleName ? moduleName : "main",
                 m_moduleBase, m_moduleSize, m_sections.size());

    for (const auto& sec : m_sections) {
        spdlog::info("  Section '{}': base=0x{:X} size=0x{:X} [{}{}{}]",
                     sec.name, sec.base, sec.size,
                     sec.IsExecutable() ? "X" : "-",
                     sec.IsReadable() ? "R" : "-",
                     sec.IsWritable() ? "W" : "-");
    }

    return true;
}

// ES: Copia la tabla de secciones del PE a m_sections (nombre, base, tamaño, flags).
// EN: Copies the PE section table into m_sections (name, base, size, flags).
bool ScannerEngine::EnumerateSections() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m_moduleBase);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(m_moduleBase + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);

    m_sections.clear();
    m_sections.reserve(nt->FileHeader.NumberOfSections);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        PESection sec;
        std::memcpy(sec.name, section->Name, 8);
        sec.name[8] = '\0';
        sec.base = m_moduleBase + section->VirtualAddress;
        sec.size = section->Misc.VirtualSize;
        sec.characteristics = section->Characteristics;
        m_sections.push_back(sec);
    }

    return !m_sections.empty();
}

// ES: Busca una sección por nombre exacto (".text", ".rdata"...).
// EN: Finds a section by exact name (".text", ".rdata"...).
const PESection* ScannerEngine::FindSection(const char* name) const {
    for (const auto& sec : m_sections) {
        if (std::strcmp(sec.name, name) == 0) return &sec;
    }
    return nullptr;
}

// ES: Parseo de patrones.
// ═══════════════════════════════════════════════════════════════════════════
//  PATTERN PARSING
// ═══════════════════════════════════════════════════════════════════════════

// ES: Convierte "48 8B ? C4" en bytes + máscara ('?'/'??' = comodín). Guarda el texto original.
// EN: Turns "48 8B ? C4" into bytes + mask ('?'/'??' = wildcard). Keeps the original text.
std::optional<ParsedPattern> ScannerEngine::Parse(const char* pattern) {
    ParsedPattern result;
    result.original = pattern;
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

// ES: Regiones de escaneo.
// ═══════════════════════════════════════════════════════════════════════════
//  SCAN REGIONS
// ═══════════════════════════════════════════════════════════════════════════

// ES: Devuelve las regiones a escanear: las secciones restringidas si las hay; si no,
//     solo .text (o el módulo entero si no existe .text).
// EN: Returns the regions to scan: the restricted sections if any; otherwise only
//     .text (or the whole module if .text is missing).
std::vector<ScannerEngine::ScanRegion> ScannerEngine::GetScanRegions() const {
    std::vector<ScanRegion> regions;

    if (!m_restrictSections.empty()) {
        for (const auto& name : m_restrictSections) {
            if (auto* sec = FindSection(name.c_str())) {
                regions.push_back({sec->base, sec->size, sec->name});
            }
        }
    } else {
        // Default: scan .text only
        if (auto* text = FindSection(".text")) {
            regions.push_back({text->base, text->size, ".text"});
        } else {
            // Fallback: scan entire module
            regions.push_back({m_moduleBase, m_moduleSize, "module"});
        }
    }

    return regions;
}

// ES: Restringe el escaneo a las secciones indicadas.
// EN: Restricts scanning to the given sections.
void ScannerEngine::SetScanSections(const std::vector<std::string>& sectionNames) {
    m_restrictSections = sectionNames;
}

// ES: Escaneo acelerado con SSE2.
// ═══════════════════════════════════════════════════════════════════════════
//  SSE2-ACCELERATED SCANNING
// ═══════════════════════════════════════════════════════════════════════════

// ES: Busca el primer byte fijo del patrón 16 bytes a la vez: _mm_cmpeq_epi8 compara
//     16 bytes con el byte buscado y _mm_movemask_epi8 da una máscara de bits con las
//     posiciones candidatas; cada candidata se verifica con el patrón completo.
//     El resto final se recorre de forma lineal. Devuelve la primera coincidencia o 0.
// EN: Searches the first fixed byte of the pattern 16 bytes at a time: _mm_cmpeq_epi8
//     compares 16 bytes with the needle and _mm_movemask_epi8 gives a bitmask of
//     candidate positions; each candidate is verified against the full pattern.
//     The tail is scanned linearly. Returns the first match or 0.
uintptr_t ScannerEngine::ScanRegionSSE2(uintptr_t start, size_t size,
                                         const ParsedPattern& pattern) const {
    if (pattern.bytes.empty() || size < pattern.bytes.size()) return 0;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(start);
    const size_t patLen = pattern.bytes.size();
    const size_t scanEnd = size - patLen;

    // ES: Primer byte no comodín (el que se acelera con SSE2).
    // Find first non-wildcard byte for SSE2 acceleration
    int firstFixed = -1;
    for (size_t i = 0; i < patLen; i++) {
        if (pattern.mask[i]) { firstFixed = static_cast<int>(i); break; }
    }

    if (firstFixed < 0) {
        // All wildcards — match at start
        return start;
    }

    const uint8_t needle = pattern.bytes[firstFixed];

    // ES: Replica el byte buscado en los 16 carriles del registro SSE.
    // SSE2: broadcast the first fixed byte to all 16 lanes
    __m128i needleVec = _mm_set1_epi8(static_cast<char>(needle));

    size_t i = 0;

    // ES: Pasada SSE2 de 16 en 16 bytes.
    // SSE2 pass: scan 16 bytes at a time looking for the first fixed byte
    const size_t simdEnd = (scanEnd > 16) ? scanEnd - 16 : 0;
    while (i <= simdEnd) {
        // Load 16 bytes from scan position (offset by firstFixed)
        __m128i chunk = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(data + i + firstFixed));

        // Compare: result has 0xFF where bytes match
        __m128i cmp = _mm_cmpeq_epi8(chunk, needleVec);

        // Collapse to bitmask — bit N is set if byte N matched
        int mask = _mm_movemask_epi8(cmp);

        while (mask != 0) {
            // Find lowest set bit
            unsigned long bitIdx;
            _BitScanForward(&bitIdx, mask);

            size_t candidate = i + bitIdx;
            if (candidate > scanEnd) return 0;

            // Full pattern verification
            bool match = true;
            for (size_t j = 0; j < patLen; j++) {
                if (pattern.mask[j] && data[candidate + j] != pattern.bytes[j]) {
                    match = false;
                    break;
                }
            }

            if (match) return start + candidate;

            // Clear this bit and continue
            mask &= mask - 1;
        }

        i += 16;
    }

    // ES: Recorrido lineal de los bytes restantes.
    // Linear fallback for remaining bytes
    for (; i <= scanEnd; i++) {
        if (data[i + firstFixed] != needle) continue;

        bool match = true;
        for (size_t j = 0; j < patLen; j++) {
            if (pattern.mask[j] && data[i + j] != pattern.bytes[j]) {
                match = false;
                break;
            }
        }
        if (match) return start + i;
    }

    return 0;
}

// ES: Escaneo lineal simple (sin SIMD), comparando primero el byte 0 si no es comodín.
// EN: Plain linear scan (no SIMD), checking byte 0 first when it is not a wildcard.
uintptr_t ScannerEngine::ScanRegionLinear(uintptr_t start, size_t size,
                                           const ParsedPattern& pattern) const {
    if (pattern.bytes.empty() || size < pattern.bytes.size()) return 0;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(start);
    const size_t patLen = pattern.bytes.size();
    const size_t scanEnd = size - patLen;

    const uint8_t firstByte = pattern.bytes[0];
    const bool firstIsWild = !pattern.mask[0];

    for (size_t i = 0; i <= scanEnd; i++) {
        if (!firstIsWild && data[i] != firstByte) continue;

        bool found = true;
        for (size_t j = 1; j < patLen; j++) {
            if (pattern.mask[j] && data[i + j] != pattern.bytes[j]) {
                found = false;
                break;
            }
        }
        if (found) return start + i;
    }

    return 0;
}

// ES: Todas las coincidencias de una región (SSE2 repetido avanzando 1 byte tras cada hallazgo).
// EN: All matches in a region (repeated SSE2 scan advancing 1 byte after each hit).
std::vector<uintptr_t> ScannerEngine::ScanRegionAll(uintptr_t start, size_t size,
                                                      const ParsedPattern& pattern) const {
    std::vector<uintptr_t> results;
    if (pattern.bytes.empty() || size < pattern.bytes.size()) return results;

    uintptr_t current = start;
    size_t remaining = size;

    while (remaining > pattern.bytes.size()) {
        uintptr_t addr = ScanRegionSSE2(current, remaining, pattern);
        if (addr == 0) break;

        results.push_back(addr);

        size_t advance = (addr - current) + 1;
        current += advance;
        remaining -= advance;
    }

    return results;
}

// ES: API de un solo patrón.
// ═══════════════════════════════════════════════════════════════════════════
//  SINGLE-PATTERN API
// ═══════════════════════════════════════════════════════════════════════════

// ES: Find sin offset.
// EN: Find without offset.
ScanResult ScannerEngine::Find(const char* pattern) const {
    return Find(pattern, 0);
}

// ES: Find con caché: la clave es "patrón@offset". Si no está cacheado, parsea,
//     escanea y guarda el resultado (también los fallos).
// EN: Cached Find: the key is "pattern@offset". If not cached, parses, scans and
//     stores the result (misses too).
ScanResult ScannerEngine::Find(const char* pattern, int offset) const {
    // Check cache
    std::string cacheKey = std::string(pattern) + "@" + std::to_string(offset);
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_cache.find(cacheKey);
        if (it != m_cache.end()) return it->second;
    }

    auto parsed = Parse(pattern);
    if (!parsed) {
        spdlog::error("ScannerEngine: Failed to parse pattern '{}'", pattern);
        return {};
    }

    parsed->offset = offset;
    ScanResult result = Find(*parsed);

    // Cache result
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_cache[cacheKey] = result;
    }

    return result;
}

// ES: Escanea las regiones configuradas y devuelve la primera coincidencia + offset
//     (confianza 1.0). No comprueba que el patrón sea único.
// EN: Scans the configured regions and returns the first match + offset
//     (confidence 1.0). Does not check that the pattern is unique.
ScanResult ScannerEngine::Find(const ParsedPattern& pattern) const {
    auto regions = GetScanRegions();

    for (const auto& region : regions) {
        uintptr_t addr = ScanRegionSSE2(region.start, region.size, pattern);
        if (addr != 0) {
            ScanResult result;
            result.address = addr + pattern.offset;
            result.valid = true;
            result.section = region.section;
            result.confidence = 1.0f;
            return result;
        }
    }

    return {};
}

// ES: Todas las coincidencias en las regiones configuradas (útil para verificar unicidad).
// EN: All matches in the configured regions (useful to verify uniqueness).
std::vector<uintptr_t> ScannerEngine::FindAll(const char* pattern) const {
    auto parsed = Parse(pattern);
    if (!parsed) return {};
    return FindAll(*parsed);
}

std::vector<uintptr_t> ScannerEngine::FindAll(const ParsedPattern& pattern) const {
    std::vector<uintptr_t> allResults;
    auto regions = GetScanRegions();

    for (const auto& region : regions) {
        auto results = ScanRegionAll(region.start, region.size, pattern);
        allResults.insert(allResults.end(), results.begin(), results.end());
    }

    return allResults;
}

// ES: Escaneo en una sección concreta.
// ═══════════════════════════════════════════════════════════════════════════
//  SECTION-SPECIFIC SCANNING
// ═══════════════════════════════════════════════════════════════════════════

// ES: Primera coincidencia dentro de la sección indicada (+ offset del patrón).
// EN: First match inside the given section (+ pattern offset).
ScanResult ScannerEngine::FindInSection(const char* sectionName, const char* pattern) const {
    auto parsed = Parse(pattern);
    if (!parsed) return {};
    return FindInSection(sectionName, *parsed);
}

ScanResult ScannerEngine::FindInSection(const char* sectionName,
                                         const ParsedPattern& pattern) const {
    auto* sec = FindSection(sectionName);
    if (!sec) return {};

    uintptr_t addr = ScanRegionSSE2(sec->base, sec->size, pattern);
    if (addr == 0) return {};

    ScanResult result;
    result.address = addr + pattern.offset;
    result.valid = true;
    result.section = sec->name;
    result.confidence = 1.0f;
    return result;
}

// ES: Todas las coincidencias dentro de la sección indicada.
// EN: All matches inside the given section.
std::vector<uintptr_t> ScannerEngine::FindAllInSection(const char* sectionName,
                                                         const char* pattern) const {
    auto parsed = Parse(pattern);
    if (!parsed) return {};

    auto* sec = FindSection(sectionName);
    if (!sec) return {};

    return ScanRegionAll(sec->base, sec->size, *parsed);
}

// ES: Escaneo de patrones compuestos.
// ═══════════════════════════════════════════════════════════════════════════
//  COMPLEX PATTERN SCANNING
// ═══════════════════════════════════════════════════════════════════════════

// ES: Busca el ancla (primer componente), comprueba cada componente adicional en su
//     offset relativo (si un componente obligatorio falla, no hay resultado), aplica
//     resultOffset y el post-proceso (seguir CALL/JMP, resolver RIP o retroceder al
//     prólogo). Confianza 0.9.
// EN: Finds the anchor (first component), checks each extra component at its relative
//     offset (if a required component fails, there is no result), applies resultOffset
//     and the post-processing (follow CALL/JMP, resolve RIP or walk back to the
//     prologue). Confidence 0.9.
ScanResult ScannerEngine::FindComplex(const ComplexPattern& pattern) const {
    if (pattern.components.empty()) return {};

    // Find the anchor (first component)
    const auto& anchor = pattern.components[0];
    ScanResult anchorResult = Find(anchor.pattern);
    if (!anchorResult) return {};

    uintptr_t anchorAddr = anchorResult.address;

    // ES: Valida los componentes adicionales en sus offsets (lectura protegida con SEH).
    // Validate all additional components at their relative offsets
    for (size_t i = 1; i < pattern.components.size(); i++) {
        const auto& comp = pattern.components[i];
        uintptr_t expectedAddr = anchorAddr + comp.relativeOffset;

        const auto& pat = comp.pattern;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(expectedAddr);

        bool match = true;
        __try {
            for (size_t j = 0; j < pat.bytes.size(); j++) {
                if (pat.mask[j] && data[j] != pat.bytes[j]) {
                    match = false;
                    break;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            match = false;
        }

        if (!match) {
            if (comp.required) return {};
            // ES: Nota: el componente opcional fallido no reduce la confianza en realidad (el
            //     comentario de abajo describe una intención no implementada).
            // Non-required component failed — reduce confidence
        }
    }

    // Calculate result address
    uintptr_t resultAddr = anchorAddr + pattern.resultOffset;

    // Post-processing
    switch (pattern.postProcess) {
        case ComplexPattern::PostProcess::FollowCall:
            resultAddr = FollowCall(resultAddr);
            if (resultAddr == 0) return {};
            break;
        case ComplexPattern::PostProcess::FollowJmp:
            resultAddr = FollowJmp(resultAddr);
            if (resultAddr == 0) return {};
            break;
        case ComplexPattern::PostProcess::ResolveRIP:
            resultAddr = ResolveRIP(resultAddr, pattern.ripOperandOffset, pattern.ripInstructionLen);
            break;
        // ES: Retrocede hasta 4 KB buscando un byte CC (padding int3) o C3 (ret) y toma como
        //     inicio el primer byte no-CC que le sigue. Heurístico: puede cortarse en un C3/CC
        //     que forme parte de otra instrucción.
        case ComplexPattern::PostProcess::FindPrologue: {
            // Walk backwards to function start
            __try {
                for (uintptr_t addr = resultAddr - 1; addr > resultAddr - 4096; addr--) {
                    uint8_t b = *reinterpret_cast<const uint8_t*>(addr);
                    if (b == 0xCC || b == 0xC3) {
                        uintptr_t candidate = addr + 1;
                        while (*reinterpret_cast<const uint8_t*>(candidate) == 0xCC)
                            candidate++;
                        resultAddr = candidate;
                        break;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return {};
            }
            break;
        }
        default:
            break;
    }

    ScanResult result;
    result.address = resultAddr;
    result.valid = true;
    result.section = anchorResult.section;
    result.confidence = 0.9f;
    return result;
}

// ES: Escaneo por lotes (una sola pasada).
// ═══════════════════════════════════════════════════════════════════════════
//  BATCH SCANNING (single pass)
// ═══════════════════════════════════════════════════════════════════════════

// ES: Resuelve muchos patrones recorriendo .text UNA sola vez: indexa cada patrón pendiente
//     por su primer byte fijo y, en cada posición, solo prueba los patrones cuyo primer
//     byte fijo coincide. Se queda con la primera coincidencia de cada patrón (no verifica
//     unicidad). Es la fase 4 del orquestador. Ojo: si pos < fixedByteOffset, patStart
//     da la vuelta (size_t); el control de límites no siempre lo evita.
// EN: Resolves many patterns walking .text ONCE: indexes each pending pattern by its
//     first fixed byte and, at each position, only tries the patterns whose first fixed
//     byte matches. Keeps the first match of each pattern (no uniqueness check). It is
//     orchestrator phase 4. Note: if pos < fixedByteOffset, patStart wraps around
//     (size_t); the bounds check does not always catch it.
void ScannerEngine::BatchScan(std::vector<BatchEntry>& entries) const {
    if (entries.empty()) return;

    auto regions = GetScanRegions();

    // ES: Índice primer byte fijo -> patrones que lo usan.
    // Build a first-byte index for fast dispatch
    // Map: first non-wildcard byte → list of entry indices
    struct FirstByteEntry { size_t entryIdx; int fixedByteOffset; };
    std::unordered_map<uint8_t, std::vector<FirstByteEntry>> byteIndex;

    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].result.valid) continue; // Already resolved

        const auto& pat = entries[i].pattern;
        int firstFixed = -1;
        for (size_t j = 0; j < pat.bytes.size(); j++) {
            if (pat.mask[j]) { firstFixed = static_cast<int>(j); break; }
        }
        if (firstFixed >= 0) {
            byteIndex[pat.bytes[firstFixed]].push_back({i, firstFixed});
        }
    }

    // ES: Cuántos patrones siguen sin resolver.
    // Count how many patterns remain unresolved
    auto unresolvedCount = [&]() {
        size_t count = 0;
        for (const auto& e : entries)
            if (!e.result.valid) count++;
        return count;
    };

    for (const auto& region : regions) {
        if (unresolvedCount() == 0) break;

        const uint8_t* data = reinterpret_cast<const uint8_t*>(region.start);
        const size_t size = region.size;

        // ES: Recorre byte a byte consultando el índice.
        // Scan byte by byte, checking the first-byte index
        for (size_t pos = 0; pos < size; pos++) {
            uint8_t b = data[pos];

            auto it = byteIndex.find(b);
            if (it == byteIndex.end()) continue;

            // Check each candidate pattern that starts with this byte
            auto& candidates = it->second;
            for (auto& cand : candidates) {
                auto& entry = entries[cand.entryIdx];
                if (entry.result.valid) continue;

                const auto& pat = entry.pattern;
                size_t patStart = pos - cand.fixedByteOffset;

                // Bounds check
                if (patStart + pat.bytes.size() > size) continue;

                // Full pattern match
                bool match = true;
                for (size_t j = 0; j < pat.bytes.size(); j++) {
                    if (pat.mask[j] && data[patStart + j] != pat.bytes[j]) {
                        match = false;
                        break;
                    }
                }

                if (match) {
                    entry.result.address = region.start + patStart + pat.offset;
                    entry.result.valid = true;
                    entry.result.section = region.section;
                    entry.result.confidence = 1.0f;
                    spdlog::info("BatchScan: '{}' found at 0x{:X}", entry.id, entry.result.address);
                }
            }
        }
    }

    size_t found = 0, missed = 0;
    for (const auto& e : entries) {
        if (e.result.valid) found++;
        else missed++;
    }
    spdlog::info("BatchScan: {}/{} patterns resolved ({} missed)",
                 found, entries.size(), missed);
}

// ES: Utilidades de resolución de direcciones.
// ═══════════════════════════════════════════════════════════════════════════
//  ADDRESS RESOLUTION UTILITIES
// ═══════════════════════════════════════════════════════════════════════════

// ES: destino = instrucción + longitud + rel32 leído en operandOffset.
// EN: target = instruction + length + rel32 read at operandOffset.
uintptr_t ScannerEngine::ResolveRIP(uintptr_t instrAddr, int operandOffset, int instrLen) {
    int32_t relative;
    std::memcpy(&relative, reinterpret_cast<void*>(instrAddr + operandOffset), sizeof(int32_t));
    return instrAddr + instrLen + relative;
}

// ES: Destino de un CALL rel32 (E8); 0 si no es E8.
// EN: Target of a CALL rel32 (E8); 0 if not E8.
uintptr_t ScannerEngine::FollowCall(uintptr_t callAddr) {
    uint8_t opcode = *reinterpret_cast<uint8_t*>(callAddr);
    if (opcode != 0xE8) return 0;
    return ResolveRIP(callAddr, 1, 5);
}

// ES: Destino de un JMP rel32 (E9); 0 si no es E9.
// EN: Target of a JMP rel32 (E9); 0 if not E9.
uintptr_t ScannerEngine::FollowJmp(uintptr_t jmpAddr) {
    uint8_t opcode = *reinterpret_cast<uint8_t*>(jmpAddr);
    if (opcode != 0xE9) return 0;
    return ResolveRIP(jmpAddr, 1, 5);
}

// ES: Destino de un salto condicional corto (7x rel8) o cercano (0F 8x rel32); 0 si no lo es.
// EN: Target of a short (7x rel8) or near (0F 8x rel32) conditional jump; 0 otherwise.
uintptr_t ScannerEngine::FollowConditionalJmp(uintptr_t jmpAddr) {
    uint8_t b0 = *reinterpret_cast<uint8_t*>(jmpAddr);
    uint8_t b1 = *reinterpret_cast<uint8_t*>(jmpAddr + 1);

    // Short conditional jumps: 7x rel8
    if ((b0 & 0xF0) == 0x70) {
        int8_t rel;
        std::memcpy(&rel, reinterpret_cast<void*>(jmpAddr + 1), 1);
        return jmpAddr + 2 + rel;
    }

    // Near conditional jumps: 0F 8x rel32
    if (b0 == 0x0F && (b1 & 0xF0) == 0x80) {
        return ResolveRIP(jmpAddr, 2, 6);
    }

    return 0;
}

// ES: Caché.
// ═══════════════════════════════════════════════════════════════════════════
//  CACHE
// ═══════════════════════════════════════════════════════════════════════════

// ES: Vacía la caché / devuelve su tamaño (con mutex).
// EN: Clears the cache / returns its size (under mutex).
void ScannerEngine::ClearCache() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_cache.clear();
}

size_t ScannerEngine::CacheSize() const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return m_cache.size();
}

} // namespace kmp
