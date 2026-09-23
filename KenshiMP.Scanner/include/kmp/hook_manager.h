// ES: hook_manager.h — gestor central de hooks del mod (singleton sobre MinHook).
//     Un hook desvía una función del juego hacia una función nuestra (detour) y nos
//     da un "trampolín" para seguir llamando a la original. Antes de instalar valida
//     con .pdata que el destino es el inicio real de una función; si el prólogo empieza
//     por 'mov rax, rsp' (48 8B C4) aplica automáticamente el fix MovRaxRsp.
//     También gestiona hooks de vtable, activar/desactivar y diagnósticos por hook.
// EN: hook_manager.h — the mod's central hook manager (singleton on top of MinHook).
//     A hook redirects a game function to one of ours (detour) and gives us a
//     "trampoline" to keep calling the original. Before installing it checks with
//     .pdata that the target is a real function start; if the prologue starts with
//     'mov rax, rsp' (48 8B C4) it automatically applies the MovRaxRsp fix.
//     It also handles vtable hooks, enable/disable and per-hook diagnostics.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "kmp/mov_rax_rsp_fix.h"

namespace kmp {

// ES: Instantánea de diagnóstico de un hook (la devuelve GetDiagnostics): dirección,
//     primeros 8 bytes del original, estado, tipo de fix y contadores de llamadas/crashes.
// Diagnostics snapshot for a single hook (returned by GetDiagnostics)
struct HookDiag {
    std::string name;
    uintptr_t   targetAddr    = 0;
    uint8_t     prologue[8]   = {};      // First 8 bytes of original function
    bool        installed     = false;
    bool        enabled       = false;
    bool        isVtable      = false;
    bool        hasRelayThunk = false;    // DEPRECATED (always false)
    bool        hasCustomCaller = false;  // True if MovRaxRsp fix is active
    bool        hasMovRaxRspFix = false;  // True if naked detour + wrapper are active
    int         callCount     = 0;
    int         crashCount    = 0;
};

// ES: Singleton que instala, activa/desactiva y retira hooks. Protegido con mutex.
// EN: Singleton that installs, enables/disables and removes hooks. Mutex-protected.
class HookManager {
public:
    // ES: Acceso a la instancia única.
    // EN: Access to the single instance.
    static HookManager& Get();

    // ES: Inicializa MinHook / apaga (solo desactiva hooks, ver hook_manager.cpp).
    // EN: Initializes MinHook / shuts down (only disables hooks, see hook_manager.cpp).
    bool Initialize();
    void Shutdown();

    // ES: Instala un hook inline con MinHook. Si el destino empieza por 'mov rax, rsp'
    //     (48 8B C4) se aplica el fix MovRaxRsp: un detour "desnudo" captura RSP a la
    //     entrada y *original recibe un wrapper de trampolín que restaura RAX antes de
    //     entrar al cuerpo de la función original (evita la corrupción de RBP derivado de RAX).
    // Install an inline hook using MinHook.
    // If the target starts with `mov rax, rsp` (48 8B C4), the MovRaxRsp fix
    // is automatically applied: a naked detour captures RSP at hook entry, and
    // *original receives a trampoline wrapper that restores RAX before entering
    // the original function body. This fixes the RBP-derived-from-RAX corruption.
    template<typename T>
    bool Install(const std::string& name, T* target, T* detour, T** original) {
        return InstallRaw(name,
            reinterpret_cast<void*>(target),
            reinterpret_cast<void*>(detour),
            reinterpret_cast<void**>(original));
    }

    // ES: Igual que Install pero con el destino dado como dirección numérica.
    // Install a hook by address
    template<typename T>
    bool InstallAt(const std::string& name, uintptr_t address, T* detour, T** original) {
        return InstallRaw(name,
            reinterpret_cast<void*>(address),
            reinterpret_cast<void*>(detour),
            reinterpret_cast<void**>(original));
    }

    // ES: Quita un hook por nombre (restaura bytes/vtable y libera stubs).
    // Remove a specific hook by name
    bool Remove(const std::string& name);

    // ES: Quita todos los hooks. Ojo: su implementación asume que el llamador ya tiene
    //     el mutex, pero Shutdown() no la llama (nadie la usa con el mutex tomado).
    // Remove all hooks
    void RemoveAll();

    // ES: Activa/desactiva un hook sin quitarlo (en hooks MovRaxRsp usa el flag de bypass).
    // Enable/disable a hook without removing it
    bool Enable(const std::string& name);
    bool Disable(const std::string& name);

    // ES: Dirección original de la función (NO el trampolín), para llamarla directamente.
    // Get the original target address (NOT the trampoline) for direct calling
    void* GetTarget(const std::string& name) const;

    // ES: Stub "custom caller" del hook. OJO: el comentario original dice que en hooks
    //     MovRaxRsp devuelve el wrapper, pero la implementación devuelve customCaller, que
    //     siempre es nullptr (obsoleto). Para el original usar el puntero *original.
    // Get the custom caller stub for a hook (or nullptr if none).
    // For MovRaxRsp hooks, this returns the trampoline wrapper.
    void* GetCustomCaller(const std::string& name) const;

    // ES: Trampolín crudo de MinHook de un hook MovRaxRsp (empieza por 'mov rax, rsp' y no
    //     usa los slots globales). Seguro para llamadas REENTRANTES. nullptr si no aplica.
    // Get MinHook's raw trampoline for a MovRaxRsp hook.
    // This starts with `mov rax, rsp` and does NOT use the global data slots.
    // Safe for REENTRANT calls where the wrapper would corrupt the outer call's state.
    // Returns nullptr for non-MovRaxRsp hooks or if the hook doesn't exist.
    void* GetRawTrampoline(const std::string& name) const;

    // ES: ¿Está instalado el hook?
    // Check if a hook is installed
    bool IsInstalled(const std::string& name) const;

    // ES: Número de hooks registrados.
    // Get hook count
    size_t GetHookCount() const;

    // ES: Hook de vtable: sustituye la entrada 'index' de la vtable por nuestro detour.
    // Install a vtable hook (swap vtable entry)
    bool InstallVTableHook(const std::string& name, void** vtable, int index,
                           void* detour, void** original);

    // ── Diagnostics ──

    // ES: Diagnóstico de todos los hooks (copia segura entre hilos).
    // Get diagnostics for all hooks (thread-safe snapshot)
    std::vector<HookDiag> GetDiagnostics() const;

    // ES: Contadores de llamadas/crashes (los llaman los detours).
    // Increment call/crash counters (called from hook detours)
    void IncrementCallCount(const std::string& name);
    void IncrementCrashCount(const std::string& name);

private:
    // ES: Constructor/destructor privados (singleton).
    // EN: Private constructor/destructor (singleton).
    HookManager() = default;
    ~HookManager() = default;

    // ES: Instalación real (común a Install/InstallAt): validación .pdata, análisis de
    //     prólogo y camino MovRaxRsp o estándar.
    // EN: Real installation (shared by Install/InstallAt): .pdata validation, prologue
    //     analysis and MovRaxRsp or standard path.
    bool InstallRaw(const std::string& name, void* target, void* detour, void** original);

    // ES: OBSOLETO: relay thunk para 'mov rax, rsp' (desactivado, devuelve nullptr).
    // Build a relay thunk for functions starting with `mov rax, rsp` (48 8B C4).
    // DEPRECATED: Relay thunks are disabled. Use MovRaxRsp fix instead.
    void* BuildRelayThunk(const std::string& name, void* trampoline);

    // ES: OBSOLETO: stub "custom caller" (mov rax,rsp; jmp original+3). Sustituido por el fix MovRaxRsp.
    // Build a custom caller stub for functions starting with `mov rax, rsp`.
    // DEPRECATED: Superseded by MovRaxRsp fix (BuildMovRaxRspHook).
    void* BuildCustomCaller(const std::string& name, uintptr_t originalAddr);

    // ES: Registro interno de un hook: destino, detour, trampolín, estado, datos de vtable,
    //     stubs del fix MovRaxRsp y datos de diagnóstico.
    // EN: Internal hook record: target, detour, trampoline, state, vtable data,
    //     MovRaxRsp fix stubs and diagnostics data.
    struct HookEntry {
        std::string name;
        void*       target   = nullptr;
        void*       detour   = nullptr;
        void*       original = nullptr;     // Raw trampoline from MinHook
        bool        enabled  = false;
        bool        isVtable = false;
        void**      vtableAddr = nullptr;
        int         vtableIndex = -1;

        // ── MovRaxRsp fix ──
        MovRaxRspHook movRaxRspHook = {}; // Naked detour + trampoline wrapper (if applicable)

        // ── Diagnostics ──
        uint8_t     prologueBytes[8] = {};  // First 8 bytes of original function
        void*       relayThunk = nullptr;   // VirtualAlloc'd relay thunk (DEPRECATED, always nullptr)
        void*       customCaller = nullptr; // DEPRECATED: use movRaxRspHook.trampolineWrapper
        bool        hasMovRaxRsp = false;   // True if target starts with 48 8B C4
        int         callCount  = 0;         // Protected by m_mutex
        int         crashCount = 0;         // Protected by m_mutex
    };

    // ES: Mapa nombre -> hook, mutex y estado de inicialización.
    // EN: Name -> hook map, mutex and init state.
    std::unordered_map<std::string, HookEntry> m_hooks;
    mutable std::mutex m_mutex;
    bool m_initialized = false;
};

// ES: Guard RAII que desactiva un hook al construirse y lo reactiva al destruirse.
//     OBSOLETO: con el fix MovRaxRsp ya no hace falta; se mantiene por compatibilidad.
// RAII guard: disables a hook on construction, re-enables on destruction.
// DEPRECATED: With the MovRaxRsp fix, this is no longer needed. The trampoline
// wrapper handles RAX correction automatically. Kept for backward compatibility.
class HookBypass {
public:
    HookBypass(const std::string& name) : m_name(name) {
        HookManager::Get().Disable(name);
    }
    ~HookBypass() {
        HookManager::Get().Enable(m_name);
    }
    HookBypass(const HookBypass&) = delete;
    HookBypass& operator=(const HookBypass&) = delete;
private:
    std::string m_name;
};

} // namespace kmp
