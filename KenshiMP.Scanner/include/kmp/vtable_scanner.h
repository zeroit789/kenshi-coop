// ES: vtable_scanner.h — escáner de vtables y jerarquía de clases C++ vía RTTI de MSVC.
//     Busca en .rdata tablas de funciones virtuales (punteros consecutivos a código),
//     lee la RTTI (vtable[-1] -> CompleteObjectLocator -> TypeDescriptor) para obtener
//     el nombre de la clase y su jerarquía, y extrae los slots virtuales.
//     El mod lo usa para resolver funciones por "clase + índice de slot" (método
//     VTableSlot), p.ej. SquadAddMember = slot 2 de la vtable de ActivePlatoon.
// EN: vtable_scanner.h — vtable and C++ class hierarchy scanner via MSVC RTTI.
//     Searches .rdata for virtual function tables (consecutive code pointers),
//     reads RTTI (vtable[-1] -> CompleteObjectLocator -> TypeDescriptor) to get the
//     class name and hierarchy, and extracts the virtual slots.
//     The mod uses it to resolve functions by "class + slot index" (VTableSlot
//     method), e.g. SquadAddMember = slot 2 of the ActivePlatoon vtable.
#pragma once
// ES: Escáner de vtables y mapa de jerarquía (descripción original en inglés abajo).
// VTable Scanner and C++ Class Hierarchy Mapper
//
// Scans .rdata for vtable arrays (consecutive code pointers),
// resolves RTTI type_info for C++ class names, maps inheritance
// hierarchies, and extracts virtual function slots.
//
// MSVC x64 RTTI layout:
//   vtable[-1] = pointer to CompleteObjectLocator
//   COL → TypeDescriptor → mangled class name
//   COL → ClassHierarchyDescriptor → base class list

#include "kmp/pdata_enumerator.h"
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>

namespace kmp {

// ES: Un slot de función virtual: índice, puntero a la función y etiqueta si se conoce.
// A virtual function slot
struct VTableSlot {
    int         index       = 0;    // Slot index in vtable
    uintptr_t   funcAddress = 0;    // Function pointer value
    std::string funcLabel;           // Label if known
    bool        isPureVirtual = false;
};

// ES: Una vtable descubierta: dirección, nombre de clase (desmangleado y crudo), slots,
//     punteros RTTI y clases base.
// A discovered vtable
struct VTableInfo {
    uintptr_t   address     = 0;    // Address of vtable in .rdata
    std::string className;           // Demangled class name (from RTTI)
    std::string mangledName;         // Raw mangled name
    size_t      slotCount   = 0;    // Number of virtual functions
    std::vector<VTableSlot> slots;

    // RTTI pointers
    uintptr_t   colAddress  = 0;    // CompleteObjectLocator
    uintptr_t   typeDescAddr = 0;   // TypeDescriptor

    // Inheritance
    std::vector<std::string> baseClasses;  // Direct and indirect bases
    int         inheritanceDepth = 0;

    // ES: Válida si tiene dirección y nombre de clase.
    // EN: Valid if it has an address and a class name.
    bool IsValid() const { return address != 0 && !className.empty(); }
};

// ES: Estructuras RTTI de MSVC x64 tal y como están en memoria (empaquetadas a 1 byte).
//     En x64 los punteros internos son RVAs de 32 bits respecto a la base del módulo.
// EN: MSVC x64 RTTI structures as laid out in memory (packed to 1 byte).
//     On x64 the internal pointers are 32-bit RVAs relative to the module base.
// MSVC RTTI structures (x64)
#pragma pack(push, 1)
struct RTTITypeDescriptor {
    uintptr_t   pVFTable;      // Always points to type_info::vftable
    uintptr_t   spare;         // Internal runtime data
    char        name[1];       // Mangled type name (variable length, null-terminated)
};

// ES: CompleteObjectLocator (COL): lo apunta vtable[-1]; lleva la RVA del
//     TypeDescriptor (nombre) y del ClassHierarchyDescriptor (bases).
// EN: CompleteObjectLocator (COL): pointed to by vtable[-1]; holds the RVA of the
//     TypeDescriptor (name) and of the ClassHierarchyDescriptor (bases).
struct RTTICompleteObjectLocator {
    uint32_t    signature;     // Always 1 for x64
    uint32_t    offset;        // Offset of this vtable in the class
    uint32_t    cdOffset;      // Constructor displacement offset
    int32_t     typeDescRVA;   // RVA of TypeDescriptor
    int32_t     classDescRVA;  // RVA of ClassHierarchyDescriptor
    int32_t     selfRVA;       // RVA of this COL (for x64)
};

// ES: Descriptor de jerarquía: número de clases base y RVA del array de bases.
// EN: Hierarchy descriptor: base class count and RVA of the base array.
struct RTTIClassHierarchyDescriptor {
    uint32_t    signature;     // Always 0
    uint32_t    attributes;    // Bit 0: multiple inheritance, Bit 1: virtual inheritance
    uint32_t    numBaseClasses;
    int32_t     baseClassArrayRVA;
};

// ES: Descriptor de una clase base (su TypeDescriptor y desplazamientos).
// EN: Base class descriptor (its TypeDescriptor and displacements).
struct RTTIBaseClassDescriptor {
    int32_t     typeDescRVA;
    uint32_t    numContainedBases;
    int32_t     memberDisplacement;
    int32_t     vbtableDisplacement;
    int32_t     displacementWithinVBTable;
    uint32_t    attributes;
    int32_t     classDescRVA;
};
#pragma pack(pop)

// ES: Escanea vtables y ofrece búsquedas por nombre de clase o slot.
// EN: Scans vtables and offers lookups by class name or slot.
class VTableScanner {
public:
    // ES: Inicializa con la info del módulo (y .pdata opcional).
    // Initialize with module info
    bool Init(uintptr_t moduleBase, size_t moduleSize,
              const PDataEnumerator* pdata = nullptr);

    // ── Scanning ──

    // ES: Busca todas las vtables que tienen RTTI.
    // Scan for all vtables with RTTI
    size_t ScanVTables();

    // ── Query API ──

    // ES: Busca una vtable por nombre de clase (coincidencia parcial).
    // Find vtable by class name (partial match)
    const VTableInfo* FindByClassName(const std::string& name) const;

    // ES: Todas las clases que heredan de 'baseName'.
    // Find all vtables for classes inheriting from a base
    std::vector<const VTableInfo*> FindDerivedClasses(const std::string& baseName) const;

    // ES: Función virtual en el slot 'slotIndex' de la clase indicada (0 si no existe).
    // Get virtual function at a specific slot
    uintptr_t GetVirtualFunction(const std::string& className, int slotIndex) const;

    // ES: Todos los slots virtuales de una clase.
    // Get all virtual functions for a class
    const std::vector<VTableSlot>* GetVirtualFunctions(const std::string& className) const;

    // ES: Todas las vtables descubiertas.
    // Get all discovered vtables
    const std::vector<VTableInfo>& GetAllVTables() const { return m_vtables; }
    size_t GetVTableCount() const { return m_vtables.size(); }

    // ES: Iteración y estadísticas.
    // EN: Iteration and statistics.
    // ── Iteration ──
    void ForEach(const std::function<void(const VTableInfo&)>& callback) const;

    // ── Statistics ──
    struct Stats {
        size_t totalVTables     = 0;
        size_t totalSlots       = 0;
        size_t uniqueClasses    = 0;
        size_t maxSlotCount     = 0;
        size_t maxInheritDepth  = 0;
    };
    Stats GetStats() const;

private:
    // ES: Estado: módulo, secciones .text/.rdata/.data, vtables e índice nombre -> vtable.
    // EN: State: module, .text/.rdata/.data sections, vtables and name -> vtable index.
    uintptr_t m_moduleBase = 0;
    size_t    m_moduleSize = 0;
    uintptr_t m_textBase   = 0;
    size_t    m_textSize   = 0;
    uintptr_t m_rdataBase  = 0;
    size_t    m_rdataSize  = 0;
    uintptr_t m_dataBase   = 0;
    size_t    m_dataSize   = 0;

    const PDataEnumerator* m_pdata = nullptr;

    std::vector<VTableInfo> m_vtables;
    std::unordered_map<std::string, size_t> m_classNameIndex; // className → vtable index

    // ES: Helpers: secciones, validación de punteros, desmangleado, lectura de COL y bases,
    //     y conteo de slots.
    // Internal helpers
    void FindSections();
    bool IsCodePointer(uintptr_t addr) const;
    bool IsInModule(uintptr_t addr) const;
    std::string DemangleName(const char* mangledName) const;
    bool ReadCOL(uintptr_t colAddr, VTableInfo& info);
    std::vector<std::string> ReadBaseClasses(int32_t classDescRVA);
    size_t CountVTableSlots(uintptr_t vtableAddr) const;
};

} // namespace kmp
