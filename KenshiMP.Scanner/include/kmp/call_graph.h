// ES: call_graph.h — analizador de grafo de llamadas de kenshi_x64.exe.
//     Para cada función conocida (vía .pdata) recorre su cuerpo buscando CALL (E8)
//     y JMP (E9) directos y construye un grafo dirigido llamador -> llamado.
//     Después propaga etiquetas (nombres deducidos por strings) a vecinos del grafo.
//     Lo usa la fase 6 del orquestador; en la práctica resuelve poco (confianza baja).
// EN: call_graph.h — call graph analyzer for kenshi_x64.exe.
//     For each known function (via .pdata) it walks its body looking for direct
//     CALL (E8) and JMP (E9) instructions and builds a caller -> callee directed graph.
//     It then propagates labels (names deduced from strings) to graph neighbors.
//     Used by orchestrator phase 6; in practice it resolves little (low confidence).
#pragma once
// ES: Analizador de grafo de llamadas (descripción original en inglés abajo).
// Call Graph Analyzer
//
// For each known function, scans its body for CALL (E8) and JMP (E9)
// instructions to build a directed call graph. Propagates labels from
// known functions to callees and callers.

#include "kmp/pdata_enumerator.h"
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace kmp {

// ES: Una arista del grafo: quién llama, a quién, desde qué instrucción, y si es JMP (tail call).
// An edge in the call graph
struct CallEdge {
    uintptr_t   callerAddr  = 0;   // Function making the call
    uintptr_t   calleeAddr  = 0;   // Function being called
    uintptr_t   callSite    = 0;   // Address of the CALL/JMP instruction
    bool        isDirectCall = true; // E8 call vs indirect call
    bool        isJmp       = false; // E9 jmp (tail call)
};

// ES: Un nodo del grafo: función, su etiqueta, llamadores, llamados y aristas salientes.
// A node in the call graph
struct CallNode {
    uintptr_t                   address  = 0;
    std::string                 label;
    std::vector<uintptr_t>      callers;   // Functions that call this one
    std::vector<uintptr_t>      callees;   // Functions this one calls
    std::vector<CallEdge>       outEdges;  // Detailed outgoing edges
    size_t                      callCount = 0; // How many times this is called
};

// ES: Construye y consulta el grafo de llamadas.
// EN: Builds and queries the call graph.
class CallGraphAnalyzer {
public:
    // ES: Inicializa con la info del módulo y el enumerador .pdata (necesario para límites de función).
    // Initialize with module info and .pdata
    bool Init(uintptr_t moduleBase, size_t moduleSize,
              const PDataEnumerator* pdata);

    // ── Analysis ──

    // ES: Construye el grafo para TODAS las funciones de .pdata (lento: recorre todo el código).
    // Build call graph for all functions in .pdata
    // This scans every function body for CALL/JMP instructions
    size_t BuildFullGraph();

    // ES: Construye el grafo solo para un subconjunto de funciones.
    // Build call graph for a subset of functions
    size_t BuildGraphFor(const std::vector<uintptr_t>& functionAddrs);

    // ES: Analiza las llamadas de una sola función.
    // Analyze a single function's calls
    std::vector<CallEdge> AnalyzeFunction(uintptr_t funcAddr, size_t funcSize) const;

    // ── Label Propagation ──

    // ES: Propaga etiquetas de funciones conocidas a sus llamados/llamadores hasta 'depth' saltos.
    // Propagate labels from labeled functions to their callees/callers
    // depth = how many hops to propagate
    size_t PropagateLabels(
        const std::unordered_map<uintptr_t, std::string>& knownLabels,
        int depth = 2);

    // ── Query API ──

    // ES: Consultas: nodo, llamadores, llamados, más llamadas, hojas, raíces,
    //     camino entre dos funciones (BFS) y vecindario a N saltos.
    // EN: Queries: node, callers, callees, most called, leaves, roots,
    //     path between two functions (BFS) and N-hop neighborhood.
    // Get node for a function
    const CallNode* GetNode(uintptr_t funcAddr) const;

    // Get all callers of a function
    std::vector<uintptr_t> GetCallers(uintptr_t funcAddr) const;

    // Get all callees of a function
    std::vector<uintptr_t> GetCallees(uintptr_t funcAddr) const;

    // Find most-called functions (hot functions)
    std::vector<std::pair<uintptr_t, size_t>> GetMostCalled(size_t topN = 50) const;

    // Find leaf functions (call nothing)
    std::vector<uintptr_t> GetLeafFunctions() const;

    // Find root functions (called by nothing in our graph)
    std::vector<uintptr_t> GetRootFunctions() const;

    // Trace call path from source to target (BFS)
    std::vector<uintptr_t> FindCallPath(uintptr_t source, uintptr_t target,
                                         int maxDepth = 10) const;

    // Get functions within N calls of a target
    std::unordered_set<uintptr_t> GetNeighborhood(uintptr_t funcAddr,
                                                    int radius = 2) const;

    // ES: Estadísticas e iteración.
    // EN: Statistics and iteration.
    // ── Statistics ──
    size_t GetNodeCount() const { return m_nodes.size(); }
    size_t GetEdgeCount() const { return m_totalEdges; }

    // ── Iteration ──
    void ForEachNode(const std::function<void(const CallNode&)>& callback) const;

private:
    // ES: Estado: módulo, sección .text, enumerador .pdata y nodos del grafo.
    // EN: State: module, .text section, .pdata enumerator and graph nodes.
    uintptr_t m_moduleBase = 0;
    size_t    m_moduleSize = 0;
    uintptr_t m_textBase   = 0;
    size_t    m_textSize   = 0;

    const PDataEnumerator* m_pdata = nullptr;

    std::unordered_map<uintptr_t, CallNode> m_nodes;
    size_t m_totalEdges = 0;

    // ES: Helpers: localizar .text, comprobar si una dirección está en .text, obtener/crear nodo.
    // EN: Helpers: locate .text, check whether an address is in .text, get/create node.
    void FindTextSection();
    bool IsInText(uintptr_t addr) const;
    CallNode& GetOrCreateNode(uintptr_t addr);
};

} // namespace kmp
