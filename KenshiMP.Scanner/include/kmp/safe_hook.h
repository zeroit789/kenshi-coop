// ES: safe_hook.h — envoltorios SEH para llamar al trampolín (función original) desde
//     los hooks. Cada llamada va dentro de __try/__except: si la firma que suponemos es
//     incorrecta (número o tipo de parámetros) o la original revienta, se captura la
//     excepción, se marca el hook como fallido (HookHealth) y se devuelve un valor seguro
//     en vez de tumbar el juego. Restricción de MSVC: __try no puede convivir con objetos
//     C++ con destructor, por eso estas funciones solo usan tipos C.
// EN: safe_hook.h — SEH wrappers to call the trampoline (original function) from hooks.
//     Each call runs inside __try/__except: if our assumed signature is wrong (parameter
//     count or types) or the original crashes, the exception is caught, the hook is marked
//     as failed (HookHealth) and a safe value is returned instead of crashing the game.
//     MSVC restriction: __try cannot coexist with C++ objects with destructors, so these
//     functions only use C types.
#pragma once
// ES: Envoltorios de llamada al trampolín protegidos con SEH (descripción en inglés abajo).
// SEH-protected trampoline call wrappers.
//
// Each hook's trampoline (original function) call is wrapped in __try/__except
// so that if the function signature is wrong (wrong parameter count, wrong types),
// the game doesn't crash — instead we catch the exception, log it, and return
// a safe default.
//
// MSVC restriction: __try/__except cannot be used in functions that contain
// C++ objects with destructors. These wrapper functions use only C types.

#include <cstdint>
#include <cstdio>
#include <atomic>
#include <Windows.h>

namespace kmp {

// ES: Estado de salud por hook: si el trampolín de un hook revienta, se marca como
//     fallido y el hook deja de llamar al trampolín.
// Per-hook health status. If a hook's trampoline crashes, it gets marked
// as failed and the hook stops calling the trampoline.
//
// [FIX-AGREGADO 2026-07] Constructor explícito OBLIGATORIO: sin él, esto era un
// AGREGADO en C++17 y `static HookHealth h{"Nombre"};` inicializaba el PRIMER
// miembro (trampolineFailed) con el puntero al string (puntero→bool = true),
// dejando name vacío. Resultado: los 12 hooks nacían con trampolineFailed=true
// y los guards de SafeCall_* cortaban SIEMPRE sin llamar jamás al original del
// juego (confirmado en vivo: failCount=0, name=""). El constructor elimina la
// propiedad de agregado (a propósito) y enruta el string a `name`.
// EN: [FIX-AGGREGATE 2026-07] An explicit constructor is MANDATORY: without it this was
//     an AGGREGATE in C++17 and `static HookHealth h{"Name"};` initialized the FIRST member
//     (trampolineFailed) with the string pointer (pointer -> bool = true), leaving name
//     empty. Result: all 12 hooks were born with trampolineFailed=true and the SafeCall_*
//     guards ALWAYS cut without ever calling the game's original (confirmed live:
//     failCount=0, name=""). The constructor removes the aggregate property (on purpose)
//     and routes the string to `name`.
struct HookHealth {
    std::atomic<bool> trampolineFailed{false};
    std::atomic<int>  failCount{0};
    const char* name = "";
    std::atomic<bool> guardLogged{false}; // one-shot: ya se logueó el corte del guard

    HookHealth() = default;
    explicit HookHealth(const char* n) : name(n) {}
};

// [HARDENING 2026-07] Guard de los SafeCall_*: devuelve true si el hook está
// marcado como fallido (el guard corta y NO se llama al original). Loguea UNA
// sola vez por hook la primera vez que corta — antes cortaba en silencio total,
// lo que mantuvo invisible el bug del agregado durante 3+ semanas. Sin heap ni
// objetos C++ con destructor (seguro de llamar desde funciones con SEH).
// EN: [HARDENING 2026-07] Guard for SafeCall_*: returns true if the hook is marked as
//     failed (the guard cuts and the original is NOT called). Logs ONLY ONCE per hook the
//     first time it cuts; before, it cut in total silence, which kept the aggregate bug
//     invisible for 3+ weeks. No heap and no C++ objects with destructors (safe to call
//     from functions with SEH).
inline bool HookGuardTripped(HookHealth* health) {
    if (!health || !health->trampolineFailed.load()) return false;
    bool expected = false;
    if (health->guardLogged.compare_exchange_strong(expected, true)) {
        char buf[192];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "[KMP][SafeHook] guard CORTA el hook '%s' (trampolineFailed=1, failCount=%d) — "
                    "el original del juego NO se llama\n",
                    (health->name && health->name[0]) ? health->name : "<sin nombre>",
                    health->failCount.load());
        OutputDebugStringA(buf);
    }
    return true;
}

// ES: Envoltorios para las distintas firmas. Son funciones estilo C con noinline que
//     envuelven la llamada en SEH; NO pueden contener objetos C++ con destructor.
//     Patrón común: si fn es nulo o el guard está cortado devuelven false/nullptr; si la
//     original lanza una excepción marcan trampolineFailed, suman failCount y devuelven
//     false/nullptr. El comentario de cada una indica la firma y qué hook la usa.
// EN: Common pattern: if fn is null or the guard is tripped they return false/nullptr;
//     if the original throws they set trampolineFailed, bump failCount and return
//     false/nullptr. Each one's comment gives the signature and which hook uses it.
// ── Safe call wrappers for different function signatures ──
// These are __declspec(noinline) C-style functions that wrap trampoline calls
// in SEH. They must NOT contain any C++ objects with destructors.

// ES: void fn(void*).
// void fn(void*)
__declspec(noinline)
inline bool SafeCall_Void_Ptr(void* fn, void* a1, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*);
    __try {
        reinterpret_cast<Fn>(fn)(a1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// ES: void* fn(void*, void*) — creación de personaje (CharacterCreate); devuelve el puntero de la original.
// void* fn(void*, void*) — CharacterCreate
__declspec(noinline)
inline void* SafeCall_Ptr_PtrPtr(void* fn, void* a1, void* a2, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return nullptr;
    using Fn = void*(__fastcall*)(void*, void*);
    __try {
        return reinterpret_cast<Fn>(fn)(a1, a2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return nullptr;
    }
}

// void* fn(void*, void*, void*) — resolvedores de 3 punteros que DEVUELVEN puntero
// (guard del retorno de BuyItem / FIX-UAF-BUYITEM). Si el original revienta DENTRO (p.ej.
// al desreferenciar la vtable de un objeto reciclado antes de devolverlo), el except
// devuelve nullptr — un retorno seguro que el caller del juego ya sabe tratar.
// EN: void* fn(void*, void*, void*) — 3-pointer resolvers that RETURN a pointer (return
//     guard of BuyItem / FIX-UAF-BUYITEM). If the original crashes INSIDE (e.g. when
//     dereferencing the vtable of a recycled object before returning it), the except
//     returns nullptr, a safe return value the game caller already knows how to handle.
__declspec(noinline)
inline void* SafeCall_Ptr_PtrPtrPtr(void* fn, void* a1, void* a2, void* a3, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return nullptr;
    using Fn = void*(__fastcall*)(void*, void*, void*);
    __try {
        return reinterpret_cast<Fn>(fn)(a1, a2, a3);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return nullptr;
    }
}

// ES: void fn(void*, float, float, float) — fijar posición (SetPosition): this + x, y, z.
// void fn(void*, float, float, float) — SetPosition
__declspec(noinline)
inline bool SafeCall_Void_PtrFFF(void* fn, void* a1, float a2, float a3, float a4, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, float, float, float);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3, a4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// void fn(void*, float) — CombatClass::update(float) (DIAG-COMBATSEED)
// El this va en rcx (a1) y el dt (float) en xmm1 (a2); el trampolín estándar de MinHook
// preserva xmm1 (la función 0x60D650 tiene prólogo limpio, sin el fix MovRaxRsp).
// EN: void fn(void*, float) — CombatClass::update(float) (DIAG-COMBATSEED). `this` goes in
//     rcx (a1) and dt (float) in xmm1 (a2); the standard MinHook trampoline preserves xmm1
//     (function 0x60D650 has a clean prologue, no MovRaxRsp fix).
__declspec(noinline)
inline bool SafeCall_Void_PtrF(void* fn, void* a1, float a2, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, float);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// ES: void fn(void*, float, float, float, int) — mover a (MoveTo): this + destino + int.
// void fn(void*, float, float, float, int) — MoveTo
__declspec(noinline)
inline bool SafeCall_Void_PtrFFFI(void* fn, void* a1, float a2, float a3, float a4, int a5, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, float, float, float, int);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3, a4, a5);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// ES: void fn(void*, void*, int, float, float, float) — aplicar daño (ApplyDamage).
// void fn(void*, void*, int, float, float, float) — ApplyDamage
__declspec(noinline)
inline bool SafeCall_Void_PtrPtrIFFF(void* fn, void* a1, void* a2, int a3,
                                       float a4, float a5, float a6, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, void*, int, float, float, float);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3, a4, a5, a6);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// ES: void fn(void*, void*) — muerte de personaje (CharacterDeath).
// void fn(void*, void*) — CharacterDeath
__declspec(noinline)
inline bool SafeCall_Void_PtrPtr(void* fn, void* a1, void* a2, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, void*);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// ES: void fn(void*, void*, void*) — inicio de ataque (StartAttack: atacante, objetivo, arma).
// void fn(void*, void*, void*) — StartAttack(attacker, target, weapon)
__declspec(noinline)
inline bool SafeCall_Void_PtrPtrPtr(void* fn, void* a1, void* a2, void* a3, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, void*, void*);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// ES: void fn(void*, int, int) — carga/descarga de zona (ZoneLoad/ZoneUnload), coordenadas de zona.
// void fn(void*, int, int) — ZoneLoad/ZoneUnload
__declspec(noinline)
inline bool SafeCall_Void_PtrII(void* fn, void* a1, int a2, int a3, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, int, int);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// ES: void fn(void*, void*, float, float, float) — colocar edificio (BuildingPlace) con posición.
// void fn(void*, void*, float, float, float) — BuildingPlace
__declspec(noinline)
inline bool SafeCall_Void_PtrPtrFFF(void* fn, void* a1, void* a2,
                                      float a3, float a4, float a5, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, void*, float, float, float);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3, a4, a5);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// ES: void fn(void*, const char*) — guardar/cargar partida (SaveGame/LoadGame) con nombre.
// void fn(void*, const char*) — SaveGame/LoadGame
__declspec(noinline)
inline bool SafeCall_Void_PtrStr(void* fn, void* a1, const char* a2, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, const char*);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// bool fn(void*, int, void*) — AddOrderBackend (validador de órdenes 0x5D1940)
// Devuelve en *outRet el bool del original (true = orden ABORTADA/tragada,
// false = orden continúa). Si el trampoline falla, deja *outRet = false para que
// el caller del juego siga el pipeline normal (degradación segura: sin validación,
// pero sin tragar la orden ni crashear).
// EN: bool fn(void*, int, void*) — AddOrderBackend (order validator 0x5D1940). Returns the
//     original's bool in *outRet (true = order ABORTED/swallowed, false = order goes on).
//     If the trampoline fails, it leaves *outRet = false so the game caller follows the
//     normal pipeline (safe degradation: no validation, but no swallowed order or crash).
__declspec(noinline)
inline bool SafeCall_Bool_PtrIPtr(void* fn, void* a1, int a2, void* a3,
                                    bool* outRet, HookHealth* health) {
    if (outRet) *outRet = false;
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = bool(__fastcall*)(void*, int, void*);
    __try {
        bool r = reinterpret_cast<Fn>(fn)(a1, a2, a3);
        if (outRet) *outRet = r;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// ES: void fn(void*, void*, int) — noqueo de personaje (CharacterKO).
// void fn(void*, void*, int) — CharacterKO
__declspec(noinline)
inline bool SafeCall_Void_PtrPtrI(void* fn, void* a1, void* a2, int a3, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, void*, int);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

} // namespace kmp
