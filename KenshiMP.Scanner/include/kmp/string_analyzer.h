// ES: string_analyzer.h — analizador de strings y referencias cruzadas (xrefs).
//     Escanea todas las cadenas de .rdata de kenshi_x64.exe, busca todas las
//     instrucciones LEA con direccionamiento RIP-relativo en .text que apuntan a cada
//     una, y con .pdata obtiene la función que contiene cada xref. Así se etiquetan
//     miles de funciones a partir de mensajes de debug ("[Clase::metodo] ..."),
//     errores y formatos. Es la fase 2 del orquestador y la base del método StringXref.
// EN: string_analyzer.h — string and cross-reference (xref) analyzer.
//     Scans every string in .rdata of kenshi_x64.exe, finds every RIP-relative LEA
//     in .text pointing to each one, and uses .pdata to get the function containing
//     each xref. This labels thousands of functions from debug messages
//     ("[Class::method] ..."), errors and format strings. It is orchestrator phase 2
//     and the basis of the StringXref method.
#pragma once
// ES: Analizador completo de strings y xrefs (descripción original en inglés abajo).
// Comprehensive String Cross-Reference Analyzer
//
// Scans ALL strings in .rdata, finds ALL code xrefs for each string,
// and resolves to function boundaries via .pdata. This auto-labels
// thousands of functions from debug strings, class names, error messages,
// and format strings embedded in the executable.

#include "kmp/pdata_enumerator.h"
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace kmp {

// ES: Una cadena encontrada en el binario (dirección, contenido, longitud, si es UTF-16)
//     y la lista de sitios del código que la referencian.
// A string found in the binary
struct GameString {
    uintptr_t   address    = 0;     // Address in memory
    std::string value;               // The string content
    size_t      length     = 0;     // String length (not including null)
    bool        isWide     = false; // UTF-16 string

    // ES: Referencia desde código: dirección del LEA, función que lo contiene (vía .pdata)
    //     y etiqueta de esa función.
    // Cross-references from code
    struct XRef {
        uintptr_t   codeAddress = 0;  // Address of the LEA instruction
        uintptr_t   funcAddress = 0;  // Function containing this xref (via .pdata)
        std::string funcLabel;         // Label of the containing function
    };
    std::vector<XRef> xrefs;
};

// ES: Categorías de strings para clasificarlas (log de debug, error, clase, formato, ruta...).
// Classification of a string for categorization
enum class StringCategory {
    Unknown,
    DebugLog,       // "[ClassName::method] message"
    ErrorMessage,   // "Error: ...", "Failed to ..."
    ClassName,      // C++ class names from RTTI or debug strings
    FunctionName,   // "functionName" in debug logs
    FormatString,   // Contains %d, %s, %f, etc.
    FilePath,       // Contains / or \ and file extensions
    GameData,       // Item names, stat names, etc.
    UIString,       // UI element names, layout references
    ConfigKey,      // Configuration key names
};

// ES: Función etiquetada gracias a sus strings: nombre deducido, clase/método si el
//     string tiene formato "[Clase::metodo]", categoría, confianza y strings que usa.
// A labeled function discovered via string analysis
struct LabeledFunction {
    uintptr_t       address     = 0;
    std::string     label;
    std::string     className;      // Extracted class name if applicable
    std::string     methodName;     // Extracted method name if applicable
    StringCategory  category    = StringCategory::Unknown;
    float           confidence  = 0.0f;
    std::vector<std::string> strings; // All strings referenced by this function
};

// ES: Descubre strings, resuelve sus xrefs y etiqueta funciones; ofrece consultas.
// EN: Discovers strings, resolves their xrefs and labels functions; offers queries.
class StringAnalyzer {
public:
    // ES: Inicializa con la info del módulo y (opcional) la tabla de funciones .pdata.
    // Initialize with module info and .pdata function table
    bool Init(uintptr_t moduleBase, size_t moduleSize,
              const PDataEnumerator* pdata = nullptr);

    // ── String Discovery ──

    // ES: Escanea .rdata buscando cadenas ASCII de al menos 'minLength' caracteres.
    // Scan .rdata for all ASCII strings (min length 4)
    size_t ScanStrings(int minLength = 4);

    // ES: Igual para cadenas anchas (UTF-16).
    // Scan for wide (UTF-16) strings
    size_t ScanWideStrings(int minLength = 4);

    // ── Cross-Reference Analysis ──

    // ES: Busca TODOS los xrefs de código de todas las cadenas descubiertas (la parte pesada:
    //     decodifica cada LEA RIP-relativo de .text una sola vez).
    // Find ALL code xrefs for all discovered strings
    // This is the main heavy-lifting function.
    size_t ResolveXrefs();

    // ES: Busca los xrefs de un único string a partir de su dirección.
    // Find xrefs for a single string at a known address
    std::vector<GameString::XRef> FindXrefs(uintptr_t stringAddr) const;

    // ── Function Labeling ──

    // ES: Etiqueta funciones según los strings que referencian (heurísticas para sacar
    //     clase/método de los mensajes de debug).
    // Label functions based on their string references.
    // Uses heuristics to extract class/method names from debug strings.
    size_t LabelFunctions();

    // ── Query API ──

    // ES: Consultas: strings por subcadena o categoría, funciones etiquetadas, funciones
    //     que referencian un string y strings de una función.
    // EN: Queries: strings by substring or category, labeled functions, functions
    //     referencing a string, and strings of a function.
    // Find strings matching a pattern (substring search)
    std::vector<const GameString*> FindStrings(const std::string& substring) const;

    // Find strings by category
    std::vector<const GameString*> FindByCategory(StringCategory category) const;

    // Get all labeled functions
    const std::vector<LabeledFunction>& GetLabeledFunctions() const { return m_labeledFunctions; }

    // Find a labeled function by name (partial match)
    const LabeledFunction* FindLabeledFunction(const std::string& name) const;

    // Find all functions referencing a specific string
    std::vector<uintptr_t> FindFunctionsReferencingString(const std::string& str) const;

    // Get function containing address, returning its strings
    std::vector<std::string> GetFunctionStrings(uintptr_t funcAddr) const;

    // ES: Estadísticas, iteración y acceso crudo (lo usa el orquestador).
    // EN: Statistics, iteration and raw access (used by the orchestrator).
    // ── Statistics ──
    size_t GetStringCount() const { return m_strings.size(); }
    size_t GetXrefCount() const { return m_totalXrefs; }
    size_t GetLabeledFunctionCount() const { return m_labeledFunctions.size(); }

    // ── Iteration ──
    void ForEachString(const std::function<void(const GameString&)>& callback) const;
    void ForEachLabeledFunction(const std::function<void(const LabeledFunction&)>& callback) const;

    // ── Raw access for orchestrator ──
    const std::vector<GameString>& GetAllStrings() const { return m_strings; }

private:
    // ES: Estado: módulo, secciones .text/.rdata, .pdata, strings, funciones etiquetadas
    //     e índice función -> strings que referencia.
    // EN: State: module, .text/.rdata sections, .pdata, strings, labeled functions
    //     and function -> referenced strings index.
    uintptr_t   m_moduleBase = 0;
    size_t      m_moduleSize = 0;
    uintptr_t   m_textBase   = 0;
    size_t      m_textSize   = 0;
    uintptr_t   m_rdataBase  = 0;
    size_t      m_rdataSize  = 0;

    const PDataEnumerator* m_pdata = nullptr;

    std::vector<GameString>       m_strings;
    std::vector<LabeledFunction>  m_labeledFunctions;
    size_t                        m_totalXrefs = 0;

    // Index: function address → list of string indices
    std::unordered_map<uintptr_t, std::vector<size_t>> m_funcStringIndex;

    // ES: Helpers internos: localizar secciones, comprobar imprimibles, clasificar string,
    //     extraer clase/método y generar una etiqueta.
    // Internal helpers
    void FindSections();
    bool IsASCIIPrintable(const uint8_t* data, size_t len) const;
    StringCategory ClassifyString(const std::string& str) const;
    std::pair<std::string, std::string> ExtractClassMethod(const std::string& str) const;
    std::string GenerateLabel(const std::vector<std::string>& strings) const;
};

} // namespace kmp
