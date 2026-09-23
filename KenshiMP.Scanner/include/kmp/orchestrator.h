// ES: orchestrator.h — PatternOrchestrator, el "camino A" para resolver en runtime las
//     direcciones de funciones y globales de kenshi_x64.exe. Mantiene un registro de
//     entradas (una por función/puntero a descubrir) y ejecuta un pipeline de fases:
//     1 .pdata, 2 strings+xrefs, 3 vtables/RTTI, 4 escaneo AOB por lotes, 5 fallbacks
//     (string, vtable, RVA fija, patrón complejo), 6 grafo de llamadas, 7 punteros globales
//     y 8 emergencia para entradas críticas. Cada entrada guarda método y confianza, y al
//     resolverse escribe la dirección en el campo correspondiente de GameFunctions.
//     (El comentario en inglés de abajo solo enumera 7 fases; existe también la fase 8.)
// EN: orchestrator.h — PatternOrchestrator, "path A" to resolve at runtime the addresses
//     of kenshi_x64.exe functions and globals. It keeps a registry of entries (one per
//     function/pointer to discover) and runs a phase pipeline: 1 .pdata, 2 strings+xrefs,
//     3 vtables/RTTI, 4 batch AOB scan, 5 fallbacks (string, vtable, fixed RVA, complex
//     pattern), 6 call graph, 7 global pointers and 8 emergency for critical entries. Each
//     entry stores method and confidence and, once resolved, writes the address into the
//     matching GameFunctions field. (The English comment below only lists 7 phases;
//     phase 8 also exists.)
#pragma once
// ES: Orquestador de patrones (descripción original en inglés abajo).
// Pattern Orchestrator — Central Intelligence for Game Reverse Engineering
//
// Manages all discovery phases in a pipeline:
//   Phase 1: PE analysis (.pdata enumeration, section mapping)
//   Phase 2: String discovery and cross-reference analysis
//   Phase 3: VTable scanning and RTTI class hierarchy
//   Phase 4: Pattern-based function resolution (SIMD batch scan)
//   Phase 5: String xref fallback for unresolved patterns
//   Phase 6: Call graph analysis and label propagation
//   Phase 7: Global pointer discovery
//
// Features:
//   - Data-driven pattern registry (add/remove patterns at runtime)
//   - Multi-method resolution with confidence tracking
//   - Per-pattern retry and deferred resolution
//   - Comprehensive reporting and statistics
//   - Individual section/component queries

#include "kmp/scanner_engine.h"
#include "kmp/pdata_enumerator.h"
#include "kmp/string_analyzer.h"
#include "kmp/vtable_scanner.h"
#include "kmp/call_graph.h"
#include "kmp/patterns.h"

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <chrono>

namespace kmp {

// ES: Entrada de patrón: una función o puntero a descubrir.
// ═══════════════════════════════════════════════════════════════════════════
//  PATTERN ENTRY — A single function/pointer to discover
// ═══════════════════════════════════════════════════════════════════════════

// ES: Método con el que se resolvió una entrada: patrón AOB, xref de string, slot de
//     vtable, grafo de llamadas, RVA fija (depende de versión), .pdata, patrón complejo o manual.
enum class ResolutionMethod {
    None,
    PatternScan,        // IDA-style byte pattern match
    StringXref,         // String cross-reference to function start
    VTableSlot,         // Virtual function from RTTI vtable
    CallGraphTrace,     // Found via call graph from a known function
    HardcodedOffset,    // Hardcoded RVA offset (version-specific)
    PDataLookup,        // Direct .pdata lookup by RVA
    ComplexPattern,     // Multi-component composed pattern
    Manual,             // Manually set by user code
};

// ES: Nombre legible del método (para logs).
// EN: Human-readable method name (for logs).
const char* ResolutionMethodName(ResolutionMethod method);

// ES: Una entrada del registro: identidad, cómo intentar resolverla (patrón AOB, string
//     ancla, RVA fija, clase+slot de vtable, patrón complejo), estado de la resolución y
//     dónde escribir el resultado (puntero en GameFunctions).
// EN: A registry entry: identity, how to try to resolve it (AOB pattern, anchor string,
//     fixed RVA, vtable class+slot, complex pattern), resolution state and where to write
//     the result (pointer inside GameFunctions).
struct PatternEntry {
    // ── Identity ──
    std::string id;                 // Unique identifier (e.g., "CharacterSpawn")
    std::string category;           // Category (e.g., "entity", "combat", "ai")
    std::string description;        // Human-readable description

    // ES: Configuración de resolución. hardcodedRVA = dirección relativa a la base del módulo
    //     para la versión conocida (0 = desconocida); solo se usa como último recurso.
    // ── Resolution Config ──
    const char* pattern         = nullptr;  // IDA byte pattern (can be nullptr)
    const char* stringAnchor    = nullptr;  // Fallback string to search for
    int         stringAnchorLen = 0;
    uint32_t    hardcodedRVA    = 0;        // Version-specific RVA (0 = unknown)
    std::string vtableClass;                // VTable class name (for vtable resolution)
    int         vtableSlot      = -1;       // VTable slot index

    // ── Complex pattern support ──
    ComplexPattern complexPattern;

    // ES: Estado: dirección resuelta, método, confianza, si es puntero global, si es crítica
    //     (activa fallbacks agresivos) y reintentos.
    // ── Resolution State ──
    uintptr_t       resolvedAddress = 0;
    ResolutionMethod resolvedMethod  = ResolutionMethod::None;
    float           confidence      = 0.0f;
    bool            isResolved      = false;
    bool            isGlobalPointer = false; // True if this is a global pointer, not a function
    bool            critical        = false; // Must-resolve: enables aggressive fallbacks
    int             retryCount      = 0;

    // ES: Destino donde escribir la dirección resuelta (función o puntero global).
    // ── Target pointer (for GameFunctions integration) ──
    void**          targetPtr       = nullptr;    // If set, writes resolved address here
    uintptr_t*      targetUintptr   = nullptr;    // For global pointers

    // ES: ¿Falta por resolver?
    // EN: Still unresolved?
    bool NeedsResolution() const { return !isResolved; }
};

// ES: Configuración del orquestador: qué fases activar, longitud mínima de string,
//     profundidad de propagación, reintentos y opciones de rendimiento.
// ═══════════════════════════════════════════════════════════════════════════
//  ORCHESTRATOR CONFIGURATION
// ═══════════════════════════════════════════════════════════════════════════

struct OrchestratorConfig {
    bool enablePData        = true;
    bool enableStrings      = true;
    bool enableVTables      = true;
    bool enableCallGraph    = true;
    bool enableBatchScan    = true;
    bool enableLabelPropagation = true;

    int  stringMinLength    = 4;
    int  callGraphDepth     = 2;    // Label propagation depth
    int  maxRetries         = 3;

    // Performance tuning
    bool scanWideStrings    = false; // UTF-16 strings (slower)
    bool fullCallGraph      = false; // Full graph vs targeted graph
};

// ES: Informe: tiempos por fase, recuento por método de resolución, estadísticas de
//     descubrimiento y lista de entradas fallidas.
// ═══════════════════════════════════════════════════════════════════════════
//  ORCHESTRATOR REPORT
// ═══════════════════════════════════════════════════════════════════════════

struct OrchestratorReport {
    // Timing
    struct PhaseTimingMs {
        double pdata        = 0;
        double strings      = 0;
        double vtables      = 0;
        double patternScan  = 0;
        double stringFallback = 0;
        double callGraph    = 0;
        double globalPtrs   = 0;
        double total        = 0;
    } timing;

    // Resolution counts
    int totalEntries        = 0;
    int resolvedByPattern   = 0;
    int resolvedByString    = 0;
    int resolvedByVTable    = 0;
    int resolvedByCallGraph = 0;
    int resolvedByHardcoded = 0;
    int resolvedByComplex   = 0;
    int totalResolved       = 0;
    int totalFailed         = 0;

    // Discovery stats
    size_t pdataFunctions   = 0;
    size_t stringsFound     = 0;
    size_t xrefsResolved    = 0;
    size_t vtablesFound     = 0;
    size_t callGraphNodes   = 0;
    size_t callGraphEdges   = 0;
    size_t labeledFunctions = 0;

    // Failed entries
    std::vector<std::string> failedEntries;
};

// ES: El orquestador.
// ═══════════════════════════════════════════════════════════════════════════
//  THE ORCHESTRATOR
// ═══════════════════════════════════════════════════════════════════════════

// ES: Coordina todos los analizadores (escáner, .pdata, strings, vtables, grafo).
// EN: Coordinates all the analyzers (scanner, .pdata, strings, vtables, graph).
class PatternOrchestrator {
public:
    PatternOrchestrator() = default;

    // ES: Inicializa el motor de escaneo para el módulo y guarda la configuración.
    // ── Initialization ──
    bool Init(const char* moduleName = nullptr, const OrchestratorConfig& config = {});

    // ── Pattern Registry ──

    // ES: Registro de patrones: añadir entrada, registrar todas las de Kenshi (enlazadas a
    //     los campos de GameFunctions), quitar, consultar y filtrar por categoría.
    // EN: Pattern registry: add entry, register all Kenshi built-ins (bound to GameFunctions
    //     fields), remove, query and filter by category.
    // Register a pattern entry for discovery
    void Register(PatternEntry entry);

    // Register all built-in Kenshi patterns (from patterns.h)
    void RegisterBuiltinPatterns(GameFunctions& funcs);

    // Remove a pattern entry
    void Unregister(const std::string& id);

    // Get a registered entry
    const PatternEntry* GetEntry(const std::string& id) const;
    PatternEntry* GetMutableEntry(const std::string& id);

    // Get all entries
    const std::vector<PatternEntry>& GetEntries() const { return m_entries; }

    // Get entries by category
    std::vector<const PatternEntry*> GetByCategory(const std::string& category) const;

    // ── Execution ──

    // ES: Ejecución: pipeline completo (Run) o fases sueltas, y reintentos de las fallidas.
    // EN: Execution: full pipeline (Run) or single phases, and retries of failed entries.
    // Run the full discovery pipeline
    OrchestratorReport Run();

    // Run individual phases (for incremental/retry)
    void RunPhase1_PData();
    void RunPhase2_Strings();
    void RunPhase3_VTables();
    void RunPhase4_PatternScan();
    void RunPhase5_StringFallback();
    void RunPhase6_CallGraph();
    void RunPhase7_GlobalPointers();
    void RunPhase8_EmergencyCritical();

    // Retry resolution for failed entries
    int RetryFailed();

    // Retry a specific entry
    bool RetryEntry(const std::string& id);

    // ── Query API ──

    // ES: Consultas por id: dirección, si está resuelta, método, confianza y recuentos.
    // EN: Queries by id: address, resolved flag, method, confidence and counts.
    // Resolve a single address by ID
    uintptr_t GetAddress(const std::string& id) const;

    // Check if resolved
    bool IsResolved(const std::string& id) const;

    // Get resolution method used
    ResolutionMethod GetMethod(const std::string& id) const;

    // Get confidence
    float GetConfidence(const std::string& id) const;

    // Count resolved
    int CountResolved() const;
    int CountTotal() const { return static_cast<int>(m_entries.size()); }

    // ES: Acceso a los componentes internos (solo lectura y mutable).
    // EN: Access to the internal components (read-only and mutable).
    // ── Component Access ──
    const ScannerEngine&     GetScanner()       const { return m_scanner; }
    const PDataEnumerator&   GetPData()         const { return m_pdata; }
    const StringAnalyzer&    GetStringAnalyzer() const { return m_strings; }
    const VTableScanner&     GetVTableScanner()  const { return m_vtables; }
    const CallGraphAnalyzer& GetCallGraph()      const { return m_callGraph; }

    // Non-const access for advanced usage
    ScannerEngine&     GetMutableScanner()  { return m_scanner; }
    PDataEnumerator&   GetMutablePData()    { return m_pdata; }
    StringAnalyzer&    GetMutableStrings()  { return m_strings; }
    VTableScanner&     GetMutableVTables()  { return m_vtables; }
    CallGraphAnalyzer& GetMutableCallGraph() { return m_callGraph; }

    // ES: Informe y funciones descubiertas más allá del registro (por strings), búsqueda de
    //     funciones por nombre y strings de una función.
    // EN: Report and functions discovered beyond the registry (via strings), function lookup
    //     by name and strings of a function.
    // ── Reporting ──
    OrchestratorReport GenerateReport() const;
    void LogReport(const OrchestratorReport& report) const;

    // ── Discovered Functions (beyond registered patterns) ──
    // All functions discovered via string analysis
    const std::vector<LabeledFunction>& GetDiscoveredFunctions() const;

    // Find any function by name across all analyzers
    uintptr_t FindFunction(const std::string& name) const;

    // Get all strings referencing a function
    std::vector<std::string> GetFunctionStrings(uintptr_t addr) const;

private:
    // ES: Componentes, entradas con índice id -> posición, último informe y estado.
    // EN: Components, entries with id -> index map, last report and state.
    OrchestratorConfig  m_config;
    ScannerEngine       m_scanner;
    PDataEnumerator     m_pdata;
    StringAnalyzer      m_strings;
    VTableScanner       m_vtables;
    CallGraphAnalyzer   m_callGraph;

    std::vector<PatternEntry> m_entries;
    std::unordered_map<std::string, size_t> m_entryIndex; // id → index

    OrchestratorReport m_lastReport;
    bool m_initialized = false;

    // ES: Medición de tiempos.
    // EN: Timing helper.
    // Timing helper
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    double ElapsedMs(TimePoint start) const;

    // ES: Métodos internos de resolución (uno por estrategia) y ResolveEntry, que aplica el
    //     filtro de alineación y escribe el resultado en el destino.
    // EN: Internal resolution methods (one per strategy) and ResolveEntry, which applies the
    //     alignment filter and writes the result to the target.
    // Internal resolution methods
    bool TryPatternScan(PatternEntry& entry);
    bool TryStringXref(PatternEntry& entry);
    bool TryVTableSlot(PatternEntry& entry);
    bool TryHardcodedOffset(PatternEntry& entry);
    bool TryComplexPattern(PatternEntry& entry);
    bool TryCallGraphTrace(PatternEntry& entry);
    bool TryDirectStringSearch(PatternEntry& entry);
    bool TryPrologueValidatedRVA(PatternEntry& entry);
    void ResolveEntry(PatternEntry& entry, uintptr_t address,
                      ResolutionMethod method, float confidence);

    // ES: Retrocede desde una dirección de código hasta el inicio de la función (padding CC
    //     y prólogos MSVC conocidos).
    // Walk backwards from a code address to find the function start
    uintptr_t WalkBackToPrologue(uintptr_t codeAddr, int maxDistance = 4096) const;
};

} // namespace kmp
