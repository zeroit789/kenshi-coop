// ES: memory.h — utilidades de acceso a memoria del proceso del juego (kenshi_x64.exe).
//     Lecturas/escrituras protegidas con SEH (__try/__except) para que un puntero
//     inválido no tumbe el juego, parcheo de bytes con VirtualProtect y recorrido
//     de cadenas de punteros estilo Cheat Engine (base + offset -> +offset -> ...).
//     La usan el escáner, el orquestador y el Core para leer estructuras del juego.
// EN: memory.h — helpers to access the game process memory (kenshi_x64.exe).
//     SEH-protected reads/writes (__try/__except) so an invalid pointer does not
//     crash the game, byte patching with VirtualProtect, and Cheat Engine style
//     pointer chain walking (base + offset -> +offset -> ...).
//     Used by the scanner, the orchestrator and Core to read game structures.
#pragma once
#include <cstdint>
#include <cstring>
#include <Windows.h>

namespace kmp {

// ES: Clase estática con todas las operaciones de memoria "seguras".
//     Todas las funciones devuelven false/0 en vez de lanzar si la dirección no es válida.
// EN: Static class with all the "safe" memory operations.
//     Every function returns false/0 instead of throwing if the address is invalid.
class Memory {
public:
    // ES: Lectura segura de memoria del juego: copia sizeof(T) bytes desde 'address' a 'out'.
    //     Si la lectura provoca una violación de acceso, el SEH la captura y devuelve false.
    // Safe read from game memory (handles access violations)
    template<typename T>
    static bool Read(uintptr_t address, T& out) {
        __try {
            out = *reinterpret_cast<T*>(address);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // ES: Escritura segura: cambia temporalmente la protección de la página a RWX,
    //     escribe el valor y restaura la protección original. Devuelve false si falla
    //     VirtualProtect o la escritura provoca una excepción.
    // Safe write to game memory
    template<typename T>
    static bool Write(uintptr_t address, const T& value) {
        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(T),
                           PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        __try {
            *reinterpret_cast<T*>(address) = value;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), oldProtect, &oldProtect);
            return false;
        }
        VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), oldProtect, &oldProtect);
        return true;
    }

    // ES: Sigue una cadena de punteros: lee *(base+off0), luego *(resultado+off1), etc.
    //     El array de offsets termina en -1. Devuelve la dirección final o 0 si algún salto falla.
    //     Ejemplo: cadenas de Cheat Engine de patterns.h (KNOWN_CHAINS) sobre PlayerBase.
    // Follow a pointer chain: base + offset1 -> +offset2 -> ... -> final value
    // Offsets array terminated by -1
    static uintptr_t FollowChain(uintptr_t base, const int* offsets) {
        uintptr_t addr = base;
        for (int i = 0; offsets[i] != -1; i++) {
            uintptr_t next;
            if (!Read(addr + offsets[i], next)) return 0;
            addr = next;
        }
        return addr;
    }

    // ES: Lee un float a través de una cadena de punteros: desreferencia todos los offsets
    //     menos el último, y en el último lee el valor (no lo desreferencia como puntero).
    //     Se usa p.ej. para vida (Health) con la cadena +0x2B8 -> +0x5F8 -> +0x40.
    // EN: (Used e.g. for Health with the chain +0x2B8 -> +0x5F8 -> +0x40.)
    // Read a float through a pointer chain
    static bool ReadChainFloat(uintptr_t base, const int* offsets, float& out) {
        uintptr_t addr = base;
        // Follow all offsets except the last
        int i = 0;
        while (offsets[i] != -1 && offsets[i + 1] != -1) {
            uintptr_t next;
            if (!Read(addr + offsets[i], next)) return false;
            addr = next;
            i++;
        }
        // Last offset: read the actual value
        if (offsets[i] != -1) {
            return Read(addr + offsets[i], out);
        }
        return Read(addr, out);
    }

    // ES: Lee un vector 3D (tres floats consecutivos: x, y, z), típico de posiciones del juego.
    // Read a Vec3 (3 consecutive floats)
    static bool ReadVec3(uintptr_t address, float& x, float& y, float& z) {
        struct { float x, y, z; } v;
        if (!Read(address, v)) return false;
        x = v.x; y = v.y; z = v.z;
        return true;
    }

    // ES: Parchea 'len' bytes en memoria (típicamente código en .text), cambiando la
    //     protección a RWX mientras copia. Sin SEH: la dirección debe ser válida.
    // Patch bytes in memory (with protection change)
    static bool Patch(uintptr_t address, const uint8_t* bytes, size_t len) {
        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void*>(address), len,
                           PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(address), bytes, len);
        VirtualProtect(reinterpret_cast<void*>(address), len, oldProtect, &oldProtect);
        return true;
    }

    // ES: Rellena 'len' bytes con 0x90 (NOP) para anular instrucciones del juego.
    // NOP out instructions
    static bool Nop(uintptr_t address, size_t len) {
        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void*>(address), len,
                           PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        std::memset(reinterpret_cast<void*>(address), 0x90, len);
        VirtualProtect(reinterpret_cast<void*>(address), len, oldProtect, &oldProtect);
        return true;
    }

    // ES: Devuelve la dirección base de un módulo cargado (nullptr = el ejecutable principal,
    //     es decir kenshi_x64.exe). Base + RVA = dirección real en runtime.
    // Get module base address
    static uintptr_t GetModuleBase(const char* moduleName = nullptr) {
        HMODULE hMod = moduleName ? GetModuleHandleA(moduleName) : GetModuleHandleA(nullptr);
        return reinterpret_cast<uintptr_t>(hMod);
    }
};

} // namespace kmp
