// ES: patterns.cpp — "camino B" de resolución de direcciones (ResolveGameFunctions) y
//     reintento de globales (RetryGlobalDiscovery). Para cada función de GameFunctions:
//     1) escanea su patrón AOB y lo valida con .pdata (RtlLookupFunctionEntry): si cae
//     a más de 0x10 bytes del inicio real de la función, es la función EQUIVOCADA y se
//     descarta; 2) si no hay patrón o falla, usa RuntimeStringScanner (string en .rdata ->
//     LEA que lo referencia -> inicio de función); 3) descubre los globales PlayerBase y
//     GameWorld desensamblando funciones ya resueltas (MOV/LEA reg,[RIP+disp32] hacia
//     .data/.rdata) o por string, con RVAs fijas de la v1.0.68 como último recurso.
// EN: patterns.cpp — address resolution "path B" (ResolveGameFunctions) and global
//     retry (RetryGlobalDiscovery). For each GameFunctions entry: 1) scans its AOB pattern
//     and validates it with .pdata (RtlLookupFunctionEntry): if it lands more than 0x10
//     bytes past the real function start it is the WRONG function and is dropped; 2) if
//     there is no pattern or it fails, uses RuntimeStringScanner (string in .rdata -> LEA
//     referencing it -> function start); 3) discovers the PlayerBase and GameWorld globals
//     by disassembling already resolved functions (MOV/LEA reg,[RIP+disp32] into
//     .data/.rdata) or via strings, with fixed v1.0.68 RVAs as last resort.
#include "kmp/patterns.h"
#include "kmp/scanner.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <Windows.h>

namespace kmp {

// ES: Escáner de strings en runtime: respaldo cuando el patrón es nullptr o no casa.
//     Busca strings conocidos en kenshi_x64.exe cargado en memoria y sigue sus xrefs hasta
//     la función (misma lógica que el script re_scanner.py).
// ── Runtime String Scanner ──
// Fallback for when static patterns are nullptr or fail to match.
// Scans the loaded kenshi_x64.exe in memory for known strings,
// follows xrefs to find function addresses (same logic as re_scanner.py).
class RuntimeStringScanner {
public:
    // ES: Guarda la base/tamaño del módulo y localiza .text, .rdata y .data.
    // EN: Stores module base/size and locates .text, .rdata and .data.
    RuntimeStringScanner(uintptr_t moduleBase, size_t moduleSize)
        : m_base(moduleBase), m_size(moduleSize) {
        FindSections();
    }

    // ES: Función que referencia un string: string -> LEA RIP-relativo -> inicio de función.
    //     Devuelve 0 si falla algún paso.
    // Find a function that references the given string.
    // Returns the function start address, or 0 on failure.
    uintptr_t FindFunctionByString(const char* searchStr, int searchLen) const {
        if (!m_textBase || !m_rdataBase) return 0;

        // Step 1: Find the string in .rdata (or any readable section)
        uintptr_t strAddr = FindStringInMemory(searchStr, searchLen);
        if (!strAddr) return 0;

        // Step 2: Find code that references this string via RIP-relative LEA
        uintptr_t xref = FindStringXref(strAddr);
        if (!xref) return 0;

        // Step 3: Walk backwards to find function prologue
        uintptr_t funcStart = FindFunctionStart(xref);
        return funcStart;
    }

    // ES: Busca un global (puntero en .data, o también .rdata si includeReadOnly) cargado en la
    //     función que referencia un string: escanea desde el inicio de la función hasta 512
    //     bytes después del xref buscando MOV/LEA reg,[RIP+disp32]. 'nth' elige la n-ésima
    //     coincidencia. Devuelve la DIRECCIÓN del global, no su valor.
    // Find a global .data pointer that is loaded near code referencing a string.
    // Scans the function containing the string xref for MOV reg, [RIP+disp32]
    // instructions that point into the .data section. Returns the address of
    // the global (not its value).
    uintptr_t FindGlobalNearString(const char* searchStr, int searchLen,
                                    int nth = 0, bool includeReadOnly = false) const {
        if (!m_textBase || !m_rdataBase) return 0;

        uintptr_t strAddr = FindStringInMemory(searchStr, searchLen);
        if (!strAddr) {
            spdlog::debug("FindGlobalNearString('{}', nth={}): string not found", searchStr, nth);
            return 0;
        }

        uintptr_t xref = FindStringXref(strAddr);
        if (!xref) {
            spdlog::debug("FindGlobalNearString('{}', nth={}): no xref for str@0x{:X}", searchStr, nth, strAddr);
            return 0;
        }

        uintptr_t funcStart = FindFunctionStart(xref);
        if (!funcStart) {
            spdlog::debug("FindGlobalNearString('{}', nth={}): no prologue found near xref@0x{:X}", searchStr, nth, xref);
            return 0;
        }

        // ES: Rango de escaneo: inicio de la función hasta xref+512 (acotado al final de .text).
        // Scan the entire function (from func start through 512 past the xref)
        // for MOV/LEA reg, [RIP+disp32] pointing to data sections
        uintptr_t scanStart = funcStart;
        uintptr_t scanEnd = xref + 512;
        if (scanEnd > m_textBase + m_textSize) scanEnd = m_textBase + m_textSize;

        uintptr_t result = ScanForGlobalLoadImpl(scanStart, scanEnd, nth, includeReadOnly);
        spdlog::debug("FindGlobalNearString('{}', nth={}, ro={}): str@0x{:X} xref@0x{:X} func@0x{:X} scan[0x{:X}..0x{:X}] => 0x{:X}",
                      searchStr, nth, includeReadOnly, strAddr, xref, funcStart, scanStart, scanEnd, result);
        return result;
    }

    // ES: Accesores y envoltorios públicos (usados también para diagnóstico).
    // EN: Accessors and public wrappers (also used for diagnostics).
    // Getters for section info
    uintptr_t GetDataBase() const { return m_dataBase; }
    size_t GetDataSize() const { return m_dataSize; }

    // Public wrapper: find string address (used by internal methods + external diagnostics)
    uintptr_t FindStringInMemory(const char* searchStr, int len) const {
        return FindStringInMemoryImpl(searchStr, len);
    }

    // Diagnostic: find code xref to string (public for logging)
    uintptr_t FindStringXref(uintptr_t stringAddr) const {
        return FindStringXrefImpl(stringAddr);
    }

    // Public: scan a code range for RIP-relative loads from data sections
    uintptr_t ScanForGlobalLoad(uintptr_t start, uintptr_t end, int nth,
                                bool includeReadOnly = false) const {
        return ScanForGlobalLoadImpl(start, end, nth, includeReadOnly);
    }

private:
    // ES: Base/tamaño del módulo y de las secciones .text/.rdata/.data.
    // EN: Module and .text/.rdata/.data section base/size.
    uintptr_t m_base = 0;
    size_t    m_size = 0;
    uintptr_t m_textBase = 0;
    size_t    m_textSize = 0;
    uintptr_t m_rdataBase = 0;
    size_t    m_rdataSize = 0;
    uintptr_t m_dataBase = 0;
    size_t    m_dataSize = 0;

    // ES: Recorre la tabla de secciones PE y guarda .text, .rdata y .data.
    // EN: Walks the PE section table and stores .text, .rdata and .data.
    void FindSections() {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m_base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(m_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;

        auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
            char name[9] = {};
            std::memcpy(name, section->Name, 8);
            if (std::strcmp(name, ".text") == 0) {
                m_textBase = m_base + section->VirtualAddress;
                m_textSize = section->Misc.VirtualSize;
            } else if (std::strcmp(name, ".rdata") == 0) {
                m_rdataBase = m_base + section->VirtualAddress;
                m_rdataSize = section->Misc.VirtualSize;
            } else if (std::strcmp(name, ".data") == 0) {
                m_dataBase = m_base + section->VirtualAddress;
                m_dataSize = section->Misc.VirtualSize;
            }
        }
    }

    // ES: ¿Está en una sección escribible del módulo (ni .text ni .rdata; p.ej. .data/.bss)?
    // Check if an address is in a writable module section (not .text, not .rdata)
    bool IsInWritableSection(uintptr_t addr) const {
        if (addr < m_base || addr >= m_base + m_size) return false;
        // Exclude .text (code) and .rdata (read-only data)
        if (addr >= m_textBase && addr < m_textBase + m_textSize) return false;
        if (addr >= m_rdataBase && addr < m_rdataBase + m_rdataSize) return false;
        // Everything else in the module is potentially writable (.data, .bss, etc.)
        return true;
    }

    // ES: ¿Está en cualquier sección de datos (todo menos .text, incluida .rdata)? MSVC puede
    //     poner globales como PlayerBase en .rdata (punteros const inicializados al arrancar).
    // Check if an address is in ANY data section (including .rdata)
    // Used for finding globals like PlayerBase which MSVC can place in .rdata
    // (static const pointers initialized at startup, read-only after that)
    bool IsInDataSection(uintptr_t addr) const {
        if (addr < m_base || addr >= m_base + m_size) return false;
        // Exclude .text (code) only
        if (addr >= m_textBase && addr < m_textBase + m_textSize) return false;
        return true;
    }

    // ES: Búsqueda por fuerza bruta (memcmp) del string: primero en .rdata y luego en todo el
    //     módulo; cada pasada protegida con SEH. Devuelve la primera coincidencia.
    // EN: Brute-force (memcmp) search of the string: first in .rdata, then the whole module;
    //     each pass SEH-protected. Returns the first match.
    uintptr_t FindStringInMemoryImpl(const char* searchStr, int len) const {
        // Search .rdata first, then full module
        uintptr_t sections[] = { m_rdataBase, m_base };
        size_t sizes[] = { m_rdataSize, m_size };

        for (int s = 0; s < 2; s++) {
            if (!sections[s] || !sizes[s]) continue;

            __try {
                auto* start = reinterpret_cast<const uint8_t*>(sections[s]);
                auto* end = start + sizes[s] - len;
                for (auto* p = start; p < end; p++) {
                    if (std::memcmp(p, searchStr, len) == 0) {
                        return reinterpret_cast<uintptr_t>(p);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
        }
        return 0;
    }

    // ES: Primer LEA RIP-relativo de .text (48/4C 8D con mod=0 rm=5) cuyo destino es el string.
    //     Solo devuelve el PRIMER xref aunque haya varios.
    // EN: First RIP-relative LEA in .text (48/4C 8D with mod=0 rm=5) targeting the string.
    //     Only returns the FIRST xref even if there are several.
    uintptr_t FindStringXrefImpl(uintptr_t stringAddr) const {
        if (!m_textBase || !m_textSize) return 0;

        __try {
            auto* text = reinterpret_cast<const uint8_t*>(m_textBase);
            size_t textLen = m_textSize;

            for (size_t i = 0; i + 7 < textLen; i++) {
                // REX.W LEA reg, [RIP+disp32]: 48 8D xx (mod=0, rm=5)
                // REX.WR LEA: 4C 8D xx
                if ((text[i] == 0x48 || text[i] == 0x4C) && text[i + 1] == 0x8D) {
                    uint8_t modrm = text[i + 2];
                    uint8_t mod = (modrm >> 6) & 3;
                    uint8_t rm = modrm & 7;
                    if (mod == 0 && rm == 5) {
                        int32_t disp;
                        std::memcpy(&disp, &text[i + 3], 4);
                        uintptr_t instrAddr = m_textBase + i;
                        uintptr_t target = instrAddr + 7 + disp;
                        if (target == stringAddr) {
                            return instrAddr;
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        return 0;
    }

    // ES: Inicio de la función que contiene 'codeAddr'. Método 1: .pdata vía
    //     RtlLookupFunctionEntry (autoritativo, sirve para funciones grandes). Método 2:
    //     retroceder hasta 16 KB buscando padding CC/C3 seguido de un prólogo reconocido
    //     (IsPrologue). El método 2 puede fallar en funciones muy grandes (p.ej. CharacterSpawn).
    // EN: Start of the function containing 'codeAddr'. Method 1: .pdata via
    //     RtlLookupFunctionEntry (authoritative, works for large functions). Method 2: walk back
    //     up to 16 KB looking for CC/C3 padding followed by a recognized prologue (IsPrologue).
    //     Method 2 can fail on very large functions (e.g. CharacterSpawn).
    uintptr_t FindFunctionStart(uintptr_t codeAddr) const {
        // Method 1: Use .pdata (RtlLookupFunctionEntry) — authoritative and works
        // for large functions (ApplyDamage=6925B, StartAttack=9253B) where the
        // string xref can be thousands of bytes from the prologue.
        __try {
            DWORD64 imageBase = 0;
            auto* rtFunc = RtlLookupFunctionEntry(
                static_cast<DWORD64>(codeAddr), &imageBase, nullptr);
            if (rtFunc) {
                uintptr_t funcStart = static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress;
                if (funcStart < codeAddr && funcStart > 0) {
                    return funcStart;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Method 2: Fallback — walk backwards looking for function prologue patterns.
        // Increased from 2048 to 16384 to handle large Kenshi functions.
        __try {
            for (uintptr_t addr = codeAddr - 1; addr > codeAddr - 16384 && addr > m_textBase; addr--) {
                uint8_t b = *reinterpret_cast<const uint8_t*>(addr);

                // Look for CC/C3 padding (end of previous function)
                if (b == 0xCC || b == 0xC3) {
                    uintptr_t candidate = addr + 1;
                    // Skip CC padding
                    while (*reinterpret_cast<const uint8_t*>(candidate) == 0xCC) {
                        candidate++;
                    }
                    if (candidate < codeAddr && IsPrologue(candidate)) {
                        return candidate;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        return 0;
    }

    // ES: Busca en [start, end) la n-ésima instrucción MOV/LEA reg,[RIP+disp32] (48/4C 8B/8D)
    //     cuyo destino cae en una sección de datos escribible (o también .rdata si
    //     includeReadOnly). Devuelve la dirección destino (el global) o 0.
    // EN: Searches [start, end) for the nth MOV/LEA reg,[RIP+disp32] (48/4C 8B/8D) whose
    //     target lies in a writable data section (or .rdata too if includeReadOnly).
    //     Returns the target address (the global) or 0.
    uintptr_t ScanForGlobalLoadImpl(uintptr_t start, uintptr_t end, int nth,
                                    bool includeReadOnly = false) const {
        // Look for MOV reg, [RIP+disp32] (REX.W prefix: 48 8B/4C 8B)
        // and LEA reg, [RIP+disp32] (48 8D/4C 8D) pointing to data sections.
        // includeReadOnly=true also accepts .rdata targets (for const globals like PlayerBase)
        __try {
            int found = 0;
            auto* code = reinterpret_cast<const uint8_t*>(start);
            size_t len = end - start;

            for (size_t i = 0; i + 7 < len; i++) {
                // REX.W (48) or REX.WR (4C) prefix
                bool hasRex = (code[i] == 0x48 || code[i] == 0x4C);
                // MOV (8B) or LEA (8D) opcode
                bool isMemOp = (code[i + 1] == 0x8B || code[i + 1] == 0x8D);

                if (hasRex && isMemOp) {
                    uint8_t modrm = code[i + 2];
                    uint8_t mod = (modrm >> 6) & 3;
                    uint8_t rm = modrm & 7;
                    if (mod == 0 && rm == 5) {
                        int32_t disp;
                        std::memcpy(&disp, &code[i + 3], 4);
                        uintptr_t instrAddr = start + i;
                        uintptr_t target = instrAddr + 7 + disp;

                        bool accepted = includeReadOnly
                            ? IsInDataSection(target)
                            : IsInWritableSection(target);
                        if (accepted) {
                            if (found == nth) return target;
                            found++;
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        return 0;
    }

    // ES: ¿Parecen los bytes en 'addr' un prólogo MSVC? Acepta guardados en shadow space,
    //     push (con/sin REX), sub rsp y también mov rax, rsp (48 8B C4). Nota: docs/03-scanner.md
    //     dice que no reconoce 48 8B C4, pero el código actual sí lo hace (última comprobación).
    // EN: Do the bytes at 'addr' look like an MSVC prologue? Accepts shadow space saves,
    //     pushes (with/without REX), sub rsp and also mov rax, rsp (48 8B C4). Note:
    //     docs/03-scanner.md says it does not recognize 48 8B C4, but current code does (last check).
    bool IsPrologue(uintptr_t addr) const {
        __try {
            auto* p = reinterpret_cast<const uint8_t*>(addr);
            // mov [rsp+xx], rbx: 48 89 5C 24
            if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x5C && p[3] == 0x24) return true;
            // mov [rsp+xx], rsi: 48 89 74 24
            if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x74 && p[3] == 0x24) return true;
            // mov [rsp+xx], rcx: 48 89 4C 24
            if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x4C && p[3] == 0x24) return true;
            // mov [rsp+xx], rdx: 48 89 54 24
            if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x54 && p[3] == 0x24) return true;
            // mov [rsp+xx], rbp: 48 89 6C 24
            if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x6C && p[3] == 0x24) return true;
            // mov [rsp+xx], r8: 4C 89 44 24
            if (p[0] == 0x4C && p[1] == 0x89 && p[2] == 0x44 && p[3] == 0x24) return true;
            // push rbx: 40 53
            if (p[0] == 0x40 && p[1] == 0x53) return true;
            // push rbp: 40 55
            if (p[0] == 0x40 && p[1] == 0x55) return true;
            // push rsi: 40 56
            if (p[0] == 0x40 && p[1] == 0x56) return true;
            // push rdi: 40 57
            if (p[0] == 0x40 && p[1] == 0x57) return true;
            // sub rsp, imm8: 48 83 EC
            if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) return true;
            // sub rsp, imm32: 48 81 EC
            if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) return true;
            // push rbp; REX: 55 48
            if (p[0] == 0x55 && (p[1] == 0x48 || p[1] == 0x8B)) return true;
            // push rbx; REX: 53 48
            if (p[0] == 0x53 && p[1] == 0x48) return true;
            // push r12/r13/r14/r15: 41 5x
            if (p[0] == 0x41 && (p[1] >= 0x54 && p[1] <= 0x57)) return true;
            // mov rax, rsp: 48 8B C4 (common MSVC prologue in Kenshi)
            if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4) return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return false;
    }
};

// ES: Resolución de las funciones del juego (camino B).
// ── Resolve Game Functions ──

// ES: Rellena 'funcs' con patrones AOB, respaldo por strings y descubrimiento de globales.
//     Devuelve funcs.IsMinimallyResolved(). No sobrescribe entradas ya resueltas (p.ej. por
//     el orquestador vía vtable).
// EN: Fills 'funcs' with AOB patterns, string fallback and global discovery.
//     Returns funcs.IsMinimallyResolved(). Does not overwrite already resolved entries
//     (e.g. by the orchestrator via vtable).
bool ResolveGameFunctions(const PatternScanner& scanner, GameFunctions& funcs) {
    uintptr_t base = scanner.GetBase();
    size_t moduleSize = scanner.GetSize();
    int resolved = 0;
    int total = 0;

    // ES: tryPattern: si el campo ya tiene valor lo respeta; si no hay patrón no hace nada;
    //     si el patrón casa, valida con .pdata: desfase <= 0x10 se autocorrige al inicio de la
    //     función, desfase mayor = función equivocada (se deja a nullptr). Además rechaza
    //     direcciones no alineadas a 16 salvo que .pdata confirme que son inicio de función.
    // EN: tryPattern: keeps the field if already set; does nothing without a pattern; if
    //     the pattern matches, validates with .pdata: offset <= 0x10 is auto-corrected to the
    //     function start, larger offset = wrong function (left nullptr). It also rejects
    //     addresses not 16-byte aligned unless .pdata confirms they are a function start.
    auto tryPattern = [&](const char* name, const char* pattern, void*& target) {
        total++;
        if (target != nullptr) {
            // Already resolved (e.g. by orchestrator vtable discovery) — don't overwrite
            resolved++;
            spdlog::debug("ResolveGameFunctions: '{}' already resolved at 0x{:X}, skipping pattern scan",
                          name, reinterpret_cast<uintptr_t>(target));
            return;
        }
        if (!pattern) {
            spdlog::debug("ResolveGameFunctions: '{}' has no pattern yet", name);
            return;
        }
        auto result = scanner.Find(pattern);
        if (result) {
            uintptr_t addr = result.address;

            // ES: Validación .pdata: algunos patrones caen dentro de OTRA función en Steam;
            //     autocorregir a esa función hookearía la equivocada -> crash. Solo se corrige <= 16 bytes.
            // Validate pattern match is a real function entry using .pdata
            // Some patterns match inside a DIFFERENT function on Steam.
            // Auto-correcting to that function's start would hook the WRONG function → crash.
            // Only accept corrections within 16 bytes (alignment/prefix instructions).
            DWORD64 imageBase = 0;
            auto* rtFunc = RtlLookupFunctionEntry(
                static_cast<DWORD64>(addr), &imageBase, nullptr);
            uintptr_t funcStart = 0;
            if (rtFunc) {
                funcStart = static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress;
                if (funcStart != addr) {
                    uintptr_t offset = addr - funcStart;
                    if (offset <= 0x10) {
                        // Small offset — likely alignment padding or prefix instruction
                        spdlog::warn("ResolveGameFunctions: '{}' pattern at 0x{:X} (offset +0x{:X}). "
                                     "Auto-correcting to function start 0x{:X}",
                                     name, addr, offset, funcStart);
                        addr = funcStart;
                    } else {
                        // Large offset — pattern matched inside a WRONG function
                        spdlog::error("ResolveGameFunctions: '{}' pattern at 0x{:X} is +0x{:X} bytes into "
                                      "function 0x{:X} — WRONG FUNCTION, nulling",
                                      name, addr, offset, funcStart);
                        target = nullptr;
                        return;
                    }
                }
            }

            // ES: Comprobación de alineación (solo se acepta sin alinear si .pdata lo confirma, caso
            //     SquadAddMember @0x928423).
            // Alignment check: MSVC usually aligns functions to 16 bytes, but NOT always.
            // .pdata is the authoritative source for function boundaries.
            // SquadAddMember at 0x928423 is a valid .pdata function entry despite not being
            // 16-byte aligned. Only warn, don't reject — .pdata already confirmed it above.
            if ((addr & 0xF) != 0) {
                if (rtFunc && funcStart == addr) {
                    // .pdata confirms this IS a function start despite odd alignment — accept it
                    spdlog::warn("ResolveGameFunctions: '{}' at 0x{:X} NOT 16-byte aligned "
                                 "(low nibble 0x{:X}) but .pdata confirms function start — accepting",
                                 name, addr, addr & 0xF);
                } else {
                    spdlog::error("ResolveGameFunctions: '{}' at 0x{:X} NOT 16-byte aligned "
                                  "(low nibble 0x{:X}) and no .pdata confirmation — nulling",
                                  name, addr, addr & 0xF);
                    target = nullptr;
                    return;
                }
            }

            target = reinterpret_cast<void*>(addr);
            resolved++;
            spdlog::info("ResolveGameFunctions: '{}' = 0x{:X} (pattern)", name, addr);
        } else {
            spdlog::warn("ResolveGameFunctions: '{}' pattern not found", name);
        }
    };

    // ES: Primero se intenta por patrón, en el mismo orden que las secciones de patterns.h.
    // Try pattern-based resolution first

    // Entity lifecycle
    tryPattern("CharacterSpawn",       patterns::CHARACTER_SPAWN,        funcs.CharacterSpawn);
    tryPattern("CharacterDestroy",     patterns::CHARACTER_DESTROY,      funcs.CharacterDestroy);
    tryPattern("CreateRandomSquad",    patterns::CREATE_RANDOM_SQUAD,    funcs.CreateRandomSquad);
    tryPattern("CharacterSerialise",   patterns::CHARACTER_SERIALISE,    funcs.CharacterSerialise);
    tryPattern("CharacterKO",          patterns::CHARACTER_KO,           funcs.CharacterKO);

    // Movement
    tryPattern("CharacterSetPosition", patterns::CHARACTER_SET_POSITION, funcs.CharacterSetPosition);
    tryPattern("CharacterMoveTo",      patterns::CHARACTER_MOVE_TO,      funcs.CharacterMoveTo);

    // Combat
    tryPattern("ApplyDamage",          patterns::APPLY_DAMAGE,           funcs.ApplyDamage);
    tryPattern("StartAttack",          patterns::START_ATTACK,           funcs.StartAttack);
    tryPattern("CharacterDeath",       patterns::CHARACTER_DEATH,        funcs.CharacterDeath);
    tryPattern("HealthUpdate",         patterns::HEALTH_UPDATE,          funcs.HealthUpdate);
    tryPattern("CutDamageMod",         patterns::CUT_DAMAGE_MOD,        funcs.CutDamageMod);
    tryPattern("UnarmedDamage",        patterns::UNARMED_DAMAGE,        funcs.UnarmedDamage);
    tryPattern("MartialArtsCombat",    patterns::MARTIAL_ARTS_COMBAT,    funcs.MartialArtsCombat);

    // World / Zones
    tryPattern("ZoneLoad",             patterns::ZONE_LOAD,              funcs.ZoneLoad);
    tryPattern("ZoneUnload",           patterns::ZONE_UNLOAD,            funcs.ZoneUnload);
    tryPattern("BuildingPlace",        patterns::BUILDING_PLACE,         funcs.BuildingPlace);
    tryPattern("BuildingDestroyed",    patterns::BUILDING_DESTROYED,     funcs.BuildingDestroyed);
    tryPattern("Navmesh",              patterns::NAVMESH,                funcs.Navmesh);
    tryPattern("SpawnCheck",           patterns::SPAWN_CHECK,            funcs.SpawnCheck);

    // Game loop / Time
    tryPattern("GameFrameUpdate",      patterns::GAME_FRAME_UPDATE,      funcs.GameFrameUpdate);
    tryPattern("TimeUpdate",           patterns::TIME_UPDATE,            funcs.TimeUpdate);
    tryPattern("SetPaused",            patterns::SET_PAUSED,             funcs.SetPaused);

    // Save/Load
    tryPattern("SaveGame",             patterns::SAVE_GAME,              funcs.SaveGame);
    tryPattern("LoadGame",             patterns::LOAD_GAME,              funcs.LoadGame);
    tryPattern("ImportGame",           patterns::IMPORT_GAME,            funcs.ImportGame);
    tryPattern("CharacterStats",       patterns::CHARACTER_STATS,        funcs.CharacterStats);

    // Input (optional - WndProc handles input)
    tryPattern("InputKeyPressed",      patterns::INPUT_KEY_PRESSED,      funcs.InputKeyPressed);
    tryPattern("InputMouseMoved",      patterns::INPUT_MOUSE_MOVED,      funcs.InputMouseMoved);

    // Squad / Platoon
    tryPattern("SquadCreate",          patterns::SQUAD_CREATE,           funcs.SquadCreate);
    tryPattern("SquadAddMember",       patterns::SQUAD_ADD_MEMBER,       funcs.SquadAddMember);

    // Inventory / Items
    tryPattern("ItemPickup",           patterns::ITEM_PICKUP,            funcs.ItemPickup);
    tryPattern("ItemDrop",             patterns::ITEM_DROP,              funcs.ItemDrop);
    tryPattern("BuyItem",              patterns::BUY_ITEM,               funcs.BuyItem);

    // Faction / Diplomacy
    tryPattern("FactionRelation",      patterns::FACTION_RELATION,       funcs.FactionRelation);

    // AI System
    tryPattern("AICreate",             patterns::AI_CREATE,              funcs.AICreate);
    tryPattern("AIPackages",           patterns::AI_PACKAGES,            funcs.AIPackages);

    // Turret / Ranged
    tryPattern("GunTurret",            patterns::GUN_TURRET,             funcs.GunTurret);
    tryPattern("GunTurretFire",        patterns::GUN_TURRET_FIRE,        funcs.GunTurretFire);

    // Building Management
    tryPattern("BuildingDismantle",    patterns::BUILDING_DISMANTLE,     funcs.BuildingDismantle);
    tryPattern("BuildingConstruct",    patterns::BUILDING_CONSTRUCT,     funcs.BuildingConstruct);
    tryPattern("BuildingRepair",       patterns::BUILDING_REPAIR,        funcs.BuildingRepair);

    // ES: Respaldo por strings en runtime: para cada función que siga a nullptr se busca su
    //     string ancla -> xref -> inicio de función, y se valida con .pdata igual que el patrón.
    // ── Runtime String Scanner Fallback ──
    // If patterns failed, try runtime string-xref scanning
    int fallbackResolved = 0;
    RuntimeStringScanner rss(base, moduleSize);

    struct FallbackEntry {
        const char* label;
        const char* searchStr;
        int         searchLen;
        void**      target;
    };

    // ES: Strings de respaldo verificados en kenshi_x64.exe v1.0.68 (id, texto, longitud,
    //     campo destino). CharacterMoveTo y SquadAddMember están comentados porque sus strings
    //     llevan a la función equivocada en Steam.
    // Fallback strings verified to exist in kenshi_x64.exe v1.0.68
    FallbackEntry fallbacks[] = {
        // Entity lifecycle
        {"CharacterSpawn",       "[RootObjectFactory::process] Character",              38, &funcs.CharacterSpawn},
        {"CharacterDestroy",     "NodeList::destroyNodesByBuilding",                    32, &funcs.CharacterDestroy},
        {"CreateRandomSquad",    "[RootObjectFactory::createRandomSquad] Missing squad leader", 59, &funcs.CreateRandomSquad},
        {"CharacterSerialise",   "[Character::serialise] Character '",                  34, &funcs.CharacterSerialise},
        {"CharacterKO",          "knockout",                                             8, &funcs.CharacterKO},
        // Movement
        {"CharacterSetPosition", "HavokCharacter::setPosition moved someone off the world", 55, &funcs.CharacterSetPosition},
        // ES: "pathfind" es demasiado genérico (cae en otra función en Steam); se queda a null
        //     y la sincronización de movimiento se hace por sondeo de posición.
        // CharacterMoveTo: "pathfind" is too generic — finds wrong function on Steam.
        // Resolved via vtable discovery or remains null (position polling handles sync).
        // {"CharacterMoveTo",   "pathfind",                                             8, &funcs.CharacterMoveTo},
        // Combat
        {"ApplyDamage",          "Attack damage effect",                                20, &funcs.ApplyDamage},
        {"StartAttack",          "Cutting damage",                                      14, &funcs.StartAttack},
        {"CharacterDeath",       "{1} has died from blood loss.",                        29, &funcs.CharacterDeath},
        {"HealthUpdate",         "block chance",                                         12, &funcs.HealthUpdate},
        {"MartialArtsCombat",    "Martial Arts",                                        12, &funcs.MartialArtsCombat},
        // World / Zones
        {"ZoneLoad",             "zone.%d.%d.zone",                                     15, &funcs.ZoneLoad},
        {"ZoneUnload",           "destroyed navmesh",                                   17, &funcs.ZoneUnload},
        {"BuildingPlace",        "[RootObjectFactory::createBuilding] Building",         44, &funcs.BuildingPlace},
        {"BuildingDestroyed",    "Building::setDestroyed",                              22, &funcs.BuildingDestroyed},
        // Game loop / Time
        {"GameFrameUpdate",      "Kenshi 1.0.",                                         11, &funcs.GameFrameUpdate},
        {"TimeUpdate",           "timeScale",                                            9, &funcs.TimeUpdate},
        // Save/Load
        {"SaveGame",             "quicksave",                                             9, &funcs.SaveGame},
        {"LoadGame",             "[SaveManager::loadGame] No towns loaded.",             40, &funcs.LoadGame},
        {"ImportGame",           "[SaveManager::importGame] No towns loaded.",           42, &funcs.ImportGame},
        {"CharacterStats",       "CharacterStats_Attributes",                            25, &funcs.CharacterStats},
        // Squad / Platoon
        {"SquadCreate",          "Reset squad positions",                                21, &funcs.SquadCreate},
        // ES: "delayedSpawningChecks" cae en otra función en Steam; se resuelve por vtable
        //     (el comentario dice core.cpp; en este módulo lo hace el orquestador por RTTI).
        // SquadAddMember: "delayedSpawningChecks" finds wrong function on Steam.
        // Resolved via vtable discovery (Squad vtable+0x10) in core.cpp instead.
        // {"SquadAddMember",    "delayedSpawningChecks",                               21, &funcs.SquadAddMember},
        // Inventory / Items
        {"ItemPickup",           "addItem",                                               7, &funcs.ItemPickup},
        {"ItemDrop",             "removeItem",                                           10, &funcs.ItemDrop},
        {"BuyItem",              "buyItem",                                               7, &funcs.BuyItem},
        // Faction / Diplomacy
        {"FactionRelation",      "faction relation",                                     16, &funcs.FactionRelation},
        // AI System
        {"AICreate",             "[AI::create] No faction for",                          27, &funcs.AICreate},
        {"AIPackages",           "AI packages",                                          11, &funcs.AIPackages},
        // Turret
        {"GunTurret",            "gun turret",                                           10, &funcs.GunTurret},
        // Building management
        {"BuildingDismantle",    "dismantle",                                             9, &funcs.BuildingDismantle},
        {"BuildingConstruct",    "construction progress",                                21, &funcs.BuildingConstruct},
    };

    // ES: Para cada entrada no resuelta: buscar por string, validar con .pdata (corrige
    //     desfases <= 0x10, descarta mayores) y, si falla, loguear el motivo (string no
    //     encontrado, sin xref o sin prólogo).
    for (auto& fb : fallbacks) {
        if (*fb.target != nullptr) continue; // Already resolved by pattern or orchestrator

        uintptr_t addr = rss.FindFunctionByString(fb.searchStr, fb.searchLen);
        if (addr) {
            // Cross-check with .pdata to ensure we found the real function entry
            DWORD64 imageBase = 0;
            auto* rtFunc = RtlLookupFunctionEntry(
                static_cast<DWORD64>(addr), &imageBase, nullptr);
            if (rtFunc) {
                uintptr_t funcStart = static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress;
                if (funcStart != addr) {
                    uintptr_t offset = addr - funcStart;
                    if (offset <= 0x10) {
                        spdlog::warn("ResolveGameFunctions: '{}' string fallback at 0x{:X} (offset +0x{:X}). "
                                     "Correcting to 0x{:X}", fb.label, addr, offset, funcStart);
                        addr = funcStart;
                    } else {
                        spdlog::error("ResolveGameFunctions: '{}' string fallback at 0x{:X} is +0x{:X} into "
                                      "function 0x{:X} — wrong function, skipping",
                                      fb.label, addr, offset, funcStart);
                        continue;
                    }
                }
            }
            *fb.target = reinterpret_cast<void*>(addr);
            fallbackResolved++;
            spdlog::info("ResolveGameFunctions: '{}' = 0x{:X} (string fallback)", fb.label, addr);
        } else {
            // ES: Diagnóstico de POR QUÉ falló el respaldo.
            // Diagnose WHY the string fallback failed
            uintptr_t strAddr = rss.FindStringInMemory(fb.searchStr, fb.searchLen);
            if (strAddr) {
                uintptr_t xref = rss.FindStringXref(strAddr);
                if (xref) {
                    spdlog::warn("ResolveGameFunctions: '{}' — string at 0x{:X}, xref at 0x{:X}, "
                                 "but FindFunctionStart failed (no prologue found)",
                                 fb.label, strAddr, xref);
                } else {
                    spdlog::warn("ResolveGameFunctions: '{}' — string at 0x{:X} but NO code xref "
                                 "(no LEA instruction references this string)", fb.label, strAddr);
                }
            } else {
                spdlog::warn("ResolveGameFunctions: '{}' — string '{}' NOT FOUND in binary",
                             fb.label, std::string(fb.searchStr, fb.searchLen));
            }
        }
    }

    // ES: Descubrimiento automático de punteros globales: en vez de fijar offsets por versión,
    //     se buscan referencias a .data cerca de strings conocidos o en funciones ya resueltas.
    // ── Auto-discover global pointers ──
    // Instead of hardcoding version-specific offsets, we find globals by
    // scanning for .data section references near known strings.

    // ES: ¿Parece un puntero de usuario válido? (> 0x10000, < 0x7FFFFFFFFFFF y distinto de los
    //     valores centinela 0xFF.., 0xCC.., 0xCD..).
    // Helper: validate that a pointer value looks like a real user-mode address.
    // On x64 Windows, user-mode addresses are below 0x00007FFFFFFFFFFF.
    // We also exclude very low addresses (< 0x10000) and uninitialized sentinels.
    auto isValidUserPtr = [](uintptr_t val) -> bool {
        return val > 0x10000 && val < 0x00007FFFFFFFFFFF &&
               val != 0xFFFFFFFFFFFFFFFF &&
               val != 0xCCCCCCCCCCCCCCCC &&
               val != 0xCDCDCDCDCDCDCDCD;
    };

    // ES: Descubrimiento de globales desensamblando funciones ya resueltas: busca la n-ésima
    //     MOV/LEA reg,[RIP+disp32] hacia .data/.rdata dentro de la función (fin según .pdata,
    //     o 4 KB si no hay entrada). Funciona igual en GOG y Steam.
    // ── Function-disassembly global discovery ──
    // The pattern scanner already resolved exact function addresses. We can scan
    // their code for MOV/LEA reg,[RIP+disp32] pointing into .data to find globals
    // like PlayerBase and GameWorld without hardcoded RVAs. This is the most
    // reliable approach since it works identically on GOG and Steam.
    auto findGlobalInFunction = [&](uintptr_t funcAddr, int nth) -> uintptr_t {
        if (!funcAddr) return 0;
        // Use .pdata to determine function end (accurate), fallback to 4KB scan.
        // 1KB was too small — many Kenshi functions are 2-8KB and load globals late.
        uintptr_t scanEnd = funcAddr + 4096;
        DWORD64 imageBase = 0;
        auto* rtFunc = RtlLookupFunctionEntry(
            static_cast<DWORD64>(funcAddr), &imageBase, nullptr);
        if (rtFunc) {
            uintptr_t pdataEnd = static_cast<uintptr_t>(imageBase) + rtFunc->EndAddress;
            if (pdataEnd > funcAddr && pdataEnd < funcAddr + 65536) {
                scanEnd = pdataEnd;
            }
        }
        return rss.ScanForGlobalLoad(funcAddr, scanEnd, nth, true);
    };

    // ES: Validación semántica de PlayerBase: es puntero-a-puntero (*PlayerBase -> objeto con
    //     vtable). El valor debe ser un puntero de heap (FUERA de la imagen del módulo) cuyo
    //     primer qword (vtable) apunte dentro del módulo. Valor 0 = partida sin cargar: no se
    //     acepta todavía. Nota: el rango "texto" usado es base+0x1000 .. fin del módulo, no solo .text.
    // ── Semantic validation: does this look like a real PlayerBase? ──
    // PlayerBase is a pointer-to-pointer: *PlayerBase -> object with vtable.
    // After game loads, the object should have a valid faction list at known offsets.
    // CRITICAL: The object must be HEAP-allocated (outside module image range).
    // .data addresses containing .text pointers (function ptrs, vtables) are NOT PlayerBase.
    auto validatePlayerBase = [&](uintptr_t candidateAddr) -> bool {
        if (candidateAddr == 0) return false;
        uintptr_t val = 0;
        if (!Memory::Read(candidateAddr, val)) return false;
        // Value=0 means game not loaded yet — NOT validated, but may be correct.
        // Don't accept zero as valid — wait for re-check after game loads.
        if (val == 0) return false;
        if (!isValidUserPtr(val)) return false;
        // MUST be outside module image — real game objects are heap-allocated.
        // .data globals containing .text/.rdata pointers are false positives.
        if (val >= base && val < base + moduleSize) return false;
        // Double-deref: object should have a readable vtable in .text range
        uintptr_t vtable = 0;
        if (!Memory::Read(val, vtable)) return false;
        // vtable should point into the module's .text section
        uintptr_t textStart = base + 0x1000; // .text is typically at base+0x1000
        uintptr_t textEnd = base + moduleSize;
        if (vtable < textStart || vtable >= textEnd) return false;
        return true;
    };

    // ES: PlayerBase: prioridad 1) desensamblado de funciones, 2) xref de strings, 3) RVA fija.
    //     ARREGLO STEAM: al principio la partida no está cargada y los globales valen 0, así
    //     que se hace en dos pasadas: estricta (puntero de heap válido) y tentativa (acepta
    //     valor 0 si la dirección está en .data), que RetryGlobalDiscovery revalida después.
    // PlayerBase: Find the global pointer that the squad/player code loads.
    // Priority: 1) function disassembly, 2) string-xref, 3) hardcoded (GOG only)
    //
    // STEAM FIX: During early init, the game hasn't loaded, so all globals are 0.
    // validatePlayerBase rejects val==0. We do a TWO-PASS approach:
    //   Pass 1: Strict (val must be valid heap pointer) — works on GOG and post-load Steam
    //   Pass 2: Tentative (accept val==0 if address is in .data section) — works on Steam pre-load
    // Pass 2 candidates are re-validated by RetryGlobalDiscovery after game loads.
    auto tentativeGlobalValidation = [&](uintptr_t candidateAddr) -> bool {
        if (candidateAddr == 0) return false;
        uintptr_t val = 0;
        if (!Memory::Read(candidateAddr, val)) return false;
        // Accept null (game not loaded yet) IF the address is in .data
        if (val == 0) {
            bool inData = rss.GetDataBase() != 0 &&
                          candidateAddr >= rss.GetDataBase() &&
                          candidateAddr < rss.GetDataBase() + rss.GetDataSize();
            return inData;
        }
        // Non-null: apply full validation
        if (!isValidUserPtr(val)) return false;
        if (val >= base && val < base + moduleSize) return false;
        uintptr_t vtable = 0;
        if (!Memory::Read(val, vtable)) return false;
        if (vtable < base + 0x1000 || vtable >= base + moduleSize) return false;
        return true;
    };

    // ES: Búsqueda de PlayerBase (solo si aún no está fijado).
    if (funcs.PlayerBase == 0) {
        // ES: Método 1: desensamblar CharacterSpawn, CharacterStats, SaveGame y LoadGame buscando
        //     globales en .data (las 16 primeras referencias de cada una).
        // Method 1: Scan resolved functions for .data globals (most reliable)
        // CharacterSpawn (RootObjectFactory::process) loads the factory singleton
        // which is often near or is PlayerBase.
        uintptr_t funcCandidates[] = {
            reinterpret_cast<uintptr_t>(funcs.CharacterSpawn),
            reinterpret_cast<uintptr_t>(funcs.CharacterStats),
            reinterpret_cast<uintptr_t>(funcs.SaveGame),
            reinterpret_cast<uintptr_t>(funcs.LoadGame),
        };
        const char* funcNames[] = { "CharacterSpawn", "CharacterStats", "SaveGame", "LoadGame" };

        // ES: Pasada 1: validación estricta.
        // Pass 1: Strict validation (non-null heap pointer with vtable)
        for (int fi = 0; fi < 4 && funcs.PlayerBase == 0; fi++) {
            if (!funcCandidates[fi]) continue;
            for (int nth = 0; nth < 16 && funcs.PlayerBase == 0; nth++) {
                uintptr_t candidate = findGlobalInFunction(funcCandidates[fi], nth);
                if (!candidate) continue;
                uintptr_t val = 0;
                Memory::Read(candidate, val);
                if (validatePlayerBase(candidate)) {
                    funcs.PlayerBase = candidate;
                    spdlog::info("ResolveGameFunctions: 'PlayerBase' = 0x{:X} (func-disasm via '{}', nth={}, -> 0x{:X})",
                                 candidate, funcNames[fi], nth, val);
                } else {
                    spdlog::debug("ResolveGameFunctions: PlayerBase candidate 0x{:X} rejected "
                                  "(func='{}', nth={}, val=0x{:X}, {})",
                                  candidate, funcNames[fi], nth, val,
                                  (val == 0) ? "zero" :
                                  (val >= base && val < base + moduleSize) ? "in-module" : "vtable-fail");
                }
            }
        }

        // ES: Método 2: xref de strings (sirve en cualquier versión). Ojo: aquí la longitud de
        //     "[Character::serialise] Character '" es 33 (sin la comilla final) y en la tabla de
        //     respaldo es 34; ambas funcionan porque es una búsqueda de prefijo.
        // Method 2: Runtime string-xref discovery (works on any version)
        if (funcs.PlayerBase == 0) {
            const char* playerAnchors[] = {
                "CharacterStats_Attributes",
                "Reset squad positions",
                "[Character::serialise] Character '",
                "[RootObjectFactory::process] Character",
            };
            int playerAnchorLens[] = { 25, 21, 33, 38 };

            for (int a = 0; a < 4 && funcs.PlayerBase == 0; a++) {
                for (int n = 0; n < 12 && funcs.PlayerBase == 0; n++) {
                    uintptr_t globalAddr = rss.FindGlobalNearString(
                        playerAnchors[a], playerAnchorLens[a], n, true);
                    if (globalAddr && validatePlayerBase(globalAddr)) {
                        uintptr_t val = 0;
                        Memory::Read(globalAddr, val);
                        funcs.PlayerBase = globalAddr;
                        spdlog::info("ResolveGameFunctions: 'PlayerBase' = 0x{:X} (string-xref via '{}', nth={}, -> 0x{:X})",
                                     globalAddr, playerAnchors[a], n, val);
                    }
                }
            }
        }

        // ES: Método 3: RVA fija de PlayerBase (0x01AC8A90, GOG/1.0.68), último recurso.
        // Method 3: Hardcoded GOG offset (last resort)
        if (funcs.PlayerBase == 0) {
            uintptr_t hardcoded = base + 0x01AC8A90;
            if (validatePlayerBase(hardcoded)) {
                uintptr_t val = 0;
                Memory::Read(hardcoded, val);
                funcs.PlayerBase = hardcoded;
                spdlog::info("ResolveGameFunctions: 'PlayerBase' = 0x{:X} (hardcoded GOG, -> 0x{:X})",
                             hardcoded, val);
            }
        }

        // ES: Pasada 2 (Steam antes de cargar): acepta globales de .data con valor nulo de forma
        //     provisional; RetryGlobalDiscovery los revalidará.
        // Pass 2 (Steam pre-load): Accept null-valued .data globals tentatively.
        // These will be re-validated by RetryGlobalDiscovery after the game loads.
        if (funcs.PlayerBase == 0) {
            spdlog::info("ResolveGameFunctions: PlayerBase not found (strict) — trying tentative (null-allowed)...");
            for (int fi = 0; fi < 4 && funcs.PlayerBase == 0; fi++) {
                if (!funcCandidates[fi]) continue;
                for (int nth = 0; nth < 16 && funcs.PlayerBase == 0; nth++) {
                    uintptr_t candidate = findGlobalInFunction(funcCandidates[fi], nth);
                    if (!candidate) continue;
                    if (tentativeGlobalValidation(candidate)) {
                        uintptr_t val = 0;
                        Memory::Read(candidate, val);
                        funcs.PlayerBase = candidate;
                        spdlog::info("ResolveGameFunctions: 'PlayerBase' = 0x{:X} (TENTATIVE, func='{}', nth={}, val=0x{:X}) — needs re-validation after game load",
                                     candidate, funcNames[fi], nth, val);
                    }
                }
            }

            // ES: Pasada tentativa por xref de strings.
            // Tentative string-xref pass
            if (funcs.PlayerBase == 0) {
                const char* playerAnchors[] = {
                    "CharacterStats_Attributes",
                    "Reset squad positions",
                    "[Character::serialise] Character '",
                    "[RootObjectFactory::process] Character",
                };
                int playerAnchorLens[] = { 25, 21, 33, 38 };

                for (int a = 0; a < 4 && funcs.PlayerBase == 0; a++) {
                    for (int n = 0; n < 12 && funcs.PlayerBase == 0; n++) {
                        uintptr_t globalAddr = rss.FindGlobalNearString(
                            playerAnchors[a], playerAnchorLens[a], n, true);
                        if (globalAddr && tentativeGlobalValidation(globalAddr)) {
                            uintptr_t val = 0;
                            Memory::Read(globalAddr, val);
                            funcs.PlayerBase = globalAddr;
                            spdlog::info("ResolveGameFunctions: 'PlayerBase' = 0x{:X} (TENTATIVE string-xref via '{}', nth={}, val=0x{:X})",
                                         globalAddr, playerAnchors[a], n, val);
                        }
                    }
                }
            }

            if (funcs.PlayerBase == 0) {
                spdlog::warn("ResolveGameFunctions: PlayerBase not found — will retry after game loads");
            }
        }
    }

    // ES: Singleton GameWorld: lo referencian las funciones de tiempo/velocidad/mundo.
    //     Prioridad: 1) desensamblado, 2) xref de strings, 3) RVA fija.
    // GameWorld singleton: referenced by time/speed/world management functions.
    // Priority: 1) function disassembly, 2) string-xref, 3) hardcoded (GOG only)
    // ── Helper: ¿es 'p' un puntero de heap válido del juego? ──
    // Mismo criterio que isValidUserPtr + alineado a 8 + FUERA de la imagen del módulo.
    // (Equivale a isValidHeapPtr de game_character.cpp, replicado aquí para el scanner.)
    // EN: Helper: is 'p' a valid game heap pointer? Same criteria as isValidUserPtr + 8-byte
    //     aligned + OUTSIDE the module image (equivalent to isValidHeapPtr in
    //     game_character.cpp, replicated here for the scanner).
    auto isHeapPtr = [&](uintptr_t p) -> bool {
        if (!isValidUserPtr(p)) return false;
        if ((p & 0x7) != 0) return false;              // objetos del juego alineados a 8
        if (p >= base && p < base + moduleSize) return false; // heap, no imagen
        return true;
    };

    // ── resolveGwObject (1.0.68: instancia embebida vs puntero clasico) ──
    // CRITICO Steam 1.0.68: GameWorld NO es un puntero global (GameWorld* ou); es la INSTANCIA
    // estatica embebida en .data. Por tanto el "candidato" base+0x2134110 ES directamente el
    // objeto GameWorld (su primer qword es la vtable en .text), NO un puntero a el.
    // Para ser robustos a version/plataforma, aceptamos AMBOS layouts:
    //   (a) puntero clasico : *candidateAddr es un heap-ptr -> el OBJETO es *candidateAddr
    //   (b) instancia directa: *candidateAddr es la vtable (.text) -> el OBJETO es candidateAddr
    // Devuelve la direccion del OBJETO GameWorld resuelto, o 0 si no encaja ninguno.
    // EN: resolveGwObject (1.0.68: embedded instance vs classic pointer). CRITICAL on Steam
    //     1.0.68: GameWorld is NOT a global pointer (GameWorld* ou); it is the static INSTANCE
    //     embedded in .data. So the candidate base+0x2134110 IS the GameWorld object itself
    //     (its first qword is the vtable in .text), NOT a pointer to it. To be robust across
    //     versions/platforms both layouts are accepted:
    //       (a) classic pointer: *candidateAddr is a heap ptr -> the OBJECT is *candidateAddr
    //       (b) direct instance: *candidateAddr is the vtable (.text) -> the OBJECT is candidateAddr
    //     Returns the address of the resolved GameWorld OBJECT, or 0 if neither fits.
    uintptr_t textStart = base + 0x1000;
    uintptr_t textEnd   = base + moduleSize;
    auto resolveGwObject = [&](uintptr_t candidateAddr) -> uintptr_t {
        if (candidateAddr == 0) return 0;
        uintptr_t first = 0;
        if (!Memory::Read(candidateAddr, first)) return 0;
        if (first == 0) return 0; // Game not loaded — don't accept yet
        // Caso (a): puntero clasico a un objeto de heap.
        if (isHeapPtr(first)) {
            uintptr_t vtable = 0;
            if (Memory::Read(first, vtable) && vtable >= textStart && vtable < textEnd)
                return first; // *candidateAddr es el objeto GameWorld (heap)
        }
        // Caso (b): instancia directa embebida — el primer qword YA es la vtable en .text.
        if (first >= textStart && first < textEnd)
            return candidateAddr; // candidateAddr ES el objeto GameWorld
        return 0;
    };

    // ── validateGameWorld (Fix 3: validador de CADENA, robusto a la versión) ──
    // El validador antiguo solo comprobaba "objeto de heap con vtable en .text" → daba
    // FALSOS POSITIVOS (aceptaba cualquier objeto del juego). Ahora exigimos que el
    // candidato sea realmente el GameWorld validando la cadena de 2-3 saltos que usa
    // GetPlayerFactionDirect (offsets KenshiLib, Kenshi 1.0.68). El objeto GameWorld se
    // resuelve con resolveGwObject (maneja instancia-directa vs puntero), luego:
    //   player (PlayerInterface*)  = *(gwObj + 0x580) (GameWorld.h:137)
    //   faction (Faction*)         = *(player + 0x2A0)(PlayerInterface.h:248)
    //   name (std::string ASCII)   =   faction + 0x1A8(Faction.h:147)  [refuerzo opcional]
    // Si la cadena no resuelve, NO es el GameWorld → se descarta el candidato. Esto deja
    // que el escáner encuentre el GameWorld REAL aunque el RVA hardcodeado no sea exacto.
    //
    // IMPORTANTE — reintentable: esta lambda se evalúa por-candidato en cada pasada y NO
    // cachea nada. Si en una carga temprana el player aún no existe, devuelve false sin
    // marcar el candidato como inválido permanente; RetryGlobalDiscovery lo reintenta más
    // tarde y entonces sí lo aceptará. No invalida para siempre.
    // EN: validateGameWorld (Fix 3: version-robust CHAIN validator). The old validator only
    //     checked "heap object with a vtable in .text", which gave FALSE POSITIVES (it accepted
    //     any game object). Now the candidate must really be the GameWorld by validating the
    //     2-3 hop chain used by GetPlayerFactionDirect (KenshiLib offsets, Kenshi 1.0.68):
    //       player (PlayerInterface*) = *(gwObj + 0x580)   (GameWorld.h:137)
    //       faction (Faction*)        = *(player + 0x2A0)  (PlayerInterface.h:248)
    //       name (ASCII std::string)  =   faction + 0x1A8  (Faction.h:147) [optional check]
    //     If the chain does not resolve, it is NOT the GameWorld and the candidate is dropped,
    //     so the scanner can find the REAL GameWorld even if the hardcoded RVA is off.
    //     Retryable: evaluated per candidate on every pass and caches nothing; if the player
    //     does not exist yet during early load it returns false without permanently
    //     invalidating the candidate, and RetryGlobalDiscovery accepts it later.
    auto validateGameWorld = [&](uintptr_t candidateAddr) -> bool {
        // gwObj = objeto GameWorld real (instancia directa o *puntero). 0 = no encaja/aún null.
        uintptr_t gwObj = resolveGwObject(candidateAddr);
        if (gwObj == 0) return false;

        // ── Salto 1: GameWorld + 0x580 -> player (PlayerInterface*) ──
        uintptr_t player = 0;
        if (!Memory::Read(gwObj + 0x580, player)) return false;
        if (!isHeapPtr(player)) return false;

        // ── Salto 2: player + 0x2A0 -> faction (Faction*) ──
        uintptr_t faction = 0;
        if (!Memory::Read(player + 0x2A0, faction)) return false;
        if (!isHeapPtr(faction)) return false;

        // ── Refuerzo opcional: el nombre en faction + 0x1A8 debe ser ASCII legible ──
        // std::string MSVC x64: si capacidad <= 15 el texto está inline en +0x00;
        // si no, el primer qword es el puntero al buffer en heap. Leemos hasta 8 bytes
        // del comienzo del buffer y exigimos algún carácter ASCII imprimible. Si la
        // lectura falla (carga muy temprana), NO invalidamos por ello: la cadena de 2
        // saltos ya es muy específica del GameWorld real. Solo rechazamos si leemos
        // basura binaria clara.
        // EN: Optional check: the name at faction + 0x1A8 must be readable ASCII. MSVC x64
        //     std::string: if capacity <= 15 the text is inline at +0x00, otherwise the first
        //     qword points to the heap buffer. Reads up to 8 bytes and rejects only clear binary
        //     garbage (< 60% printable); a failed read (very early load) does not invalidate.
        uintptr_t strField = faction + 0x1A8;
        uintptr_t cap = 0;
        Memory::Read(strField + 0x18, cap);            // capacity (uint64)
        uintptr_t bufAddr = strField;                  // SSO: texto inline
        if (cap > 15) {
            uintptr_t heapBuf = 0;
            if (Memory::Read(strField, heapBuf) && isValidUserPtr(heapBuf))
                bufAddr = heapBuf;                      // texto en heap
        }
        // Leemos 8 bytes del comienzo del buffer (Memory::Read con T POD usa SEH).
        struct Name8 { char c[8]; } name{};
        if (Memory::Read(bufAddr, name)) {
            int printable = 0, nonzero = 0;
            for (char c : name.c) {
                if (c == 0) break;
                nonzero++;
                if (static_cast<unsigned char>(c) >= 0x20 &&
                    static_cast<unsigned char>(c) <= 0x7E) printable++;
            }
            // Si hay bytes pero <60% imprimibles, es basura → no es Faction.name real.
            if (nonzero > 0 && printable * 100 < nonzero * 60) return false;
        }

        // Cadena de 2 saltos válida (y nombre no-basura): es el GameWorld REAL.
        return true;
    };

    // ES: Búsqueda de GameWorld (solo si aún no está fijado).
    if (funcs.GameWorldSingleton == 0) {
        // ES: Método 1: desensamblar TimeUpdate, ZoneLoad, GameFrameUpdate y SaveGame buscando
        //     globales y validando cada candidato con la cadena player/faction.
        // Method 1: Scan resolved functions for .data globals
        uintptr_t gwFuncCandidates[] = {
            reinterpret_cast<uintptr_t>(funcs.TimeUpdate),
            reinterpret_cast<uintptr_t>(funcs.ZoneLoad),
            reinterpret_cast<uintptr_t>(funcs.GameFrameUpdate),
            reinterpret_cast<uintptr_t>(funcs.SaveGame),
        };
        const char* gwFuncNames[] = { "TimeUpdate", "ZoneLoad", "GameFrameUpdate", "SaveGame" };

        for (int fi = 0; fi < 4 && funcs.GameWorldSingleton == 0; fi++) {
            if (!gwFuncCandidates[fi]) continue;
            for (int nth = 0; nth < 16 && funcs.GameWorldSingleton == 0; nth++) {
                uintptr_t candidate = findGlobalInFunction(gwFuncCandidates[fi], nth);
                if (!candidate) continue;
                uintptr_t val = 0;
                Memory::Read(candidate, val);
                if (validateGameWorld(candidate)) {
                    funcs.GameWorldSingleton = candidate;
                    spdlog::info("ResolveGameFunctions: 'GameWorldSingleton' = 0x{:X} (func-disasm via '{}', nth={}, -> 0x{:X})",
                                 candidate, gwFuncNames[fi], nth, val);
                } else {
                    spdlog::debug("ResolveGameFunctions: GameWorld candidate 0x{:X} rejected "
                                  "(func='{}', nth={}, val=0x{:X}, {})",
                                  candidate, gwFuncNames[fi], nth, val,
                                  (val == 0) ? "zero" :
                                  (val >= base && val < base + moduleSize) ? "in-module" : "vtable-fail");
                }
            }
        }

        // ES: Método 2: xref de strings. Ojo: "dayTime" NO existe en la v1.0.68 (según
        //     docs/03-scanner.md), así que en esa versión solo sirven los otros dos anclas.
        // Method 2: Runtime string-xref discovery
        if (funcs.GameWorldSingleton == 0) {
            const char* worldAnchors[] = { "dayTime", "zone.%d.%d.zone", "Kenshi 1.0." };
            int worldAnchorLens[] = { 7, 15, 11 };

            for (int a = 0; a < 3 && funcs.GameWorldSingleton == 0; a++) {
                for (int n = 0; n < 12 && funcs.GameWorldSingleton == 0; n++) {
                    uintptr_t globalAddr = rss.FindGlobalNearString(
                        worldAnchors[a], worldAnchorLens[a], n, true);
                    if (globalAddr && validateGameWorld(globalAddr)) {
                        uintptr_t val = 0;
                        Memory::Read(globalAddr, val);
                        funcs.GameWorldSingleton = globalAddr;
                        spdlog::info("ResolveGameFunctions: 'GameWorldSingleton' = 0x{:X} (string-xref via '{}', nth={}, -> 0x{:X})",
                                     globalAddr, worldAnchors[a], n, val);
                    }
                }
            }
        }

        // Method 3: Hardcoded offset (last resort)
        // RVA de la INSTANCIA GameWorld embebida en .data para Kenshi Steam 1.0.68.
        // En 1.0.68 GameWorld NO es un puntero (GameWorld* ou) sino la instancia misma:
        // base+0x2134110 ES el objeto (primer qword = vtable .text 0x1722608). validateGameWorld
        // ya maneja ambos casos (instancia directa / puntero clasico). Verificado RTTI/xref.
        // Historial: 0x2133040 (erroneo) -> 0x2131020 (era 1.0.65, NULL) -> 0x2134110 (1.0.68 OK).
        // EN: Method 3 detail: RVA of the GameWorld INSTANCE embedded in .data for Kenshi Steam
        //     1.0.68. On 1.0.68 GameWorld is NOT a pointer but the instance itself: base+0x2134110
        //     IS the object (first qword = .text vtable 0x1722608). validateGameWorld handles both
        //     cases. Verified via RTTI/xrefs. History: 0x2133040 (wrong) -> 0x2131020 (1.0.65 era,
        //     NULL) -> 0x2134110 (1.0.68 OK).
        if (funcs.GameWorldSingleton == 0) {
            uintptr_t hardcoded = base + 0x2134110;
            if (validateGameWorld(hardcoded)) {
                uintptr_t val = 0;
                Memory::Read(hardcoded, val);
                funcs.GameWorldSingleton = hardcoded;
                spdlog::info("ResolveGameFunctions: 'GameWorldSingleton' = 0x{:X} (hardcoded embedded-instance 1.0.68, *addr=0x{:X})",
                             hardcoded, val);
            }
        }

        // ES: Pasada 2 (Steam antes de cargar): globales de .data con valor nulo, provisionales.
        //     Nota: un candidato no nulo que sea la instancia embebida no pasa esta validación
        //     tentativa (exige puntero de heap); ese caso lo cubre la RVA fija del método 3.
        // Pass 2 (Steam pre-load): Accept null-valued .data globals tentatively.
        if (funcs.GameWorldSingleton == 0) {
            spdlog::info("ResolveGameFunctions: GameWorld not found (strict) — trying tentative (null-allowed)...");
            uintptr_t gwFuncCandidates2[] = {
                reinterpret_cast<uintptr_t>(funcs.TimeUpdate),
                reinterpret_cast<uintptr_t>(funcs.ZoneLoad),
                reinterpret_cast<uintptr_t>(funcs.GameFrameUpdate),
                reinterpret_cast<uintptr_t>(funcs.SaveGame),
            };
            const char* gwFuncNames2[] = { "TimeUpdate", "ZoneLoad", "GameFrameUpdate", "SaveGame" };

            for (int fi = 0; fi < 4 && funcs.GameWorldSingleton == 0; fi++) {
                if (!gwFuncCandidates2[fi]) continue;
                for (int nth = 0; nth < 16 && funcs.GameWorldSingleton == 0; nth++) {
                    uintptr_t candidate = findGlobalInFunction(gwFuncCandidates2[fi], nth);
                    if (!candidate) continue;
                    if (tentativeGlobalValidation(candidate)) {
                        uintptr_t val = 0;
                        Memory::Read(candidate, val);
                        funcs.GameWorldSingleton = candidate;
                        spdlog::info("ResolveGameFunctions: 'GameWorldSingleton' = 0x{:X} (TENTATIVE, func='{}', nth={}, val=0x{:X})",
                                     candidate, gwFuncNames2[fi], nth, val);
                    }
                }
            }

            // ES: Pasada tentativa por xref de strings.
            // Tentative string-xref pass
            if (funcs.GameWorldSingleton == 0) {
                const char* worldAnchors2[] = { "dayTime", "zone.%d.%d.zone", "Kenshi 1.0." };
                int worldAnchorLens2[] = { 7, 15, 11 };

                for (int a = 0; a < 3 && funcs.GameWorldSingleton == 0; a++) {
                    for (int n = 0; n < 12 && funcs.GameWorldSingleton == 0; n++) {
                        uintptr_t globalAddr = rss.FindGlobalNearString(
                            worldAnchors2[a], worldAnchorLens2[a], n, true);
                        if (globalAddr && tentativeGlobalValidation(globalAddr)) {
                            uintptr_t val = 0;
                            Memory::Read(globalAddr, val);
                            funcs.GameWorldSingleton = globalAddr;
                            spdlog::info("ResolveGameFunctions: 'GameWorldSingleton' = 0x{:X} (TENTATIVE string-xref via '{}', nth={}, val=0x{:X})",
                                         globalAddr, worldAnchors2[a], n, val);
                        }
                    }
                }
            }

            if (funcs.GameWorldSingleton == 0) {
                spdlog::warn("ResolveGameFunctions: GameWorld not found — will retry after game loads");
            }
        }
    }

    // ES: Resumen en el log y resultado: ¿hay lo mínimo resuelto para arrancar?
    int totalResolved = resolved + fallbackResolved;
    spdlog::info("ResolveGameFunctions: Resolved {} pattern + {} fallback = {} total, PlayerBase=0x{:X}",
                 resolved, fallbackResolved, totalResolved, funcs.PlayerBase);

    return funcs.IsMinimallyResolved();
}

// ES: Reintento de descubrimiento de globales tras cargar la partida (los valores ya no
//     son nulos). Revalida PlayerBase (heap + vtable) y GameWorld (cadena completa); si no
//     son válidos los vuelve a buscar por desensamblado y strings (sin RVA fija).
//     Devuelve true si hay PlayerBase (encontrar GameWorld no cambia el resultado).
// EN: Global discovery retry after the game has loaded (values are no longer null).
//     Re-validates PlayerBase (heap + vtable) and GameWorld (full chain); if invalid, searches
//     them again via disassembly and strings (no fixed RVA). Returns true if PlayerBase
//     exists (finding GameWorld does not change the result).
bool RetryGlobalDiscovery(const PatternScanner& scanner, GameFunctions& funcs) {
    uintptr_t base = scanner.GetBase();
    size_t moduleSize = scanner.GetSize();
    RuntimeStringScanner rss(base, moduleSize);
    bool found = false;

    // ES: Mismo filtro de puntero de usuario válido que en ResolveGameFunctions.
    auto isValidUserPtr = [](uintptr_t val) -> bool {
        return val > 0x10000 && val < 0x00007FFFFFFFFFFF &&
               val != 0xFFFFFFFFFFFFFFFF &&
               val != 0xCCCCCCCCCCCCCCCC &&
               val != 0xCDCDCDCDCDCDCDCD;
    };

    // ES: Validación semántica: objeto de heap con vtable dentro del módulo.
    // Semantic validation: heap-allocated object with vtable in .text
    auto validateGlobal = [&](uintptr_t candidateAddr) -> bool {
        if (candidateAddr == 0) return false;
        uintptr_t val = 0;
        if (!Memory::Read(candidateAddr, val)) return false;
        if (!isValidUserPtr(val)) return false;
        // MUST be outside module image — real game objects are heap-allocated
        if (val >= base && val < base + moduleSize) return false;
        uintptr_t vtable = 0;
        if (!Memory::Read(val, vtable)) return false;
        uintptr_t textStart = base + 0x1000;
        uintptr_t textEnd = base + moduleSize;
        return (vtable >= textStart && vtable < textEnd);
    };

    // ── isHeapPtr / validateGameWorldChain (Fix 3, también en el reintento post-carga) ──
    // RetryGlobalDiscovery corre DESPUÉS de cargar la partida, cuando player/faction ya
    // existen: es el mejor momento para exigir la cadena completa y DESCARTAR el falso
    // positivo que el Pass 2 tentativo pudo dejar fijado. Misma cadena que
    // GetPlayerFactionDirect: gwObj -> +0x580 (player) -> +0x2A0 (faction), nombre +0x1A8.
    // EN: isHeapPtr / validateGameWorldChain (Fix 3, also in the post-load retry).
    //     RetryGlobalDiscovery runs AFTER the game has loaded, when player/faction exist: the
    //     best moment to require the full chain and DROP the false positive the tentative
    //     Pass 2 may have set. Same chain as GetPlayerFactionDirect:
    //     gwObj -> +0x580 (player) -> +0x2A0 (faction), name at +0x1A8.
    auto isHeapPtr = [&](uintptr_t p) -> bool {
        if (!isValidUserPtr(p)) return false;
        if ((p & 0x7) != 0) return false;
        if (p >= base && p < base + moduleSize) return false;
        return true;
    };
    // resolveGwObject: mismo criterio que en ResolveGameFunctions — acepta instancia
    // embebida (1.0.68: *candidateAddr ES la vtable .text) o puntero clasico (*candidateAddr
    // es heap-ptr al objeto). Devuelve la addr del OBJETO GameWorld, o 0 si no encaja.
    // EN: resolveGwObject: same criteria as in ResolveGameFunctions — accepts the embedded
    //     instance (1.0.68: *candidateAddr IS the .text vtable) or a classic pointer
    //     (*candidateAddr is a heap ptr to the object). Returns the OBJECT address, or 0.
    uintptr_t textStart = base + 0x1000;
    uintptr_t textEnd   = base + moduleSize;
    auto resolveGwObject = [&](uintptr_t candidateAddr) -> uintptr_t {
        if (candidateAddr == 0) return 0;
        uintptr_t first = 0;
        if (!Memory::Read(candidateAddr, first) || first == 0) return 0;
        if (isHeapPtr(first)) { // caso (a): puntero clasico
            uintptr_t vtable = 0;
            if (Memory::Read(first, vtable) && vtable >= textStart && vtable < textEnd)
                return first;
        }
        if (first >= textStart && first < textEnd) // caso (b): instancia directa
            return candidateAddr;
        return 0;
    };
    // ES: Cadena GameWorld -> +0x580 player -> +0x2A0 facción (+ nombre en +0x1A8), igual que
    //     validateGameWorld de ResolveGameFunctions.
    // EN: GameWorld -> +0x580 player -> +0x2A0 faction chain (+ name at +0x1A8), same as
    //     validateGameWorld in ResolveGameFunctions.
    auto validateGameWorldChain = [&](uintptr_t candidateAddr) -> bool {
        uintptr_t gwObj = resolveGwObject(candidateAddr);
        if (gwObj == 0) return false;
        // Salto 1: GameWorld + 0x580 -> player (PlayerInterface*)
        uintptr_t player = 0;
        if (!Memory::Read(gwObj + 0x580, player) || !isHeapPtr(player)) return false;
        // Salto 2: player + 0x2A0 -> faction (Faction*)
        uintptr_t faction = 0;
        if (!Memory::Read(player + 0x2A0, faction) || !isHeapPtr(faction)) return false;
        // Refuerzo opcional: nombre ASCII legible en faction + 0x1A8 (std::string MSVC).
        uintptr_t strField = faction + 0x1A8;
        uintptr_t cap = 0;
        Memory::Read(strField + 0x18, cap);
        uintptr_t bufAddr = strField;
        if (cap > 15) {
            uintptr_t heapBuf = 0;
            if (Memory::Read(strField, heapBuf) && isValidUserPtr(heapBuf)) bufAddr = heapBuf;
        }
        struct Name8 { char c[8]; } name{};
        if (Memory::Read(bufAddr, name)) {
            int printable = 0, nonzero = 0;
            for (char c : name.c) {
                if (c == 0) break;
                nonzero++;
                if (static_cast<unsigned char>(c) >= 0x20 &&
                    static_cast<unsigned char>(c) <= 0x7E) printable++;
            }
            if (nonzero > 0 && printable * 100 < nonzero * 60) return false;
        }
        return true;
    };

    // ES: Igual que en ResolveGameFunctions: n-ésimo global cargado dentro de una función.
    // EN: Same as in ResolveGameFunctions: nth global loaded inside a function.
    auto findGlobalInFunction = [&](uintptr_t funcAddr, int nth) -> uintptr_t {
        if (!funcAddr) return 0;
        // Use .pdata to determine function end (accurate), fallback to 4KB scan.
        // 1KB was too small — many Kenshi functions are 2-8KB and load globals late.
        uintptr_t scanEnd = funcAddr + 4096;
        DWORD64 imageBase = 0;
        auto* rtFunc = RtlLookupFunctionEntry(
            static_cast<DWORD64>(funcAddr), &imageBase, nullptr);
        if (rtFunc) {
            uintptr_t pdataEnd = static_cast<uintptr_t>(imageBase) + rtFunc->EndAddress;
            if (pdataEnd > funcAddr && pdataEnd < funcAddr + 65536) {
                scanEnd = pdataEnd;
            }
        }
        return rss.ScanForGlobalLoad(funcAddr, scanEnd, nth, true);
    };

    // ES: Reintento de PlayerBase: si el actual ya es válido se conserva; si no, se pone a 0
    //     y se vuelve a buscar.
    // ── PlayerBase retry ──
    if (funcs.PlayerBase != 0) {
        if (validateGlobal(funcs.PlayerBase)) {
            uintptr_t val = 0;
            Memory::Read(funcs.PlayerBase, val);
            spdlog::info("RetryGlobalDiscovery: PlayerBase 0x{:X} now valid -> 0x{:X}", funcs.PlayerBase, val);
        } else {
            spdlog::info("RetryGlobalDiscovery: PlayerBase 0x{:X} still invalid — re-scanning...", funcs.PlayerBase);
            funcs.PlayerBase = 0;
        }
    }

    if (funcs.PlayerBase == 0) {
        // ES: Método 1: desensamblado de funciones.
        // Method 1: Function disassembly (most reliable after game load)
        uintptr_t funcCandidates[] = {
            reinterpret_cast<uintptr_t>(funcs.CharacterSpawn),
            reinterpret_cast<uintptr_t>(funcs.CharacterStats),
            reinterpret_cast<uintptr_t>(funcs.SaveGame),
            reinterpret_cast<uintptr_t>(funcs.LoadGame),
        };
        const char* funcNames[] = { "CharacterSpawn", "CharacterStats", "SaveGame", "LoadGame" };

        for (int fi = 0; fi < 4 && funcs.PlayerBase == 0; fi++) {
            if (!funcCandidates[fi]) continue;
            for (int nth = 0; nth < 16 && funcs.PlayerBase == 0; nth++) {
                uintptr_t candidate = findGlobalInFunction(funcCandidates[fi], nth);
                if (candidate && validateGlobal(candidate)) {
                    uintptr_t val = 0;
                    Memory::Read(candidate, val);
                    funcs.PlayerBase = candidate;
                    found = true;
                    spdlog::info("RetryGlobalDiscovery: 'PlayerBase' = 0x{:X} (func-disasm via '{}', nth={}, -> 0x{:X})",
                                 candidate, funcNames[fi], nth, val);
                }
            }
        }

        // ES: Método 2: xref de strings (añade "quicksave" como quinto ancla).
        // Method 2: String-xref fallback
        if (funcs.PlayerBase == 0) {
            const char* playerAnchors[] = {
                "CharacterStats_Attributes",
                "Reset squad positions",
                "[Character::serialise] Character '",
                "[RootObjectFactory::process] Character",
                "quicksave",
            };
            int playerAnchorLens[] = { 25, 21, 33, 38, 9 };

            for (int a = 0; a < 5 && funcs.PlayerBase == 0; a++) {
                for (int n = 0; n < 12 && funcs.PlayerBase == 0; n++) {
                    uintptr_t globalAddr = rss.FindGlobalNearString(
                        playerAnchors[a], playerAnchorLens[a], n, true);
                    if (globalAddr && validateGlobal(globalAddr)) {
                        uintptr_t val = 0;
                        Memory::Read(globalAddr, val);
                        funcs.PlayerBase = globalAddr;
                        found = true;
                        spdlog::info("RetryGlobalDiscovery: 'PlayerBase' = 0x{:X} (string-xref via '{}', nth={}, -> 0x{:X})",
                                     globalAddr, playerAnchors[a], n, val);
                    }
                }
            }
        }
    }

    // ── GameWorld retry ──
    // Revalidamos con la CADENA (no solo vtable): si el Pass 2 tentativo fijó un falso
    // positivo, aquí (ya cargada la partida) la cadena player/faction lo descarta y se
    // vuelve a escanear para encontrar el GameWorld real.
    // EN: GameWorld retry: re-validate with the CHAIN (not just the vtable). If the tentative
    //     Pass 2 fixed a false positive, here (game already loaded) the player/faction chain
    //     drops it and a rescan finds the real GameWorld.
    if (funcs.GameWorldSingleton != 0) {
        if (validateGameWorldChain(funcs.GameWorldSingleton)) {
            // Already good — cadena player/faction válida
        } else if (resolveGwObject(funcs.GameWorldSingleton) != 0) {
            // ── FIX connected-then-load (entities=0 / tracked:0) ──────────────────────
            // El candidato es ESTRUCTURALMENTE un GameWorld (instancia embebida con vtable
            // en .text), pero la sub-cadena player/faction (GW+0x580 -> +0x2A0) AÚN no
            // resuelve en este instante. Esto pasa en el flujo "connected-then-load":
            // RetryGlobalDiscovery corre justo al terminar la carga, ANTES de que
            // PlayerInterface/faction estén enlazados. ANTES borrábamos a 0 -> el bridge de
            // GameWorld nunca se seteaba -> CharacterIterator Strategy 2 jamás corría ->
            // entities=0 para siempre. Como la instancia embebida (base+0x2134110) NUNCA se
            // mueve y su validez de vtable es estable, CONSERVAMOS el candidato. La cadena
            // player/faction se revalida por-tick dentro de CharacterIterator y
            // GetPlayerFactionDirect cuando ya esté poblada.
            // EN: FIX connected-then-load (entities=0 / tracked:0): the candidate is STRUCTURALLY a
            //     GameWorld (embedded instance with a .text vtable), but the player/faction sub-chain
            //     (GW+0x580 -> +0x2A0) does NOT resolve yet. This happens in the "connected-then-load"
            //     flow: RetryGlobalDiscovery runs right after loading ends, BEFORE PlayerInterface/
            //     faction are linked. Before, it was reset to 0, so the GameWorld bridge was never set,
            //     CharacterIterator Strategy 2 never ran and entities stayed 0 forever. Since the
            //     embedded instance (base+0x2134110) NEVER moves and its vtable validity is stable, the
            //     candidate is KEPT. The player/faction chain is re-validated per tick inside
            //     CharacterIterator and GetPlayerFactionDirect once populated.
            spdlog::warn("RetryGlobalDiscovery: GameWorld 0x{:X} con vtable válida pero cadena "
                         "player/faction aún sin poblar — CONSERVANDO candidato (se revalida por tick)",
                         funcs.GameWorldSingleton);
        } else {
            spdlog::info("RetryGlobalDiscovery: GameWorld 0x{:X} no es instancia GameWorld — re-escaneando...",
                         funcs.GameWorldSingleton);
            funcs.GameWorldSingleton = 0;
        }
    }

    // ES: Búsqueda de GameWorld: método 1 desensamblado, método 2 strings (sin RVA fija aquí).
    if (funcs.GameWorldSingleton == 0) {
        // Method 1: Function disassembly
        uintptr_t gwFuncCandidates[] = {
            reinterpret_cast<uintptr_t>(funcs.TimeUpdate),
            reinterpret_cast<uintptr_t>(funcs.ZoneLoad),
            reinterpret_cast<uintptr_t>(funcs.GameFrameUpdate),
            reinterpret_cast<uintptr_t>(funcs.SaveGame),
        };
        const char* gwFuncNames[] = { "TimeUpdate", "ZoneLoad", "GameFrameUpdate", "SaveGame" };

        for (int fi = 0; fi < 4 && funcs.GameWorldSingleton == 0; fi++) {
            if (!gwFuncCandidates[fi]) continue;
            for (int nth = 0; nth < 16 && funcs.GameWorldSingleton == 0; nth++) {
                uintptr_t candidate = findGlobalInFunction(gwFuncCandidates[fi], nth);
                if (candidate && validateGameWorldChain(candidate)) {
                    uintptr_t val = 0;
                    Memory::Read(candidate, val);
                    funcs.GameWorldSingleton = candidate;
                    spdlog::info("RetryGlobalDiscovery: 'GameWorldSingleton' = 0x{:X} (func-disasm via '{}', nth={}, -> 0x{:X})",
                                 candidate, gwFuncNames[fi], nth, val);
                }
            }
        }

        // Method 2: String-xref fallback
        if (funcs.GameWorldSingleton == 0) {
            const char* worldAnchors[] = { "dayTime", "zone.%d.%d.zone", "Kenshi 1.0." };
            int worldAnchorLens[] = { 7, 15, 11 };

            for (int a = 0; a < 3 && funcs.GameWorldSingleton == 0; a++) {
                for (int n = 0; n < 12 && funcs.GameWorldSingleton == 0; n++) {
                    uintptr_t globalAddr = rss.FindGlobalNearString(
                        worldAnchors[a], worldAnchorLens[a], n, true);
                    if (globalAddr && validateGameWorldChain(globalAddr)) {
                        uintptr_t val = 0;
                        Memory::Read(globalAddr, val);
                        funcs.GameWorldSingleton = globalAddr;
                        spdlog::info("RetryGlobalDiscovery: 'GameWorldSingleton' = 0x{:X} (string-xref via '{}', nth={}, -> 0x{:X})",
                                     globalAddr, worldAnchors[a], n, val);
                    }
                }
            }
        }
    }

    // ES: Resultado: true si se encontró un PlayerBase nuevo o ya había uno válido.
    return found || (funcs.PlayerBase != 0);
}

} // namespace kmp
