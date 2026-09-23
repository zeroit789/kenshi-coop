// ES: Implementación del rastreador de personajes. El hook inline de CharAnimUpdate salta a
//     un trampolín RWX construido a mano que guarda registros, llama a SEH_OnCharUpdate con
//     RBX (AnimationClassHuman*), restaura registros, ejecuta los 14 bytes originales y
//     vuelve. Corre en el hilo del juego que anima personajes, 300+ veces por segundo, así
//     que el cuerpo evita heap, mutex bloqueante y spdlog: los personajes nuevos van a un
//     buffer circular y ProcessDeferredDiscovery() (desde OnGameTick) lee el nombre y los
//     inserta en el mapa.
// EN: Implementation of the character tracker. The CharAnimUpdate inline hook jumps to a
//     hand-built RWX trampoline that saves registers, calls SEH_OnCharUpdate with RBX
//     (AnimationClassHuman*), restores registers, runs the original 14 bytes and jumps back.
//     It runs on the game thread that animates characters, 300+ times per second, so the
//     body avoids heap, blocking mutexes and spdlog: new characters go into a ring buffer
//     and ProcessDeferredDiscovery() (from OnGameTick) reads the name and inserts them.
#include "char_tracker_hooks.h"
#include "../core.h"
#include "../game/game_types.h"
#include "../game/spawn_manager.h"
#include "kmp/hook_manager.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <unordered_map>
#include <mutex>

namespace kmp::char_tracker_hooks {

// ES: Estado: mapa CharacterHuman* -> TrackedChar (bajo s_trackerMutex), callback de
//     personaje nuevo y AnimClass del jugador local (nunca se asigna en este fichero).
// EN: State: CharacterHuman* -> TrackedChar map (under s_trackerMutex), new-character
//     callback and local player's AnimClass (never assigned in this file).
static std::mutex s_trackerMutex;
static std::unordered_map<void*, TrackedChar> s_trackedChars;
static std::function<void(const TrackedChar&)> s_onNewChar;
static void* s_localPlayerAnimClass = nullptr;

// ES: Buffer circular de descubrimiento diferido. El hook inline se dispara 300+ veces/s:
//     en su cuerpo NO puede haber heap, mutex, spdlog ni CharacterAccessor. Se guardan los
//     punteros crudos en un buffer sin bloqueos (128 huecos, un productor/un consumidor) y
//     ProcessDeferredDiscovery() hace la lectura de nombre y la inserción.
// ── Deferred discovery ring buffer ──
// The inline hook fires 300+ times/sec. We MUST avoid heap allocation, mutex,
// spdlog, and CharacterAccessor in the hook body. Instead, record raw pointers
// into a lock-free ring buffer. ProcessDeferredDiscovery() (called from
// OnGameTick) does the expensive name lookup and map insertion.
struct PendingCharUpdate {
    void* animClassPtr;
    uintptr_t charPtr;
};
static constexpr int PENDING_RING_SIZE = 128;
static PendingCharUpdate s_pendingRing[PENDING_RING_SIZE];
static std::atomic<int> s_pendingWrite{0};
static std::atomic<int> s_pendingRead{0};

// ES: Lógica del hook por cada actualización de animación: valida el puntero, lee el
//     CharacterHuman* en animClass+0x2D8 y, si ya está rastreado, refresca AnimClass y
//     marca de tiempo (solo si try_lock lo consigue, sin esperar); si es nuevo, lo encola.
// EN: Hook logic per animation update: validates the pointer, reads the CharacterHuman* at
//     animClass+0x2D8 and, if already tracked, refreshes AnimClass and timestamp (only when
//     try_lock succeeds, never waiting); if new, it is queued.
static void OnCharUpdate(void* animClassHuman) {
    if (!animClassHuman) return;
    uintptr_t animPtr = reinterpret_cast<uintptr_t>(animClassHuman);
    if (animPtr < 0x10000 || animPtr > 0x00007FFFFFFFFFFF) return;

    uintptr_t charPtr = 0;
    if (!Memory::Read(animPtr + 0x2D8, charPtr) || charPtr == 0) return;
    if (charPtr < 0x10000 || charPtr > 0x00007FFFFFFFFFFF) return;

    void* charKey = reinterpret_cast<void*>(charPtr);

    // ES: Camino rápido: ya rastreado, solo actualizar la marca de tiempo. Se usa try_lock: si
    //     el mutex está ocupado se salta esta actualización (sin bloquear el hilo del juego).
    // Fast path: already tracked — just update timestamp (atomic, no mutex)
    // Use a simple spinlock-free check: try_lock fails = skip this update (no stall)
    if (s_trackerMutex.try_lock()) {
        auto it = s_trackedChars.find(charKey);
        if (it != s_trackedChars.end()) {
            it->second.animClassPtr = animClassHuman;
            it->second.lastSeenTick = GetTickCount64();
            s_trackerMutex.unlock();
            return;
        }
        s_trackerMutex.unlock();
    }

    // ES: Camino lento: personaje nuevo, encolar en el buffer para procesarlo después.
    //     Sin heap, sin mutex, sin spdlog.
    // Slow path: new character — push to ring buffer for deferred processing.
    // NO heap allocation, NO mutex hold, NO spdlog in this path.
    int writeIdx = s_pendingWrite.load(std::memory_order_relaxed);
    int nextIdx = (writeIdx + 1) % PENDING_RING_SIZE;
    if (nextIdx != s_pendingRead.load(std::memory_order_acquire)) {
        s_pendingRing[writeIdx] = {animClassHuman, charPtr};
        s_pendingWrite.store(nextIdx, std::memory_order_release);
    }
    // ES: Si el buffer está lleno se descarta; se recogerá en el siguiente tick de animación.
    // If ring is full, drop this update (will be caught on next animation tick)
}

// ES: Hook inline: copia de los 14 bytes originales del sitio parcheado y memoria del trampolín.
// EN: Inline hook: copy of the 14 original bytes of the patched site and trampoline memory.
// ── Inline Hook Implementation ──
static uint8_t s_originalBytes[14] = {};
static void* s_trampolineAlloc = nullptr;

// ES: Envoltorio SEH que llama el trampolín; traga cualquier excepción para no tumbar el juego.
// EN: SEH wrapper called by the trampoline; swallows any exception so the game is not taken down.
static void SEH_OnCharUpdate(void* animClassHuman) {
    __try {
        OnCharUpdate(animClassHuman);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// ES: Construye el trampolín y parchea el sitio. Pasos: 1) copiar los 14 bytes originales;
//     2) reservar 256 bytes RWX; 3) emitir: push de registros, sub rsp 0x28 (shadow space),
//     mov rcx, rbx, call indirecto a SEH_OnCharUpdate, add rsp, pop de registros, los 14
//     bytes originales y jmp absoluto a hookAddr+14; 4) sobrescribir el sitio con
//     `FF 25 00000000 <addr64>` (jmp absoluto de 14 bytes) al trampolín. Supone que los 14
//     bytes originales se pueden reubicar tal cual (sin direccionamiento relativo a RIP).
// EN: Builds the trampoline and patches the site. Steps: 1) copy the 14 original bytes;
//     2) allocate 256 RWX bytes; 3) emit: register pushes, sub rsp 0x28 (shadow space),
//     mov rcx, rbx, indirect call to SEH_OnCharUpdate, add rsp, register pops, the 14
//     original bytes and an absolute jmp to hookAddr+14; 4) overwrite the site with
//     `FF 25 00000000 <addr64>` (14-byte absolute jmp) to the trampoline. Assumes the 14
//     original bytes can be relocated as-is (no RIP-relative addressing).
static bool BuildInlineHook(uintptr_t hookAddr) {
    __try {
        memcpy(s_originalBytes, reinterpret_cast<void*>(hookAddr), 14);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("char_tracker: Failed to read original bytes at 0x{:X}", hookAddr);
        return false;
    }

    size_t trampolineSize = 256;
    s_trampolineAlloc = VirtualAlloc(nullptr, trampolineSize,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s_trampolineAlloc) {
        spdlog::error("char_tracker: VirtualAlloc failed for trampoline");
        return false;
    }

    uint8_t* code = static_cast<uint8_t*>(s_trampolineAlloc);
    int off = 0;

    // ES: Guardar los registros de propósito general (push rax..r15, sin rsp).
    // Save all registers
    uint8_t saveRegs[] = {
        0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,
        0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53,
        0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57
    };
    memcpy(code + off, saveRegs, sizeof(saveRegs));
    off += sizeof(saveRegs);

    // ES: Reservar el shadow space de 32 bytes (+8) que exige la ABI x64 de Windows.
    // sub rsp, 0x28 (shadow space)
    code[off++] = 0x48; code[off++] = 0x83; code[off++] = 0xEC; code[off++] = 0x28;

    // ES: Primer argumento = RBX, donde está el AnimationClassHuman* en el sitio del hook.
    // mov rcx, rbx (AnimationClassHuman* is in RBX)
    code[off++] = 0x48; code[off++] = 0x89; code[off++] = 0xD9;

    // ES: Llamada indirecta al puntero de 8 bytes que sigue, saltando por encima de él.
    // call [rip+2]; jmp over ptr
    code[off++] = 0xFF; code[off++] = 0x15; code[off++] = 0x02;
    code[off++] = 0x00; code[off++] = 0x00; code[off++] = 0x00;
    code[off++] = 0xEB; code[off++] = 0x08;
    uintptr_t funcAddr = reinterpret_cast<uintptr_t>(&SEH_OnCharUpdate);
    memcpy(code + off, &funcAddr, 8);
    off += 8;

    // add rsp, 0x28
    code[off++] = 0x48; code[off++] = 0x83; code[off++] = 0xC4; code[off++] = 0x28;

    // ES: Restaurar los registros en orden inverso.
    // Restore all registers
    uint8_t restoreRegs[] = {
        0x41,0x5F, 0x41,0x5E, 0x41,0x5D, 0x41,0x5C,
        0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58,
        0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58
    };
    memcpy(code + off, restoreRegs, sizeof(restoreRegs));
    off += sizeof(restoreRegs);

    // ES: Ejecutar los 14 bytes originales que se sobrescribieron.
    // Execute original 14 bytes
    memcpy(code + off, s_originalBytes, 14);
    off += 14;

    // ES: Volver al código del juego justo después del parche.
    // jmp back to hookAddr + 14
    code[off++] = 0xFF; code[off++] = 0x25;
    code[off++] = 0x00; code[off++] = 0x00; code[off++] = 0x00; code[off++] = 0x00;
    uintptr_t returnAddr = hookAddr + 14;
    memcpy(code + off, &returnAddr, 8);
    off += 8;

    // ES: Parchear el código original para que salte al trampolín.
    // Patch original code to jump to trampoline
    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(hookAddr), 14, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        spdlog::error("char_tracker: VirtualProtect failed for hook site");
        VirtualFree(s_trampolineAlloc, 0, MEM_RELEASE);
        s_trampolineAlloc = nullptr;
        return false;
    }

    uint8_t* hookSite = reinterpret_cast<uint8_t*>(hookAddr);
    hookSite[0] = 0xFF; hookSite[1] = 0x25;
    hookSite[2] = 0x00; hookSite[3] = 0x00; hookSite[4] = 0x00; hookSite[5] = 0x00;
    uintptr_t trampolineAddr = reinterpret_cast<uintptr_t>(s_trampolineAlloc);
    memcpy(hookSite + 6, &trampolineAddr, 8);

    VirtualProtect(reinterpret_cast<void*>(hookAddr), 14, oldProtect, &oldProtect);

    spdlog::info("char_tracker: Inline hook installed at 0x{:X} -> trampoline 0x{:X}",
                 hookAddr, trampolineAddr);
    return true;
}

// ES: Búsqueda lineal por nombre bajo el mutex (ver aviso de vida del puntero en el .h).
// EN: Linear search by name under the mutex (see the pointer lifetime warning in the .h).
const TrackedChar* FindByName(const std::string& name) {
    std::lock_guard lock(s_trackerMutex);
    for (auto& [key, tc] : s_trackedChars) {
        if (tc.name == name) return &tc;
    }
    return nullptr;
}

// ES: Búsqueda directa por puntero de personaje.
// EN: Direct lookup by character pointer.
const TrackedChar* FindByPtr(void* characterPtr) {
    std::lock_guard lock(s_trackerMutex);
    auto it = s_trackedChars.find(characterPtr);
    return (it != s_trackedChars.end()) ? &it->second : nullptr;
}

// EN: Change 2.1: purges the entry for one specific pointer. Called from the engine destroy
//     hook (Hook_CharacterDestroy) so a destroyed Character does not leave a dangling entry
//     pointing at memory the engine may reuse. Uses the same mutex as the rest of the file.
// Cambio 2.1: purga la entrada de un puntero concreto del tracker.
// Llamado desde el destroy-hook del motor (Hook_CharacterDestroy) para que un Character
// destruido no deje una entrada colgada apuntando a memoria que el motor puede reciclar.
// Usa el mismo mutex que el resto de funciones del fichero.
void RemoveByPtr(void* ptr) {
    std::lock_guard lock(s_trackerMutex);
    s_trackedChars.erase(ptr);
}

// EN: Change 2.2: empties the WHOLE tracker at once. Called on disconnect / hot save
//     reload: every cached pointer becomes invalid (the engine frees the previous
//     session's Characters).
// Cambio 2.2: vacía TODO el tracker de golpe.
// Llamado en desconexión / recarga de save en caliente: todos los punteros cacheados
// pasan a ser inválidos (el motor libera los Character de la sesión/partida anterior).
void Clear() {
    std::lock_guard lock(s_trackerMutex);
    s_trackedChars.clear();
}

// ES: Devuelve s_localPlayerAnimClass (siempre nullptr hoy, nadie lo asigna).
// EN: Returns s_localPlayerAnimClass (always nullptr today; nothing assigns it).
void* GetLocalPlayerAnimClass() { return s_localPlayerAnimClass; }

// ES: AnimClass del personaje con ese nombre, o nullptr.
// EN: AnimClass of the character with that name, or nullptr.
void* GetRemotePlayerAnimClass(const std::string& name) {
    auto* tc = FindByName(name);
    return tc ? tc->animClassPtr : nullptr;
}

// ES: Registra el callback que se llama al descubrir un personaje nuevo.
// EN: Registers the callback invoked when a new character is discovered.
void SetOnNewCharacter(std::function<void(const TrackedChar&)> callback) {
    std::lock_guard lock(s_trackerMutex);
    s_onNewChar = callback;
}

// ES: Número de personajes rastreados.
// EN: Number of tracked characters.
int GetTrackedCount() {
    std::lock_guard lock(s_trackerMutex);
    return static_cast<int>(s_trackedChars.size());
}

// ES: Vuelca al log todos los personajes rastreados con su antigüedad.
// EN: Dumps every tracked character with its age to the log.
void DumpTrackedChars() {
    std::lock_guard lock(s_trackerMutex);
    uint64_t now = GetTickCount64();
    spdlog::info("char_tracker: {} tracked characters:", s_trackedChars.size());
    for (auto& [key, tc] : s_trackedChars) {
        float ageSec = (now - tc.lastSeenTick) / 1000.f;
        spdlog::info("  '{}' at 0x{:X} (animClass=0x{:X}), pos=({:.0f},{:.0f},{:.0f}), age={:.1f}s",
                     tc.name, reinterpret_cast<uintptr_t>(tc.characterPtr),
                     reinterpret_cast<uintptr_t>(tc.animClassPtr),
                     tc.position.x, tc.position.y, tc.position.z, ageSec);
    }
}

// ES: Consume hasta 8 entradas del buffer por tick (hilo de OnGameTick). Si el personaje
//     ya está, solo lo refresca; si no, lee nombre y posición con CharacterAccessor, lo
//     inserta en el mapa, lo loguea y llama al callback (fuera del mutex).
// EN: Consumes up to 8 ring entries per tick (OnGameTick thread). If the character is
//     already known it only refreshes it; otherwise it reads name and position with
//     CharacterAccessor, inserts it, logs it and invokes the callback (outside the mutex).
void ProcessDeferredDiscovery() {
    int processed = 0;
    while (processed < 8) { // Cap per tick to avoid stalls
        int readIdx = s_pendingRead.load(std::memory_order_relaxed);
        if (readIdx == s_pendingWrite.load(std::memory_order_acquire)) break; // Empty
        PendingCharUpdate pending = s_pendingRing[readIdx];
        s_pendingRead.store((readIdx + 1) % PENDING_RING_SIZE, std::memory_order_release);
        processed++;

        void* charKey = reinterpret_cast<void*>(pending.charPtr);

        // ES: Comprobar si ya está rastreado (pudo añadirse entre el encolado y ahora).
        // Check if already tracked (could have been added between push and now)
        {
            std::lock_guard lock(s_trackerMutex);
            if (s_trackedChars.count(charKey) > 0) {
                s_trackedChars[charKey].animClassPtr = pending.animClassPtr;
                s_trackedChars[charKey].lastSeenTick = GetTickCount64();
                continue;
            }
        }

        // ES: Trabajo caro: leer el nombre, construir el TrackedChar e insertarlo. Sin nombre se descarta.
        // Now do the expensive work: read name, build TrackedChar, insert
        game::CharacterAccessor accessor(charKey);
        std::string name = accessor.GetName();
        if (name.empty()) continue;

        TrackedChar tc;
        tc.animClassPtr = pending.animClassPtr;
        tc.characterPtr = charKey;
        tc.name = name;
        tc.position = accessor.GetPosition();
        tc.lastSeenTick = GetTickCount64();

        {
            std::lock_guard lock(s_trackerMutex);
            s_trackedChars[charKey] = tc;
        }

        spdlog::info("char_tracker: NEW character '{}' at 0x{:X} (animClass=0x{:X})",
                     name, pending.charPtr, reinterpret_cast<uintptr_t>(pending.animClassPtr));

        if (s_onNewChar) {
            s_onNewChar(tc);
        }
    }
}

// ES: Instala el hook inline en la dirección CharAnimUpdate resuelta por el escáner.
// EN: Installs the inline hook at the scanner-resolved CharAnimUpdate address.
bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();

    if (!funcs.CharAnimUpdate) {
        spdlog::warn("char_tracker: CharAnimUpdate address not resolved — hook not installed");
        return false;
    }

    uintptr_t hookAddr = reinterpret_cast<uintptr_t>(funcs.CharAnimUpdate);
    spdlog::info("char_tracker: Installing inline hook at 0x{:X}", hookAddr);

    return BuildInlineHook(hookAddr);
}

// ES: Si hay trampolín: restaura los 14 bytes originales del sitio y libera la memoria RWX.
// EN: If there is a trampoline: restores the site's 14 original bytes and frees the RWX memory.
void Uninstall() {
    if (s_trampolineAlloc) {
        auto& funcs = Core::Get().GetGameFunctions();
        if (funcs.CharAnimUpdate) {
            uintptr_t hookAddr = reinterpret_cast<uintptr_t>(funcs.CharAnimUpdate);
            DWORD oldProtect;
            if (VirtualProtect(reinterpret_cast<void*>(hookAddr), 14, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy(reinterpret_cast<void*>(hookAddr), s_originalBytes, 14);
                VirtualProtect(reinterpret_cast<void*>(hookAddr), 14, oldProtect, &oldProtect);
            }
        }
        VirtualFree(s_trampolineAlloc, 0, MEM_RELEASE);
        s_trampolineAlloc = nullptr;
    }
}

} // namespace kmp::char_tracker_hooks
