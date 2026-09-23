// ES: mov_rax_rsp_fix.h — fix para hookear funciones cuyo prólogo empieza por
//     'mov rax, rsp' (48 8B C4). Muchas funciones de Kenshi (MSVC x64) hacen
//     mov rax,rsp; push ...; lea rbp,[rax-0xNN]: el compilador hace coincidir los
//     slots de los push con offsets [rbp+XX]. Si hay un solo byte extra en la pila (p.ej.
//     la dirección de retorno de nuestro hook) todo se desplaza 8 bytes y se corrompen
//     los locales. Solución: dos stubs ASM generados en runtime por hook:
//     1) DETOUR DESNUDO (a donde salta MinHook): guarda RSP y la dirección de retorno del
//        juego, abre un hueco de 4 KB+8 en la pila, LLAMA (call) al hook C++ y al volver
//        restaura la dirección de retorno y hace ret al juego.
//     2) WRAPPER DE TRAMPOLÍN (lo que el hook C++ llama como "original"): cambia RSP a la
//        pila capturada del juego, parchea [RSP] con return_point, pone RAX = RSP y salta
//        a trampolín+3 (saltando el mov rax,rsp). La original corre sin bytes extra.
//     Hilos: los slots de datos son globales por hook (no TLS); es seguro porque la
//     lógica de Kenshi es de un solo hilo y hay guard de reentrancia.
// EN: mov_rax_rsp_fix.h — fix to hook functions whose prologue starts with
//     'mov rax, rsp' (48 8B C4). Many Kenshi functions (MSVC x64) do
//     mov rax,rsp; push ...; lea rbp,[rax-0xNN]: the compiler aliases the push slots
//     with [rbp+XX] offsets. A single extra item on the stack (e.g. our hook's return
//     address) shifts everything by 8 bytes and corrupts the locals. Solution: two
//     runtime-generated ASM stubs per hook (naked detour + trampoline wrapper; details
//     in the original English comment below). Threads: data slots are global per hook
//     (not TLS); safe because Kenshi game logic is single-threaded plus a reentrancy guard.
#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  MOV RAX, RSP HOOKING FIX — Return-Address Patching with Stack Gap
// ═══════════════════════════════════════════════════════════════════════════
//
//  Problem: Many Kenshi functions (MSVC x64) start with:
//      mov rax, rsp          ; 48 8B C4 — snapshot caller's RSP
//      push rbp              ; 55
//      push rsi/rdi/r12-r15  ; register saves
//      lea rbp, [rax-0xNN]   ; derive RBP from RAX
//      sub rsp, 0xMM         ; allocate locals
//
//  The compiler ALIASES push-saved register slots with [rbp+XX] frame
//  offsets (e.g., [rbp+0x150] IS the push rbp slot).  Any extra data on
//  the stack shifts pushes by 8 bytes, breaking the aliasing.
//
//  Solution: Two runtime-generated ASM stubs per hook.
//
//  1. NAKED DETOUR (what MinHook JMPs to):
//     Saves RSP and game's return address, creates a 4KB+8 stack gap
//     (extra 8 for 16-byte alignment), then CALLs the C++ hook.
//     When the C++ hook returns, restores the game's return address
//     at [RSP] and RETs to the game caller.
//
//  2. TRAMPOLINE WRAPPER (what the C++ hook calls as "original"):
//     Swaps RSP to captured game RSP, patches [RSP] with a return_point
//     address, sets RAX = RSP, JMPs to trampoline+3 (past mov rax,rsp).
//     The original function runs with ZERO extra bytes on stack — perfect
//     alignment.  It RETs to return_point, which swaps back and returns
//     to the C++ hook.
//
//  Thread safety: Each hook has its own data slots.  Kenshi game logic is
//  single-threaded.  For multi-thread safety, switch to TLS.
//
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>

namespace kmp {

// ES: Resultado de construir el fix para un hook: detour desnudo (destino del salto de
//     MinHook), wrapper (lo que el hook C++ llama como original), trampolín crudo
//     (seguro para reentrancia), slot de RSP capturado, flag de bypass (1 = pasar directo
//     a la original, 0 = hook activo) y la página reservada.
// EN: Result of building the fix for a hook: naked detour (MinHook jump target),
//     wrapper (what the C++ hook calls as original), raw trampoline (reentrancy-safe),
//     captured RSP slot, bypass flag (1 = pass straight to original, 0 = hook active)
//     and the allocated page.
struct MovRaxRspHook {
    void* nakedDetour       = nullptr;  // MinHook relay should JMP here
    void* trampolineWrapper = nullptr;  // C++ hook should CALL this as "original"
    void* rawTrampoline     = nullptr;  // MinHook's raw trampoline (starts with mov rax,rsp)
                                        // Safe for REENTRANT calls — no global slot manipulation
    void* capturedRspSlot   = nullptr;  // Pointer to uint64_t holding captured RSP
    volatile int32_t* bypassFlag = nullptr; // Pointer to bypass flag (1=passthrough, 0=hook active)
    void* allocBase         = nullptr;  // VirtualAlloc base (for cleanup)
    size_t allocSize        = 0;        // Allocation size
};

// ES: Uso y parámetros de la construcción de stubs (ver comentario en inglés).
//     Constantes de la página: tamaño 0x200 y detour desnudo en +0x40.
// Build the two ASM stubs for a mov-rax-rsp function hook.
//
// Usage:
//   1. MH_CreateHook(target, cppDetour, &trampoline)
//   2. BuildMovRaxRspHook(name, cppDetour, trampoline, 3)
//   3. Patch MinHook's relay to JMP to hook.nakedDetour
//   4. Set *original = hook.trampolineWrapper
//
// Parameters:
//   name             - Hook name for logging
//   cppDetour        - Your C++ __fastcall hook function
//   trampoline       - MinHook's trampoline (from MH_CreateHook's ppOriginal)
//   trampolineOffset - Bytes to skip in trampoline (3 for `mov rax, rsp`)
// Page layout constants
constexpr int MOVRAXRSP_PAGE_SIZE    = 0x200;
constexpr int MOVRAXRSP_NAKED_OFFSET = 0x40;   // naked detour stub starts here

// ES: Reserva una página ejecutable (RWX) rellena de INT3 con los slots de datos a cero.
//     El detour desnudo estará en página + MOVRAXRSP_NAKED_OFFSET: esa dirección se pasa
//     a MH_CreateHook como detour y después se llama a BuildMovRaxRspHookAt.
// Allocate a page for MovRaxRsp fix stubs.
// Returns executable memory (PAGE_EXECUTE_READWRITE) with INT3 fill + zeroed data slots.
// The naked detour address is at (returned_ptr + MOVRAXRSP_NAKED_OFFSET).
// Pass that address to MH_CreateHook as the detour, then call BuildMovRaxRspHookAt.
void* AllocMovRaxRspPage();

// ES: Genera el detour desnudo y el wrapper en una página ya reservada. Primero se llama
//     a MH_CreateHook con (página + 0x40) como detour y luego a esta con el trampolín
//     obtenido. Si va bien, result.allocBase == página; si falla, el llamador libera la página.
// Build naked detour + trampoline wrapper into a pre-allocated page.
// The page must have been allocated by AllocMovRaxRspPage().
// Call MH_CreateHook FIRST with (page + MOVRAXRSP_NAKED_OFFSET) as detour,
// then call this with the resulting trampoline.
//
// On success, result.allocBase == page (ownership transferred to result).
// On failure, returns empty result — caller must VirtualFree the page.
MovRaxRspHook BuildMovRaxRspHookAt(
    void* page,
    const std::string& name,
    void* cppDetour,
    void* trampoline,
    int trampolineOffset = 3
);

// ES: API antigua: reserva su propia página. Para código nuevo usar Alloc + BuildAt.
// Legacy API — allocates its own page internally.
// For new code, prefer AllocMovRaxRspPage + BuildMovRaxRspHookAt.
MovRaxRspHook BuildMovRaxRspHook(
    const std::string& name,
    void* cppDetour,
    void* trampoline,
    int trampolineOffset = 3
);

// ES: Libera la página de stubs de un hook.
// EN: Frees a hook's stub page.
void FreeMovRaxRspHook(MovRaxRspHook& hook);

// ES: ¿Empieza el trampolín por 48 8B C4 (mov rax, rsp)?
// EN: Does the trampoline start with 48 8B C4 (mov rax, rsp)?
bool TrampolineHasMovRaxRsp(void* trampoline);

} // namespace kmp
