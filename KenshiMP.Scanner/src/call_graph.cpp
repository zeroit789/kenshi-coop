// ES: call_graph.cpp — implementación del grafo de llamadas (ver call_graph.h).
//     El recorrido es byte a byte (no desensambla): cualquier byte E8/E9 seguido de un
//     destino dentro de .text se toma como llamada, así que puede haber aristas falsas
//     cuando E8/E9 aparece dentro de otra instrucción. Suficiente para etiquetado aproximado.
// EN: call_graph.cpp — call graph implementation (see call_graph.h).
//     The walk is byte by byte (no disassembly): any E8/E9 byte followed by a target
//     inside .text is taken as a call, so there may be false edges when E8/E9 shows
//     up inside another instruction. Good enough for approximate labeling.
#include "kmp/call_graph.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <algorithm>
#include <queue>
#include <cstring>

namespace kmp {

// ES: Helper SEH (sin objetos C++ con destructor).
// ═══════════════════════════════════════════════════════════════════════════
//  SEH HELPER — No C++ objects with destructors allowed
// ═══════════════════════════════════════════════════════════════════════════

// ES: Arista cruda en formato POD para rellenar dentro del __try.
// EN: Raw POD edge to fill inside the __try.
struct RawCallEdge {
    uintptr_t callerAddr;
    uintptr_t calleeAddr;
    uintptr_t callSite;
    bool      isJmp;
};

// ES: Recorre el cuerpo de una función buscando CALL rel32 (E8) y JMP rel32 (E9) cuyo
//     destino cae en .text, y los escribe en un array C. Los JMP a la propia función
//     se ignoran. Si hay violación de acceso devuelve lo encontrado hasta entonces.
// Scans a function body for CALL/JMP instructions and writes results
// into a plain-C array. Returns the number of edges found.
static size_t SEH_AnalyzeFunction(uintptr_t funcAddr, size_t funcSize,
                                   uintptr_t textBase, size_t textSize,
                                   RawCallEdge* outEdges, size_t maxEdges) {
    size_t count = 0;

    __try {
        const uint8_t* code = reinterpret_cast<const uint8_t*>(funcAddr);

        for (size_t i = 0; i + 5 <= funcSize && count < maxEdges; i++) {
            uintptr_t instrAddr = funcAddr + i;

            // ES: E8 = CALL rel32.
            // E8 xx xx xx xx — CALL rel32
            if (code[i] == 0xE8) {
                int32_t rel;
                std::memcpy(&rel, &code[i + 1], 4);
                uintptr_t target = instrAddr + 5 + rel;

                if (target >= textBase && target < textBase + textSize) {
                    outEdges[count].callerAddr = funcAddr;
                    outEdges[count].calleeAddr = target;
                    outEdges[count].callSite = instrAddr;
                    outEdges[count].isJmp = false;
                    count++;
                }
                i += 4;
                continue;
            }

            // ES: E9 = JMP rel32 (tail call).
            // E9 xx xx xx xx — JMP rel32 (tail call)
            if (code[i] == 0xE9) {
                int32_t rel;
                std::memcpy(&rel, &code[i + 1], 4);
                uintptr_t target = instrAddr + 5 + rel;

                if (target >= textBase && target < textBase + textSize && target != funcAddr) {
                    outEdges[count].callerAddr = funcAddr;
                    outEdges[count].calleeAddr = target;
                    outEdges[count].callSite = instrAddr;
                    outEdges[count].isJmp = true;
                    count++;
                }
                i += 4;
                continue;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Access violation — return what we found so far
    }

    return count;
}

// ES: Analizador del grafo de llamadas.
// ═══════════════════════════════════════════════════════════════════════════
//  CALL GRAPH ANALYZER
// ═══════════════════════════════════════════════════════════════════════════

// ES: Guarda módulo y .pdata y localiza .text. Devuelve false si falta alguno.
// EN: Stores module and .pdata and locates .text. Returns false if any is missing.
bool CallGraphAnalyzer::Init(uintptr_t moduleBase, size_t moduleSize,
                              const PDataEnumerator* pdata) {
    m_moduleBase = moduleBase;
    m_moduleSize = moduleSize;
    m_pdata = pdata;
    FindTextSection();
    return m_textBase != 0 && m_pdata != nullptr;
}

// ES: Busca la sección .text en las cabeceras PE.
// EN: Finds the .text section in the PE headers.
void CallGraphAnalyzer::FindTextSection() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m_moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(m_moduleBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        char name[9] = {};
        std::memcpy(name, section->Name, 8);
        if (std::strcmp(name, ".text") == 0) {
            m_textBase = m_moduleBase + section->VirtualAddress;
            m_textSize = section->Misc.VirtualSize;
            return;
        }
    }
}

// ES: ¿Está la dirección dentro de .text?
// EN: Is the address inside .text?
bool CallGraphAnalyzer::IsInText(uintptr_t addr) const {
    return addr >= m_textBase && addr < m_textBase + m_textSize;
}

// ES: Devuelve el nodo de 'addr', creándolo si no existe.
// EN: Returns the node for 'addr', creating it if missing.
CallNode& CallGraphAnalyzer::GetOrCreateNode(uintptr_t addr) {
    auto it = m_nodes.find(addr);
    if (it != m_nodes.end()) return it->second;

    CallNode node;
    node.address = addr;
    auto result = m_nodes.emplace(addr, std::move(node));
    return result.first->second;
}

// ES: Análisis de una función.
// ═══════════════════════════════════════════════════════════════════════════
//  FUNCTION ANALYSIS
// ═══════════════════════════════════════════════════════════════════════════

// ES: Convierte las aristas crudas del helper SEH en CallEdge. Ojo: el buffer de
//     4096 aristas (~128 KB) va en la pila del hilo que llama.
// EN: Converts the SEH helper raw edges into CallEdge. Note: the 4096-edge buffer
//     (~128 KB) lives on the calling thread's stack.
std::vector<CallEdge> CallGraphAnalyzer::AnalyzeFunction(uintptr_t funcAddr,
                                                          size_t funcSize) const {
    std::vector<CallEdge> edges;
    if (!funcAddr || funcSize == 0) return edges;

    // Use SEH helper to scan function body
    constexpr size_t MAX_EDGES_PER_FUNC = 4096;
    RawCallEdge rawEdges[MAX_EDGES_PER_FUNC];

    size_t count = SEH_AnalyzeFunction(funcAddr, funcSize, m_textBase, m_textSize,
                                        rawEdges, MAX_EDGES_PER_FUNC);

    edges.reserve(count);
    for (size_t i = 0; i < count; i++) {
        CallEdge edge;
        edge.callerAddr = rawEdges[i].callerAddr;
        edge.calleeAddr = rawEdges[i].calleeAddr;
        edge.callSite = rawEdges[i].callSite;
        edge.isDirectCall = true;
        edge.isJmp = rawEdges[i].isJmp;
        edges.push_back(edge);
    }

    return edges;
}

// ES: Construcción del grafo completo.
// ═══════════════════════════════════════════════════════════════════════════
//  FULL GRAPH BUILD
// ═══════════════════════════════════════════════════════════════════════════

// ES: Recorre TODAS las funciones de .pdata, analiza sus llamadas y rellena nodos
//     (llamadores/llamados/contador de llamadas). Devuelve el número de aristas.
// EN: Walks ALL .pdata functions, analyzes their calls and fills nodes
//     (callers/callees/call count). Returns the number of edges.
size_t CallGraphAnalyzer::BuildFullGraph() {
    if (!m_pdata) return 0;

    m_nodes.clear();
    m_totalEdges = 0;

    const auto& functions = m_pdata->GetFunctions();
    size_t analyzed = 0;

    for (const auto& func : functions) {
        if (func.size < 5) continue;

        auto edges = AnalyzeFunction(func.startVA, func.size);
        if (edges.empty()) continue;

        auto& callerNode = GetOrCreateNode(func.startVA);
        callerNode.label = func.label;

        for (const auto& edge : edges) {
            uintptr_t calleeStart = edge.calleeAddr;
            if (auto* calleeFunc = m_pdata->FindFunction(edge.calleeAddr)) {
                calleeStart = calleeFunc->startVA;
            }

            auto& calleeNode = GetOrCreateNode(calleeStart);
            calleeNode.callCount++;
            calleeNode.callers.push_back(func.startVA);

            callerNode.callees.push_back(calleeStart);
            callerNode.outEdges.push_back(edge);

            m_totalEdges++;
        }

        analyzed++;
    }

    spdlog::info("CallGraphAnalyzer: Analyzed {} functions, {} nodes, {} edges",
                 analyzed, m_nodes.size(), m_totalEdges);
    return m_totalEdges;
}

// ES: Igual que BuildFullGraph pero solo para las funciones indicadas (si la dirección
//     no es inicio de función se usa la función que la contiene). Acumula aristas.
// EN: Same as BuildFullGraph but only for the given functions (if the address is
//     not a function start, the containing function is used). Accumulates edges.
size_t CallGraphAnalyzer::BuildGraphFor(const std::vector<uintptr_t>& functionAddrs) {
    size_t edges = 0;

    for (uintptr_t addr : functionAddrs) {
        auto* func = m_pdata->FindFunction(addr);
        if (!func) {
            func = m_pdata->FindContaining(addr);
        }
        if (!func) continue;

        auto funcEdges = AnalyzeFunction(func->startVA, func->size);
        auto& callerNode = GetOrCreateNode(func->startVA);
        callerNode.label = func->label;

        for (const auto& edge : funcEdges) {
            uintptr_t calleeStart = edge.calleeAddr;
            if (auto* calleeFunc = m_pdata->FindFunction(edge.calleeAddr)) {
                calleeStart = calleeFunc->startVA;
            }

            auto& calleeNode = GetOrCreateNode(calleeStart);
            calleeNode.callCount++;
            calleeNode.callers.push_back(func->startVA);

            callerNode.callees.push_back(calleeStart);
            callerNode.outEdges.push_back(edge);

            edges++;
        }
    }

    m_totalEdges += edges;
    return edges;
}

// ES: Propagación de etiquetas.
// ═══════════════════════════════════════════════════════════════════════════
//  LABEL PROPAGATION
// ═══════════════════════════════════════════════════════════════════════════

// ES: Aplica las etiquetas conocidas y, durante 'depth' rondas, etiqueta a los vecinos
//     sin nombre como "callee_of_X" / "caller_of_X". Devuelve cuántas se añadieron.
// EN: Applies the known labels and, for 'depth' rounds, labels unnamed neighbors
//     as "callee_of_X" / "caller_of_X". Returns how many were added.
size_t CallGraphAnalyzer::PropagateLabels(
    const std::unordered_map<uintptr_t, std::string>& knownLabels, int depth) {
    for (const auto& [addr, label] : knownLabels) {
        auto it = m_nodes.find(addr);
        if (it != m_nodes.end()) {
            it->second.label = label;
        }
    }

    size_t labeled = 0;

    for (int d = 0; d < depth; d++) {
        std::unordered_map<uintptr_t, std::string> newLabels;

        for (const auto& [addr, node] : m_nodes) {
            if (node.label.empty()) continue;

            for (uintptr_t callee : node.callees) {
                auto calleeIt = m_nodes.find(callee);
                if (calleeIt != m_nodes.end() && calleeIt->second.label.empty()) {
                    newLabels[callee] = "callee_of_" + node.label;
                }
            }

            for (uintptr_t caller : node.callers) {
                auto callerIt = m_nodes.find(caller);
                if (callerIt != m_nodes.end() && callerIt->second.label.empty()) {
                    newLabels[caller] = "caller_of_" + node.label;
                }
            }
        }

        for (const auto& [addr, label] : newLabels) {
            m_nodes[addr].label = label;
            labeled++;
        }
    }

    spdlog::info("CallGraphAnalyzer: Propagated labels to {} additional functions", labeled);
    return labeled;
}

// ES: API de consultas.
// ═══════════════════════════════════════════════════════════════════════════
//  QUERY API
// ═══════════════════════════════════════════════════════════════════════════

// ES: Nodo de una función (nullptr si no está en el grafo).
// EN: Node for a function (nullptr if not in the graph).
const CallNode* CallGraphAnalyzer::GetNode(uintptr_t funcAddr) const {
    auto it = m_nodes.find(funcAddr);
    return it != m_nodes.end() ? &it->second : nullptr;
}

// ES: Llamadores / llamados de una función (copia del vector).
// EN: Callers / callees of a function (vector copy).
std::vector<uintptr_t> CallGraphAnalyzer::GetCallers(uintptr_t funcAddr) const {
    auto* node = GetNode(funcAddr);
    return node ? node->callers : std::vector<uintptr_t>{};
}

std::vector<uintptr_t> CallGraphAnalyzer::GetCallees(uintptr_t funcAddr) const {
    auto* node = GetNode(funcAddr);
    return node ? node->callees : std::vector<uintptr_t>{};
}

// ES: Las 'topN' funciones más llamadas (funciones "calientes").
// EN: The 'topN' most called functions ("hot" functions).
std::vector<std::pair<uintptr_t, size_t>> CallGraphAnalyzer::GetMostCalled(size_t topN) const {
    std::vector<std::pair<uintptr_t, size_t>> result;
    for (const auto& [addr, node] : m_nodes) {
        result.push_back({addr, node.callCount});
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (result.size() > topN) result.resize(topN);
    return result;
}

// ES: Funciones hoja (no llaman a nada) y raíz (nadie las llama en el grafo).
// EN: Leaf functions (call nothing) and root functions (nobody calls them in the graph).
std::vector<uintptr_t> CallGraphAnalyzer::GetLeafFunctions() const {
    std::vector<uintptr_t> result;
    for (const auto& [addr, node] : m_nodes) {
        if (node.callees.empty()) result.push_back(addr);
    }
    return result;
}

std::vector<uintptr_t> CallGraphAnalyzer::GetRootFunctions() const {
    std::vector<uintptr_t> result;
    for (const auto& [addr, node] : m_nodes) {
        if (node.callers.empty()) result.push_back(addr);
    }
    return result;
}

// ES: BFS desde 'source' siguiendo llamados hasta encontrar 'target' (máximo maxDepth saltos).
//     Devuelve el camino o vacío.
// EN: BFS from 'source' following callees until 'target' is found (at most maxDepth hops).
//     Returns the path or empty.
std::vector<uintptr_t> CallGraphAnalyzer::FindCallPath(uintptr_t source,
                                                         uintptr_t target,
                                                         int maxDepth) const {
    std::queue<std::vector<uintptr_t>> queue;
    std::unordered_set<uintptr_t> visited;

    queue.push({source});
    visited.insert(source);

    while (!queue.empty()) {
        auto path = queue.front();
        queue.pop();

        if (static_cast<int>(path.size()) > maxDepth + 1) break;

        uintptr_t current = path.back();

        auto* node = GetNode(current);
        if (!node) continue;

        for (uintptr_t callee : node->callees) {
            if (callee == target) {
                path.push_back(callee);
                return path;
            }
            if (visited.count(callee) == 0) {
                visited.insert(callee);
                auto newPath = path;
                newPath.push_back(callee);
                queue.push(std::move(newPath));
            }
        }
    }

    return {};
}

// ES: Conjunto de funciones a como mucho 'radius' saltos (llamadores y llamados).
// EN: Set of functions within 'radius' hops (callers and callees).
std::unordered_set<uintptr_t> CallGraphAnalyzer::GetNeighborhood(uintptr_t funcAddr,
                                                                   int radius) const {
    std::unordered_set<uintptr_t> result;
    std::queue<std::pair<uintptr_t, int>> queue;

    queue.push({funcAddr, 0});
    result.insert(funcAddr);

    while (!queue.empty()) {
        auto [addr, depth] = queue.front();
        queue.pop();

        if (depth >= radius) continue;

        auto* node = GetNode(addr);
        if (!node) continue;

        for (uintptr_t callee : node->callees) {
            if (result.count(callee) == 0) {
                result.insert(callee);
                queue.push({callee, depth + 1});
            }
        }
        for (uintptr_t caller : node->callers) {
            if (result.count(caller) == 0) {
                result.insert(caller);
                queue.push({caller, depth + 1});
            }
        }
    }

    return result;
}

// ES: Itera todos los nodos.
// EN: Iterates all nodes.
void CallGraphAnalyzer::ForEachNode(
    const std::function<void(const CallNode&)>& callback) const {
    for (const auto& [addr, node] : m_nodes) callback(node);
}

} // namespace kmp
