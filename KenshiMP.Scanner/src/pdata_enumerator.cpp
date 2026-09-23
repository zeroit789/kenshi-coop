// ES: pdata_enumerator.cpp — implementación del enumerador .pdata (ver pdata_enumerator.h).
//     Las lecturas de memoria del PE se hacen en helpers estilo C con SEH porque MSVC
//     no permite __try en funciones con objetos C++ que tienen destructor (error C2712).
// EN: pdata_enumerator.cpp — .pdata enumerator implementation (see pdata_enumerator.h).
//     PE memory reads are done in C-style SEH helpers because MSVC does not allow
//     __try in functions with C++ objects that have destructors (error C2712).
#include "kmp/pdata_enumerator.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <numeric>

namespace kmp {

// ES: Helper SEH para Enumerate(): solo usa punteros crudos y POD, hace las lecturas
//     protegidas y escribe en un buffer que reserva quien llama.
// ---------------------------------------------------------------------------
// SEH helper for Enumerate()
// MSVC C2712: __try cannot be in a function that has C++ objects with dtors.
// This static C-style helper takes only raw pointers / POD and does the
// SEH-protected memory reads, writing results into a caller-supplied buffer.
// ---------------------------------------------------------------------------
// ES: Versión POD (sin destructores) de FunctionEntry para rellenar dentro del __try.
// EN: POD version (no destructors) of FunctionEntry to fill inside the __try.
struct EnumerateEntryPOD {
    uint32_t startRVA;
    uint32_t endRVA;
    uintptr_t startVA;
    uintptr_t endVA;
    uint32_t unwindRVA;
    size_t   size;
    uint8_t  prologueSize;
    uint16_t unwindCodeCount;
    uint8_t  frameRegister;
    uint8_t  frameOffset;
};

// ES: Recorre las entradas RUNTIME_FUNCTION y copia inicio/fin/unwind a outBuf.
//     Salta entradas con BeginAddress 0. Devuelve false si salta una excepción.
// EN: Walks the RUNTIME_FUNCTION entries and copies start/end/unwind into outBuf.
//     Skips entries with BeginAddress 0. Returns false if an exception is raised.
static bool SehEnumeratePData(
    uintptr_t moduleBase,
    size_t moduleSize,
    const RUNTIME_FUNCTION* runtimeFuncs,
    size_t numEntries,
    EnumerateEntryPOD* outBuf,   // caller-allocated array of at least numEntries
    size_t* outCount)            // how many were actually written
{
    *outCount = 0;
    __try {
        for (size_t i = 0; i < numEntries; i++) {
            const auto& rf = runtimeFuncs[i];
            if (rf.BeginAddress == 0) continue;

            EnumerateEntryPOD e;
            e.startRVA      = rf.BeginAddress;
            e.endRVA        = rf.EndAddress;
            e.startVA       = moduleBase + rf.BeginAddress;
            e.endVA         = moduleBase + rf.EndAddress;
            e.unwindRVA     = rf.UnwindInfoAddress;
            e.size          = rf.EndAddress - rf.BeginAddress;
            e.prologueSize  = 0;
            e.unwindCodeCount = 0;
            e.frameRegister = 0;
            e.frameOffset   = 0;

            // ES: Lee lo mínimo de UNWIND_INFO: byte 1 = tamaño de prólogo, byte 2 = número de
            //     códigos, byte 3 = registro de marco (4 bits bajos) y offset (4 bits altos).
            // Parse minimal unwind info for prologue size
            auto* unwindPtr = reinterpret_cast<const uint8_t*>(
                moduleBase + (rf.UnwindInfoAddress & ~1u)); // Mask off chain bit

            if (unwindPtr && (reinterpret_cast<uintptr_t>(unwindPtr) < moduleBase + moduleSize)) {
                e.prologueSize   = unwindPtr[1];
                e.unwindCodeCount = unwindPtr[2];
                e.frameRegister  = unwindPtr[3] & 0x0F;
                e.frameOffset    = (unwindPtr[3] >> 4) & 0x0F;
            }

            outBuf[*outCount] = e;
            (*outCount)++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// ES: Helper SEH para ParseUnwindInfo(): vuelca UNWIND_INFO en una estructura POD
//     plana; los códigos de longitud variable van a un buffer de tamaño fijo.
// ---------------------------------------------------------------------------
// SEH helper for ParseUnwindInfo()
// Reads UNWIND_INFO into a flat POD structure. The variable-length unwind
// codes are written into a caller-supplied fixed-size buffer.
// ---------------------------------------------------------------------------
// ES: Código de unwind en formato POD.
// EN: Unwind code in POD form.
struct UnwindCodePOD {
    uint8_t  codeOffset;
    uint8_t  opCode;
    uint8_t  opInfo;
    uint16_t extraData;
};

// ES: UNWIND_INFO completo en formato POD (hasta 256 códigos).
// EN: Full UNWIND_INFO in POD form (up to 256 codes).
struct ParsedUnwindPOD {
    uint8_t  version;
    uint8_t  flags;
    uint8_t  prologueSize;
    uint8_t  frameRegister;
    uint8_t  frameOffset;
    uintptr_t handlerRVA;
    uintptr_t chainedRVA;
    UnwindCodePOD codes[256]; // max 255 unwind codes in practice
    size_t   codeCount;
    bool     valid;
};

// ES: Decodifica UNWIND_INFO en 'out'. Si algo falla, out->valid queda en false.
// EN: Decodes UNWIND_INFO into 'out'. On failure out->valid stays false.
static void SehParseUnwindInfo(
    uintptr_t moduleBase,
    uint32_t unwindRVA,
    ParsedUnwindPOD* out)
{
    out->valid = false;
    out->codeCount = 0;
    out->handlerRVA = 0;
    out->chainedRVA = 0;

    auto* data = reinterpret_cast<const uint8_t*>(
        moduleBase + (unwindRVA & ~1u));

    // ES: Cabecera de 4 bytes: versión (3 bits) + flags (5 bits), tamaño de prólogo,
    //     número de códigos, registro/offset de marco.
    // EN: 4-byte header: version (3 bits) + flags (5 bits), prologue size,
    //     code count, frame register/offset.
    __try {
        out->version       = data[0] & 0x07;
        out->flags         = (data[0] >> 3) & 0x1F;
        out->prologueSize  = data[1];
        uint8_t codeCount  = data[2];
        out->frameRegister = data[3] & 0x0F;
        out->frameOffset   = (data[3] >> 4) & 0x0F;

        // ES: Cada código ocupa 2 bytes; algunos (ALLOC_LARGE, SAVE_*) usan 1-2 slots extra.
        //     OJO: extraData es uint16_t, así que los valores de 32 bits (ALLOC_LARGE con
        //     opInfo=1, *_FAR) se truncan. Solo afecta a información de diagnóstico.
        // EN: Each code takes 2 bytes; some (ALLOC_LARGE, SAVE_*) use 1-2 extra slots.
        //     NOTE: extraData is uint16_t, so 32-bit values (ALLOC_LARGE with opInfo=1,
        //     *_FAR) get truncated. Only affects diagnostic info.
        // Parse unwind codes (each is 2 bytes)
        const uint16_t* codes = reinterpret_cast<const uint16_t*>(data + 4);
        size_t outIdx = 0;
        for (uint8_t i = 0; i < codeCount; ) {
            uint16_t codeWord = codes[i];
            UnwindCodePOD code;
            code.codeOffset = static_cast<uint8_t>(codeWord & 0xFF);
            code.opCode     = static_cast<uint8_t>((codeWord >> 8) & 0x0F);
            code.opInfo     = static_cast<uint8_t>((codeWord >> 12) & 0x0F);
            code.extraData  = 0;

            switch (static_cast<UnwindOpCode>(code.opCode)) {
                case UnwindOpCode::ALLOC_LARGE:
                    if (code.opInfo == 0) {
                        code.extraData = codes[i + 1];
                        i += 2;
                    } else {
                        code.extraData = static_cast<uint16_t>(
                            codes[i + 1] | (codes[i + 2] << 16));
                        i += 3;
                    }
                    break;
                case UnwindOpCode::SAVE_NONVOL_FAR:
                case UnwindOpCode::SAVE_XMM128_FAR:
                    code.extraData = static_cast<uint16_t>(
                        codes[i + 1] | (codes[i + 2] << 16));
                    i += 3;
                    break;
                case UnwindOpCode::SAVE_NONVOL:
                case UnwindOpCode::SAVE_XMM128:
                    code.extraData = codes[i + 1];
                    i += 2;
                    break;
                default:
                    i++;
                    break;
            }

            if (outIdx < 256) {
                out->codes[outIdx++] = code;
            }
        }
        out->codeCount = outIdx;

        // ES: Tras los códigos (alineados a 4 bytes) va la RVA del manejador o de la entrada encadenada.
        // Handler/chain info follows the codes (aligned to 4 bytes)
        size_t codeBytes = 4 + codeCount * 2;
        if (codeCount & 1) codeBytes += 2; // Align

        if (out->flags & 0x01) { // UNW_FLAG_EHANDLER
            uint32_t handlerRVA;
            std::memcpy(&handlerRVA, data + codeBytes, 4);
            out->handlerRVA = handlerRVA;
        }
        if (out->flags & 0x04) { // UNW_FLAG_CHAININFO
            uint32_t chainRVA;
            std::memcpy(&chainRVA, data + codeBytes, 4);
            out->chainedRVA = chainRVA;
        }

        out->valid = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Failed to parse — out->valid remains false
    }
}

// ES: Implementación de PDataEnumerator.
// ---------------------------------------------------------------------------
// PDataEnumerator implementation
// ---------------------------------------------------------------------------

// ES: Guarda base/tamaño; devuelve false si no son válidos.
// EN: Stores base/size; returns false if invalid.
bool PDataEnumerator::Init(uintptr_t moduleBase, size_t moduleSize) {
    m_moduleBase = moduleBase;
    m_moduleSize = moduleSize;
    return moduleBase != 0 && moduleSize > 0;
}

// ES: Valida cabeceras PE, localiza IMAGE_DIRECTORY_ENTRY_EXCEPTION (.pdata),
//     copia las entradas mediante el helper SEH, las convierte a FunctionEntry,
//     las ordena y loguea estadísticas.
// EN: Validates PE headers, finds IMAGE_DIRECTORY_ENTRY_EXCEPTION (.pdata),
//     copies the entries via the SEH helper, converts them to FunctionEntry,
//     sorts them and logs statistics.
bool PDataEnumerator::Enumerate() {
    if (!m_moduleBase) return false;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m_moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(m_moduleBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    // ES: Directorio de excepciones (.pdata).
    // Get exception directory (.pdata)
    auto& exceptDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exceptDir.VirtualAddress == 0 || exceptDir.Size == 0) {
        spdlog::error("PDataEnumerator: No exception directory found");
        return false;
    }

    auto* runtimeFuncs = reinterpret_cast<RUNTIME_FUNCTION*>(
        m_moduleBase + exceptDir.VirtualAddress);
    size_t numEntries = exceptDir.Size / sizeof(RUNTIME_FUNCTION);

    spdlog::info("PDataEnumerator: Found {} RUNTIME_FUNCTION entries", numEntries);

    // ES: Buffer POD que rellena el helper SEH.
    // Allocate a POD buffer for the SEH helper to fill
    auto* podBuf = new (std::nothrow) EnumerateEntryPOD[numEntries];
    if (!podBuf) {
        spdlog::error("PDataEnumerator: Failed to allocate POD buffer for {} entries", numEntries);
        return false;
    }

    size_t podCount = 0;
    bool ok = SehEnumeratePData(m_moduleBase, m_moduleSize, runtimeFuncs,
                                numEntries, podBuf, &podCount);

    // ES: Convierte los resultados POD al vector real de FunctionEntry.
    // Convert POD results into the real FunctionEntry vector (C++ objects are fine here)
    m_functions.clear();
    m_functions.reserve(podCount);

    for (size_t i = 0; i < podCount; i++) {
        const auto& p = podBuf[i];
        FunctionEntry entry;
        entry.startRVA       = p.startRVA;
        entry.endRVA         = p.endRVA;
        entry.startVA        = p.startVA;
        entry.endVA          = p.endVA;
        entry.unwindRVA      = p.unwindRVA;
        entry.size           = p.size;
        entry.prologueSize   = p.prologueSize;
        entry.unwindCodeCount = p.unwindCodeCount;
        entry.frameRegister  = p.frameRegister;
        entry.frameOffset    = p.frameOffset;
        m_functions.push_back(entry);
    }

    delete[] podBuf;

    if (!ok) {
        spdlog::error("PDataEnumerator: Exception while reading .pdata");
        return false;
    }

    BuildIndex();

    auto stats = GetStats();
    spdlog::info("PDataEnumerator: {} functions, {} total code bytes, sizes {}-{} (avg {})",
                 stats.totalFunctions, stats.totalCodeBytes,
                 stats.minSize, stats.maxSize, stats.avgSize);

    return true;
}

// ES: Ordena por dirección de inicio para las búsquedas binarias.
// EN: Sorts by start address for binary searches.
void PDataEnumerator::BuildIndex() {
    // Sort by start address for binary search
    std::sort(m_functions.begin(), m_functions.end(),
              [](const FunctionEntry& a, const FunctionEntry& b) {
                  return a.startVA < b.startVA;
              });
}

// ES: Búsqueda binaria de una función que empieza exactamente en 'address'.
// EN: Binary search for a function starting exactly at 'address'.
const FunctionEntry* PDataEnumerator::FindFunction(uintptr_t address) const {
    // Binary search for exact start address match
    auto it = std::lower_bound(m_functions.begin(), m_functions.end(), address,
                                [](const FunctionEntry& f, uintptr_t addr) {
                                    return f.startVA < addr;
                                });

    if (it != m_functions.end() && it->startVA == address) {
        return &(*it);
    }
    return nullptr;
}

// ES: Devuelve la función que contiene 'address' (o nullptr). Es la base de
//     StringXref: xref dentro de una función -> inicio de esa función.
// EN: Returns the function containing 'address' (or nullptr). This is the basis
//     of StringXref: xref inside a function -> start of that function.
const FunctionEntry* PDataEnumerator::FindContaining(uintptr_t address) const {
    if (m_functions.empty()) return nullptr;

    // ES: Primera función con inicio > address y retrocede una.
    // Find first function with startVA > address, then go back one
    auto it = std::upper_bound(m_functions.begin(), m_functions.end(), address,
                                [](uintptr_t addr, const FunctionEntry& f) {
                                    return addr < f.startVA;
                                });

    if (it != m_functions.begin()) {
        --it;
        if (address >= it->startVA && address < it->endVA) {
            return &(*it);
        }
    }
    return nullptr;
}

// ES: Filtro lineal por tamaño.
// EN: Linear filter by size.
std::vector<const FunctionEntry*> PDataEnumerator::GetFunctionsBySize(
    size_t minSize, size_t maxSize) const {
    std::vector<const FunctionEntry*> result;
    for (const auto& f : m_functions) {
        if (f.size >= minSize && f.size <= maxSize) {
            result.push_back(&f);
        }
    }
    return result;
}

// ES: Funciones que empiezan en [start, end), usando lower_bound.
// EN: Functions starting in [start, end), using lower_bound.
std::vector<const FunctionEntry*> PDataEnumerator::GetFunctionsInRange(
    uintptr_t start, uintptr_t end) const {
    std::vector<const FunctionEntry*> result;

    auto it = std::lower_bound(m_functions.begin(), m_functions.end(), start,
                                [](const FunctionEntry& f, uintptr_t addr) {
                                    return f.startVA < addr;
                                });

    while (it != m_functions.end() && it->startVA < end) {
        result.push_back(&(*it));
        ++it;
    }
    return result;
}

// ES: Parsea UNWIND_INFO vía el helper SEH y lo convierte a la estructura C++.
// EN: Parses UNWIND_INFO via the SEH helper and converts it to the C++ struct.
UnwindInfo PDataEnumerator::ParseUnwindInfo(const FunctionEntry& func) const {
    UnwindInfo info;
    if (!func.unwindRVA) return info;

    // ES: Llama al helper SEH que solo usa tipos POD.
    // Call the SEH helper that uses only POD types
    ParsedUnwindPOD pod;
    pod.version = 0;
    pod.flags = 0;
    pod.prologueSize = 0;
    pod.frameRegister = 0;
    pod.frameOffset = 0;

    SehParseUnwindInfo(m_moduleBase, func.unwindRVA, &pod);

    if (pod.valid) {
        info.version       = pod.version;
        info.flags         = pod.flags;
        info.prologueSize  = pod.prologueSize;
        info.frameRegister = pod.frameRegister;
        info.frameOffset   = pod.frameOffset;
        info.handlerRVA    = pod.handlerRVA;
        info.chainedRVA    = pod.chainedRVA;

        info.codes.reserve(pod.codeCount);
        for (size_t i = 0; i < pod.codeCount; i++) {
            UnwindCode code;
            code.codeOffset = pod.codes[i].codeOffset;
            code.opCode     = static_cast<UnwindOpCode>(pod.codes[i].opCode);
            code.opInfo     = pod.codes[i].opInfo;
            code.extraData  = pod.codes[i].extraData;
            info.codes.push_back(code);
        }
    }

    return info;
}

// ES: Itera todas las funciones.
// EN: Iterates all functions.
void PDataEnumerator::ForEach(const std::function<void(const FunctionEntry&)>& callback) const {
    for (const auto& f : m_functions) {
        callback(f);
    }
}

// ES: Calcula estadísticas de tamaños y funciones etiquetadas.
// EN: Computes size statistics and labeled function count.
PDataEnumerator::Stats PDataEnumerator::GetStats() const {
    Stats stats;
    stats.totalFunctions = m_functions.size();

    if (m_functions.empty()) return stats;

    stats.minSize = SIZE_MAX;
    stats.maxSize = 0;

    for (const auto& f : m_functions) {
        if (!f.label.empty()) stats.labeledFunctions++;
        stats.totalCodeBytes += f.size;
        if (f.size < stats.minSize) stats.minSize = f.size;
        if (f.size > stats.maxSize) stats.maxSize = f.size;
    }

    stats.avgSize = stats.totalCodeBytes / stats.totalFunctions;
    return stats;
}

} // namespace kmp
