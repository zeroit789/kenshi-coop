// ES: pdata_enumerator.h — enumerador de límites de funciones a partir de .pdata.
//     En Windows x64 toda función no-hoja tiene una entrada RUNTIME_FUNCTION en el
//     directorio de excepciones del PE (.pdata) con su inicio y fin exactos. Eso da
//     una tabla casi completa de funciones de kenshi_x64.exe sin heurísticas.
//     Es la fuente AUTORITATIVA de límites de función del Scanner: se usa para saber
//     en qué función cae un xref (FindContaining) y para validar que una dirección
//     resuelta es el inicio real de una función antes de hookearla.
// EN: pdata_enumerator.h — function boundary enumerator based on .pdata.
//     On Windows x64 every non-leaf function has a RUNTIME_FUNCTION entry in the
//     PE exception directory (.pdata) with its exact start and end. That gives an
//     almost complete function table of kenshi_x64.exe without heuristics.
//     It is the Scanner's AUTHORITATIVE source of function boundaries: used to know
//     which function contains a xref (FindContaining) and to validate that a resolved
//     address is a real function start before hooking it.
#pragma once
// ES: Enumerador de límites de funciones por .pdata (descripción original en inglés abajo).
// .pdata Function Boundary Enumerator
//
// Uses the PE exception directory (.pdata) to enumerate EVERY function
// in the executable with exact start/end addresses. On x64 Windows,
// every non-leaf function MUST have a RUNTIME_FUNCTION entry for
// structured exception handling (SEH). This gives us a nearly complete
// function table without any heuristics.

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>

namespace kmp {

// ES: Una función tal y como la describe .pdata (RVA = relativa a la base del módulo;
//     VA = dirección real = base + RVA). También guarda etiquetas que ponen otros analizadores.
// A single function as described by .pdata
struct FunctionEntry {
    uintptr_t   startRVA    = 0;   // Function start (RVA from module base)
    uintptr_t   endRVA      = 0;   // Function end (exclusive)
    uintptr_t   startVA     = 0;   // Virtual address (base + RVA)
    uintptr_t   endVA       = 0;   // End VA
    uint32_t    unwindRVA   = 0;   // UNWIND_INFO pointer
    size_t      size        = 0;   // Function size in bytes

    // ES: Etiquetas aplicadas por otros analizadores (strings, RTTI...): nombre, categoría y confianza.
    // Labels applied by other analyzers
    std::string label;              // Human-readable name (from strings, RTTI, etc.)
    std::string category;           // Category tag (e.g., "combat", "ui", "ai")
    float       confidence  = 0.0f; // Label confidence (0-1)

    // ES: Datos del prólogo sacados de UNWIND_INFO (tamaño del prólogo, registro de marco...).
    // Prologue info (extracted from UNWIND_INFO)
    uint8_t     prologueSize = 0;
    uint8_t     frameRegister = 0;
    uint8_t     frameOffset = 0;
    uint16_t    unwindCodeCount = 0;

    // ES: Válida si tiene dirección y tamaño.
    // EN: Valid if it has an address and a size.
    bool IsValid() const { return startVA != 0 && size > 0; }
};

// ES: Tipos de códigos de unwind según la especificación PE (qué hace cada paso del prólogo).
// Unwind code types from the PE spec
enum class UnwindOpCode : uint8_t {
    PUSH_NONVOL     = 0,
    ALLOC_LARGE     = 1,
    ALLOC_SMALL     = 2,
    SET_FPREG       = 3,
    SAVE_NONVOL     = 4,
    SAVE_NONVOL_FAR = 5,
    SAVE_XMM128     = 8,
    SAVE_XMM128_FAR = 9,
    PUSH_MACHFRAME  = 10,
};

// ES: Un código de unwind ya decodificado (offset en el prólogo, operación e info extra).
// EN: A decoded unwind code (offset in the prologue, operation and extra info).
struct UnwindCode {
    uint8_t       codeOffset;
    UnwindOpCode  opCode;
    uint8_t       opInfo;
    uint16_t      extraData = 0;   // For ALLOC_LARGE, SAVE_NONVOL_FAR, etc.
};

// ES: UNWIND_INFO parseado: versión, flags, tamaño de prólogo, registro de marco,
//     códigos de unwind y, si los hay, manejador de excepciones o entrada encadenada.
// UNWIND_INFO parsed from .pdata
struct UnwindInfo {
    uint8_t                 version     = 0;
    uint8_t                 flags       = 0;
    uint8_t                 prologueSize = 0;
    uint8_t                 frameRegister = 0;
    uint8_t                 frameOffset  = 0;
    std::vector<UnwindCode> codes;
    uintptr_t               handlerRVA  = 0;  // If UNW_FLAG_EHANDLER/UHANDLER
    uintptr_t               chainedRVA  = 0;  // If UNW_FLAG_CHAININFO
};

// ES: Lee el directorio de excepciones del PE, construye un índice ordenado por dirección
//     y ofrece búsquedas binarias (función exacta / función que contiene una dirección).
// EN: Reads the PE exception directory, builds an address-sorted index and offers
//     binary searches (exact function / function containing an address).
class PDataEnumerator {
public:
    // ES: Guarda base y tamaño del módulo a enumerar.
    // Initialize from a module
    bool Init(uintptr_t moduleBase, size_t moduleSize);

    // ES: Lee todas las RUNTIME_FUNCTION de .pdata (bajo SEH) y construye el índice ordenado.
    // Enumerate all functions from .pdata
    bool Enumerate();

    // ES: Acceso a los resultados.
    // Access results
    const std::vector<FunctionEntry>& GetFunctions() const { return m_functions; }
    size_t GetFunctionCount() const { return m_functions.size(); }

    // ES: Busca una función cuyo inicio sea EXACTAMENTE 'address' (búsqueda binaria).
    // Lookup by address (binary search)
    const FunctionEntry* FindFunction(uintptr_t address) const;

    // ES: Busca la función que CONTIENE 'address' (inicio <= address < fin).
    // Find function containing an address
    const FunctionEntry* FindContaining(uintptr_t address) const;

    // ES: Funciones cuyo tamaño está en [minSize, maxSize].
    // Get functions in a size range
    std::vector<const FunctionEntry*> GetFunctionsBySize(size_t minSize, size_t maxSize) const;

    // ES: Funciones que empiezan dentro de [start, end).
    // Get functions in an address range
    std::vector<const FunctionEntry*> GetFunctionsInRange(uintptr_t start, uintptr_t end) const;

    // ES: Parsea el UNWIND_INFO completo de una función.
    // Parse UNWIND_INFO for a function
    UnwindInfo ParseUnwindInfo(const FunctionEntry& func) const;

    // ES: Recorre todas las funciones llamando al callback.
    // Iterate all functions
    void ForEach(const std::function<void(const FunctionEntry&)>& callback) const;

    // ES: Estadísticas (total, etiquetadas, tamaños mínimo/máximo/medio, bytes de código).
    // Statistics
    struct Stats {
        size_t totalFunctions   = 0;
        size_t labeledFunctions = 0;
        size_t minSize          = 0;
        size_t maxSize          = 0;
        size_t avgSize          = 0;
        size_t totalCodeBytes   = 0;
    };
    Stats GetStats() const;

private:
    // ES: Base/tamaño del módulo y vector de funciones ordenado por startVA.
    // EN: Module base/size and function vector sorted by startVA.
    uintptr_t m_moduleBase = 0;
    size_t    m_moduleSize = 0;
    std::vector<FunctionEntry> m_functions;  // Sorted by startVA

    // ES: Ordena el vector para permitir búsqueda binaria.
    // Build sorted index for binary search
    void BuildIndex();
};

} // namespace kmp
