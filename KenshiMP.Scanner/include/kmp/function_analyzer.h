// ES: function_analyzer.h — análisis heurístico de prólogos de funciones x64.
//     A partir de los primeros bytes de una función del juego estima cuántos
//     parámetros recibe (uso de RCX/RDX/R8/R9/XMM0-3, guardados en shadow space),
//     el tamaño del marco de pila y los registros apilados. Sirve para comprobar
//     que los typedefs de nuestros hooks coinciden con la función real. Es informativo:
//     no bloquea la instalación de hooks.
// EN: function_analyzer.h — heuristic analysis of x64 function prologues.
//     From the first bytes of a game function it estimates how many parameters
//     it takes (use of RCX/RDX/R8/R9/XMM0-3, shadow space saves), the stack frame
//     size and the pushed registers. Used to check that our hook typedefs match
//     the real function. It is informational: it does not block hook installation.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kmp {

// ES: Resultado del análisis de una función (firma estimada).
// Analysis result for a single function
struct FunctionSignature {
    uintptr_t address = 0;
    std::string name;

    // ES: Datos del prólogo: tamaño de 'sub rsp', parámetros guardados en shadow space,
    //     número de push y estimación de parámetros.
    // EN: Prologue data: 'sub rsp' size, parameters saved to shadow space,
    //     push count and parameter estimate.
    // Prologue analysis
    int stackFrameSize = 0;      // Total sub rsp, N
    int shadowSpaceUsed = 0;     // How many of RCX/RDX/R8/R9 are saved to shadow space
    int pushedRegs = 0;          // Number of push instructions in prologue
    int estimatedParams = 0;     // Best guess at parameter count

    // ES: Registros de parámetros usados en el prólogo (convención de llamada Windows x64:
    //     RCX, RDX, R8, R9 para enteros/punteros; XMM0-3 para floats).
    // EN: Parameter registers used in the prologue (Windows x64 calling convention:
    //     RCX, RDX, R8, R9 for ints/pointers; XMM0-3 for floats).
    // Detected register usage in prologue (first 64 bytes)
    bool usesRCX = false;  // 1st param
    bool usesRDX = false;  // 2nd param
    bool usesR8  = false;  // 3rd param
    bool usesR9  = false;  // 4th param
    bool usesXMM0 = false; // float 1st param
    bool usesXMM1 = false; // float 2nd param
    bool usesXMM2 = false; // float 3rd param
    bool usesXMM3 = false; // float 4th param

    // ES: ¿Lee parámetros de la pila (más allá del shadow space)? => más de 4 parámetros.
    // Whether the function accesses stack parameters (beyond shadow space)
    bool accessesStackParams = false;

    // ES: Bytes crudos del prólogo para depuración.
    // Raw prologue bytes for debugging
    uint8_t prologueBytes[64] = {};
    int prologueLen = 0;

    // ES: Válida si tiene dirección.
    // EN: Valid if it has an address.
    bool IsValid() const { return address != 0; }
};

// ES: Analiza prólogos x64 para deducir firmas. Ayuda a validar que las firmas de
//     nuestros hooks coinciden con las funciones reales del juego que enganchamos.
// Analyzes x64 function prologues to determine signatures.
// This helps validate that our hook function signatures match the
// actual game functions we're hooking.
class FunctionAnalyzer {
public:
    // ES: Analiza la función en 'address' (lee hasta 64 bytes con SEH) y devuelve su firma estimada.
    // Analyze a function at the given address
    static FunctionSignature Analyze(uintptr_t address, const std::string& name);

    // ES: Comprueba que el número de parámetros de nuestro typedef de hook encaja con la
    //     estimación (con cierta tolerancia). Devuelve true si coincide o es aceptable.
    // Validate that our hook signature has the right parameter count.
    // Returns true if the hook signature matches (or is close enough).
    // hookParamCount is the number of parameters in our hook typedef.
    static bool ValidateSignature(const FunctionSignature& sig, int hookParamCount);

    // ES: Escribe en el log el resultado del análisis de todas las funciones.
    // Log analysis results for all functions
    static void LogAnalysis(const std::vector<FunctionSignature>& signatures);

private:
    // ES: Estima el número de parámetros a partir de los registros usados.
    // Count parameters from prologue register usage
    static int CountParamsFromRegisters(const FunctionSignature& sig);

    // ES: Detecta los guardados de registros de parámetros al shadow space (bytes de ejemplo abajo).
    // Detect if a byte sequence stores a parameter register to shadow space
    // mov [rsp+08], rcx = 48 89 4C 24 08
    // mov [rsp+10], rdx = 48 89 54 24 10
    // mov [rsp+18], r8  = 4C 89 44 24 18
    // mov [rsp+20], r9  = 4C 89 4C 24 20
    static void DetectShadowSpaceSaves(const uint8_t* code, int len, FunctionSignature& sig);

    // ES: Detecta 'sub rsp, N' para conocer el tamaño del marco de pila.
    // Detect sub rsp, N for stack frame size
    static void DetectStackFrame(const uint8_t* code, int len, FunctionSignature& sig);

    // ES: Detecta los registros apilados con push en el prólogo.
    // Detect pushed registers
    static void DetectPushedRegisters(const uint8_t* code, int len, FunctionSignature& sig);

    // ES: Detecta uso de registros de parámetros más allá de los guardados en shadow space.
    // Detect parameter register usage beyond shadow space saves
    static void DetectRegisterUsage(const uint8_t* code, int len, FunctionSignature& sig);
};

} // namespace kmp
