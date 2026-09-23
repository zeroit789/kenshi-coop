// ES: vtable_scanner.cpp — implementación del escáner de vtables por RTTI de MSVC
//     (ver vtable_scanner.h). Toda lectura de estructuras RTTI se hace en helpers SEH
//     sin objetos C++ con destructor; luego se construyen los VTableInfo.
// EN: vtable_scanner.cpp — MSVC RTTI vtable scanner implementation
//     (see vtable_scanner.h). Every RTTI structure read is done in SEH helpers with no
//     C++ objects with destructors; VTableInfo objects are built afterwards.
#include "kmp/vtable_scanner.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <DbgHelp.h>
#include <algorithm>
#include <cstring>

// ES: Enlaza dbghelp para UnDecorateSymbolName (desmangleado de nombres MSVC).
// EN: Links dbghelp for UnDecorateSymbolName (MSVC name demangling).
#pragma comment(lib, "dbghelp.lib")

namespace kmp {

// ES: Helpers SEH (sin objetos C++ con destructor).
// ═══════════════════════════════════════════════════════════════════════════
//  SEH HELPER FUNCTIONS — No C++ objects with destructors allowed
// ═══════════════════════════════════════════════════════════════════════════

// ES: Datos crudos del CompleteObjectLocator leídos dentro del SEH.
// Raw COL data extracted inside SEH
struct RawCOLData {
    bool        valid           = false;
    uint32_t    colSignature    = 0;
    uintptr_t   colAddress      = 0;
    uintptr_t   typeDescAddr    = 0;
    uintptr_t   pVFTable        = 0;
    int32_t     classDescRVA    = 0;
    char        mangledName[256] = {};
};

// ES: Lee un COL: exige signature == 1 (x64), sigue la RVA al TypeDescriptor, comprueba
//     que todo cae dentro del módulo y copia el nombre mangleado (".?AVClase@@").
// EN: Reads a COL: requires signature == 1 (x64), follows the RVA to the TypeDescriptor,
//     checks everything is inside the module and copies the mangled name (".?AVClass@@").
static bool SEH_ReadCOL(uintptr_t colAddr, uintptr_t moduleBase, size_t moduleSize,
                         RawCOLData* out) {
    __try {
        auto* col = reinterpret_cast<const RTTICompleteObjectLocator*>(colAddr);
        if (col->signature != 1) return false;

        out->colSignature = col->signature;
        out->colAddress = colAddr;

        uintptr_t typeDescAddr = moduleBase + col->typeDescRVA;
        if (typeDescAddr < moduleBase || typeDescAddr >= moduleBase + moduleSize) return false;

        out->typeDescAddr = typeDescAddr;

        auto* typeDesc = reinterpret_cast<const RTTITypeDescriptor*>(typeDescAddr);
        if (typeDesc->pVFTable < moduleBase || typeDesc->pVFTable >= moduleBase + moduleSize)
            return false;

        out->pVFTable = typeDesc->pVFTable;
        out->classDescRVA = col->classDescRVA;

        // Copy mangled name safely
        size_t nameLen = 0;
        while (nameLen < 255 && typeDesc->name[nameLen] != '\0') {
            out->mangledName[nameLen] = typeDesc->name[nameLen];
            nameLen++;
        }
        out->mangledName[nameLen] = '\0';
        out->valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Datos crudos de una clase base leídos dentro del SEH.
// Raw base class data extracted inside SEH
struct RawBaseClassEntry {
    char mangledName[256];
    bool valid;
};

// ES: Lee el ClassHierarchyDescriptor y copia los nombres mangleados de las clases base
//     (saltando el índice 0, que es la propia clase). Máximo 32 bases.
// EN: Reads the ClassHierarchyDescriptor and copies the mangled names of the base
//     classes (skipping index 0, which is the class itself). At most 32 bases.
static int SEH_ReadBaseClasses(uintptr_t moduleBase, size_t moduleSize,
                                int32_t classDescRVA,
                                RawBaseClassEntry* outBases, int maxBases) {
    __try {
        uintptr_t chAddr = moduleBase + classDescRVA;
        if (chAddr < moduleBase || chAddr >= moduleBase + moduleSize) return 0;

        auto* ch = reinterpret_cast<const RTTIClassHierarchyDescriptor*>(chAddr);
        if (ch->numBaseClasses == 0 || ch->numBaseClasses > 64) return 0;

        uintptr_t baseArrayAddr = moduleBase + ch->baseClassArrayRVA;
        if (baseArrayAddr < moduleBase || baseArrayAddr >= moduleBase + moduleSize) return 0;

        auto* baseArray = reinterpret_cast<const int32_t*>(baseArrayAddr);
        int count = 0;

        // Skip index 0 (self)
        for (uint32_t i = 1; i < ch->numBaseClasses && i < 32 && count < maxBases; i++) {
            uintptr_t bcdAddr = moduleBase + baseArray[i];
            if (bcdAddr < moduleBase || bcdAddr >= moduleBase + moduleSize) continue;

            auto* bcd = reinterpret_cast<const RTTIBaseClassDescriptor*>(bcdAddr);
            uintptr_t baseTypeAddr = moduleBase + bcd->typeDescRVA;
            if (baseTypeAddr < moduleBase || baseTypeAddr >= moduleBase + moduleSize) continue;

            auto* baseType = reinterpret_cast<const RTTITypeDescriptor*>(baseTypeAddr);
            size_t nameLen = 0;
            while (nameLen < 255 && baseType->name[nameLen] != '\0') {
                outBases[count].mangledName[nameLen] = baseType->name[nameLen];
                nameLen++;
            }
            outBases[count].mangledName[nameLen] = '\0';
            outBases[count].valid = (nameLen > 0);
            count++;
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ES: Cuenta slots consecutivos de la vtable que apuntan a .text (para en nulo o en
//     el primer puntero fuera de código; máximo 512).
// EN: Counts consecutive vtable slots pointing into .text (stops at null or at the
//     first non-code pointer; at most 512).
static size_t SEH_CountVTableSlots(uintptr_t vtableAddr, uintptr_t textBase, size_t textSize) {
    __try {
        auto* slots = reinterpret_cast<const uintptr_t*>(vtableAddr);
        size_t count = 0;
        while (count < 512) {
            uintptr_t funcPtr = slots[count];
            if (funcPtr == 0) break;
            if (funcPtr < textBase || funcPtr >= textBase + textSize) break;
            count++;
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ES: Copia los punteros de los slots a un array plano.
// Read vtable slot pointers into a plain array
static size_t SEH_ReadVTableSlots(uintptr_t vtableAddr, uintptr_t* outSlots,
                                   size_t maxSlots) {
    __try {
        auto* slots = reinterpret_cast<const uintptr_t*>(vtableAddr);
        size_t count = 0;
        for (size_t i = 0; i < maxSlots; i++) {
            outSlots[i] = slots[i];
            count++;
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ES: Candidato a vtable encontrado en .rdata: (posible COL en vtable[-1], dirección de vtable[0]).
// Scan .rdata for vtable candidates: returns pairs of (possibleCOL, firstSlot)
struct VTableCandidate {
    uintptr_t colPtr;
    uintptr_t vtableAddr;
    size_t    rdataIndex;
};

// ES: Recorre .rdata de 8 en 8 bytes: un candidato es una posición cuyo slot[0] apunta a
//     .text y cuyo slot[-1] apunta a .rdata (donde vive el COL). Máximo maxCandidates.
// EN: Walks .rdata 8 bytes at a time: a candidate is a position whose slot[0] points to
//     .text and whose slot[-1] points to .rdata (where the COL lives). At most maxCandidates.
static size_t SEH_ScanRdataForCandidates(uintptr_t rdataBase, size_t rdataSize,
                                           uintptr_t textBase, size_t textSize,
                                           VTableCandidate* outCandidates, size_t maxCandidates) {
    __try {
        const uintptr_t* rdataSlots = reinterpret_cast<const uintptr_t*>(rdataBase);
        size_t numSlots = rdataSize / sizeof(uintptr_t);
        size_t found = 0;

        for (size_t i = 1; i < numSlots && found < maxCandidates; i++) {
            uintptr_t possibleCOL = rdataSlots[i - 1];
            uintptr_t firstSlot   = rdataSlots[i];

            // vtable[0] must be a code pointer
            if (firstSlot < textBase || firstSlot >= textBase + textSize) continue;

            // vtable[-1] must point into .rdata (where COL lives)
            if (possibleCOL < rdataBase || possibleCOL >= rdataBase + rdataSize) continue;

            outCandidates[found].colPtr = possibleCOL;
            outCandidates[found].vtableAddr = rdataBase + i * sizeof(uintptr_t);
            outCandidates[found].rdataIndex = i;
            found++;
        }
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ES: Implementación de VTableScanner.
// ═══════════════════════════════════════════════════════════════════════════
//  VTABLE SCANNER IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════════════

// ES: Guarda módulo y .pdata y localiza secciones. False si falta .text o .rdata.
// EN: Stores module and .pdata and locates sections. False if .text or .rdata is missing.
bool VTableScanner::Init(uintptr_t moduleBase, size_t moduleSize,
                          const PDataEnumerator* pdata) {
    m_moduleBase = moduleBase;
    m_moduleSize = moduleSize;
    m_pdata = pdata;
    FindSections();
    return m_textBase != 0 && m_rdataBase != 0;
}

// ES: Localiza .text, .rdata y .data en la tabla de secciones PE.
// EN: Locates .text, .rdata and .data in the PE section table.
void VTableScanner::FindSections() {
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
        } else if (std::strcmp(name, ".rdata") == 0) {
            m_rdataBase = m_moduleBase + section->VirtualAddress;
            m_rdataSize = section->Misc.VirtualSize;
        } else if (std::strcmp(name, ".data") == 0) {
            m_dataBase = m_moduleBase + section->VirtualAddress;
            m_dataSize = section->Misc.VirtualSize;
        }
    }
}

// ES: ¿Apunta a .text? / ¿Está dentro del módulo?
// EN: Points into .text? / Is it inside the module?
bool VTableScanner::IsCodePointer(uintptr_t addr) const {
    return addr >= m_textBase && addr < m_textBase + m_textSize;
}

bool VTableScanner::IsInModule(uintptr_t addr) const {
    return addr >= m_moduleBase && addr < m_moduleBase + m_moduleSize;
}

// ES: Desmanglea: para ".?AVClase@@" / ".?AUStruct@@" extrae el nombre directamente;
//     para lo demás prueba UnDecorateSymbolName y, si falla, quita el '.' inicial.
// EN: Demangles: for ".?AVClass@@" / ".?AUStruct@@" extracts the name directly;
//     otherwise tries UnDecorateSymbolName and, if that fails, strips the leading '.'.
std::string VTableScanner::DemangleName(const char* mangledName) const {
    if (!mangledName || mangledName[0] == '\0') return "";

    std::string mangled(mangledName);

    // RTTI type info names: ".?AVClassName@@" or ".?AUStructName@@"
    // Extract the class name directly — UnDecorateSymbolName often returns these as-is.
    if (mangled.size() > 4 && (mangled.substr(0, 4) == ".?AV" || mangled.substr(0, 4) == ".?AU")) {
        size_t end = mangled.find("@@");
        if (end != std::string::npos && end > 4) {
            return mangled.substr(4, end - 4);
        }
    }

    // Try MSVC demangler for other decorated names
    char demangled[512] = {};
    DWORD result = UnDecorateSymbolName(mangledName, demangled, sizeof(demangled),
                                         UNDNAME_NAME_ONLY);
    if (result > 0) return demangled;

    if (mangled[0] == '.') return mangled.substr(1);
    return mangled;
}

// ES: Lee el COL de una vtable y rellena nombre de clase y clases base en 'info'.
// EN: Reads a vtable's COL and fills class name and base classes in 'info'.
bool VTableScanner::ReadCOL(uintptr_t colAddr, VTableInfo& info) {
    RawCOLData raw;
    if (!SEH_ReadCOL(colAddr, m_moduleBase, m_moduleSize, &raw)) return false;

    info.colAddress = raw.colAddress;
    info.typeDescAddr = raw.typeDescAddr;
    info.mangledName = raw.mangledName;
    info.className = DemangleName(raw.mangledName);

    if (info.className.empty()) return false;

    if (raw.classDescRVA != 0) {
        info.baseClasses = ReadBaseClasses(raw.classDescRVA);
        info.inheritanceDepth = static_cast<int>(info.baseClasses.size());
    }

    return true;
}

// ES: Lee y desmanglea las clases base de un ClassHierarchyDescriptor.
// EN: Reads and demangles the base classes of a ClassHierarchyDescriptor.
std::vector<std::string> VTableScanner::ReadBaseClasses(int32_t classDescRVA) {
    std::vector<std::string> bases;

    RawBaseClassEntry rawBases[32];
    int count = SEH_ReadBaseClasses(m_moduleBase, m_moduleSize, classDescRVA, rawBases, 32);

    for (int i = 0; i < count; i++) {
        if (rawBases[i].valid) {
            std::string baseName = DemangleName(rawBases[i].mangledName);
            if (!baseName.empty()) {
                bases.push_back(baseName);
            }
        }
    }

    return bases;
}

// ES: Número de slots de código de una vtable.
// EN: Number of code slots of a vtable.
size_t VTableScanner::CountVTableSlots(uintptr_t vtableAddr) const {
    return SEH_CountVTableSlots(vtableAddr, m_textBase, m_textSize);
}

// ES: Escaneo principal de vtables.
// ═══════════════════════════════════════════════════════════════════════════
//  MAIN VTABLE SCAN
// ═══════════════════════════════════════════════════════════════════════════

// ES: Fase 1: busca candidatos en .rdata (hasta 65536). Fase 2: por cada uno lee el COL,
//     cuenta y lee sus slots y etiqueta cada slot con .pdata. Nota: si una clase tiene
//     varias vtables (herencia múltiple) el índice por nombre se queda con la última.
// EN: Phase 1: finds candidates in .rdata (up to 65536). Phase 2: for each one reads the
//     COL, counts and reads its slots and labels each slot via .pdata. Note: if a class
//     has several vtables (multiple inheritance) the name index keeps the last one.
size_t VTableScanner::ScanVTables() {
    if (!m_rdataBase || !m_rdataSize) return 0;

    m_vtables.clear();
    m_classNameIndex.clear();

    // Phase 1: SEH-protected scan for vtable candidates in .rdata
    // Allocate a reasonable buffer for candidates
    constexpr size_t MAX_CANDIDATES = 65536;
    auto candidates = std::make_unique<VTableCandidate[]>(MAX_CANDIDATES);

    size_t numCandidates = SEH_ScanRdataForCandidates(
        m_rdataBase, m_rdataSize, m_textBase, m_textSize,
        candidates.get(), MAX_CANDIDATES);

    spdlog::info("VTableScanner: Found {} vtable candidates in .rdata", numCandidates);

    // Phase 2: For each candidate, read COL and build VTableInfo (C++ objects safe here)
    size_t found = 0;
    for (size_t c = 0; c < numCandidates; c++) {
        const auto& cand = candidates[c];

        VTableInfo info;
        if (!ReadCOL(cand.colPtr, info)) continue;

        info.address = cand.vtableAddr;
        info.slotCount = CountVTableSlots(cand.vtableAddr);
        if (info.slotCount == 0) continue;

        // Read slot function pointers via SEH
        constexpr size_t MAX_SLOTS = 512;
        uintptr_t rawSlots[MAX_SLOTS] = {};
        size_t slotsRead = SEH_ReadVTableSlots(cand.vtableAddr, rawSlots,
                                                 info.slotCount < MAX_SLOTS ? info.slotCount : MAX_SLOTS);

        info.slots.resize(slotsRead);
        for (size_t s = 0; s < slotsRead; s++) {
            info.slots[s].index = static_cast<int>(s);
            info.slots[s].funcAddress = rawSlots[s];

            if (m_pdata) {
                auto* func = m_pdata->FindFunction(rawSlots[s]);
                if (func && !func->label.empty()) {
                    info.slots[s].funcLabel = func->label;
                }
            }
        }

        m_classNameIndex[info.className] = m_vtables.size();
        m_vtables.push_back(std::move(info));
        found++;
    }

    spdlog::info("VTableScanner: Found {} vtables with RTTI", found);
    return found;
}

// ES: API de consultas.
// ═══════════════════════════════════════════════════════════════════════════
//  QUERY API
// ═══════════════════════════════════════════════════════════════════════════

// ES: Busca por nombre exacto en el índice y, si no, por subcadena.
// EN: Looks up by exact name in the index, then by substring.
const VTableInfo* VTableScanner::FindByClassName(const std::string& name) const {
    auto it = m_classNameIndex.find(name);
    if (it != m_classNameIndex.end()) return &m_vtables[it->second];

    for (const auto& vt : m_vtables) {
        if (vt.className.find(name) != std::string::npos) return &vt;
    }
    return nullptr;
}

// ES: Clases cuya lista de bases contiene 'baseName' (subcadena).
// EN: Classes whose base list contains 'baseName' (substring).
std::vector<const VTableInfo*> VTableScanner::FindDerivedClasses(
    const std::string& baseName) const {
    std::vector<const VTableInfo*> result;
    for (const auto& vt : m_vtables) {
        for (const auto& base : vt.baseClasses) {
            if (base.find(baseName) != std::string::npos) {
                result.push_back(&vt);
                break;
            }
        }
    }
    return result;
}

// ES: Dirección de la función virtual del slot indicado (0 si no hay). Es lo que usa
//     el método VTableSlot del orquestador (p.ej. SquadAddMember).
// EN: Address of the virtual function in the given slot (0 if none). This is what the
//     orchestrator VTableSlot method uses (e.g. SquadAddMember).
uintptr_t VTableScanner::GetVirtualFunction(const std::string& className, int slotIndex) const {
    auto* vt = FindByClassName(className);
    if (!vt || slotIndex < 0 || slotIndex >= static_cast<int>(vt->slotCount)) return 0;
    return vt->slots[slotIndex].funcAddress;
}

// ES: Todos los slots de una clase (nullptr si no existe).
// EN: All slots of a class (nullptr if missing).
const std::vector<VTableSlot>* VTableScanner::GetVirtualFunctions(
    const std::string& className) const {
    auto* vt = FindByClassName(className);
    return vt ? &vt->slots : nullptr;
}

// ES: Itera todas las vtables.
// EN: Iterates all vtables.
void VTableScanner::ForEach(const std::function<void(const VTableInfo&)>& callback) const {
    for (const auto& vt : m_vtables) callback(vt);
}

// ES: Estadísticas de vtables, slots, clases y profundidad de herencia.
// EN: Statistics of vtables, slots, classes and inheritance depth.
VTableScanner::Stats VTableScanner::GetStats() const {
    Stats stats;
    stats.totalVTables = m_vtables.size();
    stats.uniqueClasses = m_classNameIndex.size();

    for (const auto& vt : m_vtables) {
        stats.totalSlots += vt.slotCount;
        if (vt.slotCount > stats.maxSlotCount) stats.maxSlotCount = vt.slotCount;
        if (static_cast<size_t>(vt.inheritanceDepth) > stats.maxInheritDepth)
            stats.maxInheritDepth = vt.inheritanceDepth;
    }

    return stats;
}

} // namespace kmp
