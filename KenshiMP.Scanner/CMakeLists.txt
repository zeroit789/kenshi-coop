# ES: KenshiMP.Scanner: librería estática que analiza kenshi_x64.exe en memoria (patrones AOB,
#     cadenas, vtables, .pdata, grafo de llamadas) para resolver direcciones de funciones del
#     juego, y gestiona los hooks con MinHook. dbghelp se usa para símbolos/volcados.
# EN: KenshiMP.Scanner: static library that analyzes kenshi_x64.exe in memory (AOB patterns,
#     strings, vtables, .pdata, call graph) to resolve game function addresses, and manages
#     hooks with MinHook. dbghelp is used for symbols/dumps.
add_library(KenshiMP.Scanner STATIC
    src/scanner.cpp
    src/scanner_engine.cpp
    src/patterns.cpp
    src/memory.cpp
    src/hook_manager.cpp
    src/mov_rax_rsp_fix.cpp
    src/function_analyzer.cpp
    src/pdata_enumerator.cpp
    src/string_analyzer.cpp
    src/vtable_scanner.cpp
    src/call_graph.cpp
    src/orchestrator.cpp
)

target_include_directories(KenshiMP.Scanner PUBLIC include)
target_link_libraries(KenshiMP.Scanner PUBLIC KenshiMP.Common minhook spdlog::spdlog dbghelp)
